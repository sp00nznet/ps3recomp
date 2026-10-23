/* The guest clocks on POSIX hosts come from one host clock, host_monotonic_ns
 * (runtime/syscalls/sys_timer.c): the PPU timebase (every lifted mftb),
 * sys_time_get_system_time and the lv2 timers' expiration times. On macOS it
 * is mach_absolute_time, which costs a fraction of clock_gettime there.
 *
 * Checks that it is monotonic, runs at the rate of CLOCK_MONOTONIC, and that
 * the timebase and the timers' microsecond clock read the same clock (a
 * reading of each falls between two readings of host_monotonic_ns). Prints
 * what a read costs, for information.
 *
 * tools/test_host_monotonic_clock.py builds and runs it.
 */
#include "../runtime/syscalls/sys_timer.c"
#include <assert.h>
#include <stdio.h>
#include <time.h>

uint8_t* vm_base;

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t to_tb(uint64_t ns)
{
    return (ns / 1000000000ull) * PS3_TIMEBASE_FREQ +
           (ns % 1000000000ull) * PS3_TIMEBASE_FREQ / 1000000000ull;
}

int main(void)
{
    /* monotonic, read after read */
    uint64_t prev = host_monotonic_ns();
    for (int i = 0; i < 1000000; i++) {
        const uint64_t now = host_monotonic_ns();
        assert(now >= prev);
        prev = now;
    }

    /* the same rate as CLOCK_MONOTONIC: over a 200 ms sleep the two agree
     * to well under a millisecond (the two reads are not simultaneous) */
    const uint64_t h0 = host_monotonic_ns(), m0 = mono_ns();
    struct timespec nap = {0, 200 * 1000 * 1000};
    nanosleep(&nap, NULL);
    const uint64_t h1 = host_monotonic_ns(), m1 = mono_ns();
    const int64_t drift = (int64_t)(h1 - h0) - (int64_t)(m1 - m0);
    printf("200 ms sleep: host clock %.3f ms, CLOCK_MONOTONIC %.3f ms\n",
           (h1 - h0) / 1e6, (m1 - m0) / 1e6);
    assert(h1 - h0 >= 200000000ull);
    assert(drift < 1000000 && drift > -1000000);

    /* the timebase and the timers' clock read host_monotonic_ns */
    for (int i = 0; i < 1000; i++) {
        const uint64_t a = host_monotonic_ns();
        const uint64_t tb = ppu_timebase_now();
        const uint64_t us = timer_now_us();
        const uint64_t b = host_monotonic_ns();
        assert(tb >= to_tb(a) && tb <= to_tb(b));
        assert(us >= a / 1000ull && us <= b / 1000ull);
    }

    /* cost per read, for the log */
    enum { N = 2000000 };
    uint64_t sink = 0;
    uint64_t t0 = mono_ns();
    for (int i = 0; i < N; i++) sink += host_monotonic_ns();
    const uint64_t t1 = mono_ns();
    for (int i = 0; i < N; i++) sink += mono_ns();
    const uint64_t t2 = mono_ns();
    printf("per read: host_monotonic_ns %.1f ns, clock_gettime(CLOCK_MONOTONIC) %.1f ns (%llu)\n",
           (double)(t1 - t0) / N, (double)(t2 - t1) / N, (unsigned long long)(sink & 1));

    puts("PASS: the timebase and the timers read one monotonic host clock at CLOCK_MONOTONIC's rate");
    return 0;
}
