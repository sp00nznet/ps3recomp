/* x86 (SSE4.1/AVX2) SPU helpers vs their scalar spu_X_ref, on random and edge inputs.
 *
 *   clang -O2 -mavx2 -Iruntime/spu -Iruntime -Iinclude \
 *         runtime/spu/tests/test_spu_int_x86.c -o test_spu_int_x86 && ./test_spu_int_x86
 *
 * With -msse4.1 in place of -mavx2 it checks the SSE4.1-only set (the variable shifts are
 * then their own references).
 *
 * The generate-controls (spu_cbd_pos & co.) lost their byte loops on every host; they are
 * checked here against those loops, kept below as the reference. */
#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>

#if !SPU_X86
#error "build with -msse4.1 or -mavx2 (SPU_X86 off)"
#endif

static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return (uint32_t)s_rng; }
/* Mostly random words, with the small and boundary values shifts and compares care about. */
static uint32_t rword(void) {
    static const uint32_t edge[] = {0, 1, 2, 3, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 0x7F, 0x80, 0xFF,
                                    0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000u, 0xFFFFFFFFu,
                                    0xFFFFFFF0u, 0xFFFFFFE0u, 0x00800080u, 0x80008000u, 0x7F7F7F7Fu};
    switch (rnd() & 3) {
    case 0: return edge[rnd() % (sizeof edge / sizeof edge[0])];
    case 1: return rnd() & 0x3F;
    default: return rnd();
    }
}
static u128 rv(void) { u128 r; for (int i = 0; i < 4; i++) r._u32[i] = rword(); return r; }
static int s_fail;
static void cmp(const char* name, u128 got, u128 want, u128 a, u128 b, u128 c, int imm) {
    if (memcmp(&got, &want, 16) == 0) return;
    if (s_fail++ < 20)
        printf("FAIL %s imm=%d a=%08X%08X%08X%08X b=%08X%08X%08X%08X c=%08X..\n  got  %08X %08X %08X %08X\n  want %08X %08X %08X %08X\n",
               name, imm, a._u32[0], a._u32[1], a._u32[2], a._u32[3], b._u32[0], b._u32[1], b._u32[2], b._u32[3], c._u32[0],
               got._u32[0], got._u32[1], got._u32[2], got._u32[3], want._u32[0], want._u32[1], want._u32[2], want._u32[3]);
}

/* The loops the generate-controls had. */
static u128 ref_cbd_pos(int pos) { u128 r; for (int t = 0; t < 16; t++) r._u8[SPU_W(t)] = (uint8_t)(0x10 + t);
    r._u8[SPU_W(pos & 0xF)] = 0x03; return r; }
static u128 ref_chd_pos(int pos) { u128 r; for (int t = 0; t < 16; t++) r._u8[SPU_W(t)] = (uint8_t)(0x10 + t);
    int p = (pos & 0xF) & ~1; r._u8[SPU_W(p)] = 0x02; r._u8[SPU_W(p + 1)] = 0x03; return r; }
static u128 ref_cwd_pos(int pos) { u128 r; for (int t = 0; t < 16; t++) r._u8[SPU_W(t)] = (uint8_t)(0x10 + t);
    int p = (pos & 0xF) & ~3; for (int k = 0; k < 4; k++) r._u8[SPU_W(p + k)] = (uint8_t)k; return r; }
static u128 ref_cdd_pos(int pos) { u128 r; for (int t = 0; t < 16; t++) r._u8[SPU_W(t)] = (uint8_t)(0x10 + t);
    int p = (pos & 0xF) & ~7; for (int k = 0; k < 8; k++) r._u8[SPU_W(p + k)] = (uint8_t)k; return r; }

#define T2(f)  cmp(#f, f(a, b), f##_ref(a, b), a, b, z, 0)
#define T3(f)  cmp(#f, f(a, b, c), f##_ref(a, b, c), a, b, c, 0)
#define T1(f)  cmp(#f, f(a), f##_ref(a), a, z, z, 0)
#define TI(f)  cmp(#f, f(a, imm), f##_ref(a, imm), a, z, z, imm)

int main(void) {
    const u128 z = spu_zero();
    for (long n = 0; n < 2000000; n++) {
        const u128 a = rv(), b = rv(), c = rv();
        const int imm = (int)(rnd() % 3 == 0 ? (int32_t)rword() : (int32_t)(rnd() % 256) - 128);
        T2(spu_a); T2(spu_sf); T2(spu_and); T2(spu_or); T2(spu_xor);
        T2(spu_ceq); T2(spu_cgt); T2(spu_clgt); T3(spu_selb);
        TI(spu_ai); TI(spu_sfi); TI(spu_andi); TI(spu_ori); TI(spu_xori);
        TI(spu_ceqi); TI(spu_cgti); TI(spu_clgti);
        TI(spu_shlqbyi); TI(spu_rotqbyi); TI(spu_rotqmbyi);
        T2(spu_shlqby); T2(spu_rotqby); T2(spu_shlqbybi); T2(spu_rotqbybi); T2(spu_rotqmby); T2(spu_rotqmbybi);
        TI(spu_cwd);
        T2(spu_absdb); T2(spu_avgb); T2(spu_ceqb); T2(spu_cgtb); T2(spu_clgtb);
        T2(spu_ceqh); T2(spu_cgth); T2(spu_clgth); T2(spu_ah); T2(spu_sfh);
        T2(spu_shl); T2(spu_rot); T2(spu_rotm); T2(spu_rotma); T2(spu_cg); T2(spu_bg);
        T2(spu_mpy); T2(spu_mpyu); T2(spu_mpyh); T2(spu_mpyhh); T2(spu_mpyhhu); T2(spu_mpys);
        TI(spu_ceqbi); TI(spu_cgtbi); TI(spu_clgtbi); TI(spu_ceqhi); TI(spu_cgthi); TI(spu_clgthi);
        TI(spu_ahi); TI(spu_sfhi); TI(spu_mpyi); TI(spu_mpyui);
        TI(spu_rothi); TI(spu_roti); TI(spu_rotmahi); TI(spu_rotmai);
        T3(spu_mpya); T3(spu_mpyhha); T3(spu_mpyhhau); T3(spu_addx); T3(spu_sfx);
        T1(spu_cntb); T1(spu_xsbh); T1(spu_xshw);
        const int pos = (int)rnd();
        cmp("cbd_pos", spu_cbd_pos(pos), ref_cbd_pos(pos), z, z, z, pos);
        cmp("chd_pos", spu_chd_pos(pos), ref_chd_pos(pos), z, z, z, pos);
        cmp("cwd_pos", spu_cwd_pos(pos), ref_cwd_pos(pos), z, z, z, pos);
        cmp("cdd_pos", spu_cdd_pos(pos), ref_cdd_pos(pos), z, z, z, pos);
    }
    printf(s_fail ? "%d FAILURES\n" : "all x86 SPU helpers match their references\n", s_fail);
    return s_fail != 0;
}
