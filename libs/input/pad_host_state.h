/*
 * ps3recomp - the per-port state a host pad backend hands to the guest
 *
 * One instance per host port (cellPad.c), filled by the backend's poll and
 * read by cellPadGetData. It lives in its own header because a backend can
 * fill it from another translation unit (pad_gc_map.c): every backend produces
 * exactly this state, so the guest cannot tell which host produced it.
 */

#ifndef PS3RECOMP_PAD_HOST_STATE_H
#define PS3RECOMP_PAD_HOST_STATE_H

#include "ps3emu/ps3types.h"

typedef struct {
    int  connected;
    u16  buttons;           /* CELL_PAD_CTRL_* bitmask */
    u8   analog_lx;         /* 0-255, center=128 */
    u8   analog_ly;
    u8   analog_rx;
    u8   analog_ry;
    u8   trigger_l2;        /* 0-255 */
    u8   trigger_r2;        /* 0-255 */
    /* Pressure-sensitive face buttons (0-255) */
    u8   press_right;
    u8   press_left;
    u8   press_up;
    u8   press_down;
    u8   press_triangle;
    u8   press_circle;
    u8   press_cross;
    u8   press_square;
    u8   press_l1;
    u8   press_r1;
} PadHostState;

#endif /* PS3RECOMP_PAD_HOST_STATE_H */
