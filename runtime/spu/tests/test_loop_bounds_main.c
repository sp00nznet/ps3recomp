/*
 * T-0001 regression test: the inFamous WWS scatter loop's bound contract.
 *
 * Drives test_loop_bounds.elf (a faithful transcription of image
 * spu_0003_at_006A3300's 0x12D80/0x12E40 scatter loop — the producer of
 * the once-per-boot stack-smash crash) for several (count, shiftA, shiftB)
 * parameter sets, and asserts by sentinel-scanning the ENTIRE 256 KB local
 * store:
 *
 *   1. the loop exits (selb picks the r0 exit link, bi dispatches to the
 *      stop stub) — no hang, no branch-to-data;
 *   2. EXACTLY count records landed: every 16-byte line inside the
 *      predicted span changed, and NOTHING outside it (for shiftB=0 the
 *      span is contiguous, so full coverage proves the iteration count);
 *   3. any store outside the span — the T-0001 flood signature that
 *      flattens the kernel stack — fails loudly.
 *
 * Part A (host-side, same binary) pins the helper arithmetic the loop's
 * contract depends on: shl's 6-bit vs shlqbi's 3-bit shift masking (the
 * asymmetry that turns shift>7 into a 256x overrun), sfi negation, ceqi,
 * and wrap-around addition — plus a model sweep asserting that consistent
 * parameters yield exactly-count iterations and that the pathological
 * families (shift>7, count*16<<shift overflowing 32 bits) are exactly the
 * ones that overshoot or never exit.
 *
 * run_tests.sh builds this against the lifted loop; spu_interp_selftest.c
 * runs the same guest encoding through the interpreter.
 */

#include "spu_recomp.h"
#include "spu_helpers.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* Required by spu_dma.h (referenced via the helpers chain). Unused. */
uint8_t* vm_base = 0;
int g_cri_video_dma = 0;

/* stop/halt hooks required by lifter output */
void spu_stop(spu_context* ctx) { (void)ctx; }
void spu_halt(spu_context* ctx) { (void)ctx; }

/* LS-watch diagnostics globals the lifted write path references; all off. */
int g_spu_ls_watch_n = 0, g_spu_ls_dbg = 0, g_spu_smc_watch = 0, g_wws_code_probe = 0;
void spu_ls_watch_slow(const struct spu_context* c, uint32_t lsa, int is_write,
                       const uint8_t* p, uint32_t pc, uint32_t lr)
{ (void)c; (void)lsa; (void)is_write; (void)p; (void)pc; (void)lr; }

/* ---- channel overrides: the loop issues no channel ops ---- */
u128 spu_rdch(spu_context* c, uint32_t ch) { (void)c; (void)ch; return spu_zero(); }
uint32_t spu_rchcnt(spu_context* c, uint32_t ch) { (void)c; (void)ch; return 1; }
void spu_wrch(spu_context* c, uint32_t ch, u128 v) { (void)c; (void)ch; (void)v; }

/* ---- indirect-branch dispatch (the loop's selb+bi exit idiom) ----
 * The lift registers every function via spu_register_function; bi $rN
 * sets ctx->pc + ctx->tramp_fn = spu_indirect_branch and returns, so the
 * harness (this file) drains the trampoline. */
static struct { uint32_t addr; void (*fn)(spu_context*); } fns[64];
static int n_fns = 0;
void spu_register_function(uint32_t addr, void (*fn)(spu_context*)) {
    if (n_fns < 64) { fns[n_fns].addr = addr; fns[n_fns].fn = fn; n_fns++; }
}
void spu_indirect_branch(spu_context* ctx) {
    uint32_t a = ctx->pc & 0x3FFFC;
    for (int i = 0; i < n_fns; i++)
        if (fns[i].addr == a) { fns[i].fn(ctx); return; }
    fprintf(stderr, "FAIL: indirect branch to unregistered LS 0x%05X\n", ctx->pc);
    ctx->status = SPU_STATUS_STOPPED_BY_HALT;
}

/* ---- guest run ---- */
#define T0001_HEAD 0x00
#define T0001_BODY 0x80
#define T0001_EXIT 0xC0
#define T0001_BASE 0x8000      /* scratch record area (16-byte aligned) */
#define LS_SIZE    0x40000

static int g_fail = 0;
#define CHECK(cond, msg, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: " msg "\n", __VA_ARGS__); g_fail = 1; } } while (0)

/* Runs one (count, shiftA, shiftB) case. Returns 0 ok. Sentinel-fills the
 * whole LS (except the code area) first, so a single scan afterward proves
 * both full coverage of the expected span and zero collateral damage. */
static void run_case(uint32_t cnt, uint32_t sa, uint32_t sb)
{
    static spu_context ctx;
    memset(&ctx, 0, sizeof ctx);
    spu_context_init(&ctx, 0);
    memset(ctx.ls, 0xA5, LS_SIZE);
    /* keep the code area (program is below 0x1000) intact */
    (void)0;

    ctx.gpr[0]._u32[0] = T0001_EXIT;       /* exit link (selb picks this) */
    ctx.gpr[1]._u32[0] = 0x3F000;          /* stack pointer, away from scratch */
    ctx.gpr[4]._u32[0] = T0001_BASE;       /* record base */
    ctx.gpr[5]._u32[0] = cnt;
    ctx.gpr[8]._u32[0] = sa;
    ctx.gpr[9]._u32[0] = sb;
    ctx.tramp_fn = 0;

    spu_func_00000000(&ctx);
    long hops = 0;
    while (ctx.tramp_fn) {
        void (*fn)(spu_context*) = ctx.tramp_fn;
        ctx.tramp_fn = 0;
        fn(&ctx);
        if (++hops > 2000000) { fprintf(stderr, "FAIL: trampoline runaway\n"); g_fail = 1; return; }
    }

    uint32_t stride = 16u << (sb & 7);     /* S: stream pitch (bytes between the
                                           * 4 interleaved record streams) */
    /* The real loop (verified against the actual guest bytes at image-4 LS
     * 0x12D80, 2026-10-06): the head zeroes r14 (prologue trick), so round 1
     * stores SET 0 with the pipeline's seed records and round 2 re-stores set
     * 0 with real data; the exit round stores too. Net: cnt/4+1 store
     * rounds, cnt+4 record WRITES into cnt distinct records -- 4 streams, one
     * every stride bytes, cnt/4 interleaved sets advancing 4*stride per
     * round. Top line = (cnt/4-1)*4S + 3S + 16 = (cnt-1)*stride + 16. */
    uint32_t span   = stride * (cnt - 1) + 16;

    CHECK(ctx.status == SPU_STATUS_STOPPED_BY_STOP,
          "cnt=%u sa=%u sb=%u: no clean exit (status=%d pc=0x%05X)",
          cnt, sa, sb, ctx.status, ctx.pc);

    /* every 16-byte line in the expected span changed, none outside */
    uint32_t lo = T0001_BASE & ~0xFu, hi = (T0001_BASE + span + 15) & ~0xFu;
    for (uint32_t a = 0; a < LS_SIZE; a += 16) {
        int changed = memcmp(&ctx.ls[a], "\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5", 16) != 0;
        if (a < 0x1000) continue;                     /* code area */
        int inside = a >= lo && a < hi;
        if (inside && stride == 16 && changed == 0 && a < hi - 16) {
            /* contiguous case (shiftB=0): full coverage below the last
             * iteration's tail proves exactly cnt iterations. */
            fprintf(stderr, "FAIL: cnt=%u sa=%u sb=%u: hole at 0x%05X inside span "
                            "(early exit?)\n", cnt, sa, sb, a);
            g_fail = 1;
        }
        if (!inside && changed) {
            fprintf(stderr, "FAIL: cnt=%u sa=%u sb=%u: FLOOD signature -- store at "
                            "0x%05X outside span [0x%05X,0x%05X)\n",
                    cnt, sa, sb, a, lo, hi);
            g_fail = 1;
        }
    }
    if (!g_fail)
        printf("OK: cnt=%-4u sa=%u sb=%u -> exited, %u records in [0x%05X,0x%05X)\n",
               cnt, sa, sb, cnt, lo, hi);
}

/* ---- Part A: host-side helper/loop-contract pins ---- */
static u128 sp1(uint32_t v) { u128 r = spu_zero(); r._u32[0] = v; return r; }

static int model_iters(uint32_t cnt, uint32_t sa, uint32_t sb, uint32_t cap,
                       uint32_t* footprint)
{
    /* Exact replay of the loop's arithmetic with the real helpers, including
     * the head's shlqbii x4 that the first transcription missed: the steady
     * body steps the counter by 4*(16<<(sa&7)) and the store offset by
     * 4*(16<<(sb&7)), while the four stream bases stay one 16<<(sb&7) apart.
     * Consequence: the loop runs cnt/4 iterations and cnt must be divisible
     * by 4 for the counter to ever hit zero (cnt=5834 -> NEVER exits: the
     * boot8 hang family; cnt%4==0 but huge -> full-LS sweep then exit: the
     * boot6 crash family). */
    u128 r11 = spu_splat_u32(16);
    u128 r2 = spu_shl(spu_shli(spu_splat_u32(cnt), 4), sp1(sa));
    r2 = spu_sfi(r2, 0);
    u128 r8 = spu_shlqbii(spu_shlqbi(r11, sp1(sa)), 2);
    u128 r9 = spu_shlqbii(spu_shlqbi(r11, sp1(sb)), 2);
    u128 r12 = spu_zero();
    int it = 0;
    uint32_t lo = 0xFFFFFFFF, hi = 0;
    while (it < (int)cap) {
        if (spu_ceqi(r2, 0)._u32[0]) break;
        uint32_t a = (T0001_BASE + r12._u32[0]) & 0x3FFF0;
        if (a < lo) lo = a;
        if (a > hi) hi = a;
        r2 = spu_a(r8, r2);
        r12 = spu_a(r9, r12);
        it++;
    }
    if (footprint) *footprint = (it && hi >= lo) ? (hi - lo + 16) : 0;
    return it == (int)cap ? -1 : it;   /* -1 = never exited within cap */
}

static void host_contract(void)
{
    /* the T-0001 asymmetry pair: shl honors 6 bits, shlqbi only 3 */
    CHECK(spu_shl(spu_splat_u32(1), sp1(8))._u32[0] == 0x100,
          "shl 1<<8 = %#x (want 0x100)", spu_shl(spu_splat_u32(1), sp1(8))._u32[0]);
    CHECK(spu_shl(spu_splat_u32(1), sp1(64))._u32[0] == 1,
          "shl shift 64 masks to 0 (got %#x)", spu_shl(spu_splat_u32(1), sp1(64))._u32[0]);
    CHECK(spu_shlqbi(spu_splat_u32(16), sp1(8))._u32[0] == 16,
          "shlqbi shift 8 masks to 0: 16 stays 16 (got %u)",
          spu_shlqbi(spu_splat_u32(16), sp1(8))._u32[0]);
    CHECK(spu_shlqbi(spu_splat_u32(16), sp1(7))._u32[0] == 0x800,
          "shlqbi shift 7: 16<<7 (got %u)", spu_shlqbi(spu_splat_u32(16), sp1(7))._u32[0]);
    CHECK(spu_sfi(spu_splat_u32(0x800), 0)._u32[0] == 0xFFFFF800u,
          "sfi negation (got %#x)", spu_sfi(spu_splat_u32(0x800), 0)._u32[0]);
    CHECK(spu_a(spu_splat_u32(0xFFFFFFE0u), spu_splat_u32(0x20))._u32[0] == 0,
          "a wraps mod 2^32 (got %#x)",
          spu_a(spu_splat_u32(0xFFFFFFE0u), spu_splat_u32(0x20))._u32[0]);

    /* consistent parameters: cnt/4 iterations (the x4 step), footprint =
     * cnt*S + 3S + 16; cnt must be divisible by 4 to exit at all */
    struct { uint32_t c, a, b; } good[] = {
        {4,0,0},{32,0,0},{128,0,0},{608,0,0},{128,3,3},{16,7,7},
    };
    for (unsigned k = 0; k < sizeof good / sizeof good[0]; k++) {
        uint32_t fp = 0;
        int it = model_iters(good[k].c, good[k].a, good[k].b, 100000, &fp);
        CHECK(it >= 0 && (uint32_t)it == good[k].c / 4,
              "model cnt=%u sa=%u: exited after %d iterations (want %u)",
              good[k].c, good[k].a, it, good[k].c / 4);
    }
    /* the flood families: (a) count not divisible by 4 -- the counter starts
     * at -(cnt*16 << sa) and steps by 64<<(sa&7); if cnt%4 != 0 it NEVER
     * hits zero, not even across the 2^32 wrap: the boot8 black-screen hang
     * (cnt=5834 = the boot6 dump's record count is exactly such a value);
     * (b) shift>7 (step masks to &7 -> 256x overshoot); (c) count*16<<shift
     * overflowing 32 bits (counter wraps positive, never hits zero).
     * These MUST be flagged as anomalies -- they are exactly what the T-0001
     * tripwire detects in the live game. */
    struct { uint32_t c, a, b; const char* why; } bad[] = {
        {1, 0, 0, "cnt%4!=0: single record -- never exits"},
        {607, 0, 0, "cnt%4!=0: never exits (hang family)"},
        {5834, 0, 0, "cnt%4!=0: never exits -- the boot6/8 flood count"},
        {128, 8, 8, "shift 8: step masks to 0, 256x overshoot"},
        {128, 11, 11, "shift 11: step masks to 3"},
        {1024, 8, 8, "shift 8 with large count"},
    };
    for (unsigned k = 0; k < sizeof bad / sizeof bad[0]; k++) {
        uint32_t fp = 0;
        int it = model_iters(bad[k].c, bad[k].a, bad[k].b, 100000, &fp);
        int anomalous = (it == -1) || (it != -1 && (uint32_t)it > bad[k].c / 4);
        CHECK(anomalous,
              "model cnt=%u sa=%u (%s): NOT anomalous (it=%d) -- the "
              "exit contract changed; T-0001 would regress silently",
              bad[k].c, bad[k].a, bad[k].why, it);
    }
}

int main(void)
{
    spu_recomp_register();               /* fill the dispatch table */
    CHECK(n_fns >= 3, "lift registered %d functions (want >= 3: head/body/exit)", n_fns);

    host_contract();

    /* guest runs: the observed live traffic (all counts divisible by 4 --
     * the exit contract) plus a stride-128 case */
    run_case(32, 0, 0);                  /* observed good calls */
    run_case(128, 0, 0);                 /* observed good calls */
    run_case(4, 0, 0);                   /* degenerate: one iteration, 4 records */
    run_case(128, 3, 3);                 /* stride 128 streams */

    if (!g_fail) printf("loop_bounds: PASS (host contract + %d guest cases)\n", 4);
    return g_fail ? 1 : 0;
}
