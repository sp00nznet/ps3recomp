/*
 * Bounded Audio Grace Policy Acceptance Tests
 *
 * Note on audio policy integration:
 * PCM nonzero is heuristic not proof of completed block; controller must
 * reacquire/revalidate ports, sample device padding, use one shared wait
 * budget across all ports, never change published read index in this policy.
 */

#include "../audio_grace.h"
#include <stdio.h>
#include <stdint.h>
#include <limits.h>

static int g_failed = 0;
static int g_total = 0;

#define CHECK(cond, msg) do { \
    g_total++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (line %d): %s\n", __func__, __LINE__, (msg)); \
        g_failed++; \
    } \
} while (0)

#define CHECK_EQ_UINT(actual, expected, msg) do { \
    g_total++; \
    unsigned a_ = (actual); \
    unsigned e_ = (expected); \
    if (a_ != e_) { \
        fprintf(stderr, "FAIL: %s (line %d): %s (expected %u, got %u)\n", \
                __func__, __LINE__, (msg), e_, a_); \
        g_failed++; \
    } \
} while (0)

#define CHECK_EQ_INT(actual, expected, msg) do { \
    g_total++; \
    int a_ = (actual); \
    int e_ = (expected); \
    if (a_ != e_) { \
        fprintf(stderr, "FAIL: %s (line %d): %s (expected %d, got %d)\n", \
                __func__, __LINE__, (msg), e_, a_); \
        g_failed++; \
    } \
} while (0)

static void test_zero_initialization(void) {
    AudioGraceState s = {0};
    CHECK_EQ_UINT(s.previous_audible, 0, "zero init previous_audible is 0");
    CHECK_EQ_UINT(s.consecutive_waits, 0, "zero init consecutive_waits is 0");
    CHECK_EQ_UINT(audio_grace_budget_us(&s, 1, 768, 48000), 0,
                  "zero-init state has 0 budget due to previous_audible == 0");
}

static void test_nominal_allowed_wait(void) {
    AudioGraceState s = {0};
    audio_grace_finish(&s, 1, 0);
    CHECK_EQ_UINT(audio_grace_budget_us(&s, 1, 768, 48000), 3000,
                  "nominal allowed wait provides 3000us budget");
}

static void test_no_previous_audio(void) {
    AudioGraceState s = {0};
    audio_grace_finish(&s, 0, 0);
    CHECK_EQ_UINT(s.previous_audible, 0, "previous_audible cleared");
    CHECK_EQ_UINT(audio_grace_budget_us(&s, 1, 768, 48000), 0,
                  "no previous audio yields 0 budget");
}

static void test_disabled(void) {
    AudioGraceState s = {0};
    audio_grace_finish(&s, 1, 0);
    CHECK_EQ_UINT(audio_grace_budget_us(&s, 0, 768, 48000), 0,
                  "disabled policy yields 0 budget even with audio");
}

static void test_padding_thresholds(void) {
    AudioGraceState s = {0};
    audio_grace_finish(&s, 1, 0);

    /* At 48000 Hz: ceil(48000 * 16 / 1000) = 768 frames */
    CHECK_EQ_UINT(audio_grace_budget_us(&s, 1, 767, 48000), 0,
                  "48kHz padding below 768 returns 0");
    CHECK_EQ_UINT(audio_grace_budget_us(&s, 1, 768, 48000), 3000,
                  "48kHz padding at 768 returns 3000");
    CHECK_EQ_UINT(audio_grace_budget_us(&s, 1, 1000, 48000), 3000,
                  "48kHz padding above 768 returns 3000");

    /* At 44100 Hz: ceil(44100 * 16 / 1000) = ceil(705.6) = 706 frames */
    CHECK_EQ_UINT(audio_grace_budget_us(&s, 1, 705, 44100), 0,
                  "44.1kHz padding below 706 returns 0");
    CHECK_EQ_UINT(audio_grace_budget_us(&s, 1, 706, 44100), 3000,
                  "44.1kHz padding at 706 returns 3000");
    CHECK_EQ_UINT(audio_grace_budget_us(&s, 1, 800, 44100), 3000,
                  "44.1kHz padding above 706 returns 3000");
}

static void test_zero_sample_rate(void) {
    AudioGraceState s = {0};
    audio_grace_finish(&s, 1, 0);
    CHECK_EQ_UINT(audio_grace_budget_us(&s, 1, 1000, 0), 0,
                  "zero sample rate returns 0 budget");
}

static void test_overflow_and_extremes(void) {
    AudioGraceState s = {0};
    audio_grace_finish(&s, 1, 0);

    /* UINT_MAX padding with standard sample rate should succeed without overflow */
    CHECK_EQ_UINT(audio_grace_budget_us(&s, 1, UINT_MAX, 48000), 3000,
                  "UINT_MAX padding returns 3000");

    /* UINT_MAX sample rate requires uint64 multiplication: ceil(UINT_MAX * 16 / 1000) */
    uint64_t req_frames = ((uint64_t)UINT_MAX * 16ULL + 999ULL) / 1000ULL;
    CHECK_EQ_UINT(audio_grace_budget_us(&s, 1, 0, UINT_MAX), 0,
                  "UINT_MAX rate with 0 padding returns 0");
    CHECK_EQ_UINT(audio_grace_budget_us(&s, 1, (unsigned)(req_frames - 1), UINT_MAX), 0,
                  "UINT_MAX rate below ceil padding returns 0");
    CHECK_EQ_UINT(audio_grace_budget_us(&s, 1, (unsigned)req_frames, UINT_MAX), 3000,
                  "UINT_MAX rate meeting ceil padding returns 3000");
}

static void test_successive_waits_cap_and_reset(void) {
    AudioGraceState s = {0};
    audio_grace_finish(&s, 1, 0);
    CHECK_EQ_UINT(s.consecutive_waits, 0, "consecutive_waits starts at 0");
    CHECK_EQ_UINT(audio_grace_budget_us(&s, 1, 768, 48000), 3000,
                  "budget available with 0 waits");

    /* First wait */
    audio_grace_finish(&s, 1, 1);
    CHECK_EQ_UINT(s.consecutive_waits, 1, "1st wait increments consecutive_waits to 1");
    CHECK_EQ_UINT(audio_grace_budget_us(&s, 1, 768, 48000), 3000,
                  "budget available with 1 wait (< 2)");

    /* Second wait */
    audio_grace_finish(&s, 1, 1);
    CHECK_EQ_UINT(s.consecutive_waits, 2, "2nd wait increments consecutive_waits to 2");
    CHECK_EQ_UINT(audio_grace_budget_us(&s, 1, 768, 48000), 0,
                  "budget denied with 2 consecutive waits");

    /* Third wait: saturates at 2 */
    audio_grace_finish(&s, 1, 1);
    CHECK_EQ_UINT(s.consecutive_waits, 2, "3rd wait saturates consecutive_waits at 2");
    CHECK_EQ_UINT(audio_grace_budget_us(&s, 1, 768, 48000), 0,
                  "budget denied while saturated");

    /* waited == 0 resets consecutive_waits to 0 */
    audio_grace_finish(&s, 1, 0);
    CHECK_EQ_UINT(s.consecutive_waits, 0, "waited=0 resets consecutive_waits to 0");
    CHECK_EQ_UINT(audio_grace_budget_us(&s, 1, 768, 48000), 3000,
                  "budget restored after wait reset");
}

static void test_state_cross_talk(void) {
    AudioGraceState s1 = {0};
    AudioGraceState s2 = {0};

    audio_grace_finish(&s1, 1, 1);
    audio_grace_finish(&s1, 1, 1);
    CHECK_EQ_UINT(s1.consecutive_waits, 2, "s1 has 2 waits");
    CHECK_EQ_UINT(audio_grace_budget_us(&s1, 1, 768, 48000), 0, "s1 budget is 0");

    /* s2 must remain unaffected */
    CHECK_EQ_UINT(s2.consecutive_waits, 0, "s2 consecutive_waits remains 0");
    CHECK_EQ_UINT(s2.previous_audible, 0, "s2 previous_audible remains 0");
    CHECK_EQ_UINT(audio_grace_budget_us(&s2, 1, 768, 48000), 0, "s2 has no audio yet");

    audio_grace_finish(&s2, 1, 0);
    CHECK_EQ_UINT(audio_grace_budget_us(&s2, 1, 768, 48000), 3000,
                  "s2 operates independently with budget 3000");
    CHECK_EQ_UINT(audio_grace_budget_us(&s1, 1, 768, 48000), 0,
                  "s1 remains unchanged with budget 0");
}

static void test_null_handling(void) {
    /* finish with NULL pointer must not crash */
    audio_grace_finish(NULL, 1, 1);
    audio_grace_finish(NULL, 0, 0);

    /* budget with NULL pointer must return 0 */
    CHECK_EQ_UINT(audio_grace_budget_us(NULL, 1, 768, 48000), 0,
                  "NULL state yields 0 budget");
}

static void test_deadline_boundary_backward_wrap(void) {
    /* Nominal within-budget */
    CHECK_EQ_INT(audio_grace_before_deadline(100, 100, 3000), 1,
                 "start == now with budget 3000 is before deadline");
    CHECK_EQ_INT(audio_grace_before_deadline(100, 3099, 3000), 1,
                 "now = start + 2999 is before deadline");

    /* Exact boundary and past deadline */
    CHECK_EQ_INT(audio_grace_before_deadline(100, 3100, 3000), 0,
                 "now = start + 3000 exactly at deadline is false");
    CHECK_EQ_INT(audio_grace_before_deadline(100, 3101, 3000), 0,
                 "now = start + 3001 past deadline is false");

    /* Zero budget */
    CHECK_EQ_INT(audio_grace_before_deadline(100, 100, 0), 0,
                 "budget 0 always returns false");
    CHECK_EQ_INT(audio_grace_before_deadline(0, 0, 0), 0,
                 "start=0, now=0, budget=0 returns false");

    /* Backward clock */
    CHECK_EQ_INT(audio_grace_before_deadline(100, 99, 3000), 0,
                 "backward clock (now < start) returns false");
    CHECK_EQ_INT(audio_grace_before_deadline(1000, 0, 3000), 0,
                 "clock rewound to 0 returns false");

    /* Clock wrap */
    CHECK_EQ_INT(audio_grace_before_deadline(UINT64_MAX, 0, 3000), 0,
                 "wrap around from UINT64_MAX to 0 returns false");
    CHECK_EQ_INT(audio_grace_before_deadline(UINT64_MAX - 10, 5, 3000), 0,
                 "wrap around from near UINT64_MAX returns false");

    /* Upper 64-bit range without wrap */
    CHECK_EQ_INT(audio_grace_before_deadline(UINT64_MAX - 2000, UINT64_MAX - 1000, 3000), 1,
                 "large uint64 values within deadline");
    CHECK_EQ_INT(audio_grace_before_deadline(UINT64_MAX - 3000, UINT64_MAX, 3000), 0,
                 "large uint64 values at deadline boundary");
}

int main(void) {
    test_zero_initialization();
    test_nominal_allowed_wait();
    test_no_previous_audio();
    test_disabled();
    test_padding_thresholds();
    test_zero_sample_rate();
    test_overflow_and_extremes();
    test_successive_waits_cap_and_reset();
    test_state_cross_talk();
    test_null_handling();
    test_deadline_boundary_backward_wrap();

    printf("Audio grace tests: %d checks run, %d failures.\n", g_total, g_failed);
    return g_failed != 0 ? 1 : 0;
}
