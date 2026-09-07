/*
 * Every live SPU context gets an MFC engine of its own, and gives it back.
 *
 * There are eight slots in the per-context MFC table and a thread group starts
 * all of its threads at once, so the claim is a race between as many host
 * threads as the group has, each arriving on its first DMA. Two of them taking
 * the same slot is silent: both SPUs then share one engine's command queue and
 * tag-completion mask, so one SPU's tag wait is satisfied by the other SPU's
 * transfer and a job reads a buffer that has not arrived yet. Nothing reports
 * it, and it does not reproduce -- it depends on which two threads collided on
 * a particular run.
 *
 * The other half is the table running dry. Slots used to be claimed and never
 * released, so a title with more SPU contexts over its life than there are
 * slots ran everything past the eighth on one shared fallback engine, carrying
 * whatever queue and tag state the previous owners left in it.
 *
 * So the assertions are: eight contexts claiming at the same instant get eight
 * different engines; each engine keeps the state its own context wrote while
 * the others are writing theirs; the engines are handed out again to the next
 * eight contexts, freshly initialized; and over thirty-two rounds -- two
 * hundred and fifty-six contexts, none of them at an address a previous one
 * used -- exactly the same eight engines are handed out, which is what says
 * the slots came back rather than the overflow engine being shared.
 *
 * spu_channels.c is compiled in rather than linked, because the slot table and
 * mfc_for() are file-static: the same reason test_spu_lifted_start.c compiles
 * in lv2_register.c.
 *
 * Build:
 *   clang -std=gnu17 -I include -I runtime/spu -I runtime/platform \
 *         -o /tmp/test_spu_mfc_slots \
 *         runtime/spu/tests/test_spu_mfc_slots.c \
 *         runtime/spu/spu_drain.c runtime/spu/spu_lockstep.c \
 *         runtime/spu/spu_coherency.c runtime/spu/spu_workload.c \
 *         runtime/spu/spurs_policy.c runtime/spu/spurs_job.c \
 *         runtime/spu/spu_tsp_weak.c runtime/spu/spu_vm_pagemap.c \
 *         runtime/spu/spurs_policy_blob_weak.c runtime/spu/spu_interp.c \
 *         runtime/platform/win32_compat.c -lpthread
 *
 * Exit code: 0 if all passed, 1 if any failed. Final line prints a summary.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Guest memory. Nothing here issues a DMA, so the arena stays empty; the
 * symbol exists because the DMA paths compiled in below read through it. */
uint8_t* vm_base = NULL;
uint32_t ppu_vm_size = 0;

/* The unit under test. */
#include "../spu_channels.c"

/* ---------------------------------------------------------------------------
 * Harness
 * -----------------------------------------------------------------------*/
static int g_pass, g_fail;

static void check(int ok, const char* what)
{
    if (ok) { g_pass++; return; }
    g_fail++;
    printf("  FAIL: %s\n", what);
}

#define CHECK(cond)  check((cond), #cond)

/* ---------------------------------------------------------------------------
 * Eight contexts claiming at once, thirty-two times over
 * -----------------------------------------------------------------------*/
#define NCTX    SPU_MAX_CONTEXTS   /* exactly fills the table */
#define ROUNDS  32                 /* 256 contexts, well past the eight slots */

/* Every context in the run is live at once and none is freed, so no two ever
 * share an address. A round that reused an address would find a leaked slot by
 * pointer and pass while leaking. The table keys on the address and never
 * reads through it, so these stay untouched and cost address space only. */
static spu_context* g_ctx[ROUNDS][NCTX];

static mfc_engine*  g_engine[ROUNDS][NCTX];  /* what each context was handed  */
static int          g_fresh[ROUNDS][NCTX];   /* ...initialized when handed    */
static int          g_stable[ROUNDS][NCTX];  /* ...found again by the lookup  */
static int          g_private[ROUNDS][NCTX]; /* ...still holding its own state */
static int          g_timeout;               /* a barrier gave up             */

/* A barrier across the NCTX worker threads, so the claims really are
 * simultaneous rather than merely on different threads.
 *
 * The threads leave it spinning rather than on a condition variable, and that
 * is the difference between a check with teeth and one without. The window an
 * unsynchronized claim leaves open is the few instructions between finding a
 * free slot and storing into it; woken one at a time by a broadcast, eight
 * threads walk through that window in single file and the collision showed up
 * in two runs out of five. Released within a cache line's propagation of each
 * other, they collide in the first round or two of every run.
 *
 * It has a deadline, so a regression is a failing test and not a hanging one. */
static volatile LONG g_bar_arrived;
static volatile LONG g_bar_gen;

static void barrier_wait(void)
{
    LONG gen = g_bar_gen;
    if (InterlockedIncrement(&g_bar_arrived) == NCTX) {
        g_bar_arrived = 0;
        InterlockedIncrement(&g_bar_gen);   /* releases the other seven */
        return;
    }
    ULONGLONG start = GetTickCount64();
    while (g_bar_gen == gen) {
        if (GetTickCount64() - start > 10000) { g_timeout = 1; return; }
        YieldProcessor();
    }
}

static void* claim_thread(void* arg)
{
    int w = (int)(intptr_t)arg;

    for (int r = 0; r < ROUNDS; r++) {
        spu_context* ctx = g_ctx[r][w];

        barrier_wait();                       /* all eight claim together */
        mfc_engine* e = mfc_for(ctx);
        g_engine[r][w] = e;
        /* A slot that came back from a previous owner has to come back clean:
         * all tags idle, nothing queued. */
        g_fresh[r][w]  = (e->tag_completed == 0xFFFFFFFFu) && (e->queue_count == 0);
        g_stable[r][w] = (mfc_for(ctx) == e);

        /* Something only this context would have written, in the two fields a
         * shared engine corrupts. */
        uint32_t mark = 0xA5000000u | (uint32_t)(r * NCTX + w);
        e->tag_completed = mark;
        e->queue_count   = (uint32_t)w + 1u;

        barrier_wait();                       /* everyone has written theirs */
        g_private[r][w] = (e->tag_completed == mark) &&
                          (e->queue_count == (uint32_t)w + 1u);

        barrier_wait();                       /* ...and everyone has read it */
        spu_mfc_release(ctx);                 /* what the thread's exit does */
    }
    return NULL;
}

/* Is `e` in the first `n` entries of `set`? */
static int engine_seen(mfc_engine* const* set, int n, const mfc_engine* e)
{
    for (int i = 0; i < n; i++)
        if (set[i] == e) return 1;
    return 0;
}

int main(void)
{
    printf("SPU MFC engine slots\n");

    for (int r = 0; r < ROUNDS; r++) {
        for (int w = 0; w < NCTX; w++) {
            g_ctx[r][w] = (spu_context*)calloc(1, sizeof(spu_context));
            if (!g_ctx[r][w]) {
                printf("  FAIL: out of memory for %d SPU contexts\n", ROUNDS * NCTX);
                return 1;
            }
        }
    }

    pthread_t th[NCTX];
    for (int w = 0; w < NCTX; w++)
        if (pthread_create(&th[w], NULL, claim_thread, (void*)(intptr_t)w) != 0) {
            printf("  FAIL: could not start worker %d\n", w);
            return 1;
        }
    for (int w = 0; w < NCTX; w++)
        pthread_join(th[w], NULL);

    CHECK(!g_timeout);

    /* No two contexts of a round share an engine, each engine was handed over
     * initialized, the lookup keeps finding it, and it holds only what its own
     * context put in it. */
    for (int r = 0; r < ROUNDS; r++) {
        int distinct = 1;
        for (int w = 0; w < NCTX; w++) {
            if (!g_engine[r][w]) { distinct = 0; continue; }
            for (int v = w + 1; v < NCTX; v++)
                if (g_engine[r][w] == g_engine[r][v]) distinct = 0;
        }
        CHECK(distinct);
        for (int w = 0; w < NCTX; w++) {
            CHECK(g_fresh[r][w]);
            CHECK(g_stable[r][w]);
            CHECK(g_private[r][w]);
        }
    }

    /* Two hundred and fifty-six contexts, eight engines. More than eight would
     * mean a round fell through to the shared overflow engine, which is what
     * happens when a slot is not given back. */
    mfc_engine* seen[ROUNDS * NCTX];
    int nseen = 0;
    for (int r = 0; r < ROUNDS; r++)
        for (int w = 0; w < NCTX; w++)
            if (g_engine[r][w] && !engine_seen(seen, nseen, g_engine[r][w]))
                seen[nseen++] = g_engine[r][w];
    CHECK(nseen == NCTX);

    /* Releasing a context that never issued a DMA, and so holds no slot, is a
     * scan that finds nothing rather than a slot lost. */
    spu_context* idle = (spu_context*)calloc(1, sizeof(spu_context));
    CHECK(idle != NULL);
    spu_mfc_release(idle);

    /* The table is empty again: fill it, then check what the ninth context
     * gets. Sharing the overflow engine is the documented behaviour above
     * SPU_MAX_CONTEXTS -- a real group has at most six -- and it must not cost
     * a slot, so releasing one of the eight hands that same slot to the next
     * context that asks. */
    spu_context* held[NCTX];
    mfc_engine*  helde[NCTX];
    for (int i = 0; i < NCTX; i++) {
        held[i] = (spu_context*)calloc(1, sizeof(spu_context));
        CHECK(held[i] != NULL);
        helde[i] = mfc_for(held[i]);
    }
    spu_context* ninth = (spu_context*)calloc(1, sizeof(spu_context));
    CHECK(ninth != NULL);
    mfc_engine* ninthe = mfc_for(ninth);
    CHECK(ninthe != NULL);
    CHECK(!engine_seen(helde, NCTX, ninthe));   /* the overflow one */

    spu_mfc_release(held[3]);
    spu_context* reuser = (spu_context*)calloc(1, sizeof(spu_context));
    CHECK(reuser != NULL);
    mfc_engine* reusere = mfc_for(reuser);
    CHECK(reusere == helde[3]);                 /* the freed slot, not overflow */
    CHECK(reusere->tag_completed == 0xFFFFFFFFu);

    printf("SPU MFC engine slots: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * The runtime beyond the SPU channel layer, stubbed. These are what
 * spu_channels.c reaches for outside its own subsystem: the lv2 mailbox
 * delivery hook's owner, the PPU's reservation and watch helpers, and the
 * title-specific SPURS hooks. None of them is on the path this test drives.
 * -----------------------------------------------------------------------*/
void ppu_resv_break_store(uint64_t ea)                 { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w)
{
    (void)a; (void)v; (void)w;
}
uint64_t ps3_ms_now(void)                              { return 0; }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int      g_resv_store_active = 0;
uint32_t g_barrier_sync_watch = 0;

void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0;
uint32_t g_ydkj_real_spurs_ea = 0;
uint32_t g_ydkj_real_taskset_ea = 0;
uint32_t g_ydkj_real_taskid = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void)                   { }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d)
{
    (void)c; (void)a; (void)b; (void)d;
}

/* Loader diagnostics and worker publication are outside this fixture. */
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }
