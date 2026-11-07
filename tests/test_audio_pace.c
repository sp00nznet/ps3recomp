/* The POSIX cellAudio clock pacing (libs/audio/audio_pace.h): 256 samples at
 * 48 kHz is one block per 5333 us, so 10 s of simulated monotonic time must
 * mix 1875 blocks, not 2000-5000. Also checks the stall re-anchor. */
#include "../libs/audio/audio_pace.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    /* 10 s of 1 ms ticks: count how many times the pace says "mix now". */
    audio_pace p;
    audio_pace_start(&p, 0);
    unsigned long long blocks = 0;
    for (long long now = 0; now <= 10000000000ll; now += 1000000ll) {
        long long sleep_ns = 0;
        if (audio_pace_ready(&p, now, AUDIO_PACE_PERIOD_NS, &sleep_ns)) {
            blocks++;
            audio_pace_next(&p);
        } else assert(sleep_ns > 0);
    }
    printf("blocks in 10 s: %llu\n", blocks);
    assert(blocks >= 1874 && blocks <= 1876);   /* 1875 ideal */

    /* A 1 s stall does not accumulate a burst: one block now, then the origin
     * is re-anchored so the next deadline is ~5.333 ms later. */
    audio_pace_start(&p, 0);
    long long sleep_ns = 0;
    assert(audio_pace_ready(&p, 0, AUDIO_PACE_PERIOD_NS, &sleep_ns));
    audio_pace_next(&p);
    assert(audio_pace_ready(&p, 1000000000ll, AUDIO_PACE_PERIOD_NS, &sleep_ns) == 1);
    audio_pace_next(&p);   /* that block is mixed; origin re-anchored to now - 1 period */
    /* The next deadline is one period after the late block, not a bundled catch-up. */
    assert(audio_pace_ready(&p, 1003000000ll, AUDIO_PACE_PERIOD_NS, &sleep_ns) == 0);
    assert(sleep_ns > 0);
    puts("PASS: audio pacing is 187.5 blocks/s on the monotonic clock");
    return 0;
}
