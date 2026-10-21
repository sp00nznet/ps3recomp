/*
 * audio_grace.h - bounded audio-grace policy (portable C11, header-only).
 *
 * Decides whether a host-side audio controller may wait briefly (a fixed
 * 3000 us budget) for an in-flight block before continuing. The policy is
 * pure: no blocking, allocation, platform API, global state, or I/O.
 *
 * Caveats for the controller using this policy:
 *  - "audible" (nonzero PCM) is a heuristic, not proof that a block was
 *    completed; treat it only as a hint that waiting may be worthwhile.
 *  - After any wait the controller must reacquire and revalidate its ports
 *    and re-sample device padding; nothing observed before the wait is
 *    assumed to still hold.
 *  - One shared wait budget covers all ports for a given pass; do not grant
 *    a separate budget per port.
 *  - This policy never changes the published read index; that remains the
 *    controller's responsibility.
 *
 * A zero-initialized AudioGraceState is valid.
 */
#ifndef AUDIO_GRACE_H
#define AUDIO_GRACE_H

#include <stdint.h>

#define AUDIO_GRACE_BUDGET_US 3000u
#define AUDIO_GRACE_MAX_WAITS 2u
#define AUDIO_GRACE_MIN_PADDING_MS 16u

typedef struct AudioGraceState {
    unsigned previous_audible;
    unsigned consecutive_waits;
} AudioGraceState;

/* Returns AUDIO_GRACE_BUDGET_US when a wait is allowed, otherwise 0.
 * Padding must cover at least ceil(sample_rate * 16 / 1000) frames. */
static inline unsigned audio_grace_budget_us(const AudioGraceState *s,
                                             int enabled,
                                             unsigned padding_frames,
                                             unsigned sample_rate)
{
    uint64_t min_frames;

    if (!enabled || s == NULL || !s->previous_audible ||
        s->consecutive_waits >= AUDIO_GRACE_MAX_WAITS || sample_rate == 0)
        return 0;

    min_frames = ((uint64_t)sample_rate * AUDIO_GRACE_MIN_PADDING_MS + 999u) / 1000u;
    if ((uint64_t)padding_frames < min_frames)
        return 0;

    return AUDIO_GRACE_BUDGET_US;
}

/* True only while elapsed time is within the budget. A clock that moved
 * backward (including wrap) is rejected. */
static inline int audio_grace_before_deadline(uint64_t start_us,
                                              uint64_t now_us,
                                              unsigned budget_us)
{
    if (budget_us == 0 || now_us < start_us)
        return 0;
    return (now_us - start_us) < (uint64_t)budget_us;
}

/* Records the outcome of a pass. Null state is ignored. */
static inline void audio_grace_finish(AudioGraceState *s, int audible, int waited)
{
    if (s == NULL)
        return;

    s->previous_audible = audible != 0;
    if (waited) {
        if (s->consecutive_waits < AUDIO_GRACE_MAX_WAITS)
            s->consecutive_waits++;
    } else {
        s->consecutive_waits = 0;
    }
}

#endif /* AUDIO_GRACE_H */
