#ifndef PAD_MOTION_G7_H
#define PAD_MOTION_G7_H
#include "g7_motion_backend.h"
/* RPCS3 sdl_pad_handler.cpp: DS3 neutral 512, accelerometer 113 counts/g.
 * Restart recordings: +Y gravity at rest; front-up gives -Z, right-up +X.
 * Measured flat gravity magnitude is 8181.7 raw counts. No sign inversion
 * beyond the documented SDL-to-DS3 conversion, and no artificial flick pulse.
 * https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Input/sdl_pad_handler.cpp */
static inline void pad_motion_g7_map(const g7_motion_sample *raw, uint16_t out[4]) {
    for (int axis = 0; axis < 3; ++axis) {
        int value = (int)(512.0f - (float)raw->accel[axis] * (113.0f / 8182.0f));
        out[axis] = (uint16_t)(value < 0 ? 0 : value > 1023 ? 1023 : value);
    }
    /* Yaw sign/scale is not established by the recordings. Advertise/use
     * accelerometer motion only; never invent gyro rates from raw counts. */
    out[3] = 512;
}
#endif
