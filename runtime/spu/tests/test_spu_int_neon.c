/* The NEON integer/permute helpers must be bit-identical to their scalar
 * references (spu_*_ref) for random operands and every shift amount. Build on
 * arm64:  cc -O2 -I.. -I../.. -I../../.. test_spu_int_neon.c -o t && ./t */
#include "../spu_helpers.h"
#include <stdio.h>

static uint64_t s_rng = 0x2545F4914F6CDD1Dull;
static uint32_t rnd32(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return (uint32_t)(s_rng >> 16);
}
static u128 rnd128(void) { u128 r; for (int i = 0; i < 4; i++) r._u32[i] = rnd32(); return r; }

static long s_bad;
static void check(const char* op, u128 got, u128 ref, u128 a, u128 b, int imm)
{
    if (!memcmp(&got, &ref, 16)) return;
    if (s_bad++ < 12)
        printf("MISMATCH %s a=%08X%08X%08X%08X b=%08X%08X%08X%08X imm=%d got=%08X%08X%08X%08X ref=%08X%08X%08X%08X\n",
               op, a._u32[0], a._u32[1], a._u32[2], a._u32[3], b._u32[0], b._u32[1], b._u32[2], b._u32[3], imm,
               got._u32[0], got._u32[1], got._u32[2], got._u32[3], ref._u32[0], ref._u32[1], ref._u32[2], ref._u32[3]);
}

int main(void)
{
#if !defined(__ARM_NEON)
    puts("test_spu_int_neon: no NEON, nothing to compare");
    return 0;
#else
    const long N = 200000;
    for (long it = 0; it < N; it++) {
        u128 a = rnd128(), b = rnd128(), c = rnd128();
        if (it & 1) b._u32[0] = rnd32() & 0xFF;          /* small shift counts */
        if (it % 5 == 0) b = a;                           /* equal operands for compares */
        int imm = (int)(rnd32() & 0x3FF) - 0x200;         /* 10-bit signed immediate */
        if (it % 3 == 0) imm = (int32_t)a._u32[rnd32() & 3];
        int sh = (int)(it % 64) - 16;
#define C2(op) check(#op, spu_##op(a, b), spu_##op##_ref(a, b), a, b, 0)
#define CI(op, v) check(#op, spu_##op(a, v), spu_##op##_ref(a, v), a, b, v)
        C2(a); C2(sf); C2(and); C2(or); C2(xor); C2(ceq); C2(cgt); C2(clgt);
        CI(ai, imm); CI(sfi, imm); CI(andi, imm); CI(ori, imm); CI(xori, imm);
        CI(ceqi, imm); CI(cgti, imm); CI(clgti, imm);
        check("selb", spu_selb(a, b, c), spu_selb_ref(a, b, c), a, b, 0);
        CI(shlqbyi, sh); CI(rotqbyi, sh); CI(rotqmbyi, sh); CI(cwd, sh);
        C2(shlqby); C2(rotqby); C2(shlqbybi); C2(rotqbybi); C2(rotqmby); C2(rotqmbybi);
    }
    if (s_bad) { printf("test_spu_int_neon: %ld mismatches\n", s_bad); return 1; }
    printf("test_spu_int_neon: ok (%ld random cases x 27 ops)\n", N);

    /* The per-lane ops vectorised for tests/bench/spu. Shift and rotate counts come per lane,
     * so b is drawn three ways: random bits, a small count (0..80, past every lane width) in
     * every lane, and equal to a (the compares' equality edge). Immediates sweep all 10-bit
     * values and the 7-bit shift range including negatives. */
    for (long it = 0; it < 4 * N; it++) {
        u128 a = rnd128(), b = rnd128(), t = rnd128();
        const int kind = (int)(it % 4);
        if (kind == 1) for (int i = 0; i < 16; i++) b._u8[i] = (uint8_t)(rnd32() % 81);
        if (kind == 2) for (int i = 0; i < 8; i++) b._u16[i] = (uint16_t)(0u - rnd32() % 81);   /* rotm-style negated */
        if (kind == 3) b = a;
        if (it % 7 == 0) for (int i = 0; i < 16; i++) a._u8[i] = (uint8_t)(rnd32() % 3 ? a._u8[i] : 0x80 + (rnd32() & 1) - 1);
        const int imm = (int)(it % 1024) - 512;
        const int i7 = (int)(it % 192) - 64;
#define C3(op) check(#op, spu_##op(a, b, t), spu_##op##_ref(a, b, t), a, b, 0)
#define C1(op) check(#op, spu_##op(a), spu_##op##_ref(a), a, b, 0)
        C2(absdb); C2(avgb); C2(ceqb); C2(cgtb); C2(clgtb); C2(ceqh); C2(cgth); C2(clgth); C2(ah); C2(sfh);
        C2(shlh); C2(roth); C2(rothm); C2(rothma); C2(shl); C2(rot); C2(rotm); C2(rotma); C2(cg); C2(bg);
        C2(mpy); C2(mpyu); C2(mpyh); C2(mpyhh); C2(mpyhhu); C2(mpys); C2(sumb);
        CI(ceqbi, imm); CI(cgtbi, imm); CI(clgtbi, imm); CI(ceqhi, imm); CI(cgthi, imm); CI(clgthi, imm);
        CI(ahi, imm); CI(sfhi, imm); CI(mpyi, imm); CI(mpyui, imm);
        CI(ceqbi, (int32_t)a._u8[3]); CI(ceqhi, (int32_t)a._s16[1]); CI(cgthi, (int32_t)a._s16[1]);
        CI(rothi, i7); CI(roti, i7); CI(rotmahi, i7); CI(rotmai, i7);
        C3(mpya); C3(mpyhha); C3(mpyhhau); C3(addx); C3(sfx); C3(cgx); C3(bgx);
        if (it & 1) { t = spu_splat_u32(1); C3(cgx); C3(bgx); C3(addx); C3(sfx); }
        C1(cntb); C1(xsbh); C1(xshw); C1(clz); C1(gb); C1(gbh); C1(gbb); C1(fsm); C1(fsmh); C1(fsmb);
        C1(orx); C1(xswd);
        { u128 z = spu_zero(); C1(clz); a = z; C1(clz); C1(orx); }
    }
    if (s_bad) { printf("test_spu_int_neon: %ld mismatches (vectorised per-lane ops)\n", s_bad); return 1; }
    printf("test_spu_int_neon: ok (%ld cases x 60 vectorised per-lane ops)\n", 4 * N);
    return 0;
#endif
}
