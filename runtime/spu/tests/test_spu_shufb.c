/* spu_shufb (pshufb / NEON tbl path) must match the byte-loop reference for
 * every selector class: a/b sources, 0x00 / 0xFF / 0x80 specials. Build with
 * -msse4.1 on x86 so the SIMD version is the one under test (NEON is always
 * on for arm64).
 *
 * Two parts:
 *  1. Brute force over the selector. A result byte depends on its own selector
 *     byte and on the two sources; the SSE path also moves selector bits with
 *     16-bit shifts, so a byte's neighbour inside the same 16-bit lane is the
 *     only other selector byte that can reach it. Hence: for each of the 8
 *     lanes, all 65536 values of its two selector bytes, with the other 14
 *     selector bytes at three backgrounds (all 0x00, all 0xFF, a mixed
 *     pattern), on three source pairs: 32 distinct bytes that are none of the
 *     special results, the complement of that, and one where a and b hold the
 *     special results themselves (0x00 / 0xFF / 0x80).
 *     8 * 65536 * 3 * 3 = 4718592 cases.
 *  2. 200000 random cases (all 16 selector bytes and both sources random). */
#include "../spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>

static int check(const u128* a, const u128* b, const u128* c, const char* what, long n)
{
    const u128 x = spu_shufb(*a, *b, *c), y = spu_shufb_ref(*a, *b, *c);
    if (!memcmp(&x, &y, 16)) return 0;
    printf("test_spu_shufb: MISMATCH (%s case %ld)\n  c   =", what, n);
    for (int i = 0; i < 16; i++) printf(" %02X", c->_u8[i]);
    printf("\n  simd=");
    for (int i = 0; i < 16; i++) printf(" %02X", x._u8[i]);
    printf("\n  ref =");
    for (int i = 0; i < 16; i++) printf(" %02X", y._u8[i]);
    printf("\n");
    return 1;
}

int main(void)
{
#if !defined(__SSE4_1__) && !defined(__ARM_NEON)
    puts("test_spu_shufb: built without SSE4.1 or NEON, nothing to compare");
    return 0;
#else
    /* part 1: brute force over each 16-bit lane's two selector bytes */
    u128 src[3][2];
    for (int i = 0; i < 16; i++) {
        src[0][0]._u8[i] = (uint8_t)(0x11 + i);          /* 0x11..0x20 */
        src[0][1]._u8[i] = (uint8_t)(0x31 + i);          /* 0x31..0x40 */
        src[1][0]._u8[i] = (uint8_t)~src[0][0]._u8[i];
        src[1][1]._u8[i] = (uint8_t)~src[0][1]._u8[i];
        src[2][0]._u8[i] = (uint8_t)(i % 3 == 0 ? 0x00 : i % 3 == 1 ? 0xFF : 0x80);
        src[2][1]._u8[i] = (uint8_t)(i % 3 == 0 ? 0x80 : i % 3 == 1 ? 0x00 : 0xFF);
    }
    long n = 0;
    for (int lane = 0; lane < 8; lane++)
        for (int bg = 0; bg < 3; bg++) {
            u128 c;
            for (int i = 0; i < 16; i++)
                c._u8[i] = (uint8_t)(bg == 0 ? 0x00 : bg == 1 ? 0xFF : (0x1F + 0x35 * i));
            for (int v = 0; v < 65536; v++) {
                c._u8[2 * lane] = (uint8_t)v; c._u8[2 * lane + 1] = (uint8_t)(v >> 8);
                for (int s = 0; s < 3; s++) {
                    if (check(&src[s][0], &src[s][1], &c, "selector pair", n)) return 1;
                    n++;
                }
            }
        }
    /* part 2: random */
    srand(12345);
    for (int it = 0; it < 200000; it++) {
        u128 a, b, c;
        for (int i = 0; i < 16; i++) {
            a._u8[i] = (uint8_t)rand(); b._u8[i] = (uint8_t)rand();
            c._u8[i] = (uint8_t)rand();          /* covers all 256 selector values */
        }
        if (check(&a, &b, &c, "random", it)) return 1;
    }
    printf("test_spu_shufb: ok (%ld selector-pair cases, 200000 random cases)\n", n);
    return 0;
#endif
}
