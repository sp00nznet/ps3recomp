/* Optional accessibility mapping: right-stick flick (with R1 held) to a
 * synthetic accelerometer pulse. Pure, deterministic, caller-clocked.
 * Waveform values are calibration candidates only. */
#ifndef PAD_STICK_MOTION_H
#define PAD_STICK_MOTION_H

#include <stdint.h>

#define PSM_CENTER 512u
#define PSM_REST_Y 399u
#define PSM_PULSE_HI_Y 60u
#define PSM_PULSE_LO_Y 738u
#define PSM_PHASE1_MS 80u
#define PSM_PULSE_MS 160u
#define PSM_ARM_MS 250u
#define PSM_NEUTRAL_MIN 96u
#define PSM_NEUTRAL_MAX 160u
#define PSM_TRIGGER_MAX 32u

typedef struct PadStickMotion {
    int has_time;
    uint64_t last_ms;
    int armed;
    uint64_t arm_ms;
    int firing;
    uint64_t fire_ms;
    int need_neutral;
} PadStickMotion;

static inline void pad_stick_motion_reset(PadStickMotion *s)
{
    if (s)
        *s = (PadStickMotion){0};
}

static inline void psm_set_(uint16_t *o, unsigned y)
{
    o[0] = PSM_CENTER;
    o[1] = (uint16_t)y;
    o[2] = PSM_CENTER;
    o[3] = PSM_CENTER;
}

static inline int pad_stick_motion_update(PadStickMotion *s, int active, int sensor_on, int hold_r1,
                                          unsigned stick_y, uint64_t now_ms, uint16_t sample[4])
{
    if (!s || !sample || (unsigned)active > 1u || (unsigned)sensor_on > 1u ||
        (unsigned)hold_r1 > 1u || stick_y > 255u || !active || !sensor_on) {
        if (sample)
            sample[0] = sample[1] = sample[2] = sample[3] = 0;
        pad_stick_motion_reset(s);
        return 0;
    }
    if (s->has_time && now_ms < s->last_ms) {
        pad_stick_motion_reset(s);
        s->has_time = 1;
        s->last_ms = now_ms;
        psm_set_(sample, PSM_REST_Y);
        return 1;
    }
    s->has_time = 1;
    s->last_ms = now_ms;

    if (!hold_r1) {
        s->armed = s->firing = s->need_neutral = 0;
        psm_set_(sample, PSM_REST_Y);
        return 1;
    }
    if (s->firing) {
        uint64_t e = now_ms - s->fire_ms; /* fire_ms <= now_ms: regression handled above */
        if (e < PSM_PHASE1_MS) {
            psm_set_(sample, PSM_PULSE_HI_Y);
            return 1;
        }
        if (e < PSM_PULSE_MS) {
            psm_set_(sample, PSM_PULSE_LO_Y);
            return 1;
        }
        s->firing = 0;
    }
    if (stick_y >= PSM_NEUTRAL_MIN && stick_y <= PSM_NEUTRAL_MAX) {
        s->need_neutral = 0;
        s->armed = 1;
        s->arm_ms = now_ms;
    } else {
        if (s->need_neutral || stick_y > PSM_NEUTRAL_MAX ||
            (s->armed && now_ms - s->arm_ms > PSM_ARM_MS))
            s->armed = 0;
        if (s->armed && stick_y <= PSM_TRIGGER_MAX) {
            s->armed = 0;
            s->firing = 1;
            s->fire_ms = now_ms;
            s->need_neutral = 1;
            psm_set_(sample, PSM_PULSE_HI_Y);
            return 1;
        }
    }
    psm_set_(sample, PSM_REST_Y);
    return 1;
}

#endif
