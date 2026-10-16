#include "../rsx_surface_layout.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                    \
    } while (0)

static int is_zeroed(const rsx_surface_layout* l)
{
    return l->width == 0 && l->height == 0 && l->storage_width == 0 &&
           l->storage_height == 0 && l->samples == 0 && l->samples_x == 0 &&
           l->samples_y == 0;
}

static void expect_layout(const rsx_surface_layout* l, uint32_t w, uint32_t h, uint32_t sw,
                          uint32_t sh, uint32_t s, uint32_t sx, uint32_t sy)
{
    CHECK(l->width == w);
    CHECK(l->height == h);
    CHECK(l->storage_width == sw);
    CHECK(l->storage_height == sh);
    CHECK(l->samples == s);
    CHECK(l->samples_x == sx);
    CHECK(l->samples_y == sy);
}

/* Pre-fills with garbage so failure paths must actively zero the output. */
static void expect_fail(uint32_t aa, uint32_t cw, uint32_t ch, uint32_t cbpp,
                        const uint32_t* cpitch, uint32_t mask, uint32_t dbpp, uint32_t dpitch,
                        int dused)
{
    rsx_surface_layout l;
    memset(&l, 0xAB, sizeof(l));
    CHECK(rsx_surface_layout_compute(aa, cw, ch, cbpp, cpitch, mask, dbpp, dpitch, dused, &l) == 0);
    CHECK(is_zeroed(&l));
}

static void test_examples(void)
{
    const uint32_t pitch[4] = {10240, 0, 0, 0};
    rsx_surface_layout l;

    CHECK(rsx_surface_layout_compute(3, 2560, 720, 4, pitch, 1, 4, 10240, 1, &l) == 1);
    expect_layout(&l, 1280, 720, 2560, 720, 2, 2, 1);

    CHECK(rsx_surface_layout_compute(0, 2560, 720, 4, pitch, 1, 4, 10240, 1, &l) == 1);
    expect_layout(&l, 2560, 720, 2560, 720, 1, 1, 1);
}

static void test_four_sample_vertical(void)
{
    const uint32_t pitch[4] = {10240, 0, 0, 0};
    rsx_surface_layout l;

    CHECK(rsx_surface_layout_compute(4, 2560, 720, 4, pitch, 1, 4, 10240, 1, &l) == 1);
    expect_layout(&l, 1280, 720, 2560, 1440, 4, 2, 2);

    CHECK(rsx_surface_layout_compute(5, 1280, 360, 4, pitch, 1, 0, 0, 0, &l) == 1);
    expect_layout(&l, 1280, 360, 2560, 720, 4, 2, 2);
}

static void test_smaller_clip(void)
{
    const uint32_t pitch[4] = {10240, 0, 0, 0};
    rsx_surface_layout l;

    CHECK(rsx_surface_layout_compute(3, 640, 480, 4, pitch, 1, 4, 10240, 1, &l) == 1);
    expect_layout(&l, 640, 480, 1280, 480, 2, 2, 1);
}

static void test_minimum_across_attachments(void)
{
    /* Capacities at 2x1: 4096/8=512, 2048/8=256, 8192/8=1024; depth 1600/8=200. */
    const uint32_t pitch[4] = {4096, 2048, 0, 8192};
    rsx_surface_layout l;

    CHECK(rsx_surface_layout_compute(3, 1000, 100, 4, pitch, 0xB, 4, 1600, 1, &l) == 1);
    expect_layout(&l, 200, 100, 400, 100, 2, 2, 1);

    /* Without depth the narrowest colour attachment governs. */
    CHECK(rsx_surface_layout_compute(3, 1000, 100, 4, pitch, 0xB, 4, 1600, 0, &l) == 1);
    expect_layout(&l, 256, 100, 512, 100, 2, 2, 1);
}

static void test_depth_only(void)
{
    rsx_surface_layout l;

    CHECK(rsx_surface_layout_compute(3, 2560, 720, 0, NULL, 0, 4, 10240, 1, &l) == 1);
    expect_layout(&l, 1280, 720, 2560, 720, 2, 2, 1);

    CHECK(rsx_surface_layout_compute(0, 640, 480, 0, NULL, 0, 2, 4096, 1, &l) == 1);
    expect_layout(&l, 640, 480, 640, 480, 1, 1, 1);
}

static void test_inactive_fields_ignored(void)
{
    /* Slots 1..3 have zero pitch but are masked off; depth is unused with zero bpp/pitch. */
    const uint32_t pitch[4] = {10240, 0, 0, 0};
    rsx_surface_layout l;

    CHECK(rsx_surface_layout_compute(3, 2560, 720, 4, pitch, 1, 0, 0, 0, &l) == 1);
    expect_layout(&l, 1280, 720, 2560, 720, 2, 2, 1);

    /* Unused depth with a tiny pitch must not constrain width. */
    CHECK(rsx_surface_layout_compute(0, 2560, 720, 4, pitch, 1, 4, 1, 0, &l) == 1);
    expect_layout(&l, 2560, 720, 2560, 720, 1, 1, 1);

    /* Depth-only: colour bpp zero and null pitch are irrelevant when mask is zero. */
    CHECK(rsx_surface_layout_compute(0, 16, 16, 0, NULL, 0, 4, 64, 1, &l) == 1);
    expect_layout(&l, 16, 16, 16, 16, 1, 1, 1);
}

static void test_rejections(void)
{
    const uint32_t pitch[4] = {10240, 10240, 10240, 10240};
    const uint32_t zero_pitch[4] = {0, 10240, 10240, 10240};
    uint32_t mode;

    /* Null output. */
    CHECK(rsx_surface_layout_compute(0, 2560, 720, 4, pitch, 1, 4, 10240, 1, NULL) == 0);

    /* Zero clip dimensions. */
    expect_fail(0, 0, 720, 4, pitch, 1, 4, 10240, 1);
    expect_fail(0, 2560, 0, 4, pitch, 1, 4, 10240, 1);

    /* Mask bits beyond the low four. */
    expect_fail(0, 2560, 720, 4, pitch, 0x10, 4, 10240, 1);
    expect_fail(0, 2560, 720, 4, pitch, 0x80000001u, 4, 10240, 1);

    /* Unsupported modes. */
    for (mode = 1; mode < 3; ++mode)
        expect_fail(mode, 2560, 720, 4, pitch, 1, 4, 10240, 1);
    expect_fail(6, 2560, 720, 4, pitch, 1, 4, 10240, 1);
    expect_fail(UINT32_MAX, 2560, 720, 4, pitch, 1, 4, 10240, 1);

    /* No active attachments. */
    expect_fail(0, 2560, 720, 4, pitch, 0, 4, 10240, 0);

    /* Missing colour pitch with active colour. */
    expect_fail(0, 2560, 720, 4, NULL, 1, 4, 10240, 1);

    /* Zero bpp for an active attachment. */
    expect_fail(0, 2560, 720, 0, pitch, 1, 4, 10240, 1);
    expect_fail(0, 2560, 720, 4, pitch, 1, 0, 10240, 1);

    /* Zero capacity for an active attachment. */
    expect_fail(0, 2560, 720, 4, zero_pitch, 1, 4, 10240, 1);
    expect_fail(3, 2560, 720, 4, pitch, 1, 4, 7, 1);
    expect_fail(0, 2560, 720, 4, pitch, 1, 4, 3, 1);
}

static void test_multiply_overflow(void)
{
    const uint32_t pitch[4] = {10240, 0, 0, 0};
    rsx_surface_layout l;

    /* Storage height = UINT32_MAX * 2 exceeds 32 bits. */
    expect_fail(4, 16, UINT32_MAX, 4, pitch, 1, 0, 0, 0);

    /* Same height without vertical expansion is representable. */
    CHECK(rsx_surface_layout_compute(3, 16, UINT32_MAX, 4, pitch, 1, 0, 0, 0, &l) == 1);
    expect_layout(&l, 16, UINT32_MAX, 32, UINT32_MAX, 2, 2, 1);

    /* Largest representable expanded height. */
    CHECK(rsx_surface_layout_compute(4, 16, UINT32_MAX / 2, 4, pitch, 1, 0, 0, 0, &l) == 1);
    CHECK(l.storage_height == (UINT32_MAX / 2) * 2);
}

static void test_bpp_product_overflow(void)
{
    const uint32_t pitch[4] = {UINT32_MAX, 0, 0, 0};
    rsx_surface_layout l;

    /* 0x80000000 * 2 wraps to 0 in 32 bits; wide arithmetic gives capacity 0 -> reject. */
    expect_fail(3, 2560, 720, 0x80000000u, pitch, 1, 0, 0, 0);
    expect_fail(4, 2560, 720, 0, NULL, 0, 0x80000000u, UINT32_MAX, 1);

    /* 0x80000001 * 2 wraps to 2 in 32 bits; wide arithmetic still rejects. */
    expect_fail(3, 2560, 720, 0x80000001u, pitch, 1, 0, 0, 0);

    /* Without horizontal expansion the same bpp fits exactly one pixel. */
    CHECK(rsx_surface_layout_compute(0, 2560, 720, 0x80000000u, pitch, 1, 0, 0, 0, &l) == 1);
    expect_layout(&l, 1, 720, 1, 720, 1, 1, 1);

    /* Max bpp and pitch still yield one pixel at 1x. */
    CHECK(rsx_surface_layout_compute(0, 2560, 720, UINT32_MAX, pitch, 1, 0, 0, 0, &l) == 1);
    expect_layout(&l, 1, 720, 1, 720, 1, 1, 1);
}

static void test_zero_on_failure_deterministic(void)
{
    const uint32_t pitch[4] = {10240, 0, 0, 0};
    rsx_surface_layout a, b;

    /* A prior success must not leak into a later failure. */
    CHECK(rsx_surface_layout_compute(4, 2560, 720, 4, pitch, 1, 4, 10240, 1, &a) == 1);
    CHECK(rsx_surface_layout_compute(2, 2560, 720, 4, pitch, 1, 4, 10240, 1, &a) == 0);
    CHECK(is_zeroed(&a));

    memset(&a, 0x5A, sizeof(a));
    memset(&b, 0xC3, sizeof(b));
    CHECK(rsx_surface_layout_compute(0, 0, 0, 0, NULL, 0, 0, 0, 0, &a) == 0);
    CHECK(rsx_surface_layout_compute(0, 0, 0, 0, NULL, 0, 0, 0, 0, &b) == 0);
    CHECK(memcmp(&a, &b, sizeof(a)) == 0);
    CHECK(is_zeroed(&a));
}

int main(void)
{
    test_examples();
    test_four_sample_vertical();
    test_smaller_clip();
    test_minimum_across_attachments();
    test_depth_only();
    test_inactive_fields_ignored();
    test_rejections();
    test_multiply_overflow();
    test_bpp_product_overflow();
    test_zero_on_failure_deterministic();

    printf("rsx_surface_layout: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
