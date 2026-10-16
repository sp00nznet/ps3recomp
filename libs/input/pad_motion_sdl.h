/*
 * pad_motion_sdl.h - SDL2 controller accelerometer/gyroscope adapter for cellPad.
 *
 * Conversion semantics follow RPCS3 rpcs3/Input/sdl_pad_handler.cpp at revision
 * 4d5def5880217135554ed434cd5055cceebe9cc2 (GPL-2.0-only). The semantics are
 * reimplemented here; no upstream code is copied. The runtime's existing GPL
 * notice applies to this file.
 *
 * Only samples read successfully from an attached controller exposing both
 * SDL_SENSOR_ACCEL and SDL_SENSOR_GYRO are published. There is no neutral or
 * default fallback: on any failure valid is 0 and sample/timestamp are zero.
 *
 * No internal locking, threads, globals, allocations or clocks. The caller
 * serializes all calls (cellPad backend lock). PadMotionSDL is
 * zero-initializable.
 *
 * Host calls, overridable for tests (define before including this header):
 *   PAD_MOTION_ATTACHED    SDL_GameControllerGetAttached
 *   PAD_MOTION_HAS_SENSOR  SDL_GameControllerHasSensor
 *   PAD_MOTION_SET_ENABLED SDL_GameControllerSetSensorEnabled
 *   PAD_MOTION_IS_ENABLED  SDL_GameControllerIsSensorEnabled
 *   PAD_MOTION_GET_DATA    SDL_GameControllerGetSensorDataWithTimestamp
 * No other SDL function is called.
 *
 * sample[0..2]: accelerometer X/Y/Z, sample[3]: gyroscope Y; 0..1023, 512 centre.
 * timestamp: later of the two SDL sensor timestamps (SDL microseconds), or
 * zero when the driver supplies no timestamps. SDL explicitly allows zero:
 * https://wiki.libsdl.org/SDL2/SDL_GameControllerGetSensorDataWithTimestamp
 */
#ifndef PAD_MOTION_SDL_H
#define PAD_MOTION_SDL_H

#include <SDL2/SDL.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#if !SDL_VERSION_ATLEAST(2, 26, 0)
#error "pad_motion_sdl.h requires SDL2 >= 2.26 (SDL_GameControllerGetSensorDataWithTimestamp)"
#endif

#ifndef PAD_MOTION_ATTACHED
#define PAD_MOTION_ATTACHED SDL_GameControllerGetAttached
#endif
#ifndef PAD_MOTION_HAS_SENSOR
#define PAD_MOTION_HAS_SENSOR SDL_GameControllerHasSensor
#endif
#ifndef PAD_MOTION_SET_ENABLED
#define PAD_MOTION_SET_ENABLED SDL_GameControllerSetSensorEnabled
#endif
#ifndef PAD_MOTION_IS_ENABLED
#define PAD_MOTION_IS_ENABLED SDL_GameControllerIsSensorEnabled
#endif
#ifndef PAD_MOTION_GET_DATA
#define PAD_MOTION_GET_DATA SDL_GameControllerGetSensorDataWithTimestamp
#endif

/* Single-precision constants as in the reference f32 path and SDL's own
 * SDL_STANDARD_GRAVITY, so exactly 1 g or pi/2 rad/s reported as float maps to
 * whole counts instead of truncating just below them. */
#define PAD_MOTION_SDL_GRAVITY ((double)SDL_STANDARD_GRAVITY)
#define PAD_MOTION_SDL_PI ((double)3.14159265358979323846f)

typedef struct PadMotionSDL {
    int enabled, valid;
    uint16_t sample[4];
    uint64_t timestamp;
} PadMotionSDL;

static inline void pad_motion_sdl_clear(PadMotionSDL* state)
{
    state->valid = 0;
    memset(state->sample, 0, sizeof state->sample);
    state->timestamp = 0;
}

static inline void pad_motion_sdl_reset(PadMotionSDL* state)
{
    pad_motion_sdl_clear(state);
    state->enabled = 0;
}

/* Clamp to 0..1023 before conversion; the cast truncates toward zero. */
static inline uint16_t pad_motion_sdl_to_count(double v)
{
    if (v < 0.0)
        v = 0.0;
    if (v > 1023.0)
        v = 1023.0;
    return (uint16_t)v;
}

static inline int pad_motion_sdl_capable(SDL_GameController* gc)
{
    return gc && PAD_MOTION_ATTACHED(gc) &&
           PAD_MOTION_HAS_SENSOR(gc, SDL_SENSOR_ACCEL) &&
           PAD_MOTION_HAS_SENSOR(gc, SDL_SENSOR_GYRO);
}

static inline int pad_motion_sdl_set_enabled(PadMotionSDL* state, SDL_GameController* gc, int enabled)
{
    int accel_set, gyro_set;

    if (!state || (enabled != 0 && enabled != 1))
        return 0;

    if (!enabled) {
        int ok = 1;
        pad_motion_sdl_reset(state);
        if (pad_motion_sdl_capable(gc)) {
            /* Attempt both even if the first fails. */
            if (PAD_MOTION_SET_ENABLED(gc, SDL_SENSOR_ACCEL, SDL_FALSE) != 0)
                ok = 0;
            if (PAD_MOTION_SET_ENABLED(gc, SDL_SENSOR_GYRO, SDL_FALSE) != 0)
                ok = 0;
        }
        return ok;
    }

    if (!pad_motion_sdl_capable(gc)) {
        pad_motion_sdl_reset(state);
        return 0;
    }
    if (state->enabled && PAD_MOTION_IS_ENABLED(gc, SDL_SENSOR_ACCEL) &&
        PAD_MOTION_IS_ENABLED(gc, SDL_SENSOR_GYRO))
        return 1;

    pad_motion_sdl_reset(state);
    accel_set = PAD_MOTION_SET_ENABLED(gc, SDL_SENSOR_ACCEL, SDL_TRUE) == 0;
    gyro_set = accel_set && PAD_MOTION_SET_ENABLED(gc, SDL_SENSOR_GYRO, SDL_TRUE) == 0;
    if (gyro_set && PAD_MOTION_IS_ENABLED(gc, SDL_SENSOR_ACCEL) &&
        PAD_MOTION_IS_ENABLED(gc, SDL_SENSOR_GYRO)) {
        state->enabled = 1;
        return 1;
    }

    /* Roll back only the sensors this call enabled. */
    if (gyro_set)
        (void)PAD_MOTION_SET_ENABLED(gc, SDL_SENSOR_GYRO, SDL_FALSE);
    if (accel_set)
        (void)PAD_MOTION_SET_ENABLED(gc, SDL_SENSOR_ACCEL, SDL_FALSE);
    return 0;
}

static inline int pad_motion_sdl_poll(PadMotionSDL* state, SDL_GameController* gc)
{
    Uint64 accel_ts = 0, gyro_ts = 0;
    float accel[3], gyro[3];
    uint16_t sample[4];
    int i;

    if (!state)
        return 0;
    pad_motion_sdl_clear(state);
    if (!state->enabled)
        return 0;
    if (!pad_motion_sdl_capable(gc)) {
        state->enabled = 0;
        return 0;
    }
    if (!PAD_MOTION_IS_ENABLED(gc, SDL_SENSOR_ACCEL) || !PAD_MOTION_IS_ENABLED(gc, SDL_SENSOR_GYRO))
        return 0;

    /* The gyro is not read if the accelerometer read fails. */
    if (PAD_MOTION_GET_DATA(gc, SDL_SENSOR_ACCEL, &accel_ts, accel, 3) != 0)
        return 0;
    if (PAD_MOTION_GET_DATA(gc, SDL_SENSOR_GYRO, &gyro_ts, gyro, 3) != 0)
        return 0;
    /* A successful read may have timestamp zero (SDL2-compat on Steam Deck).
     * Preserve that unavailable metadata; never manufacture a timestamp. */
    for (i = 0; i < 3; ++i)
        if (!isfinite(accel[i]) || !isfinite(gyro[i]))
            return 0;

    /* SDL accel is m/s^2 including gravity; gyro is rad/s. */
    for (i = 0; i < 3; ++i)
        sample[i] = pad_motion_sdl_to_count(512.0 - (double)accel[i] * 113.0 / PAD_MOTION_SDL_GRAVITY);
    sample[3] = pad_motion_sdl_to_count(512.0 + (double)gyro[1] * 246.0 / PAD_MOTION_SDL_PI);

    memcpy(state->sample, sample, sizeof sample);
    state->timestamp = accel_ts > gyro_ts ? accel_ts : gyro_ts;
    state->valid = 1;
    return 1;
}

#endif /* PAD_MOTION_SDL_H */
