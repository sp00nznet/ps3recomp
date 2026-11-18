/*
 * ps3recomp - GameController profile -> PadHostState mapping (pure C17)
 *
 * A native GameController backend (Apple's GameController framework) reads a
 * controller's extended profile into a pad_gc_report and pad_gc_map turns that
 * into the state cellPad hands to the guest. No framework headers are
 * involved, so every host compiles it and the tests run without a device
 * (libs/input/tests/test_pad_gc_map.c).
 */

#ifndef PS3RECOMP_PAD_GC_MAP_H
#define PS3RECOMP_PAD_GC_MAP_H

#include <stdint.h>

#include "pad_host_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One snapshot of the profile. The sticks use GameController's convention:
 * y is up-positive. Every analog value is a button/trigger `value` in 0..1. */
typedef struct pad_gc_report {
    uint32_t buttons;            /* PAD_GC_BTN_* bits */
    float lx, ly, rx, ry;        /* -1..1; GameController convention: y up is +1 */
    float l2, r2;                /* 0..1 */
    float cross, circle, square, triangle, l1, r1, up, down, left, right; /* 0..1 button values */
} pad_gc_report;

/* GameController face-button names. The PS3 assignment is the SDL2 path's
 * (pad_poll_sdl2): A Cross, B Circle, X Square, Y Triangle. */
enum {
    PAD_GC_BTN_A = 1u << 0,
    PAD_GC_BTN_B = 1u << 1,
    PAD_GC_BTN_X = 1u << 2,
    PAD_GC_BTN_Y = 1u << 3,
    PAD_GC_BTN_L1 = 1u << 4,
    PAD_GC_BTN_R1 = 1u << 5,
    PAD_GC_BTN_L3 = 1u << 6,
    PAD_GC_BTN_R3 = 1u << 7,
    PAD_GC_BTN_MENU = 1u << 8,
    PAD_GC_BTN_OPTIONS = 1u << 9,
    PAD_GC_BTN_UP = 1u << 10,
    PAD_GC_BTN_DOWN = 1u << 11,
    PAD_GC_BTN_LEFT = 1u << 12,
    PAD_GC_BTN_RIGHT = 1u << 13
};

/* Writes every field of *out (connected = 1). NaN reads neutral (a centred
 * stick, a released button); out-of-range values clamp. */
void pad_gc_map(const pad_gc_report* in, PadHostState* out);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_PAD_GC_MAP_H */
