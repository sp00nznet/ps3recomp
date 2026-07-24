/*
 * SPU lockstep gate implementation (faithful-adopt, from canersaka's fork).
 *
 * One global run token (spu_context* s_holder) shared by every registered SPU
 * host thread, protected by a single mutex+condvar. Members live in a fixed
 * ring (registration order; tombstoned, never compacted, on unregister). At
 * quantum expiry (yz_lockstep_tick) the holder hands the token to the next
 * RUNNABLE member (skipping members mid-block in a channel wait) and blocks
 * until granted the token again -- cooperative round robin, not a scheduler.
 *
 * Adapted for this tree: timestamps use a local monotonic-ns clock (our RdDec
 * uses dec_base_ns, not the guest timebase, so the decrementer-freeze anchor is
 * kept on the unused dec_start_tb field -- a refinement for a later pass).
 */
#include "spu_lockstep.h"
#include "spu_context.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <intrin.h>
#  define LS_RELAX() _mm_pause()
static SRWLOCK            s_ls_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_ls_cv   = CONDITION_VARIABLE_INIT;
#  define LS_LOCK()       AcquireSRWLockExclusive(&s_ls_lock)
#  define LS_UNLOCK()     ReleaseSRWLockExclusive(&s_ls_lock)
#  define LS_WAIT()       SleepConditionVariableSRW(&s_ls_cv, &s_ls_lock, INFINITE, 0)
#  define LS_BROADCAST()  WakeAllConditionVariable(&s_ls_cv)
static uint64_t ls_now_ns(void)
{
    static LARGE_INTEGER freq; if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (uint64_t)((c.QuadPart * 1000000000ull) / (uint64_t)freq.QuadPart);
}
#else
#  include <pthread.h>
#  include <time.h>
#  define LS_RELAX() ((void)0)
static pthread_mutex_t s_ls_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_ls_cv   = PTHREAD_COND_INITIALIZER;
#  define LS_LOCK()       pthread_mutex_lock(&s_ls_lock)
#  define LS_UNLOCK()     pthread_mutex_unlock(&s_ls_lock)
#  define LS_WAIT()       pthread_cond_wait(&s_ls_cv, &s_ls_lock)
#  define LS_BROADCAST()  pthread_cond_broadcast(&s_ls_cv)
static uint64_t ls_now_ns(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

volatile int g_yz_lockstep_on = -1;

#define YZ_LOCKSTEP_MAX_SPUS         32
#define YZ_LOCKSTEP_DEFAULT_QUANTUM  65536ull
#define YZ_LOCKSTEP_STARVE_PRINTS    10
#define YZ_LOCKSTEP_STARVE_NS        (5ull * 1000000000ull)   /* 5s wall */

typedef struct {
    spu_context* ctx;
    int active;     /* 0 = free/tombstoned slot */
    int runnable;   /* 0 = mid channel wait (not eligible to receive the token) */
} yz_ls_slot;

static yz_ls_slot          s_ring[YZ_LOCKSTEP_MAX_SPUS];
static int                 s_ring_count = 0;
static spu_context*        s_holder = 0;
static uint64_t            s_holder_acquired_ns = 0;
static uint64_t            s_quantum = YZ_LOCKSTEP_DEFAULT_QUANTUM;
static unsigned long long  s_pass_count = 0;
static int                 s_watchdog_prints = 0;

static atomic_flag  s_init_claimed = ATOMIC_FLAG_INIT;
static volatile int s_init_complete = 0;

static void ls_arm_now(void)
{
    const char* e = getenv("YZ_SPU_LOCKSTEP");
    int on = (e && *e && *e != '0') ? 1 : 0;
    if (on) {
        const char* qs = getenv("YZ_LOCKSTEP_QUANTUM");
        uint64_t q = (qs && *qs) ? strtoull(qs, NULL, 10) : YZ_LOCKSTEP_DEFAULT_QUANTUM;
        if (q < 1) q = 1;
        s_quantum = q;
        fprintf(stderr, "[lockstep] ARMED quantum=%llu (YZ_SPU_LOCKSTEP=1; unset to disable)\n",
                (unsigned long long)q);
        fflush(stderr);
    }
    g_yz_lockstep_on = on;
}

int yz_lockstep_enabled(void)
{
    if (s_init_complete) return g_yz_lockstep_on;
    if (!atomic_flag_test_and_set_explicit(&s_init_claimed, memory_order_acq_rel)) {
        ls_arm_now();
        s_init_complete = 1;
    } else {
        while (!s_init_complete) LS_RELAX();
    }
    return g_yz_lockstep_on;
}

static int ls_hot(void)
{
    if (g_yz_lockstep_on == 0) return 0;
    if (g_yz_lockstep_on < 0) return yz_lockstep_enabled();
    return 1;
}

/* --- ring helpers; all callers hold s_ls_lock --- */
static int ls_find_slot(spu_context* ctx)
{
    for (int i = 0; i < s_ring_count; i++)
        if (s_ring[i].active && s_ring[i].ctx == ctx) return i;
    return -1;
}

static spu_context* ls_next_runnable_after(int after)
{
    if (s_ring_count <= 0 || after < 0) return 0;
    for (int step = 1; step < s_ring_count; step++) {
        int i = (after + step) % s_ring_count;
        if (s_ring[i].active && s_ring[i].runnable) return s_ring[i].ctx;
    }
    return 0;
}

static int ls_any_other_active(int after)
{
    for (int i = 0; i < s_ring_count; i++)
        if (i != after && s_ring[i].active) return 1;
    return 0;
}

static void ls_acquire_locked(spu_context* ctx)
{
    uint64_t now = ls_now_ns();
    ctx->dec_start_tb += (now - ctx->lockstep_release_tb);  /* freeze anchor */
    s_holder = ctx;
    s_holder_acquired_ns = now;
}

static int ls_release_locked(spu_context* ctx, int keep_runnable)
{
    int idx = ls_find_slot(ctx);
    uint64_t now = ls_now_ns();

    if (s_holder == ctx && idx >= 0 && ls_any_other_active(idx)) {
        uint64_t held = now - s_holder_acquired_ns;
        if (held > YZ_LOCKSTEP_STARVE_NS && s_watchdog_prints < YZ_LOCKSTEP_STARVE_PRINTS) {
            s_watchdog_prints++;
            fprintf(stderr, "[lockstep] STARVATION token held >5s by spu=%X pc=0x%05X (pass #%llu)\n",
                    ctx->spu_id, ctx->pc & SPU_LS_MASK, (unsigned long long)s_pass_count);
            fflush(stderr);
        }
    }

    if (idx >= 0) s_ring[idx].runnable = keep_runnable;
    ctx->lockstep_release_tb = now;

    if (s_holder != ctx) return 0;

    spu_context* next = (idx >= 0) ? ls_next_runnable_after(idx) : 0;
    s_holder = next;
    if (next) {
        s_pass_count++;
        LS_BROADCAST();
        return 1;
    }
    return 0;
}

/* --- public API --- */
void yz_lockstep_register(spu_context* ctx)
{
    if (!yz_lockstep_enabled()) return;

    LS_LOCK();
    ctx->lockstep_quantum_ctr = 0;
    ctx->lockstep_release_tb = ls_now_ns();

    int idx = -1;
    for (int i = 0; i < s_ring_count; i++) if (!s_ring[i].active) { idx = i; break; }
    if (idx < 0) {
        if (s_ring_count >= YZ_LOCKSTEP_MAX_SPUS) {
            static int wn = 0; if (wn < 4) { wn++;
                fprintf(stderr, "[lockstep] WARNING ring full -- spu=%X runs UNGATED\n", ctx->spu_id);
                fflush(stderr); }
            LS_UNLOCK();
            return;   /* fail-open */
        }
        idx = s_ring_count++;
    }
    s_ring[idx].ctx = ctx; s_ring[idx].active = 1; s_ring[idx].runnable = 1;

    if (s_holder == 0) {
        ls_acquire_locked(ctx);
    } else if (s_holder != ctx) {
        while (s_holder != ctx) LS_WAIT();
        ls_acquire_locked(ctx);
    }
    LS_UNLOCK();
}

void yz_lockstep_unregister(spu_context* ctx)
{
    if (g_yz_lockstep_on != 1) return;
    LS_LOCK();
    int idx = ls_find_slot(ctx);
    if (idx >= 0) {
        if (s_holder == ctx) {
            spu_context* next = ls_next_runnable_after(idx);
            s_ring[idx].active = 0;
            s_holder = next;
            if (next) { s_holder_acquired_ns = ls_now_ns(); LS_BROADCAST(); }
        } else {
            s_ring[idx].active = 0;
        }
    }
    LS_UNLOCK();
}

void yz_lockstep_tick(spu_context* ctx)
{
    if (!ls_hot()) return;
    if (++ctx->lockstep_quantum_ctr < s_quantum) return;
    ctx->lockstep_quantum_ctr = 0;

    LS_LOCK();
    if (ls_release_locked(ctx, /*keep_runnable=*/1)) {
        while (s_holder != ctx) LS_WAIT();
    }
    ls_acquire_locked(ctx);
    LS_UNLOCK();
}

void yz_lockstep_block_begin(spu_context* ctx)
{
    if (!ls_hot()) return;
    LS_LOCK();
    ls_release_locked(ctx, /*keep_runnable=*/0);
    LS_UNLOCK();
}

void yz_lockstep_block_end(spu_context* ctx)
{
    if (!ls_hot()) return;
    LS_LOCK();
    int idx = ls_find_slot(ctx); if (idx >= 0) s_ring[idx].runnable = 1;
    if (s_holder != 0 && s_holder != ctx) {
        while (s_holder != ctx) LS_WAIT();
    }
    ls_acquire_locked(ctx);
    LS_UNLOCK();
}
