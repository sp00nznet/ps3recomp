/*
 * A task parked in WAIT_SIGNAL wakes when another SPU signals it
 * (runtime/spu/spu_workload.c, runtime/spu/spu_channels.c).
 *
 * libspurs signals a task from the SPU side: a GETLLAR/PUTLLC loop that sets
 * the target's bit in the taskset's signalled bitset. The parked task checks
 * that guest bit, but it sleeps on a host condition variable that only the
 * PPU-side cellSpursSendSignal used to broadcast. An SPU-side signal was
 * therefore seen only when the waiter's 1 s timed wait ran out. Twisted
 * Metal's post-FX pipeline chains six tasks that way, several times a frame,
 * and took 4 s or more per frame.
 *
 * So the assertions are on the waiter's latency: after an SPU commits the
 * waiter's bit with PUTLLC, or with PUTLLUC, the waiter is back well inside
 * the 1 s timeout. The cases that must stay parked are here too: a commit to
 * the same line that sets another task's bit, and a commit to the bitset of
 * another taskset. A spurious wake is allowed, a spurious return is not.
 *
 * spu_channels.c is compiled in rather than linked, because spu_mfc_atomic()
 * is file-static: the same reason test_spu_lockline_peer.c does it.
 *
 * Build:
 *   clang -std=gnu17 -I include -I runtime/spu -I runtime/platform \
 *         -o /tmp/test_spu_taskset_wake \
 *         runtime/spu/tests/test_spu_taskset_wake.c \
 *         runtime/spu/spu_drain.c runtime/spu/spu_lockstep.c \
 *         runtime/spu/spu_coherency.c runtime/spu/spu_workload.c \
 *         runtime/spu/spurs_policy.c runtime/spu/spurs_job.c \
 *         runtime/spu/spu_tsp_weak.c runtime/spu/spu_vm_pagemap.c \
 *         runtime/spu/spurs_policy_blob_weak.c runtime/spu/spu_interp.c \
 *         runtime/platform/win32_compat.c -lpthread -lm
 *
 * Exit code: 0 if all passed, 1 if any failed. Final line prints a summary.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Guest memory. The atomic path indexes straight off this, so it has to cover
 * every EA the test uses. */
static uint8_t g_guest_mem[1u << 20];
uint8_t* vm_base = g_guest_mem;
uint32_t ppu_vm_size = 0;

/* The unit under test. */
#include "../spu_channels.c"

int  spu_taskset_wait_signal(uint32_t taskset_ea, uint32_t taskId);
void spu_taskset_parked_add(uint32_t taskset_ea, uint32_t taskId, uint32_t wait_obj);
void spu_taskset_parked_del(uint32_t taskset_ea, uint32_t taskId);
void spu_taskset_signal_task(uint32_t taskset_ea, uint32_t taskId);

/* ---------------------------------------------------------------------------
 * Harness
 * -----------------------------------------------------------------------*/
static int g_pass, g_fail;
static const char* g_current = "(none)";

#define TEST(name) g_current = (name)

#define CHECK(cond) do {                                          \
    if (cond) { g_pass++; }                                       \
    else {                                                        \
        g_fail++;                                                 \
        fprintf(stderr, "FAIL: %s\n  at %s:%d: %s\n",             \
                g_current, __FILE__, __LINE__, #cond);            \
    }                                                             \
} while (0)

/* Two tasksets, 128-byte aligned as the SDK requires. The signalled bitset is
 * at +0x40 (CSTS_SIGNALLED in libs/spurs/spurs_taskset.h, which is not included
 * here: it declares spurs_pm_build_context with the signature the stub below
 * cannot have), so it shares its line with the other task bitsets. Bit 0 is
 * the most significant bit of the first big-endian word. */
#define SIGNALLED  0x40u
#define TASKSET_A  0x00030000u
#define TASKSET_B  0x00032000u
#define LSA        0x00001000u

/* The task that parks, and a second one whose bit shares the word. */
#define WAITER     3u
#define OTHER      5u

/* Well under the waiter's 1 s timed wait: a wake that only came from the
 * timeout lands after this. */
#define WAKE_MS    400u
/* How long a waiter that must stay parked is watched. */
#define STAY_MS    300u

static spu_context g_spu;   /* 256 KB of local store: not on the stack */

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ---------------------------------------------------------------------------
 * Driving the real channel path, as test_spu_lockline_peer.c does
 * -----------------------------------------------------------------------*/
static void wrch(spu_context* ctx, uint32_t channel, uint32_t v)
{
    spu_wrch(ctx, channel, spu_make_preferred_u32(v));
}

static void spu_atomic(spu_context* ctx, uint32_t lsa, uint32_t ea, uint32_t cmd)
{
    wrch(ctx, MFC_LSA, lsa);
    wrch(ctx, MFC_EAH, 0);
    wrch(ctx, MFC_EAL, ea);
    wrch(ctx, MFC_Cmd, cmd);
}

/* What libspurs does on the SPU: reserve the taskset's line, set the task's
 * bit in the signalled bitset in local store, commit. `cmd` is the commit:
 * PUTLLC after the reservation, or PUTLLUC. */
static void spu_signal(uint32_t taskset, uint32_t task, uint32_t cmd)
{
    spu_atomic(&g_spu, LSA, taskset, MFC_GETLLAR_CMD);
    uint8_t* b = &g_spu.ls[LSA + SIGNALLED + (task / 32u) * 4u];
    const uint32_t mask = (1u << 31) >> (task % 32u);
    b[0] |= (uint8_t)(mask >> 24); b[1] |= (uint8_t)(mask >> 16);
    b[2] |= (uint8_t)(mask >> 8);  b[3] |= (uint8_t)mask;
    spu_atomic(&g_spu, LSA, taskset, cmd);
}

/* ---------------------------------------------------------------------------
 * The parked task: its own host thread, as a lifted task is
 * -----------------------------------------------------------------------*/
typedef struct {
    uint32_t taskset, task;
    volatile int returned;
    uint64_t returned_ms;
} waiter;

static void* waiter_main(void* p)
{
    waiter* w = (waiter*)p;
    spu_taskset_parked_add(w->taskset, w->task, 0);
    spu_taskset_wait_signal(w->taskset, w->task);
    spu_taskset_parked_del(w->taskset, w->task);
    w->returned_ms = now_ms();
    __atomic_store_n(&w->returned, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void park(waiter* w, pthread_t* th, uint32_t taskset, uint32_t task)
{
    memset((void*)w, 0, sizeof(*w));
    w->taskset = taskset;
    w->task = task;
    pthread_create(th, NULL, waiter_main, w);
    sleep_ms(100);   /* let it reach the timed wait: a wake before is not lost
                      * anyway, the bit is checked before it sleeps */
}

/* Wait up to `ms` for the waiter to return; how long it took, or ~0u. */
static uint64_t wait_return(waiter* w, uint64_t since, unsigned ms)
{
    const uint64_t end = now_ms() + ms;
    while (now_ms() < end) {
        if (__atomic_load_n(&w->returned, __ATOMIC_ACQUIRE)) return w->returned_ms - since;
        sleep_ms(1);
    }
    return ~(uint64_t)0;
}

static int signalled(uint32_t taskset, uint32_t task)
{
    return (g_guest_mem[taskset + SIGNALLED + task / 8u] >> (7u - task % 8u)) & 1u;
}

static void clear_guest(void)
{
    memset(&g_guest_mem[TASKSET_A], 0, 0x80);
    memset(&g_guest_mem[TASKSET_B], 0, 0x80);
}

/* ---------------------------------------------------------------------------
 * Tests
 * -----------------------------------------------------------------------*/
static void test_putllc_signal_wakes(void)
{
    TEST("an SPU PUTLLC setting the waiter's bit wakes it");
    clear_guest();
    waiter w; pthread_t th;
    park(&w, &th, TASKSET_A, WAITER);
    CHECK(!w.returned);
    const uint64_t t = now_ms();
    spu_signal(TASKSET_A, WAITER, MFC_PUTLLC_CMD);
    CHECK(g_spu.atomic_stat == 0);   /* the commit went through */
    const uint64_t took = wait_return(&w, t, WAKE_MS);
    CHECK(took != ~(uint64_t)0);
    if (took == ~(uint64_t)0) {
        fprintf(stderr, "  waiter still parked %u ms after the commit\n", WAKE_MS);
        wait_return(&w, t, 2000);
    }
    pthread_join(th, NULL);
    /* The wait consumed the signal, as on the console. */
    CHECK(!signalled(TASKSET_A, WAITER));
}

static void test_putlluc_signal_wakes(void)
{
    TEST("an SPU PUTLLUC setting the waiter's bit wakes it");
    clear_guest();
    waiter w; pthread_t th;
    park(&w, &th, TASKSET_A, WAITER);
    const uint64_t t = now_ms();
    spu_signal(TASKSET_A, WAITER, MFC_PUTLLUC_CMD);
    const uint64_t took = wait_return(&w, t, WAKE_MS);
    CHECK(took != ~(uint64_t)0);
    if (took == ~(uint64_t)0) wait_return(&w, t, 2000);
    pthread_join(th, NULL);
}

static void test_other_task_stays_parked(void)
{
    TEST("a commit setting another task's bit leaves the waiter parked");
    clear_guest();
    waiter w; pthread_t th;
    park(&w, &th, TASKSET_A, WAITER);
    const uint64_t t = now_ms();
    spu_signal(TASKSET_A, OTHER, MFC_PUTLLC_CMD);
    CHECK(wait_return(&w, t, STAY_MS) == ~(uint64_t)0);
    /* Release it the PPU way, so the thread can be joined. */
    spu_taskset_signal_task(TASKSET_A, WAITER);
    CHECK(wait_return(&w, t, 2000) != ~(uint64_t)0);
    pthread_join(th, NULL);
}

static void test_other_taskset_stays_parked(void)
{
    TEST("a signal to another taskset leaves the waiter parked");
    clear_guest();
    waiter w; pthread_t th;
    park(&w, &th, TASKSET_A, WAITER);
    const uint64_t t = now_ms();
    spu_signal(TASKSET_B, WAITER, MFC_PUTLLC_CMD);
    CHECK(wait_return(&w, t, STAY_MS) == ~(uint64_t)0);
    spu_taskset_signal_task(TASKSET_A, WAITER);
    CHECK(wait_return(&w, t, 2000) != ~(uint64_t)0);
    pthread_join(th, NULL);
}

/* The commit hook only visits slots that can hold a parked task: none while
 * nothing is parked, and none past the highest slot ever used. A waiter that
 * parked behind other entries, some since removed, must still be reached. */
static void test_waiter_behind_other_entries_wakes(void)
{
    TEST("a waiter parked behind other entries and holes wakes");
    clear_guest();
    /* Slots 0-4: other tasks of taskset B, parked without threads. */
    for (uint32_t t = 10; t < 15; t++) spu_taskset_parked_add(TASKSET_B, t, 0);
    waiter w; pthread_t th;
    park(&w, &th, TASKSET_A, WAITER);   /* slot 5 */
    /* Leave slot 0 and the waiter: two entries, three holes between them. */
    for (uint32_t t = 11; t < 15; t++) spu_taskset_parked_del(TASKSET_B, t);
    const uint64_t t0 = now_ms();
    spu_signal(TASKSET_A, WAITER, MFC_PUTLLC_CMD);
    const uint64_t took = wait_return(&w, t0, WAKE_MS);
    CHECK(took != ~(uint64_t)0);
    if (took == ~(uint64_t)0) wait_return(&w, t0, 2000);
    pthread_join(th, NULL);
    spu_taskset_parked_del(TASKSET_B, 10);
}

static void test_unmatched_remove_keeps_bookkeeping(void)
{
    TEST("removing a task that is not parked does not hide a later waiter");
    clear_guest();
    /* Nothing is parked, so this finds no entry and must change nothing:
     * counting it as a removal would leave the waiter below uncounted. */
    spu_taskset_parked_del(TASKSET_A, WAITER);
    waiter w; pthread_t th;
    park(&w, &th, TASKSET_A, WAITER);
    const uint64_t t0 = now_ms();
    spu_signal(TASKSET_A, WAITER, MFC_PUTLLUC_CMD);
    const uint64_t took = wait_return(&w, t0, WAKE_MS);
    CHECK(took != ~(uint64_t)0);
    if (took == ~(uint64_t)0) wait_return(&w, t0, 2000);
    pthread_join(th, NULL);
}

int main(void)
{
    printf("SPURS task wake on an SPU-side signal\n");
    spu_context_init(&g_spu, 0);   /* points ls at its own store */
    test_putllc_signal_wakes();
    test_putlluc_signal_wakes();
    test_other_task_stays_parked();
    test_other_taskset_stays_parked();
    test_waiter_behind_other_entries_wakes();
    test_unmatched_remove_keeps_bookkeeping();
    printf("SPURS task wake on an SPU-side signal: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * The runtime beyond the SPU channel layer, stubbed as in
 * test_spu_lockline_peer.c. None of it is on the path this test drives; the
 * clock only feeds the WAIT_SIGNAL log line.
 * -----------------------------------------------------------------------*/
void ppu_resv_break_store(uint64_t ea)                 { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w)
{
    (void)a; (void)v; (void)w;
}
unsigned long long ps3_ms_now(void)                    { return now_ms(); }
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

uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }
