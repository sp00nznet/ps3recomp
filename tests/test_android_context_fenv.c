/*
 * Regression: native AArch64 libucontext contexts must preserve FPCR.RMode
 * (bits 22..23) and FPSR cumulative exception flags (bits 0..4) across
 * getcontext capture, swapcontext and setcontext. Trap enables are untouched.
 *
 * Build (actual NDK clang, installed libucontext prefix):
 *   $NDK/toolchains/llvm/prebuilt/<host>/bin/aarch64-linux-android24-clang \
 *     -std=c17 -O2 -Wall -Wextra -I$UCTX_PREFIX/include \
 *     tests/test_android_context_fenv.c $UCTX_PREFIX/lib/libucontext.a \
 *     -o test_android_context_fenv
 *   adb push test_android_context_fenv /data/local/tmp/
 *   adb shell /data/local/tmp/test_android_context_fenv
 */
#if !defined(__aarch64__)
#error "test_android_context_fenv.c requires an AArch64 target"
#endif

#include <stdint.h>
#include <stdio.h>
#include <libucontext/libucontext.h>

#define ITERS 40u
#define STACK_SIZE (128u * 1024u)
#define RM_SHIFT 22
#define RM_MASK (UINT64_C(3) << RM_SHIFT)
#define CUM_MASK UINT64_C(0x1f)
#define TRAP_MASK UINT64_C(0x9f00)
#define ALWAYS_INLINE static inline __attribute__((always_inline))

static libucontext_ucontext_t sched_ctx, fib_ctx[2];
static _Alignas(16) unsigned char fib_stack[2][STACK_SIZE];

static uint64_t orig_cr, orig_sr, got_cr, got_sr;
static unsigned steps[2], seen[2], fib_k[2], sched_k, sched_i;
static int done[2], runner = -1;
static struct {
    int set, is_count, who, rc;
    const char *stage;
    unsigned iter;
    uint64_t exp_rm, act_rm, exp_fs, act_fs, exp_trap, act_trap;
    long exp_n, act_n;
} fail;

static const uint64_t sched_rm = 1, fib_rm[2] = {2, 3};
static const uint64_t inh_rm[2] = {3, 2}, inh_fs[2] = {0x06, 0x05};

/* Owner bits keep every party distinct: scheduler 0x01, fiber0 0x02, fiber1 0x04. */
static uint64_t sched_fs(unsigned k) { return 0x01u | ((uint64_t)(k % 3u + 1u) << 3); }
static uint64_t fib_fs(int i, unsigned k)
{
    return (UINT64_C(0x02) << i) | ((uint64_t)((k + 1u + (unsigned)i) % 3u + 1u) << 3);
}

ALWAYS_INLINE uint64_t rd_fpcr(void) { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v; }
ALWAYS_INLINE uint64_t rd_fpsr(void) { uint64_t v; __asm__ volatile("mrs %0, fpsr" : "=r"(v)); return v; }
ALWAYS_INLINE void wr_fpcr(uint64_t v) { __asm__ volatile("msr fpcr, %0" : : "r"(v) : "memory"); }
ALWAYS_INLINE void wr_fpsr(uint64_t v) { __asm__ volatile("msr fpsr, %0" : : "r"(v) : "memory"); }

ALWAYS_INLINE void put_fp(uint64_t rm, uint64_t fs)
{
    uint64_t cr = rd_fpcr(), sr = rd_fpsr();
    wr_fpcr((cr & ~RM_MASK) | (rm << RM_SHIFT));
    wr_fpsr((sr & ~CUM_MASK) | fs);
}

ALWAYS_INLINE void grab(void) { got_cr = rd_fpcr(); got_sr = rd_fpsr(); }

static int check(const char *stage, int who, unsigned iter, int rc, uint64_t rm, uint64_t fs)
{
    uint64_t arm = (got_cr & RM_MASK) >> RM_SHIFT, afs = got_sr & CUM_MASK;
    uint64_t etrap = orig_cr & TRAP_MASK, atrap = got_cr & TRAP_MASK;
    if (fail.set)
        return 0;
    if (rc == 0 && arm == rm && afs == fs && atrap == etrap)
        return 1;
    fail.set = 1; fail.is_count = 0; fail.stage = stage; fail.who = who; fail.iter = iter;
    fail.rc = rc; fail.exp_rm = rm; fail.act_rm = arm; fail.exp_fs = fs; fail.act_fs = afs;
    fail.exp_trap = etrap; fail.act_trap = atrap;
    return 0;
}

static void bad_count(const char *stage, int who, unsigned iter, long exp, long act)
{
    if (fail.set)
        return;
    fail.set = 1; fail.is_count = 1; fail.stage = stage; fail.who = who; fail.iter = iter;
    fail.exp_n = exp; fail.act_n = act;
}

static void fiber_body(int i)
{
    grab();
    if (!check("fiber entry inherited getcontext state", i, 0, 0, inh_rm[i], inh_fs[i]))
        goto out;
    for (fib_k[i] = 0; fib_k[i] < ITERS; fib_k[i]++) {
        uint64_t fs = fib_fs(i, fib_k[i]);
        steps[i]++;
        runner = i;
        put_fp(fib_rm[i], fs);
        int rc = libucontext_swapcontext(&fib_ctx[i], &sched_ctx);
        grab();
        if (!check("fiber resume after swapcontext", i, fib_k[i], rc, fib_rm[i], fs))
            goto out;
    }
    steps[i]++;
    runner = i;
    done[i] = 1;
    put_fp(fib_rm[i], fib_fs(i, ITERS));
out:
    libucontext_setcontext(&sched_ctx);
    bad_count("fiber setcontext to scheduler returned", i, fib_k[i], 0, -1);
    /* Returning falls back to uc_link == &sched_ctx. */
}

static void fiber0_entry(void) { fiber_body(0); }
static void fiber1_entry(void) { fiber_body(1); }

static void run_scheduler(void)
{
    for (sched_i = 0; sched_i < 2; sched_i++) {
        put_fp(inh_rm[sched_i], inh_fs[sched_i]);
        int rc = libucontext_getcontext(&fib_ctx[sched_i]);
        grab();
        if (!check("getcontext capture", (int)sched_i, 0, rc, inh_rm[sched_i], inh_fs[sched_i]))
            return;
        fib_ctx[sched_i].uc_stack.ss_sp = fib_stack[sched_i];
        fib_ctx[sched_i].uc_stack.ss_size = sizeof fib_stack[sched_i];
        fib_ctx[sched_i].uc_link = &sched_ctx;
        libucontext_makecontext(&fib_ctx[sched_i], sched_i ? fiber1_entry : fiber0_entry, 0);
    }
    for (sched_k = 0; !(done[0] && done[1]); sched_k++) {
        if (sched_k >= 2u * (ITERS + 1u)) {
            bad_count("scheduler switch limit exceeded", -1, sched_k, 2L * (ITERS + 1u), sched_k + 1L);
            return;
        }
        sched_i = sched_k & 1u;
        if (done[sched_i])
            sched_i ^= 1u;
        uint64_t fs = sched_fs(sched_k);
        runner = -1;
        put_fp(sched_rm, fs);
        int rc = libucontext_swapcontext(&sched_ctx, &fib_ctx[sched_i]);
        grab();
        if (fail.set)
            return;
        if (!check("scheduler return", -1, sched_k, rc, sched_rm, fs))
            return;
        seen[sched_i]++;
        if (runner != (int)sched_i) {
            bad_count("runner identity after return", -1, sched_k, (long)sched_i, runner);
            return;
        }
        if (steps[sched_i] != seen[sched_i]) {
            bad_count("fiber step count", (int)sched_i, sched_k, seen[sched_i], steps[sched_i]);
            return;
        }
    }
    if (sched_k != 2u * (ITERS + 1u))
        bad_count("total scheduler switches", -1, sched_k, 2L * (ITERS + 1u), sched_k);
    for (sched_i = 0; sched_i < 2; sched_i++)
        if (seen[sched_i] != ITERS + 1u)
            bad_count("per-fiber switch total", (int)sched_i, sched_k, ITERS + 1L, seen[sched_i]);
}

int main(void)
{
    orig_cr = rd_fpcr();
    orig_sr = rd_fpsr();
    run_scheduler();
    wr_fpcr(orig_cr);
    wr_fpsr(orig_sr);
    if (fail.set) {
        char who[16];
        snprintf(who, sizeof who, fail.who < 0 ? "scheduler" : "fiber%d", fail.who);
        fprintf(stderr, "FAIL: stage=\"%s\" who=%s iter=%u\n", fail.stage, who, fail.iter);
        if (fail.is_count)
            fprintf(stderr, "  expected=%ld actual=%ld\n", fail.exp_n, fail.act_n);
        else
            fprintf(stderr,
                    "  rc=%d FPCR.RMode expected=%llu actual=%llu; FPSR[4:0] expected=0x%02llx "
                    "actual=0x%02llx; FPCR traps expected=0x%04llx actual=0x%04llx\n",
                    fail.rc, (unsigned long long)fail.exp_rm, (unsigned long long)fail.act_rm,
                    (unsigned long long)fail.exp_fs, (unsigned long long)fail.act_fs,
                    (unsigned long long)fail.exp_trap, (unsigned long long)fail.act_trap);
        return 1;
    }
    printf("PASS: actual native libucontext context FP-control test passed "
           "(2 fibers x %u swapcontext + setcontext completion, %u scheduler switches; "
           "FPCR.RMode and FPSR cumulative flags preserved)\n", ITERS, sched_k);
    return 0;
}
