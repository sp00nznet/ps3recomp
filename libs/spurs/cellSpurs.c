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
#include "spu_workload.h"   /* SPU image -> lifted-entry dispatch (runtime/spu) */
#include "spurs_taskset.h"  /* REAL BE CellSpursTaskset layout builders (fork Option-B) */
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base (guest mem) */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

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
    const void* pm;            /* policy-module image EA (guest) */
    u32         sizePm;
    u64         data;          /* workload data (e.g. joblist EA) */
    u32         spurs_ea;      /* owning CellSpurs instance EA */
    u8          priority[CELL_SPURS_MAX_SPU];
    u32         minContention;
    u32         maxContention;
    u32         readyCount;
} SpursWorkload;

typedef struct {
    int         in_use;
    u32         id;
    int         active;
    int         completed;
    s32         exitCode;
    void*       entryPoint;
    u64         argA;
} SpursTask;

/* Global workload table (per-SPURS instance in a real system, simplified) */
static SpursWorkload s_workloads[CELL_SPURS_MAX_WORKLOAD];
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
            s_ef_sync[i].initialized = 1;
#ifdef _WIN32
            InitializeCriticalSection(&s_ef_sync[i].cs);
            InitializeConditionVariable(&s_ef_sync[i].cv);
#else
            pthread_mutex_init(&s_ef_sync[i].mtx, NULL);
            pthread_cond_init(&s_ef_sync[i].cond, NULL);
#endif
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
static void spurs_ef_set_locked(uint32_t ea, u16 bits)
{
    u16 events        = vm_read16(ea + EF_EVENTS);
    u16 used          = (u16)(vm_read16(ea + EF_SPU_USED_SLOTS) &
                              ~vm_read16(ea + EF_SPU_PENDING_RECV));
    u16 waitmode      = vm_read16(ea + EF_SPU_WAIT_MODE);
    u16 eventsToClear = 0;
    u16 pendingRecv   = 0;

    for (int s = 0; s < CELL_SPURS_EVENT_FLAG_MAX_WAIT_SLOTS; s++) {
        u16 bit = (u16)(0x8000u >> s);
        if (!(used & bit)) continue;
        u16 mask = vm_read16(ea + EF_SPU_WAIT_MASK_ARR + 2u * s);
        u16 rel  = (u16)((events | bits) & mask);
        int mode_and = (waitmode & bit) != 0;
        if ((mask & ~rel) == 0 || (!mode_and && rel != 0)) {
            eventsToClear |= rel;
            pendingRecv   |= bit;
            vm_write16(ea + EF_PENDING_RECV_EVT + 2u * s, rel);
        }
    }

    events = (u16)(events | bits);
    if (pendingRecv) {
        vm_write16(ea + EF_SPU_PENDING_RECV,
                   (u16)(vm_read16(ea + EF_SPU_PENDING_RECV) | pendingRecv));
        if (vm_read8(ea + EF_CLEAR_MODE) == CELL_SPURS_EVENT_FLAG_CLEAR_AUTO)
            events = (u16)(events & ~eventsToClear);
    }
    vm_write16(ea + EF_EVENTS, events);

    for (int s = 0; s < CELL_SPURS_EVENT_FLAG_MAX_WAIT_SLOTS; s++) {
        if (pendingRecv & (0x8000u >> s)) {
            uint32_t taskset_ea = (uint32_t)vm_read64(ea + EF_ADDR);
            u32 taskId = vm_read8(ea + EF_WAITING_TASK_ID + s);
            fprintf(stderr, "[cellSpurs] EventFlagSet 0x%08X satisfies SPU task "
                    "slot %d (taskset=0x%08X task=%u)\n", ea, s, taskset_ea, taskId);
            spu_taskset_signal_task(taskset_ea, taskId);
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
enum {
    SPURS_WKL_READY1   = 0x00,
    SPURS_WKL_IDLE2    = 0x10,
    SPURS_WKL_CURCONT  = 0x20,
    SPURS_WKL_MINCONT  = 0x40,
    SPURS_WKL_MAXCONT  = 0x50,
    SPURS_WKL_FLAG     = 0x60,
    SPURS_WKL_SIGNAL1  = 0x70,
    SPURS_NSPUS        = 0x76,
    SPURS_WKL_STATE1   = 0x80,
    SPURS_WKL_ENABLED  = 0xB0,
    SPURS_SYSSRV_MSG   = 0xBD,
    SPURS_WKL_INFO1    = 0xB00,
    SPURS_WKL_INFO_SZ  = 0x20,
    /* CELL_SPURS_SIZE = 4096 (SDK cell/spurs/types.h). The 8192-byte variant
     * is CellSpurs2 (cellSpursInitialize*2* NIDs) which LBP does not use —
     * clearing 0x2000 here overran the game's 4KB heap block and corrupted
     * the allocator (abort in the job pump's first object destruction). */
    SPURS_INST_SIZE    = 0x1000,
};

#define MAX_SPURS_INST 4
static struct SpursInst {
    u32           ea;          /* 0 = free */
    u32           nspus;
    char          prefix[16];
    volatile long kernel_live; /* poll thread started */
} s_inst[MAX_SPURS_INST];

static struct SpursInst* spurs_inst_find(u32 ea)
{
    for (int i = 0; i < MAX_SPURS_INST; i++)
        if (s_inst[i].ea == ea) return &s_inst[i];
    return NULL;
}

#ifdef _WIN32
static DWORD WINAPI spurs_kernel_thread(LPVOID p);
#endif

static s32 spurs_initialize_common(u32 spurs_ea, u32 nspus, const char* prefix)
{
    struct SpursInst* si = spurs_inst_find(spurs_ea);
    if (!si) {
        for (int i = 0; i < MAX_SPURS_INST; i++)
            if (!s_inst[i].ea) { si = &s_inst[i]; break; }
    }
    if (!si) return CELL_SPURS_CORE_ERROR_NOMEM;

    si->ea    = spurs_ea;
    si->nspus = (nspus > 0 && nspus <= CELL_SPURS_MAX_SPU) ? nspus : 1;
    memset(si->prefix, 0, sizeof(si->prefix));
    if (prefix) memcpy(si->prefix, prefix, 15);

    /* Real BE instance: zero it, then the few live fields. (The global
     * workload table is NOT wiped here — the title may init several SPURS
     * instances before adding workloads to any of them.) */
    memset(vm_base + spurs_ea, 0, SPURS_INST_SIZE);
    *(vm_base + spurs_ea + SPURS_NSPUS) = (u8)si->nspus;
    vm_write64(spurs_ea + SPURS_WKL_FLAG, 0xFFFFFFFFFFFFFFFFull); /* no receiver */

#ifdef _WIN32
    if (!si->kernel_live) {
        si->kernel_live = 1;
        CreateThread(NULL, 1u << 20, spurs_kernel_thread, si, 0, NULL);
    }
#endif
    printf("[cellSpurs] Initialize \"%s\" ea=0x%08X nSpus=%u (real BE instance + kernel poll)\n",
           si->prefix, spurs_ea, si->nspus);
    return CELL_OK;
}

s32 cellSpursInitialize(CellSpurs* spurs, s32 nSpus, s32 spuPriority,
                        s32 ppuPriority, u8 exitIfNoWork)
{
    (void)spuPriority; (void)ppuPriority; (void)exitIfNoWork;
    if (!spurs)
        return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    return spurs_initialize_common((u32)(uintptr_t)spurs, (u32)nSpus, NULL);
}

s32 cellSpursInitializeWithAttribute(CellSpurs* spurs,
                                     const CellSpursAttribute* attr)
{
    if (!spurs || !attr)
        return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    u32 spurs_ea = (u32)(uintptr_t)spurs;
    attr = GUEST_PTR(attr, const CellSpursAttribute*);
    return spurs_initialize_common(spurs_ea, attr->nSpus, (const char*)attr->prefix);
}

s32 cellSpursFinalize(CellSpurs* spurs)
{
    if (!spurs)
        return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    struct SpursInst* si = spurs_inst_find((u32)(uintptr_t)spurs);
    if (!si)
        return CELL_SPURS_CORE_ERROR_STAT;

    printf("[cellSpurs] Finalize(ea=0x%08X)\n", si->ea);
    memset(s_workloads, 0, sizeof(s_workloads));
    si->ea = 0;   /* kernel thread sees a dead instance and idles */
    return CELL_OK;
}

s32 cellSpursAttributeInitialize(CellSpursAttribute* attr, s32 nSpus,
                                 s32 spuPriority, s32 ppuPriority,
                                 u8 exitIfNoWork)
{
    (void)ppuPriority; (void)exitIfNoWork;

    if (!attr)
        return CELL_SPURS_CORE_ERROR_NULL_POINTER;

    /* `attr` is a GUEST address (generic HLE adapter passes r3 raw). */
    attr = GUEST_PTR(attr, CellSpursAttribute*);
    memset(attr, 0, sizeof(CellSpursAttribute));
    attr->nSpus = (nSpus > 0 && nSpus <= CELL_SPURS_MAX_SPU)
                  ? (u32)nSpus : 1;

    for (int i = 0; i < CELL_SPURS_MAX_SPU; i++)
        attr->spuPriority[i] = spuPriority;

    printf("[cellSpurs] AttributeInitialize(nSpus=%d)\n", nSpus);
    return CELL_OK;
}

/* The SDK's cellSpursAttributeInitialize() macro imports this internal name
 * (NID 0x95180230). Forward to the implementation above. */
s32 _cellSpursAttributeInitialize(CellSpursAttribute* attr, s32 nSpus,
                                  s32 spuPriority, s32 ppuPriority,
                                  u8 exitIfNoWork)
{
    return cellSpursAttributeInitialize(attr, nSpus, spuPriority,
                                        ppuPriority, exitIfNoWork);
}

s32 cellSpursAttributeSetNamePrefix(CellSpursAttribute* attr,
                                    const char* prefix, u32 size)
{
    if (!attr)
        return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    attr = GUEST_PTR(attr, CellSpursAttribute*);
    const char* prefix_h = GUEST_PTR(prefix, const char*);

    if (prefix_h && size > 0) {
        u32 copyLen = size < sizeof(attr->prefix) ? size : sizeof(attr->prefix) - 1;
        memcpy(attr->prefix, prefix_h, copyLen);
        attr->prefix[copyLen] = '\0';
        attr->prefixSize = copyLen;
    }

    return CELL_OK;
}

s32 cellSpursAttributeSetSpuThreadGroupType(CellSpursAttribute* attr,
                                            s32 type)
{
    (void)type;
    if (!attr) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    return CELL_OK;
}

s32 cellSpursAttributeEnableSpuPrintfIfAvailable(CellSpursAttribute* attr)
{
    if (!attr) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    return CELL_OK;
}

s32 cellSpursGetNumSpuThread(const CellSpurs* spurs, u32* nThreads)
{
    if (!spurs || !nThreads)
        return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    struct SpursInst* si = spurs_inst_find((u32)(uintptr_t)spurs);
    u32* nThreads_h = GUEST_PTR(nThreads, u32*);

    if (!si)
        return CELL_SPURS_CORE_ERROR_STAT;

    /* out-param is guest BE */
    vm_write32((u32)(uintptr_t)nThreads, si->nspus);
    (void)nThreads_h;
    return CELL_OK;
}

s32 cellSpursSetMaxContention(CellSpurs* spurs, CellSpursWorkloadId wid,
                              u32 maxContention)
{
    (void)maxContention;

    if (!spurs) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (wid >= CELL_SPURS_MAX_WORKLOAD) return CELL_SPURS_CORE_ERROR_INVAL;
    if (!s_workloads[wid].in_use) return CELL_SPURS_CORE_ERROR_SRCH;

    s_workloads[wid].maxContention = maxContention;
    return CELL_OK;
}

s32 cellSpursSetPriorities(CellSpurs* spurs, CellSpursWorkloadId wid,
                           const u8* priorities)
{
    if (!spurs || !priorities) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    const u8* priorities_h = GUEST_PTR(priorities, const u8*);
    if (wid >= CELL_SPURS_MAX_WORKLOAD) return CELL_SPURS_CORE_ERROR_INVAL;
    if (!s_workloads[wid].in_use) return CELL_SPURS_CORE_ERROR_SRCH;

    memcpy(s_workloads[wid].priority, priorities_h, CELL_SPURS_MAX_SPU);
    return CELL_OK;
}

s32 cellSpursAttachLv2EventQueue(CellSpurs* spurs, u32 queue, u8* port,
                                 s32 isDynamic)
{
    (void)queue; (void)isDynamic;

    if (!spurs || !port) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    u8* port_h = GUEST_PTR(port, u8*);

    *port_h = 0; /* give it port 0 */
    printf("[cellSpurs] AttachLv2EventQueue(queue=%u)\n", queue);
    return CELL_OK;
}

s32 cellSpursDetachLv2EventQueue(CellSpurs* spurs, u8 port)
{
    (void)port;
    if (!spurs) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
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

    /* Find a free task slot */
    for (u32 i = 0; i < CELL_SPURS_MAX_TASK; i++) {
        if (!s_tasks[i].in_use) {
            s_tasks[i].in_use = 1;
            s_tasks[i].id = s_next_task_id++;
            s_tasks[i].active = 1;
            s_tasks[i].completed = 0;
            s_tasks[i].exitCode = 0;
            s_tasks[i].entryPoint = elf;

            if (taskId_h) *taskId_h = s_tasks[i].id;
            taskset->taskCount++;

            /* Register the task in the REAL BE taskset: writes task_info[slot]
             * (args/elf/context/ls_pattern) + sets enabled+ready bits so the PM's
             * SELECT_TASK picks it. Slot index i = the SPURS taskId (bitset bit). */
            spurs_taskset_add_task(taskset_ea, i, (uint64_t)elf_ea,
                                   (uint64_t)context_ea, task_arg, task_lsp);
            /* Bridge to the image-22 dispatch so build_context uses this taskset+task. */
            g_ydkj_real_taskset_ea = taskset_ea;
            g_ydkj_real_taskid     = i;

            printf("[cellSpurs] CreateTask(id=%u, entry=%p, arg=%08X %08X %08X %08X)\n",
                   s_tasks[i].id, elf, task_arg[0], task_arg[1], task_arg[2], task_arg[3]);

            /* One-shot: dump the memory the task argument points at, to find the
             * pointer that reads back 0 (the task GETs from EA 0 -> some field of
             * its work descriptor is null in our run). Each of the 4 arg words that
             * looks like a valid guest EA gets 64 bytes dumped as BE u32s. */
            if (getenv("LBP_TASKSET_TRACE")) {
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
                     * inline would block this PPU thread forever (deadlock). */
                    spu_workload_dispatch_async(host_elf, (uint32_t)sz,
                                                (uint32_t)(uintptr_t)context);
            }
            return CELL_OK;
        }
    }

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
      static int _n = 0; if (_n++ < 6) {
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
    (void)taskset;

    printf("[cellSpurs] SendSignal(id=%u)\n", taskId);

    for (u32 i = 0; i < CELL_SPURS_MAX_TASK; i++) {
        if (s_tasks[i].in_use && s_tasks[i].id == taskId) {
            /* In a real implementation, signal the task's wait condition */
            return CELL_OK;
        }
    }

    return CELL_SPURS_TASK_ERROR_SRCH;
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

s32 cellSpursAddWorkload(CellSpurs* spurs, CellSpursWorkloadId* wid,
                         const void* pm, u32 sizePm, u64 data,
                         const u8* priority, u32 minContention,
                         u32 maxContention)
{
    if (!spurs || !wid)
        return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    /* spurs/wid/priority are guest EAs; pm stays a guest EA (it's the SPU
     * program address consumed later by the workload dispatch). */
    uint32_t spurs_ea = (uint32_t)(uintptr_t)spurs;
    struct SpursInst* si = spurs_inst_find(spurs_ea);
    const u8* priority_h = GUEST_PTR(priority, const u8*);

    if (!si)
        return CELL_SPURS_CORE_ERROR_STAT;

    for (u32 i = 0; i < CELL_SPURS_MAX_WORKLOAD; i++) {
        if (!s_workloads[i].in_use) {
            s_workloads[i].in_use = 1;
            s_workloads[i].pm = pm;
            s_workloads[i].sizePm = sizePm;
            s_workloads[i].data = data;
            s_workloads[i].spurs_ea = spurs_ea;
            s_workloads[i].minContention = minContention;
            s_workloads[i].maxContention = maxContention;
            s_workloads[i].readyCount = 0;

            if (priority_h)
                memcpy(s_workloads[i].priority, priority_h, CELL_SPURS_MAX_SPU);
            else
                memset(s_workloads[i].priority, 0, CELL_SPURS_MAX_SPU);

            /* Publish the workload in the REAL BE instance so the game's
             * inlined kernel protocol (readyCount stores, signal bits, state
             * reads) and the policy module's own instance DMAs see it. */
            u32 info = spurs_ea + SPURS_WKL_INFO1 + i * SPURS_WKL_INFO_SZ;
            vm_write64(info + 0x00, (u64)(uintptr_t)pm);        /* addr */
            vm_write64(info + 0x08, data);                       /* arg  */
            vm_write32(info + 0x10, sizePm);                     /* size */
            vm_write32(info + 0x14, i << 24);                    /* uniqueId */
            for (int b = 0; b < 8; b++)
                *(vm_base + info + 0x18 + b) = priority_h ? priority_h[b] : 0;
            *(vm_base + spurs_ea + SPURS_WKL_STATE1  + i) = 2;   /* runnable */
            *(vm_base + spurs_ea + SPURS_WKL_MINCONT + i) = (u8)(minContention ? minContention : 1);
            *(vm_base + spurs_ea + SPURS_WKL_MAXCONT + i) = (u8)(maxContention ? maxContention : 1);
            vm_write32(spurs_ea + SPURS_WKL_ENABLED,
                       vm_read32(spurs_ea + SPURS_WKL_ENABLED) | (0x80000000u >> i));
            *(vm_base + spurs_ea + SPURS_SYSSRV_MSG) = 0xFF;

            /* wid out-param is guest BE */
            vm_write32((u32)(uintptr_t)wid, i);
            printf("[cellSpurs] AddWorkload(wid=%u, pm=%p, size=%u)\n",
                   i, pm, sizePm);
            return CELL_OK;
        }
    }

    return CELL_SPURS_CORE_ERROR_NOMEM;
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
    if (!attr) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    uint32_t attr_ea = (uint32_t)(uintptr_t)attr;

    /* Decode the REAL BE attribute from guest memory. */
    u32 pm_ea = vm_read32(attr_ea + WKATTR_PM);
    u32 pm_sz = vm_read32(attr_ea + WKATTR_SIZE);
    u64 data  = vm_read64(attr_ea + WKATTR_DATA);
    u32 minc  = vm_read32(attr_ea + WKATTR_MIN_CONT);
    u32 maxc  = vm_read32(attr_ea + WKATTR_MAX_CONT);
    u32 nmcls = vm_read32(attr_ea + WKATTR_NAME_CLASS);
    u32 nmins = vm_read32(attr_ea + WKATTR_NAME_INST);

    {   /* Layout ground truth: dump the raw attr words for the first few calls
         * (if the decode above prints nonsense, these bytes are the arbiter). */
        static int _n = 0;
        if (_n < 3) {
            printf("[cellSpurs] AddWorkloadWA attr=0x%08X raw:", attr_ea);
            for (int o = 0; o < 0x40; o += 4) {
                if ((o & 15) == 0) printf("\n    +%02X:", o);
                printf(" %08X", vm_read32(attr_ea + o));
            }
            printf("\n");
        } else if (_n == 3) {
            printf("[cellSpurs] AddWorkloadWA raw dumps suppressed from here\n");
        }
        printf("[cellSpurs] AddWorkloadWA: pm=0x%08X size=%u data=0x%016llX minC=%u maxC=%u name=%s/%s\n",
               pm_ea, pm_sz, (unsigned long long)data, minc, maxc,
               nmcls ? (const char*)(vm_base + nmcls) : "-",
               nmins ? (const char*)(vm_base + nmins) : "-");
        if (_n == 0 && pm_ea && pm_ea < 0x10000000u) {
            printf("[cellSpurs] PM@0x%08X first 96B:", pm_ea);
            for (int o = 0; o < 96; o += 4) {
                if ((o & 15) == 0) printf("\n    +%02X:", o);
                printf(" %08X", vm_read32(pm_ea + o));
            }
            printf("\n");
        }
        _n++;
    }

    /* Forward the BE-decoded values (priority as guest EA of the 8-byte table). */
    return cellSpursAddWorkload(spurs, wid, (const void*)(uintptr_t)pm_ea,
                               pm_sz, data,
                               (const u8*)(uintptr_t)(attr_ea + WKATTR_PRIORITY),
                               minc, maxc);
}

/* The SDK-versioned workload-attribute initializer (the import the game links;
 * NID differs from the non-underscore inline wrapper). Args arrive raw in
 * r3..r10; writes the REAL BE layout so AddWorkloadWithAttribute round-trips. */
s32 _cellSpursWorkloadAttributeInitialize(u64 attr_ea, u32 revision, u32 sdkVersion,
                                          u64 pm_ea, u32 size, u64 data,
                                          u64 prio_ea, u32 minContention)
{
    if (!attr_ea) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    vm_write32((u32)attr_ea + WKATTR_REVISION,   revision);
    vm_write32((u32)attr_ea + WKATTR_SDKVERSION, sdkVersion);
    vm_write32((u32)attr_ea + WKATTR_PM,         (u32)pm_ea);
    vm_write32((u32)attr_ea + WKATTR_SIZE,       size);
    vm_write64((u32)attr_ea + WKATTR_DATA,       data);
    for (int i = 0; i < 8; i++)
        *(vm_base + (u32)attr_ea + WKATTR_PRIORITY + i) =
            prio_ea ? *(vm_base + (u32)prio_ea + i) : 0;
    vm_write32((u32)attr_ea + WKATTR_MIN_CONT, minContention);
    vm_write32((u32)attr_ea + WKATTR_MAX_CONT, 1);   /* 9th arg is beyond the 8-GPR adapter */
    vm_write32((u32)attr_ea + WKATTR_NAME_CLASS, 0);
    vm_write32((u32)attr_ea + WKATTR_NAME_INST,  0);
    vm_write32((u32)attr_ea + WKATTR_HOOK,     0);
    vm_write32((u32)attr_ea + WKATTR_HOOK_ARG, 0);
    printf("[cellSpurs] _WorkloadAttributeInitialize(attr=0x%08X pm=0x%08X size=%u data=0x%llX minC=%u)\n",
           (u32)attr_ea, (u32)pm_ea, size, (unsigned long long)data, minContention);
    return CELL_OK;
}

s32 cellSpursWorkloadAttributeSetName(u64 attr_ea, u64 nameClass_ea, u64 nameInstance_ea)
{
    if (!attr_ea) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
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
    if (!spurs) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (wid >= CELL_SPURS_MAX_WORKLOAD) return CELL_SPURS_CORE_ERROR_INVAL;
    if (!s_workloads[wid].in_use) return CELL_SPURS_CORE_ERROR_SRCH;

    s_workloads[wid].in_use = 0;
    printf("[cellSpurs] RemoveWorkload(wid=%u)\n", wid);
    return CELL_OK;
}

s32 cellSpursWorkloadAttributeInitialize(CellSpursWorkloadAttribute* attr,
                                         u32 revision, u32 sdkVersion,
                                         const void* pm, u32 sizePm,
                                         u64 data, const u8* priority,
                                         u32 minContention,
                                         u32 maxContention)
{
    if (!attr) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    attr = GUEST_PTR(attr, CellSpursWorkloadAttribute*);
    const u8* priority_h = GUEST_PTR(priority, const u8*);

    memset(attr, 0, sizeof(CellSpursWorkloadAttribute));
    attr->revision = revision;
    attr->sdkVersion = sdkVersion;
    attr->pm = (u64)(uintptr_t)pm;   /* pm kept as guest EA */
    attr->sizePm = sizePm;
    attr->data = data;
    attr->minContention = minContention;
    attr->maxContention = maxContention;

    if (priority_h)
        memcpy(attr->priority, priority_h, CELL_SPURS_MAX_SPU);

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * The SPURS "kernel": one host poll thread per instance (the virtual SPU).
 *
 * The game kicks work by storing a nonzero wklReadyCount1[wid] byte or a
 * wklSignal1 bit into the instance — mostly with INLINED atomics (LBP never
 * calls an API for it beyond cellSpursReadyCountStore). The kernel thread
 * polls those real BE fields, consumes one ready unit (decrement / clear the
 * signal bit, like the real kernel's dispatch), and runs the workload's
 * policy module to completion via spu_run_policy_module.
 * -----------------------------------------------------------------------*/
typedef struct {
    spu_lifted_entry_fn fn;
    int                 image_id;
    int                 resolved;   /* 0=not tried, 1=found, -1=missing */
} WklPm;
static WklPm s_wkl_pm[CELL_SPURS_MAX_WORKLOAD];

static WklPm* spurs_resolve_pm(u32 wid)
{
    WklPm* r = &s_wkl_pm[wid];
    if (r->resolved) return r->resolved > 0 ? r : NULL;
    SpursWorkload* w = &s_workloads[wid];
    uint64_t fp = spu_workload_fingerprint(vm_base + (uint32_t)(uintptr_t)w->pm,
                                           w->sizePm);
    r->fn = spu_workload_find_img(fp, &r->image_id);
    r->resolved = r->fn ? 1 : -1;
    if (r->fn)
        printf("[cellSpurs] wid=%u PM resolved (fp=0x%016llX image=%d)\n",
               wid, (unsigned long long)fp, r->image_id);
    else
        printf("[cellSpurs] wid=%u PM NOT LIFTED (fp=0x%016llX size=%u) -- workload will not run\n",
               wid, (unsigned long long)fp, w->sizePm);
    return r->fn ? r : NULL;
}

#ifdef _WIN32
static DWORD WINAPI spurs_kernel_thread(LPVOID p)
{
    struct SpursInst* si = (struct SpursInst*)p;
    static volatile long s_pm_off = -1;
    if (s_pm_off < 0) s_pm_off = getenv("PS3_NO_SPURS_PM") ? 1 : 0;

    fprintf(stderr, "[spurs-kern] \"%s\" poll thread live: ea=0x%08X pm=%s\n",
            si->prefix, si->ea, s_pm_off ? "DISABLED (PS3_NO_SPURS_PM)" : "enabled");
    fflush(stderr);

    /* Instance change detector (SPURS_KERN_WATCH=1).
     *
     * We assumed the title kicks a workload by poking wklReadyCount/wklSignal
     * with inlined atomics -- but this thread watches exactly those bytes every
     * 1 ms and has never once seen them nonzero, across whole runs. Rather than
     * guess again, shadow the head of the instance and report EVERY byte the
     * title changes. Whatever the real kick is, it has to land in here. */
    static const u32 WATCH_LEN = 0xC0;
    unsigned char shadow[0xC0];
    int shadow_primed = 0;
    int watch = getenv("SPURS_KERN_WATCH") ? 1 : 0;
    int changes_logged = 0;

    for (;;) {
        Sleep(1);
        u32 ea = si->ea;
        if (!ea || s_pm_off) continue;

        if (watch) {
            const unsigned char* live = (const unsigned char*)vm_base + ea;
            if (!shadow_primed) { memcpy(shadow, live, WATCH_LEN); shadow_primed = 1; }
            else if (memcmp(shadow, live, WATCH_LEN) != 0) {
                for (u32 o = 0; o < WATCH_LEN; o++) {
                    if (shadow[o] == live[o]) continue;
                    if (changes_logged < 200) {
                        changes_logged++;
                        fprintf(stderr, "[spurs-kern] \"%s\" INSTANCE +0x%02X: %02X -> %02X%s\n",
                                si->prefix, o, shadow[o], live[o],
                                o < 16                    ? "  (wklReadyCount1)" :
                                o >= 0x70 && o < 0x76     ? "  (wklSignal1)"     :
                                o >= 0x80 && o < 0x90     ? "  (wklState1)"      :
                                o >= 0xB0 && o < 0xB4     ? "  (wklEnabled)"     : "");
                    }
                }
                memcpy(shadow, live, WATCH_LEN);
                fflush(stderr);
            }
        }

        u32 enabled = vm_read32(ea + SPURS_WKL_ENABLED);

        /* Kick visibility (can't-miss): log ANY nonzero readyCount/signal state
         * even for wids the dispatch filter below would skip — the game pokes
         * these bytes with inlined atomics and this is our only tap. */
        {
            static int _seen[MAX_SPURS_INST][16];
            int slot = (int)(si - s_inst);
            u32 sig = vm_read32(ea + SPURS_WKL_SIGNAL1) >> 16;
            for (u32 w = 0; w < 16; w++) {
                u8 rc = *(vm_base + ea + SPURS_WKL_READY1 + w);
                if ((rc || (sig & (0x8000u >> w))) && _seen[slot][w] < 4) {
                    _seen[slot][w]++;
                    fprintf(stderr, "[spurs-kern] \"%s\" POKE wid=%u ready=%u sig=%u enabled=%d state=%u\n",
                            si->prefix, w, rc, (sig >> (15 - w)) & 1,
                            (enabled >> (31 - w)) & 1,
                            *(vm_base + ea + SPURS_WKL_STATE1 + w));
                }
            }
        }
        if (!enabled) continue;

        for (u32 wid = 0; wid < 16; wid++) {
            if (!(enabled & (0x80000000u >> wid))) continue;
            if (*(vm_base + ea + SPURS_WKL_STATE1 + wid) != 2) continue;
            if (!s_workloads[wid].in_use || s_workloads[wid].spurs_ea != ea) continue;

            /* A SPURS policy module is a PERSISTENT SPU program. The real kernel
             * schedules an ENABLED, runnable workload onto an SPU and the module
             * then polls its OWN job queue in main memory; being enabled is the
             * trigger, not a per-job kick. That is why this title never writes
             * wklReadyCount, never sets a signal bit, and never calls
             * cellSpursReadyCountStore -- on hardware it does not have to. Gating
             * dispatch on a kick meant the module never ran at all, so nothing
             * ever called cellSpursEventFlagSet and the title's loading thread
             * blocked forever.
             *
             * Run one scheduling quantum per enabled workload per pass: the
             * module does its work, exits to the kernel (LS 0x9C0) when it has
             * none, and we re-enter it on the next pass -- which is exactly what
             * the real kernel's dispatch loop does.
             *
             * readyCount/wklSignal are still honoured when a title DOES use them:
             * consume one unit so a kick-driven title paces the same as before. */
            volatile u8* rdy = vm_base + ea + SPURS_WKL_READY1 + wid;
            u32 sig = vm_read32(ea + SPURS_WKL_SIGNAL1) >> 16;    /* be u16 @0x70 */
            int kicked = (*rdy != 0) || ((sig & (0x8000u >> wid)) != 0);
            if (*rdy) (*rdy)--;
            if (sig & (0x8000u >> wid))
                vm_write32(ea + SPURS_WKL_SIGNAL1,
                           (vm_read32(ea + SPURS_WKL_SIGNAL1) & ~((0x8000u >> wid) << 16)));

            WklPm* r = spurs_resolve_pm(wid);
            if (!r) continue;

            /* Idle backoff: running EVERY enabled workload's module EVERY 1ms
             * pass (x N instance threads) burned ~5 host cores on modules that
             * immediately exit-to-kernel with no work, starving the actual
             * decode/render threads (LBP movie at ~1fps while 500% CPU).
             * A module that keeps finding nothing gets re-run every 2nd, 4th,
             * ... up to 16th pass; an explicit kick (readyCount/signal) resets
             * it to every pass, so kick-driven latency is unchanged. */
            {
                static u8 s_idle[CELL_SPURS_MAX_WORKLOAD];      /* idle streak (log2 cadence) */
                static u32 s_pass_no;                            /* shared pass counter is fine */
                if (wid == 0) s_pass_no++;
                if (kicked) s_idle[wid] = 0;
                u32 cad = 1u << (s_idle[wid] > 4 ? 4 : s_idle[wid]);
                if (!kicked && (s_pass_no & (cad - 1)) != 0) continue;
                extern volatile unsigned g_spurs_pm_polls;
                u32 polls_before = g_spurs_pm_polls;   /* heuristic only */
                (void)polls_before;

            {   static int _n = 0;
                if (_n < 8) { _n++;
                    fprintf(stderr, "[spurs-kern] \"%s\" dispatch wid=%u (enabled, state=2) "
                                    "image=%d ready=%u\n", si->prefix, wid, r->image_id, *rdy);
                    fflush(stderr); } }

            /* Live workload arg from the real wklInfo (the game may update it). */
            u64 arg = vm_read64(ea + SPURS_WKL_INFO1 + wid * SPURS_WKL_INFO_SZ + 8);
            *(vm_base + ea + SPURS_WKL_CURCONT + wid) = 1;
            spu_run_policy_module(r->fn, r->image_id,
                                  (const uint8_t*)vm_base + (uint32_t)(uintptr_t)s_workloads[wid].pm,
                                  s_workloads[wid].sizePm, arg, wid, ea);
            *(vm_base + ea + SPURS_WKL_CURCONT + wid) = 0;
            /* "Found work" heuristic: a module that did something polls the
             * kernel for MORE work before exiting (selectWorkload calls >0);
             * an idle module exits immediately with polls==0. Grow the idle
             * streak on the latter, reset on the former. */
            if (g_spurs_pm_polls == 0) { if (s_idle[wid] < 8) s_idle[wid]++; }
            else s_idle[wid] = 0;
            }
        }
    }
}
#endif

s32 cellSpursReadyCountStore(CellSpurs* spurs, CellSpursWorkloadId wid,
                             u32 value)
{
    if (!spurs) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (wid >= CELL_SPURS_MAX_WORKLOAD) return CELL_SPURS_CORE_ERROR_INVAL;
    if (!s_workloads[wid].in_use) return CELL_SPURS_CORE_ERROR_SRCH;

    s_workloads[wid].readyCount = value;
    /* The real store: the instance byte the kernel (poll thread) watches. */
    *(vm_base + (u32)(uintptr_t)spurs + SPURS_WKL_READY1 + wid) = (u8)value;
    {   static int _n = 0;
        if (_n < 32)
            printf("[cellSpurs] ReadyCountStore(wid=%u, value=%u)\n", wid, value);
        else if (_n == 32)
            printf("[cellSpurs] ReadyCountStore further logs suppressed\n");
        _n++;
    }
    return CELL_OK;
}

s32 cellSpursReadyCountSwap(CellSpurs* spurs, CellSpursWorkloadId wid,
                            u32* old, u32 value)
{
    if (!spurs || !old) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (wid >= CELL_SPURS_MAX_WORKLOAD) return CELL_SPURS_CORE_ERROR_INVAL;
    if (!s_workloads[wid].in_use) return CELL_SPURS_CORE_ERROR_SRCH;

    *old = s_workloads[wid].readyCount;
    s_workloads[wid].readyCount = value;
    return CELL_OK;
}

s32 cellSpursReadyCountCompareAndSwap(CellSpurs* spurs,
                                       CellSpursWorkloadId wid,
                                       u32* old, u32 compare, u32 value)
{
    if (!spurs || !old) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (wid >= CELL_SPURS_MAX_WORKLOAD) return CELL_SPURS_CORE_ERROR_INVAL;
    if (!s_workloads[wid].in_use) return CELL_SPURS_CORE_ERROR_SRCH;

    *old = s_workloads[wid].readyCount;
    if (s_workloads[wid].readyCount == compare)
        s_workloads[wid].readyCount = value;

    return CELL_OK;
}

s32 cellSpursWakeUp(CellSpurs* spurs)
{
    if (!spurs) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    /* In a full implementation, wake the worker threads */
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

    { static int _n=0; if (_n++ < 40 || (_n%1000)==0)
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
                    fprintf(stderr, "[cellSpurs] EventFlagWait BLOCKED %us on pattern 0x%04X "
                                    "(mode=%s, bits=0x%04X) flagEA=0x%08X -- waiting for an SPU "
                                    "workload to cellSpursEventFlagSet it\n",
                            waits / 500, pattern,
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

    /* Hand back the observed bits; consume the received ones on AUTO clear. */
    vm_write16(bits_ea, current);
    u16 received = (mode == CELL_SPURS_EVENT_FLAG_AND) ? pattern
                                                       : (u16)(current & pattern);
    if (vm_read8(ea + EF_CLEAR_MODE) == CELL_SPURS_EVENT_FLAG_CLEAR_AUTO)
        vm_write16(ea + EF_EVENTS, (u16)(current & ~received));

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
    if (vm_read8(ea + EF_CLEAR_MODE) == CELL_SPURS_EVENT_FLAG_CLEAR_AUTO)
        vm_write16(ea + EF_EVENTS, (u16)(current & ~received));

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
    vm_write16(ea + EF_EVENTS, (u16)(vm_read16(ea + EF_EVENTS) & ~bits));
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
    (void)taskset;
    printf("[cellSpurs] _SendSignal(taskId=%u)\n", taskId);
    /* In recomp without SPU execution, signals are no-ops */
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

    for (int i = 0; i < MAX_JOBCHAINS; i++) {
        if (!s_jobchains[i].jc_ea || s_jobchains[i].jc_ea == (u32)jc_ea) {
            s_jobchains[i].jc_ea     = (u32)jc_ea;
            s_jobchains[i].entry_ea  = entry;
            s_jobchains[i].size_desc = (u16)(sd_mg >> 16);
            s_jobchains[i].max_grab  = (u16)(sd_mg & 0xFFFFu);
            s_jobchains[i].run_count = 0;
            return CELL_OK;
        }
    }
    printf("[cellSpurs] CreateJobChainWithAttribute: registry full\n");
    return CELL_OK;
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
        if (_b++ < 4)
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
static void jc_execute(u32 entry_ea, u32 jc_ea, u32 size_desc)
{
    u32 pc = entry_ea, ret_pc = 0;
    int jobs = 0;
    for (int step = 0; step < 4096; step++) {
        u64 cmd = vm_read64(pc);
        u32 op  = (u32)(cmd & 7);
        u32 ext = (u32)(cmd & 127);

        if (cmd != 0 && op == 0) {                    /* JOB */
            jc_run_one_job((u32)(cmd & ~7ull), jobs++, size_desc);
            pc += 8; continue;
        }
        if (op == 1) { pc = (u32)(cmd & ~7ull); continue; }   /* RESET_PC */
        if (op == 3) { pc = (u32)(cmd & ~7ull); continue; }   /* NEXT     */
        if (op == 4) { ret_pc = pc + 8; pc = (u32)(cmd & ~7ull); continue; }  /* CALL */
        if (op == 7) {
            if (ext == (7 | (15 << 3))) {                     /* END */
                printf("[cellSpurs] chain 0x%08X: END after %d job(s)\n", jc_ea, jobs);
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
        /* NOP(0) / SYNC+LWSYNC(2, one virtual SPU: nothing to wait for) /
         * FLUSH(5) / JOBLIST(6, nested arrays TODO) / GUARD / SET_LABEL */
        pc += 8;
    }
    printf("[cellSpurs] chain 0x%08X: hit the 4096-step bound after %d job(s) -- malformed?\n",
           jc_ea, jobs);
}

#ifdef _WIN32
static DWORD WINAPI jc_thread(LPVOID p)
{
    int slot = (int)(intptr_t)p;
    /* TIMING PROBE (LBP_JC_DELAY=ms): the real jm2 chain walker is async and
     * picks up jobs as the PPU appends them + fills their descriptors. Our walk
     * is one-shot; if it reads descriptors before the PPU populates the I/O
     * (n_dma=0, empty ioBuffer), deferring the walk should let real I/O appear.
     * Confirms timing-vs-never before committing to the async rewrite. */
    { const char* d = getenv("LBP_JC_DELAY");
      if (d && *d) Sleep((unsigned)atoi(d)); }
    jc_execute(s_jobchains[slot].entry_ea, s_jobchains[slot].jc_ea,
               s_jobchains[slot].size_desc);
    s_jobchains[slot].running = 0;
    return 0;
}
#endif

/* cellSpursRunJobChain -- start job chain execution (async, like the real one). */
s32 cellSpursRunJobChain(u64 spurs_ea, u64 jc_ea)
{
    (void)spurs_ea;
    static int s_off = -1;
    if (s_off < 0) s_off = getenv("PS3_NO_JOBCHAIN") ? 1 : 0;

    for (int i = 0; i < MAX_JOBCHAINS; i++) {
        if (s_jobchains[i].jc_ea != (u32)jc_ea) continue;
        s_jobchains[i].run_count++;
        if (s_jobchains[i].run_count <= 3) {
            printf("[cellSpurs] RunJobChain(jc=0x%08X) run#%d entry=0x%08X\n",
                   (u32)jc_ea, s_jobchains[i].run_count, s_jobchains[i].entry_ea);
            jc_dump_commands("RunJobChain", s_jobchains[i].entry_ea, 16);
        }
        if (s_off || !s_jobchains[i].entry_ea) return CELL_OK;
#ifdef _WIN32
        /* Coalesce: a chain already being walked must not start twice. */
        if (_InterlockedCompareExchange(&s_jobchains[i].running, 1, 0) == 0) {
            HANDLE th = CreateThread(NULL, 1u << 20, jc_thread, (LPVOID)(intptr_t)i, 0, NULL);
            if (th) CloseHandle(th);
            else s_jobchains[i].running = 0;
        }
#endif
        return CELL_OK;
    }
    printf("[cellSpurs] RunJobChain(jc=0x%08X) -- UNKNOWN chain (no Create seen)\n", (u32)jc_ea);
    return CELL_OK;
}

s32 cellSpursJoinJobChain(u64 spurs_ea, u64 jc_ea)
{
    (void)spurs_ea;
    static int _n = 0;
    if (_n++ < 8) printf("[cellSpurs] JoinJobChain(jc=0x%08X)\n", (u32)jc_ea);
    return CELL_OK;
}

s32 cellSpursJobChainGetError(u64 jc_ea, u64 cause_out_ea)
{
    static int _n = 0;
    if (_n++ < 8) printf("[cellSpurs] JobChainGetError(jc=0x%08X)\n", (u32)jc_ea);
    if (cause_out_ea) vm_write32((u32)cause_out_ea, 0);
    return CELL_OK;
}

/* cellSpursKickJobChain — kick a running job chain */
s32 cellSpursKickJobChain(void* spurs, void* jobChain)
{
    (void)spurs; (void)jobChain;
    return CELL_OK;
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
    static int _n = 0;
    if (_n++ < 8)
        printf("[cellSpurs] _QueueInitialize(taskset=0x%08X q=0x%08X buf=0x%08X size=%u depth=%u dir=%u)\n",
               (u32)taskset_ea, (u32)queue_ea, (u32)buffer_ea, size, depth, direction);
    return CELL_OK;
}

s32 cellSpursQueueClear(u64 queue_ea)
{
    static int _n = 0;
    if (_n++ < 8) printf("[cellSpurs] QueueClear(q=0x%08X)\n", (u32)queue_ea);
    return CELL_OK;
}

/* SDK ABI: cellSpursQueuePushBody(CellSpursQueue*, const void* buffer, bool isBlocking) */
s32 cellSpursQueuePushBody(u64 queue_ea, u64 data_ea, u32 isBlocking)
{
    static int _n = 0;
    if (_n < 8)
        printf("[cellSpurs] QueuePushBody(q=0x%08X data=0x%08X blocking=%u)\n",
               (u32)queue_ea, (u32)data_ea, isBlocking);
    else if (_n == 8)
        printf("[cellSpurs] QueuePushBody further logs suppressed\n");
    _n++;
    return CELL_OK;
}

/* =========================================================================
 * Misc SPURS surface the title links (logged CELL_OK stubs).
 * =====================================================================*/

s32 cellSpursRequestIdleSpu(u64 spurs_ea)
{
    static int _n = 0;
    if (_n++ < 4) printf("[cellSpurs] RequestIdleSpu(spurs=0x%08X)\n", (u32)spurs_ea);
    return CELL_OK;
}

s32 cellSpursGetInfo(u64 spurs_ea, u64 info_ea)
{
    struct SpursInst* si = spurs_inst_find((u32)spurs_ea);
    static int _n = 0;
    if (_n++ < 4) printf("[cellSpurs] GetInfo(spurs=0x%08X info=0x%08X)\n", (u32)spurs_ea, (u32)info_ea);
    if (info_ea) {
        vm_write32((u32)info_ea, si ? si->nspus : 1);   /* nSpus */
        for (int o = 4; o < 0x28; o += 4) vm_write32((u32)info_ea + o, 0);
    }
    return CELL_OK;
}

s32 cellSpursSetExceptionEventHandler(u64 spurs_ea, u64 handler_ea, u64 arg_ea)
{
    static int _n = 0;
    if (_n++ < 4)
        printf("[cellSpurs] SetExceptionEventHandler(spurs=0x%08X handler=0x%08X)\n",
               (u32)spurs_ea, (u32)handler_ea);
    return CELL_OK;
}

s32 cellSpursGetWorkloadInfo(u64 spurs_ea, u32 wid, u64 info_ea)
{
    static int _n = 0;
    if (_n++ < 8) printf("[cellSpurs] GetWorkloadInfo(wid=%u info=0x%08X)\n", wid, (u32)info_ea);

    if (!spurs_ea || !info_ea)
        return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (wid >= CELL_SPURS_MAX_WORKLOAD || !s_workloads[wid].in_use)
        return CELL_SPURS_CORE_ERROR_INVAL;

    /* CellSpursWorkloadInfo is a GUEST out-buffer (cell/spurs/workload_types.h),
     * big-endian, 32-bit pointers. This used to return CELL_OK writing NOTHING,
     * so the guest read whatever stale bytes sat at info_ea as the descriptor.
     * Fill the header from our workload mirror + the live BE instance counters;
     * zero the pointer/name/hook fields we do not track. Byte fields are single
     * bytes (endian-neutral); multi-byte fields go through vm_write* (BE). */
    const SpursWorkload* w = &s_workloads[wid];
    u32 base = (u32)info_ea;
    for (u32 o = 0; o < 0x30; o += 4) vm_write32(base + o, 0);   /* header clean */

    vm_write64(base + 0x00, w->data);                            /* data        */
    for (int b = 0; b < 8 && b < CELL_SPURS_MAX_SPU; b++)
        *(vm_base + base + 0x08 + b) = w->priority[b];           /* priority[8] */
    vm_write32(base + 0x10, (u32)(uintptr_t)w->pm);              /* policyModule (32-bit EA) */
    vm_write32(base + 0x14, w->sizePm);                          /* sizePolicyModule */
    /* nameClass/nameInstance (0x18/0x1C): not tracked -> left 0 by the zero above. */

    u32 se = (u32)spurs_ea;
    *(vm_base + base + 0x20) = *(vm_base + se + SPURS_WKL_CURCONT + wid);  /* contention   */
    *(vm_base + base + 0x21) = (u8)w->minContention;                       /* minContention */
    *(vm_base + base + 0x22) = (u8)w->maxContention;                       /* maxContention */
    *(vm_base + base + 0x23) = *(vm_base + se + SPURS_WKL_READY1 + wid);   /* readyCount   */
    *(vm_base + base + 0x24) = *(vm_base + se + SPURS_WKL_IDLE2  + wid);   /* idleSpuRequest */
    u32 sig = vm_read32(se + SPURS_WKL_SIGNAL1) >> 16;
    *(vm_base + base + 0x25) = (sig & (0x8000u >> wid)) ? 1 : 0;           /* hasSignal    */
    return CELL_OK;
}

s32 cellSpursShutdownWorkload(u64 spurs_ea, u32 wid)
{
    static int _n = 0;
    if (_n++ < 8) printf("[cellSpurs] ShutdownWorkload(wid=%u)\n", wid);
    return CELL_OK;
}

s32 cellSpursWaitForWorkloadShutdown(u64 spurs_ea, u32 wid)
{
    static int _n = 0;
    if (_n++ < 8) printf("[cellSpurs] WaitForWorkloadShutdown(wid=%u)\n", wid);
    return CELL_OK;
}
