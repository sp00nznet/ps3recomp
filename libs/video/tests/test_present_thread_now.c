/* rsx_present_thread_now() exists on every platform and is a monotonic nanosecond clock.
 *
 * The Vulkan backend stamps its present timing with it, also where the present thread is unsupported (Windows),
 * and the header defined it in its POSIX half only: a Windows build with Vulkan stopped on an implicit declaration. */
#include <stdio.h>
#include <stdint.h>
#include "rsx_present_thread.h"

#ifdef _WIN32
#include <windows.h>
static void nap_ms(unsigned ms) { Sleep(ms); }
#else
#include <time.h>
static void nap_ms(unsigned ms)
{
    struct timespec t = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    nanosleep(&t, NULL);
}
#endif

int main(void)
{
    int fails = 0;
    uint64_t prev = rsx_present_thread_now();
    for (int i = 0; i < 1000; i++) {
        const uint64_t t = rsx_present_thread_now();
        if (t < prev) { fprintf(stderr, "FAIL: the clock went back (%llu -> %llu)\n",
                                (unsigned long long)prev, (unsigned long long)t); fails++; break; }
        prev = t;
    }
    const uint64_t t0 = rsx_present_thread_now();
    nap_ms(50);
    const uint64_t dt = rsx_present_thread_now() - t0;
    /* A 50 ms sleep is 50e6 ns: never less (a little slack for the timer), and far from seconds. */
    if (dt < 45000000ull || dt > 2000000000ull) {
        fprintf(stderr, "FAIL: a 50 ms sleep measured %llu ns\n", (unsigned long long)dt);
        fails++;
    }
    if (!fails) printf("ok: rsx_present_thread_now, 50 ms = %llu ns\n", (unsigned long long)dt);
    return fails ? 1 : 0;
}
