/*
 * ps3recomp - cellAudio mixer pacing decision (pure, host-independent)
 *
 * The PS3 mixes one block of 256 samples at 48 kHz per period on a fixed clock:
 * 16,000,000 / 3 ns (5.333 ms), 187.5 blocks per second, one guest event each.
 * The mixer keeps that cadence on the host's monotonic clock and uses the host
 * device's queue only as a guard against the drift between the device's clock
 * and the host's. Pacing on the queue alone paces on the device's pulls, and a
 * device that pulls two blocks at once (macOS SDL) then turns the guest's
 * steady events into pairs 10.7 ms apart.
 */

#ifndef PS3RECOMP_AUDIO_PACE_H
#define PS3RECOMP_AUDIO_PACE_H

#include <stdbool.h>
#include <stdint.h>

/* Deadline n is anchor + n * NUM / DENOM: computed from n, so 1e6 periods carry
 * no rounding drift (a fixed 5,333,333 ns step loses 1/3 ns per period). */
#define AUDIO_PACE_PERIOD_NUM_NS    16000000ULL
#define AUDIO_PACE_PERIOD_DENOM     3ULL

/* At a deadline with this many blocks queued, skip the period: the device's
 * clock runs slower than the host's. Six blocks bound the added latency at 32 ms. */
#define AUDIO_PACE_QUEUE_HI_BLOCKS  6U

/* Before a deadline with fewer blocks queued, mix now and restart the cadence:
 * the device's clock runs faster than the host's, or it just started. Two blocks
 * cover a device that pulls two at a time. */
#define AUDIO_PACE_QUEUE_LO_BLOCKS  2U

/* More than this many periods late (a host stall, a debugger stop), restart the
 * cadence at now instead of mixing the backlog as a burst of guest events. */
#define AUDIO_PACE_STALL_PERIODS    4U

typedef enum audio_pace_action {
    AUDIO_PACE_ACTION_MIX,   /* mix one block now */
    AUDIO_PACE_ACTION_SLEEP, /* sleep until deadline_ns, then decide again */
    AUDIO_PACE_ACTION_SKIP,  /* no block this period (queue at HI) */
} audio_pace_action;

typedef struct audio_pace_state {
    uint64_t anchor_ns;
    uint64_t period_idx;
    uint64_t reanchor_count;
} audio_pace_state;

typedef struct audio_pace_decision {
    audio_pace_action action;
    uint64_t deadline_ns;   /* when action == SLEEP */
    uint64_t sleep_ns;      /* deadline_ns - now_ns when action == SLEEP */
    bool reanchored;        /* the cadence restarted at now during this call */
} audio_pace_decision;

static inline uint64_t audio_pace_deadline(const audio_pace_state* state)
{
    return state->anchor_ns + (state->period_idx * AUDIO_PACE_PERIOD_NUM_NS) / AUDIO_PACE_PERIOD_DENOM;
}

static inline void audio_pace_init(audio_pace_state* state, uint64_t now_ns)
{
    state->anchor_ns = now_ns;
    state->period_idx = 0;
    state->reanchor_count = 0;
}

static inline audio_pace_decision audio_pace_decide(audio_pace_state* state, uint64_t now_ns,
                                                    uint32_t queued_blocks, bool has_device)
{
    audio_pace_decision dec;
    dec.action = AUDIO_PACE_ACTION_MIX;
    dec.deadline_ns = 0;
    dec.sleep_ns = 0;
    dec.reanchored = false;

    uint64_t deadline = audio_pace_deadline(state);
    const uint64_t stall_ns = (AUDIO_PACE_STALL_PERIODS * AUDIO_PACE_PERIOD_NUM_NS) / AUDIO_PACE_PERIOD_DENOM;

    if (now_ns > deadline && now_ns - deadline > stall_ns) {
        state->anchor_ns = now_ns;
        state->period_idx = 0;
        state->reanchor_count++;
        dec.reanchored = true;
        deadline = now_ns;
    }

    if (has_device && now_ns < deadline && queued_blocks < AUDIO_PACE_QUEUE_LO_BLOCKS) {
        if (state->anchor_ns != now_ns) {
            state->anchor_ns = now_ns;
            state->reanchor_count++;
            dec.reanchored = true;
        }
        state->period_idx = 1;
        return dec;
    }

    if (now_ns < deadline) {
        dec.action = AUDIO_PACE_ACTION_SLEEP;
        dec.deadline_ns = deadline;
        dec.sleep_ns = deadline - now_ns;
        return dec;
    }

    state->period_idx++;
    if (has_device && queued_blocks >= AUDIO_PACE_QUEUE_HI_BLOCKS)
        dec.action = AUDIO_PACE_ACTION_SKIP;
    return dec;
}

#endif /* PS3RECOMP_AUDIO_PACE_H */
