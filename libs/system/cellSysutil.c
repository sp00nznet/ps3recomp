/*
 * ps3recomp - cellSysutil HLE implementation
 *
 * System callbacks, parameter queries, BGM control, system cache,
 * and disc game check.
 */

#include "cellSysutil.h"
#include "ps3emu/guest_call.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Guest-memory stores — out-params from the recompiled title are guest VM
 * addresses, not host pointers. */
extern void vm_write8(uint64_t addr, uint8_t v);
extern void vm_write32(uint64_t addr, uint32_t v);
extern uint8_t* vm_base;

/* ---------------------------------------------------------------------------
 * Guest callback dispatch hook (set by the game's host code at startup).
 * -----------------------------------------------------------------------*/
ps3_guest_caller_fn g_ps3_guest_caller = NULL;

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

typedef struct {
    /* Stored as a guest OPD address rather than a host function pointer:
     * the recompiled game passes its OPD addr through Register, and the
     * dispatcher hook resolves the OPD when actually invoking. The
     * CellSysutilCallback typedef maps to a pointer, so we cast through
     * uintptr_t at the API boundary. */
    uint32_t            guest_opd;
    uint32_t            userdata;     /* guest pointer; opaque to the runtime */
    int                 registered;
} SysutilCallbackSlot;

static SysutilCallbackSlot s_callbacks[CELL_SYSUTIL_MAX_CALLBACKS];

/* ---------------------------------------------------------------------------
 * Sysutil event queue.
 *
 * The OS would deliver events asynchronously (XMB open/close, drawing
 * begin/end, save-data complete, etc.). Games drain them by calling
 * cellSysutilCheckCallback periodically. We accept queued events from
 * the host side via cellSysutilQueueEvent() and drain them here.
 * -----------------------------------------------------------------------*/

typedef struct {
    int      slot;
    uint32_t status;
    uint32_t param;
} SysutilEvent;

#define SYSUTIL_EVENT_QUEUE_SIZE 32
static SysutilEvent s_event_queue[SYSUTIL_EVENT_QUEUE_SIZE];
static int          s_event_head = 0;  /* next read */
static int          s_event_tail = 0;  /* next write */

void cellSysutilQueueEvent(int slot, uint32_t status, uint32_t param)
{
    int next = (s_event_tail + 1) % SYSUTIL_EVENT_QUEUE_SIZE;
    if (next == s_event_head) {
        printf("[cellSysutil] event queue full — dropping status=0x%X\n", status);
        return;
    }
    s_event_queue[s_event_tail].slot   = slot;
    s_event_queue[s_event_tail].status = status;
    s_event_queue[s_event_tail].param  = param;
    s_event_tail = next;
}

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
    /* `func` and `userdata` are guest VM addresses; reinterpret without
     * touching them. NULL func means "unregister this slot" — some games
     * (notably flOw) call Register with func=NULL as a clear-on-poll. */
    uint32_t func_addr     = (uint32_t)(uintptr_t)func;
    uint32_t userdata_addr = (uint32_t)(uintptr_t)userdata;

    printf("[cellSysutil] RegisterCallback(slot=%d, func=0x%08X)\n",
           slot, func_addr);

    if (slot < 0 || slot >= CELL_SYSUTIL_MAX_CALLBACKS)
        return CELL_SYSUTIL_ERROR_NUM;

    if (func_addr == 0) {
        /* Treat as Unregister — matches observed game behaviour. */
        s_callbacks[slot].guest_opd  = 0;
        s_callbacks[slot].userdata   = 0;
        s_callbacks[slot].registered = 0;
        return CELL_OK;
    }

    s_callbacks[slot].guest_opd  = func_addr;
    s_callbacks[slot].userdata   = userdata_addr;
    s_callbacks[slot].registered = 1;

    return CELL_OK;
}

s32 cellSysutilUnregisterCallback(s32 slot)
{
    printf("[cellSysutil] UnregisterCallback(slot=%d)\n", slot);

    if (slot < 0 || slot >= CELL_SYSUTIL_MAX_CALLBACKS)
        return CELL_SYSUTIL_ERROR_NUM;

    s_callbacks[slot].guest_opd  = 0;
    s_callbacks[slot].userdata   = 0;
    s_callbacks[slot].registered = 0;

    return CELL_OK;
}

s32 cellSysutilCheckCallback(void)
{
    /* Drain the event queue, dispatching each event into guest code via
     * the registered ps3_guest_caller hook. Standard PS3 sysutil callback
     * signature is:
     *   void cb(uint64_t status, uint64_t param, void* userdata)
     * which maps to PPC r3=status, r4=param, r5=userdata; r6 is unused.
     *
     * Stops when the queue empties OR when no guest caller is installed
     * (in which case we drop pending events to avoid stalling).
     */
    while (s_event_head != s_event_tail) {
        SysutilEvent e = s_event_queue[s_event_head];
        s_event_head = (s_event_head + 1) % SYSUTIL_EVENT_QUEUE_SIZE;

        if (e.slot < 0 || e.slot >= CELL_SYSUTIL_MAX_CALLBACKS) continue;
        if (!s_callbacks[e.slot].registered) continue;
        if (!s_callbacks[e.slot].guest_opd) continue;

        if (g_ps3_guest_caller) {
            g_ps3_guest_caller(s_callbacks[e.slot].guest_opd,
                               (uint64_t)e.status,
                               (uint64_t)e.param,
                               (uint64_t)s_callbacks[e.slot].userdata,
                               0);
        }
    }
    return CELL_OK;
}

s32 cellSysutilGetSystemParamInt(s32 id, s32* value)
{
    /* `value` is a GUEST address (the recompiled title passes its own VM
     * pointer); dereferencing it as a host pointer faults. Compute locally
     * and store big-endian via vm_write32 (this crashed minecraft's boot at
     * its very first GetSystemParamInt(LANG) call). */
    uint32_t out_ea = (uint32_t)(uintptr_t)value;
    if (!out_ea)
        return CELL_SYSUTIL_ERROR_VALUE;

    s32 v = 0;
    switch (id) {
    case CELL_SYSUTIL_SYSTEMPARAM_ID_LANG:
        v = CELL_SYSUTIL_LANG_ENGLISH_US;
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_ENTER_BUTTON_ASSIGN:
        v = CELL_SYSUTIL_ENTER_BUTTON_ASSIGN_CROSS;
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_DATE_FORMAT:    v = 0; break; /* YYYYMMDD */
    case CELL_SYSUTIL_SYSTEMPARAM_ID_TIME_FORMAT:    v = 0; break; /* 24-hour */
    case CELL_SYSUTIL_SYSTEMPARAM_ID_TIMEZONE:       v = 0; break; /* UTC */
    case CELL_SYSUTIL_SYSTEMPARAM_ID_SUMMERTIME:     v = 0; break; /* No DST */
    case CELL_SYSUTIL_SYSTEMPARAM_ID_GAME_PARENTAL_LEVEL:           v = 0; break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_GAME_PARENTAL_LEVEL0_RESTRICT: v = 0; break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_CURRENT_USER_HAS_NP_ACCOUNT:   v = 1; break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_CAMERA_PLFREQ:  v = 0; break; /* 60Hz */
    case CELL_SYSUTIL_SYSTEMPARAM_ID_PAD_RUMBLE:     v = 1; break; /* Rumble on */
    case CELL_SYSUTIL_SYSTEMPARAM_ID_KEYBOARD_TYPE:  v = 0; break; /* US/101 */
    case CELL_SYSUTIL_SYSTEMPARAM_ID_PAD_AUTOOFF:    v = 0; break; /* Disabled */
    default:
        printf("[cellSysutil] GetSystemParamInt: unknown id 0x%04X\n", id);
        v = 0;
        break;
    }

    vm_write32(out_ea, (uint32_t)v);
    return CELL_OK;
}

s32 cellSysutilGetSystemParamString(s32 id, char* buf, u32 bufsize)
{
    if (!buf || bufsize == 0)
        return CELL_SYSUTIL_ERROR_VALUE;

    switch (id) {
    case CELL_SYSUTIL_SYSTEMPARAM_ID_NICKNAME: {
        /* `buf` is a GUEST address — serialize byte-wise via vm_write8. */
        const char* s = "ps3recomp_user";
        uint32_t ea = (uint32_t)(uintptr_t)buf;
        u32 i;
        for (i = 0; s[i] && i < bufsize - 1; i++) vm_write8(ea + i, (uint8_t)s[i]);
        vm_write8(ea + i, 0);
        break;
    }
    case CELL_SYSUTIL_SYSTEMPARAM_ID_CURRENT_USERNAME: {
        const char* s = "User";
        uint32_t ea = (uint32_t)(uintptr_t)buf;
        u32 i;
        for (i = 0; s[i] && i < bufsize - 1; i++) vm_write8(ea + i, (uint8_t)s[i]);
        vm_write8(ea + i, 0);
        break;
    }
    default:
        printf("[cellSysutil] GetSystemParamString: unknown id 0x%04X\n", id);
        vm_write8((uint32_t)(uintptr_t)buf, 0);
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
    /* `status` is a GUEST address (YDKJ's "FMOD BGM status query thread" passes its
     * own stack, e.g. 0xD011FF00) -- the old `*status = ...` dereferenced it as a
     * HOST pointer and segfaulted (same bug the GetSystemParamInt comment above
     * describes). It is also NOT an s32 out-param: the real API fills a
     * CellSysutilBgmPlaybackStatus (guest, big-endian):
     *   +0x00 u8  playerState        +0x01 u8 reserved[7]
     *   +0x08 char contentId[16]     +0x18 u8 reserved2[8]
     *   +0x20 s32 currentFadeRatio   +0x24 u8 reserved3[4]     (0x28 bytes)
     */
    uint32_t out_ea = (uint32_t)(uintptr_t)status;
    if (!out_ea)
        return CELL_SYSUTIL_ERROR_VALUE;

    for (uint32_t o = 0; o < 0x28; o += 4)
        vm_write32(out_ea + o, 0);
    vm_write8(out_ea + 0x00, (uint8_t)s_bgm_status);   /* playerState */
    vm_write32(out_ea + 0x20, 0);                      /* currentFadeRatio */
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

s32 cellSysCacheMount(char* param)
{
    /* The argument is a GUEST EA of CellSysCacheParam:
     *   +0x00 char cacheId[32]       (IN)
     *   +0x20 char getCachePath[136] (OUT)
     *   +0xA8 void* reserved
     * The old code strncpy'd through the raw EA as a host char* -> host AV
     * (LBP: write fault at 0xD00109F0, a guest thread-stack address). */
    uint32_t ea = (uint32_t)(uintptr_t)param;
    if (!ea)
        return CELL_EINVAL;

    char cache_id[33];
    memcpy(cache_id, vm_base + ea, 32);
    cache_id[32] = '\0';
    printf("[cellSysutil] SysCacheMount(id='%s')\n", cache_id);

    snprintf(s_cache_path, CELL_SYSCACHE_PATH_MAX, "/dev_hdd1/cache/%s", cache_id);
    size_t n = strlen(s_cache_path);
    if (n > 135) n = 135;
    memcpy(vm_base + ea + 0x20, s_cache_path, n);
    vm_base[ea + 0x20 + n] = '\0';
    s_cache_mounted = 1;

    return CELL_OK;   /* CELL_SYSCACHE_RET_OK_CLEARED */
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

    /* Both out-params are GUEST addresses (generic adapter passes raw EAs). */
    uint32_t type_ea  = (uint32_t)(uintptr_t)type;
    uint32_t title_ea = (uint32_t)(uintptr_t)titleId;

    if (type_ea)
        vm_write32(type_ea, CELL_DISCGAME_TYPE_HDD); /* pretend HDD game */

    if (title_ea && titleIdSize > 0) {
        /* Use the real title id (from PARAM.SFO at boot) instead of a placeholder. */
        extern const char* cellGame_get_title_id(void);
        const char* tid = cellGame_get_title_id();
        if (!tid || !tid[0]) tid = "GAME00000";
        size_t n = strlen(tid);
        if (n > titleIdSize - 1) n = titleIdSize - 1;
        memcpy(vm_base + title_ea, tid, n);
        vm_base[title_ea + n] = '\0';
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
