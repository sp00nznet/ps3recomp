/*
 * The GET_REPORT clock (gcm_report_clock.h), driven with made-up times.
 *
 * A title times its GPU frame with a report at either end of it. With scale 0
 * the walker's drain time between them does not count; the time it waited for
 * commands does. Scale 1 is the host's time, as before. Stamps never repeat or
 * go back, and a scale outside 0..1 is taken as 1.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "gcm_report_clock.h"

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                                              fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

#define MS 1000000ull

/* xorshift32: the same sequence everywhere. */
static uint32_t next_rand(uint32_t* s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

/* One frame as the walker sees it: it idles idle_ms waiting for commands,
 * then drains busy_ms with a report at the start and at the end of that pass.
 * Returns the time between the two reports. */
static uint64_t frame(gcm_report_clock* c, uint64_t* now, uint64_t idle_ms, uint64_t busy_ms)
{
    *now += idle_ms * MS;
    gcm_report_clock_drain_begin(c, *now);
    const uint64_t a = gcm_report_clock_stamp(c, *now);
    *now += busy_ms * MS;
    const uint64_t b = gcm_report_clock_stamp(c, *now);
    gcm_report_clock_drain_end(c, *now);
    return b - a;
}

int main(void)
{
    gcm_report_clock c;
    uint64_t now = 1000 * MS;

    /* Scale 1: the host's time, drain or not. */
    gcm_report_clock_init(&c, 1.0);
    CHECK(gcm_report_clock_stamp(&c, now) == now, "scale 1 stamps the host time");
    CHECK(frame(&c, &now, 5, 40) == 40 * MS, "scale 1 counts the drain");

    /* Scale 0: a 40 ms drain between the reports is not GPU time ... */
    gcm_report_clock_init(&c, 0.0);
    now = 1000 * MS;
    const uint64_t d = frame(&c, &now, 5, 40);
    CHECK(d == 1, "scale 0: reports in one pass are 1 ns apart, got %llu", (unsigned long long)d);

    /* ... the 5 ms the walker idled before the next pass is. */
    gcm_report_clock_stamp(&c, now);                     /* the end report of the frame above */
    now += 5 * MS;
    gcm_report_clock_drain_begin(&c, now);
    const uint64_t s0 = c.last;
    const uint64_t s1 = gcm_report_clock_stamp(&c, now);
    /* Within the few ns the strictly rising stamps of the pass above took. */
    CHECK(s1 - s0 + 4 >= 5 * MS && s1 - s0 <= 5 * MS, "scale 0 counts idle time: %llu",
          (unsigned long long)(s1 - s0));
    gcm_report_clock_drain_end(&c, now);

    /* Half scale: half of the drain counts. */
    gcm_report_clock_init(&c, 0.5);
    now = 1000 * MS;
    CHECK(frame(&c, &now, 5, 40) == 20 * MS, "scale 0.5 counts half the drain");

    /* Stamps rise strictly even when the host time does not move. */
    gcm_report_clock_init(&c, 1.0);
    const uint64_t r0 = gcm_report_clock_stamp(&c, now);
    const uint64_t r1 = gcm_report_clock_stamp(&c, now);
    const uint64_t r2 = gcm_report_clock_stamp(&c, now - 10);
    CHECK(r1 == r0 + 1 && r2 == r1 + 1, "stamps rise strictly");

    /* With scale 0 the clock never goes back, over many frames of any mix, and
     * runs ahead of the host time only by the 1 ns a stamp needs to rise when
     * the host time has not moved. Our own generator: the libc's rand()
     * sequence differs between platforms. */
    gcm_report_clock_init(&c, 0.0);
    now = 1000 * MS;
    uint64_t prev = 0;
    uint32_t rng = 7u;
    for (int i = 0; i < 10000; i++) {
        now += (uint64_t)(next_rand(&rng) % 3u) * MS;
        if (next_rand(&rng) & 1u) gcm_report_clock_drain_begin(&c, now);
        now += (uint64_t)(next_rand(&rng) % 50u) * MS;
        const uint64_t t = gcm_report_clock_stamp(&c, now);
        CHECK(t > prev, "stamp %d went back", i);
        CHECK(t <= now || t == prev + 1u, "stamp %d ahead of the host time", i);
        prev = t;
        gcm_report_clock_drain_end(&c, now);
    }

    /* The knob's text: unset, empty, words or trailing junk are 1. */
    CHECK(gcm_report_clock_parse_scale(NULL) == 1.0, "unset is 1");
    CHECK(gcm_report_clock_parse_scale("") == 1.0, "empty is 1");
    CHECK(gcm_report_clock_parse_scale("0") == 0.0, "\"0\" is 0");
    CHECK(gcm_report_clock_parse_scale("0.25 ") == 0.25, "\"0.25 \" is 0.25");
    CHECK(gcm_report_clock_parse_scale("1") == 1.0, "\"1\" is 1");
    CHECK(gcm_report_clock_parse_scale("off") == 1.0, "\"off\" is 1, not 0");
    CHECK(gcm_report_clock_parse_scale("  ") == 1.0, "blanks are 1");
    CHECK(gcm_report_clock_parse_scale("0x") == 1.0, "\"0x\" is 1");
    CHECK(gcm_report_clock_parse_scale("0.5ms") == 1.0, "trailing text is 1");

    /* Out of range or not a number: the host time. */
    gcm_report_clock_init(&c, -1.0);
    CHECK(c.scale == 1.0, "negative scale is 1");
    gcm_report_clock_init(&c, 3.0);
    CHECK(c.scale == 1.0, "scale above 1 is 1");
    gcm_report_clock_init(&c, nan(""));
    CHECK(c.scale == 1.0, "NaN scale is 1");

    if (failures) { fprintf(stderr, "test_gcm_report_clock: %d failure(s)\n", failures); return 1; }
    printf("test_gcm_report_clock: ok\n");
    return 0;
}
