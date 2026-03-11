/*
 * ps3recomp - cellPad HLE stub implementation
 *
 * In a full implementation this would read from the host input backend
 * (SDL, XInput, etc.) and translate to PS3 pad format.  For now, stubs
 * report a single connected controller with no buttons pressed.
 */

#include "cellPad.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int  s_pad_initialized = 0;
static u32  s_max_connect = 0;
static u32  s_port_setting[CELL_PAD_MAX_PORT_NUM];

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

/* NID: 0x1CF98800 */
s32 cellPadInit(u32 max_connect)
{
    printf("[cellPad] Init(max_connect=%u)\n", max_connect);

    if (s_pad_initialized)
        return CELL_PAD_ERROR_ALREADY_OPENED;

    if (max_connect == 0 || max_connect > CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    s_pad_initialized = 1;
    s_max_connect = max_connect;
    memset(s_port_setting, 0, sizeof(s_port_setting));

    return CELL_OK;
}

/* NID: 0x4D9B75D5 */
s32 cellPadEnd(void)
{
    printf("[cellPad] End()\n");

    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    s_pad_initialized = 0;
    return CELL_OK;
}

/* NID: 0x8B72CBA1 */
s32 cellPadGetData(u32 port_no, CellPadData* data)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= s_max_connect || !data)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    memset(data, 0, sizeof(CellPadData));

    /*
     * Report a valid but idle controller on port 0.
     * Analog sticks centered at 128.
     */
    if (port_no == 0) {
        data->len = 24;    /* standard data length */
        data->button[0] = 24;
        data->button[1] = 0;
        data->button[2] = 0;   /* no buttons pressed */
        data->button[3] = 0;
        data->button[4] = 128; /* right stick X */
        data->button[5] = 128; /* right stick Y */
        data->button[6] = 128; /* left stick X */
        data->button[7] = 128; /* left stick Y */
    } else {
        data->len = 0;  /* no controller on this port */
    }

    return CELL_OK;
}

/* NID: 0x8B8013DA */
s32 cellPadGetInfo2(CellPadInfo2* info)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (!info)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    memset(info, 0, sizeof(CellPadInfo2));

    info->max_connect = s_max_connect;
    info->now_connect = 1;  /* one controller connected */

    /* Port 0 is connected and ready */
    info->port_status[0]      = 1;  /* connected */
    info->port_setting[0]     = s_port_setting[0];
    info->device_capability[0] = 0x000F;  /* standard controller caps */
    info->device_type[0]      = CELL_PAD_DEV_TYPE_STANDARD;

    return CELL_OK;
}

/* NID: 0xA703A51D */
s32 cellPadSetPortSetting(u32 port_no, u32 port_setting)
{
    printf("[cellPad] SetPortSetting(port=%u, setting=0x%X)\n",
           port_no, port_setting);

    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    s_port_setting[port_no] = port_setting;
    return CELL_OK;
}
