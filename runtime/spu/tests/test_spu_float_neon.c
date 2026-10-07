/* The NEON spu_fa/fs/fm/fma/fms/fnms must be bit-identical to the scalar
 * references (spu_*_ref) for every input: random words, ordinary floats, the
 * normal/extended-range boundaries, zeros and denormals, mixed per lane.
 * Prints a rough speed comparison too. Build on arm64:
 *   cc -O2 -I.. -I../.. -I../../.. test_spu_float_neon.c -o t && ./t */
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

int main(void)
{
#if !defined(__ARM_NEON)
    puts("test_spu_float_neon: no NEON, nothing to compare");
    return 0;
#else
    long bad = 0;
    const long N = 4000000;
    for (long it = 0; it < N; it++) {
        u128 a, b, c, got, ref;
        for (int i = 0; i < 4; i++) { a._u32[i] = rnd_float(); b._u32[i] = rnd_float(); c._u32[i] = rnd_float(); }
        /* close magnitudes stress cancellation in fa/fs and fma */
        if (it % 7 == 0) for (int i = 0; i < 4; i++) b._u32[i] = (a._u32[i] ^ 0x80000000u) + (rnd32() & 0xFF);
        const int op = (int)(it % 6);
        switch (op) {
        case 0: got = spu_fa(a, b);      ref = spu_fa_ref(a, b); break;
        case 1: got = spu_fs(a, b);      ref = spu_fs_ref(a, b); break;
        case 2: got = spu_fm(a, b);      ref = spu_fm_ref(a, b); break;
        case 3: got = spu_fma(a, b, c);  ref = spu_fma_ref(a, b, c); break;
        case 4: got = spu_fms(a, b, c);  ref = spu_fms_ref(a, b, c); break;
        default: got = spu_fnms(a, b, c); ref = spu_fnms_ref(a, b, c); break;
        }
        if (memcmp(&got, &ref, 16)) {
            if (bad++ < 10)
                printf("MISMATCH op=%d a=%08X %08X %08X %08X b=%08X %08X %08X %08X c=%08X %08X %08X %08X\n"
                       "  got=%08X %08X %08X %08X ref=%08X %08X %08X %08X\n", op,
                       a._u32[0], a._u32[1], a._u32[2], a._u32[3], b._u32[0], b._u32[1], b._u32[2], b._u32[3],
                       c._u32[0], c._u32[1], c._u32[2], c._u32[3],
                       got._u32[0], got._u32[1], got._u32[2], got._u32[3],
                       ref._u32[0], ref._u32[1], ref._u32[2], ref._u32[3]);
        }
    }
    if (bad) { printf("test_spu_float_neon: %ld mismatches in %ld cases\n", bad, N); return 1; }
    printf("test_spu_float_neon: ok (%ld random cases)\n", N);

    /* fma's fast path trusts a double sum unless it was rounded (TwoSum error != 0) AND landed
     * on a 24-bit boundary. Random operands almost never reach either side of that test, so aim
     * at it: products of short (dyadic) significands, plus an addend that is either dyadic too
     * (exact sums: must stay correct on the fast path) or a lone power of two far below the
     * product (the sum rounds back onto the product -- a boundary -- and must go exact:
     * 1*1 - 2^-60 truncates to 0x3F7FFFFF, not 1.0). */
    {
        long hits = 0, exact_sums = 0;
        const long M = 2000000;
        for (long it = 0; it < M; it++) {
            u128 a, b, c, got, ref;
            for (int i = 0; i < 4; i++) {
                const uint32_t ea = 115 + rnd32() % 24, eb = 115 + rnd32() % 24;
                a._u32[i] = ((rnd32() & 1u) << 31) | (ea << 23) | ((rnd32() & 7u) << 20);
                b._u32[i] = ((rnd32() & 1u) << 31) | (eb << 23) | ((rnd32() & 7u) << 20);
                const int ep = (int)ea + (int)eb - 127;                /* product's biased exponent */
                int ec;
                if (rnd32() & 1) { ec = ep - (int)(rnd32() % 24); c._u32[i] = (rnd32() & 7u) << 20; }
                else             { ec = ep - 25 - (int)(rnd32() % 70); c._u32[i] = 0; }
                if (ec < 1) ec = 1;
                if (ec > 254) ec = 254;
                c._u32[i] |= ((rnd32() & 1u) << 31) | ((uint32_t)ec << 23);
                const double pd = (double)spu__sf_f(a._u32[i]) * (double)spu__sf_f(b._u32[i]);
                const double zd = (double)spu__sf_f(c._u32[i]), sd = pd + zd, bb = sd - pd;
                const double er = (pd - (sd - bb)) + (zd - bb);
                uint64_t su; memcpy(&su, &sd, 8);
                hits += er != 0.0 && !(su & 0x1FFFFFFFull);
                exact_sums += er == 0.0;
            }
            const int op = (int)(it % 3);
            if (op == 0)      { got = spu_fma(a, b, c);  ref = spu_fma_ref(a, b, c); }
            else if (op == 1) { got = spu_fms(a, b, c);  ref = spu_fms_ref(a, b, c); }
            else              { got = spu_fnms(a, b, c); ref = spu_fnms_ref(a, b, c); }
            if (memcmp(&got, &ref, 16)) {
                if (bad++ < 10)
                    printf("MISMATCH (boundary) op=%d a=%08X b=%08X c=%08X got=%08X ref=%08X\n", op,
                           a._u32[0], b._u32[0], c._u32[0], got._u32[0], ref._u32[0]);
            }
        }
        u128 one, tiny; memset(&one, 0, 16); memset(&tiny, 0, 16);
        one._u32[0] = 0x3F800000u; tiny._u32[0] = 0x80000000u | ((127u - 60u) << 23);   /* -2^-60 */
        const u128 r = spu_fma(one, one, tiny);
        if (r._u32[0] != 0x3F7FFFFFu) { printf("fma(1, 1, -2^-60) = %08X, want 3F7FFFFF\n", r._u32[0]); bad++; }
        if (bad || !hits || !exact_sums) {
            printf("test_spu_float_neon: boundary pass: %ld mismatches, %ld rounded-onto-boundary lanes, "
                   "%ld exact sums\n", bad, hits, exact_sums);
            return 1;
        }
        printf("test_spu_float_neon: ok (%ld boundary cases: %ld rounded onto a boundary, %ld exact sums)\n",
               M, hits, exact_sums);
    }

    /* Zero lanes are handled on the vector path (T-0014), so aim at them: every operand lane is
     * drawn from zeros of every kind (+0, -0, and exponent-0 words with a nonzero fraction, which
     * the SPU reads as zero), extended-range values, and ordinary values, so all mixes of zero
     * operand position, sign and neighbour occur. */
    {
        static const uint32_t zeros[] = { 0x00000000u, 0x80000000u, 0x00000001u, 0x007FFFFFu, 0x807FFFFFu, 0x80400000u };
        long zcases = 0;
        const long M = 2000000;
        for (long it = 0; it < M; it++) {
            u128 a, b, c, got, ref;
            for (int i = 0; i < 4; i++) {
                uint32_t* w[3] = { &a._u32[i], &b._u32[i], &c._u32[i] };
                for (int k = 0; k < 3; k++) {
                    const uint32_t r = rnd32() % 8;
                    *w[k] = r < 3 ? zeros[rnd32() % 6]
                          : r == 3 ? ((rnd32() & 1u) << 31) | (255u << 23) | (rnd32() & 0x7FFFFFu)
                          : rnd_float();
                }
                zcases += !((a._u32[i] >> 23) & 0xFF) || !((b._u32[i] >> 23) & 0xFF) || !((c._u32[i] >> 23) & 0xFF);
            }
            const int op = (int)(it % 6);
            switch (op) {
            case 0: got = spu_fa(a, b);      ref = spu_fa_ref(a, b); break;
            case 1: got = spu_fs(a, b);      ref = spu_fs_ref(a, b); break;
            case 2: got = spu_fm(a, b);      ref = spu_fm_ref(a, b); break;
            case 3: got = spu_fma(a, b, c);  ref = spu_fma_ref(a, b, c); break;
            case 4: got = spu_fms(a, b, c);  ref = spu_fms_ref(a, b, c); break;
            default: got = spu_fnms(a, b, c); ref = spu_fnms_ref(a, b, c); break;
            }
            if (memcmp(&got, &ref, 16)) {
                if (bad++ < 10)
                    printf("MISMATCH (zero) op=%d a=%08X %08X %08X %08X b=%08X %08X %08X %08X c=%08X %08X %08X %08X\n"
                           "  got=%08X %08X %08X %08X ref=%08X %08X %08X %08X\n", op,
                           a._u32[0], a._u32[1], a._u32[2], a._u32[3], b._u32[0], b._u32[1], b._u32[2], b._u32[3],
                           c._u32[0], c._u32[1], c._u32[2], c._u32[3],
                           got._u32[0], got._u32[1], got._u32[2], got._u32[3],
                           ref._u32[0], ref._u32[1], ref._u32[2], ref._u32[3]);
            }
        }
        if (bad) { printf("test_spu_float_neon: %ld mismatches in the zero-lane pass\n", bad); return 1; }
        printf("test_spu_float_neon: ok (%ld zero-lane cases, %ld lanes with a zero operand)\n", M, zcases);
    }

    /* The NEON paths leave the thread in round-toward-zero; double precision
     * must still round to nearest: 1 + (2^-53 + 2^-60) -> 1 + 2^-52. */
    {
        u128 x, y; memset(&x, 0, 16); memset(&y, 0, 16);
        x._u32[0] = 0x3FF00000u; y._u32[0] = 0x3CA02000u;          /* 1.0, 2^-53 + 2^-60 */
        (void)spu_fa(x, y);                                          /* sets RZ */
        u128 d = spu_dfa(x, y);
        if (d._u32[0] != 0x3FF00000u || d._u32[1] != 1u) {
            printf("test_spu_float_neon: dfa under RZ gave %08X%08X, want 3FF0000000000001\n", d._u32[0], d._u32[1]);
            return 1;
        }
    }

    /* speed on ordinary operands that stay ordinary */
    static u128 in[256][3];
    for (int k = 0; k < 256; k++)
        for (int i = 0; i < 4; i++) {
            in[k][0]._u32[i] = (rnd32() & 0x807FFFFFu) | ((120u + rnd32() % 16) << 23);
            in[k][1]._u32[i] = (rnd32() & 0x807FFFFFu) | ((120u + rnd32() % 16) << 23);
            in[k][2]._u32[i] = (rnd32() & 0x807FFFFFu) | ((120u + rnd32() % 16) << 23);
        }
    const int R = 20000000;
    uint32_t acc = 0;
    clock_t t0 = clock();
    for (int i = 0; i < R; i++) { const u128* q = in[i & 255];
        u128 r1 = spu_fma(q[0], q[1], q[2]), r2 = spu_fm(q[0], q[1]); acc += r1._u32[i & 3] ^ r2._u32[(i + 1) & 3]; }
    clock_t t1 = clock();
    for (int i = 0; i < R; i++) { const u128* q = in[i & 255];
        u128 r1 = spu_fma_ref(q[0], q[1], q[2]), r2 = spu_fm_ref(q[0], q[1]); acc += r1._u32[i & 3] ^ r2._u32[(i + 1) & 3]; }
    clock_t t2 = clock();
    printf("fma+fm pair: neon %.1f ns, scalar %.1f ns (acc %u)\n",
           (t1 - t0) * 1e9 / CLOCKS_PER_SEC / R, (t2 - t1) * 1e9 / CLOCKS_PER_SEC / R, acc);
    return 0;
#endif
}
