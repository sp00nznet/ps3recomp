/*
 * The NEON spu_fi and spu_cflts against their scalar references
 * (spu_fi_ref, spu_cflts_ref in spu_helpers.h), lane for lane.
 *
 * Inputs: every exponent and sign with random fractions, the borrow edge of
 * fi (step * y around base << 19), zero and exponent-255 lanes, and every
 * cflts scale. Run once in the default FPCR and once with round-toward-zero
 * plus flush-to-zero, the mode SPU_FLOAT_RZ runs lifted code in.
 *
 * Build:
 *   clang -std=gnu17 -O2 -I include -I runtime/spu \
 *         -o /tmp/test_spu_fi_cflts_neon runtime/spu/tests/test_spu_fi_cflts_neon.c
 */
#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>

static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return (uint32_t)s_rng; }

static unsigned long long g_n, g_bad;
static void cmp(const char* what, u128 got, u128 want, u128 a, u128 b, int i8)
{
    g_n++;
    if (!memcmp(&got, &want, 16)) return;
    if (g_bad++ < 10)
        fprintf(stderr, "FAIL %s i8=%d a=%08X b=%08X got=%08X want=%08X\n", what, i8,
                a._u32[0], b._u32[0], got._u32[0], want._u32[0]);
}

static uint32_t edge_float(void)
{
    uint32_t r = rnd();
    switch (r & 7) {
    case 0: return r & 0x807FFFFFu;                       /* exponent 0 */
    case 1: return r | 0x7F800000u;                       /* exponent 255 */
    default: return r;
    }
}

static void run(void)
{
    for (int round = 0; round < 400000; ++round) {
        u128 a, b;
        for (int i = 0; i < 4; ++i) { a._u32[i] = edge_float(); b._u32[i] = edge_float(); }
        /* fi near its borrow edge: step * y close to base << 19 */
        if (round & 1) for (int i = 0; i < 4; ++i) {
            uint32_t base = rnd() & 0x1FFFu, step = (rnd() & 0x3FFu) | 1u;
            uint32_t y = (uint32_t)(((uint64_t)base << 19) / step) + (rnd() % 5) - 2;
            b._u32[i] = (b._u32[i] & 0xFF800000u) | (base << 10) | step;
            a._u32[i] = (a._u32[i] & ~0x7FFFFu) | (y & 0x7FFFFu);
        }
        cmp("fi", spu_fi(a, b), spu_fi_ref(a, b), a, b, 0);
        const int i8 = (int)(rnd() & 0xFF);
        cmp("cflts", spu_cflts(a, i8), spu_cflts_ref(a, i8), a, b, i8);
        for (int s = 46; s <= 173; s += 31) cmp("cflts", spu_cflts(b, s), spu_cflts_ref(b, s), b, a, s);
    }
}

int main(void)
{
#if defined(__aarch64__)
    run();
    uint64_t fpcr; __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    __asm__ volatile("msr fpcr, %0" :: "r"(fpcr | (3ull << 22) | (1ull << 24)));   /* RZ + FZ */
    run();
    __asm__ volatile("msr fpcr, %0" :: "r"(fpcr));
#else
    run();
#endif
    printf("spu_fi/spu_cflts NEON vs reference: %llu vectors, %llu mismatched\n", g_n, g_bad);
    return g_bad != 0;
}
