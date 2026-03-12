/*
 * ps3recomp - cellSysutil HLE implementation
 *
 * System callbacks, parameter queries, BGM control, system cache,
 * and disc game check.
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

static s32 s_bgm_enabled = 1;
static s32 s_bgm_status = CELL_SYSUTIL_BGMPLAYBACK_STATUS_STOP;
static char s_cache_path[CELL_SYSCACHE_PATH_MAX];
static int s_cache_mounted = 0;

static void (*s_disc_change_cb)(void*) = NULL;
static void* s_disc_change_arg = NULL;

/* ---------------------------------------------------------------------------
 * Core callbacks & params
 * -----------------------------------------------------------------------*/

s32 cellSysutilRegisterCallback(s32 slot, CellSysutilCallback func, void* userdata)
{
    printf("[cellSysutil] RegisterCallback(slot=%d, func=%p)\n",
           slot, (void*)(uintptr_t)func);

    if (slot < 0 || slot >= CELL_SYSUTIL_MAX_CALLBACKS)
        return CELL_SYSUTIL_ERROR_NUM;

    if (!func)
        return CELL_SYSUTIL_ERROR_VALUE;

    s_callbacks[slot].func       = func;
    s_callbacks[slot].userdata   = userdata;
    s_callbacks[slot].registered = 1;

    return CELL_OK;
}

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

s32 cellSysutilCheckCallback(void)
{
    /* Process pending callbacks - in recomp, no system events to deliver,
       but we still need to call this without error so game loops work */
    return CELL_OK;
}

s32 cellSysutilGetSystemParamInt(s32 id, s32* value)
{
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
    case CELL_SYSUTIL_SYSTEMPARAM_ID_GAME_PARENTAL_LEVEL0_RESTRICT:
        *value = 0;
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_CURRENT_USER_HAS_NP_ACCOUNT:
        *value = 1; /* Has NP account */
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_CAMERA_PLFREQ:
        *value = 0; /* 60Hz */
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_PAD_RUMBLE:
        *value = 1; /* Rumble on */
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_KEYBOARD_TYPE:
        *value = 0; /* US/101 */
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_PAD_AUTOOFF:
        *value = 0; /* Disabled */
        break;
    default:
        printf("[cellSysutil] GetSystemParamInt: unknown id 0x%04X\n", id);
        *value = 0;
        break;
    }

    return CELL_OK;
}

s32 cellSysutilGetSystemParamString(s32 id, char* buf, u32 bufsize)
{
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
        printf("[cellSysutil] GetSystemParamString: unknown id 0x%04X\n", id);
        buf[0] = '\0';
        break;
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * BGM playback control
 * -----------------------------------------------------------------------*/

s32 cellSysutilEnableBgmPlayback(void)
{
    printf("[cellSysutil] EnableBgmPlayback()\n");
    s_bgm_enabled = 1;
    return CELL_OK;
}

s32 cellSysutilDisableBgmPlayback(void)
{
    printf("[cellSysutil] DisableBgmPlayback()\n");
    s_bgm_enabled = 0;
    s_bgm_status = CELL_SYSUTIL_BGMPLAYBACK_STATUS_STOP;
    return CELL_OK;
}

s32 cellSysutilGetBgmPlaybackStatus(s32* status)
{
    if (!status)
        return CELL_SYSUTIL_ERROR_VALUE;

    *status = s_bgm_status;
    return CELL_OK;
}

s32 cellSysutilSetBgmPlaybackExtraParam(void* param)
{
    (void)param;
    printf("[cellSysutil] SetBgmPlaybackExtraParam()\n");
    return CELL_OK;
}

s32 cellSysutilEnableBgmPlaybackEx(s32 param)
{
    (void)param;
    printf("[cellSysutil] EnableBgmPlaybackEx(%d)\n", param);
    s_bgm_enabled = 1;
    return CELL_OK;
}

s32 cellSysutilDisableBgmPlaybackEx(void)
{
    printf("[cellSysutil] DisableBgmPlaybackEx()\n");
    s_bgm_enabled = 0;
    s_bgm_status = CELL_SYSUTIL_BGMPLAYBACK_STATUS_STOP;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * System cache
 * -----------------------------------------------------------------------*/

s32 cellSysCacheMount(char* cachePath)
{
    printf("[cellSysutil] SysCacheMount()\n");

    if (!cachePath)
        return CELL_EINVAL;

    /* Provide a temp directory path */
    strncpy(s_cache_path, "/dev_hdd1/caches", CELL_SYSCACHE_PATH_MAX - 1);
    s_cache_path[CELL_SYSCACHE_PATH_MAX - 1] = '\0';
    strncpy(cachePath, s_cache_path, CELL_SYSCACHE_PATH_MAX - 1);
    cachePath[CELL_SYSCACHE_PATH_MAX - 1] = '\0';
    s_cache_mounted = 1;

    return CELL_OK;
}

s32 cellSysCacheClear(void)
{
    printf("[cellSysutil] SysCacheClear()\n");
    s_cache_mounted = 0;
    s_cache_path[0] = '\0';
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Disc game check
 * -----------------------------------------------------------------------*/

s32 cellDiscGameGetBootDiscInfo(u32* type, char* titleId, u32 titleIdSize)
{
    printf("[cellSysutil] DiscGameGetBootDiscInfo()\n");

    if (type)
        *type = CELL_DISCGAME_TYPE_HDD; /* pretend HDD game */

    if (titleId && titleIdSize > 0) {
        strncpy(titleId, "GAME00000", titleIdSize - 1);
        titleId[titleIdSize - 1] = '\0';
    }

    return CELL_OK;
}

s32 cellDiscGameRegisterDiscChangeCallback(void (*callback)(void*), void* arg)
{
    printf("[cellSysutil] DiscGameRegisterDiscChangeCallback()\n");
    s_disc_change_cb = callback;
    s_disc_change_arg = arg;
    return CELL_OK;
}

s32 cellDiscGameUnregisterDiscChangeCallback(void)
{
    printf("[cellSysutil] DiscGameUnregisterDiscChangeCallback()\n");
    s_disc_change_cb = NULL;
    s_disc_change_arg = NULL;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Misc utilities
 * -----------------------------------------------------------------------*/

s32 cellSysutilGetLicenseArea(void)
{
    /* Return 'A' for America */
    return 'A';
}

s32 cellSysutilIsMeetingApp(void)
{
    return 0; /* Not a meeting app */
}
