/* The vector spu_fi/frest/frsqest/cflts/cfltu/csflt/cuflt (AVX2 on x86) must be bit-identical
 * to the scalar references (spu_*_ref) for every input: random words, ordinary floats, the
 * normal/extended-range boundaries, zeros and denormals, integers of every magnitude, and every
 * scale immediate. Prints a rough speed comparison too. Build on x86 with AVX2:
 *   cc -O2 -mavx2 -mfma -I../../../include -I.. test_spu_float_est.c -o t && ./t */
#include "../spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd32(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return (uint32_t)(s_rng >> 16);
}

static uint32_t rnd_float(void)
{
    uint32_t sign = (rnd32() & 1u) << 31, man = rnd32() & 0x7FFFFFu, e;
    switch (rnd32() % 10) {
    case 0: e = 0; break;                              /* zero / denormal */
    case 1: e = 1 + rnd32() % 4; break;                /* bottom of normal range */
    case 2: e = 248 + rnd32() % 8; break;              /* top, into extended range */
    case 3: return rnd32();                            /* anything */
    case 4: return sign;                               /* +-0 */
    default: e = 110 + rnd32() % 36; break;            /* ordinary */
    }
    return sign | (e << 23) | man;
}

static uint32_t rnd_int(void)
{
    switch (rnd32() % 6) {
    case 0: return 0;
    case 1: return 0x80000000u >> (rnd32() % 32);      /* powers of two */
    case 2: return 0xFFFFFFFFu >> (rnd32() % 32);      /* all-ones runs */
    case 3: return (uint32_t)-(int32_t)(rnd32() >> (rnd32() % 32));
    default: return rnd32() >> (rnd32() % 32);         /* every magnitude */
    }
}

static int same(u128 a, u128 b) { return !memcmp(a._u32, b._u32, 16); }

static long s_bad;
static void report(const char* op, int imm, u128 a, u128 b, u128 got, u128 want)
{
    if (s_bad++ < 20)
        printf("MISMATCH %s imm=%d a=%08X %08X %08X %08X b=%08X %08X %08X %08X\n"
               "  got  %08X %08X %08X %08X\n  want %08X %08X %08X %08X\n", op, imm,
               a._u32[0], a._u32[1], a._u32[2], a._u32[3], b._u32[0], b._u32[1], b._u32[2], b._u32[3],
               got._u32[0], got._u32[1], got._u32[2], got._u32[3],
               want._u32[0], want._u32[1], want._u32[2], want._u32[3]);
}

#define CHECK1(op, a)        do { u128 got_ = spu_##op(a), want_ = spu_##op##_ref(a); \
                                  if (!same(got_, want_)) report(#op, 0, a, a, got_, want_); } while (0)
#define CHECK2(op, a, b)     do { u128 got_ = spu_##op(a, b), want_ = spu_##op##_ref(a, b); \
                                  if (!same(got_, want_)) report(#op, 0, a, b, got_, want_); } while (0)
#define CHECKI(op, a, i)     do { u128 got_ = spu_##op(a, i), want_ = spu_##op##_ref(a, i); \
                                  if (!same(got_, want_)) report(#op, i, a, a, got_, want_); } while (0)

int main(void)
{
#if !(defined(__AVX2__) && !defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS))
    puts("test_spu_float_est: no AVX2, nothing to compare");
    return 0;
#else
    const long N = 2000000;
    for (long it = 0; it < N; it++) {
        u128 f, g, n;
        for (int k = 0; k < 4; k++) { f._u32[k] = rnd_float(); g._u32[k] = rnd_float(); n._u32[k] = rnd_int(); }
        CHECK1(frest, f);
        CHECK1(frsqest, f);
        CHECK2(fi, g, f);
        CHECK2(fi, g, spu_frest_ref(f));               /* the frest -> fi pair as code uses it */
        CHECK2(fi, g, spu_frsqest_ref(f));
        const int imm = (int)(rnd32() & 0xFF);
        CHECKI(cflts, f, imm);
        CHECKI(cfltu, f, imm);
        CHECKI(csflt, n, imm);
        CHECKI(cuflt, n, imm);
    }
    /* every immediate on a fixed spread of operands */
    for (int imm = 0; imm < 256; imm++)
        for (int it = 0; it < 2000; it++) {
            u128 f, n;
            for (int k = 0; k < 4; k++) { f._u32[k] = rnd_float(); n._u32[k] = rnd_int(); }
            CHECKI(cflts, f, imm); CHECKI(cfltu, f, imm); CHECKI(csflt, n, imm); CHECKI(cuflt, n, imm);
        }
    printf("test_spu_float_est: %ld mismatches\n", s_bad);

    /* speed: the frest/fi reciprocal pair, vector vs scalar */
    u128 x, y; for (int k = 0; k < 4; k++) { x._u32[k] = 0x3F800000u + rnd32() % 0x800000u; y = x; }
    volatile uint32_t sink = 0;
    const long R = 20000000;
    clock_t t0 = clock();
    for (long i = 0; i < R; i++) { x = spu_fi(y, spu_frest(x)); x._u32[0] ^= (uint32_t)i & 1u; }
    sink += x._u32[0];
    clock_t t1 = clock();
    for (long i = 0; i < R; i++) { x = spu_fi_ref(y, spu_frest_ref(x)); x._u32[0] ^= (uint32_t)i & 1u; }
    sink += x._u32[0];
    clock_t t2 = clock();
    printf("frest+fi: vector %.1f ns, scalar %.1f ns\n",
           1e9 * (double)(t1 - t0) / CLOCKS_PER_SEC / R, 1e9 * (double)(t2 - t1) / CLOCKS_PER_SEC / R);
    (void)sink;
    return s_bad ? 1 : 0;
#endif
}
