/*
 * ps3recomp - GameController profile -> PadHostState mapping
 *
 * The mapping is the SDL2 path's (pad_poll_sdl2 in cellPad.c), so a pad reads
 * the same whatever host drives it: A Cross, B Circle, X Square, Y Triangle,
 * shoulders, stick clicks, Menu Start, Options Select, D-pad in both forms.
 */

#include "pad_gc_map.h"

#include <math.h>
#include <string.h>

#include "cellPad.h"

/* pad_poll_sdl2 sets the L2/R2 digital bit from the raw trigger axis above
 * 3000 of 32767 (that backend's digital trigger threshold). Here the trigger
 * is 0..1, so the same fraction of travel keeps a title that reads the digital
 * bit -- not the pressure word -- behaving as it does with a pad on SDL. */
#define PAD_GC_TRIGGER_THRESHOLD (3000.0f / 32767.0f)

/* A stick axis: -1..1, NaN and out-of-range clamp. */
static float pad_gc_axis(float v)
{
    if (isnan(v)) return 0.0f;
    if (v < -1.0f) return -1.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* A button or trigger value: 0..1, NaN and out-of-range clamp. */
static float pad_gc_unit(float v)
{
    if (isnan(v)) return 0.0f;
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* lroundf rounds half away from zero, so a button held to 0.5 reads 128, not
 * 127, and a centred stick (0.0) reads 128. */
static u8 pad_gc_axis_u8(float v) { return (u8)lroundf(pad_gc_axis(v) * 127.5f + 127.5f); }
static u8 pad_gc_unit_u8(float v) { return (u8)lroundf(pad_gc_unit(v) * 255.0f); }

void pad_gc_map(const pad_gc_report* in, PadHostState* out)
{
    memset(out, 0, sizeof(*out));
    out->connected = 1;

    u16 btns = 0;
    if (in->buttons & PAD_GC_BTN_A)       btns |= CELL_PAD_CTRL_CROSS;
    if (in->buttons & PAD_GC_BTN_B)       btns |= CELL_PAD_CTRL_CIRCLE;
    if (in->buttons & PAD_GC_BTN_X)       btns |= CELL_PAD_CTRL_SQUARE;
    if (in->buttons & PAD_GC_BTN_Y)       btns |= CELL_PAD_CTRL_TRIANGLE;
    if (in->buttons & PAD_GC_BTN_L1)      btns |= CELL_PAD_CTRL_L1;
    if (in->buttons & PAD_GC_BTN_R1)      btns |= CELL_PAD_CTRL_R1;
    if (in->buttons & PAD_GC_BTN_L3)      btns |= CELL_PAD_CTRL_L3;
    if (in->buttons & PAD_GC_BTN_R3)      btns |= CELL_PAD_CTRL_R3;
    if (in->buttons & PAD_GC_BTN_MENU)    btns |= CELL_PAD_CTRL_START;
    if (in->buttons & PAD_GC_BTN_OPTIONS) btns |= CELL_PAD_CTRL_SELECT;
    if (in->buttons & PAD_GC_BTN_UP)      btns |= CELL_PAD_CTRL_UP;
    if (in->buttons & PAD_GC_BTN_DOWN)    btns |= CELL_PAD_CTRL_DOWN;
    if (in->buttons & PAD_GC_BTN_LEFT)    btns |= CELL_PAD_CTRL_LEFT;
    if (in->buttons & PAD_GC_BTN_RIGHT)   btns |= CELL_PAD_CTRL_RIGHT;

    /* An analog trigger presses its digital button at the SDL path's travel. */
    if (pad_gc_unit(in->l2) > PAD_GC_TRIGGER_THRESHOLD) btns |= CELL_PAD_CTRL_L2;
    if (pad_gc_unit(in->r2) > PAD_GC_TRIGGER_THRESHOLD) btns |= CELL_PAD_CTRL_R2;

    out->buttons = btns;

    /* PS3's stick origin is the top-left: 0 is full up. GameController reports
     * up as +1, so the y axes are inverted (ly = (1 - y) * 127.5). */
    out->analog_lx = pad_gc_axis_u8(in->lx);
    out->analog_ly = pad_gc_axis_u8(-in->ly);
    out->analog_rx = pad_gc_axis_u8(in->rx);
    out->analog_ry = pad_gc_axis_u8(-in->ry);

    out->trigger_l2 = pad_gc_unit_u8(in->l2);
    out->trigger_r2 = pad_gc_unit_u8(in->r2);

    /* GameController profiles carry pressure on every button, so unlike the
     * SDL path (0 or 255 only) this reports the real value; cellPadGetData
     * still lifts a pressed digital button to 255. */
    out->press_right    = pad_gc_unit_u8(in->right);
    out->press_left     = pad_gc_unit_u8(in->left);
    out->press_up       = pad_gc_unit_u8(in->up);
    out->press_down     = pad_gc_unit_u8(in->down);
    out->press_triangle = pad_gc_unit_u8(in->triangle);
    out->press_circle   = pad_gc_unit_u8(in->circle);
    out->press_cross    = pad_gc_unit_u8(in->cross);
    out->press_square   = pad_gc_unit_u8(in->square);
    out->press_l1       = pad_gc_unit_u8(in->l1);
    out->press_r1       = pad_gc_unit_u8(in->r1);
}
