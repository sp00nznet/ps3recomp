#include <stdio.h>
#include <stdint.h>
#include "../pad_stick_motion.h"

static int g_checks, g_fail;
static uint16_t o[4];

#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define REST 399
#define HI 60
#define LO 738
#define EXPECT(r1, y, t, v) CHECK(U(&s, (r1), (y), (t)) == 1 && is(v))

static int U(PadStickMotion *s, int r1, unsigned y, uint64_t t)
{
    return pad_stick_motion_update(s, 1, 1, r1, y, t, o);
}
static int is(unsigned y) { return o[0] == 512 && o[1] == y && o[2] == 512 && o[3] == 512; }
static int zero(void) { return !o[0] && !o[1] && !o[2] && !o[3]; }
static void fill(void) { o[0] = o[1] = o[2] = o[3] = 0xFFFF; }

static void test_idle(void)
{
    PadStickMotion s = {0};
    EXPECT(0, 128, 0, REST);
    EXPECT(1, 128, 5, REST);
    EXPECT(0, 0, 10, REST);
}

static void test_disabled(void)
{
    PadStickMotion s = {0};
    fill();
    CHECK(pad_stick_motion_update(&s, 0, 1, 1, 128, 0, o) == 0 && zero());
    fill();
    CHECK(pad_stick_motion_update(&s, 1, 0, 1, 128, 0, o) == 0 && zero());
    EXPECT(1, 128, 10, REST);
    CHECK(pad_stick_motion_update(&s, 0, 1, 1, 128, 20, o) == 0 && zero());
    EXPECT(1, 0, 30, REST); /* disabling dropped the arm */
}

static void test_enable_while_up(void)
{
    PadStickMotion s = {0};
    for (uint64_t t = 0; t <= 500; t += 10)
        EXPECT(1, 0, t, REST);
}

static void test_r1_gating(void)
{
    PadStickMotion s = {0};
    EXPECT(0, 128, 0, REST);
    EXPECT(1, 0, 10, REST); /* neutral without R1 does not arm */
    pad_stick_motion_reset(&s);
    EXPECT(1, 128, 0, REST);
    EXPECT(0, 128, 10, REST);
    EXPECT(1, 0, 20, REST); /* release cleared arm */
    pad_stick_motion_reset(&s);
    EXPECT(0, 0, 0, REST);
    EXPECT(1, 0, 10, REST); /* R1 pressed while already up */
    pad_stick_motion_reset(&s);
    EXPECT(1, 128, 0, REST);
    EXPECT(1, 0, 10, HI);
    EXPECT(0, 0, 20, REST); /* release cancels pulse */
    EXPECT(1, 0, 30, REST);
}

static void test_pulse_boundaries(void)
{
    PadStickMotion s = {0};
    EXPECT(1, 128, 1000, REST);
    EXPECT(1, 32, 1100, HI);
    EXPECT(1, 0, 1179, HI);
    EXPECT(1, 0, 1180, LO);
    EXPECT(1, 0, 1259, LO);
    EXPECT(1, 0, 1260, REST);

    pad_stick_motion_reset(&s);
    EXPECT(1, 128, 0, REST);
    EXPECT(1, 33, 10, REST);
    EXPECT(1, 33, 20, REST);
    EXPECT(1, 32, 30, HI);

    pad_stick_motion_reset(&s);
    EXPECT(1, 95, 0, REST);
    EXPECT(1, 0, 10, REST);
    pad_stick_motion_reset(&s);
    EXPECT(1, 161, 0, REST);
    EXPECT(1, 0, 10, REST);
    pad_stick_motion_reset(&s);
    EXPECT(1, 96, 0, REST);
    EXPECT(1, 0, 10, HI);
    pad_stick_motion_reset(&s);
    EXPECT(1, 160, 0, REST);
    EXPECT(1, 0, 10, HI);
}

static void test_slow_flick(void)
{
    PadStickMotion s = {0};
    EXPECT(1, 128, 0, REST);
    EXPECT(1, 64, 100, REST);
    EXPECT(1, 0, 251, REST);
    pad_stick_motion_reset(&s);
    EXPECT(1, 128, 0, REST);
    EXPECT(1, 0, 250, HI);
}

static void test_repeats_and_rearm(void)
{
    PadStickMotion s = {0};
    EXPECT(1, 128, 0, REST);
    EXPECT(1, 0, 50, HI);
    for (uint64_t t = 210; t <= 3000; t += 10)
        EXPECT(1, 0, t, REST);
    EXPECT(1, 128, 3010, REST);
    EXPECT(1, 0, 3050, HI);

    pad_stick_motion_reset(&s); /* neutral during pulse does not rearm */
    EXPECT(1, 128, 0, REST);
    EXPECT(1, 0, 10, HI);
    EXPECT(1, 128, 100, LO);
    EXPECT(1, 0, 200, REST);

    pad_stick_motion_reset(&s); /* rearm after R1 release */
    EXPECT(1, 128, 0, REST);
    EXPECT(1, 0, 10, HI);
    EXPECT(0, 0, 20, REST);
    EXPECT(1, 0, 30, REST);
    EXPECT(1, 128, 40, REST);
    EXPECT(1, 0, 60, HI);
}

static void test_sensor_off_midpulse(void)
{
    PadStickMotion s = {0};
    EXPECT(1, 128, 0, REST);
    EXPECT(1, 0, 10, HI);
    fill();
    CHECK(pad_stick_motion_update(&s, 1, 0, 1, 0, 20, o) == 0 && zero());
    EXPECT(1, 0, 30, REST);
    EXPECT(1, 128, 40, REST);
    EXPECT(1, 0, 50, HI);
}

static void test_time_regression(void)
{
    PadStickMotion s = {0};
    EXPECT(1, 128, 1000, REST);
    EXPECT(1, 0, 900, REST);
    EXPECT(1, 0, 950, REST);

    pad_stick_motion_reset(&s);
    EXPECT(1, 128, 1000, REST);
    EXPECT(1, 0, 1010, HI);
    EXPECT(1, 0, 1005, REST);
    EXPECT(1, 0, 1020, REST);
    EXPECT(1, 128, 1030, REST);
    EXPECT(1, 0, 1040, HI);
}

static void test_nulls(void)
{
    PadStickMotion s = {0};
    fill();
    CHECK(pad_stick_motion_update(NULL, 1, 1, 1, 128, 0, o) == 0 && zero());
    CHECK(pad_stick_motion_update(NULL, 1, 1, 1, 128, 0, NULL) == 0);
    pad_stick_motion_reset(NULL);
    EXPECT(1, 128, 0, REST);
    CHECK(pad_stick_motion_update(&s, 1, 1, 1, 128, 10, NULL) == 0);
    EXPECT(1, 0, 20, REST); /* state was reset */
}

static void test_invalid_args(void)
{
    static const struct { int a, so, r1; unsigned y; } bad[] = {
        {1, 1, 1, 256}, {1, 1, 1, 1000}, {2, 1, 1, 128}, {-1, 1, 1, 128},
        {1, 2, 1, 128}, {1, -1, 1, 128}, {1, 1, 2, 128}, {1, 1, -1, 128},
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        PadStickMotion s = {0};
        EXPECT(1, 128, 0, REST);
        fill();
        CHECK(pad_stick_motion_update(&s, bad[i].a, bad[i].so, bad[i].r1, bad[i].y, 5, o) == 0 &&
              zero());
        EXPECT(1, 0, 10, REST);
    }
}

static void test_large_times(void)
{
    const uint64_t M = UINT64_MAX;
    PadStickMotion s = {0};
    EXPECT(1, 128, M - 200, REST);
    EXPECT(1, 0, M - 170, HI);
    EXPECT(1, 0, M - 91, HI);
    EXPECT(1, 0, M - 90, LO);
    EXPECT(1, 0, M - 11, LO);
    EXPECT(1, 0, M - 10, REST);
    EXPECT(1, 128, M, REST);
    EXPECT(1, 0, 0, REST); /* regression from max */

    pad_stick_motion_reset(&s);
    EXPECT(1, 128, 0, REST);
    EXPECT(1, 0, 10, HI);
    EXPECT(1, 0, M, REST); /* huge gap: no late pulse */

    pad_stick_motion_reset(&s);
    EXPECT(1, 128, 0, REST);
    EXPECT(1, 0, M, REST); /* arm expired */
}

int main(void)
{
    test_idle();
    test_disabled();
    test_enable_while_up();
    test_r1_gating();
    test_pulse_boundaries();
    test_slow_flick();
    test_repeats_and_rearm();
    test_sensor_off_midpulse();
    test_time_regression();
    test_nulls();
    test_invalid_args();
    test_large_times();
    printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
