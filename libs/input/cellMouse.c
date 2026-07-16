/*
 * ps3recomp - cellMouse HLE implementation
 *
 * Mouse input. Tracks mouse state from host events and provides data
 * in PS3 CellMouseData format.
 *
 * The host input layer should call cellMouse_moveEvent(), cellMouse_buttonEvent(),
 * and cellMouse_wheelEvent() when mouse events occur.
 */

#include "cellMouse.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

typedef struct {
    int  connected;
    u32  mode;          /* relative or absolute */
    u8   buttons;       /* current button state */
    /* Accumulated delta since last GetData */
    s32  acc_dx;
    s32  acc_dy;
    s32  acc_wheel;
    int  updated;
    /* Ring buffer for data list mode */
    CellMouseData ring[CELL_MOUSE_MAX_DATA_LIST_NUM];
    u32  ring_write;
    u32  ring_count;
} MousePortState;

static int            s_mouse_initialized = 0;
static u32            s_mouse_max_connect = 0;
static MousePortState s_mouse_ports[CELL_MOUSE_MAX_MICE];

/* ---------------------------------------------------------------------------
 * Host event injection
 * -----------------------------------------------------------------------*/

void cellMouse_moveEvent(u32 port, s8 dx, s8 dy)
{
    if (!s_mouse_initialized || port >= CELL_MOUSE_MAX_MICE)
        return;

    MousePortState* ms = &s_mouse_ports[port];
    if (!ms->connected) ms->connected = 1;

    ms->acc_dx += dx;
    ms->acc_dy += dy;
    ms->updated = 1;
}

void cellMouse_buttonEvent(u32 port, u8 button_mask, int pressed)
{
    if (!s_mouse_initialized || port >= CELL_MOUSE_MAX_MICE)
        return;

    MousePortState* ms = &s_mouse_ports[port];
    if (!ms->connected) ms->connected = 1;

    if (pressed)
        ms->buttons |= button_mask;
    else
        ms->buttons &= ~button_mask;
    ms->updated = 1;
}

void cellMouse_wheelEvent(u32 port, s8 wheel)
{
    if (!s_mouse_initialized || port >= CELL_MOUSE_MAX_MICE)
        return;

    MousePortState* ms = &s_mouse_ports[port];
    if (!ms->connected) ms->connected = 1;

    ms->acc_wheel += wheel;
    ms->updated = 1;
}

/* Push a snapshot into the ring buffer */
static void mouse_push_ring(MousePortState* ms)
{
    CellMouseData d;
    d.update  = ms->updated ? 1 : 0;
    d.buttons = ms->buttons;

    /* Clamp accumulated deltas to s8 range */
    s32 dx = ms->acc_dx;
    s32 dy = ms->acc_dy;
    s32 wh = ms->acc_wheel;
    if (dx > 127) dx = 127; if (dx < -128) dx = -128;
    if (dy > 127) dy = 127; if (dy < -128) dy = -128;
    if (wh > 127) wh = 127; if (wh < -128) wh = -128;

    d.x_axis = (s8)dx;
    d.y_axis = (s8)dy;
    d.wheel  = (s8)wh;
    d.tilt   = 0;

    u32 idx = ms->ring_write % CELL_MOUSE_MAX_DATA_LIST_NUM;
    ms->ring[idx] = d;
    ms->ring_write++;
    if (ms->ring_count < CELL_MOUSE_MAX_DATA_LIST_NUM)
        ms->ring_count++;
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellMouseInit(u32 max_connect)
{
    printf("[cellMouse] Init(max_connect=%u)\n", max_connect);

    if (s_mouse_initialized)
        return CELL_MOUSE_ERROR_ALREADY_INITIALIZED;

    if (max_connect == 0 || max_connect > CELL_MOUSE_MAX_MICE)
        return CELL_MOUSE_ERROR_INVALID_PARAMETER;

    s_mouse_initialized = 1;
    s_mouse_max_connect = max_connect;
    memset(s_mouse_ports, 0, sizeof(s_mouse_ports));

    /* No mouse is connected until the host says one is (cellMouse_moveEvent
     * and friends set ports[n].connected). This used to assert port 0 was
     * always present, which reports hardware that is not there: a title is
     * then entitled to open a mouse-driven path and wait on input that can
     * never arrive. Report what actually exists -- an unplugged port is a
     * perfectly normal thing for a title to see. */

    return CELL_OK;
}

s32 cellMouseEnd(void)
{
    printf("[cellMouse] End()\n");

    if (!s_mouse_initialized)
        return CELL_MOUSE_ERROR_UNINITIALIZED;

    s_mouse_initialized = 0;
    return CELL_OK;
}

s32 cellMouseGetData(u32 port_no, CellMouseData* data)
{
    if (!s_mouse_initialized)
        return CELL_MOUSE_ERROR_UNINITIALIZED;

    if (port_no >= s_mouse_max_connect || !data)
        return CELL_MOUSE_ERROR_INVALID_PARAMETER;

    MousePortState* ms = &s_mouse_ports[port_no];

    if (!ms->connected) {
        memset(data, 0, sizeof(CellMouseData));
        return CELL_MOUSE_ERROR_NO_DEVICE;
    }

    data->update  = ms->updated ? 1 : 0;
    data->buttons = ms->buttons;

    /* Clamp accumulated deltas to s8 range */
    s32 dx = ms->acc_dx;
    s32 dy = ms->acc_dy;
    s32 wh = ms->acc_wheel;
    if (dx > 127) dx = 127; if (dx < -128) dx = -128;
    if (dy > 127) dy = 127; if (dy < -128) dy = -128;
    if (wh > 127) wh = 127; if (wh < -128) wh = -128;

    data->x_axis = (s8)dx;
    data->y_axis = (s8)dy;
    data->wheel  = (s8)wh;
    data->tilt   = 0;

    /* Reset accumulated state */
    ms->acc_dx = 0;
    ms->acc_dy = 0;
    ms->acc_wheel = 0;
    ms->updated = 0;

    return CELL_OK;
}

s32 cellMouseGetDataList(u32 port_no, CellMouseDataList* data)
{
    if (!s_mouse_initialized)
        return CELL_MOUSE_ERROR_UNINITIALIZED;

    if (port_no >= s_mouse_max_connect || !data)
        return CELL_MOUSE_ERROR_INVALID_PARAMETER;

    MousePortState* ms = &s_mouse_ports[port_no];

    if (!ms->connected) {
        memset(data, 0, sizeof(CellMouseDataList));
        return CELL_MOUSE_ERROR_NO_DEVICE;
    }

    /* Push current state into ring if there's pending data */
    if (ms->updated) {
        mouse_push_ring(ms);
        ms->acc_dx = 0;
        ms->acc_dy = 0;
        ms->acc_wheel = 0;
        ms->updated = 0;
    }

    data->list_num = ms->ring_count;
    u32 start = 0;
    if (ms->ring_write > CELL_MOUSE_MAX_DATA_LIST_NUM)
        start = ms->ring_write - CELL_MOUSE_MAX_DATA_LIST_NUM;

    for (u32 i = 0; i < ms->ring_count; i++) {
        u32 idx = (start + i) % CELL_MOUSE_MAX_DATA_LIST_NUM;
        data->list[i] = ms->ring[idx];
    }

    /* Clear ring */
    ms->ring_count = 0;
    ms->ring_write = 0;

    return CELL_OK;
}

/* CellMouseInfo lives in GUEST memory and is BIG-ENDIAN. Layout is the SDK's
 * (cell/mouse/mouse_codes.h, CELL_MAX_MICE = 127):
 *   +0x000 u32 max_connect
 *   +0x004 u32 now_connect
 *   +0x008 u32 info
 *   +0x00C u16 vendor_id[127]    (ends +0x10A)
 *   +0x10A u16 product_id[127]   (ends +0x208)
 *   +0x208 u8  status[127]       (ends +0x287 -- 647 bytes total)
 *
 * This took a CellMouseInfo* and memset/assigned through it, which dereferences
 * the raw guest EA as a host pointer (instant AV), and the local struct declares
 * u32 arrays where hardware has u16/u16/u8, so even a translated pointer would
 * lay the fields out wrong. Neither showed up because the NID was never
 * registered: gen_hle_nids.py was handed the PRX name "sys_io", but the file is
 * cellMouse.c, so it warned once into the build noise and left every mouse NID
 * unresolved -- and the dispatcher's unresolved path answers CELL_OK, so LBP
 * polled a mouse that silently never existed.
 */
extern void vm_write8 (unsigned long long a, unsigned char  v);
extern void vm_write16(unsigned long long a, unsigned short v);
extern void vm_write32(unsigned long long a, unsigned int   v);

#define MOUSE_INFO_VENDOR    0x00Cu
#define MOUSE_INFO_PRODUCT   0x10Au
#define MOUSE_INFO_STATUS    0x208u
#define MOUSE_INFO_SIZE      0x287u
#define MOUSE_INFO_MAX_MICE  127u

s32 cellMouseGetInfo(u32 info_ea)
{
    if (!s_mouse_initialized)
        return CELL_MOUSE_ERROR_UNINITIALIZED;

    if (!info_ea)
        return CELL_MOUSE_ERROR_INVALID_PARAMETER;

    for (u32 i = 0; i < MOUSE_INFO_SIZE; i++)
        vm_write8((unsigned long long)info_ea + i, 0);

    vm_write32((unsigned long long)info_ea + 0, s_mouse_max_connect);

    u32 connected = 0;
    for (u32 i = 0; i < s_mouse_max_connect && i < MOUSE_INFO_MAX_MICE
                    && i < CELL_MOUSE_MAX_MICE; i++) {
        if (!s_mouse_ports[i].connected)
            continue;
        vm_write8 ((unsigned long long)info_ea + MOUSE_INFO_STATUS  + i,     CELL_MOUSE_STATUS_CONNECTED);
        vm_write16((unsigned long long)info_ea + MOUSE_INFO_VENDOR  + i * 2, 0x054C); /* Sony */
        vm_write16((unsigned long long)info_ea + MOUSE_INFO_PRODUCT + i * 2, 0x0001);
        connected++;
    }
    vm_write32((unsigned long long)info_ea + 4, connected);

    return CELL_OK;
}

s32 cellMouseSetTabletMode(u32 port_no, u32 mode)
{
    printf("[cellMouse] SetTabletMode(port=%u, mode=%u)\n", port_no, mode);

    if (!s_mouse_initialized)
        return CELL_MOUSE_ERROR_UNINITIALIZED;

    if (port_no >= CELL_MOUSE_MAX_MICE)
        return CELL_MOUSE_ERROR_INVALID_PARAMETER;

    s_mouse_ports[port_no].mode = mode;
    return CELL_OK;
}

s32 cellMouseClearBuf(u32 port_no)
{
    if (!s_mouse_initialized)
        return CELL_MOUSE_ERROR_UNINITIALIZED;

    if (port_no >= CELL_MOUSE_MAX_MICE)
        return CELL_MOUSE_ERROR_INVALID_PARAMETER;

    MousePortState* ms = &s_mouse_ports[port_no];
    ms->acc_dx = 0;
    ms->acc_dy = 0;
    ms->acc_wheel = 0;
    ms->updated = 0;
    ms->ring_count = 0;
    ms->ring_write = 0;

    return CELL_OK;
}
