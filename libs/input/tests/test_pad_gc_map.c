/* pad_gc_map: the GameController profile -> PadHostState mapping a native
 * GameController backend hands to the guest. Pure C, no framework: builds and
 * runs on any host (Linux GCC included).
 *
 *   cc -std=c17 -Wall -Wextra -I include -I libs/input \
 *         libs/input/tests/test_pad_gc_map.c libs/input/pad_gc_map.c -lm \
 *         -o /tmp/test_pad_gc_map && /tmp/test_pad_gc_map
 *
 * Values round with lroundf -- half away from zero -- so a button held to 0.5
 * reads 128 and a centred stick (0.0) reads 128. PS3's stick origin is the
 * top-left (0 = up) while GameController reports up as +1, so y is inverted.
 */
#include "cellPad.h"
#include "pad_gc_map.h"

#include <assert.h>
#include <math.h>
#include <string.h>

static pad_gc_report neutral(void)
{
    pad_gc_report r;
    memset(&r, 0, sizeof r);
    return r;
}

static int all_released(const PadHostState* hs)
{
    return hs->press_right == 0 && hs->press_left == 0 &&
           hs->press_up == 0 && hs->press_down == 0 &&
           hs->press_triangle == 0 && hs->press_circle == 0 &&
           hs->press_cross == 0 && hs->press_square == 0 &&
           hs->press_l1 == 0 && hs->press_r1 == 0;
}

static void test_neutral(void)
{
    pad_gc_report r = neutral();
    PadHostState hs;
    memset(&hs, 0xA5, sizeof hs);   /* the mapping writes every field */
    pad_gc_map(&r, &hs);
    assert(hs.connected == 1);
    assert(hs.buttons == 0);
    assert(hs.analog_lx == 128 && hs.analog_ly == 128);
    assert(hs.analog_rx == 128 && hs.analog_ry == 128);
    assert(hs.trigger_l2 == 0 && hs.trigger_r2 == 0);
    assert(all_released(&hs));
}

static void test_buttons(void)
{
    static const struct { uint32_t gc; u16 cell; } map[] = {
        { PAD_GC_BTN_A,       CELL_PAD_CTRL_CROSS },
        { PAD_GC_BTN_B,       CELL_PAD_CTRL_CIRCLE },
        { PAD_GC_BTN_X,       CELL_PAD_CTRL_SQUARE },
        { PAD_GC_BTN_Y,       CELL_PAD_CTRL_TRIANGLE },
        { PAD_GC_BTN_L1,      CELL_PAD_CTRL_L1 },
        { PAD_GC_BTN_R1,      CELL_PAD_CTRL_R1 },
        { PAD_GC_BTN_L3,      CELL_PAD_CTRL_L3 },
        { PAD_GC_BTN_R3,      CELL_PAD_CTRL_R3 },
        { PAD_GC_BTN_MENU,    CELL_PAD_CTRL_START },
        { PAD_GC_BTN_OPTIONS, CELL_PAD_CTRL_SELECT },
        { PAD_GC_BTN_UP,      CELL_PAD_CTRL_UP },
        { PAD_GC_BTN_DOWN,    CELL_PAD_CTRL_DOWN },
        { PAD_GC_BTN_LEFT,    CELL_PAD_CTRL_LEFT },
        { PAD_GC_BTN_RIGHT,   CELL_PAD_CTRL_RIGHT },
    };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) {
        pad_gc_report r = neutral();
        r.buttons = map[i].gc;
        PadHostState hs;
        pad_gc_map(&r, &hs);
        /* exactly its PS3 bit: nothing else may come along */
        assert(hs.buttons == map[i].cell);
    }

    /* A bit with no PS3 counterpart is ignored. */
    pad_gc_report r = neutral();
    r.buttons = 0x80000000u;
    PadHostState hs;
    pad_gc_map(&r, &hs);
    assert(hs.buttons == 0);
}

static void test_sticks(void)
{
    PadHostState hs;
    pad_gc_report r = neutral();

    r.lx = -1.0f; r.ly = 1.0f;    /* left: left, up            */
    r.rx = 1.0f;  r.ry = -1.0f;   /* right: right, down        */
    pad_gc_map(&r, &hs);
    assert(hs.analog_lx == 0);
    assert(hs.analog_ly == 0);    /* GC +1 is up; PS3 0 is up too */
    assert(hs.analog_rx == 255);
    assert(hs.analog_ry == 255);  /* GC -1 is down; PS3 255 is down */

    r = neutral();
    r.lx = 0.5f;
    pad_gc_map(&r, &hs);
    assert(hs.analog_lx == 191);  /* lroundf((0.5 + 1) * 127.5) = lroundf(191.25) */

    /* The y inversion in the middle of the travel: GC up-half reads PS3
     * up-half (below 128), GC down-half reads above 128. */
    r = neutral();
    r.ly = 0.5f;
    pad_gc_map(&r, &hs);
    assert(hs.analog_ly == 64);   /* lroundf((1 - 0.5) * 127.5) = lroundf(63.75) */
    r.ly = -0.5f;
    pad_gc_map(&r, &hs);
    assert(hs.analog_ly == 191);
}

static void test_triggers(void)
{
    /* The SDL backend's digital threshold: raw > 3000 of 32767. */
    const float below = 3000.0f / 32767.0f;
    const float above = 3001.0f / 32767.0f;
    PadHostState hs;
    pad_gc_report r = neutral();

    r.l2 = below;                 /* exactly at the threshold: not pressed */
    pad_gc_map(&r, &hs);
    assert((hs.buttons & CELL_PAD_CTRL_L2) == 0);
    assert(hs.trigger_l2 == 23);  /* 3000/32767*255 = 23.35 -> 23 */

    r = neutral();
    r.l2 = above;                 /* one step past it: pressed */
    pad_gc_map(&r, &hs);
    assert(hs.buttons == CELL_PAD_CTRL_L2);
    assert(hs.trigger_l2 == 23);

    r = neutral();
    r.r2 = 1.0f;
    pad_gc_map(&r, &hs);
    assert(hs.buttons == CELL_PAD_CTRL_R2);
    assert(hs.trigger_r2 == 255);
    assert(hs.trigger_l2 == 0);
}

static void test_pressure(void)
{
    pad_gc_report r = neutral();
    r.cross = 0.5f;        /* lroundf(127.5) = 128, half away from zero */
    r.circle = 1.0f;
    r.square = 0.0f;
    r.triangle = 0.25f;    /* lroundf(63.75) = 64 */
    r.l1 = 0.5f;
    r.r1 = 1.0f;
    r.up = 1.0f;
    r.down = 0.5f;
    r.left = 0.5f;
    r.right = 0.25f;

    PadHostState hs;
    pad_gc_map(&r, &hs);
    assert(hs.press_cross == 128);
    assert(hs.press_circle == 255);
    assert(hs.press_square == 0);
    assert(hs.press_triangle == 64);
    assert(hs.press_l1 == 128);
    assert(hs.press_r1 == 255);
    assert(hs.press_up == 255);
    assert(hs.press_down == 128);
    assert(hs.press_left == 128);
    assert(hs.press_right == 64);
    /* Pressure alone does not press a digital button. */
    assert(hs.buttons == 0);
}

static void test_clamp(void)
{
    PadHostState hs;
    pad_gc_report r = neutral();

    /* NaN reads neutral: sticks centred, buttons and triggers released. */
    r.lx = NAN; r.ly = NAN; r.rx = NAN; r.ry = NAN;
    r.l2 = NAN; r.r2 = NAN;
    r.cross = NAN; r.up = NAN;
    pad_gc_map(&r, &hs);
    assert(hs.analog_lx == 128 && hs.analog_ly == 128);
    assert(hs.analog_rx == 128 && hs.analog_ry == 128);
    assert(hs.trigger_l2 == 0 && hs.trigger_r2 == 0);
    assert(hs.press_cross == 0 && hs.press_up == 0);
    assert((hs.buttons & (CELL_PAD_CTRL_L2 | CELL_PAD_CTRL_R2)) == 0);

    /* Infinities and out-of-range values clamp to the ends of each axis. */
    r = neutral();
    r.lx = INFINITY; r.ly = INFINITY;   /* right, up   */
    r.rx = -INFINITY; r.ry = -2.0f;     /* left, down  */
    r.l2 = 2.0f; r.r2 = -1.0f;
    r.cross = 2.0f; r.circle = -1.0f;
    pad_gc_map(&r, &hs);
    assert(hs.analog_lx == 255);
    assert(hs.analog_ly == 0);
    assert(hs.analog_rx == 0);
    assert(hs.analog_ry == 255);
    assert(hs.trigger_l2 == 255 && (hs.buttons & CELL_PAD_CTRL_L2));
    assert(hs.trigger_r2 == 0 && (hs.buttons & CELL_PAD_CTRL_R2) == 0);
    assert(hs.press_cross == 255);
    assert(hs.press_circle == 0);
}

int main(void)
{
    test_neutral();
    test_buttons();
    test_sticks();
    test_triggers();
    test_pressure();
    test_clamp();
    return 0;
}
