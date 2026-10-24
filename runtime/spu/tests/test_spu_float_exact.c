/* spu_fa/fs/fm/fma/fms/fnms (the NEON, AVX2 or scalar form this build selects) against the exact
 * per-lane references (spu_*_ref: integer arithmetic, one truncation) -- every lane must match
 * bit for bit. Three passes:
 *  1. classes: a word set with every biased exponent 0..255, both signs and edge fractions (0, 1,
 *     2, half, all-ones, ...); fa/fs/fm over every pair of it, so every exponent pair and gap,
 *     zero and denormal pattern and extended-range word meets every other;
 *  2. fma/fms/fnms over every triple of biased exponents (16.7M), fractions drawn per lane from
 *     the edge set or at random;
 *  3. random vectors from a mix of generators aimed at the hard spots: results near 2^-126 and
 *     2^128, cancellation in fa/fs and fma (z close to -x*y), fma sums rounded onto a 24-bit
 *     boundary, wide exponent gaps, zeros of every kind, extended-range words, raw words.
 * Build (arm64, or x86 with -mavx2 -mfma):
 *   cc -O2 -I runtime/spu -I include runtime/spu/tests/test_spu_float_exact.c -lm -o t && ./t [scale]
 * scale (default 1) multiplies the pass-3 size: 1 = 64M vectors (256M lanes). */
#include "spu_helpers.h"
#include "../../../tests/spu_ls_watch_fixture.h"
#include <stdio.h>
#include <stdlib.h>

static uint64_t s_rng = 0x2545F4914F6CDD1Dull;
static inline uint32_t rnd32(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return (uint32_t)(s_rng >> 16);
}

static const uint32_t k_frac[] = { 0x000000u, 0x000001u, 0x000002u, 0x400000u, 0x7FFFFFu, 0x7FFFFEu,
                                   0x3FFFFFu, 0x555555u, 0x0F0F0Fu, 0x100000u };
#define NFRAC (sizeof k_frac / sizeof k_frac[0])

static long s_bad, s_lanes;
static const char* const k_ops[6] = { "fa", "fs", "fm", "fma", "fms", "fnms" };

static void run(int op, u128 a, u128 b, u128 c)
{
    u128 got, ref;
    switch (op) {
    case 0:  got = spu_fa(a, b);      ref = spu_fa_ref(a, b); break;
    case 1:  got = spu_fs(a, b);      ref = spu_fs_ref(a, b); break;
    case 2:  got = spu_fm(a, b);      ref = spu_fm_ref(a, b); break;
    case 3:  got = spu_fma(a, b, c);  ref = spu_fma_ref(a, b, c); break;
    case 4:  got = spu_fms(a, b, c);  ref = spu_fms_ref(a, b, c); break;
    default: got = spu_fnms(a, b, c); ref = spu_fnms_ref(a, b, c); break;
    }
    s_lanes += 4;
    for (int i = 0; i < 4; i++)
        if (got._u32[i] != ref._u32[i] && s_bad++ < 20)
            printf("MISMATCH %s a=%08X b=%08X c=%08X got=%08X ref=%08X\n", k_ops[op],
                   a._u32[i], b._u32[i], c._u32[i], got._u32[i], ref._u32[i]);
}

static inline uint32_t word(uint32_t sign, uint32_t be, uint32_t frac) { return sign << 31 | be << 23 | frac; }
static inline uint32_t frac_any(void) { return (rnd32() & 1) ? k_frac[rnd32() % NFRAC] : rnd32() & 0x7FFFFFu; }
static inline uint32_t clampe(int e) { return e < 0 ? 0 : e > 255 ? 255 : (uint32_t)e; }

/* one lane of pass 3: kind picks the generator */
static void gen_lane(int kind, uint32_t* x, uint32_t* y, uint32_t* z)
{
    const uint32_t s0 = rnd32() & 1, s1 = rnd32() & 1, s2 = rnd32() & 1;
    switch (kind) {
    case 0:                                             /* raw words */
        *x = rnd32(); *y = rnd32(); *z = rnd32(); break;
    case 1: {                                           /* ordinary */
        *x = word(s0, 100 + rnd32() % 56, rnd32() & 0x7FFFFF); *y = word(s1, 100 + rnd32() % 56, rnd32() & 0x7FFFFF);
        *z = word(s2, 100 + rnd32() % 56, rnd32() & 0x7FFFFF); break; }
    case 2: {                                           /* products near 2^-126 (and the 2^-102 band) */
        const int ex = 1 + (int)(rnd32() % 254), ey = 254 - ex - (int)(rnd32() % 32) + 8;
        *x = word(s0, (uint32_t)ex, frac_any()); *y = word(s1, clampe(ey), frac_any());
        *z = word(s2, clampe(1 + (int)(rnd32() % 30)), frac_any()); break; }
    case 3: {                                           /* products and sums near 2^128 */
        const int ex = 1 + (int)(rnd32() % 254), ey = 254 + 127 - ex - 2 + (int)(rnd32() % 4);
        *x = word(s0, (uint32_t)ex, frac_any()); *y = word(s1, clampe(ey), frac_any());
        *z = word(s2, 250 + rnd32() % 6, frac_any()); break; }
    case 4: {                                           /* fa/fs cancellation: y = -x nudged */
        *x = rnd32(); if (rnd32() & 1) *x = (*x & 0x807FFFFFu) | (1 + rnd32() % 254) << 23;
        *y = (*x ^ 0x80000000u) + (rnd32() % 512) - 256; *z = rnd32(); break; }
    case 5: {                                           /* fma cancellation: z = -(x*y), nudged */
        *x = word(s0, 1 + rnd32() % 254, rnd32() & 0x7FFFFF); *y = word(s1, 1 + rnd32() % 254, rnd32() & 0x7FFFFF);
        u128 a, b; memset(&a, 0, 16); memset(&b, 0, 16); a._u32[0] = *x; b._u32[0] = *y;
        *z = (spu_fm_ref(a, b)._u32[0] ^ 0x80000000u) + (rnd32() % 16) - 8;
        if (rnd32() & 1) *z = (*z & 0xFF800000u) | (rnd32() & 0x7FFFFF); break; }
    case 6: {                                           /* fma onto a boundary: short significands, tiny z */
        const uint32_t ea = 100 + rnd32() % 56, eb = 100 + rnd32() % 56;
        *x = word(s0, ea, (rnd32() & 7u) << 20); *y = word(s1, eb, (rnd32() & 7u) << 20);
        const int ep = (int)ea + (int)eb - 127;
        *z = (rnd32() & 1) ? word(s2, clampe(ep - (int)(rnd32() % 24)), (rnd32() & 7u) << 20)
                           : word(s2, clampe(ep - 24 - (int)(rnd32() % 80)), rnd32() & 1 ? 0 : rnd32() & 0x7FFFFF);
        break; }
    case 7: {                                           /* wide exponent gaps */
        const uint32_t ex = 1 + rnd32() % 254;
        *x = word(s0, ex, frac_any()); *y = word(s1, clampe((int)ex - 20 - (int)(rnd32() % 120)), frac_any());
        if (rnd32() & 1) { const uint32_t t = *x; *x = *y; *y = t; }
        *z = word(s2, clampe((int)ex + (int)(rnd32() % 120) - 60), frac_any()); break; }
    case 8: {                                           /* fma results at and just below 2^-126 */
        const int ex = 1 + (int)(rnd32() % 254), ey = 128 - ex + (int)(rnd32() % 40);
        *x = word(s0, (uint32_t)ex, rnd32() & 0x7FFFFF); *y = word(s1, clampe(ey), rnd32() & 0x7FFFFF);
        const double pd = (double)spu__sf_f(*x) * (double)spu__sf_f(*y);
        const float zf = (float)((pd < 0 ? -0x1p-126 : 0x1p-126) - pd);   /* z ~ +-2^-126 - x*y */
        memcpy(z, &zf, 4);
        *z += (rnd32() % 9) - 4;
        break; }
    default: {                                          /* zeros, denormals, extended range, mixed */
        uint32_t* w[3] = { x, y, z };
        for (int k = 0; k < 3; k++) {
            const uint32_t r = rnd32() % 6, sg = rnd32() & 1;
            *w[k] = r == 0 ? word(sg, 0, 0) : r == 1 ? word(sg, 0, frac_any() | (rnd32() & 1))
                  : r == 2 ? word(sg, 255, frac_any()) : r == 3 ? word(sg, 254, frac_any())
                  : r == 4 ? word(sg, 1 + rnd32() % 3, frac_any()) : word(sg, 100 + rnd32() % 56, rnd32() & 0x7FFFFF);
        }
        break; }
    }
}

static int all_passes(long scale, const char* mode)
{
    s_bad = 0; s_lanes = 0;
    printf("test_spu_float_exact: FPCR %s\n", mode);

    /* pass 1: every pair of the class words for fa, fs, fm */
    static uint32_t cls[256 * NFRAC * 2];
    int n = 0;
    for (uint32_t e = 0; e < 256; e++)
        for (unsigned f = 0; f < NFRAC; f++)
            for (uint32_t sg = 0; sg < 2; sg++) cls[n++] = word(sg, e, k_frac[f]);
    for (int op = 0; op < 3; op++)
        for (int i = 0; i < n; i++) {
            u128 a, b, c; memset(&c, 0, 16);
            for (int j = 0; j < n; j += 4) {
                for (int k = 0; k < 4; k++) { a._u32[k] = cls[i]; b._u32[k] = cls[j + k]; }
                run(op, a, b, c);
                for (int k = 0; k < 4; k++) { a._u32[k] = cls[j + k]; b._u32[k] = cls[i]; }   /* both orders, mixed lanes */
                run(op, a, b, c);
            }
        }
    printf("test_spu_float_exact: pass 1 (class pairs, %d words): %ld lanes, %ld mismatches\n", n, s_lanes, s_bad);

    /* pass 2: every biased exponent triple for fma, fms, fnms */
    const long lanes1 = s_lanes;
    for (uint32_t ex = 0; ex < 256; ex++)
        for (uint32_t ey = 0; ey < 256; ey++)
            for (uint32_t ez = 0; ez < 256; ez += 4) {
                u128 a, b, c;
                for (int k = 0; k < 4; k++) {
                    a._u32[k] = word(rnd32() & 1, ex, frac_any());
                    b._u32[k] = word(rnd32() & 1, ey, frac_any());
                    c._u32[k] = word(rnd32() & 1, ez + (uint32_t)k, frac_any());
                }
                run(3 + (int)((ex + ey + ez) % 3), a, b, c);
                for (int k = 0; k < 4; k++) c._u32[k] = word(rnd32() & 1, ez + (uint32_t)k, frac_any());
                run(3 + (int)((ex + ey + ez + 1) % 3), a, b, c);
            }
    printf("test_spu_float_exact: pass 2 (fma exponent triples): %ld lanes, %ld mismatches\n", s_lanes - lanes1, s_bad);

    /* pass 3: random vectors, lanes from mixed generators */
    const long lanes2 = s_lanes, N = 64L * 1000 * 1000 * scale;
    for (long it = 0; it < N; it++) {
        u128 a, b, c;
        const int kind = (int)(it % 10);
        for (int k = 0; k < 4; k++) {
            /* mostly one generator per vector, sometimes one per lane */
            gen_lane((it & 0x30) ? kind : (int)(rnd32() % 10), &a._u32[k], &b._u32[k], &c._u32[k]);
        }
        run((int)((it / 10) % 6), a, b, c);
    }
    printf("test_spu_float_exact: pass 3 (random): %ld lanes, %ld mismatches\n", s_lanes - lanes2, s_bad);

    if (s_bad) { printf("test_spu_float_exact: FAILED, %ld mismatching lanes of %ld\n", s_bad, s_lanes); return 1; }
    printf("test_spu_float_exact: ok, %ld lanes bit-identical\n", s_lanes);
    return 0;
}

#if defined(__aarch64__) && defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS)
static uint64_t fpcr_get(void) { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v; }
static void fpcr_set(uint64_t v) { __asm__ volatile("msr fpcr, %0" :: "r"(v)); }
/* SPU_FLOAT_RZ: double precision and frds must give the default-FPCR answer on a thread whose
 * FPCR is round toward zero with flush-to-zero; FPCR must be back afterwards. */
static long dp_under_rz(long n)
{
    long bad = 0;
    for (long it = 0; it < n; it++) {
        u128 a, b, c;
        for (int k = 0; k < 4; k++) {
            a._u32[k] = rnd32(); b._u32[k] = rnd32(); c._u32[k] = rnd32();
            if (it & 1) { a._u32[k & ~1] &= 0x800FFFFFu; }   /* denormal doubles */
            if (it & 2) { b._u32[k & ~1] = (b._u32[k & ~1] & 0x800FFFFFu) | 0x3FF00000u; }
        }
        u128 want[12], got[12];
        want[0] = spu_dfa(a, b); want[1] = spu_dfs(a, b); want[2] = spu_dfm(a, b);
        want[3] = spu_dfma(a, b, c); want[4] = spu_dfms(a, b, c); want[5] = spu_dfnms(a, b, c);
        want[6] = spu_dfnma(a, b, c); want[7] = spu_frds(a); want[8] = spu_dfceq(a, b);
        want[9] = spu_dfcmeq(a, b); want[10] = spu_dfcgt(a, b); want[11] = spu_dfcmgt(a, b);
        fpcr_set(SPU_FPCR_RZFZ);
        got[0] = spu_dfa(a, b); got[1] = spu_dfs(a, b); got[2] = spu_dfm(a, b);
        got[3] = spu_dfma(a, b, c); got[4] = spu_dfms(a, b, c); got[5] = spu_dfnms(a, b, c);
        got[6] = spu_dfnma(a, b, c); got[7] = spu_frds(a); got[8] = spu_dfceq(a, b);
        got[9] = spu_dfcmeq(a, b); got[10] = spu_dfcgt(a, b); got[11] = spu_dfcmgt(a, b);
        const uint64_t after = fpcr_get();
        fpcr_set(0);
        if (after != SPU_FPCR_RZFZ) bad++;
        for (int k = 0; k < 12; k++) bad += memcmp(&want[k], &got[k], 16) != 0;
    }
    return bad;
}
#endif

int main(int argc, char** argv)
{
    const long scale = argc > 1 ? atol(argv[1]) : 1;
    int rc = all_passes(scale, "default (round to nearest)");
#if defined(__aarch64__) && defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS)
    /* SPU_FLOAT_RZ: the same passes on a thread running round toward zero with flush-to-zero,
     * where spu_fa.. take their native form. */
    fpcr_set(SPU_FPCR_RZFZ);
    rc |= all_passes(scale, "round toward zero, flush-to-zero (SPU_FLOAT_RZ)");
    fpcr_set(0);
    const long dbad = dp_under_rz(4L * 1000 * 1000);
    printf("test_spu_float_exact: double precision under SPU_FLOAT_RZ: %ld mismatches\n", dbad);
    rc |= dbad != 0;
#endif
    return rc;
}
