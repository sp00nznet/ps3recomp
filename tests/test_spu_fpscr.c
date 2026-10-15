/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Tests for the SPU FPSCR state module, whose behavior is derived from
 * RPCS3's SPU_FPSCR (rpcs3/Emu/Cell/SPUThread.h, upstream revision
 * 4d5def5880217135554ed434cd5055cceebe9cc2, GPL-2.0). Expected values are
 * written out explicitly in our LEFT-first word order.
 */
#include "../runtime/spu/spu_fpscr.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static int g_failures;
static int g_checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
        }                                                                    \
    } while (0)

static void expect_words_at(const spu_fpscr *s, uint32_t e0, uint32_t e1,
                            uint32_t e2, uint32_t e3, int line)
{
    uint32_t got[4];
    const uint32_t exp[4] = { e0, e1, e2, e3 };
    int i;

    spu_fpscr_read(s, got);
    for (i = 0; i < 4; ++i) {
        ++g_checks;
        if (got[i] != exp[i] || s->words[i] != exp[i]) {
            ++g_failures;
            fprintf(stderr,
                    "%s:%d: word %d: read 0x%08lx, stored 0x%08lx, expected 0x%08lx\n",
                    __FILE__, line, i, (unsigned long)got[i],
                    (unsigned long)s->words[i], (unsigned long)exp[i]);
        }
    }
}

#define EXPECT_WORDS(s, e0, e1, e2, e3) expect_words_at((s), (e0), (e1), (e2), (e3), __LINE__)

static void fill_garbage(spu_fpscr *s)
{
    s->words[0] = 0xDEADBEEFu;
    s->words[1] = 0xFFFFFFFFu;
    s->words[2] = 0x12345678u;
    s->words[3] = 0x80000001u;
}

static void test_reset(void)
{
    spu_fpscr s;
    fill_garbage(&s);
    spu_fpscr_reset(&s);
    EXPECT_WORDS(&s, 0u, 0u, 0u, 0u);
    CHECK(spu_fpscr_get_rounding(&s, 0) == SPU_FPSCR_RN_NEAR);
    CHECK(spu_fpscr_get_rounding(&s, 1) == SPU_FPSCR_RN_NEAR);
}

static void test_independent_contexts(void)
{
    spu_fpscr a;
    spu_fpscr b;
    spu_fpscr_reset(&a);
    spu_fpscr_reset(&b);

    CHECK(spu_fpscr_single_flags(&a, 1, SPU_FPSCR_SOVF) == 0);
    CHECK(spu_fpscr_set_rounding(&a, 0, SPU_FPSCR_RN_MINF) == 0);
    CHECK(spu_fpscr_divide_by_zero(&a, 2) == 0);
    EXPECT_WORDS(&a, 0x00000C00u, 0x00000004u, 0u, 0x00000200u);
    EXPECT_WORDS(&b, 0u, 0u, 0u, 0u);

    CHECK(spu_fpscr_double_flags(&b, 1, SPU_FPSCR_DINV) == 0);
    EXPECT_WORDS(&b, 0u, 0u, 0x00000400u, 0u);
    EXPECT_WORDS(&a, 0x00000C00u, 0x00000004u, 0u, 0x00000200u);
}

static void test_all_ones_write(void)
{
    static const uint32_t ones[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
    spu_fpscr s;
    spu_fpscr_reset(&s);
    spu_fpscr_write(&s, ones);
    EXPECT_WORDS(&s, 0x00000F07u, 0x00003F07u, 0x00003F07u, 0x00000F07u);
    CHECK(spu_fpscr_get_rounding(&s, 0) == SPU_FPSCR_RN_MINF);
    CHECK(spu_fpscr_get_rounding(&s, 1) == SPU_FPSCR_RN_MINF);
}

static void test_asymmetric_endian_mapping(void)
{
    /* Distinct per-word patterns; bits outside the masks must vanish. */
    static const uint32_t in[4] = { 0xF0F00B02u, 0x0000C203u, 0xFFFF2A04u, 0x10000805u };
    spu_fpscr s;
    spu_fpscr_reset(&s);
    spu_fpscr_write(&s, in);
    EXPECT_WORDS(&s, 0x00000B02u, 0x00000203u, 0x00002A04u, 0x00000805u);
    /* LEFT word bits 10-11 = 2 (slice 0), bits 8-9 = 3 (slice 1). */
    CHECK(spu_fpscr_get_rounding(&s, 0) == SPU_FPSCR_RN_PINF);
    CHECK(spu_fpscr_get_rounding(&s, 1) == SPU_FPSCR_RN_MINF);

    {
        static const uint32_t in2[4] = { 0x00000100u, 0x00002000u, 0x00000001u, 0x00000006u };
        spu_fpscr_write(&s, in2);
        EXPECT_WORDS(&s, 0x00000100u, 0x00002000u, 0x00000001u, 0x00000006u);
        CHECK(spu_fpscr_get_rounding(&s, 0) == SPU_FPSCR_RN_NEAR);
        CHECK(spu_fpscr_get_rounding(&s, 1) == SPU_FPSCR_RN_ZERO);
    }
}

static void test_round_trip_and_zero_clear(void)
{
    static const uint32_t pattern[4] = { 0x00000E05u, 0x00001B02u, 0x00003401u, 0x00000903u };
    static const uint32_t zeros[4] = { 0u, 0u, 0u, 0u };
    uint32_t out[4] = { 0xAAAAAAAAu, 0xAAAAAAAAu, 0xAAAAAAAAu, 0xAAAAAAAAu };
    spu_fpscr s;
    int i;

    spu_fpscr_reset(&s);
    spu_fpscr_write(&s, pattern);
    spu_fpscr_read(&s, out);
    for (i = 0; i < 4; ++i) {
        CHECK(out[i] == pattern[i]);
    }

    /* Writing zeros replaces everything, including sticky flags. */
    CHECK(spu_fpscr_single_flags(&s, 3, SPU_FPSCR_SDIFF) == 0);
    CHECK(spu_fpscr_double_flags(&s, 0, SPU_FPSCR_DOVF) == 0);
    spu_fpscr_write(&s, zeros);
    EXPECT_WORDS(&s, 0u, 0u, 0u, 0u);
    CHECK(spu_fpscr_get_rounding(&s, 0) == 0);
    CHECK(spu_fpscr_get_rounding(&s, 1) == 0);

    /* Write replaces rather than merges. */
    spu_fpscr_write(&s, pattern);
    {
        static const uint32_t other[4] = { 0x00000001u, 0u, 0x00000100u, 0u };
        spu_fpscr_write(&s, other);
        EXPECT_WORDS(&s, 0x00000001u, 0u, 0x00000100u, 0u);
    }
}

static void test_in_place(void)
{
    spu_fpscr s;

    /* Unmasked garbage written directly, then FSCRWR from the state itself. */
    s.words[0] = 0xFFFFFFFFu;
    s.words[1] = 0x0000FFFFu;
    s.words[2] = 0xFFFF0000u;
    s.words[3] = 0x12345678u;
    spu_fpscr_write(&s, s.words);
    EXPECT_WORDS(&s, 0x00000F07u, 0x00003F07u, 0u, 0x00000600u);

    spu_fpscr_read(&s, s.words);
    EXPECT_WORDS(&s, 0x00000F07u, 0x00003F07u, 0u, 0x00000600u);

    spu_fpscr_write(&s, s.words);
    EXPECT_WORDS(&s, 0x00000F07u, 0x00003F07u, 0u, 0x00000600u);
}

static void test_single_flag_mapping(void)
{
    static const uint32_t flags[3] = { SPU_FPSCR_SOVF, SPU_FPSCR_SUNF, SPU_FPSCR_SDIFF };
    static const uint32_t bits[3] = { 0x4u, 0x2u, 0x1u };
    unsigned slice;
    int f;

    for (slice = 0; slice < 4u; ++slice) {
        for (f = 0; f < 3; ++f) {
            spu_fpscr s;
            uint32_t e[4] = { 0u, 0u, 0u, 0u };
            spu_fpscr_reset(&s);
            CHECK(spu_fpscr_single_flags(&s, slice, flags[f]) == 0);
            e[slice] = bits[f];
            EXPECT_WORDS(&s, e[0], e[1], e[2], e[3]);
        }
    }

    {
        spu_fpscr s;
        spu_fpscr_reset(&s);
        CHECK(spu_fpscr_single_flags(&s, 0, SPU_FPSCR_SOVF | SPU_FPSCR_SDIFF) == 0);
        CHECK(spu_fpscr_single_flags(&s, 3, SPU_FPSCR_SUNF) == 0);
        CHECK(spu_fpscr_single_flags(&s, 2, 0u) == 0);
        EXPECT_WORDS(&s, 0x5u, 0u, 0u, 0x2u);
    }
}

static void test_double_flag_mapping(void)
{
    static const uint32_t flags[6] = {
        SPU_FPSCR_DOVF, SPU_FPSCR_DUNF, SPU_FPSCR_DINX,
        SPU_FPSCR_DINV, SPU_FPSCR_DNAN, SPU_FPSCR_DDENORM
    };
    static const uint32_t bits[6] = { 0x2000u, 0x1000u, 0x0800u, 0x0400u, 0x0200u, 0x0100u };
    int f;

    for (f = 0; f < 6; ++f) {
        spu_fpscr s;
        spu_fpscr_reset(&s);
        CHECK(spu_fpscr_double_flags(&s, 0, flags[f]) == 0);
        EXPECT_WORDS(&s, 0u, bits[f], 0u, 0u);

        spu_fpscr_reset(&s);
        CHECK(spu_fpscr_double_flags(&s, 1, flags[f]) == 0);
        EXPECT_WORDS(&s, 0u, 0u, bits[f], 0u);
    }

    {
        spu_fpscr s;
        spu_fpscr_reset(&s);
        CHECK(spu_fpscr_double_flags(&s, 0, 0x3F00u) == 0);
        CHECK(spu_fpscr_double_flags(&s, 1, SPU_FPSCR_DNAN | SPU_FPSCR_DDENORM) == 0);
        EXPECT_WORDS(&s, 0u, 0x3F00u, 0x0300u, 0u);
    }
}

static void test_divide_by_zero_mapping(void)
{
    static const uint32_t bits[4] = { 0x800u, 0x400u, 0x200u, 0x100u };
    unsigned slice;
    spu_fpscr all;

    for (slice = 0; slice < 4u; ++slice) {
        spu_fpscr s;
        spu_fpscr_reset(&s);
        CHECK(spu_fpscr_divide_by_zero(&s, slice) == 0);
        EXPECT_WORDS(&s, 0u, 0u, 0u, bits[slice]);
    }

    spu_fpscr_reset(&all);
    for (slice = 0; slice < 4u; ++slice) {
        CHECK(spu_fpscr_divide_by_zero(&all, slice) == 0);
    }
    EXPECT_WORDS(&all, 0u, 0u, 0u, 0x00000F00u);
    CHECK(spu_fpscr_get_rounding(&all, 0) == 0);
    CHECK(spu_fpscr_get_rounding(&all, 1) == 0);
}

static void test_rounding_modes(void)
{
    static const uint32_t ones[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
    /* Expected LEFT word after setting mode m from zero state. */
    static const uint32_t zero_s0[4] = { 0x000u, 0x400u, 0x800u, 0xC00u };
    static const uint32_t zero_s1[4] = { 0x000u, 0x100u, 0x200u, 0x300u };
    /* Expected LEFT word after setting mode m from all-ones state. */
    static const uint32_t ones_s0[4] = { 0x307u, 0x707u, 0xB07u, 0xF07u };
    static const uint32_t ones_s1[4] = { 0xC07u, 0xD07u, 0xE07u, 0xF07u };
    unsigned m;

    for (m = 0; m < 4u; ++m) {
        spu_fpscr s;

        spu_fpscr_reset(&s);
        CHECK(spu_fpscr_set_rounding(&s, 0, m) == 0);
        EXPECT_WORDS(&s, zero_s0[m], 0u, 0u, 0u);
        CHECK(spu_fpscr_get_rounding(&s, 0) == (int)m);
        CHECK(spu_fpscr_get_rounding(&s, 1) == 0);

        spu_fpscr_reset(&s);
        CHECK(spu_fpscr_set_rounding(&s, 1, m) == 0);
        EXPECT_WORDS(&s, zero_s1[m], 0u, 0u, 0u);
        CHECK(spu_fpscr_get_rounding(&s, 1) == (int)m);
        CHECK(spu_fpscr_get_rounding(&s, 0) == 0);

        spu_fpscr_write(&s, ones);
        CHECK(spu_fpscr_set_rounding(&s, 0, m) == 0);
        EXPECT_WORDS(&s, ones_s0[m], 0x3F07u, 0x3F07u, 0xF07u);
        CHECK(spu_fpscr_get_rounding(&s, 0) == (int)m);
        CHECK(spu_fpscr_get_rounding(&s, 1) == 3);

        spu_fpscr_write(&s, ones);
        CHECK(spu_fpscr_set_rounding(&s, 1, m) == 0);
        EXPECT_WORDS(&s, ones_s1[m], 0x3F07u, 0x3F07u, 0xF07u);
        CHECK(spu_fpscr_get_rounding(&s, 1) == (int)m);
        CHECK(spu_fpscr_get_rounding(&s, 0) == 3);
    }

    {
        spu_fpscr s;
        spu_fpscr_reset(&s);
        CHECK(spu_fpscr_single_flags(&s, 0, SPU_FPSCR_SUNF) == 0);
        CHECK(spu_fpscr_set_rounding(&s, 0, SPU_FPSCR_RN_ZERO) == 0);
        CHECK(spu_fpscr_set_rounding(&s, 1, SPU_FPSCR_RN_PINF) == 0);
        EXPECT_WORDS(&s, 0x602u, 0u, 0u, 0u);
        CHECK(spu_fpscr_set_rounding(&s, 0, SPU_FPSCR_RN_NEAR) == 0);
        EXPECT_WORDS(&s, 0x202u, 0u, 0u, 0u);
    }
}

static void test_sticky_accumulation(void)
{
    static const uint32_t base[4] = { 0x00000001u, 0x00000100u, 0u, 0x00000800u };
    spu_fpscr s;
    spu_fpscr_reset(&s);

    CHECK(spu_fpscr_single_flags(&s, 2, SPU_FPSCR_SOVF) == 0);
    CHECK(spu_fpscr_single_flags(&s, 2, SPU_FPSCR_SUNF) == 0);
    CHECK(spu_fpscr_single_flags(&s, 2, SPU_FPSCR_SOVF) == 0);
    EXPECT_WORDS(&s, 0u, 0u, 0x6u, 0u);
    CHECK(spu_fpscr_single_flags(&s, 2, SPU_FPSCR_SDIFF) == 0);
    EXPECT_WORDS(&s, 0u, 0u, 0x7u, 0u);

    CHECK(spu_fpscr_double_flags(&s, 1, SPU_FPSCR_DOVF) == 0);
    CHECK(spu_fpscr_double_flags(&s, 1, SPU_FPSCR_DNAN) == 0);
    CHECK(spu_fpscr_double_flags(&s, 1, SPU_FPSCR_DOVF) == 0);
    EXPECT_WORDS(&s, 0u, 0u, 0x2207u, 0u);

    CHECK(spu_fpscr_divide_by_zero(&s, 1) == 0);
    CHECK(spu_fpscr_divide_by_zero(&s, 1) == 0);
    CHECK(spu_fpscr_divide_by_zero(&s, 3) == 0);
    EXPECT_WORDS(&s, 0u, 0u, 0x2207u, 0x500u);

    /* Flags OR onto values loaded by write. */
    spu_fpscr_write(&s, base);
    CHECK(spu_fpscr_single_flags(&s, 0, SPU_FPSCR_SOVF) == 0);
    CHECK(spu_fpscr_double_flags(&s, 0, SPU_FPSCR_DINX) == 0);
    CHECK(spu_fpscr_divide_by_zero(&s, 3) == 0);
    CHECK(spu_fpscr_single_flags(&s, 3, SPU_FPSCR_SUNF) == 0);
    EXPECT_WORDS(&s, 0x5u, 0x900u, 0u, 0x902u);
}

static void test_invalid_rejection(void)
{
    static const uint32_t pattern[4] = { 0x00000B05u, 0x00002A03u, 0x00001506u, 0x00000C01u };
    spu_fpscr s;
    spu_fpscr_reset(&s);
    spu_fpscr_write(&s, pattern);

    CHECK(spu_fpscr_get_rounding(NULL, 0) == -1);
    CHECK(spu_fpscr_set_rounding(NULL, 0, 0) == -1);
    CHECK(spu_fpscr_single_flags(NULL, 0, SPU_FPSCR_SOVF) == -1);
    CHECK(spu_fpscr_double_flags(NULL, 0, SPU_FPSCR_DOVF) == -1);
    CHECK(spu_fpscr_divide_by_zero(NULL, 0) == -1);

    CHECK(spu_fpscr_get_rounding(&s, 2) == -1);
    CHECK(spu_fpscr_get_rounding(&s, 0xFFFFFFFFu) == -1);
    CHECK(spu_fpscr_set_rounding(&s, 2, 0) == -1);
    CHECK(spu_fpscr_set_rounding(&s, 0xFFFFFFFFu, 1) == -1);
    CHECK(spu_fpscr_set_rounding(&s, 0, 4) == -1);
    CHECK(spu_fpscr_set_rounding(&s, 1, 0xFFFFFFFFu) == -1);
    CHECK(spu_fpscr_single_flags(&s, 4, SPU_FPSCR_SOVF) == -1);
    CHECK(spu_fpscr_single_flags(&s, 0xFFFFFFFFu, SPU_FPSCR_SOVF) == -1);
    CHECK(spu_fpscr_single_flags(&s, 0, 0x8u) == -1);
    CHECK(spu_fpscr_single_flags(&s, 1, SPU_FPSCR_DOVF) == -1);
    CHECK(spu_fpscr_single_flags(&s, 2, 0x7u | 0x100u) == -1);
    CHECK(spu_fpscr_single_flags(&s, 3, 0x80000000u) == -1);
    CHECK(spu_fpscr_double_flags(&s, 2, SPU_FPSCR_DOVF) == -1);
    CHECK(spu_fpscr_double_flags(&s, 0xFFFFFFFFu, SPU_FPSCR_DOVF) == -1);
    CHECK(spu_fpscr_double_flags(&s, 0, SPU_FPSCR_SOVF) == -1);
    CHECK(spu_fpscr_double_flags(&s, 1, 0x4000u) == -1);
    CHECK(spu_fpscr_double_flags(&s, 0, 0x3F00u | 0x80u) == -1);
    CHECK(spu_fpscr_double_flags(&s, 1, 0xFFFFFFFFu) == -1);
    CHECK(spu_fpscr_divide_by_zero(&s, 4) == -1);
    CHECK(spu_fpscr_divide_by_zero(&s, 0xFFFFFFFFu) == -1);

    EXPECT_WORDS(&s, 0x00000B05u, 0x00002A03u, 0x00001506u, 0x00000C01u);
    CHECK(spu_fpscr_get_rounding(&s, 0) == SPU_FPSCR_RN_PINF);
    CHECK(spu_fpscr_get_rounding(&s, 1) == SPU_FPSCR_RN_MINF);
}

static void test_mask_vectors(void)
{
    static const uint32_t masks[4] = { 0x00000F07u, 0x00003F07u, 0x00003F07u, 0x00000F07u };
    uint32_t seed = 0x2545F491u;
    int c;

    for (c = 0; c < 256; ++c) {
        uint32_t in[4];
        uint32_t out[4];
        uint32_t exp[4];
        spu_fpscr s;
        int i;

        for (i = 0; i < 4; ++i) {
            seed = seed * 1664525u + 1013904223u;
            in[i] = seed ^ ((uint32_t)c * 0x01010101u);
            exp[i] = in[i] & masks[i];
        }

        fill_garbage(&s);
        spu_fpscr_write(&s, in);
        spu_fpscr_read(&s, out);
        for (i = 0; i < 4; ++i) {
            CHECK(out[i] == exp[i]);
        }
        CHECK(spu_fpscr_get_rounding(&s, 0) == (int)((exp[0] >> 10) & 3u));
        CHECK(spu_fpscr_get_rounding(&s, 1) == (int)((exp[0] >> 8) & 3u));

        /* Re-writing the read value is a stable round trip. */
        spu_fpscr_write(&s, out);
        EXPECT_WORDS(&s, exp[0], exp[1], exp[2], exp[3]);
    }
}

int main(void)
{
    test_reset();
    test_independent_contexts();
    test_all_ones_write();
    test_asymmetric_endian_mapping();
    test_round_trip_and_zero_clear();
    test_in_place();
    test_single_flag_mapping();
    test_double_flag_mapping();
    test_divide_by_zero_mapping();
    test_rounding_modes();
    test_sticky_accumulation();
    test_invalid_rejection();
    test_mask_vectors();

    if (g_failures != 0) {
        fprintf(stderr, "test_spu_fpscr: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("test_spu_fpscr: all %d checks passed\n", g_checks);
    return 0;
}
