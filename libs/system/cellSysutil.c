/*
 * ps3recomp - cellSysutil HLE stub implementation
 */

#include "cellSysutil.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

typedef struct {
    CellSysutilCallback func;
    void*               userdata;
    int                 registered;
} SysutilCallbackSlot;

static SysutilCallbackSlot s_callbacks[CELL_SYSUTIL_MAX_CALLBACKS];

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

/* NID: 0x9D848F34 */
s32 cellSysutilRegisterCallback(s32 slot, CellSysutilCallback func, void* userdata)
{
    printf("[cellSysutil] RegisterCallback(slot=%d, func=%p, userdata=%p)\n",
           slot, (void*)(uintptr_t)func, userdata);

    if (slot < 0 || slot >= CELL_SYSUTIL_MAX_CALLBACKS)
        return CELL_SYSUTIL_ERROR_NUM;

    if (!func)
        return CELL_SYSUTIL_ERROR_VALUE;

    s_callbacks[slot].func       = func;
    s_callbacks[slot].userdata   = userdata;
    s_callbacks[slot].registered = 1;

    return CELL_OK;
}

/* NID: 0x02FF3C1B */
s32 cellSysutilUnregisterCallback(s32 slot)
{
    printf("[cellSysutil] UnregisterCallback(slot=%d)\n", slot);

    if (slot < 0 || slot >= CELL_SYSUTIL_MAX_CALLBACKS)
        return CELL_SYSUTIL_ERROR_NUM;

    s_callbacks[slot].func       = NULL;
    s_callbacks[slot].userdata   = NULL;
    s_callbacks[slot].registered = 0;

    return CELL_OK;
}

/* NID: 0x189A74DA */
s32 cellSysutilCheckCallback(void)
{
    /* In a real implementation this would pump system events and dispatch
       callbacks.  For now we just silently succeed. */
    return CELL_OK;
}

/* NID: 0x40E895D3 */
s32 cellSysutilGetSystemParamInt(s32 id, s32* value)
{
    printf("[cellSysutil] GetSystemParamInt(id=0x%04X)\n", id);

    if (!value)
        return CELL_SYSUTIL_ERROR_VALUE;

    switch (id) {
    case CELL_SYSUTIL_SYSTEMPARAM_ID_LANG:
        *value = CELL_SYSUTIL_LANG_ENGLISH_US;
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_ENTER_BUTTON_ASSIGN:
        *value = CELL_SYSUTIL_ENTER_BUTTON_ASSIGN_CROSS;
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_DATE_FORMAT:
        *value = 0; /* YYYYMMDD */
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_TIME_FORMAT:
        *value = 0; /* 24-hour */
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_TIMEZONE:
        *value = 0; /* UTC */
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_SUMMERTIME:
        *value = 0; /* No DST */
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_GAME_PARENTAL_LEVEL:
        *value = 0; /* No restriction */
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_PAD_RUMBLE:
        *value = 1; /* Rumble on */
        break;
    default:
        printf("[cellSysutil] WARNING: unknown system param int id 0x%04X\n", id);
        *value = 0;
        break;
    }

    return CELL_OK;
}

/* NID: 0x938013A0 */
s32 cellSysutilGetSystemParamString(s32 id, char* buf, u32 bufsize)
{
    printf("[cellSysutil] GetSystemParamString(id=0x%04X, bufsize=%u)\n", id, bufsize);

    if (!buf || bufsize == 0)
        return CELL_SYSUTIL_ERROR_VALUE;

    switch (id) {
    case CELL_SYSUTIL_SYSTEMPARAM_ID_NICKNAME:
        strncpy(buf, "ps3recomp_user", bufsize - 1);
        buf[bufsize - 1] = '\0';
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_CURRENT_USERNAME:
        strncpy(buf, "User", bufsize - 1);
        buf[bufsize - 1] = '\0';
        break;
    default:
        printf("[cellSysutil] WARNING: unknown system param string id 0x%04X\n", id);
        buf[0] = '\0';
        break;
    }

    return CELL_OK;
}
