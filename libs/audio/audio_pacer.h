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

/* Standing device fill. A shared-mode engine pulls a whole period (10 ms =
 * 480 frames on most endpoints, sometimes two at once) per pass. Holding only
 * the two primed blocks (512 frames) left about one pass of margin: any late
 * wake or double pull ran the stream dry and the engine played zeros (crackle).
 * Hold two periods plus two blocks instead, and steer toward that level. */
static inline uint32_t audio_fill_target(uint32_t period_frames, uint32_t buffer_frames)
{
    uint32_t target = period_frames * 2 + 256 * 2;
    if (target + 256 * 2 > buffer_frames)
        target = buffer_frames > 256 * 4 ? buffer_frames - 256 * 2 : buffer_frames / 2;
    return target;
}

/* Device-clock trim. The endpoint consumes on its own 48 kHz crystal, which
 * differs from the performance counter by tens to hundreds of ppm; on the QPC
 * alone the fill drifts until it runs dry or overflows. Average the fill seen
 * before each block (about 1.4 s) and move the deadline epoch by at most
 * 1/1000 of a block per block (1000 ppm) toward the target. Samples are never
 * altered or skipped: the guest mixer clock follows the device that plays it,
 * as on the console, and the long-run rate equals the device's exactly. */
typedef struct audio_fill {
    int64_t avg_q8;     /* smoothed fill, frames * 256 */
    uint32_t target;
    int have_avg;
    int64_t trim_ticks; /* net epoch movement so far (diagnostic) */
} audio_fill;

static inline int64_t audio_fill_observe(audio_fill* f, audio_pacer* p, uint32_t padding)
{
    const int64_t sample = (int64_t)padding * 256;
    if (!f->have_avg) { f->avg_q8 = sample; f->have_avg = 1; }
    else f->avg_q8 += (sample - f->avg_q8) / 256;
    const int64_t block_ticks = (int64_t)(p->frequency * 256 / 48000);
    const int64_t max_ticks = block_ticks / 1000;
    const int64_t error_q8 = f->avg_q8 - (int64_t)f->target * 256; /* + : too full */
    int64_t adjust = error_q8 / 256 * max_ticks / 256;            /* full rate at 256 frames off */
    if (adjust > max_ticks) adjust = max_ticks;
    if (adjust < -max_ticks) adjust = -max_ticks;
    p->epoch = (uint64_t)((int64_t)p->epoch + adjust);
    f->trim_ticks += adjust;
    return adjust;
}
#endif
