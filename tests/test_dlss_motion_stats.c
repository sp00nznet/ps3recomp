#include "../libs/video/rsx_dlss_motion_stats.h"

#include <stdint.h>
#include <stdio.h>

static unsigned checks, failures;
#define CHECK(expr) do { \
    ++checks; \
    if (!(expr)) { \
        ++failures; \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
    } \
} while (0)

static int near(double a, double b)
{
    double d = a - b;
    return d < 0.00000001 && d > -0.00000001;
}

static void valid_case(rsx_dlss_motion_counts c, uint32_t w, uint32_t h,
                       uint64_t eligible, double matched, double fallback)
{
    rsx_dlss_motion_stats s = rsx_dlss_summarize_motion(c, w, h);
    CHECK(s.valid);
    CHECK(s.total == (uint64_t)w * (uint64_t)h);
    CHECK(s.eligible == eligible);
    CHECK(s.coverage_available == (eligible != 0));
    CHECK(near(s.matched_percent, matched));
    CHECK(near(s.fallback_percent, fallback));
}

static void invalid_case(rsx_dlss_motion_counts c, uint32_t w, uint32_t h,
                         uint64_t total, uint64_t eligible)
{
    rsx_dlss_motion_stats s = rsx_dlss_summarize_motion(c, w, h);
    CHECK(!s.valid);
    CHECK(!s.coverage_available);
    CHECK(s.total == total);
    CHECK(s.eligible == eligible);
    CHECK(s.matched_percent == 0.0);
    CHECK(s.fallback_percent == 0.0);
}

static uint32_t rng_state = UINT32_C(0x6d2b79f5);
static uint32_t next_random(void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_state = x;
}

int main(void)
{
    rsx_dlss_motion_counts c = {0, 0, 0, 0, 0, 0};
    const uint32_t hd = 1280u * 720u;
    const uint32_t wide = 3440u * 1440u;

    c.background = hd;
    valid_case(c, 1280, 720, 0, 0.0, 0.0);
    c.background = 0; c.reset = wide;
    valid_case(c, 3440, 1440, 0, 0.0, 0.0);
    c.reset = 0; c.matched = hd; c.tracked = hd; c.disoccluded = hd / 4;
    valid_case(c, 1280, 720, hd, 100.0, 0.0);
    c.matched = 0; c.fallback = wide; c.tracked = wide; c.disoccluded = 0;
    valid_case(c, 3440, 1440, wide, 0.0, 100.0);
    c.matched = hd / 2; c.fallback = hd / 2; c.tracked = hd;
    valid_case(c, 1280, 720, hd, 50.0, 50.0);
    c.matched = 1; c.fallback = 2; c.tracked = 3; c.background = 4; c.reset = 2;
    valid_case(c, 3, 3, 3, 100.0 / 3.0, 200.0 / 3.0);

    c = (rsx_dlss_motion_counts){0, 0, 0, 0, 0, 0};
    invalid_case(c, 0, 0, 0, 0);
    invalid_case(c, 0, 9, 0, 0);
    invalid_case(c, 9, 0, 0, 0);
    c.background = 8;
    invalid_case(c, 3, 3, 8, 0);
    c = (rsx_dlss_motion_counts){3, 0, 1, 1, 7, 0};
    invalid_case(c, 3, 3, 9, 2);
    c.tracked = 2; c.disoccluded = 3;
    invalid_case(c, 3, 3, 9, 2);
    c = (rsx_dlss_motion_counts){UINT32_MAX, UINT32_MAX, UINT32_MAX,
                                 UINT32_MAX, UINT32_MAX, UINT32_MAX};
    invalid_case(c, UINT32_MAX, UINT32_MAX,
                 (uint64_t)UINT32_MAX * UINT64_C(4),
                 (uint64_t)UINT32_MAX * UINT64_C(2));
    c = (rsx_dlss_motion_counts){UINT32_MAX, 0, UINT32_MAX, 0, 0, 0};
    invalid_case(c, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX);

    for (unsigned i = 0; i < 256; ++i) {
        uint32_t w = 1 + next_random() % 99u;
        uint32_t h = 1 + next_random() % 83u;
        uint32_t n = w * h;
        uint32_t matched = next_random() % (n + 1u);
        uint32_t rem = n - matched;
        uint32_t fallback = next_random() % (rem + 1u);
        rem -= fallback;
        uint32_t background = next_random() % (rem + 1u);
        uint32_t reset = rem - background;
        uint32_t tracked = matched + fallback;
        uint32_t disoccluded = tracked ? next_random() % (tracked + 1u) : 0;
        c = (rsx_dlss_motion_counts){tracked, disoccluded, matched, fallback,
                                     background, reset};
        double expected_matched = tracked ? (double)matched * 100.0 / tracked : 0.0;
        double expected_fallback = tracked ? (double)fallback * 100.0 / tracked : 0.0;
        valid_case(c, w, h, tracked, expected_matched, expected_fallback);
        c.background++;
        invalid_case(c, w, h, (uint64_t)n + 1u, tracked);
        c.background--;
        if (tracked) {
            c.tracked--;
            invalid_case(c, w, h, n, tracked);
        }
    }

    printf("test_dlss_motion_stats: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
