/*
 * Unit tests for SPU instruction-semantics helpers (runtime/spu/spu_helpers.h).
 *
 * Each test fixes a known input and asserts the bit-exact output. Vectors
 * are taken from the IBM Cell BE Architecture / SPU ISA reference; tricky
 * cases (selector specials in shufb, boundary shifts at 0 and >=width,
 * negative-direction rotates in rotmi/rotmai, insertion-mask generation)
 * have multiple vectors.
 *
 * Build:
 *   gcc -std=c11 -I../.. test_spu_helpers.c -o test_spu_helpers
 *
 * Exit code: 0 if all passed, 1 if any failed. Final line prints summary.
 */

#include "../spu_helpers.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;
static const char* g_current = "(none)";

static u128 make128(uint64_t hi, uint64_t lo) {
    u128 r; r._u64[0] = hi; r._u64[1] = lo; return r;
}

/* Construct from 16 explicit bytes — endianness-neutral (writes _u8[i]=b[i]).
 * Use this for any test that cares about specific byte layout (shufb, cwd,
 * fsmbi, byte permutations). make128 above interprets values according to
 * host endianness, which is fine only for lane-symmetric vectors. */
static u128 from_bytes(const uint8_t b[16]) {
    u128 r;
    for (int i = 0; i < 16; i++) r._u8[i] = b[i];
    return r;
}

static int u128_eq(u128 a, u128 b) {
    return a._u64[0] == b._u64[0] && a._u64[1] == b._u64[1];
}

static void print_u128(const char* label, u128 v) {
    fprintf(stderr, "  %-8s %016llX %016llX\n", label,
            (unsigned long long)v._u64[0],
            (unsigned long long)v._u64[1]);
}

#define EXPECT_EQ(actual, expected) do {                          \
    u128 _a = (actual), _e = (expected);                          \
    if (u128_eq(_a, _e)) { g_pass++; }                            \
    else {                                                        \
        g_fail++;                                                 \
        fprintf(stderr, "FAIL: %s\n", g_current);                 \
        print_u128("actual",   _a);                               \
        print_u128("expected", _e);                               \
    }                                                             \
} while (0)

#define TEST(name) g_current = name

/* ===========================================================================
 * Immediate loaders: broadcast semantics
 * ===========================================================================*/
static void test_immediate_loaders(void) {
    TEST("il splats sign-extended 16-bit to 4 words");
    EXPECT_EQ(spu_il(5),    make128(0x0000000500000005ULL, 0x0000000500000005ULL));
    EXPECT_EQ(spu_il(-1),   make128(0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL));
    EXPECT_EQ(spu_il(-2),   make128(0xFFFFFFFEFFFFFFFEULL, 0xFFFFFFFEFFFFFFFEULL));

    TEST("ilh splats 16-bit to 8 halfwords");
    EXPECT_EQ(spu_ilh(0xABCD), make128(0xABCDABCDABCDABCDULL, 0xABCDABCDABCDABCDULL));

    TEST("ilhu shifts immediate left 16 then splats to words");
    EXPECT_EQ(spu_ilhu(0x1234), make128(0x1234000012340000ULL, 0x1234000012340000ULL));

    TEST("iohl OR's halfword lower into each word");
    EXPECT_EQ(spu_iohl(spu_ilhu(0x1234), 0x5678),
              make128(0x1234567812345678ULL, 0x1234567812345678ULL));
}

/* ===========================================================================
 * Per-lane SIMD arithmetic
 * ===========================================================================*/
static void test_simd_arith(void) {
    TEST("a (add word) per-lane");
    u128 a4 = make128(0x0000000100000002ULL, 0x0000000300000004ULL);
    u128 b4 = make128(0x0000001000000020ULL, 0x0000003000000040ULL);
    EXPECT_EQ(spu_a(a4, b4),
              make128(0x0000001100000022ULL, 0x0000003300000044ULL));

    TEST("sf (subtract from word) does rb-ra, not ra-rb");
    EXPECT_EQ(spu_sf(spu_il(3), spu_il(7)), spu_il(4));   /* 7-3 = 4 */

    TEST("ah (add halfword) per-lane");
    EXPECT_EQ(spu_ah(spu_ilh(10), spu_ilh(20)), spu_ilh(30));

    TEST("ai (add immediate, sign-extended)");
    EXPECT_EQ(spu_ai(spu_il(100), -5), spu_il(95));

    TEST("cg (carry generate, per word) — sets 1 when overflow");
    u128 cg_in_a = spu_il((int32_t)0xFFFFFFFF);
    u128 cg_in_b = spu_il(1);
    EXPECT_EQ(spu_cg(cg_in_a, cg_in_b), spu_il(1));

    TEST("cg — no overflow returns 0");
    EXPECT_EQ(spu_cg(spu_il(1), spu_il(2)), spu_il(0));

    TEST("addx (add extended w/ carry-in from rt LSB)");
    u128 carry_one = spu_il(1);   /* rt._u32[i] & 1 = 1 */
    EXPECT_EQ(spu_addx(spu_il(10), spu_il(20), carry_one), spu_il(31));
    u128 carry_zero = spu_il(2);  /* LSB = 0 */
    EXPECT_EQ(spu_addx(spu_il(10), spu_il(20), carry_zero), spu_il(30));
}

/* ===========================================================================
 * Multiply family — these mix lanes; common to get wrong
 * ===========================================================================*/
static void test_multiply(void) {
    /* mpy: low halfword of each word × low halfword → 32-bit word. */
    TEST("mpy multiplies low halfwords of each word");
    /* word i = 0x00030007 -> low half = 7, high half = 3.
       For mpy: low * low = 7*5 = 35 per word. */
    u128 a = make128(0x0003000700030007ULL, 0x0003000700030007ULL);
    u128 b = make128(0x0002000500020005ULL, 0x0002000500020005ULL);
    EXPECT_EQ(spu_mpy(a, b), spu_il(35));

    TEST("mpyh multiplies high half of ra by low half of rb, then <<16");
    /* per word: (high_a * low_b) << 16 = (3 * 5) << 16 = 15 << 16 = 0xF0000 */
    EXPECT_EQ(spu_mpyh(a, b), spu_il(0xF0000));

    TEST("mpyhh multiplies high halves of both");
    /* high_a * high_b = 3 * 2 = 6 per word */
    EXPECT_EQ(spu_mpyhh(a, b), spu_il(6));

    TEST("mpyu unsigned matches mpy for positive small values");
    EXPECT_EQ(spu_mpyu(a, b), spu_il(35));
}

/* ===========================================================================
 * Compares — produce all-ones or all-zeros per lane
 * ===========================================================================*/
static void test_compares(void) {
    TEST("ceq word equal");
    u128 x = make128(0x0000000A0000000BULL, 0x0000000C0000000BULL);
    u128 y = spu_il(0xB);
    EXPECT_EQ(spu_ceq(x, y),
              make128(0x00000000FFFFFFFFULL, 0x00000000FFFFFFFFULL));

    TEST("cgt signed greater (negative is less)");
    EXPECT_EQ(spu_cgt(spu_il(1), spu_il(-1)), spu_il(-1));   /* 1 > -1 */
    EXPECT_EQ(spu_cgt(spu_il(-1), spu_il(1)), spu_il(0));    /* -1 not > 1 */

    TEST("clgt unsigned greater");
    /* 0xFFFFFFFF > 1 unsigned */
    EXPECT_EQ(spu_clgt(spu_il(-1), spu_il(1)), spu_il(-1));

    TEST("ceqb byte-wise equal produces 0xFF per matching byte");
    {
        uint8_t zb_bytes[16] = {1,2,3,4,5,6,7,8, 1,2,3,4,5,6,7,8};
        uint8_t wb_bytes[16] = {1,0,3,0,4,6,5,8, 1,0,3,0,4,6,5,8};
        /* per byte: ==FF / !=00; expected mask: FF,00,FF,00,00,FF,00,FF (x2) */
        uint8_t exp_bytes[16] = {0xFF,0,0xFF,0,0,0xFF,0,0xFF,
                                 0xFF,0,0xFF,0,0,0xFF,0,0xFF};
        EXPECT_EQ(spu_ceqb(from_bytes(zb_bytes), from_bytes(wb_bytes)),
                  from_bytes(exp_bytes));
    }
}

/* ===========================================================================
 * Shift / rotate: boundary cases at 0 and >= width
 * ===========================================================================*/
static void test_shift_rotate_boundaries(void) {
    TEST("shli with sh=0 is identity (no UB)");
    EXPECT_EQ(spu_shli(spu_il(0xAB), 0), spu_il(0xAB));

    TEST("shli with sh=31 keeps only LSB");
    EXPECT_EQ(spu_shli(spu_il(1), 31), spu_il((int32_t)0x80000000));

    TEST("shli with sh>=32 is zero");
    EXPECT_EQ(spu_shli(spu_il(0x1234), 32), spu_il(0));

    TEST("shlhi halfword sh=0 is identity");
    EXPECT_EQ(spu_shlhi(spu_ilh(0xABCD), 0), spu_ilh(0xABCD));

    TEST("shlhi sh>=16 is zero");
    EXPECT_EQ(spu_shlhi(spu_ilh(0xABCD), 16), spu_ilh(0));

    TEST("roti at sh=0 is identity (was UB pre-fix)");
    EXPECT_EQ(spu_roti(spu_il((int32_t)0xDEADBEEF), 0), spu_il((int32_t)0xDEADBEEF));

    TEST("roti with sh=8 rotates left 8 bits per word");
    EXPECT_EQ(spu_roti(spu_il((int32_t)0x12345678), 8), spu_il((int32_t)0x34567812));

    TEST("rotmi: sh effective = (-i7) & 0x3F; i7=1 -> shift right by 63 -> 0");
    EXPECT_EQ(spu_rotmi(spu_il((int32_t)0x80000000), 1), spu_il(0));

    TEST("rotmi: i7=-4 -> shift right by 4");
    EXPECT_EQ(spu_rotmi(spu_il((int32_t)0xFFFF0000), -4), spu_il(0x0FFFF000));

    TEST("rotmai: signed right shift, sign-extends");
    EXPECT_EQ(spu_rotmai(spu_il((int32_t)0x80000000), -4),
              spu_il((int32_t)0xF8000000));

    TEST("shlqbyi shifts whole quadword left by 4 bytes");
    {
        uint8_t v[16]   = {1,2,3,4,5,6,7,8, 9,10,11,12,13,14,15,16};
        uint8_t exp[16] = {5,6,7,8,9,10,11,12, 13,14,15,16,0,0,0,0};
        EXPECT_EQ(spu_shlqbyi(from_bytes(v), 4), from_bytes(exp));
    }

    TEST("rotqbyi rotates whole quadword by 4 bytes (no zero fill)");
    {
        uint8_t v[16]   = {1,2,3,4,5,6,7,8, 9,10,11,12,13,14,15,16};
        uint8_t exp[16] = {5,6,7,8,9,10,11,12, 13,14,15,16,1,2,3,4};
        EXPECT_EQ(spu_rotqbyi(from_bytes(v), 4), from_bytes(exp));
    }
}

/* ===========================================================================
 * shufb: special selector values and byte permutation
 * ===========================================================================*/
static void test_shufb(void) {
    uint8_t a_bytes[16] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                           0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F};
    uint8_t b_bytes[16] = {0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
                           0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F};
    u128 a = from_bytes(a_bytes);
    u128 b = from_bytes(b_bytes);

    TEST("shufb identity (selectors 0x00..0x0F pick a in order)");
    EXPECT_EQ(spu_shufb(a, b, from_bytes(a_bytes)), a);

    TEST("shufb selectors 0x10..0x1F pick b in order");
    EXPECT_EQ(spu_shufb(a, b, from_bytes(b_bytes)), b);

    TEST("shufb special: sel & 0xE0 == 0xE0 -> 0x80");
    EXPECT_EQ(spu_shufb(a, b, spu_splat_u8(0xE0)), spu_splat_u8(0x80));
    EXPECT_EQ(spu_shufb(a, b, spu_splat_u8(0xE3)), spu_splat_u8(0x80));

    TEST("shufb special: sel & 0xC0 == 0xC0 (and not 0xE0) -> 0xFF");
    EXPECT_EQ(spu_shufb(a, b, spu_splat_u8(0xC0)), spu_splat_u8(0xFF));

    TEST("shufb special: sel & 0xC0 == 0x80 -> 0x00");
    EXPECT_EQ(spu_shufb(a, b, spu_splat_u8(0x80)), spu_splat_u8(0x00));

    TEST("shufb mixed: reverse first 4 bytes of a, rest from b");
    {
        uint8_t sel[16] = {0x03,0x02,0x01,0x00,
                           0x14,0x15,0x16,0x17,0x18,0x19,0x1A,0x1B,
                           0x1C,0x1D,0x1E,0x1F};
        uint8_t expected[16] = {0x03,0x02,0x01,0x00,
                                0x14,0x15,0x16,0x17,0x18,0x19,0x1A,0x1B,
                                0x1C,0x1D,0x1E,0x1F};
        EXPECT_EQ(spu_shufb(a, b, from_bytes(sel)), from_bytes(expected));
    }
}

/* ===========================================================================
 * selb: bitwise select (c-bit ? b : a)
 * ===========================================================================*/
static void test_selb(void) {
    TEST("selb with c=all-zeros returns a");
    u128 a = make128(0xAAAAAAAAAAAAAAAAULL, 0xAAAAAAAAAAAAAAAAULL);
    u128 b = make128(0xBBBBBBBBBBBBBBBBULL, 0xBBBBBBBBBBBBBBBBULL);
    EXPECT_EQ(spu_selb(a, b, spu_zero()), a);

    TEST("selb with c=all-ones returns b");
    EXPECT_EQ(spu_selb(a, b, spu_splat_u8(0xFF)), b);

    TEST("selb per-bit mix");
    /* c = 0x0F repeating -> low nibble picks b, high nibble picks a */
    u128 c = spu_splat_u8(0x0F);
    u128 expected = spu_splat_u8(0xAB);   /* high nibble A from a, low nibble B from b */
    EXPECT_EQ(spu_selb(a, b, c), expected);
}

/* ===========================================================================
 * Constant generators: insertion mask shape
 * ===========================================================================*/
static void test_constant_generators(void) {
    /* Default selector pattern = identity-from-B: 0x10, 0x11, ..., 0x1F. */
    uint8_t identity_b[16] = {0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
                              0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F};

    TEST("cwd_pos at byte 0 inserts word at slot 0");
    {
        uint8_t exp[16]; memcpy(exp, identity_b, 16);
        exp[0]=0x00; exp[1]=0x01; exp[2]=0x02; exp[3]=0x03;
        EXPECT_EQ(spu_cwd_pos(0), from_bytes(exp));
    }
    TEST("cwd_pos at byte 4 inserts word at slot 1");
    {
        uint8_t exp[16]; memcpy(exp, identity_b, 16);
        exp[4]=0x00; exp[5]=0x01; exp[6]=0x02; exp[7]=0x03;
        EXPECT_EQ(spu_cwd_pos(4), from_bytes(exp));
    }
    TEST("cwd_pos at byte 12 inserts word at slot 3");
    {
        uint8_t exp[16]; memcpy(exp, identity_b, 16);
        exp[12]=0x00; exp[13]=0x01; exp[14]=0x02; exp[15]=0x03;
        EXPECT_EQ(spu_cwd_pos(12), from_bytes(exp));
    }
    TEST("cwd_pos byte 5 aligns down to slot 1 (offset 4)");
    EXPECT_EQ(spu_cwd_pos(5), spu_cwd_pos(4));

    TEST("cbd_pos at byte 7 inserts single byte 0x03 at offset 7");
    {
        uint8_t exp[16]; memcpy(exp, identity_b, 16);
        exp[7]=0x03;
        EXPECT_EQ(spu_cbd_pos(7), from_bytes(exp));
    }
    TEST("chd_pos at byte 6 inserts halfword [02,03] at offset 6 (aligned)");
    {
        uint8_t exp[16]; memcpy(exp, identity_b, 16);
        exp[6]=0x02; exp[7]=0x03;
        EXPECT_EQ(spu_chd_pos(6), from_bytes(exp));
    }
    TEST("cdd_pos at byte 0 inserts dword [00..07] at slot 0");
    {
        uint8_t exp[16]; memcpy(exp, identity_b, 16);
        for (int j = 0; j < 8; j++) exp[j] = (uint8_t)j;
        EXPECT_EQ(spu_cdd_pos(0), from_bytes(exp));
    }
}

/* ===========================================================================
 * fsmbi: bit-mask to byte-mask
 * ===========================================================================*/
static void test_fsmbi(void) {
    TEST("fsmbi 0x0000 -> all zeros");
    EXPECT_EQ(spu_fsmbi(0x0000), spu_zero());

    TEST("fsmbi 0xFFFF -> all 0xFF");
    EXPECT_EQ(spu_fsmbi(0xFFFF), spu_splat_u8(0xFF));

    TEST("fsmbi 0x8000 -> byte 0 = 0xFF (MSB bit -> first byte)");
    {
        uint8_t exp[16] = {0xFF,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0};
        EXPECT_EQ(spu_fsmbi(0x8000), from_bytes(exp));
    }
    TEST("fsmbi 0x0001 -> byte 15 = 0xFF (LSB bit -> last byte)");
    {
        uint8_t exp[16] = {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0xFF};
        EXPECT_EQ(spu_fsmbi(0x0001), from_bytes(exp));
    }
}

/* ===========================================================================
 * Sign extension
 * ===========================================================================*/
static void test_sign_extend(void) {
    TEST("xsbh: sign-extend low byte of each halfword");
    /* halfword 0xFF80 -> low byte = 0x80 = -128 -> sign-extend to 0xFF80 */
    u128 in_xsbh = spu_ilh(0xFF80);
    EXPECT_EQ(spu_xsbh(in_xsbh), spu_ilh(0xFF80));

    TEST("xsbh: positive small low byte stays the same");
    EXPECT_EQ(spu_xsbh(spu_ilh(0x007F)), spu_ilh(0x007F));

    TEST("xshw: sign-extend low halfword of each word");
    EXPECT_EQ(spu_xshw(spu_il(0x0000FFFE)), spu_il((int32_t)0xFFFFFFFE));

    TEST("xswd: sign-extend low word of each dword to 64-bit");
    u128 in_xswd = make128(0x00000000FFFFFFFFULL, 0x000000007FFFFFFFULL);
    u128 expected_xswd = make128(0xFFFFFFFFFFFFFFFFULL, 0x000000007FFFFFFFULL);
    EXPECT_EQ(spu_xswd(in_xswd), expected_xswd);
}

/* ===========================================================================
 * Floating-point fused multiply / negative variants
 * ===========================================================================*/
static void float_splat(u128* r, float v) {
    for (int i = 0; i < 4; i++) r->_f32[i] = v;
}
static int u128_f32_eq_all(u128 a, float v) {
    for (int i = 0; i < 4; i++) if (a._f32[i] != v) return 0;
    return 1;
}
static void test_float_fused(void) {
    u128 a, b, c;
    float_splat(&a, 2.0f);
    float_splat(&b, 3.0f);
    float_splat(&c, 1.0f);

    TEST("fma: a*b + c = 7.0");
    if (u128_f32_eq_all(spu_fma(a, b, c), 7.0f)) g_pass++;
    else { g_fail++; fprintf(stderr, "FAIL: %s\n", g_current); }

    TEST("fms: a*b - c = 5.0");
    if (u128_f32_eq_all(spu_fms(a, b, c), 5.0f)) g_pass++;
    else { g_fail++; fprintf(stderr, "FAIL: %s\n", g_current); }

    TEST("fnms: c - a*b = -5.0  (note: NEG-multiply-SUB convention)");
    if (u128_f32_eq_all(spu_fnms(a, b, c), -5.0f)) g_pass++;
    else { g_fail++; fprintf(stderr, "FAIL: %s\n", g_current); }
}

/* ===========================================================================
 * main: run all and report
 * ===========================================================================*/
int main(void) {
    test_immediate_loaders();
    test_simd_arith();
    test_multiply();
    test_compares();
    test_shift_rotate_boundaries();
    test_shufb();
    test_selb();
    test_constant_generators();
    test_fsmbi();
    test_sign_extend();
    test_float_fused();

    printf("\nSPU helper tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
