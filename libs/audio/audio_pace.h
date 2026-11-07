/*
 * ps3recomp - monotonic pacing for the POSIX cellAudio mixing thread.
 *
 * The audio block is 256 samples at 48 kHz: one block every 5333 us
 * (187.5 blocks/s). The old POSIX loop slept 2 ms or 5 ms after every block
 * instead, so it could mix far faster and publish that read index to the
 * guest producer.
 *
 * Header-only so tests/test_audio_pace.c drives the same arithmetic the
 * mixing thread uses. `now_ns` is CLOCK_MONOTONIC nanoseconds.
 *
 * The deadline is relative to an origin and the block count, so a late wake-up
 * does not accumulate drift. A gap longer than 50 ms (debugger stop, backend
 * gone) re-anchors the origin instead of mixing a burst of backlog blocks.
 */
#ifndef PS3RECOMP_AUDIO_PACE_H
#define PS3RECOMP_AUDIO_PACE_H

#include <stdint.h>

#define AUDIO_PACE_PERIOD_NS 5333333ll
#define AUDIO_PACE_RESYNC_NS 50000000ll

typedef struct {
    long long origin_ns;
    unsigned long long blocks;
} audio_pace;

static inline void audio_pace_start(audio_pace* p, long long now_ns)
{
    p->origin_ns = now_ns;
    p->blocks = 0;
}

/* 1 = mix now; 0 = not yet, `*sleep_ns` until the next deadline. */
static inline int audio_pace_ready(audio_pace* p, long long now_ns,
                                   long long period_ns, long long* sleep_ns)
{
    const long long due = p->origin_ns + (long long)p->blocks * period_ns;
    if (now_ns < due) {
        *sleep_ns = due - now_ns;
        return 0;
    }
    if (now_ns - due > AUDIO_PACE_RESYNC_NS)
        p->origin_ns = now_ns - (long long)p->blocks * period_ns;
    *sleep_ns = 0;
    return 1;
}

static inline void audio_pace_next(audio_pace* p)
{
    p->blocks++;
}

#endif /* PS3RECOMP_AUDIO_PACE_H */
