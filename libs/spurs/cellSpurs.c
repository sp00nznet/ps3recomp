/*
 * ps3recomp - cellSpurs HLE implementation
 *
 * Provides the SPURS management API so games can call SPU task/workload
 * functions without crashing.  Full SPU execution requires recompiling
 * SPU programs; this layer provides the scheduling and management APIs.
 *
 * Tasks and workloads are tracked.  If a game provides PPU fallback
 * callbacks, those can be invoked through the task submission path.
 */

#include "cellSpurs.h"
#include "../../runtime/platform/win32_compat.h"
#include "../../runtime/ps3_log.h"
#include "spu_workload.h"   /* SPU image -> lifted-entry dispatch (runtime/spu) */
#include "spurs_taskset.h"  /* REAL BE CellSpursTaskset layout builders (fork Option-B) */
#include "spurs_instance.h" /* instance layout + the SPU-side kernel */
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base (guest mem) */
#include "ps3emu/guest_call.h"               /* shutdown-completion hooks */
#include <stdio.h>
#include <stdlib.h>   /* getenv -- an implicit decl returns int, truncating the pointer */
#include <string.h>
#include <stdint.h>
#include <sched.h>
#include <unistd.h>

/* Bridge the real (BE) taskset EA + selected taskId from CreateTask to the image-22
 * SPU dispatch (spu_workload.c), so spurs_pm_build_context can build the leaf's
 * SpursTasksetContext from the real taskset. Set right before dispatch_async (the PPU
 * create path is sequential here). Gated by YDKJ_REAL_TASKSET in the dispatch. */
uint32_t g_ydkj_real_taskset_ea = 0;
uint32_t g_ydkj_real_taskid     = 0;
uint32_t g_ydkj_real_spurs_ea   = 0;   /* real CellSpurs instance EA (for the taskset-policy handoff) */

/* Generic HLE adapter passes GUEST addresses; translate pointer args. CellSpurs
 * is treated opaquely by the game (passed back as a handle), so translating the
 * pointer is enough here. */
#define GUEST_PTR(p, T) ((T)((p) ? (void*)(vm_base + (uint32_t)(uintptr_t)(p)) : (void*)0))

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

/* ---------------------------------------------------------------------------
 * Internal workload tracking
 * -----------------------------------------------------------------------*/


typedef struct {
    int         in_use;
    u32         id;
    int         active;
    int         completed;
    s32         exitCode;
    void*       entryPoint;
    u64         argA;
} SpursTask;

static SpursTask     s_tasks[CELL_SPURS_MAX_TASK];
static u32           s_next_task_id = 0;

/* ---------------------------------------------------------------------------
 * Event flag sync side table
 *
 * The CellSpursEventFlag itself is 128 bytes of GAME-owned guest memory in
 * the REAL big-endian kernel layout (see EF_* offsets below) — the SPU side
 * (lifted task-library code) reads/writes it with DMA + atomics, so the PPU
 * HLE must operate on the same guest bytes, never a host struct. This side
 * table (keyed by guest EA) only adds the host mutex + condvar used to block
 * and wake PPU waiters.
 * -----------------------------------------------------------------------*/
#define MAX_EVENT_FLAGS 64

typedef struct {
    uint32_t            ea;      /* guest EA of the 128-byte flag */
#ifdef _WIN32
    CRITICAL_SECTION    cs;
    CONDITION_VARIABLE  cv;
#else
    pthread_mutex_t     mtx;
    pthread_cond_t      cond;
#endif
    int                 initialized;
} EventFlagSync;

static EventFlagSync s_ef_sync[MAX_EVENT_FLAGS];

/* PPU threads inside cellSpursEventFlagWait. The SPU sets a flag with
 * GETLLAR/PUTLLC on the guest struct, which never touched the waiter's
 * condition variable, so every wait ran out its 2 ms poll: FMOD's mixer waits
 * on its SPU task once or twice per 5.3 ms audio block, fell behind, and
 * GH3's songs played ~60% silence (heard as static). */
static volatile long s_ef_ppu_waiters;
static void ef_line_committed(uint32_t line)
{
    if (!s_ef_ppu_waiters) return;
    for (int i = 0; i < MAX_EVENT_FLAGS; i++) {
        EventFlagSync* s = &s_ef_sync[i];
        if (!s->initialized || (s->ea & ~127u) != line) continue;
        /* Take the waiter's lock so the wake cannot fall between its check of
         * the flag and its sleep. Caller holds no lock-line lock. */
#ifdef _WIN32
        EnterCriticalSection(&s->cs); WakeAllConditionVariable(&s->cv); LeaveCriticalSection(&s->cs);
#else
        pthread_mutex_lock(&s->mtx); pthread_cond_broadcast(&s->cond); pthread_mutex_unlock(&s->mtx);
#endif
    }
}

static EventFlagSync* ef_sync_find(uint32_t ea)
{
    for (int i = 0; i < MAX_EVENT_FLAGS; i++) {
        if (s_ef_sync[i].initialized && s_ef_sync[i].ea == ea)
            return &s_ef_sync[i];
    }
    return NULL;
}

static EventFlagSync* ef_sync_alloc(uint32_t ea)
{
    for (int i = 0; i < MAX_EVENT_FLAGS; i++) {
        if (!s_ef_sync[i].initialized) {
            s_ef_sync[i].ea = ea;
            { extern void (*g_spu_line_commit_hook)(uint32_t);
              g_spu_line_commit_hook = ef_line_committed; }
#ifdef _WIN32
            InitializeCriticalSection(&s_ef_sync[i].cs);
            InitializeConditionVariable(&s_ef_sync[i].cv);
#else
            pthread_mutex_init(&s_ef_sync[i].mtx, NULL);
            pthread_cond_init(&s_ef_sync[i].cond, NULL);
#endif
            s_ef_sync[i].initialized = 1;
            return &s_ef_sync[i];
        }
    }
    return NULL;
}

/* Lenient lookup: a flag touched before we saw its Initialize (alternate init
 * paths, e.g. taskset2) still gets a sync slot instead of an error. */
static EventFlagSync* ef_sync_get(uint32_t ea)
{
    EventFlagSync* s = ef_sync_find(ea);
    if (!s) {
        s = ef_sync_alloc(ea);
        if (s)
            fprintf(stderr, "[cellSpurs] event flag 0x%08X used before Initialize "
                            "-- sync slot auto-allocated\n", ea);
    }
    return s;
}

static void ef_sync_free(EventFlagSync* sync)
{
    if (!sync) return;
#ifdef _WIN32
    DeleteCriticalSection(&sync->cs);
    /* CONDITION_VARIABLE has no destroy on Windows */
#else
    pthread_mutex_destroy(&sync->mtx);
    pthread_cond_destroy(&sync->cond);
#endif
    sync->ea = 0;
    sync->initialized = 0;
}

static inline void ef_lock(EventFlagSync* s)
{
#ifdef _WIN32
    EnterCriticalSection(&s->cs);
#else
    pthread_mutex_lock(&s->mtx);
#endif
}

static inline void ef_unlock(EventFlagSync* s)
{
#ifdef _WIN32
    LeaveCriticalSection(&s->cs);
#else
    pthread_mutex_unlock(&s->mtx);
#endif
}

/* Returns 1 if signaled, 0 if it timed out after `ms`. */
static inline int ef_wait_timed(EventFlagSync* s, unsigned ms)
{
#ifdef _WIN32
    return SleepConditionVariableCS(&s->cv, &s->cs, ms) ? 1 : 0;
#else
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000; ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(&s->cond, &s->mtx, &ts) == 0 ? 1 : 0;
#endif
}

static inline void ef_broadcast(EventFlagSync* s)
{
#ifdef _WIN32
    WakeAllConditionVariable(&s->cv);
#else
    pthread_cond_broadcast(&s->cond);
#endif
}

/* ---------------------------------------------------------------------------
 * REAL CellSpursEventFlag guest layout (128 bytes, big-endian; RPCS3
 * cellSpurs.h is the reference). The SPU task library manipulates this exact
 * layout with GETLLAR/PUTLLC, so every PPU-side access goes through vm_*.
 * -----------------------------------------------------------------------*/
#define EF_EVENTS            0x00u  /* be u16: the event bits */
#define EF_SPU_PENDING_RECV  0x02u  /* be u16: slot bits with met conditions */
#define EF_PPU_WAIT_MASK     0x04u  /* be u16: blocked PPU thread's mask */
#define EF_PPU_WAIT_SLOTMODE 0x06u  /* u8: hi4 = wait slot, lo4 = wait mode */
#define EF_PPU_PENDING_RECV  0x07u  /* u8: 1 when the PPU waiter's cond met */
#define EF_SPU_USED_SLOTS    0x08u  /* be u16: wait slots in use (bit 0x8000>>s) */
#define EF_SPU_WAIT_MODE     0x0Au  /* be u16: per-slot mode (1 = AND) */
#define EF_SPU_PORT          0x0Cu  /* u8 */
#define EF_IS_IWL            0x0Du  /* u8: addr is a wkl instead of taskset */
#define EF_DIRECTION         0x0Eu  /* u8: CELL_SPURS_EVENT_FLAG_* direction */
#define EF_CLEAR_MODE        0x0Fu  /* u8: 0 = AUTO, 1 = MANUAL */
#define EF_SPU_WAIT_MASK_ARR 0x10u  /* be u16 [16]: per-slot wait masks */
#define EF_PENDING_RECV_EVT  0x30u  /* be u16 [16]: events handed to waiters */
#define EF_WAITING_TASK_ID   0x50u  /* u8 [16]: waiting task ids */
#define EF_WAITING_WKL_ID    0x60u  /* u8 [16]: waiting workload ids */
#define EF_ADDR              0x70u  /* be u64: taskset EA (isIwl=0) */
#define EF_EVENT_PORT_ID     0x78u  /* be u32 */
#define EF_EVENT_QUEUE_ID    0x7Cu  /* be u32 */
#define EF_GUEST_SIZE        0x80u

/* Layer-2 hook: wake an SPU task that registered a wait slot and slept via
 * the taskset syscall. Implemented in runtime/spu (task park/wake); the
 * event-flag core only reports WHO must wake. */
extern void spu_taskset_signal_task(uint32_t taskset_ea, uint32_t taskId);

/* Core Set protocol on the guest struct (sync lock held). Mirrors the real
 * kernel/RPCS3 semantics: satisfy registered SPU-task wait slots (slot s uses
 * bit 0x8000>>s in the used/pending/mode words), hand each its received
 * events, auto-clear consumed bits, then signal the tasks. PPU waiters are
 * poll-based here (ef_wait_timed ticks re-check EF_EVENTS), so no PPU
 * wait-slot registration is needed — and deliberately NOT written, so the
 * SPU-side lifted Set code takes its "no PPU waiter" path and simply ORs
 * bits that our poll then observes. */
/* The flag struct is ONE 128-byte line the SPU task library updates with
 * GETLLAR/PUTLLC. A PPU read-modify-write of it must hold the lock-line lock
 * for its whole span, or a PUTLLC landing between the read and the write is
 * silently undone: an SPU wait registered there is lost, nothing ever signals
 * the task, and its PPU partner blocks for good (GH3: FMOD's mixer deadlocked
 * at boot in 1 run of ~4). Raw accesses inside: vm_write16 would take the
 * same (non-recursive) lock. The end notifies reserving SPUs, under the lock
 * as spu_coh_notify_write expects. */
extern void spu_lockline_lock(void);
extern void spu_lockline_unlock(void);
extern int  spu_coh_is_reserved(uint32_t);
extern void spu_coh_notify_write(uint32_t);
static inline u16 ef_r16(uint32_t a) { const uint8_t* p = vm_base + a; return (u16)((p[0] << 8) | p[1]); }
static inline void ef_w16(uint32_t a, u16 v) { uint8_t* p = vm_base + a; p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void ef_line_end(uint32_t ea)
{
    if (spu_coh_is_reserved(ea & ~127u)) spu_coh_notify_write(ea & ~127u);
    spu_lockline_unlock();
}

static void spurs_ef_set_locked(uint32_t ea, u16 bits)
{
    spu_lockline_lock();
    u16 events        = ef_r16(ea + EF_EVENTS);
    u16 used          = (u16)(ef_r16(ea + EF_SPU_USED_SLOTS) &
                              ~ef_r16(ea + EF_SPU_PENDING_RECV));
    u16 waitmode      = ef_r16(ea + EF_SPU_WAIT_MODE);
    u16 eventsToClear = 0;
    u16 pendingRecv   = 0;

    for (int s = 0; s < CELL_SPURS_EVENT_FLAG_MAX_WAIT_SLOTS; s++) {
        u16 bit = (u16)(0x8000u >> s);
        if (!(used & bit)) continue;
        u16 mask = ef_r16(ea + EF_SPU_WAIT_MASK_ARR + 2u * s);
        u16 rel  = (u16)((events | bits) & mask);
        int mode_and = (waitmode & bit) != 0;
        if ((mask & ~rel) == 0 || (!mode_and && rel != 0)) {
            eventsToClear |= rel;
            pendingRecv   |= bit;
            ef_w16(ea + EF_PENDING_RECV_EVT + 2u * s, rel);
        }
    }

    events = (u16)(events | bits);
    if (pendingRecv) {
        ef_w16(ea + EF_SPU_PENDING_RECV,
               (u16)(ef_r16(ea + EF_SPU_PENDING_RECV) | pendingRecv));
        if (vm_read8(ea + EF_CLEAR_MODE) == CELL_SPURS_EVENT_FLAG_CLEAR_AUTO)
            events = (u16)(events & ~eventsToClear);
    }
    ef_w16(ea + EF_EVENTS, events);
    ef_line_end(ea);   /* before signalling: signal_task takes its own locks */

    for (int s = 0; s < CELL_SPURS_EVENT_FLAG_MAX_WAIT_SLOTS; s++) {
        if (pendingRecv & (0x8000u >> s)) {
            uint32_t taskset_ea = (uint32_t)vm_read64(ea + EF_ADDR);
            u32 taskId = vm_read8(ea + EF_WAITING_TASK_ID + s);
            if (ps3_log_verbose())
                fprintf(stderr, "[cellSpurs] EventFlagSet 0x%08X satisfies SPU task "
                        "slot %d (taskset=0x%08X task=%u)\n", ea, s, taskset_ea, taskId);
            spu_taskset_signal_task(taskset_ea, taskId);
        }
    }

    /* SPURS_EF_WAKE_PARKED: a task that parked in WAIT_SIGNAL WITHOUT registering
     * a wait slot in this flag is unreachable by the loop above, which only
     * signals slots present in `used`. YDKJ CRI tasks do exactly that, so a
     * PPU->SPU Set (direction 2) delivered nothing and they slept forever. Same
     * failure class canersaka documented for SPURS queues in Yakuza Dead Souls:
     * the HLE woke only its host condvar, which lifted SPU tasks never wait on.
     * Signalling a task that was not waiting on THIS flag is safe -- signals are
     * latched in guest state and every wait loop re-checks its own predicate. */
    /* Record the bits this Set owes the task waiting on (flag - 0x80),
     * unconditionally. Gating this on EF_DIRECTION made delivery depend on
     * Initialize having already run, and the CRI flags are Set before that --
     * so the post silently never happened. An unclaimed post is harmless: it is
     * only ever consumed by a task that names this exact object in its wait. */
    if (ea > 0x80u) {
        uint32_t obj = ea - 0x80u;
        /* Write NOW *and* record for consume time. The two orderings are both
         * real and neither alone is enough: if the task has not read yet the
         * immediate write is what it sees, and if it is parked (or took the
         * latched-wake path and never parked at all) the WAIT_SIGNAL handler
         * replays it just before returning. Writing the same bits twice is
         * idempotent, so covering both costs nothing and removes the race that
         * made this fire in only 1 of 4 runs. */
        { static int s_od = -1;
          if (s_od < 0) s_od = getenv("SPURS_EF_OBJ_DELIVER") ? 1 : 0;
          if (s_od) {
              vm_write16(obj + EF_EVENTS, bits);
              for (uint32_t s = 0; s < 16; s++)
                  vm_write16(obj + EF_PENDING_RECV_EVT + 2u * s, bits);
          } }
        { extern void spu_ef_bits_post(uint32_t, uint16_t);
          spu_ef_bits_post(obj, bits); }
    }
    if (!pendingRecv) {
        static int s_wp = -1;
        if (s_wp < 0) s_wp = getenv("SPURS_EF_WAKE_PARKED") ? 1 : 0;

        if (s_wp && vm_read8(ea + EF_DIRECTION) == 2) {
            uint32_t taskset_ea = (uint32_t)vm_read64(ea + EF_ADDR);
            /* The task waiting for THIS flag parked on object (flag - 0x80):
             * YDKJ CRI obj 0x006B4500/4780/4A00 <-> flag 0x006B4580/4800/4A80.
             * Fall back to any parked task of the taskset if none matches. */
            extern int spu_taskset_signal_parked_obj(uint32_t, uint32_t);

            int woke = spu_taskset_signal_parked_obj(taskset_ea, ea - 0x80u);
            if (!woke) woke = spu_taskset_signal_parked_obj(taskset_ea, 0);
            if (!woke) {   /* nobody parked yet -- do not lose the wakeup */
                extern void spu_taskset_latch_wake(uint32_t);
                spu_taskset_latch_wake(taskset_ea);
            }
            static int _n = 0;
            if (woke && __atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 12)
                fprintf(stderr, "[cellSpurs] EventFlagSet 0x%08X woke %d parked "
                                "task(s) on taskset 0x%08X (no wait slot)\n",
                        ea, woke, taskset_ea);
        }
    }
}

/* SPU-side entry (Layer 2): a task's flag Set arriving via the taskset
 * syscall or a runtime bridge. Same protocol, takes the lock itself. */
void spurs_ef_set_from_spu(uint32_t flag_ea, uint16_t bits)
{
    EventFlagSync* sync = ef_sync_get(flag_ea);
    if (!sync) return;
    ef_lock(sync);
    spurs_ef_set_locked(flag_ea, (u16)bits);
    ef_broadcast(sync);
    ef_unlock(sync);
}

/* =========================================================================
 * SPURS core
 *
 * The CellSpurs instance (0x2000 bytes of GAME-owned guest memory) is real
 * shared state: the game's engine pokes it with INLINED atomics (readyCount
 * stores, signal bits) and the policy modules DMA it from the SPU side. So
 * it must hold the REAL big-endian kernel layout — never a host struct.
 * Verified offsets (RPCS3 cellSpurs.h contract):
 *   +0x00 wklReadyCount1[16] (u8/wid)   +0x80 wklState1[16] (u8: 2=runnable)
 *   +0x10 wklIdleSpuCount[16]           +0x90 wklStatus1[16]
 *   +0x20 wklCurrentContention[16]      +0xA0 wklEvent1[16]
 *   +0x40 wklMinContention[16]          +0xB0 wklEnabled (be u32, bit 31-wid)
 *   +0x50 wklMaxContention[16]          +0xBD sysSrvMsgUpdateWorkload (u8)
 *   +0x60 wklFlag (be u64)              +0xB00 wklInfo1[16] (32B each:
 *   +0x70 wklSignal1 (be u16)                  addr u64, arg u64, size u32,
 *   +0x76 nSpus (u8)                           uniqueId u8, prio[8] @+0x18)
 * Our own bookkeeping lives in a host side-table keyed by instance EA.
 * A host "kernel" thread per instance polls readyCount/signal and runs the
 * workload's policy module (spurs_policy.c) — the virtual SPU.
 * =====================================================================*/

/* CellSpursAttribute (512 bytes, 8-byte aligned, big-endian). */
enum {
    SATTR_REVISION     = 0x00,
    SATTR_SDK_VERSION  = 0x04,
    SATTR_NSPUS        = 0x08,
    SATTR_SPU_PRIO     = 0x0C,
    SATTR_PPU_PRIO     = 0x10,
    SATTR_EXIT_NO_WORK = 0x14,     /* u8 */
    SATTR_PREFIX       = 0x15,     /* char[15], not NUL-terminated */
    SATTR_PREFIX_SIZE  = 0x24,
    SATTR_FLAGS        = 0x28,
    SATTR_CONTAINER    = 0x2C,
    SATTR_SIZE         = 0x200,
};
#define SPURS_SAF_EXIT_IF_NO_WORK 0x1u
#define SPURS_SAF_SECOND_VERSION  0x4u
#define SPURS_NAME_MAX            15u
#define SPURS_KERNEL_PORT         0x10u  /* the instance's own port: dynamic attach starts above it */

/* libsre argument checks, verified against the firmware
 * (tests/conformance/spurs/t_core): the instance must be non-null and
 * 128-byte aligned. Core calls report CELL_SPURS_CORE_ERROR_*, workload-level
 * calls CELL_SPURS_POLICY_MODULE_ERROR_* (same low byte). */
static s32 spurs_check_core(u32 ea)
{
    if (!ea) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (ea & 127) return CELL_SPURS_CORE_ERROR_ALIGN;
    return CELL_OK;
}

static s32 spurs_check_pm(u32 ea)
{
    if (!ea) return CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER;
    if (ea & 127) return CELL_SPURS_POLICY_MODULE_ERROR_ALIGN;
    return CELL_OK;
}

static int spurs_wkl_enabled(u32 ea, u32 wid)
{
    return (vm_read32(ea + SPURS_WKL_ENABLED) & (0x80000000u >> wid)) != 0;
}

/* Workload calls: instance, then workload id range, then that the workload
 * exists (enabled bit). `ok_ptr` is the call's own out-pointer, checked
 * together with the instance. */
static s32 spurs_check_wid_arg(u32 ea, int ok_ptr, u32 wid, int bad_arg)
{
    if (!ea || !ok_ptr) return CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER;
    if (ea & 127) return CELL_SPURS_POLICY_MODULE_ERROR_ALIGN;
    if (wid >= CELL_SPURS_MAX_WORKLOAD || bad_arg) return CELL_SPURS_POLICY_MODULE_ERROR_INVAL;
    if (!spurs_wkl_enabled(ea, wid)) return CELL_SPURS_POLICY_MODULE_ERROR_SRCH;
    return CELL_OK;
}
#define spurs_check_wid(ea, ok_ptr, wid) spurs_check_wid_arg(ea, ok_ptr, wid, 0)

/* ...and, for calls that change scheduling, that it is runnable (state 2). */
static s32 spurs_check_wid_runnable(u32 ea, int ok_ptr, u32 wid, int bad_arg)
{
    s32 rc = spurs_check_wid_arg(ea, ok_ptr, wid, bad_arg);
    if (rc) return rc;
    if (*(vm_base + ea + SPURS_WKL_STATE1 + wid) != 2) return CELL_SPURS_POLICY_MODULE_ERROR_STAT;
    return CELL_OK;
}

#define MAX_SPURS_INST 4
static struct SpursInst {
    u32           ea;          /* 0 = free */
    u32           nspus;
    char          prefix[16];
} s_inst[MAX_SPURS_INST];

static struct SpursInst* spurs_inst_find(u32 ea)
{
    for (int i = 0; i < MAX_SPURS_INST; i++)
        if (s_inst[i].ea == ea) return &s_inst[i];
    return NULL;
}

static u32 s_spurs_port_queue[64];   /* SPU port -> lv2 queue; see spurs_port_queue */

static s32 spurs_initialize_common(u32 spurs_ea, u32 revision, u32 sdk_version,
                                   s32 nspus, s32 spu_prio, s32 ppu_prio, u32 flags,
                                   u32 prefix_ea, u32 prefix_size)
{
    if (prefix_size > SPURS_NAME_MAX)
        return CELL_SPURS_CORE_ERROR_INVAL;
    /* What libsre forwards to sys_spu_thread_group_create / sys_ppu_thread_create
     * and those reject: 1..6 SPUs, SPU group priority 16..255, PPU priority
     * 0..3071 -- all CELL_EINVAL, unchanged. */
    if (nspus < 1 || nspus > 6 || spu_prio < 16 || spu_prio > 255 ||
        ppu_prio < 0 || ppu_prio > 3071)
        return (s32)CELL_EINVAL;

    struct SpursInst* si = spurs_inst_find(spurs_ea);
    if (!si) {
        for (int i = 0; i < MAX_SPURS_INST; i++)
            if (!s_inst[i].ea) { si = &s_inst[i]; break; }
    }
    if (!si) return CELL_SPURS_CORE_ERROR_NOMEM;

    si->ea    = spurs_ea;
    si->nspus = (u32)nspus;
    memset(si->prefix, 0, sizeof(si->prefix));
    if (prefix_ea && prefix_size) memcpy(si->prefix, vm_base + prefix_ea, prefix_size);

    /* Real BE instance: zero it, then the live fields. (The global
     * workload table is NOT wiped here -- the title may init several SPURS
     * instances before adding workloads to any of them.) */
    memset(vm_base + spurs_ea, 0, SPURS_INST_SIZE);
    *(vm_base + spurs_ea + SPURS_NSPUS) = (u8)si->nspus;
    /* wids 16..31 are SPURS2-only: marked taken so the free-wid search
     * (first clear bit) ends at 16; a full instance reports wid 32. */
    vm_write32(spurs_ea + SPURS_WKL_ENABLED, 0x0000FFFFu);
    vm_write32(spurs_ea + SPURS_WKL_FLAG + 0x0C, 0xFFFFFFFFu);      /* flag word */
    *(vm_base + spurs_ea + SPURS_WKL_FLAG_RCV) = 0xFF;              /* no receiver */
    vm_write64(spurs_ea + SPURS_PPU0, 0xFFFFFFFFull);
    vm_write64(spurs_ea + SPURS_PPU1, 0xFFFFFFFFull);
    vm_write32(spurs_ea + SPURS_CFG_FLAGS, flags);
    vm_write32(spurs_ea + SPURS_SPU_PRIO, (u32)spu_prio);
    vm_write32(spurs_ea + SPURS_PPU_PRIO, (u32)ppu_prio);
    if (prefix_ea && prefix_size)
        memcpy(vm_base + spurs_ea + SPURS_PREFIX, vm_base + prefix_ea, prefix_size);
    *(vm_base + spurs_ea + SPURS_PREFIX_SIZE) = (u8)prefix_size;
    vm_write32(spurs_ea + SPURS_REVISION, revision);
    vm_write32(spurs_ea + SPURS_SDK_VERSION, sdk_version);

    /* flags1 and the system service's per-SPU preemption slots */
    if (flags & SPURS_SAF_EXIT_IF_NO_WORK) vm_base[spurs_ea + SPURS_FLAGS1] |= SPURS_SF1_EXIT_IF_NO_WORK;
    memset(vm_base + spurs_ea + 0xC0, 0xFF, 8);         /* sysSrvPreemptWklId[8] */
    vm_base[spurs_ea + SPURS_SPU_PORT] = SPURS_KERNEL_PORT;

    /* The SPU thread group: one SPURS kernel per SPU (spurs_kernel.c). */
    spurs_kernel_stop(spurs_ea);                        /* re-initialised without Finalize */
    spurs_kernel_start(spurs_ea, si->nspus);
    printf("[cellSpurs] Initialize \"%s\" ea=0x%08X nSpus=%u (real BE instance + kernel poll)\n",
           si->prefix, spurs_ea, si->nspus);
    return CELL_OK;
}

s32 cellSpursInitialize(CellSpurs* spurs, s32 nSpus, s32 spuPriority,
                        s32 ppuPriority, u8 exitIfNoWork)
{
    const u32 spurs_ea = (u32)(uintptr_t)spurs;
    s32 rc = spurs_check_core(spurs_ea);
    if (rc) return rc;
    return spurs_initialize_common(spurs_ea, 0, 0, nSpus, spuPriority, ppuPriority,
                                   exitIfNoWork ? SPURS_SAF_EXIT_IF_NO_WORK : 0, 0, 0);
}

s32 cellSpursInitializeWithAttribute(CellSpurs* spurs,
                                     const CellSpursAttribute* attr)
{
    const u32 spurs_ea = (u32)(uintptr_t)spurs, a = (u32)(uintptr_t)attr;
    if (!spurs_ea || !a) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if ((spurs_ea & 127) || (a & 7)) return CELL_SPURS_CORE_ERROR_ALIGN;
    u32 flags = vm_read32(a + SATTR_FLAGS);
    if (*(vm_base + a + SATTR_EXIT_NO_WORK)) flags |= SPURS_SAF_EXIT_IF_NO_WORK;
    return spurs_initialize_common(spurs_ea, vm_read32(a + SATTR_REVISION),
                                   vm_read32(a + SATTR_SDK_VERSION),
                                   (s32)vm_read32(a + SATTR_NSPUS),
                                   (s32)vm_read32(a + SATTR_SPU_PRIO),
                                   (s32)vm_read32(a + SATTR_PPU_PRIO), flags,
                                   a + SATTR_PREFIX, vm_read32(a + SATTR_PREFIX_SIZE));
}

static void spurs_cancel_attached_queues(u32 spurs_ea);  /* defined with the queue table */

s32 cellSpursFinalize(CellSpurs* spurs)
{
    { s32 rc = spurs_check_core((u32)(uintptr_t)spurs); if (rc) return rc; }
    /* libsre refuses while any workload is still added: they must be shut
     * down and removed first. */
    if (spurs_inst_find((u32)(uintptr_t)spurs) &&
        (vm_read32((u32)(uintptr_t)spurs + SPURS_WKL_ENABLED) & 0xFFFF0000u))
        return CELL_SPURS_CORE_ERROR_BUSY;

    /* After this, no job completion can ever reach the attached queues, so a
     * worker parked on one with an infinite timeout would sleep forever -- and
     * the shutdown that called us then blocks in sys_ppu_thread_join waiting
     * for that worker to notice it should quit. Tokyo Jungle wedges exactly
     * there: "terminate audio thread (6)" then join(6), with tid 6 in
     * event_queue_receive(q=3) and the title never rendering again. Releasing
     * the waiters lets the receive fail, which is the path its loop already
     * handles -- an error return leaves the loop and the thread exits.
     *
     * Done BEFORE the instance lookup: that lookup fails here, and a failed
     * handle is no reason to strand a thread. */
    spurs_cancel_attached_queues((u32)(uintptr_t)spurs);

    struct SpursInst* si = spurs_inst_find((u32)(uintptr_t)spurs);
    if (!si)
        return CELL_SPURS_CORE_ERROR_STAT;

    {   const u64 bits = vm_read64(si->ea + SPURS_PORT_BITS);
        for (u32 p = 0; p < 64; p++) if (bits >> p & 1) s_spurs_port_queue[p] = 0; }
    spurs_kernel_stop(si->ea);
    printf("[cellSpurs] Finalize(ea=0x%08X)\n", si->ea);
    si->ea = 0;   /* kernel thread sees a dead instance and idles */
    return CELL_OK;
}

/* The SDK's cellSpursAttributeInitialize() is an inline wrapper over this
 * export (NID 0x95180230); it fills the real big-endian attribute. */
s32 _cellSpursAttributeInitialize(CellSpursAttribute* attr, u32 revision,
                                  u32 sdkVersion, u32 nSpus, s32 spuPriority,
                                  s32 ppuPriority, u8 exitIfNoWork)
{
    const u32 a = (u32)(uintptr_t)attr;
    if (!a) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (a & 7) return CELL_SPURS_CORE_ERROR_ALIGN;
    memset(vm_base + a, 0, SATTR_SIZE);
    vm_write32(a + SATTR_REVISION, revision);
    vm_write32(a + SATTR_SDK_VERSION, sdkVersion);
    vm_write32(a + SATTR_NSPUS, nSpus);
    vm_write32(a + SATTR_SPU_PRIO, (u32)spuPriority);
    vm_write32(a + SATTR_PPU_PRIO, (u32)ppuPriority);
    *(vm_base + a + SATTR_EXIT_NO_WORK) = exitIfNoWork ? 1 : 0;
    return CELL_OK;
}

s32 cellSpursAttributeInitialize(CellSpursAttribute* attr, s32 nSpus,
                                 s32 spuPriority, s32 ppuPriority,
                                 u8 exitIfNoWork)
{
    return _cellSpursAttributeInitialize(attr, 2, 0, (u32)nSpus, spuPriority,
                                         ppuPriority, exitIfNoWork);
}

s32 cellSpursAttributeSetNamePrefix(CellSpursAttribute* attr,
                                    const char* prefix, u32 size)
{
    const u32 a = (u32)(uintptr_t)attr, pe = (u32)(uintptr_t)prefix;
    if (!a || !pe) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (a & 7) return CELL_SPURS_CORE_ERROR_ALIGN;
    if (size > SPURS_NAME_MAX) return CELL_SPURS_CORE_ERROR_INVAL;
    memcpy(vm_base + a + SATTR_PREFIX, vm_base + pe, size);
    vm_write32(a + SATTR_PREFIX_SIZE, size);
    return CELL_OK;
}

s32 cellSpursAttributeSetSpuThreadGroupType(CellSpursAttribute* attr,
                                            s32 type)
{
    (void)type;
    const u32 a = (u32)(uintptr_t)attr;
    if (!a) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (a & 7) return CELL_SPURS_CORE_ERROR_ALIGN;
    return CELL_OK;
}

s32 cellSpursAttributeEnableSpuPrintfIfAvailable(CellSpursAttribute* attr)
{
    const u32 a = (u32)(uintptr_t)attr;
    if (!a) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (a & 7) return CELL_SPURS_CORE_ERROR_ALIGN;
    return CELL_OK;
}

s32 cellSpursGetNumSpuThread(const CellSpurs* spurs, u32* nThreads)
{
    const u32 ea = (u32)(uintptr_t)spurs;
    if (!ea || !nThreads) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (ea & 127) return CELL_SPURS_CORE_ERROR_ALIGN;
    vm_write32((u32)(uintptr_t)nThreads, *(vm_base + ea + SPURS_NSPUS));
    return CELL_OK;
}

s32 cellSpursGetSpuThreadId(u64 spurs_ea, u64 thread_ea, u64 nThreads_ea)
{
    const u32 ea = (u32)spurs_ea;
    if (!ea || !thread_ea || !nThreads_ea) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (ea & 127) return CELL_SPURS_CORE_ERROR_ALIGN;
    u32 n = vm_read32((u32)nThreads_ea), have = *(vm_base + ea + SPURS_NSPUS);
    if (n > have) n = have;
    for (u32 i = 0; i < n; i++)
        vm_write32((u32)thread_ea + 4 * i, vm_read32(ea + SPURS_SPUS + 4 * i));
    vm_write32((u32)nThreads_ea, n);
    return CELL_OK;
}

s32 cellSpursGetSpuThreadGroupId(u64 spurs_ea, u64 group_ea)
{
    const u32 ea = (u32)spurs_ea;
    if (!ea || !group_ea) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (ea & 127) return CELL_SPURS_CORE_ERROR_ALIGN;
    vm_write32((u32)group_ea, vm_read32(ea + SPURS_SPU_TG));
    return CELL_OK;
}

/* Core-family workload checks (SetMaxContention / SetPriorities): instance,
 * id range, existence. */
static s32 spurs_check_wid_core(u32 ea, u32 wid)
{
    s32 rc = spurs_check_core(ea);
    if (rc) return rc;
    if (wid >= CELL_SPURS_MAX_WORKLOAD) return CELL_SPURS_CORE_ERROR_INVAL;
    if (!spurs_wkl_enabled(ea, wid)) return CELL_SPURS_CORE_ERROR_SRCH;
    return CELL_OK;
}

s32 cellSpursSetMaxContention(CellSpurs* spurs, CellSpursWorkloadId wid,
                              u32 maxContention)
{
    const u32 ea = (u32)(uintptr_t)spurs;
    s32 rc = spurs_check_wid_core(ea, wid);
    if (rc) return rc;
    *(vm_base + ea + SPURS_WKL_MAXCONT + wid) = (u8)(maxContention > 8 ? 8 : maxContention);
    spurs_kernel_notify(ea);
    return CELL_OK;
}

s32 cellSpursSetPriorities(CellSpurs* spurs, CellSpursWorkloadId wid,
                           const u8* priorities)
{
    const u32 ea = (u32)(uintptr_t)spurs;
    s32 rc = spurs_check_wid_core(ea, wid);
    if (rc) return rc;
    /* libsre checks the table pointer only after finding the workload. */
    if (!priorities) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    const u8* priorities_h = GUEST_PTR(priorities, const u8*);
    for (int i = 0; i < 8; i++)
        if (priorities_h[i] > 15) return CELL_SPURS_CORE_ERROR_INVAL;
    memcpy(vm_base + ea + SPURS_WKL_INFO1 + wid * SPURS_WKL_INFO_SZ + 0x18, priorities_h, 8);
    /* every SPU re-reads its priorities through the system service */
    __atomic_store_n(vm_base + ea + SPURS_SYSSRV_MSG, 0xFF, __ATOMIC_SEQ_CST);
    __atomic_store_n(vm_base + ea + SPURS_SYSSRV_MESSAGE, 0xFF, __ATOMIC_SEQ_CST);
    spurs_kernel_notify(ea);
    return CELL_OK;
}

/* The lv2 event queues the app attached to SPURS, and the port we handed back.
 * A title may attach more than one (Tokyo Jungle attaches two), so keep them
 * all -- storing a single id let the second attach hide the first. */
#define MAX_SPURS_QUEUES 8
#define SPURS_EVENT_PORT 0u
extern int sys_event_queue_push_by_id(uint32_t queue_id, uint64_t source,
                                      uint64_t data1, uint64_t data2, uint64_t data3);
extern void sys_event_queue_cancel_by_id(uint32_t queue_id);
extern void sys_event_queue_uncancel_by_id(uint32_t queue_id);

static u32 s_spurs_event_queue[MAX_SPURS_QUEUES];
/* WHICH SPURS instance attached each queue. A title with more than one instance
 * -- Tokyo Jungle boots one for its data install and keeps a second for audio --
 * finalises them separately, and cancelling every queue on any Finalize kills
 * the surviving instance's queues too. That reads to the title as "the SPU
 * stopped answering": its sound engine's receives fail with ECANCELED and it
 * tears itself down. Cancel only the queues the instance being finalised owns. */
static u32 s_spurs_event_queue_owner[MAX_SPURS_QUEUES];
static int s_spurs_event_queue_n = 0;

/* Release anyone blocked on the completion queues SPURS attached; see
 * cellSpursFinalize. */
static void spurs_cancel_attached_queues(u32 spurs_ea)
{
    int keep = 0;
    for (int i = 0; i < s_spurs_event_queue_n; i++) {
        /* owner 0 means "attached before we tracked owners" -- cancel those on
         * any finalize, which is the old behaviour and the safe default. */
        u32 owner = s_spurs_event_queue_owner[i];
        if (spurs_ea == 0 || owner == 0 || owner == spurs_ea) {
            sys_event_queue_cancel_by_id(s_spurs_event_queue[i]);
        } else {
            s_spurs_event_queue[keep]       = s_spurs_event_queue[i];
            s_spurs_event_queue_owner[keep] = owner;
            keep++;
        }
    }
    s_spurs_event_queue_n = keep;
}

/* SPU user-event port -> lv2 queue, for SPU code running under SPURS.
 *
 * A SPURS task signals the PPU with send_event(port, ...) exactly as a raw SPU
 * thread does, but there is no lv2 SPU thread group to hold the port binding,
 * so the lv2 delivery path had nowhere to look and dropped the event -- and
 * with it the ack the SPU then blocks on in SPU_RdInMbox. Havok's SPU task
 * stalls on the first physics step that way (port 2), and the PPU step waits
 * for it forever. */
static u32 s_spurs_port_queue[64];

uint32_t spurs_port_queue(uint32_t port)
{
    return port < 64 ? s_spurs_port_queue[port] : 0;
}

extern int sys_event_queue_exists(uint32_t queue_id);

/* Ports are per instance (an SPU thread group's), held in the instance's
 * spuPortBits; the instance's own kernel port, SPURS_KERNEL_PORT, is always
 * taken. A static attach names its port (0..15 for SDK 1.8+ titles; 16..63
 * are the dynamic range), a dynamic one takes the lowest free port above the
 * kernel's. Detach really disconnects: the port is free again afterwards. */
s32 cellSpursAttachLv2EventQueue(CellSpurs* spurs, u32 queue, u8* port,
                                 s32 isDynamic)
{
    const u32 ea = (u32)(uintptr_t)spurs;
    if (!ea || !port) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (ea & 127) return CELL_SPURS_CORE_ERROR_ALIGN;
    u8* port_h = GUEST_PTR(port, u8*);

    u32 p = 0;
    if (!isDynamic) {
        p = *port_h;
        if (p > 0x3F) return CELL_SPURS_CORE_ERROR_INVAL;
        if (p > 0x0F) return CELL_SPURS_CORE_ERROR_PERM;
    }
    if (!sys_event_queue_exists(queue)) return (s32)CELL_ESRCH;

    const u64 used = vm_read64(ea + SPURS_PORT_BITS) | (1ull << SPURS_KERNEL_PORT);
    if (isDynamic) {
        for (p = SPURS_KERNEL_PORT; p < 64 && (used >> p & 1); p++) {}
        if (p == 64) return CELL_SPURS_CORE_ERROR_BUSY;
    } else if (used >> p & 1) {
        return CELL_SPURS_CORE_ERROR_BUSY;
    }
    vm_write64(ea + SPURS_PORT_BITS, vm_read64(ea + SPURS_PORT_BITS) | (1ull << p));
    *port_h = (u8)p;
    s_spurs_port_queue[p] = queue;

    /* KEEP the queue id. SPURS signals the application through the queue it
     * attaches here, so nothing SPURS does could otherwise wake a thread
     * sitting in sys_event_queue_receive on it (Tokyo Jungle kicks its job
     * chain and then blocks on exactly that receive). A previous Finalize may
     * have cancelled the queue; attaching gives it a producer again. */
    sys_event_queue_uncancel_by_id(queue);
    int dup = 0;
    for (int i = 0; i < s_spurs_event_queue_n; i++)
        if (s_spurs_event_queue[i] == queue) dup = 1;
    if (!dup && s_spurs_event_queue_n < MAX_SPURS_QUEUES) {
        s_spurs_event_queue_owner[s_spurs_event_queue_n] = ea;
        s_spurs_event_queue[s_spurs_event_queue_n++] = queue;
    }
    printf("[cellSpurs] AttachLv2EventQueue(queue=%u) port=%u%s\n", queue, p,
           isDynamic ? " (dynamic)" : "");
    return CELL_OK;
}

s32 cellSpursDetachLv2EventQueue(CellSpurs* spurs, u8 port)
{
    const u32 ea = (u32)(uintptr_t)spurs;
    s32 rc = spurs_check_core(ea);
    if (rc) return rc;
    if (port > 0x3F) return CELL_SPURS_CORE_ERROR_INVAL;
    const u64 bits = vm_read64(ea + SPURS_PORT_BITS);
    if (!(bits >> port & 1)) return CELL_SPURS_CORE_ERROR_SRCH;
    vm_write64(ea + SPURS_PORT_BITS, bits & ~(1ull << port));
    s_spurs_port_queue[port] = 0;
    printf("[cellSpurs] DetachLv2EventQueue(port=%u)\n", port);
    return CELL_OK;
}

/* =========================================================================
 * Taskset
 * =====================================================================*/

s32 cellSpursCreateTaskset(CellSpurs* spurs, CellSpursTaskset* taskset,
                           u64 args, const u8* priority, u32 maxContention)
{
    (void)args; (void)priority; (void)maxContention;

    /* Capture the GUEST EAs (raw register values) BEFORE host translation -- the real
     * BE taskset builder writes to guest memory at these EAs. */
    uint32_t taskset_ea = (uint32_t)(uintptr_t)taskset;
    uint32_t spurs_ea   = (uint32_t)(uintptr_t)spurs;

    /* Args arrive as guest effective addresses (ps3_hle_call passes raw guest
     * register values); translate to host before dereferencing. */
    taskset = GUEST_PTR(taskset, CellSpursTaskset*);

    if (!spurs || !taskset) {
        fprintf(stderr, "[cellSpurs] CreateTaskset REJECT null (spurs=0x%08X taskset=0x%08X)\n",
                spurs_ea, taskset_ea);
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    }

    if (!spurs_inst_find(spurs_ea)) {
        fprintf(stderr, "[cellSpurs] CreateTaskset REJECT unregistered spurs=0x%08X (taskset=0x%08X)\n",
                spurs_ea, taskset_ea);
        return CELL_SPURS_CORE_ERROR_STAT;
    }

    memset(taskset, 0, sizeof(CellSpursTaskset));
    taskset->initialized = 1;
    taskset->spurs = spurs;

    /* Write the REAL big-endian CellSpursTaskset layout (fork Option-B) so the lifted
     * SPU leaf + spurs_pm_build_context read valid data (the native writes above are
     * little-endian = garbage to the SPU). Overwrites 0x00-0x80 with BE fields. */
    spurs_taskset_init(taskset_ea, spurs_ea, args, /*wid*/0,
                       (uint32_t)sizeof(CellSpursTaskset), /*evf1*/0, /*evf2*/0);
    g_ydkj_real_taskset_ea = taskset_ea;

    g_ydkj_real_spurs_ea = spurs_ea;   /* capture for the taskset-policy handoff (LS[0x1C0]) */
    printf("[cellSpurs] CreateTaskset() ea=0x%08X spurs=0x%08X (real BE layout)\n", taskset_ea, spurs_ea);
    return CELL_OK;
}

s32 cellSpursCreateTasksetWithAttribute(CellSpurs* spurs,
                                        CellSpursTaskset* taskset,
                                        const CellSpursTasksetAttribute* attr)
{
    (void)attr;
    return cellSpursCreateTaskset(spurs, taskset, 0, NULL, 0);
}

s32 cellSpursDestroyTaskset(CellSpursTaskset* taskset)
{
    taskset = GUEST_PTR(taskset, CellSpursTaskset*);
    if (!taskset)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    printf("[cellSpurs] DestroyTaskset()\n");
    taskset->initialized = 0;
    return CELL_OK;
}

s32 cellSpursShutdownTaskset(CellSpursTaskset* taskset)
{
    taskset = GUEST_PTR(taskset, CellSpursTaskset*);
    if (!taskset)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    printf("[cellSpurs] ShutdownTaskset()\n");
    taskset->shutdownRequested = 1;
    return CELL_OK;
}

s32 cellSpursJoinTaskset(CellSpursTaskset* taskset)
{
    taskset = GUEST_PTR(taskset, CellSpursTaskset*);
    if (!taskset)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    printf("[cellSpurs] JoinTaskset()\n");
    /* In a full implementation, wait for all tasks to complete */
    return CELL_OK;
}

s32 cellSpursTasksetAttributeInitialize(CellSpursTasksetAttribute* attr)
{
    if (!attr) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    attr = GUEST_PTR(attr, CellSpursTasksetAttribute*);
    memset(attr, 0, sizeof(CellSpursTasksetAttribute));
    attr->revision = 1;
    return CELL_OK;
}

s32 cellSpursTasksetAttributeSetName(CellSpursTasksetAttribute* attr,
                                      const char* name)
{
    (void)name;
    if (!attr) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    return CELL_OK;
}

/* =========================================================================
 * Task
 * =====================================================================*/

/* Real SDK ABI (verified against RPCS3 cellSpurs.cpp:370):
 *   cellSpursCreateTask(taskset, taskId, elf, context, sizeContext,
 *                       CellSpursTaskLsPattern* lsPattern,
 *                       CellSpursTaskArgument*  argument)
 * -- SEVEN args (r3..r9), not six. The old 6-arg form treated r8 as an opaque
 * `attr` and never read r9 (the argument), so the task's 16-byte work-descriptor
 * argument was dropped and TaskInfo.args stayed 0. LBP's audio task DMAs its work
 * from an EA computed out of r3 = that argument (spu_0003 task main @0x17e70:
 * `wrch $ch18, f(r3.word3)`), so a zero argument makes it GET from EA 0 and
 * stall. lsPattern/argument arrive as guest EAs (generic adapter forwards r3..r10). */
s32 cellSpursCreateTask(CellSpursTaskset* taskset, CellSpursTaskId* taskId,
                        void* elf, void* context, u32 sizeContext,
                        u32 lsPattern_ea, u32 argument_ea)
{
    (void)context; (void)sizeContext;

    /* Read the 16-byte CellSpursTaskArgument + optional LS pattern from guest mem. */
    uint32_t task_arg[4] = {0,0,0,0};
    uint32_t task_lsp[4] = {0,0,0,0};
    if (argument_ea)  for (int _i=0;_i<4;_i++) task_arg[_i] = vm_read32(argument_ea + _i*4);
    if (lsPattern_ea) for (int _i=0;_i<4;_i++) task_lsp[_i] = vm_read32(lsPattern_ea + _i*4);

    /* Capture guest EAs BEFORE host translation (the real BE taskset builder + the
     * SPU DMA use guest EAs). */
    uint32_t taskset_ea = (uint32_t)(uintptr_t)taskset;
    uint32_t elf_ea     = (uint32_t)(uintptr_t)elf;
    uint32_t context_ea = (uint32_t)(uintptr_t)context;

    /* taskId/taskset are guest EAs; translate before deref. elf/context stay
     * guest EAs (handled below — elf is translated for load, context kept EA). */
    taskset = GUEST_PTR(taskset, CellSpursTaskset*);
    CellSpursTaskId* taskId_h = GUEST_PTR(taskId, CellSpursTaskId*);

    if (!taskset) {
        fprintf(stderr, "[cellSpurs] CreateTask REJECT null taskset (elf=0x%08X)\n", elf_ea);
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    }

    if (!g_ydkj_real_taskset_ea) { /* real-BE init flag (native ->initialized clobbered by BE layout) */
        fprintf(stderr, "[cellSpurs] CreateTask REJECT no-init (taskset=0x%08X elf=0x%08X)\n",
                taskset_ea, elf_ea);
        return CELL_SPURS_TASK_ERROR_STAT;
    }

    /* Find a free task slot IN THIS TASKSET: the first task id whose bit in the
     * taskset's enabled bitset (+0x30, 128 bits, task 0 = MSB) is clear. Task
     * exit clears that bit (spu_taskset_task_exited), so ids recycle the way
     * real SPURS recycles them. The old scan of the global s_tasks[] never
     * freed a slot unless the task was joined; Havok creates a task per step
     * and never joins, so after 128 steps CreateTask failed and the physics
     * step waited forever for a task that was never started.
     * ponytail: s_tasks[] is shared by every taskset and now only records the
     * last task per slot id -- enough for JoinTask2's lookup; key it by
     * (taskset, id) if a title joins tasks in two tasksets at once. */
    u32 free_slot = CELL_SPURS_MAX_TASK;
    for (u32 t = 0; t < CELL_SPURS_MAX_TASK; t++)
        if (!(vm_read8(taskset_ea + 0x30 + t / 8) & (0x80u >> (t % 8)))) { free_slot = t; break; }
    if (free_slot < CELL_SPURS_MAX_TASK) {
        u32 i = free_slot;
        {
            s_tasks[i].in_use = 1;
            s_tasks[i].id = s_next_task_id++;
            s_tasks[i].active = 1;
            s_tasks[i].completed = 0;
            s_tasks[i].exitCode = 0;
            s_tasks[i].entryPoint = elf;

            /* The guest reads this out-param BIG-ENDIAN, and a native store
             * put it in host order: task 1 came back to the game as 0x01000000,
             * which it then handed to _cellSpursSendSignal. No task by that id
             * exists, the signal was dropped, and the SPU task sat in
             * WAIT_SIGNAL while the PPU waited on the event flag it would have
             * set -- a two-sided deadlock from one missing byte swap.
             *
             * Hand back the SLOT INDEX, not the global counter: `i` is what
             * spurs_taskset_add_task() sets as the taskset's bitset bit and
             * what spu_taskset_signal_task() looks up, so the id the guest
             * signals with has to be the same number. */
            if (taskId) vm_write32((u32)(uintptr_t)taskId, i);
            (void)taskId_h;
            taskset->taskCount++;

            /* Register the task in the REAL BE taskset: writes task_info[slot]
             * (args/elf/context/ls_pattern) + sets enabled+ready bits so the PM's
             * SELECT_TASK picks it. Slot index i = the SPURS taskId (bitset bit). */
            spurs_taskset_add_task(taskset_ea, i, (uint64_t)elf_ea,
                                   (uint64_t)context_ea, task_arg, task_lsp);
            /* Bridge to the image-22 dispatch so build_context uses this taskset+task. */
            g_ydkj_real_taskset_ea = taskset_ea;
            g_ydkj_real_taskid     = i;

            printf("[cellSpurs] CreateTask(id=%u, entry=%p, arg=%08X %08X %08X %08X"
                   " ctx=0x%08X lsp=%08X %08X %08X %08X)\n",
                   s_tasks[i].id, elf, task_arg[0], task_arg[1], task_arg[2], task_arg[3],
                   context_ea, task_lsp[0], task_lsp[1], task_lsp[2], task_lsp[3]);

            /* One-shot: dump the memory the task argument points at, to find the
             * pointer that reads back 0 (the task GETs from EA 0 -> some field of
             * its work descriptor is null in our run). Each of the 4 arg words that
             * looks like a valid guest EA gets 64 bytes dumped as BE u32s. */
            if (getenv("SPURS_TASKSET_TRACE")) {
                for (int a = 0; a < 4; a++) {
                    uint32_t p = task_arg[a];
                    if (p < 0x10000 || p >= 0x50000000u) continue;   /* not a plausible EA */
                    fprintf(stderr, "[argdump] arg[%d]=0x%08X ->", a, p);
                    for (int o = 0; o < 64; o += 4)
                        fprintf(stderr, " %08X", vm_read32(p + o));
                    fprintf(stderr, "\n");
                }
                /* Full heap object (arg[2], the audio engine object): the count the
                 * PPU validated is at +0xC8 (16-aligned, <=0x160 = stream count); the
                 * task's per-stream buffer pointers live at +0x124 (=arg[1]). Dump
                 * +0x00..+0x160 so we can see: 0 streams (task should yield) vs N
                 * streams with null data buffers (fill gap). */
                uint32_t ho = task_arg[2];
                if (ho >= 0x10000 && ho < 0x50000000u) {
                    fprintf(stderr, "[heapobj] 0x%08X count@+0xC8=0x%08X\n", ho, vm_read32(ho + 0xC8));
                    for (int o = 0; o < 0x160; o += 16)
                        fprintf(stderr, "  +%03X: %08X %08X %08X %08X\n", o,
                                vm_read32(ho+o), vm_read32(ho+o+4), vm_read32(ho+o+8), vm_read32(ho+o+12));
                }
                /* The descriptor block the SPU task actually DMAs + reads its buffer
                 * pointers from: v10[336..] at v10+1344 = arg[3]-64 (arg[3]=v10+1408).
                 * v10[344]=a1[141], v10[345]=a1[140] (the FMOD DSP buffers). If those
                 * words are 0 here, they are the null source (task GETs from EA 0). */
                uint32_t d = task_arg[3];
                if (d >= 0x10040 && d < 0x50000000u) {
                    uint32_t db = d - 64;   /* 0x0094F6C0 = v10+1344 */
                    fprintf(stderr, "[descblk] v10+1344=0x%08X (a1[141]@+0x20, a1[140]@+0x24):\n", db);
                    for (int o = 0; o < 0x40; o += 16)
                        fprintf(stderr, "  +%02X: %08X %08X %08X %08X\n", o,
                                vm_read32(db+o), vm_read32(db+o+4), vm_read32(db+o+8), vm_read32(db+o+12));
                }
                /* a1 (the FMOD object) = taskId_ea - 628 (sub_48420C passes the taskId
                 * out-param as (_DWORD)a1+628 for task 0). a1[140]/a1[141] (= a1+0x230/
                 * +0x234) are the null DSP-buffer fields. Log the EAs so the next run
                 * can YDKJ_WWATCH=<a1+0x230> to catch who should write it (or prove no
                 * one does). Only for task 0 (offset 628); task 1 uses +688. */
                uint32_t a1 = (uint32_t)(uintptr_t)taskId - 628u;
                if (a1 < 0x50000000u)
                    fprintf(stderr, "[a1obj] a1=0x%08X  a1+0x230(dsp0)=0x%08X val=0x%08X  "
                            "a1+0x234(dsp1)=0x%08X val=0x%08X\n", a1,
                            a1+0x230, vm_read32(a1+0x230), a1+0x234, vm_read32(a1+0x234));
                fflush(stderr);
            }

            /* Run the task's SPU program if a lifted build is registered for it.
             * The registry maps the task ELF (by content fingerprint) to its
             * pre-lifted native entry; dispatch loads the ELF into a local store
             * and runs it with the task arg in r3. INERT until the title
             * registers its lifted SPU set: an unregistered image MISSes and
             * returns 0, preserving the prior "track only" behaviour.
             *
             * NOTE: dispatch is synchronous (runs to completion inline). That
             * suits create+join task patterns; a workload/taskset whose SPU job
             * waits on concurrent PPU-side signals will want the async lv2
             * SPU-thread path instead — wired when a title exercises it. */
            if (elf) {
                /* elf/context are guest effective addresses; translate the image
                 * pointer to host memory for fingerprint+load, but keep context
                 * as the guest EA (the SPU job's DMA uses guest EAs / r3). */
                const uint8_t* host_elf = GUEST_PTR(elf, const uint8_t*);
                size_t sz = spu_elf_image_size(host_elf, 2u * 1024 * 1024);
                if (sz)
                    /* Async: SPURS tasks are persistent workers — running them
                     * inline would block this PPU thread forever (deadlock).
                     * Unlifted images run on the interpreter (spu_task_dispatch). */
                    spu_task_dispatch(taskset_ea, i, host_elf, (uint32_t)sz,
                                      (uint32_t)(uintptr_t)context);
            }
            return CELL_OK;
        }
    }

    fprintf(stderr, "[cellSpurs] CreateTask: taskset 0x%08X has no free task id (all 128 enabled)\n",
            taskset_ea);
    return CELL_SPURS_TASK_ERROR_NOMEM;
}

/* The SDK's versioned task-attribute initializer. ABI (8 GPR args):
 *   r3=attr r4=revision r5=sdkVersion r6=eaElf r7=eaContext r8=sizeContext
 *   r9=lsPattern r10=argument
 * Stash the task ELF EA + context so cellSpursCreateTaskWithAttribute can
 * dispatch the SPU job. */
s32 _cellSpursTaskAttributeInitialize(CellSpursTaskAttribute* attr, u32 revision,
                                      u32 sdkVersion, u64 eaElf, u64 eaContext,
                                      u32 sizeContext, const void* lsPattern,
                                      const void* argument)
{
    (void)sdkVersion;
    if (!attr) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    attr = GUEST_PTR(attr, CellSpursTaskAttribute*);
    memset(attr, 0, sizeof(CellSpursTaskAttribute));
    attr->revision    = revision;
    attr->sizeContext = sizeContext;
    attr->eaContext   = eaContext;
    attr->eaElf       = eaElf;
    /* lsPattern/argument are guest EAs of 16-byte blocks; carry them so
     * CreateTaskWithAttribute writes them into the TaskInfo. The SPU task
     * library refuses blocking waits for a task whose argument is zero or
     * whose lsPattern doesn't cover its stack (0x8041090F). */
    attr->lsPattern_ea = (u32)(uintptr_t)lsPattern;
    attr->argument_ea  = (u32)(uintptr_t)argument;

    /* SPURS_TASKATTR_R8: some callers use a SIX-argument form, passing only
     * r3..r8 -- r9/r10 then hold caller leftovers, not arguments. YDKJ is one:
     * func_00331DA4 sets r3..r8 and nothing else, so our 8-parameter prototype
     * reads lsPattern=r9 (a size, 0x0003D400) and argument=r10 (0x2E, the ASCII
     * terminator its hex-string parser stopped on). r8 is the real 16-byte
     * CellSpursTaskArgument -- proven by its contents matching the quadword the
     * SPU task later receives in r3. A zero/garbage argument makes the SPU task
     * library refuse every blocking wait (0x8041090F), which is exactly how the
     * CRI tasks end up parked in WAIT_SIGNAL forever.
     * CONFIRMED by a second, independently lifted title: Jackbox Party Pack
     * calls the same NID with the same shape -- r3=attr, r4=1, r5=0x00330000,
     * r6=eaElf, r7=sp+0x180, r8=sp+0x170 (two adjacent 16-byte blocks), and
     * r9/r10 never written. Two unrelated callers agreeing settles it, so this
     * is now the default; SPURS_TASKATTR_LEGACY=1 restores the old 8-arg read. */
    { static int s_r8 = -1;
      if (s_r8 < 0) s_r8 = getenv("SPURS_TASKATTR_LEGACY") ? 0 : 1;
      if (s_r8) {
          attr->argument_ea  = (u32)sizeContext;   /* r8 */
          attr->lsPattern_ea = 0;                  /* r9 was a leftover */
          printf("[cellSpurs] TaskAttr R8-form: argument_ea=0x%08X (was 0x%08X)\n",
                 attr->argument_ea, (u32)(uintptr_t)argument);
      } }
    /* SPURS_TASKATTR_DESC: a THIRD caller shape, found in Saints Row 2. Its
     * taskset builder (func_009F56E0) does not pass eaContext/sizeContext/
     * lsPattern as separate arguments at all -- r7 points at a 3-word
     * DESCRIPTOR it fills immediately before the call, and r8 is the argument:
     *
     *   009F58D0  addi r7, r1, 128     ; r7 = sp+0x80  -> the descriptor
     *   009F58D4  addi r8, r1, 176     ; r8 = sp+0xB0  -> CellSpursTaskArgument
     *   009F58E8  stw  r9,  0x80(r1)   ;   [0] context EA
     *   009F58EC  stw  r11, 0x84(r1)   ;   [1] context size, straight out of
     *                                  ;       cellSpursTaskGetContextSaveAreaSize
     *   009F58F0  stw  r25, 0x88(r1)   ;   [2] CellSpursTaskLsPattern*
     *
     * Read the 8-argument way, sizeContext comes out as r8 -- a STACK ADDRESS,
     * not a size (the log shows szctx=267382952 = 0x0FEFF0A8) -- and lsPattern
     * comes out of r9, which this caller never sets, so it is 0. Those are
     * exactly the conditions the SPU task library refuses to run a blocking
     * task under, and the tasks then park in WAIT_SIGNAL forever doing 0 ms of
     * work apiece, which is the symptom this title shows. Off by default: the
     * R8-form above is right for YDKJ and Jackbox and they are unaffected. */
    { static int s_desc = -1;
      if (s_desc < 0) s_desc = getenv("SPURS_TASKATTR_DESC") ? 1 : 0;
      if (s_desc) {
          uint32_t d = (uint32_t)eaContext;          /* r7 */
          attr->eaContext    = vm_read32(d + 0);
          attr->sizeContext  = vm_read32(d + 4);
          attr->lsPattern_ea = vm_read32(d + 8);
          attr->argument_ea  = (u32)sizeContext;     /* r8 */
          printf("[cellSpurs] TaskAttr DESC-form: desc=0x%08X -> ctx=0x%08X size=%u lsp=0x%08X arg=0x%08X\n",
                 d, (u32)attr->eaContext, attr->sizeContext,
                 attr->lsPattern_ea, attr->argument_ea);
      } }
    printf("[cellSpurs] _TaskAttributeInitialize(eaElf=0x%08X ctx=0x%08X szctx=%u lsp=0x%08X arg=0x%08X)\n",
           (u32)eaElf, (u32)eaContext, sizeContext,
           attr->lsPattern_ea, attr->argument_ea);
    return CELL_OK;
}

/* Create a task from a pre-initialized attribute (carries ELF EA + context).
 * Forwards to cellSpursCreateTask, which translates taskset/taskId and runs the
 * SPU image through spu_workload_dispatch. */
s32 cellSpursCreateTaskWithAttribute(CellSpursTaskset* taskset,
                                     CellSpursTaskId* taskId,
                                     CellSpursTaskAttribute* attr)
{
    if (!attr) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    CellSpursTaskAttribute* attr_h = GUEST_PTR(attr, CellSpursTaskAttribute*);
    /* Dump the raw attribute: our struct doesn't model lsPattern/argument, and
     * a wait-capable task NEEDS its context size + ls pattern carried through
     * (a no-context task may not block -- SPU task-lib waits then fail with
     * ERROR_STAT). Learn the real field offsets from the bytes. */
    { uint32_t aea = (uint32_t)(uintptr_t)attr;
      static int _n = 0; if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 6) {
        fprintf(stderr, "[cellSpurs] CreateTaskWithAttr attr=0x%08X raw:", aea);
        for (int o = 0; o < 0x40; o += 4) fprintf(stderr, " %08X", vm_read32(aea + o));
        fprintf(stderr, "\n"); } }
    /* taskset/taskId forwarded raw (callee translates); elf/context are guest
     * EAs, as are lsPattern/argument (stored by _cellSpursTaskAttributeInitialize;
     * dropping them left the TaskInfo with a zero argument + zero lsPattern and
     * the SPU task library then refuses every blocking wait with 0x8041090F --
     * LBP's binkspu movie-IO task spun forever on that). */
    return cellSpursCreateTask(taskset, taskId,
                               (void*)(uintptr_t)(u32)attr_h->eaElf,
                               (void*)(uintptr_t)(u32)attr_h->eaContext,
                               attr_h->sizeContext,
                               attr_h->lsPattern_ea, attr_h->argument_ea);
}

/* The SDK's versioned taskset-attribute initializer. We forward taskset creation
 * through CreateTaskset (which ignores the attribute), so just zero the struct. */
s32 _cellSpursTasksetAttributeInitialize(CellSpursTasksetAttribute* attr,
                                         u32 revision, u32 sdkVersion, u64 argTaskset,
                                         u64 priority, u32 maxContention)
{
    (void)sdkVersion; (void)argTaskset; (void)priority; (void)maxContention;
    if (!attr) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    attr = GUEST_PTR(attr, CellSpursTasksetAttribute*);
    memset(attr, 0, sizeof(CellSpursTasksetAttribute));
    attr->revision = revision ? revision : 1;
    printf("[cellSpurs] _TasksetAttributeInitialize(rev=%u)\n", revision);
    return CELL_OK;
}

s32 cellSpursJoinTask(CellSpursTaskset* taskset, CellSpursTaskId taskId,
                      s32* exitCode)
{
    (void)taskset;
    s32* exitCode_h = GUEST_PTR(exitCode, s32*);

    printf("[cellSpurs] JoinTask(id=%u)\n", taskId);

    /* Find the task and mark as completed */
    for (u32 i = 0; i < CELL_SPURS_MAX_TASK; i++) {
        if (s_tasks[i].in_use && s_tasks[i].id == taskId) {
            s_tasks[i].completed = 1;
            s_tasks[i].active = 0;
            if (exitCode_h)
                *exitCode_h = s_tasks[i].exitCode;
            s_tasks[i].in_use = 0;
            return CELL_OK;
        }
    }

    return CELL_SPURS_TASK_ERROR_SRCH;
}

s32 cellSpursSendSignal(CellSpursTaskset* taskset, CellSpursTaskId taskId)
{
    /* Capture the guest taskset EA before host translation -- the WAIT_SIGNAL
     * waiter (spu_taskset_wait_signal) keys on the guest EA + taskId. */
    uint32_t taskset_ea = (uint32_t)(uintptr_t)taskset;

    printf("[cellSpurs] SendSignal(taskset=0x%08X id=%u)\n", taskset_ea, taskId);

    /* Deliver the signal for real: set the task's bit in the guest taskset's
     * SIGNALLED bitset and wake its blocked host thread. (Was a documented
     * no-op from the pre-SPU-execution era -- a dropped signal deadlocked any
     * task parked in WAIT_SIGNAL waiting for it.) */
    if (taskset_ea) spu_taskset_signal_task(taskset_ea, taskId);
    return CELL_OK;
}

s32 cellSpursTaskAttributeInitialize(CellSpursTaskAttribute* attr)
{
    if (!attr) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    attr = GUEST_PTR(attr, CellSpursTaskAttribute*);
    memset(attr, 0, sizeof(CellSpursTaskAttribute));
    attr->revision = 1;
    return CELL_OK;
}

/* =========================================================================
 * Workload
 * =====================================================================*/


static u32* spurs_u32(u32 ea) { return (u32*)(vm_base + ea); }

/* Atomic read-modify-write of a big-endian guest word. */
static u32 spurs_be32_update(u32 ea, u32 set, u32 clear)
{
    u32 raw = __atomic_load_n(spurs_u32(ea), __ATOMIC_SEQ_CST), nv;
    do {
        nv = __builtin_bswap32((__builtin_bswap32(raw) & ~clear) | set);
    } while (!__atomic_compare_exchange_n(spurs_u32(ea), &raw, nv, 0,
                                          __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    return __builtin_bswap32(raw);
}

static u8 spurs_byte_or(u32 ea, u8 bits)
{
    return __atomic_fetch_or(vm_base + ea, bits, __ATOMIC_SEQ_CST);
}

/* The firmware's add_workload: claim the first clear bit of wklEnabled,
 * publish the workload into the instance, mark it runnable. */
static s32 spurs_add_workload(u32 ea, u32 wid_ea, u32 pm, u32 size, u64 data,
                              u32 prio_ea, u32 minc, u32 maxc,
                              u32 name_class, u32 name_inst, u32 hook, u32 hook_arg)
{
    if (!ea || !wid_ea || !pm) return CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER;
    if ((ea & 127) || (pm & 15)) return CELL_SPURS_POLICY_MODULE_ERROR_ALIGN;
    u8 prio[8] = {0};       /* libsre takes a null table as all zeros */
    if (prio_ea) memcpy(prio, vm_base + prio_ea, 8);
    if (minc == 0) return CELL_SPURS_POLICY_MODULE_ERROR_INVAL;
    for (int i = 0; i < 8; i++)
        if (prio[i] > 15) return CELL_SPURS_POLICY_MODULE_ERROR_INVAL;

    u32 raw = __atomic_load_n(spurs_u32(ea + SPURS_WKL_ENABLED), __ATOMIC_SEQ_CST), nv, wnum;
    do {
        const u32 v = __builtin_bswap32(raw);
        wnum = ~v ? (u32)__builtin_clz(~v) : 32;
        if (wnum >= CELL_SPURS_MAX_WORKLOAD) break;
        nv = __builtin_bswap32(v | (0x80000000u >> wnum));
    } while (!__atomic_compare_exchange_n(spurs_u32(ea + SPURS_WKL_ENABLED), &raw, nv, 0,
                                          __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    vm_write32(wid_ea, wnum);
    if (wnum >= CELL_SPURS_MAX_WORKLOAD) return CELL_SPURS_POLICY_MODULE_ERROR_AGAIN;

    u8* b = vm_base + ea;
    b[SPURS_WKL_STATE1 + wnum]  = WKL_STATE_PREPARING;
    b[SPURS_WKL_STATUS1 + wnum] = 0;
    b[SPURS_WKL_EVENT1 + wnum]  = 0;
    const u32 info = ea + SPURS_WKL_INFO1 + wnum * SPURS_WKL_INFO_SZ;
    vm_write64(info + 0x00, pm);
    vm_write64(info + 0x08, data);
    vm_write32(info + 0x10, (size + 15) & ~15u);           /* DMA granularity (libsre) */
    memcpy(vm_base + info + 0x18, prio, 8);
    vm_write64(ea + SPURS_WKL_H1 + 16 * wnum + 0, name_class);
    vm_write64(ea + SPURS_WKL_H1 + 16 * wnum + 8, name_inst);
    const u32 f1 = ea + SPURS_WKL_F1 + SPURS_WKL_F1_SZ * wnum;
    memset(vm_base + f1, 0, 0x20);
    memset(vm_base + f1 + 0x28, 0, 0x58);
    if (hook) {
        vm_write64(f1 + 0x30, hook);
        vm_write64(f1 + 0x38, hook_arg);
        b[SPURS_WKL_EVENT1 + wnum] |= 0x02;
    }
    b[SPURS_WKL_IDLE2 + wnum]   = 0;
    b[SPURS_WKL_MINCONT + wnum] = (u8)(minc > 8 ? 8 : minc);
    b[SPURS_WKL_READY1 + wnum]  = 0;
    b[SPURS_WKL_MAXCONT + wnum] = (u8)(maxc > 8 ? 8 : maxc);
    __atomic_fetch_and(vm_base + ea + SPURS_WKL_SIGNAL1 + (wnum >> 3),
                       (u8)~(0x80u >> (wnum & 7)), __ATOMIC_SEQ_CST);
    {   u8 w = (u8)wnum;
        __atomic_compare_exchange_n(vm_base + ea + SPURS_WKL_FLAG_RCV, &w, 0xFF, 0,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }

    /* uniqueId: shared by every workload running the same policy module,
     * otherwise the lowest id not in use. */
    const u32 mask = vm_read32(ea + SPURS_WKL_MSKB) & ~(0x80000000u >> wnum);
    u32 uid = 0, taken = 0;
    for (u32 i = 0; i < 32; i++) {
        if (!(mask & (0x80000000u >> i)) || i >= CELL_SPURS_MAX_WORKLOAD) continue;
        const u32 other = ea + SPURS_WKL_INFO1 + i * SPURS_WKL_INFO_SZ;
        if (vm_read64(other) == pm) { uid = vm_base[other + 0x14]; break; }
        taken |= 0x80000000u >> vm_base[other + 0x14];
        uid = ~taken ? (u32)__builtin_clz(~taken) : 32;
    }
    vm_base[info + 0x14] = (u8)uid;
    spurs_be32_update(ea + SPURS_WKL_MSKB, 0x80000000u >> wnum, 0);
    b[SPURS_WKL_STATE1 + wnum] = WKL_STATE_RUNNABLE;
    b[SPURS_SYSSRV_MSG]        = 0xFF;
    b[SPURS_SYSSRV_MESSAGE]    = 0xFF;

    spurs_kernel_notify(ea);
    printf("[cellSpurs] AddWorkload(wid=%u, pm=0x%08X, size=%u)\n", wnum, pm, size);
    return CELL_OK;
}

s32 cellSpursAddWorkload(CellSpurs* spurs, CellSpursWorkloadId* wid,
                         const void* pm, u32 sizePm, u64 data,
                         const u8* priority, u32 minContention,
                         u32 maxContention)
{
    return spurs_add_workload((u32)(uintptr_t)spurs, (u32)(uintptr_t)wid,
                              (u32)(uintptr_t)pm, sizePm, data, (u32)(uintptr_t)priority,
                              minContention, maxContention, 0, 0, 0, 0);
}

/* Real (BE) CellSpursWorkloadAttribute offsets (libspurs layout; the game's
 * inlined SDK code writes the struct directly in guest memory, so it must be
 * read back big-endian at these offsets -- never through a native host struct
 * (see the BIG-ENDIAN WARNING in spurs_taskset.h). */
enum {
    WKATTR_REVISION   = 0x00,   /* be u32 */
    WKATTR_SDKVERSION = 0x04,   /* be u32 */
    WKATTR_PM         = 0x08,   /* be u32: policy-module image EA */
    WKATTR_SIZE       = 0x0C,   /* be u32: policy-module size */
    WKATTR_DATA       = 0x10,   /* be u64: workload data (jobchain/queue EA) */
    WKATTR_PRIORITY   = 0x18,   /* u8[8] */
    WKATTR_MIN_CONT   = 0x20,   /* be u32 */
    WKATTR_MAX_CONT   = 0x24,   /* be u32 */
    WKATTR_NAME_CLASS = 0x28,   /* be u32: char* EA */
    WKATTR_NAME_INST  = 0x2C,   /* be u32: char* EA */
    WKATTR_HOOK       = 0x30,   /* be u32 */
    WKATTR_HOOK_ARG   = 0x34,   /* be u32 */
};

s32 cellSpursAddWorkloadWithAttribute(CellSpurs* spurs,
                                       CellSpursWorkloadId* wid,
                                       const CellSpursWorkloadAttribute* attr)
{
    uint32_t attr_ea = (uint32_t)(uintptr_t)attr;
    if (!spurs || !wid || !attr_ea) return CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER;
    if (((u32)(uintptr_t)spurs & 127) || (attr_ea & 7)) return CELL_SPURS_POLICY_MODULE_ERROR_ALIGN;

    /* Decode the REAL BE attribute from guest memory. */
    u32 pm_ea = vm_read32(attr_ea + WKATTR_PM);
    u32 pm_sz = vm_read32(attr_ea + WKATTR_SIZE);
    u64 data  = vm_read64(attr_ea + WKATTR_DATA);
    u32 minc  = vm_read32(attr_ea + WKATTR_MIN_CONT);
    u32 maxc  = vm_read32(attr_ea + WKATTR_MAX_CONT);
    u32 nmcls = vm_read32(attr_ea + WKATTR_NAME_CLASS);
    u32 nmins = vm_read32(attr_ea + WKATTR_NAME_INST);

    return spurs_add_workload((u32)(uintptr_t)spurs, (u32)(uintptr_t)wid, pm_ea, pm_sz, data,
                              attr_ea + WKATTR_PRIORITY, minc, maxc, nmcls, nmins,
                              vm_read32(attr_ea + WKATTR_HOOK), vm_read32(attr_ea + WKATTR_HOOK_ARG));
}

/* The SDK-versioned workload-attribute initializer (the import the game links;
 * NID differs from the non-underscore inline wrapper). Args arrive raw in
 * r3..r10; writes the REAL BE layout so AddWorkloadWithAttribute round-trips. */
s32 _cellSpursWorkloadAttributeInitialize(u64 attr_ea, u32 revision, u32 sdkVersion,
                                          u64 pm_ea, u32 size, u64 data,
                                          u64 prio_ea, u32 minContention)
{
    if (!attr_ea || !pm_ea) return CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER;
    if ((attr_ea & 7) || (pm_ea & 15)) return CELL_SPURS_POLICY_MODULE_ERROR_ALIGN;
    if (minContention == 0) return CELL_SPURS_POLICY_MODULE_ERROR_INVAL;
    if (prio_ea)
        for (int i = 0; i < 8; i++)
            if (vm_base[(u32)prio_ea + i] > 15) return CELL_SPURS_POLICY_MODULE_ERROR_INVAL;
    memset(vm_base + (u32)attr_ea, 0, 0x200);
    vm_write32((u32)attr_ea + WKATTR_REVISION,   revision);
    vm_write32((u32)attr_ea + WKATTR_SDKVERSION, sdkVersion);
    vm_write32((u32)attr_ea + WKATTR_PM,         (u32)pm_ea);
    vm_write32((u32)attr_ea + WKATTR_SIZE,       size);
    vm_write64((u32)attr_ea + WKATTR_DATA,       data);
    for (int i = 0; i < 8; i++)
        *(vm_base + (u32)attr_ea + WKATTR_PRIORITY + i) =
            prio_ea ? *(vm_base + (u32)prio_ea + i) : 0;
    vm_write32((u32)attr_ea + WKATTR_MIN_CONT, minContention);
    /* maxContention is the 9th argument: the plain 8-GPR adapter cannot see
     * it, so it defaults to 1 here and the context handler below
     * (_cellSpursWorkloadAttributeInitialize_ctx) stores the real value. */
    vm_write32((u32)attr_ea + WKATTR_MAX_CONT, 1);
    vm_write32((u32)attr_ea + WKATTR_NAME_CLASS, 0);
    vm_write32((u32)attr_ea + WKATTR_NAME_INST,  0);
    vm_write32((u32)attr_ea + WKATTR_HOOK,     0);
    vm_write32((u32)attr_ea + WKATTR_HOOK_ARG, 0);
    printf("[cellSpurs] _WorkloadAttributeInitialize(attr=0x%08X pm=0x%08X size=%u data=0x%llX minC=%u)\n",
           (u32)attr_ea, (u32)pm_ea, size, (unsigned long long)data, minContention);
    return CELL_OK;
}

/* Context-ABI entry (registered by ppu_sysprx.cpp): PPC64 passes arguments 9+
 * in the caller's parameter save area, so maxContention is the u32 at
 * r1 + 112 (low word of the doubleword at 112). */
void _cellSpursWorkloadAttributeInitialize_ctx(uint64_t* gpr)
{
    s32 rc = _cellSpursWorkloadAttributeInitialize(gpr[3], (u32)gpr[4], (u32)gpr[5], gpr[6],
                                                   (u32)gpr[7], gpr[8], gpr[9], (u32)gpr[10]);
    if (rc == CELL_OK)
        vm_write32((u32)gpr[3] + WKATTR_MAX_CONT, vm_read32((u32)gpr[1] + 112 + 4));
    gpr[3] = (uint64_t)(int64_t)rc;
}

s32 cellSpursWorkloadAttributeSetName(u64 attr_ea, u64 nameClass_ea, u64 nameInstance_ea)
{
    if (!attr_ea) return CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER;
    if (attr_ea & 7) return CELL_SPURS_POLICY_MODULE_ERROR_ALIGN;
    vm_write32((u32)attr_ea + WKATTR_NAME_CLASS, (u32)nameClass_ea);
    vm_write32((u32)attr_ea + WKATTR_NAME_INST,  (u32)nameInstance_ea);
    printf("[cellSpurs] WorkloadAttributeSetName(attr=0x%08X, \"%s\", \"%s\")\n",
           (u32)attr_ea,
           nameClass_ea ? (const char*)(vm_base + (u32)nameClass_ea) : "-",
           nameInstance_ea ? (const char*)(vm_base + (u32)nameInstance_ea) : "-");
    return CELL_OK;
}

s32 cellSpursRemoveWorkload(CellSpurs* spurs, CellSpursWorkloadId wid)
{
    const u32 ea = (u32)(uintptr_t)spurs;
    s32 rc = spurs_check_wid(ea, 1, wid);
    if (rc) return rc;
    switch (vm_base[ea + SPURS_WKL_STATE1 + wid]) {
    case WKL_STATE_SHUTTING_DOWN: return CELL_SPURS_POLICY_MODULE_ERROR_BUSY;
    case WKL_STATE_REMOVABLE: break;
    default: return CELL_SPURS_POLICY_MODULE_ERROR_STAT;
    }
    {   u8 w = (u8)wid;     /* a removed workload stops receiving the flag */
        __atomic_compare_exchange_n(vm_base + ea + SPURS_WKL_FLAG_RCV, &w, 0xFF, 0,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
    vm_base[ea + SPURS_WKL_STATE1 + wid] = WKL_STATE_NONE;
    spurs_be32_update(ea + SPURS_WKL_MSKB, 0, 0x80000000u >> wid);
    spurs_be32_update(ea + SPURS_WKL_ENABLED, 0, 0x80000000u >> wid);
    printf("[cellSpurs] RemoveWorkload(wid=%u)\n", wid);
    return CELL_OK;

}

s32 cellSpursWorkloadAttributeSetShutdownCompletionEventHook(u64 attr_ea, u64 hook_ea, u64 arg_ea)
{
    if (!attr_ea || !hook_ea) return CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER;
    if (attr_ea & 7) return CELL_SPURS_POLICY_MODULE_ERROR_ALIGN;
    vm_write32((u32)attr_ea + WKATTR_HOOK, (u32)hook_ea);
    vm_write32((u32)attr_ea + WKATTR_HOOK_ARG, (u32)arg_ea);
    return CELL_OK;
}

/* SDK inline wrapper over the export above (not itself a firmware export). */
s32 cellSpursWorkloadAttributeInitialize(CellSpursWorkloadAttribute* attr,
                                         u32 revision, u32 sdkVersion,
                                         const void* pm, u32 sizePm,
                                         u64 data, const u8* priority,
                                         u32 minContention,
                                         u32 maxContention)
{
    s32 rc = _cellSpursWorkloadAttributeInitialize((u32)(uintptr_t)attr, revision, sdkVersion,
                                                   (u32)(uintptr_t)pm, sizePm, data,
                                                   (u32)(uintptr_t)priority, minContention);
    if (rc == CELL_OK)
        vm_write32((u32)(uintptr_t)attr + WKATTR_MAX_CONT, maxContention);
    return rc;
}

/* readyCount lives in the instance (wklReadyCount1[wid]); the kernel and the
 * game's own inlined SDK code both use that byte, so every update is an
 * atomic operation on it. */
s32 cellSpursReadyCountStore(CellSpurs* spurs, CellSpursWorkloadId wid,
                             u32 value)
{
    const u32 ea = (u32)(uintptr_t)spurs;
    s32 rc = spurs_check_wid_runnable(ea, 1, wid, value > 0xFF);
    if (rc) return rc;
    __atomic_store_n(vm_base + ea + SPURS_WKL_READY1 + wid, (u8)value, __ATOMIC_SEQ_CST);
    {   static int _n = 0;
        if (_n < 32)
            printf("[cellSpurs] ReadyCountStore(wid=%u, value=%u)\n", wid, value);
        else if (_n == 32)
            printf("[cellSpurs] ReadyCountStore further logs suppressed\n");
        _n++;
    }
    spurs_kernel_notify(ea);
    return CELL_OK;
}

s32 cellSpursReadyCountAdd(u64 spurs_ea, u32 wid, u64 old_ea, s32 value)
{
    const u32 ea = (u32)spurs_ea;
    s32 rc = spurs_check_wid_runnable(ea, old_ea != 0, wid, 0);
    if (rc) return rc;
    u8* rc8 = vm_base + ea + SPURS_WKL_READY1 + wid;
    u8 cur = __atomic_load_n(rc8, __ATOMIC_SEQ_CST), nv;
    do {
        s32 v = (s32)cur + value;
        nv = (u8)(v < 0 ? 0 : v > 255 ? 255 : v);
    } while (!__atomic_compare_exchange_n(rc8, &cur, nv, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    vm_write32((u32)old_ea, cur);
    spurs_kernel_notify(ea);
    return CELL_OK;
}

s32 cellSpursReadyCountSwap(CellSpurs* spurs, CellSpursWorkloadId wid,
                            u32* old, u32 value)
{
    const u32 ea = (u32)(uintptr_t)spurs;
    s32 rc = spurs_check_wid_runnable(ea, old != 0, wid, value > 0xFF);
    if (rc) return rc;
    u8 prev = __atomic_exchange_n(vm_base + ea + SPURS_WKL_READY1 + wid, (u8)value, __ATOMIC_SEQ_CST);
    vm_write32((u32)(uintptr_t)old, prev);
    spurs_kernel_notify(ea);
    return CELL_OK;
}

s32 cellSpursReadyCountCompareAndSwap(CellSpurs* spurs,
                                       CellSpursWorkloadId wid,
                                       u32* old, u32 compare, u32 value)
{
    const u32 ea = (u32)(uintptr_t)spurs;
    s32 rc = spurs_check_wid_runnable(ea, old != 0, wid, compare > 0xFF || value > 0xFF);
    if (rc) return rc;
    u8 cur = (u8)compare;
    __atomic_compare_exchange_n(vm_base + ea + SPURS_WKL_READY1 + wid, &cur, (u8)value, 0,
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    vm_write32((u32)(uintptr_t)old, cur);
    spurs_kernel_notify(ea);
    return CELL_OK;
}

s32 cellSpursSendWorkloadSignal(u64 spurs_ea, u32 wid)
{
    const u32 ea = (u32)spurs_ea;
    s32 rc = spurs_check_wid_runnable(ea, 1, wid, 0);
    if (rc) return rc;
    /* wklSignal1 is a big-endian u16: wid 0 is the top bit of byte 0x70. */
    __atomic_fetch_or(vm_base + ea + SPURS_WKL_SIGNAL1 + (wid >> 3), (u8)(0x80u >> (wid & 7)),
                      __ATOMIC_SEQ_CST);
    spurs_kernel_notify(ea);
    return CELL_OK;
}

s32 cellSpursGetWorkloadData(u64 spurs_ea, u64 data_ea, u32 wid)
{
    const u32 ea = (u32)spurs_ea;
    s32 rc = spurs_check_wid(ea, data_ea != 0, wid);
    if (rc) return rc;
    vm_write64((u32)data_ea, vm_read64(ea + SPURS_WKL_INFO1 + wid * SPURS_WKL_INFO_SZ + 0x08));
    return CELL_OK;
}

s32 cellSpursGetWorkloadFlag(u64 spurs_ea, u64 flag_ea)
{
    const u32 ea = (u32)spurs_ea;
    if (!ea || !flag_ea) return CELL_SPURS_POLICY_MODULE_ERROR_NULL_POINTER;
    if (ea & 127) return CELL_SPURS_POLICY_MODULE_ERROR_ALIGN;
    vm_write32((u32)flag_ea, ea + SPURS_WKL_FLAG);
    return CELL_OK;
}

/* One workload at a time may own the instance's workload flag. Setting or
 * releasing it re-arms the flag word (-1). */
s32 _cellSpursWorkloadFlagReceiver(u64 spurs_ea, u32 wid, u32 is_set)
{
    const u32 ea = (u32)spurs_ea;
    s32 rc = spurs_check_wid(ea, 1, wid);
    if (rc) return rc;
    u8* rcv = vm_base + ea + SPURS_WKL_FLAG_RCV;
    u8 want = is_set ? 0xFF : (u8)wid;
    if (!__atomic_compare_exchange_n(rcv, &want, is_set ? (u8)wid : 0xFF, 0,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return is_set ? CELL_SPURS_POLICY_MODULE_ERROR_BUSY : CELL_SPURS_POLICY_MODULE_ERROR_PERM;
    vm_write32(ea + SPURS_WKL_FLAG + 0x0C, 0xFFFFFFFFu);
    spurs_kernel_notify(ea);
    return CELL_OK;
}

s32 cellSpursWakeUp(CellSpurs* spurs)
{
    { s32 rc = spurs_check_pm((u32)(uintptr_t)spurs); if (rc) return rc; }

    /* Mark the handler thread dirty: an instance created exitIfNoWork whose
     * SPU group has exited re-checks for ready workloads and restarts it
     * (a readyCount store alone does not). */
    spurs_kernel_wakeup((u32)(uintptr_t)spurs);
    return CELL_OK;
}

/* =========================================================================
 * Event flags
 * =====================================================================*/

s32 cellSpursEventFlagInitialize(CellSpursTaskset* taskset,
                                 CellSpursEventFlag* eventFlag,
                                 u32 clearMode, u32 direction)
{
    /* All pointers are raw guest EAs; the flag lives in guest memory in the
     * REAL BE layout (the SPU task library DMAs this exact struct). */
    uint32_t eventFlag_ea = (uint32_t)(uintptr_t)eventFlag;
    uint32_t taskset_ea   = (uint32_t)(uintptr_t)taskset;

    if (!eventFlag_ea)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (eventFlag_ea & 0x7F)
        return CELL_SPURS_TASK_ERROR_ALIGN;

    /* Re-initialization: recycle the sync slot. */
    EventFlagSync* old = ef_sync_find(eventFlag_ea);
    if (old) ef_sync_free(old);

    for (uint32_t o = 0; o < EF_GUEST_SIZE; o += 8)
        vm_write64(eventFlag_ea + o, 0);
    vm_write8(eventFlag_ea + EF_DIRECTION,  (uint8_t)direction);
    vm_write8(eventFlag_ea + EF_CLEAR_MODE, (uint8_t)clearMode);
    /* addr = the owning taskset (isIwl=0); SPU-side Set uses it to find whom
     * to signal, and our Set hook passes it to spu_taskset_signal_task. */
    vm_write8(eventFlag_ea + EF_IS_IWL, 0);
    vm_write64(eventFlag_ea + EF_ADDR, (uint64_t)taskset_ea);

    EventFlagSync* sync = ef_sync_alloc(eventFlag_ea);
    if (!sync) {
        printf("[cellSpurs] EventFlagInitialize: no free sync slots!\n");
        return CELL_SPURS_TASK_ERROR_NOMEM;
    }

    printf("[cellSpurs] EventFlagInitialize(clearMode=%u, direction=%u) flagEA=0x%08X taskset=0x%08X\n",
           clearMode, direction, eventFlag_ea, taskset_ea);
    return CELL_OK;
}

s32 cellSpursEventFlagAttachLv2EventQueue(CellSpursEventFlag* eventFlag)
{
    eventFlag = GUEST_PTR(eventFlag, CellSpursEventFlag*);
    if (!eventFlag) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    printf("[cellSpurs] EventFlagAttachLv2EventQueue()\n");
    return CELL_OK;
}

s32 cellSpursEventFlagDetachLv2EventQueue(CellSpursEventFlag* eventFlag)
{
    eventFlag = GUEST_PTR(eventFlag, CellSpursEventFlag*);
    if (!eventFlag) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    printf("[cellSpurs] EventFlagDetachLv2EventQueue()\n");
    return CELL_OK;
}

s32 cellSpursEventFlagSet(CellSpursEventFlag* eventFlag, u16 bits)
{
    uint32_t ea = (uint32_t)(uintptr_t)eventFlag;
    if (!ea)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    EventFlagSync* sync = ef_sync_get(ea);
    if (!sync)
        return CELL_SPURS_TASK_ERROR_STAT;

    { static int _n=0; if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 40 || (_n%1000)==0)
        fprintf(stderr, "[cellSpurs] EventFlagSet#%d flagEA=0x%08X bits=0x%04X "
                "events=0x%04X used=0x%04X pend=0x%04X mode=0x%04X\n",
                _n, ea, (unsigned)bits,
                vm_read16(ea + EF_EVENTS), vm_read16(ea + EF_SPU_USED_SLOTS),
                vm_read16(ea + EF_SPU_PENDING_RECV), vm_read16(ea + EF_SPU_WAIT_MODE)); }

    ef_lock(sync);
    spurs_ef_set_locked(ea, bits);
    ef_broadcast(sync);
    ef_unlock(sync);

    return CELL_OK;
}

/* SPURS_EF_STATS=1: per flag, how many PPU waits completed in the last 5 s
 * and their mean / max duration -- one SPU task round trip each. */
static void ef_wait_stats(uint32_t ea, long long t0)
{
    static int on = -1; if (on < 0) on = getenv("SPURS_EF_STATS") ? 1 : 0;
    if (!on) return;
    static uint32_t fea[8]; static unsigned n[8]; static double sum[8], mx[8];
    static long long last; static SRWLOCK lk = SRWLOCK_INIT;
    LARGE_INTEGER now, f; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&f);
    double us = (now.QuadPart - t0) * 1e6 / (double)f.QuadPart;
    AcquireSRWLockExclusive(&lk);
    for (int i = 0; i < 8; i++)
        if (fea[i] == ea || !fea[i]) { fea[i] = ea; n[i]++; sum[i] += us; if (us > mx[i]) mx[i] = us; break; }
    if (!last) last = now.QuadPart;
    if ((now.QuadPart - last) > 5 * f.QuadPart) {
        for (int i = 0; i < 8 && fea[i]; i++) {
            fprintf(stderr, "[ef-stats] flag=0x%08X waits=%.1f/s mean=%.0fus max=%.0fus%c",
                    fea[i], n[i] * (double)f.QuadPart / (now.QuadPart - last),
                    n[i] ? sum[i] / n[i] : 0.0, mx[i], 10);
            n[i] = 0; sum[i] = mx[i] = 0;
        }
        last = now.QuadPart;
    }
    ReleaseSRWLockExclusive(&lk);
}

s32 cellSpursEventFlagWait(CellSpursEventFlag* eventFlag, u16* bits,
                           u32 mode)
{
    uint32_t ea      = (uint32_t)(uintptr_t)eventFlag;
    uint32_t bits_ea = (uint32_t)(uintptr_t)bits;
    if (!ea || !bits_ea)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    EventFlagSync* sync = ef_sync_get(ea);
    if (!sync)
        return CELL_SPURS_TASK_ERROR_STAT;

    u16 pattern = vm_read16(bits_ea);

    ef_lock(sync);

    /* Block until the requested bit pattern is satisfied in the GUEST struct
     * (BE events word at +0x00) — set by an SPU task (lifted code / taskset
     * syscall) or another PPU thread. Poll-based: ef_wait_timed ticks re-read
     * guest memory, so task-side PUTLLC stores are observed without needing
     * the real lv2-event-queue notification path.
     *
     * A wait the SPU never satisfies is a REAL deadlock and must present as
     * one — that is the signal naming the workload that is not executing.
     * SPURS_EF_FORCE=1 restores the old fake for A/B comparison only. */
    static int s_force = -1;
    if (s_force < 0) s_force = getenv("SPURS_EF_FORCE") ? 1 : 0;
    unsigned waits = 0;
    u16 current;
    __atomic_add_fetch(&s_ef_ppu_waiters, 1, __ATOMIC_SEQ_CST);
    LARGE_INTEGER ef_t0; QueryPerformanceCounter(&ef_t0);
    for (;;) {
        current = vm_read16(ea + EF_EVENTS);

        if (mode == CELL_SPURS_EVENT_FLAG_AND) {
            if ((current & pattern) == pattern)
                break;
        } else {
            /* OR mode: any requested bit set */
            if ((current & pattern) != 0)
                break;
        }

        if (!ef_wait_timed(sync, 2)) {
            /* Report the stall once per second, naming what would have to run. */
            if (++waits % 500 == 0) {
                static int _n = 0;
                if (_n < 24) { _n++;
                    fprintf(stderr, "[cellSpurs] EventFlagWait BLOCKED tid=%lu %us on pattern 0x%04X "
                                    "(mode=%s, bits=0x%04X) flagEA=0x%08X -- waiting for an SPU "
                                    "workload to cellSpursEventFlagSet it\n",
                            (unsigned long)GetCurrentThreadId(), waits / 500, pattern,
                            mode == CELL_SPURS_EVENT_FLAG_AND ? "AND" : "OR",
                            current, ea);
                    fflush(stderr);
                }
            }
            if (s_force && waits >= 1000) {
                fprintf(stderr, "[cellSpurs] EventFlagWait: SPURS_EF_FORCE -- faking pattern "
                                "0x%04X (NOT real; A/B only)\n", pattern);
                vm_write16(ea + EF_EVENTS, (u16)(current | pattern));
            }
        }
    }

    __atomic_sub_fetch(&s_ef_ppu_waiters, 1, __ATOMIC_SEQ_CST);
    ef_wait_stats(ea, ef_t0.QuadPart);
    /* Hand back the bits that satisfied the wait (current & pattern, as
     * libsre does -- not every set bit); consume them on AUTO clear. */
    u16 received = (mode == CELL_SPURS_EVENT_FLAG_AND) ? pattern
                                                       : (u16)(current & pattern);
    vm_write16(bits_ea, received);
    { static int _n = 0; if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 40)
        fprintf(stderr, "[cellSpurs] EventFlagWait WAKE tid=%lu flagEA=0x%08X "
                "pattern=0x%04X got=0x%04X (waits=%u)\n",
                (unsigned long)GetCurrentThreadId(), ea, pattern, current, waits); }
    if (vm_read8(ea + EF_CLEAR_MODE) == CELL_SPURS_EVENT_FLAG_CLEAR_AUTO) {
        /* Clear from the CURRENT word: bits an SPU set since `current` was
         * read must survive. */
        spu_lockline_lock();
        ef_w16(ea + EF_EVENTS, (u16)(ef_r16(ea + EF_EVENTS) & ~received));
        ef_line_end(ea);
    }

    ef_unlock(sync);

    return CELL_OK;
}

s32 cellSpursEventFlagTryWait(CellSpursEventFlag* eventFlag, u16* bits,
                              u32 mode)
{
    uint32_t ea      = (uint32_t)(uintptr_t)eventFlag;
    uint32_t bits_ea = (uint32_t)(uintptr_t)bits;
    if (!ea || !bits_ea)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    EventFlagSync* sync = ef_sync_get(ea);
    if (!sync)
        return CELL_SPURS_TASK_ERROR_STAT;

    u16 pattern = vm_read16(bits_ea);

    ef_lock(sync);

    u16 current = vm_read16(ea + EF_EVENTS);

    if (mode == CELL_SPURS_EVENT_FLAG_AND) {
        if ((current & pattern) != pattern) {
            ef_unlock(sync);
            return CELL_SPURS_TASK_ERROR_BUSY;
        }
    } else {
        if ((current & pattern) == 0) {
            ef_unlock(sync);
            return CELL_SPURS_TASK_ERROR_BUSY;
        }
    }

    vm_write16(bits_ea, current);
    u16 received = (mode == CELL_SPURS_EVENT_FLAG_AND) ? pattern
                                                       : (u16)(current & pattern);
    if (vm_read8(ea + EF_CLEAR_MODE) == CELL_SPURS_EVENT_FLAG_CLEAR_AUTO) {
        spu_lockline_lock();
        ef_w16(ea + EF_EVENTS, (u16)(ef_r16(ea + EF_EVENTS) & ~received));
        ef_line_end(ea);
    }

    ef_unlock(sync);
    return CELL_OK;
}

s32 cellSpursEventFlagClear(CellSpursEventFlag* eventFlag, u16 bits)
{
    uint32_t ea = (uint32_t)(uintptr_t)eventFlag;
    if (!ea)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    EventFlagSync* sync = ef_sync_get(ea);
    if (!sync)
        return CELL_SPURS_TASK_ERROR_STAT;

    ef_lock(sync);
    spu_lockline_lock();
    ef_w16(ea + EF_EVENTS, (u16)(ef_r16(ea + EF_EVENTS) & ~bits));
    ef_line_end(ea);
    ef_unlock(sync);

    return CELL_OK;
}

s32 cellSpursEventFlagGetDirection(CellSpursEventFlag* eventFlag,
                                   u32* direction)
{
    uint32_t ea     = (uint32_t)(uintptr_t)eventFlag;
    uint32_t dir_ea = (uint32_t)(uintptr_t)direction;
    if (!ea || !dir_ea)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    vm_write32(dir_ea, vm_read8(ea + EF_DIRECTION));
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Additional functions needed by Tokyo Jungle (from RPCS3 audit)
 * -----------------------------------------------------------------------*/

/* _cellSpursEventFlagInitialize — internal init with more parameters */
s32 _cellSpursEventFlagInitialize(void* spurs, void* taskset,
                                    CellSpursEventFlag* eventFlag,
                                    u32 clearMode, u32 direction)
{
    (void)spurs; (void)taskset;
    printf("[cellSpurs] _EventFlagInitialize(clearMode=%u, dir=%u)\n",
           clearMode, direction);
    if (!eventFlag) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    /* Forward raw guest pointers; cellSpursEventFlagInitialize translates them
     * (translating here too would double-translate -> out-of-bounds). */
    return cellSpursEventFlagInitialize((CellSpursTaskset*)taskset, eventFlag, clearMode, direction);
}

/* _cellSpursSendSignal — internal signal delivery */
s32 _cellSpursSendSignal(void* taskset, u32 taskId)
{
    uint32_t taskset_ea = (uint32_t)(uintptr_t)taskset;
    printf("[cellSpurs] _SendSignal(taskset=0x%08X id=%u)\n", taskset_ea, taskId);
    /* Real delivery now that SPU tasks execute (comment was stale). */
    if (taskset_ea) spu_taskset_signal_task(taskset_ea, taskId);
    return CELL_OK;
}

/* =========================================================================
 * Job chains (LBP's render path: the game emits SPURS job descriptors and
 * chains them via u64 command words; the jobchain policy module walks the
 * chain on SPU). Facts-first bring-up: decode + log the REAL BE guest
 * structures at the SDK offsets; execution wiring lands once the logged
 * shapes confirm the descriptor formats.
 * =====================================================================*/

/* Real (BE) CellSpursJobChainAttribute offsets. */
enum {
    JCATTR_REVISION   = 0x00,   /* be u32 */
    JCATTR_SDKVERSION = 0x04,   /* be u32 */
    JCATTR_ENTRY      = 0x08,   /* be u32: EA of the first jobchain command word */
    JCATTR_SIZE_DESC  = 0x0C,   /* be u16: sizeJobDescriptor */
    JCATTR_MAX_GRAB   = 0x0E,   /* be u16: maxGrabbedJob */
    JCATTR_PRIORITY   = 0x10,   /* u8[8] */
    JCATTR_MAX_CONT   = 0x18,   /* be u32 */
    JCATTR_AUTO_RDY   = 0x1C,   /* u8 bool: autoReadyCount */
    JCATTR_TAG1       = 0x20,   /* be u32 */
    JCATTR_TAG2       = 0x24,   /* be u32 */
    JCATTR_FIXED_MEM  = 0x28,   /* u8 bool */
    JCATTR_MAX_SIZE_D = 0x2C,   /* be u32 */
    JCATTR_INIT_SPU   = 0x30,   /* be u32 */
    JCATTR_NAME       = 0x34,   /* be u32: char* EA (SetName) */
};

/* Host-side jobchain registry (the CellSpursJobChain guest struct is opaque
 * to the game; we track what we need beside it). */
#define MAX_JOBCHAINS 32
static struct {
    u32 jc_ea;        /* CellSpursJobChain EA (0 = free) */
    u32 entry_ea;     /* first command word EA */
    u16 size_desc;
    u16 max_grab;
    int run_count;
    volatile long running;   /* 1 while a host thread walks this chain */
} s_jobchains[MAX_JOBCHAINS];

static void jc_dump_commands(const char* tag, u32 ea, int max_words)
{
    printf("[cellSpurs] %s chain@0x%08X commands:", tag, ea);
    for (int i = 0; i < max_words; i++) {
        u64 cmd = vm_read64(ea + (u32)i * 8);
        printf("\n    [%2d] 0x%016llX", i, (unsigned long long)cmd);
        if (cmd == 0) { printf(" (halt/empty)"); break; }
    }
    printf("\n");
}

/* SDK ABI (cell/spurs/job_chain.h): the REVISIONS come first -- attr is r5.
 *   _cellSpursJobChainAttributeInitialize(jmRevision, sdkRevision, attr,
 *       jobChainEntry, sizeJobDescriptor, maxGrabbedJob, priorityTable,
 *       maxContention, [stack: autoRequestSpuCount, tag1, tag2,
 *       isFixedMemAlloc, maxSizeJobDescriptor, initialRequestSpuCount]) */
s32 _cellSpursJobChainAttributeInitialize(u32 jmRevision, u32 sdkRevision, u64 attr_ea,
                                          u64 entry_ea, u32 sizeJobDescriptor,
                                          u32 maxGrabbedJob, u64 prio_ea, u32 maxContention)
{
    if (!attr_ea) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    vm_write32((u32)attr_ea + JCATTR_REVISION,   jmRevision);
    vm_write32((u32)attr_ea + JCATTR_SDKVERSION, sdkRevision);
    vm_write32((u32)attr_ea + JCATTR_ENTRY,      (u32)entry_ea);
    vm_write32((u32)attr_ea + JCATTR_SIZE_DESC,
               ((sizeJobDescriptor & 0xFFFFu) << 16) | (maxGrabbedJob & 0xFFFFu));
    for (int i = 0; i < 8; i++)
        *(vm_base + (u32)attr_ea + JCATTR_PRIORITY + i) =
            prio_ea ? *(vm_base + (u32)prio_ea + i) : 0;
    vm_write32((u32)attr_ea + JCATTR_MAX_CONT, maxContention);
    /* args 9+ (autoReadyCount, tag1, tag2, isFixedMemAlloc, maxSizeJobDescriptor,
     * initSpuCount) are on the guest stack, beyond the 8-GPR HLE adapter -- defaults. */
    vm_write32((u32)attr_ea + JCATTR_AUTO_RDY,   0);
    vm_write32((u32)attr_ea + JCATTR_TAG1,       0);
    vm_write32((u32)attr_ea + JCATTR_TAG2,       0);
    vm_write32((u32)attr_ea + JCATTR_FIXED_MEM,  0);
    vm_write32((u32)attr_ea + JCATTR_MAX_SIZE_D, 0);
    vm_write32((u32)attr_ea + JCATTR_INIT_SPU,   0);
    vm_write32((u32)attr_ea + JCATTR_NAME,       0);
    printf("[cellSpurs] _JobChainAttributeInitialize(attr=0x%08X entry=0x%08X sizeDesc=%u maxGrab=%u maxCont=%u)\n",
           (u32)attr_ea, (u32)entry_ea, sizeJobDescriptor, maxGrabbedJob, maxContention);
    return CELL_OK;
}

s32 cellSpursJobChainAttributeSetName(u64 attr_ea, u64 name_ea)
{
    if (!attr_ea) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    vm_write32((u32)attr_ea + JCATTR_NAME, (u32)name_ea);
    printf("[cellSpurs] JobChainAttributeSetName(attr=0x%08X, \"%s\")\n",
           (u32)attr_ea, name_ea ? (const char*)(vm_base + (u32)name_ea) : "-");
    return CELL_OK;
}

/* Register (or re-register) a job chain handle. Shared by both Create forms. */
static s32 jc_register(u32 jc_ea, u32 entry_ea, u16 size_desc, u16 max_grab,
                       const char* who)
{
    for (int i = 0; i < MAX_JOBCHAINS; i++) {
        if (!s_jobchains[i].jc_ea || s_jobchains[i].jc_ea == jc_ea) {
            s_jobchains[i].jc_ea     = jc_ea;
            s_jobchains[i].entry_ea  = entry_ea;
            s_jobchains[i].size_desc = size_desc;
            s_jobchains[i].max_grab  = max_grab;
            s_jobchains[i].run_count = 0;
            return CELL_OK;
        }
    }
    printf("[cellSpurs] %s: registry full\n", who);
    return CELL_OK;
}

s32 cellSpursCreateJobChainWithAttribute(u64 spurs_ea, u64 jc_ea, u64 attr_ea)
{
    if (!spurs_ea || !jc_ea || !attr_ea) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    u32 entry   = vm_read32((u32)attr_ea + JCATTR_ENTRY);
    u32 sd_mg   = vm_read32((u32)attr_ea + JCATTR_SIZE_DESC);
    u32 name_ea = vm_read32((u32)attr_ea + JCATTR_NAME);

    {   /* Layout ground truth (same rationale as AddWorkloadWA). */
        static int _n = 0;
        if (_n < 3) {
            printf("[cellSpurs] CreateJobChainWA jc=0x%08X attr=0x%08X raw:", (u32)jc_ea, (u32)attr_ea);
            for (int o = 0; o < 0x40; o += 4) {
                if ((o & 15) == 0) printf("\n    +%02X:", o);
                printf(" %08X", vm_read32((u32)attr_ea + o));
            }
            printf("\n");
            if (entry && entry < 0x10000000u) jc_dump_commands("CreateJobChainWA", entry, 16);
        } else if (_n == 3) {
            printf("[cellSpurs] CreateJobChainWA raw dumps suppressed from here\n");
        }
        _n++;
    }
    printf("[cellSpurs] CreateJobChainWithAttribute(jc=0x%08X entry=0x%08X sizeDesc=%u maxGrab=%u name=\"%s\")\n",
           (u32)jc_ea, entry, sd_mg >> 16, sd_mg & 0xFFFFu,
           name_ea ? (const char*)(vm_base + name_ea) : "-");

    return jc_register((u32)jc_ea, entry, (u16)(sd_mg >> 16), (u16)(sd_mg & 0xFFFFu),
                       "CreateJobChainWithAttribute");
}

/* cellSpursCreateJobChain -- the no-attribute form, and the one the WWS job
 * manager's own wrapper calls. Same registration, with the fields passed
 * directly instead of read back out of a CellSpursJobChainAttribute:
 *
 *   cellSpursCreateJobChain(CellSpurs*, CellSpursJobChain*, const u64* entry,
 *                           u16 sizeJobDescriptor, u16 maxGrabbedJob,
 *                           const u8 priorityTable[8], u32 maxContention,
 *                           bool autoRequestSpuCount, u32 tag1, u32 tag2)
 *
 * tag1/tag2 are guest-stack args, past the 8-GPR HLE adapter; nothing here
 * needs them. Without this entry point the chain is never registered at all,
 * so the later kick finds "UNKNOWN chain (no Create seen)" and returns
 * CELL_OK having walked nothing -- and the title waits forever on SPU work
 * that was never started. Gran Turismo 5 Prologue parks its whole boot there. */
s32 cellSpursCreateJobChain(u64 spurs_ea, u64 jc_ea, u64 entry_ea,
                            u32 sizeJobDescriptor, u32 maxGrabbedJob,
                            u64 prio_ea, u32 maxContention,
                            u32 autoRequestSpuCount)
{
    (void)spurs_ea; (void)prio_ea;
    if (!jc_ea) return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    static int _n = 0;
    if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 3) {
        printf("[cellSpurs] CreateJobChain(jc=0x%08X entry=0x%08X sizeDesc=%u maxGrab=%u "
               "maxCont=%u autoReq=%u)\n",
               (u32)jc_ea, (u32)entry_ea, sizeJobDescriptor, maxGrabbedJob,
               maxContention, autoRequestSpuCount);
        if (entry_ea && (u32)entry_ea < 0x10000000u)
            jc_dump_commands("CreateJobChain", (u32)entry_ea, 16);
    }
    return jc_register((u32)jc_ea, (u32)entry_ea, (u16)sizeJobDescriptor,
                       (u16)maxGrabbedJob, "CreateJobChain");
}

/* ---------------------------------------------------------------------------
 * Job-chain execution
 *
 * A job chain is a u64 command stream (SDK cell/spurs/job_commands.h): the low
 * 3 bits select the opcode, and a nonzero word whose low 3 bits are 0 IS a job
 * pointer. The real jobchain policy module walks this on an SPU, fetching each
 * CellSpursJobHeader and running its binary.
 *
 * LBP's draw pipeline is built on this -- the jobs emit the GCM commands. With
 * the chain unimplemented the FIFO starves, the RSX `ref` fence stops advancing
 * and the game spins on it forever (the boot hang: ref frozen at 0x2B3 while
 * the chain sat un-run).
 *
 * We walk the stream on a host thread (one per chain; the real thing is async
 * on SPUs, so RunJobChain must not block the PPU) and push each job binary
 * through the same fingerprint -> lifted-SPU dispatch that already runs this
 * title's workload images.
 * -----------------------------------------------------------------------*/
enum {                              /* CellSpursJobHeader (48 B) */
    JH_EA_BINARY     = 0x00,        /* be u64: job binary EA (low 3 bits = flags) */
    JH_SIZE_BINARY   = 0x08,        /* be u16: binary size >> 4                   */
    JH_JOB_TYPE      = 0x2C,        /* u8                                         */
    JH_SIZE          = 0x30,
};

static void jc_run_one_job(u32 job_ea, int idx, u32 size_desc)
{
    u64 ea_bin_raw = vm_read64(job_ea + JH_EA_BINARY);
    u32 ea_bin     = (u32)(ea_bin_raw & ~7ull);          /* low 3 bits = flags */
    u32 size_bin   = ((vm_read32(job_ea + JH_SIZE_BINARY) >> 16) & 0xFFFFu) << 4;
    u8  job_type   = *(vm_base + job_ea + JH_JOB_TYPE);

    { static int _n = 0;
      if (_n < 8) {
          printf("[cellSpurs]   job[%d] @0x%08X eaBinary=0x%08X size=%u type=0x%02X hdr:",
                 idx, job_ea, ea_bin, size_bin, job_type);
          for (int o = 0; o < JH_SIZE; o += 4) {
              if ((o & 15) == 0) printf("\n      +%02X:", o);
              printf(" %08X", vm_read32(job_ea + o));
          }
          printf("\n");
      }
      _n++; }

    if (!ea_bin || !size_bin || ea_bin >= 0x10000000u) {
        static int _b = 0;
        if (__atomic_fetch_add(&_b, 1, __ATOMIC_RELAXED) < 4)
            printf("[cellSpurs]   job[%d]: implausible binary (ea=0x%08X size=%u) -- skipped\n",
                   idx, ea_bin, size_bin);
        return;
    }
    /* A jobchain job is NOT a SPURS task: Sony's jm2 stages the whole working
     * set in local store and enters the job's CRT with r3 = CellSpursJobContext2*
     * and r4 = the job descriptor, both LS pointers. Dispatching it on the task
     * ABI (arg EA in r3) left it reading a zeroed context and parking with no
     * DMA traffic at all. spu_workload_dispatch_job reproduces jm2's staging.
     * Synchronous by design: a job runs to completion, and the chain's own
     * NEXT/CALL/RET commands are what order them. */
    spu_workload_dispatch_job(vm_base + ea_bin, size_bin, job_ea, size_desc);
}

/* Walk one chain's command stream. Bounded: a malformed or self-looping stream
 * must not spin a host thread forever. */
/* ---------------------------------------------------------------------------
 * Job guards
 *
 * A job chain can begin with a GUARD command (op 7, ext 7|(1<<3)), whose EA is
 * the CellSpursJobGuard. The chain BLOCKS there until the guard's notify count
 * reaches zero; the PPU releases it with cellSpursJobGuardNotify once the job's
 * parameters are ready. With autoReset the count is restored afterwards, so a
 * looping chain waits again on the next pass.
 *
 * These were unimplemented, and the walker fell through the GUARD command into
 * the default (ignore) case. Tokyo Jungle's "soc-job" chain is
 *
 *     [0] GUARD 0x02932C00      [1] JOB 0x02932D00
 *     [2] SYNC                  [3] NEXT -> [0]        (a loop)
 *
 * so the job ran immediately, on the first pass, against a parameter block the
 * game had not filled in yet -- 0x02937580 read back as all zeroes -- and the
 * SPU went on to compute a wild DMA address from it. Honouring the guard is
 * what makes "run this job when I say so" mean anything.
 * -----------------------------------------------------------------------*/

#define MAX_JOBGUARDS 32
static struct {
    u32 ea;          /* guest CellSpursJobGuard address (128-byte aligned) */
    u32 count;       /* notifications still outstanding                    */
    u32 reset;       /* value to restore when autoReset is set             */
    u32 pending;     /* notifications received, not yet consumed        */
    u32 auto_reset;
    int active;
} s_jobguards[MAX_JOBGUARDS];

static int jg_find(u32 ea)
{
    for (int i = 0; i < MAX_JOBGUARDS; i++)
        if (s_jobguards[i].active && s_jobguards[i].ea == ea) return i;
    return -1;
}

/* cellSpursJobGuardInitialize(CellSpursJobChain* jobChain,
 *                             CellSpursJobGuard* jobGuard,
 *                             u32 notifyCount, u8 requestSpuCount, u8 autoReset)
 * Argument order confirmed against the guest: r3 is the chain (0x02932A80,
 * the handle CreateJobChainWithAttribute registered) and r4 is the guard
 * (0x02932C00, the EA the chain's GUARD command carries). */
s32 cellSpursJobGuardInitialize(u64 jc_ea, u64 guard_ea, u32 notify_count,
                                u32 request_spu_count, u32 auto_reset)
{
    (void)jc_ea; (void)request_spu_count;
    u32 ea = (u32)guard_ea;
    if (!ea) return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    int i = jg_find(ea);
    if (i < 0)
        for (i = 0; i < MAX_JOBGUARDS; i++) if (!s_jobguards[i].active) break;
    if (i >= MAX_JOBGUARDS) return CELL_SPURS_TASK_ERROR_AGAIN;

    s_jobguards[i].ea         = ea;
    s_jobguards[i].count      = notify_count;
    s_jobguards[i].reset      = notify_count;
    s_jobguards[i].pending    = 0;
    s_jobguards[i].auto_reset = auto_reset;
    s_jobguards[i].active     = 1;

    /* Mirror the count into guest memory. The SPU side reads the guard on
     * hardware; keeping the two consistent costs nothing and makes a guest-side
     * peek meaningful. */
    vm_write32(ea, notify_count);

    printf("[cellSpurs] JobGuardInitialize(guard=0x%08X chain=0x%08X notify=%u "
           "autoReset=%u)\n", ea, (u32)jc_ea, notify_count, auto_reset);
    return CELL_OK;
}

/* cellSpursJobGuardNotify(CellSpursJobGuard* jobGuard) -- one argument. */
s32 cellSpursJobGuardNotify(u64 guard_ea)
{
    u32 ea = (u32)guard_ea;
    int i = jg_find(ea);
    if (i < 0) {
        printf("[cellSpurs] JobGuardNotify(0x%08X) -- unknown guard\n", ea);
        return CELL_SPURS_TASK_ERROR_INVAL;
    }
    /* Count notifications as CREDITS rather than decrementing to a floor of
     * zero. The guard used to reload its count at the moment the walker
     * OBSERVED zero, which threw away any notify that arrived while the job
     * was still running -- the title notified 6 times and the chain completed
     * 2 laps, so the thread waiting on the other 4 completions never woke. */
    s_jobguards[i].pending++;
    { u32 remaining = (s_jobguards[i].pending >= s_jobguards[i].reset)
                    ? 0u : s_jobguards[i].reset - s_jobguards[i].pending;
      s_jobguards[i].count = remaining;
      vm_write32(ea, remaining); }
    { static int _n = 0;
      if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 8)
          printf("[cellSpurs] JobGuardNotify(0x%08X) -> %u remaining\n",
                 ea, s_jobguards[i].count); }
    return CELL_OK;
}

/* cellSpursJobGuardReset(CellSpursJobGuard*) -- restore the initial count. */
s32 cellSpursJobGuardReset(u64 guard_ea)
{
    u32 ea = (u32)guard_ea;
    int i = jg_find(ea);
    if (i < 0) return CELL_SPURS_TASK_ERROR_INVAL;
    s_jobguards[i].count = s_jobguards[i].reset;
    vm_write32(ea, s_jobguards[i].count);
    return CELL_OK;
}

/* Wait for a guard to be released. Returns 1 if it opened, 0 on timeout (the
 * chain then gives up this pass rather than spinning the walker thread
 * forever). An unknown guard opens immediately -- a chain guarded by something
 * we never saw initialised must not deadlock the walker. */
static int jg_wait(u32 ea)
{
    int i = jg_find(ea);
    if (i < 0) return 1;
    for (int spin = 0; spin < 20000; spin++) {     /* ~20 s at 1 ms */
        if (s_jobguards[i].pending >= s_jobguards[i].reset) {
            /* Consume exactly one release worth of credits, so notifies that
             * arrived during the previous lap still count. */
            s_jobguards[i].pending -= s_jobguards[i].reset;
            if (!s_jobguards[i].auto_reset) s_jobguards[i].reset = 0;
            s_jobguards[i].count = s_jobguards[i].reset;
            vm_write32(ea, s_jobguards[i].count);
            return 1;
        }
#ifdef _WIN32
        Sleep(1);
#else
        usleep(1000);
#endif
    }
    printf("[cellSpurs] guard 0x%08X still closed after 20 s -- chain gives up\n", ea);
    return 0;
}

/* Tell the application a lap of chain work is done.
 *
 * Timing is the whole point here. The obvious place is after the walk
 * FINISHES, but a guarded chain never finishes: it loops back to its GUARD and
 * blocks for the next notify. Tokyo Jungle notifies the guard, then waits on
 * this queue, and would only notify again after the event arrives -- so
 * signalling at walker exit deadlocks both sides. Signal when the WORK
 * completes: at END, and on reaching a guard with jobs run this lap. */
#ifdef _WIN32
#  define SPURS_JOB_TLS __declspec(thread)
#else
#  define SPURS_JOB_TLS __thread
#endif
extern SPURS_JOB_TLS uint32_t g_spurs_job_mbox, g_spurs_job_mbox_intr, g_spurs_job_cmd;
extern SPURS_JOB_TLS int g_spurs_job_mbox_valid;

/* SPURS_EVENT_D3=<n>: probe. The value a job query returns to the PPU is read
 * from the completion event's data3 field (the guest stores r7 at sp+0xB8 and
 * reads the count back from sp+0xBC). This lets that be confirmed by observing
 * the returned count change, without having to guess the real value first. */
static int d3_probe_valid = 0;
static u64 spurs_event_data3_probe(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("SPURS_EVENT_D3");
                 d3_probe_valid = e ? 1 : 0; v = e ? atoi(e) : 0; }
    return (u64)(unsigned)v;
}

static void jc_signal_done(u32 jc_ea, int job_index)
{
    /* Carry the job's mailbox answer in the completion event. On hardware the
     * SPU's outbound/interrupt mailbox is what the SPURS event delivers; a
     * synthetic payload means the caller reads back zero for whatever it
     * asked. Keep the chain EA in data1 for anything that used it.
     *
     * data2 is WHICH job finished: the PPU side of a job chain counts on it.
     * GT5P's audio service does
     *     do { JobGuardNotify; receive(q); i = data2; run_callback(i); }
     *     while (i + 1 < queued);
     * so a data2 pinned at 0 never ends that loop -- with the chain's lwmutex
     * held, which blocks the main thread's teardown behind it. A chain-level
     * signal (job_index < 0) sends 0. */
    u64 d2 = (u64)(unsigned)(job_index < 0 ? 0 : job_index);
    u64 mbox = 0, d3 = 0;
    (void)spurs_event_data3_probe();   /* prime d3_probe_valid */
    /* A PPU-side query submits a job and reads the reply out of the completion
     * event's data3 (the guest stores r7 at sp+0xB8 and reads the value back
     * from sp+0xBC, the low half). Tokyo Jungle asks each DSP module for its
     * input and output bus count this way -- commands 0x105 and 0x106 -- and
     * the SPU answers both in its outbound mailbox. We computed that mailbox
     * and then dropped it, pushing 0, so every module registered with a bus
     * count of 0 and the bus setup rejected the first bus it was asked for
     * ("sgxbus.c: out of range bus"): the check errors when count < requested,
     * and 0 < 1.
     *
     * Only the two count queries are forwarded. Neighbouring commands
     * (0x102/0x103/0x104) reply with LS ADDRESSES rather than counts, and
     * func_00201D38 stores each reply into module+0x1C/+0x20/+0x24. The
     * mailbox never goes in data2, which is the job index above: a mailbox
     * value there would end the consumer's wait early. */
    enum { SGX_Q_IN_BUSES = 0x105, SGX_Q_OUT_BUSES = 0x106 };
    const int answers_a_count = (g_spurs_job_cmd == SGX_Q_IN_BUSES ||
                                 g_spurs_job_cmd == SGX_Q_OUT_BUSES);
    if (g_spurs_job_mbox_valid) {
        mbox = g_spurs_job_mbox;
        d3 = g_spurs_job_mbox_intr; (void)d3;
        g_spurs_job_mbox_valid = 0;
    }
    for (int i = 0; i < s_spurs_event_queue_n; i++) {
        int rc = sys_event_queue_push_by_id(s_spurs_event_queue[i],
                                            SPURS_EVENT_PORT, jc_ea, d2,
                                            d3_probe_valid
                                                ? spurs_event_data3_probe()
                                                : (answers_a_count ? mbox : 0));
        static int n = 0;
        if (__atomic_fetch_add(&n, 1, __ATOMIC_RELAXED) < 8)
            printf("[cellSpurs] chain 0x%08X job %d done -> event queue %u "
                   "(data2=%llu rc=%d)\n",
                   jc_ea, job_index, s_spurs_event_queue[i],
                   (unsigned long long)d2, rc);
    }
}

static void jc_execute(u32 entry_ea, u32 jc_ea, u32 size_desc)
{
    u32 pc = entry_ea, ret_pc = 0;
    int jobs = 0;
    /* Bound IDLE steps, not total steps. A SPURS job chain is frequently a
     * SERVICE LOOP -- guard, job, sync, next, repeat -- and is meant to run for
     * the lifetime of the subsystem. A flat cap on total commands therefore
     * kills a perfectly healthy chain: Tokyo Jungle's audio chain hit it after
     * 1332 jobs and its sound pipeline simply stopped. What actually indicates
     * a malformed chain is spinning through commands WITHOUT running a job, so
     * count that instead and reset it whenever real work happens. */
    int idle = 0;
    for (;; idle++) {
        if (idle > 4096) break;
        u64 cmd = vm_read64(pc);
        u32 op  = (u32)(cmd & 7);
        u32 ext = (u32)(cmd & 127);

        if (cmd != 0 && op == 0) {                    /* JOB */
            { extern u32 g_spurs_job_ls_handle; g_spurs_job_ls_handle = jc_ea; }
            int idx = jobs++;
            jc_run_one_job((u32)(cmd & ~7ull), idx, size_desc);
            idle = 0;                    /* real work: the chain is healthy */
            /* Signal per JOB, not per lap. The application waits once for each
             * unit of work it queued -- this title notified the guard 7 times
             * and sat in event_queue_receive 30 times -- so batching the
             * completion into one event per pass leaves it waiting for
             * completions that, from its point of view, never arrive. */
            jc_signal_done(jc_ea, idx);
            pc += 8; continue;
        }
        if (op == 1) { pc = (u32)(cmd & ~7ull); continue; }   /* RESET_PC */
        if (op == 3) { pc = (u32)(cmd & ~7ull); continue; }   /* NEXT     */
        if (op == 4) { ret_pc = pc + 8; pc = (u32)(cmd & ~7ull); continue; }  /* CALL */
        if (op == 7) {
            if (ext == (7 | (15 << 3))) {                     /* END */
                printf("[cellSpurs] chain 0x%08X: END after %d job(s)\n", jc_ea, jobs);
                if (jobs) jc_signal_done(jc_ea, jobs - 1);
                return;
            }
            if (ext == (7 | (14 << 3))) {                     /* RET */
                if (!ret_pc) return;
                pc = ret_pc; ret_pc = 0; continue;
            }
            if (ext == (7 | (0 << 3))) {                      /* ABORT */
                printf("[cellSpurs] chain 0x%08X: ABORT\n", jc_ea);
                return;
            }
        }
        if (op == 7 && ext == (7 | (1 << 3))) {           /* GUARD */
            u32 g_ea = (u32)(cmd & ~127ull);
            if (!jg_wait(g_ea)) return;                  /* still closed */
            pc += 8; continue;
        }
        /* NOP(0) / SYNC+LWSYNC(2, one virtual SPU: nothing to wait for) /
         * FLUSH(5) / JOBLIST(6, nested arrays TODO) / SET_LABEL */
        pc += 8;
    }
    printf("[cellSpurs] chain 0x%08X: 4096 commands with no job run after %d "
           "job(s) -- malformed?\n",
           jc_ea, jobs);
}

static DWORD WINAPI jc_thread(LPVOID p)
{
    int slot = (int)(intptr_t)p;
    /* TIMING PROBE (SPURS_JC_DELAY=ms): the real jm2 chain walker is async and
     * picks up jobs as the PPU appends them + fills their descriptors. Our walk
     * is one-shot; if it reads descriptors before the PPU populates the I/O
     * (n_dma=0, empty ioBuffer), deferring the walk should let real I/O appear.
     * Confirms timing-vs-never before committing to the async rewrite. */
    { const char* d = getenv("SPURS_JC_DELAY");
      if (d && *d) Sleep((unsigned)atoi(d)); }
    jc_execute(s_jobchains[slot].entry_ea, s_jobchains[slot].jc_ea,
               s_jobchains[slot].size_desc);
    s_jobchains[slot].running = 0;
    /* Tell the application the chain is done. Without this the walk finishes
     * in silence and a thread waiting on the attached queue never runs again. */
    jc_signal_done(s_jobchains[slot].jc_ea, -1);
    return 0;
}

/* cellSpursRunJobChain -- start job chain execution (async, like the real one). */
/* cellSpursRunJobChain(const CellSpursJobChain* jobChain) -- ONE argument.
 *
 * This was declared (spurs, jobChain), so the ABI adapter fed it r3=spurs,
 * r4=<whatever>, and the chain it looked up was r4 -- never the handle
 * CreateJobChainWithAttribute registered. Every run logged
 * "UNKNOWN chain (no Create seen)" and returned CELL_OK without walking
 * anything, so the title sat in event_queue_receive waiting for SPU work that
 * was never started. Tokyo Jungle blocks its whole boot there.
 *
 * Verified against the guest: Create takes the chain in r4 (r3 is the CellSpurs)
 * and passes 0x02932A80; Run then arrives with r3=0x02932A80. Join and Shutdown
 * are the same one-argument shape. */
static s32 jc_start(u64 jc_ea, const char* who)
{
    static int s_off = -1;
    if (s_off < 0) s_off = getenv("PS3_NO_JOBCHAIN") ? 1 : 0;

    for (int i = 0; i < MAX_JOBCHAINS; i++) {
        if (s_jobchains[i].jc_ea != (u32)jc_ea) continue;
        s_jobchains[i].run_count++;
        if (s_jobchains[i].run_count <= 3) {
            printf("[cellSpurs] %s(jc=0x%08X) run#%d entry=0x%08X\n",
                   who, (u32)jc_ea, s_jobchains[i].run_count, s_jobchains[i].entry_ea);
            jc_dump_commands(who, s_jobchains[i].entry_ea, 16);
        }
        if (s_off || !s_jobchains[i].entry_ea) return CELL_OK;
        /* Coalesce: a chain already being walked must not start twice. */
        if (_InterlockedCompareExchange(&s_jobchains[i].running, 1, 0) == 0) {
            HANDLE th = CreateThread(NULL, 1u << 20, jc_thread, (LPVOID)(intptr_t)i, 0, NULL);
            if (th) CloseHandle(th);
            else s_jobchains[i].running = 0;
        }
        return CELL_OK;
    }
    printf("[cellSpurs] %s(jc=0x%08X) -- UNKNOWN chain (no Create seen)\n", who, (u32)jc_ea);
    return CELL_OK;
}

s32 cellSpursRunJobChain(u64 jc_ea)
{
    return jc_start(jc_ea, "RunJobChain");
}

/* cellSpursShutdownJobChain(CellSpursJobChain*) -- the other half of the pair.
 *
 * A title that starts a chain with Run/Kick ends it with Shutdown, then blocks
 * in Join until the chain has actually stopped. Leaving Shutdown unimplemented
 * is not harmless: the import logs UNIMPLEMENTED and returns whatever was in
 * r3, so the caller reads a garbage result for "did the shutdown take?" and a
 * title that checks it can decide the subsystem failed and tear itself down.
 *
 * Clearing `running` is what makes the pair honest: the host walker stops
 * grabbing new work, and the Join that follows has something true to observe.
 * Found in Tokyo Jungle, whose sound engine shuts its chain down during init
 * and then reports "failed to recv data from SPU" when the handshake does not
 * complete. */
s32 cellSpursShutdownJobChain(u64 jc_ea)
{
    static int _n = 0;
    if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 8) printf("[cellSpurs] ShutdownJobChain(jc=0x%08X)\n", (u32)jc_ea);
    for (int i = 0; i < MAX_JOBCHAINS; i++) {
        if (s_jobchains[i].jc_ea == (u32)jc_ea) {
            s_jobchains[i].running = 0;   /* stop the walker; Join can now succeed */
            return CELL_OK;
        }
    }
    /* Not one of ours: still CELL_OK -- the chain is, trivially, not running. */
    return CELL_OK;
}

/* cellSpursJoinJobChain(const CellSpursJobChain*) -- one argument, as above. */
s32 cellSpursJoinJobChain(u64 jc_ea)
{
    static int _n = 0;
    if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 8) printf("[cellSpurs] JoinJobChain(jc=0x%08X)\n", (u32)jc_ea);
    /* Run/Kick walk the chain on a host thread, so Join has to wait for it.
     * Returning at once told the title its jobs were done while they were
     * still running. The Simpsons Arcade Game runs and joins a chain every
     * frame, and those jobs DMA the fragment programs and vertices of draws
     * already queued in the FIFO: the RSX drain then reached the draws first
     * (black frames, glyph quads built from half-written vertices), and the
     * next frame's Run was coalesced away while this walker was still busy,
     * so that frame's data was never written at all. */
    for (int i = 0; i < MAX_JOBCHAINS; i++) {
        if (s_jobchains[i].jc_ea != (u32)jc_ea) continue;
        while (s_jobchains[i].running) {
#ifdef _WIN32
            Sleep(0);
#else
            usleep(0);
#endif
        }
    }
    return CELL_OK;
}

s32 cellSpursJobChainGetError(u64 jc_ea, u64 cause_out_ea)
{
    static int _n = 0;
    if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 8) printf("[cellSpurs] JobChainGetError(jc=0x%08X)\n", (u32)jc_ea);
    if (cause_out_ea) vm_write32((u32)cause_out_ea, 0);
    return CELL_OK;
}

/* cellSpursKickJobChain(CellSpursJobChain* jobChain, u8 numReadyCount)
 *
 * Two arguments, jobChain in r3 -- the same one-handle shape as Run/Join, not
 * (spurs, jobChain). It is the *other* way a title starts a chain: Run for one
 * that was created ready to go, Kick to hand the SPUs another `numReadyCount`
 * units of an already-created chain. Titles built on the WWS job manager use
 * Kick, so leaving this a no-op meant their chains were created and then never
 * walked -- which looks exactly like a hang with no error anywhere. */
s32 cellSpursKickJobChain(u64 jc_ea, u32 numReadyCount)
{
    (void)numReadyCount;
    return jc_start(jc_ea, "KickJobChain");
}

/* =========================================================================
 * SPURS queues (PPU<->SPU bounded FIFO; LBP's audio instance uses these).
 * Log-only bring-up: capture the shapes before wiring real state.
 * =====================================================================*/

/* SDK ABI (cell/spurs/queue.h):
 *   _cellSpursQueueInitialize(CellSpurs*, CellSpursTaskset*, CellSpursQueue*,
 *       const void* buffer, u32 size, u32 depth, CellSpursQueueDirection) */
s32 _cellSpursQueueInitialize(u64 spurs_ea, u64 taskset_ea, u64 queue_ea,
                              u64 buffer_ea, u32 size, u32 depth, u32 direction)
{
    (void)spurs_ea;
    if (!queue_ea || !buffer_ea) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (!size || !depth)         return CELL_SPURS_TASK_ERROR_INVAL;

    /* Same 128-byte big-endian line as the LF variant below, and the title's
     * own call says so: it passes q=0x032B2980 with buffer=0x032B2A00, exactly
     * 0x80 apart, just as its LFQueue passes 0x4059FD00/0x4059FD80. The
     * consumer is task 5 (image 1), whose CellSpursTaskArgument points into
     * this same 0x032B2xxx block and which reads the line with GETLLAR/PUTLLC,
     * so leaving it uninitialised handed recompiled SPU code a zero-depth
     * queue to reason about. */
    uint32_t q = (uint32_t)queue_ea;
    for (uint32_t o = 0; o < 128; o += 4) vm_write32(q + o, 0);
    /* W3 at +0x0C is the ring modulus the consumer reduces indices by: the
     * empty test is (W1-W0) mod 2*W3 and the buffer slot is index mod W3
     * (derived at 0x12914..0x129B8). Leaving it zero degenerated both and made
     * the queue look permanently empty no matter what a producer wrote. */
    vm_write32(q + 0x0C, depth);
    vm_write32(q + 0x10, size);
    vm_write32(q + 0x14, depth);
    vm_write64(q + 0x18, (u64)(uint32_t)buffer_ea);
    /* NOT direction at +0x24 and NOT init at +0x2C. Both sit inside the
     * 16-byte group at +0x20..+0x2F that the consumer owns and shifts wholesale
     * (shlqbyi <group>,1 at 0x12A0C). The trace shows the two values we used to
     * write there marching through it one byte per dequeue --
     *   +0x24: 00000002 -> 00000200 -> 00020000 -> 02000000
     *   +0x2C: 00000001 -> 00000100 -> 00010000 -> 01000000
     * -- i.e. we were feeding garbage into the SPU's own state every cycle.
     * Whatever holds direction/init, it is not these offsets. */
    vm_write64(q + 0x70, (u64)(uint32_t)taskset_ea);
    memset(vm_base + (uint32_t)buffer_ea, 0, (size_t)size * depth);

    static int _n = 0;
    if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 8)
        printf("[cellSpurs] _QueueInitialize(taskset=0x%08X q=0x%08X buf=0x%08X size=%u depth=%u dir=%u) -> BE line written\n",
               (u32)taskset_ea, (u32)queue_ea, (u32)buffer_ea, size, depth, direction);
    return CELL_OK;
}

s32 cellSpursQueueClear(u64 queue_ea)
{
    static int _n = 0;
    if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 8) printf("[cellSpurs] QueueClear(q=0x%08X)\n", (u32)queue_ea);
    return CELL_OK;
}

/* SDK ABI: cellSpursQueuePushBody(CellSpursQueue*, const void* buffer, bool isBlocking) */
s32 cellSpursQueuePushBody(u64 queue_ea, u64 data_ea, u32 isBlocking)
{
    /* SPURS_QUEUE_PUSH=1 -- EXPERIMENT, off by default.
     *
     * A deliberately self-revealing probe rather than a finished push. The
     * waiter half of the protocol is known (see PROGRESS.md phase 19): the
     * 16-byte group at queue+0x20 is a ring of blocked task ids, byte[0] the
     * count and byte[13] the cursor. The DATA half is not: which ring slot an
     * element belongs in has never been observed, because nothing has ever
     * pushed and so the consumer has never taken its non-empty path.
     *
     * So push into a CANDIDATE slot (a counter kept in the push1 half of the
     * line at +0x0C), wake the registered waiter, and let the consumer's own
     * DMA say where it actually looks. Its read address names the correct slot
     * whether or not the guess was right, which turns one run into the answer.
     * Everything is logged for that reason. */
    static int s_exp = -1;
    if (s_exp < 0) s_exp = getenv("SPURS_QUEUE_PUSH") ? 1 : 0;

    static int _n = 0;
    if (!s_exp) {
        if (_n < 8)
            printf("[cellSpurs] QueuePushBody(q=0x%08X data=0x%08X blocking=%u)\n",
                   (u32)queue_ea, (u32)data_ea, isBlocking);
        else if (_n == 8)
            printf("[cellSpurs] QueuePushBody further logs suppressed\n");
        _n++;
        return CELL_OK;
    }

    if (!queue_ea || !data_ea) return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    uint32_t q   = (uint32_t)queue_ea;
    uint32_t sz  = vm_read32(q + 0x10);
    uint32_t dep = vm_read32(q + 0x14);
    uint32_t buf = (uint32_t)vm_read64(q + 0x18);
    uint32_t tsp = (uint32_t)vm_read64(q + 0x70);      /* eaSignal: taskset */
    if (!sz || !dep || !buf) return CELL_SPURS_TASK_ERROR_INVAL;

    /* Derived ring model (see PROGRESS.md phase 22). Working the dataflow of
     * the consumer's decision at 0x12914..0x129D0 backwards:
     *
     *   r11 = normalise(W0)        W0 = queue+0x00
     *   r2  = normalise(W1)        W1 = queue+0x04
     *   r60 = W3                   W3 = queue+0x0C
     *     where normalise(v) is  cgti/nor/selb  ==  (v >= 0 ? v : ~v)
     *
     *   r81 = r2 - r11
     *   r79 = (r2 + r60) - (r11 - r60)  =  r2 - r11 + 2*r60
     *   r68 = (r11 > r2) ? r79 : r81    =  (r2 - r11) mod 2*r60
     *   ceqi r77, r68, 0                -> the EMPTY test
     *
     * and, separately at 0x129A8..0x129B8,
     *
     *   r14 = (r60 > r11) ? r11 : r11 - r60   =  index mod r60
     *
     * which is the buffer slot. So this is an ordinary ring: W0 is the pop
     * index, W1 the push index, indices run modulo 2*W3, and the slot is the
     * index modulo W3 -- the standard scheme that keeps "full" distinguishable
     * from "empty". W3 is therefore the depth.
     *
     * And that is the bug: the initialiser left queue+0x0C at ZERO, so r60 = 0
     * and the whole computation degenerates -- occupancy is (W1-W0) mod 0 and
     * the slot is index mod 0. With both indices at 0 the empty test is
     * trivially true, which is precisely why the consumer parked every time and
     * never once read the element buffer, whatever the producer wrote. */
    uint8_t* qp = vm_base + q;

    int32_t  sync  = (int32_t)vm_read32(q + 0x00);          /* W0, pop  */
    int32_t  tail  = (int32_t)vm_read32(q + 0x04);          /* W1, push */
    int32_t  mod   = (int32_t)vm_read32(q + 0x0C);          /* W3       */
    if (mod <= 0) mod = (int32_t)dep;                       /* pre-fix queues */

    int32_t  nsync = (sync < 0) ? ~sync : sync;             /* normalise */
    int32_t  cur   = (tail < 0) ? ~tail : tail;
    uint32_t slot  = (uint32_t)(cur % mod);

    memcpy(vm_base + buf + (size_t)slot * sz, vm_base + (uint32_t)data_ea, sz);

    int32_t  nxt   = (cur + 1) % (2 * mod);
    int32_t  ntail = nxt;
    vm_write32(q + 0x04, (uint32_t)ntail);

    /* Waiter ring at +0x20: byte[0] = count, byte[1..12] = task ids. */
    uint32_t waiters = qp[0x20];
    int woke = -1;
    if (waiters > 0 && waiters <= 3 && tsp) {
        /* The entry area is m_bs[1..3] ONLY -- bytes 0x21..0x23. Shifting the
         * whole 16-byte group (as a first cut of this did) walks straight over
         * direction at +0x24 and init at +0x2C and corrupts the header; the
         * trace caught it as direction turning from 00000002 into 00020000. */
        woke = qp[0x21];
        qp[0x21] = qp[0x22];
        qp[0x22] = qp[0x23];
        qp[0x23] = 0;
        qp[0x20] = (uint8_t)(waiters - 1);
        extern void spu_taskset_signal_task(uint32_t taskset_ea, uint32_t taskId);
        spu_taskset_signal_task(tsp, (uint32_t)woke);
    }

    if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 16)
        printf("[cellSpurs] QueuePush#%d q=0x%08X slot=%u (cur=%u->%u) buf=0x%08X+0x%X "
               "size=%u depth=%u waiters=%u woke=%d sync=%d/%d tail=%d->%d taskset=0x%08X\n",
               _n, q, slot, cur, nxt, buf, slot * sz, sz, dep, waiters, woke, sync, nsync, tail, ntail, tsp);
    return CELL_OK;
}

/* =========================================================================
 * Misc SPURS surface the title links (logged CELL_OK stubs).
 * =====================================================================*/

s32 cellSpursRequestIdleSpu(u64 spurs_ea, u32 wid, u32 count)
{
    const u32 ea = (u32)spurs_ea;
    s32 rc = spurs_check_wid(ea, 1, wid);
    if (rc) return rc;
    static int _n = 0;
    if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 4) printf("[cellSpurs] RequestIdleSpu(spurs=0x%08X wid=%u count=%u)\n", ea, wid, count);
    *(vm_base + ea + SPURS_WKL_IDLE2 + wid) = (u8)count;
    spurs_kernel_notify(ea);
    return CELL_OK;
}

/* CellSpursInfo (280 bytes). libsre writes these fields only -- the rest of
 * the caller's buffer, and the name bytes past the prefix's NUL, keep what
 * they held. */
s32 cellSpursGetInfo(u64 spurs_ea, u64 info_ea)
{
    const u32 ea = (u32)spurs_ea, in = (u32)info_ea;
    if (!ea || !in) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (ea & 127) return CELL_SPURS_CORE_ERROR_ALIGN;
    const u32 flags = vm_read32(ea + SPURS_CFG_FLAGS);
    vm_write32(in + 0x00, *(vm_base + ea + SPURS_NSPUS));
    vm_write32(in + 0x04, vm_read32(ea + SPURS_SPU_PRIO));
    vm_write32(in + 0x08, vm_read32(ea + SPURS_PPU_PRIO));
    *(vm_base + in + 0x0C) = (flags & SPURS_SAF_EXIT_IF_NO_WORK) ? 1 : 0;
    *(vm_base + in + 0x0D) = (flags & SPURS_SAF_SECOND_VERSION) ? 1 : 0;
    vm_write32(in + 0x10, 0);                       /* traceBuffer */
    vm_write64(in + 0x18, 0);                       /* traceBufferSize */
    vm_write32(in + 0x20, 0);                       /* traceMode */
    vm_write32(in + 0x24, vm_read32(ea + SPURS_SPU_TG));
    memcpy(vm_base + in + 0x28, vm_base + ea + SPURS_SPUS, 32);
    vm_write64(in + 0x48, vm_read64(ea + SPURS_PPU0));
    vm_write64(in + 0x50, vm_read64(ea + SPURS_PPU1));
    const u8 n = *(vm_base + ea + SPURS_PREFIX_SIZE);
    memcpy(vm_base + in + 0x58, vm_base + ea + SPURS_PREFIX, n);
    *(vm_base + in + 0x58 + n) = 0;
    vm_write32(in + 0x68, n);
    vm_write32(in + 0x6C, 0);                       /* deadlineMissCounter */
    vm_write32(in + 0x70, 0);                       /* deadlineMeetCounter */
    return CELL_OK;
}

s32 cellSpursSetExceptionEventHandler(u64 spurs_ea, u64 handler_ea, u64 arg_ea)
{
    static int _n = 0;
    if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 4)
        printf("[cellSpurs] SetExceptionEventHandler(spurs=0x%08X handler=0x%08X)\n",
               (u32)spurs_ea, (u32)handler_ea);
    return CELL_OK;
}

/* CellSpursWorkloadInfo (0x30 bytes, big-endian, 32-bit pointers), filled
 * from the instance as libsre does; the two padding bytes at 0x26 are left
 * as they were. */
s32 cellSpursGetWorkloadInfo(u64 spurs_ea, u32 wid, u64 info_ea)
{
    const u32 ea = (u32)spurs_ea, o = (u32)info_ea;
    s32 rc = spurs_check_wid(ea, info_ea != 0, wid);
    if (rc) return rc;
    const u32 info = ea + SPURS_WKL_INFO1 + wid * SPURS_WKL_INFO_SZ;
    const u32 f1 = ea + SPURS_WKL_F1 + SPURS_WKL_F1_SZ * wid;
    vm_write64(o + 0x00, vm_read64(info + 0x08));                   /* data */
    memcpy(vm_base + o + 0x08, vm_base + info + 0x18, 8);           /* priority[8] */
    vm_write32(o + 0x10, (u32)vm_read64(info + 0x00));              /* policyModule */
    vm_write32(o + 0x14, vm_read32(info + 0x10));                   /* sizePolicyModule */
    vm_write32(o + 0x18, (u32)vm_read64(ea + SPURS_WKL_H1 + 16 * wid));      /* nameClass */
    vm_write32(o + 0x1C, (u32)vm_read64(ea + SPURS_WKL_H1 + 16 * wid + 8));  /* nameInstance */
    vm_base[o + 0x20] = vm_base[ea + SPURS_WKL_CURCONT + wid];      /* contention */
    vm_base[o + 0x21] = vm_base[ea + SPURS_WKL_MINCONT + wid];
    vm_base[o + 0x22] = vm_base[ea + SPURS_WKL_MAXCONT + wid];
    vm_base[o + 0x23] = vm_base[ea + SPURS_WKL_READY1 + wid];
    vm_base[o + 0x24] = vm_base[ea + SPURS_WKL_IDLE2 + wid];        /* idleSpuRequest */
    vm_base[o + 0x25] = (vm_base[ea + SPURS_WKL_SIGNAL1 + (wid >> 3)] >> (7 - (wid & 7))) & 1;
    vm_write32(o + 0x28, (u32)vm_read64(f1 + 0x30));                /* shutdown hook */
    vm_write32(o + 0x2C, (u32)vm_read64(f1 + 0x38));                /* its argument */
    return CELL_OK;

}

/* Shutdown, as libsre does it: a workload no SPU has taken is removable at
 * once; otherwise it is shutting down until the last SPU holding it lets go
 * (the system service, spurs_kernel.c), which then reports completion to the
 * event helper thread (hook, waiter). */
s32 cellSpursShutdownWorkload(u64 spurs_ea, u32 wid)
{
    const u32 ea = (u32)spurs_ea;
    s32 rc = spurs_check_wid(ea, 1, wid);
    if (rc) return rc;
    u8* st = vm_base + ea + SPURS_WKL_STATE1 + wid;
    u8 cur = __atomic_load_n(st, __ATOMIC_SEQ_CST), next;
    do {
        if (cur <= WKL_STATE_PREPARING) return CELL_SPURS_POLICY_MODULE_ERROR_STAT;
        if (cur == WKL_STATE_SHUTTING_DOWN || cur == WKL_STATE_REMOVABLE) return CELL_OK;
        next = vm_base[ea + SPURS_WKL_STATUS1 + wid] ? WKL_STATE_SHUTTING_DOWN : WKL_STATE_REMOVABLE;
    } while (!__atomic_compare_exchange_n(st, &cur, next, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    if (next == WKL_STATE_SHUTTING_DOWN) {
        __atomic_store_n(vm_base + ea + SPURS_SYSSRV_MSG, 0xFF, __ATOMIC_SEQ_CST);
        __atomic_store_n(vm_base + ea + SPURS_SYSSRV_MESSAGE, 0xFF, __ATOMIC_SEQ_CST);
        spurs_kernel_notify(ea);
    } else {
        const u8 ev = spurs_byte_or(ea + SPURS_WKL_EVENT1 + wid, 0x01);
        if ((ev & 0x12) && !(ev & 0x01))
            spurs_kernel_shutdown_completed(ea, wid);
    }
    static int _n = 0;
    if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 8) printf("[cellSpurs] ShutdownWorkload(wid=%u)\n", wid);
    return CELL_OK;
}

s32 cellSpursWaitForWorkloadShutdown(u64 spurs_ea, u32 wid)
{
    const u32 ea = (u32)spurs_ea;
    s32 rc = spurs_check_wid(ea, 1, wid);
    if (rc) return rc;
    /* One waiter per workload: a second wait is refused (libsre: INVAL). */
    u32* waiter = spurs_u32(ea + SPURS_WKL_F1 + SPURS_WKL_F1_SZ * wid + 0x28);
    u32 none = 0;
    if (!__atomic_compare_exchange_n(waiter, &none, __builtin_bswap32(2u), 0,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return CELL_SPURS_POLICY_MODULE_ERROR_INVAL;
    /* Wait unless the shutdown is complete and its hook (if any) has run;
     * a waiter is marked so the event helper posts the semaphore. */
    u8* ev = vm_base + ea + SPURS_WKL_EVENT1 + wid;
    u8 e = __atomic_load_n(ev, __ATOMIC_SEQ_CST);
    int wait = 0;
    do {
        wait = !(e & 0x01) || (e & 0x22) == 0x02;
        if (!wait) break;
    } while (!__atomic_compare_exchange_n(ev, &e, (u8)(e | 0x10), 0,
                                          __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    if (wait) spurs_kernel_wait_shutdown_sema(ea, wid);
    return CELL_OK;
}

/* =========================================================================
 * Task LS patterns and context save area sizing.
 *
 * Saints Row 2 builds every taskset through this sequence (func_009F56E0):
 *
 *   _cellSpursTasksetAttributeInitialize(attr, ...)
 *   cellSpursTasksetAttributeSetName(attr, name)
 *   cellSpursTasksetAttributeSetTasksetSize(attr, size)
 *   cellSpursCreateTasksetWithAttribute(spurs, taskset, attr)
 *   ...
 *   _cellSpursQueueInitialize(...) / cellSpursQueueAttachLv2EventQueue(q)
 *   cellSpursTaskGetReadOnlyAreaPattern(&ro,   elf)
 *   cellSpursTaskGetContextSaveAreaSize (&size, &ls)   ls = default & ~ro
 *   _cellSpursTaskAttributeInitialize(attr2, .., size, &ls, ..)
 *
 * The ABI of the last two is not guesswork -- it is readable straight off the
 * call site. At 0x009F5878 the caller sets r3 = sp+0x70 and r4 = r25, where
 * r25 = sp+0x90 (set at 0x009F5744) is exactly the 16-byte pattern the two
 * `andc`+`std` pairs at 0x009F5888..0x009F5894 had just built. After the call
 * it does `lwz r11, 0x70(r1)` -- reading a u32 back out of r3 -- checks it
 * against 0x2D400 and bails if it does not fit, then stores the size and the
 * pattern pointer side by side at sp+0x84/sp+0x88 for the task attribute. So:
 *
 *   s32 cellSpursTaskGetContextSaveAreaSize(u32* size_out,
 *                                           const CellSpursTaskLsPattern* ls);
 *
 * A task's context save area holds the SPU register file plus every 2 KB local
 * store block the task may dirty; the read-only blocks (its code/rodata, which
 * can simply be reloaded from the ELF) are masked out by the caller first.
 * Local store is 256 KB and the pattern is 128 bits, so one bit == 2 KB.
 * =====================================================================*/

#define SPURS_LS_BLOCK      2048u    /* 256 KB local store / 128 pattern bits */
#define SPURS_CTX_HDR       2048u    /* 128 SPU registers x 16 bytes          */
#define SPURS_CTX_ALIGN     128u     /* CELL_SPURS_TASK_CONTEXT_SAVE_AREA_ALIGN */

/* cellSpursTaskGetReadOnlyAreaPattern(CellSpursTaskLsPattern* pattern,
 *                                     const void* elf)
 *
 * Mark the local-store blocks covered by the image's NON-writable PT_LOAD
 * segments. The caller clears these out of its default pattern, so anything we
 * fail to report simply ends up saved as well -- costing context space, never
 * correctness. Bit numbering is PPC-style (bit 0 = MSB of the first word), and
 * the size path below only ever popcounts the result, so a bit-order mistake
 * here cannot change the computed size.
 */
s32 cellSpursTaskGetReadOnlyAreaPattern(u64 pattern_ea, u64 elf_ea)
{
    if (!pattern_ea) return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    u64 word[2] = { 0, 0 };

    if (elf_ea) {
        const uint8_t* img = vm_base + (uint32_t)elf_ea;
        if (img[0] == 0x7F && img[1] == 'E' && img[2] == 'L' && img[3] == 'F' &&
            img[4] == 1 /*ELFCLASS32*/ && img[5] == 2 /*ELFDATA2MSB*/) {
            uint32_t e_phoff = ((uint32_t)img[0x1C] << 24) | ((uint32_t)img[0x1D] << 16) |
                               ((uint32_t)img[0x1E] << 8)  |  (uint32_t)img[0x1F];
            uint16_t e_phentsize = (uint16_t)((img[0x2A] << 8) | img[0x2B]);
            uint16_t e_phnum     = (uint16_t)((img[0x2C] << 8) | img[0x2D]);
            if (e_phentsize < 0x20) e_phentsize = 0x20;

            for (uint16_t i = 0; i < e_phnum; i++) {
                const uint8_t* ph = img + e_phoff + (size_t)i * e_phentsize;
                #define BE32(p) (((uint32_t)(p)[0] << 24) | ((uint32_t)(p)[1] << 16) | \
                                 ((uint32_t)(p)[2] << 8)  |  (uint32_t)(p)[3])
                uint32_t p_type  = BE32(ph + 0x00);
                uint32_t p_vaddr = BE32(ph + 0x08);
                uint32_t p_memsz = BE32(ph + 0x14);
                uint32_t p_flags = BE32(ph + 0x18);
                #undef BE32
                if (p_type != 1 /*PT_LOAD*/ || !p_memsz) continue;
                if (p_flags & 2 /*PF_W*/)                continue;   /* writable: must be saved */
                if ((uint64_t)p_vaddr + p_memsz > SPU_LS_SIZE) continue;

                uint32_t first = p_vaddr / SPURS_LS_BLOCK;
                uint32_t last  = (p_vaddr + p_memsz - 1) / SPURS_LS_BLOCK;
                for (uint32_t b = first; b <= last && b < 128; b++)
                    word[b >> 6] |= (u64)1 << (63 - (b & 63));
            }
        }
    }

    vm_write64((uint32_t)pattern_ea,     word[0]);
    vm_write64((uint32_t)pattern_ea + 8, word[1]);

    static int _n = 0;
    if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 8)
        printf("[cellSpurs] TaskGetReadOnlyAreaPattern(elf=0x%08X) -> %016llX %016llX\n",
               (u32)elf_ea, (unsigned long long)word[0], (unsigned long long)word[1]);
    return CELL_OK;
}

/* cellSpursTaskGetContextSaveAreaSize(u32* size_out,
 *                                     const CellSpursTaskLsPattern* lsPattern) */
s32 cellSpursTaskGetContextSaveAreaSize(u64 size_out_ea, u64 pattern_ea)
{
    if (!size_out_ea || !pattern_ea) return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    u64 hi = vm_read64((uint32_t)pattern_ea);
    u64 lo = vm_read64((uint32_t)pattern_ea + 8);

    uint32_t blocks = 0;
    for (int i = 0; i < 64; i++) {
        if (hi & ((u64)1 << i)) blocks++;
        if (lo & ((u64)1 << i)) blocks++;
    }

    uint32_t size = SPURS_CTX_HDR + blocks * SPURS_LS_BLOCK;
    size = (size + (SPURS_CTX_ALIGN - 1)) & ~(SPURS_CTX_ALIGN - 1);

    vm_write32((uint32_t)size_out_ea, size);

    static int _n = 0;
    if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 8)
        printf("[cellSpurs] TaskGetContextSaveAreaSize(ls=%016llX %016llX, %u blocks) -> %u (0x%X)\n",
               (unsigned long long)hi, (unsigned long long)lo, blocks, size, size);
    return CELL_OK;
}

/* cellSpursTasksetAttributeSetTasksetSize(CellSpursTasksetAttribute*, u32 size)
 * CreateTaskset ignores the attribute entirely (it builds the real BE layout
 * itself), so record the request and accept it. */
s32 cellSpursTasksetAttributeSetTasksetSize(CellSpursTasksetAttribute* attr, u32 size)
{
    if (!attr) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    static int _n = 0;
    if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 8) printf("[cellSpurs] TasksetAttributeSetTasksetSize(size=%u)\n", size);
    return CELL_OK;
}

/* cellSpursQueueAttachLv2EventQueue(CellSpursQueue*)
 * The queue's completion signalling runs through our own event-flag path, so
 * there is no lv2 queue to bind; accept and log. */
s32 cellSpursQueueAttachLv2EventQueue(u64 queue_ea)
{
    static int _n = 0;
    if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 8) printf("[cellSpurs] QueueAttachLv2EventQueue(q=0x%08X)\n", (u32)queue_ea);
    return CELL_OK;
}

/* ---- SPURS LFQueue (CellSpursLFQueue == the 128-byte CellSyncLFQueue line) ----
 *
 * The consumer is recompiled SPU task code that works on the queue line in main
 * memory with GETLLAR/PUTLLC, so the bytes ARE the interface. The layout and the
 * protocol below were read out of inFamous' EDGE zlib task (SPU image 1, the
 * libspurs task-side LFQueue code linked into it):
 *   GetPopPointer      LS 0x9B10..0x9F98
 *   CompletePopPointer LS 0x9FA0..0xA5A0 (wakes collected waiters via 0xA698)
 *   data fetch         LS 0xA5A8  (slot = index mod depth, at buffer + slot*size)
 *   waiter id          LS 0x9800  ((workload id << 8) | task id)
 * The PPU push is that pop protocol mirrored (pop fields <-> push fields).
 *
 *   +0x00 u16 pop  done index      +0x08 u16 push done index
 *   +0x02 u16 pop  done bitmap     +0x0A u16 push done bitmap
 *   +0x04 u16 pop  waiters (dir 3) +0x0C u16 push waiters (dir 3)
 *   +0x06 s16 pop  reserve index   +0x0E s16 push reserve index
 *   +0x10 u32 element size   +0x14 u32 depth   +0x18 u64 buffer EA
 *   +0x20 u8 bs[4]   +0x24 u32 direction
 *   +0x30 u16 pop  waiters  +0x32 u16 pop  waiter ids[15]
 *   +0x50 u16 push waiters  +0x52 u16 push waiter ids[15]
 *   +0x70 u64 owner (taskset, or SPURS|1)
 *
 * Indices run modulo 2*depth. A "waiters" word packs three 5-bit counters that
 * run modulo 30: w0 = registered (tail), w1 = woken and retried, w2 = woken
 * (head). A waiter registers its id at ids[w0 % 15] and sleeps; it is woken in
 * FIFO order from w2. For direction 3 (ANY2ANY) the code keeps those words at
 * +0x04/+0x0C instead of +0x30/+0x50 (it swaps them in and out around the
 * update; the id arrays stay put). A completion sets bit (15 - k) in the
 * bitmap, k = index - done; done then advances over the leading run of ones,
 * so out-of-order completions retire in order. At most 15 operations may be
 * outstanding per side (the bitmap width). */
enum {
    LFQ_POP_DONE = 0x00, LFQ_POP_BITS = 0x02, LFQ_POP_W3 = 0x04, LFQ_POP_RSV = 0x06,
    LFQ_PUSH_DONE = 0x08, LFQ_PUSH_BITS = 0x0A, LFQ_PUSH_W3 = 0x0C, LFQ_PUSH_RSV = 0x0E,
    LFQ_SIZE = 0x10, LFQ_DEPTH = 0x14, LFQ_BUFFER = 0x18, LFQ_DIRECTION = 0x24,
    LFQ_POP_W = 0x30, LFQ_PUSH_W = 0x50, LFQ_OWNER = 0x70,
};
#define LFQ_DIR_SPU2SPU 0u
#define LFQ_DIR_SPU2PPU 1u
#define LFQ_DIR_PPU2SPU 2u
#define LFQ_DIR_ANY2ANY 3u
#define LFQ_ERR_AGAIN   0x80410101u   /* CELL_SYNC_ERROR_AGAIN */
#define LFQ_ERR_INVAL   0x80410102u
#define LFQ_ERR_PERM    0x80410109u   /* what the SPU pop returns for a bad direction */
#define LFQ_ERR_ALIGN   0x80410110u
#define LFQ_ERR_NULL    0x80410111u

/* _cellSpursLFQueueInitialize(void* pTasksetOrSpurs, CellSpursLFQueue* pQueue,
 *      const void* buffer, u32 size, u32 depth, u32 direction)
 * Empty queue = all indices, bitmaps and waiter words zero. The SPU pop reads
 * the direction at +0x24 (and takes the ANY2ANY path on 3), so it must be set. */
s32 _cellSpursLFQueueInitialize(u64 owner_ea, u64 queue_ea, u64 buffer_ea,
                                u32 size, u32 depth, u32 direction)
{
    if (!queue_ea || !buffer_ea) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (!size || !depth)         return CELL_SPURS_TASK_ERROR_INVAL;

    uint32_t q = (uint32_t)queue_ea;
    for (uint32_t o = 0; o < 128; o += 4) vm_write32(q + o, 0);
    vm_write32(q + LFQ_SIZE, size);
    vm_write32(q + LFQ_DEPTH, depth);
    vm_write64(q + LFQ_BUFFER, (u64)(uint32_t)buffer_ea);
    vm_write32(q + LFQ_DIRECTION, direction);
    vm_write64(q + LFQ_OWNER, (u64)(uint32_t)owner_ea);

    memset(vm_base + (uint32_t)buffer_ea, 0, (size_t)size * depth);

    static int _n = 0;
    if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 8)
        printf("[cellSpurs] _LFQueueInitialize(owner=0x%08X q=0x%08X buf=0x%08X size=%u depth=%u dir=%u)\n",
               (u32)owner_ea, (u32)queue_ea, (u32)buffer_ea, size, depth, direction);
    return CELL_OK;
}

s32 cellSpursLFQueueAttachLv2EventQueue(u64 queue_ea)
{
    static int _n = 0;
    if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 8) printf("[cellSpurs] LFQueueAttachLv2EventQueue(q=0x%08X)\n", (u32)queue_ea);
    return CELL_OK;
}

static inline int lfq_mod(int v, int m) { v %= m; return v < 0 ? v + m : v; }
static inline u16 lfq_wake_next(u16 w) { u16 h = (u16)((w >> 10) & 31); return (u16)((w & 0x83FFu) | ((h == 29 ? 0 : h + 1) << 10)); }
/* id slot a waiter counter refers to: ids[c % 15], i.e. halfword (c % 15) + 1 of the line */
static inline uint32_t lfq_id_addr(uint32_t wline, u16 c) { return wline + 2u * (uint32_t)(c % 15u + 1u); }

/* A waiter id is (workload id << 8) | task id. Every taskset this HLE creates
 * is workload 0 (see cellSpursCreateTaskset), so the owner taskset is either
 * the queue's owner itself or the one taskset recorded for the instance. */
static uint32_t lfq_taskset_for(uint32_t q, u16 id)
{
    uint32_t owner = (uint32_t)vm_read64(q + LFQ_OWNER);
    if (owner && !(owner & 1u)) return owner;
    if ((id >> 8) == 0) return g_ydkj_real_taskset_ea;
    return 0;
}

/* _cellSpursLFQueuePushBody(CellSpursLFQueue* q, const void* data, u32 flags)
 * PPU-side push: reserve a slot, copy the element in, complete it, and wake
 * the SPU tasks whose pops the new element satisfies. Each step is one
 * read-modify-write of the line under the lock-line lock, which is what makes
 * it atomic against the task's GETLLAR/PUTLLC. A blocked PPU pusher polls
 * instead of registering a waiter (it never appears in the push id array).
 * Bit 0 of flags = blocking (inFamous' EDGE zlib passes 3; bit 1 is unknown
 * and ignored). */
s32 _cellSpursLFQueuePushBody(u64 queue_ea, u64 data_ea, u32 flags)
{
    const uint32_t q = (uint32_t)queue_ea;
    if (!q || !(uint32_t)data_ea) return (s32)LFQ_ERR_NULL;
    if (q & 127u)                 return (s32)LFQ_ERR_ALIGN;

    const u32 dir = vm_read32(q + LFQ_DIRECTION);
    if (dir != LFQ_DIR_PPU2SPU && dir != LFQ_DIR_ANY2ANY) return (s32)LFQ_ERR_PERM;
    const uint32_t popw  = dir == LFQ_DIR_ANY2ANY ? q + LFQ_POP_W3  : q + LFQ_POP_W;
    const uint32_t pushw = dir == LFQ_DIR_ANY2ANY ? q + LFQ_PUSH_W3 : q + LFQ_PUSH_W;
    const int depth = (int)vm_read32(q + LFQ_DEPTH);
    const u32 size  = vm_read32(q + LFQ_SIZE);
    if (depth <= 0 || depth > 0x7FFF) return (s32)LFQ_ERR_INVAL;
    const int ring = 2 * depth;

    /* 1. reserve */
    int idx;
    for (unsigned spins = 0;; spins++) {
        spu_lockline_lock();
        const int z  = ef_r16(q + LFQ_POP_DONE);
        const int y  = ef_r16(q + LFQ_PUSH_DONE);
        const int xp = (int16_t)ef_r16(q + LFQ_PUSH_RSV);
        const u16 w  = ef_r16(pushw);
        const int w0 = w & 31, w1 = (w >> 5) & 31, w2 = (w >> 10) & 31;
        const int woken = lfq_mod(w2 - w1, 30);
        const int space = depth - lfq_mod(xp - z, ring);
        const int busy  = lfq_mod(xp - y, ring);
        if (space - woken > 0 && busy + woken <= 15 && w0 == w2) {
            idx = xp;
            ef_w16(q + LFQ_PUSH_RSV, (u16)(xp + 1 == ring ? 0 : xp + 1));
            ef_line_end(q);
            break;
        }
        ef_line_end(q);
        if (!(flags & 1u)) return (s32)LFQ_ERR_AGAIN;
        if (spins == 2000) {
            static int _n = 0;
            if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 8) fprintf(stderr, "[cellSpurs] LFQueuePush q=0x%08X full for ~2s (space=%d woken=%d busy=%d)\n",
                                  q, space, woken, busy);
        }
        Sleep(1);
    }

    /* 2. copy the element into its slot */
    const uint32_t buf = (uint32_t)vm_read64(q + LFQ_BUFFER);
    /* SPURS_LFQ_LOG=1: every pushed element, as words. */
    { static int s_l = -1; if (s_l < 0) s_l = getenv("SPURS_LFQ_LOG") ? 1 : 0;
      if (s_l) { fprintf(stderr, "[lfq-push] q=0x%08X idx=%d:", q, idx);
                 for (u32 o = 0; o < size && o < 64; o += 4) fprintf(stderr, " %08X", vm_read32((uint32_t)data_ea + o));
                 fputc('\n', stderr); } }
    memcpy(vm_base + buf + (uint32_t)(idx % depth) * size, vm_base + (uint32_t)data_ea, size);

    /* 3. complete, then wake pop waiters (at most one per newly completed element) */
    u16 wake_ids[15];
    int nwake = 0;
    spu_lockline_lock();
    {
        const int y = ef_r16(q + LFQ_PUSH_DONE);
        u32 bits = ef_r16(q + LFQ_PUSH_BITS) | (1u << (15 - lfq_mod(idx - y, ring)));
        int done = 0;
        while (done < 16 && (bits & (0x8000u >> done))) done++;
        ef_w16(q + LFQ_PUSH_DONE, (u16)lfq_mod(y + done, ring));
        ef_w16(q + LFQ_PUSH_BITS, (u16)(bits << done));

        const int z = ef_r16(q + LFQ_POP_DONE);
        const int x = (int16_t)ef_r16(q + LFQ_POP_RSV);
        int pop_busy = lfq_mod(x - z, ring);
        u16 pw = ef_r16(popw);
        int pop_woken = lfq_mod(((pw >> 10) & 31) - ((pw >> 5) & 31), 30);
        int pop_waiting = lfq_mod((pw & 31) - ((pw >> 10) & 31), 30);
        if (pop_busy + pop_woken > 15) pop_waiting = 0;
        for (int i = 0; i < done && pop_waiting > 0 && pop_busy + pop_woken <= 15; i++) {
            const u16 head = (u16)((pw >> 10) & 31);
            wake_ids[nwake++] = ef_r16(lfq_id_addr(q + LFQ_POP_W, head));
            pw = lfq_wake_next(pw);
            pop_woken++; pop_waiting--;
        }
        ef_w16(popw, pw);
    }
    ef_line_end(q);

    for (int i = 0; i < nwake; i++) {
        const u16 id = wake_ids[i];
        const uint32_t ts = id == 0xFFFFu ? 0 : lfq_taskset_for(q, id);
        if (!ts) {
            static int _n = 0;
            if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 8) fprintf(stderr, "[cellSpurs] LFQueuePush q=0x%08X: cannot wake waiter id 0x%04X\n", q, id);
            continue;
        }
        spu_taskset_signal_task(ts, id & 0xFFu);
    }

    static int _n = 0;
    if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 8)
        printf("[cellSpurs] LFQueuePush q=0x%08X slot=%d flags=%u woke=%d\n", q, idx, flags, nwake);
    /* Throughput: a push per decompression request, so the rate is the
     * streaming rate of everything that goes through the EDGE zlib task. */
    { static unsigned long tot = 0; static LARGE_INTEGER t0, fq;
      if (!tot) { QueryPerformanceCounter(&t0); QueryPerformanceFrequency(&fq); }
      if ((++tot & 255) == 0) { LARGE_INTEGER t; QueryPerformanceCounter(&t);
          fprintf(stderr, "[cellSpurs] LFQueuePush total=%lu after %.1fs\n", tot,
                  (double)(t.QuadPart - t0.QuadPart) / (double)fq.QuadPart); } }
    return CELL_OK;
}
