#ifndef PS3RECOMP_AUDIO_PACER_H
#define PS3RECOMP_AUDIO_PACER_H
#include <stdint.h>

/* Guest notifications follow 256 frames / 48 kHz, independently of the host
 * device's (often 480-frame) processing quantum. Deadline accumulation is
 * rational: rounding 5333 us each block loses time. A delayed wake may catch
 * up at most two blocks per period, never consume a burst at one timestamp.
 * This controls production only; submitted PCM remains unmodified 48 kHz. */
typedef struct audio_pacer {
    uint64_t epoch, frequency, blocks, last, rebases;
    int have_last;
} audio_pacer;

static inline void audio_pacer_init(audio_pacer* p, uint64_t now, uint64_t frequency)
{
    *p = (audio_pacer){.epoch = now, .frequency = frequency};
}

static inline uint64_t audio_pacer_offset(const audio_pacer* p)
{
    /* Division first for long-running sessions, retaining the remainder. */
    uint64_t frames = p->blocks * 256;
    return (frames / 48000) * p->frequency +
           (frames % 48000) * p->frequency / 48000;
}

static inline int audio_pacer_ready(audio_pacer* p, uint64_t now)
{
    uint64_t offset = audio_pacer_offset(p);
    uint64_t due = p->epoch + offset;
    if (now < due) return 0;
    if (p->have_last && now - p->last < p->frequency * 128 / 48000)
        return 0;
    /* Suspension cannot be repaired by advancing guest rings at CPU speed.
     * Report the rebase; output starvation remains visible to diagnostics. */
    if (now - due > p->frequency / 20) {
        p->epoch = now - offset;
        ++p->rebases;
    }
    return 1;
}

static inline uint64_t audio_pacer_deadline(const audio_pacer* p)
{
    uint64_t due = p->epoch + audio_pacer_offset(p);
    uint64_t minimum = p->last + p->frequency * 128 / 48000;
    return p->have_last && minimum > due ? minimum : due;
}

static inline void audio_pacer_consumed(audio_pacer* p, uint64_t now)
{
    ++p->blocks;
    p->last = now;
    p->have_last = 1;
}
#endif
