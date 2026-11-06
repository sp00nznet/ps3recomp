/* The vector single-precision ops this build selects (spu_fa/fs/fm/fma/fms/fnms: the SSE4.1, AVX2
 * or NEON form) against the scalar per-lane forms (spu__*_s: scalar fast path, else exact), lane
 * by lane and bit for bit. test_spu_float_exact compares both against the exact references; this
 * one is the direct differential check between the two code paths a build can take.
 * Lanes are drawn from SPU edge words (+-0, words with biased exponent 0 and a fraction, the
 * smallest normal, the largest IEEE-finite word, extended-range words of biased exponent 255,
 * Smax), random normal values in a narrow and a wide exponent window, values whose product or
 * sum lands near 2^-126 or 2^128, fma addends close to -x*y, and raw random words; each vector
 * mixes generators per lane so that a vector with one odd lane still takes the fix-up path.
 * Build: cc -O2 -msse4.1 -ffp-contract=off -I runtime/spu -I include THIS -lm -o t && ./t [millions] */
#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>

static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static inline uint32_t rnd32(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return (uint32_t)(s_rng >> 16);
}
static const uint32_t k_edge[] = {
    0x00000000u, 0x80000000u, 0x00000001u, 0x807FFFFFu, 0x00400000u, 0x00800000u, 0x80800000u,
    0x00800001u, 0x3F800000u, 0xBF800000u, 0x3F7FFFFFu, 0x3F800001u, 0x7F7FFFFFu, 0xFF7FFFFFu,
    0x7F000000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7F800001u, 0x7FFFFFFFu, 0xFFFFFFFFu,
    0x01000000u, 0x00FFFFFFu, 0x4B000000u, 0x4B7FFFFFu, 0x33800000u, 0x34000000u, 0x5F800000u,
    0x1F800000u, 0x20000000u, 0x60000000u,
};
#define NEDGE (sizeof k_edge / sizeof k_edge[0])
static uint32_t word(uint32_t z)
{
    const uint32_t sign = rnd32() & 0x80000000u;
    switch (rnd32() % 9) {
    case 0: return k_edge[rnd32() % NEDGE] ^ (rnd32() & 0x80000000u);
    case 1: return sign | ((100u + rnd32() % 56u) << 23) | (rnd32() & 0x7FFFFFu);      /* narrow window */
    case 2: return sign | ((1u + rnd32() % 254u) << 23) | (rnd32() & 0x7FFFFFu);       /* any normal */
    case 3: return sign | ((rnd32() % 2u ? 255u : 0u) << 23) | (rnd32() & 0x7FFFFFu);  /* exp 0 / 255 */
    case 4: return sign | ((1u + rnd32() % 4u) << 23) | (rnd32() & 0x7FFFFFu);         /* near 2^-126 */
    case 5: return sign | ((250u + rnd32() % 6u) << 23) | (rnd32() & 0x7FFFFFu);       /* near 2^128 */
    case 6: return z ? (z ^ 0x80000000u) + (rnd32() % 5u) - 2u : rnd32();               /* cancellation */
    case 7: return sign | ((63u + rnd32() % 2u) << 23) | (rnd32() & 0x7FFFFFu);        /* products near 2^-126 */
    default: return rnd32();
    }
}
static long s_bad, s_lanes;
static const char* const k_ops[6] = { "fa", "fs", "fm", "fma", "fms", "fnms" };
static void check(int op, u128 a, u128 b, u128 c)
{
    u128 got, ref;
    switch (op) {
    case 0:  got = spu_fa(a, b);      ref = spu__fa_s(a, b); break;
    case 1:  got = spu_fs(a, b);      ref = spu__fs_s(a, b); break;
    case 2:  got = spu_fm(a, b);      ref = spu__fm_s(a, b); break;
    case 3:  got = spu_fma(a, b, c);  ref = spu__fma_s(a, b, c); break;
    case 4:  got = spu_fms(a, b, c);  ref = spu__fms_s(a, b, c); break;
    default: got = spu_fnms(a, b, c); ref = spu__fnms_s(a, b, c); break;
    }
    s_lanes += 4;
    for (int i = 0; i < 4; i++)
        if (got._u32[i] != ref._u32[i] && s_bad++ < 20)
            printf("MISMATCH %s a=%08X b=%08X c=%08X vector=%08X scalar=%08X\n", k_ops[op],
                   a._u32[i], b._u32[i], c._u32[i], got._u32[i], ref._u32[i]);
}
int main(int argc, char** argv)
{
    const long millions = argc > 1 ? atol(argv[1]) : 16;
    /* every pair of edge words in every lane position */
    for (unsigned i = 0; i < NEDGE; i++) for (unsigned j = 0; j < NEDGE; j++) for (unsigned k = 0; k < NEDGE; k++) {
        u128 a, b, c;
        for (int l = 0; l < 4; l++) {
            a._u32[l] = k_edge[(i + l) % NEDGE]; b._u32[l] = k_edge[(j + 3 * l) % NEDGE]; c._u32[l] = k_edge[(k + 5 * l) % NEDGE];
        }
        for (int op = 0; op < 6; op++) check(op, a, b, c);
    }
    for (long n = 0; n < millions * 1000000L; n++) {
        u128 a, b, c;
        for (int l = 0; l < 4; l++) {
            a._u32[l] = word(0); b._u32[l] = word(0);
            /* for fma, an addend close to -a*b in the cancellation generator */
            const u128 one = { ._u32 = { a._u32[l], 0, 0, 0 } }, two = { ._u32 = { b._u32[l], 0, 0, 0 } };
            c._u32[l] = word(spu__fm_s(one, two)._u32[0]);
        }
        check((int)(n % 6), a, b, c);
    }
    printf("test_spu_float_vector_scalar: %ld lanes, %ld mismatches\n", s_lanes, s_bad);
    return s_bad != 0;
}
