/* Concurrent per-context MFC ownership, including more than eight live
 * contexts and stalled lists retained across interleaved context use. */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Guest memory. The stalled-list checks exercise actual DMA into private local stores. */
static uint8_t guest_memory[1u << 20];
uint8_t* vm_base = guest_memory;
uint32_t ppu_vm_size = 0;

/* The unit under test. */
#include "../spu_channels.c"
#include "../spu_lifted_job.h"

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
 * More than eight contexts claiming concurrently, across repeated lifetimes
 * -----------------------------------------------------------------------*/
#define NCTX    12   /* exceeds the old hardware-count registry */
#define ROUNDS  4

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
        InterlockedIncrement(&g_bar_gen);   /* releases the other threads */
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

        barrier_wait();                       /* all contexts claim together */
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

static void transient_job(spu_context* ctx)
{
    mfc_engine* engine = mfc_for(ctx);
    CHECK(engine->queue_count == 0);
    CHECK(engine->tag_completed == 0xFFFFFFFFu);
    engine->queue_count = 7;
    engine->tag_completed = 0;
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

    /* Releasing a context that never issued a DMA, and so holds no slot, is a
     * scan that finds nothing rather than a slot lost. */
    spu_context* idle = (spu_context*)calloc(1, sizeof(spu_context));
    CHECK(idle != NULL);
    spu_mfc_release(idle);

    /* Retain all contexts while one completes a stalled list. The completion
     * must not complete another context's same-numbered tag. */
    spu_context* held[NCTX];
    mfc_engine* helde[NCTX];
    for (int i = 0; i < NCTX; i++) {
        held[i] = (spu_context*)calloc(1, sizeof(spu_context));
        CHECK(held[i] != NULL);
        spu_context_init(held[i], (uint32_t)i);
        helde[i] = mfc_for(held[i]);
        uint32_t ea = 0x20000u + (uint32_t)i * 0x100u;
        memset(guest_memory + ea, i + 1, 16);
        memset(guest_memory + ea + 16, i + 0x80, 16);
        spu_ls_write32(held[i], 0x1000, 0x80000010u); /* transfer then stall */
        spu_ls_write32(held[i], 0x1004, ea);
        spu_ls_write32(held[i], 0x1008, 16);
        spu_ls_write32(held[i], 0x100C, ea + 16);
        held[i]->mfc_lsa = 0x2000;
        held[i]->mfc_eal = 0x1000;
        held[i]->mfc_size = 16;
        held[i]->mfc_tag = 3;
        spu_wrch(held[i], MFC_Cmd, spu_make_preferred_u32(MFC_GETL_CMD));
        CHECK(held[i]->ls[0x2000] == i + 1);
        CHECK(held[i]->ls[0x2010] == 0); /* tail must wait for this context's ack */
    }
    for (int i = 0; i < NCTX; i++) {
        CHECK(mfc_for(held[i]) == helde[i]);
        CHECK(!(helde[i]->tag_completed & (1u << 3)));
        CHECK(held[i]->list_stall_mask == (1u << 3));
        spu_wrch(held[i], MFC_WrListStallAck, spu_make_preferred_u32(3));
        for (unsigned n = 0; n < 16; ++n)
            CHECK(held[i]->ls[0x2010 + n] == i + 0x80);
        CHECK(helde[i]->tag_completed & (1u << 3));
        CHECK(held[i]->list_stall_mask == 0);
        for (int j = i + 1; j < NCTX; j++)
            CHECK(!(helde[j]->tag_completed & (1u << 3)));
    }
    /* A new lifetime at the SAME address gets clean engine state. Other live
     * engines must remain untouched, regardless of allocator address reuse. */
    helde[3]->queue_count = 7;
    helde[3]->tag_completed = 0;
    spu_mfc_release(held[3]);
    mfc_engine* reusere = mfc_for(held[3]);
    CHECK(reusere->queue_count == 0);
    CHECK(reusere->tag_completed == 0xFFFFFFFFu);
    for (int i = 0; i < NCTX; i++) {
        spu_mfc_release(held[i]);
        spu_mfc_release(held[i]); /* idempotent, before a fresh claim */
        free(held[i]);
    }
    free(idle);
    for (int r = 0; r < ROUNDS; r++)
        for (int w = 0; w < NCTX; w++) free(g_ctx[r][w]);

    /* Thread-exit cleanup forgets all abandoned owners in a stack range,
     * including adjacent list nodes, and preserves owners outside that range. */
    spu_context* range = calloc(3, sizeof(*range));
    spu_context* outside = calloc(1, sizeof(*outside));
    CHECK(range != NULL && outside != NULL);
    for (unsigned i = 0; i < 3; ++i) (void)mfc_for(&range[i]);
    mfc_engine* outside_engine = mfc_for(outside);
    outside_engine->tag_completed = 0x12345678;
    spu_mfc_forget_range((uintptr_t)range, (uintptr_t)(range + 3));
    CHECK(mfc_for(outside) == outside_engine);
    CHECK(outside_engine->tag_completed == 0x12345678);
    CHECK(s_mfc_slots && s_mfc_slots->ctx == outside && !s_mfc_slots->next);
    free(range);
    spu_mfc_release(outside);
    free(outside);

    for (unsigned i = 0; i < 12; ++i) {
        CHECK(spu_run_lifted_job_img(transient_job, NULL, 0, 0) == 0);
        CHECK(s_mfc_slots == NULL); /* stack context actually relinquished its node */
    }
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
unsigned long long ps3_ms_now(void)                    { return 0; }
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
