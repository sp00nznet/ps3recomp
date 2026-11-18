/*
 * cellAudio mixer pacing (libs/audio/audio_pace.h), driven with a simulated
 * clock and a simulated device queue. No sleeps: every case is deterministic.
 *
 *  steady      a device that pulls two blocks every two periods (macOS SDL):
 *              every mix one period after the last, 187.5 blocks/s exactly
 *              over 1e6 periods.
 *  cold        the same device from an empty queue: after a short fill the
 *              cadence settles and no catch-up or pair follows.
 *  slower      device 0.1 % slower than the host: the queue stays <= HI and
 *              the skips make up the difference.
 *  faster      device 0.1 % faster, pulling one or two blocks at a time: it
 *              never finds the queue short, and catch-ups make up the deficit.
 *  stall       50 ms host stall: one restart, no burst.
 *  nodevice    pure cadence.
 *
 * `test_audio_pace smoke` (not in ctest) runs the real sleep path for 2000
 * periods on this host and prints the wake-late percentiles and the rate.
 */

#define _POSIX_C_SOURCE 200809L
#include "../audio_pace.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        return false; \
    } \
} while (0)

static uint64_t period_end(uint64_t n) { return n * AUDIO_PACE_PERIOD_NUM_NS / AUDIO_PACE_PERIOD_DENOM; }

/* A device that pulls `per_pull` blocks at t = k * pull_num / pull_den ns (k >= 1). */
typedef struct sim_device {
    uint32_t queue;
    uint64_t pulls;          /* pulls done */
    uint64_t pull_num, pull_den;
    uint32_t per_pull;
    uint32_t min_queue;      /* lowest level right after a pull */
    uint32_t max_queue;      /* highest level right after a mix */
} sim_device;

static uint64_t next_pull(const sim_device* d) { return (d->pulls + 1) * d->pull_num / d->pull_den; }

/* Advance the device to `t`; false if it ran dry (an underrun). */
static bool device_run_to(sim_device* d, uint64_t t)
{
    while (next_pull(d) <= t) {
        if (d->queue < d->per_pull) return false;
        d->queue -= d->per_pull;
        d->pulls++;
        if (d->queue < d->min_queue) d->min_queue = d->queue;
    }
    return true;
}

typedef struct sim_result {
    uint64_t mixes, skips, reanchors, now;
    uint64_t last_mix, max_interval, min_interval;
    uint64_t first_irregular_after;   /* last mix index whose interval was not one period */
} sim_result;

/* Run until `mixes` blocks are mixed; mix intervals are recorded from mix `from` on. */
static bool simulate(sim_device* d, uint64_t mixes, uint64_t from, sim_result* r)
{
    audio_pace_state p;
    audio_pace_init(&p, 0);
    memset(r, 0, sizeof *r);
    r->min_interval = UINT64_MAX;
    uint64_t guard = mixes * 8 + 1000;
    while (r->mixes < mixes) {
        CHECK(guard-- > 0, "no progress after %" PRIu64 " mixes", r->mixes);
        const audio_pace_decision dec = audio_pace_decide(&p, r->now, d->queue, true);
        if (dec.reanchored) r->reanchors++;
        if (dec.action == AUDIO_PACE_ACTION_SLEEP) {
            CHECK(dec.deadline_ns > r->now, "sleep to a past deadline");
            CHECK(device_run_to(d, dec.deadline_ns), "device ran dry at %" PRIu64 " ns", dec.deadline_ns);
            r->now = dec.deadline_ns;
            continue;
        }
        if (dec.action == AUDIO_PACE_ACTION_SKIP) { r->skips++; continue; }
        if (r->mixes >= from && r->mixes > 0) {
            const uint64_t iv = r->now - r->last_mix;
            if (iv > r->max_interval) r->max_interval = iv;
            if (iv < r->min_interval) r->min_interval = iv;
        }
        if (r->mixes > 0 && r->now - r->last_mix != 5333333 && r->now - r->last_mix != 5333334)
            r->first_irregular_after = r->mixes;
        r->last_mix = r->now;
        r->mixes++;
        d->queue++;
        if (d->queue > d->max_queue) d->max_queue = d->queue;
    }
    return true;
}

static bool test_steady(void)
{
    sim_device d = { .queue = 4, .pull_num = 2 * AUDIO_PACE_PERIOD_NUM_NS, .pull_den = AUDIO_PACE_PERIOD_DENOM,
                     .per_pull = 2, .min_queue = 4 };
    sim_result r;
    if (!simulate(&d, 1000001, 1, &r)) return false;
    CHECK(r.min_interval >= 5333333 && r.max_interval <= 5333334,
          "mix interval %" PRIu64 "..%" PRIu64 " ns, want one period", r.min_interval, r.max_interval);
    CHECK(r.reanchors == 0 && r.skips == 0, "%" PRIu64 " restarts, %" PRIu64 " skips", r.reanchors, r.skips);
    /* Mix 0 is at t = 0, mix 1e6 at exactly 1e6 periods: 187.5 blocks/s. */
    CHECK(r.last_mix == period_end(1000000), "mix 1e6 at %" PRIu64 " ns, want %" PRIu64, r.last_mix,
          period_end(1000000));
    return true;
}

static bool test_cold(void)
{
    sim_device d = { .queue = 0, .pull_num = 2 * AUDIO_PACE_PERIOD_NUM_NS, .pull_den = AUDIO_PACE_PERIOD_DENOM,
                     .per_pull = 2, .min_queue = 0 };
    sim_result r;
    if (!simulate(&d, 20000, 0, &r)) return false;
    CHECK(r.first_irregular_after < 16, "an irregular mix interval at mix %" PRIu64, r.first_irregular_after);
    CHECK(d.max_queue <= AUDIO_PACE_QUEUE_HI_BLOCKS, "queue reached %u", d.max_queue);
    CHECK(r.skips == 0, "%" PRIu64 " skips", r.skips);
    return true;
}

static bool test_slower(void)
{
    /* One block per pull at 187.5 * 0.999 blocks/s: every 16e9/2997 ns. */
    sim_device d = { .queue = 3, .pull_num = 16000000000ULL, .pull_den = 2997, .per_pull = 1, .min_queue = 3 };
    sim_result r;
    if (!simulate(&d, 20000, 0, &r)) return false;
    CHECK(d.max_queue <= AUDIO_PACE_QUEUE_HI_BLOCKS, "queue reached %u", d.max_queue);
    /* 20000 mixes at 0.1 %: about 20 periods' worth of surplus to skip. */
    CHECK(r.skips >= 15 && r.skips <= 25, "%" PRIu64 " skips, want about 20", r.skips);
    return true;
}

static bool faster(uint32_t per_pull)
{
    /* `per_pull` blocks per pull at 187.5 * 1.001 blocks/s. simulate() fails on
     * the first pull that finds too few blocks queued (an underrun). */
    sim_device d = { .queue = 3, .pull_num = per_pull * 16000000000ULL, .pull_den = 3003, .per_pull = per_pull,
                     .min_queue = 3 };
    sim_result r;
    if (!simulate(&d, 20000, 0, &r)) return false;
    CHECK(r.reanchors >= 5, "%" PRIu64 " catch-ups, want the deficit made up", r.reanchors);
    CHECK(r.skips == 0, "%" PRIu64 " skips", r.skips);
    CHECK(d.max_queue <= AUDIO_PACE_QUEUE_HI_BLOCKS, "queue reached %u", d.max_queue);
    return true;
}

static bool test_faster(void) { return faster(1); }
static bool test_faster_pairs(void) { return faster(2); }

/* The guards act exactly at their thresholds: LO-1 blocks before a deadline
 * mixes now, LO sleeps; HI-1 at a deadline mixes, HI skips. */
static bool test_bounds(void)
{
    audio_pace_state p;
    audio_pace_init(&p, 0);
    audio_pace_decision dec = audio_pace_decide(&p, 0, AUDIO_PACE_QUEUE_LO_BLOCKS, true);
    CHECK(dec.action == AUDIO_PACE_ACTION_MIX, "first period: action %d", (int)dec.action);
    const uint64_t mid = period_end(1) / 2;
    audio_pace_state q = p;
    dec = audio_pace_decide(&q, mid, AUDIO_PACE_QUEUE_LO_BLOCKS, true);
    CHECK(dec.action == AUDIO_PACE_ACTION_SLEEP && dec.deadline_ns == period_end(1),
          "LO blocks before the deadline: action %d to %" PRIu64, (int)dec.action, dec.deadline_ns);
    q = p;
    dec = audio_pace_decide(&q, mid, AUDIO_PACE_QUEUE_LO_BLOCKS - 1, true);
    CHECK(dec.action == AUDIO_PACE_ACTION_MIX && dec.reanchored, "LO-1 blocks: action %d", (int)dec.action);
    CHECK(audio_pace_deadline(&q) == mid + period_end(1), "cadence after a catch-up restarts at now");
    q = p;
    dec = audio_pace_decide(&q, period_end(1), AUDIO_PACE_QUEUE_HI_BLOCKS - 1, true);
    CHECK(dec.action == AUDIO_PACE_ACTION_MIX, "HI-1 blocks at the deadline: action %d", (int)dec.action);
    q = p;
    dec = audio_pace_decide(&q, period_end(1), AUDIO_PACE_QUEUE_HI_BLOCKS, true);
    CHECK(dec.action == AUDIO_PACE_ACTION_SKIP, "HI blocks at the deadline: action %d", (int)dec.action);
    CHECK(audio_pace_deadline(&q) == period_end(2), "a skip moves on by one period");
    return true;
}

static bool test_stall(void)
{
    audio_pace_state p;
    audio_pace_init(&p, 0);
    uint64_t now = 0;
    for (int mixed = 0; mixed < 10;) {
        const audio_pace_decision dec = audio_pace_decide(&p, now, 3, true);
        if (dec.action == AUDIO_PACE_ACTION_SLEEP) now = dec.deadline_ns;
        else { CHECK(dec.action == AUDIO_PACE_ACTION_MIX, "action %d", (int)dec.action); mixed++; }
    }
    now += 50000000;   /* the host stalls 50 ms; the queue still holds 3 blocks */
    unsigned burst = 0;
    const uint64_t restarts = p.reanchor_count;
    for (;;) {
        const audio_pace_decision dec = audio_pace_decide(&p, now, 3, true);
        if (dec.action == AUDIO_PACE_ACTION_SLEEP) break;
        CHECK(++burst <= 2, "burst of %u mixes after a stall", burst);
    }
    CHECK(p.reanchor_count - restarts == 1, "%" PRIu64 " restarts", p.reanchor_count - restarts);
    CHECK(burst == 1, "%u mixes at the restart, want 1", burst);
    return true;
}

static bool test_nodevice(void)
{
    audio_pace_state p;
    audio_pace_init(&p, 0);
    uint64_t now = 0, mixes = 0;
    while (mixes <= 1000) {
        /* A queue reading would be ignored without a device: pass HI to prove it. */
        const audio_pace_decision dec = audio_pace_decide(&p, now, AUDIO_PACE_QUEUE_HI_BLOCKS, false);
        if (dec.action == AUDIO_PACE_ACTION_SLEEP) { now = dec.deadline_ns; continue; }
        CHECK(dec.action == AUDIO_PACE_ACTION_MIX, "action %d without a device", (int)dec.action);
        CHECK(now == period_end(mixes), "mix %" PRIu64 " at %" PRIu64 " ns", mixes, now);
        mixes++;
    }
    return true;
}

static int cmp_u64(const void* a, const void* b)
{
    const uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y;
}

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int smoke(void)
{
    enum { N = 2000 };
    static uint64_t late[N];
    audio_pace_state p;
    const uint64_t t0 = mono_ns();
    audio_pace_init(&p, t0);
    int n = 0;
    while (n < N) {
        uint64_t now = mono_ns();
        const audio_pace_decision dec = audio_pace_decide(&p, now, 3, true);
        if (dec.action != AUDIO_PACE_ACTION_SLEEP) continue;
#if defined(__linux__)
        const struct timespec req = { (time_t)(dec.deadline_ns / 1000000000ull),
                                      (long)(dec.deadline_ns % 1000000000ull) };
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &req, NULL) == EINTR) {}
#else
        /* macOS has no clock_nanosleep and coalesces timers: wake-ns sleeps are
         * longer than asked, and the deadline arithmetic makes that up. */
        const struct timespec req = { (time_t)(dec.sleep_ns / 1000000000ull),
                                      (long)(dec.sleep_ns % 1000000000ull) };
        nanosleep(&req, NULL);
#endif
        now = mono_ns();
        late[n++] = now > dec.deadline_ns ? now - dec.deadline_ns : 0;
    }
    const double s = (double)(mono_ns() - t0) / 1e9;
    qsort(late, N, sizeof late[0], cmp_u64);
    printf("%d periods in %.4f s: %.3f blocks/s; wake late p50 %.1f us, p90 %.1f us, p99 %.1f us, max %.1f us\n",
           N, s, N / s, late[N / 2] / 1e3, late[N * 9 / 10] / 1e3, late[N * 99 / 100] / 1e3, late[N - 1] / 1e3);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc > 1 && strcmp(argv[1], "smoke") == 0) return smoke();
    struct { const char* name; bool (*fn)(void); } cases[] = {
        { "steady", test_steady }, { "cold", test_cold }, { "slower", test_slower },
        { "faster", test_faster }, { "faster_pairs", test_faster_pairs }, { "stall", test_stall },
        { "nodevice", test_nodevice }, { "bounds", test_bounds },
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const bool ok = cases[i].fn();
        printf("%s %s\n", ok ? "ok  " : "FAIL", cases[i].name);
        failed += !ok;
    }
    return failed ? 1 : 0;
}
