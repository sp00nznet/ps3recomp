/*
 * ps3recomp - cellSysutil HLE implementation
 *
 * System callbacks, parameter queries, BGM control, system cache,
 * and disc game check.
 */

#include "cellSysutil.h"
#include "ps3emu/guest_call.h"
#include <stdio.h>
#include <stdlib.h>
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

/* Dialog and other one-shot completions carry their own OPD, rather than a
 * registered sysutil slot. Detach one batch before invoking guest code so a
 * callback may safely enqueue another completion for the next poll. */
#ifdef _WIN32
#include <windows.h>
static SRWLOCK s_completion_lock = SRWLOCK_INIT;
#define COMPLETION_LOCK() AcquireSRWLockExclusive(&s_completion_lock)
#define COMPLETION_UNLOCK() ReleaseSRWLockExclusive(&s_completion_lock)
#define COMPLETION_TLS __declspec(thread)
#else
#include <pthread.h>
static pthread_mutex_t s_completion_lock = PTHREAD_MUTEX_INITIALIZER;
#define COMPLETION_LOCK() pthread_mutex_lock(&s_completion_lock)
#define COMPLETION_UNLOCK() pthread_mutex_unlock(&s_completion_lock)
#define COMPLETION_TLS _Thread_local
#endif

typedef struct GuestCompletion {
    struct GuestCompletion* next;
    u32 opd;
    u64 args[8];
} GuestCompletion;
static GuestCompletion* s_completion_head;
static GuestCompletion* s_completion_tail;

s32 cellSysutilQueueGuestCallbackArgs(u32 opd, const u64 args[8])
{
    if (!opd) return CELL_OK;
    GuestCompletion* item = malloc(sizeof(*item));
    if (!item) return (s32)CELL_ENOMEM;
    item->next = NULL; item->opd = opd; memcpy(item->args, args, sizeof(item->args));
    COMPLETION_LOCK();
    if (s_completion_tail) s_completion_tail->next = item;
    else s_completion_head = item;
    s_completion_tail = item;
    COMPLETION_UNLOCK();
    return CELL_OK;
}

s32 cellSysutilQueueGuestCallback(u32 opd, u64 arg0, u64 arg1)
{
    const u64 args[8] = {arg0, arg1, 0, 0, 0, 0, 0, 0};
    return cellSysutilQueueGuestCallbackArgs(opd, args);
}

static void drain_guest_completions(void)
{
    static COMPLETION_TLS int draining;
    if (draining || !g_ps3_guest_caller) return;
    draining = 1;
    COMPLETION_LOCK();
    GuestCompletion* item = s_completion_head;
    s_completion_head = s_completion_tail = NULL;
    COMPLETION_UNLOCK();
    while (item) {
        GuestCompletion* next = item->next;
        g_ps3_guest_caller(item->opd, item->args[0], item->args[1], item->args[2],
            item->args[3], item->args[4], item->args[5], item->args[6], item->args[7]);
        free(item);
        item = next;
    }
    draining = 0;
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

/* Whether the title actually pumps sysutil. cellMsgDialog needs to know: it
 * defers a dialog answer to this pump (which is where hardware delivers it) and
 * must not defer into a pump that never runs. */
static int s_pump_seen = 0;
int cellSysutil_pump_seen(void) { return s_pump_seen; }

s32 cellSysutilCheckCallback(void)
{
    drain_guest_completions();
    {
      if (!s_pump_seen) {
          s_pump_seen = 1;
          printf("[cellSysutil] CheckCallback: the title pumps sysutil%c", 10);
      }
    }
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
                               0, 0, 0, 0, 0);
        }
    }
    return CELL_OK;
}

/* value_ea is a GUEST address, and it is spelt as one. It used to be declared
 * s32* and immediately cast back to a guest EA, with a comment explaining that
 * dereferencing it as a host pointer faults -- true, and no help to a caller
 * who reads the declaration instead of the body. A host that translated the
 * argument first, which is what a pointer parameter asks for, handed this the
 * address of a host stack local; the cast truncated it to 32 bits and
 * vm_write32 stored four bytes at whatever guest address that spelt. It varies
 * with stack layout, so it lands somewhere different every run and nowhere
 * near the guest's variable, and nothing on the way says a word.
 *
 * A u32 cannot be passed a host pointer by accident. */
s32 cellSysutilGetSystemParamInt(s32 id, u32 value_ea)
{
    if (!value_ea)
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

    vm_write32(value_ea, (uint32_t)v);
    return CELL_OK;
}

/* buf_ea is a GUEST address, for the same reason and with the same history as
 * the Int form above: the body always treated it as one, and only the
 * declaration said otherwise. */
s32 cellSysutilGetSystemParamString(s32 id, u32 buf_ea, u32 bufsize)
{
    if (!buf_ea || bufsize == 0)
        return CELL_SYSUTIL_ERROR_VALUE;

    switch (id) {
    case CELL_SYSUTIL_SYSTEMPARAM_ID_NICKNAME: {
        const char* s = "ps3recomp_user";
        u32 i;
        for (i = 0; s[i] && i < bufsize - 1; i++) vm_write8(buf_ea + i, (uint8_t)s[i]);
        vm_write8(buf_ea + i, 0);
        break;
    }
    case CELL_SYSUTIL_SYSTEMPARAM_ID_CURRENT_USERNAME: {
        const char* s = "User";
        u32 i;
        for (i = 0; s[i] && i < bufsize - 1; i++) vm_write8(buf_ea + i, (uint8_t)s[i]);
        vm_write8(buf_ea + i, 0);
        break;
    }
    default:
        printf("[cellSysutil] GetSystemParamString: unknown id 0x%04X\n", id);
        vm_write8(buf_ea, 0);
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

/* cellDiscGameGetBootDiscInfo(CellDiscGameSystemFileParam* getParam)
 *
 * ONE argument, not three. This was declared (u32* type, char* titleId, u32
 * size), so the adapter handed it whatever r4 and r5 happened to hold at the
 * call -- and it wrote a title-id string through r4. In GT5P r4 still holds an
 * unrelated global the guest loaded for the cellDiscGameRegisterDiscChangeCallback
 * call two instructions earlier, so every boot stamped a string into 0x01025510.
 *
 * The real shape is a single out-struct the caller pre-zeroes (this title zeroes
 * 0x20 bytes of it) whose first field is titleId[CELL_DISCGAME_SYSP_TITLEID_SIZE].
 * The return code is what carries disc-vs-HDD: CELL_OK means booted from disc,
 * CELL_DISCGAME_ERROR_NOT_DISCBOOT (0x8002BD02) means not -- GT5P branches on
 * exactly those two values and ignores everything else.
 */
s32 cellDiscGameGetBootDiscInfo(void* getParam)
{
    uint32_t ea = (uint32_t)(uintptr_t)getParam;
    printf("[cellSysutil] DiscGameGetBootDiscInfo(param=0x%08X)\n", ea);

    if (ea) {
        extern const char* cellGame_get_title_id(void);
        const char* tid = cellGame_get_title_id();
        if (!tid || !tid[0]) tid = "GAME00000";
        size_t n = strlen(tid);
        if (n > CELL_DISCGAME_SYSP_TITLEID_SIZE - 1)
            n = CELL_DISCGAME_SYSP_TITLEID_SIZE - 1;
        memcpy(vm_base + ea, tid, n);
        vm_base[ea + n] = '\0';
    }

    /* PS3_DISCGAME_BOOT=0 reports NOT_DISCBOOT instead, for a title that is
     * genuinely installed to HDD. Disc is the default because that is the tree
     * cellFs actually serves. */
    { static int s_disc = -1;
      if (s_disc < 0) { const char* e = getenv("PS3_DISCGAME_BOOT");
                        s_disc = e ? atoi(e) : 1; }
      if (!s_disc) {
          printf("[cellSysutil]   -> NOT_DISCBOOT\n");
          return (s32)CELL_DISCGAME_ERROR_NOT_DISCBOOT;
      } }
    printf("[cellSysutil]   -> disc boot, titleId='%s'\n",
           (const char*)(vm_base + ea));
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
