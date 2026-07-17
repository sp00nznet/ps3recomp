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

/* C-linkage setter so the LLE libsre lift (compiled as C) can wire the real
 * SPURS+taskset EAs it sees in cellSpursCreateTaskset -> the cri-task
 * SpursTasksetContext builder. (LLE bypasses the HLE cellSpurs CreateTaskset.) */
void ydkj_set_real_taskset_ea(uint32_t spurs_ea, uint32_t taskset_ea)
{
    g_ydkj_real_spurs_ea   = spurs_ea;
    g_ydkj_real_taskset_ea = taskset_ea;
    { static int n = 0; if (n++ < 3) {
        fprintf(stderr, "[cri] wired REAL taskset: spurs=0x%08X taskset=0x%08X (from LLE libsre)\n",
                spurs_ea, taskset_ea);
        { extern void ydkj_host_bt(const char*); ydkj_host_bt("CreateTaskset caller"); } } }
}

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
    const void* pm;
    u32         sizePm;
    u64         data;
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
 * We can't embed OS handles in CellSpursEventFlag (game code controls its
 * layout), so we keep a small side table that maps event flag pointers to
 * their host mutex + condition variable.
 * -----------------------------------------------------------------------*/
#define MAX_EVENT_FLAGS 64

typedef struct {
    CellSpursEventFlag* ef;
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

static EventFlagSync* ef_sync_find(CellSpursEventFlag* ef)
{
    for (int i = 0; i < MAX_EVENT_FLAGS; i++) {
        if (s_ef_sync[i].initialized && s_ef_sync[i].ef == ef)
            return &s_ef_sync[i];
    }
    return NULL;
}

static EventFlagSync* ef_sync_alloc(CellSpursEventFlag* ef)
{
    for (int i = 0; i < MAX_EVENT_FLAGS; i++) {
        if (!s_ef_sync[i].initialized) {
            s_ef_sync[i].ef = ef;
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
    sync->ef = NULL;
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

/* =========================================================================
 * SPURS core
 * =====================================================================*/

s32 cellSpursInitialize(CellSpurs* spurs, s32 nSpus, s32 spuPriority,
                        s32 ppuPriority, u8 exitIfNoWork)
{
    (void)spuPriority; (void)ppuPriority; (void)exitIfNoWork;

    if (!spurs)
        return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    spurs = GUEST_PTR(spurs, CellSpurs*);

    printf("[cellSpurs] Initialize(nSpus=%d)\n", nSpus);

    memset(spurs, 0, sizeof(CellSpurs));
    spurs->initialized = 1;
    spurs->nSpus = (nSpus > 0 && nSpus <= CELL_SPURS_MAX_SPU)
                   ? (u32)nSpus : 1;

    memset(s_workloads, 0, sizeof(s_workloads));
    return CELL_OK;
}

s32 cellSpursInitializeWithAttribute(CellSpurs* spurs,
                                     const CellSpursAttribute* attr)
{
    if (!spurs || !attr)
        return CELL_SPURS_CORE_ERROR_NULL_POINTER;

    spurs = GUEST_PTR(spurs, CellSpurs*);
    attr  = GUEST_PTR(attr, const CellSpursAttribute*);
    printf("[cellSpurs] InitializeWithAttribute(prefix=\"%.15s\")\n", attr->prefix);

    memset(spurs, 0, sizeof(CellSpurs));
    spurs->initialized = 1;
    spurs->nSpus = attr->nSpus;
    spurs->flags = attr->flags;
    memcpy(spurs->prefix, attr->prefix, sizeof(spurs->prefix));

    memset(s_workloads, 0, sizeof(s_workloads));
    return CELL_OK;
}

s32 cellSpursFinalize(CellSpurs* spurs)
{
    if (!spurs)
        return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    spurs = GUEST_PTR(spurs, CellSpurs*);

    if (!spurs->initialized)
        return CELL_SPURS_CORE_ERROR_STAT;

    printf("[cellSpurs] Finalize()\n");

    memset(s_workloads, 0, sizeof(s_workloads));
    spurs->initialized = 0;
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
    spurs = GUEST_PTR(spurs, const CellSpurs*);
    u32* nThreads_h = GUEST_PTR(nThreads, u32*);

    if (!spurs->initialized)
        return CELL_SPURS_CORE_ERROR_STAT;

    *nThreads_h = spurs->nSpus;
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
    spurs   = GUEST_PTR(spurs, CellSpurs*);
    taskset = GUEST_PTR(taskset, CellSpursTaskset*);

    if (!spurs || !taskset)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    if (!spurs->initialized)
        return CELL_SPURS_CORE_ERROR_STAT;

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

s32 cellSpursCreateTask(CellSpursTaskset* taskset, CellSpursTaskId* taskId,
                        void* elf, void* context, u32 sizeContext,
                        CellSpursTaskAttribute* attr)
{
    (void)context; (void)sizeContext; (void)attr;

    /* Capture guest EAs BEFORE host translation (the real BE taskset builder + the
     * SPU DMA use guest EAs). */
    uint32_t taskset_ea = (uint32_t)(uintptr_t)taskset;
    uint32_t elf_ea     = (uint32_t)(uintptr_t)elf;
    uint32_t context_ea = (uint32_t)(uintptr_t)context;

    /* taskId/taskset are guest EAs; translate before deref. elf/context stay
     * guest EAs (handled below — elf is translated for load, context kept EA). */
    taskset = GUEST_PTR(taskset, CellSpursTaskset*);
    CellSpursTaskId* taskId_h = GUEST_PTR(taskId, CellSpursTaskId*);

    if (!taskset)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    if (!g_ydkj_real_taskset_ea)   /* real-BE init flag (native ->initialized clobbered by BE layout) */
        return CELL_SPURS_TASK_ERROR_STAT;

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
                                   (uint64_t)context_ea, /*arg*/NULL, /*ls_pattern*/NULL);
            /* Bridge to the image-22 dispatch so build_context uses this taskset+task. */
            g_ydkj_real_taskset_ea = taskset_ea;
            g_ydkj_real_taskid     = i;

            printf("[cellSpurs] CreateTask(id=%u, entry=%p) - task logged\n",
                   s_tasks[i].id, elf);

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
    (void)sdkVersion; (void)lsPattern; (void)argument;
    if (!attr) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    attr = GUEST_PTR(attr, CellSpursTaskAttribute*);
    memset(attr, 0, sizeof(CellSpursTaskAttribute));
    attr->revision    = revision;
    attr->sizeContext = sizeContext;
    attr->eaContext   = eaContext;
    attr->eaElf       = eaElf;
    printf("[cellSpurs] _TaskAttributeInitialize(eaElf=0x%08X ctx=0x%08X szctx=%u)\n",
           (u32)eaElf, (u32)eaContext, sizeContext);
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
    /* taskset/taskId forwarded raw (callee translates); elf/context are guest EAs. */
    return cellSpursCreateTask(taskset, taskId,
                               (void*)(uintptr_t)(u32)attr_h->eaElf,
                               (void*)(uintptr_t)(u32)attr_h->eaContext,
                               attr_h->sizeContext, attr);
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
    spurs = GUEST_PTR(spurs, CellSpurs*);
    CellSpursWorkloadId* wid_h = GUEST_PTR(wid, CellSpursWorkloadId*);
    const u8* priority_h = GUEST_PTR(priority, const u8*);

    if (!spurs->initialized)
        return CELL_SPURS_CORE_ERROR_STAT;

    for (u32 i = 0; i < CELL_SPURS_MAX_WORKLOAD; i++) {
        if (!s_workloads[i].in_use) {
            s_workloads[i].in_use = 1;
            s_workloads[i].pm = pm;
            s_workloads[i].sizePm = sizePm;
            s_workloads[i].data = data;
            s_workloads[i].minContention = minContention;
            s_workloads[i].maxContention = maxContention;
            s_workloads[i].readyCount = 0;

            if (priority_h)
                memcpy(s_workloads[i].priority, priority_h, CELL_SPURS_MAX_SPU);
            else
                memset(s_workloads[i].priority, 0, CELL_SPURS_MAX_SPU);

            *wid_h = i;
            printf("[cellSpurs] AddWorkload(wid=%u, pm=%p, size=%u)\n",
                   i, pm, sizePm);
            return CELL_OK;
        }
    }

    return CELL_SPURS_CORE_ERROR_NOMEM;
}

s32 cellSpursAddWorkloadWithAttribute(CellSpurs* spurs,
                                       CellSpursWorkloadId* wid,
                                       const CellSpursWorkloadAttribute* attr)
{
    if (!attr) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    attr = GUEST_PTR(attr, const CellSpursWorkloadAttribute*);

    /* spurs/wid stay guest EAs (cellSpursAddWorkload translates them); attr->pm
     * and attr->priority are guest EAs carried through verbatim. */
    return cellSpursAddWorkload(spurs, wid, (const void*)(uintptr_t)attr->pm,
                               attr->sizePm, attr->data, attr->priority,
                               attr->minContention, attr->maxContention);
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

s32 cellSpursReadyCountStore(CellSpurs* spurs, CellSpursWorkloadId wid,
                             u32 value)
{
    if (!spurs) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (wid >= CELL_SPURS_MAX_WORKLOAD) return CELL_SPURS_CORE_ERROR_INVAL;
    if (!s_workloads[wid].in_use) return CELL_SPURS_CORE_ERROR_SRCH;

    s_workloads[wid].readyCount = value;
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
    taskset = GUEST_PTR(taskset, CellSpursTaskset*);
    eventFlag = GUEST_PTR(eventFlag, CellSpursEventFlag*);
    (void)taskset;

    if (!eventFlag)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    /* If this event flag was previously initialized, free old sync slot */
    EventFlagSync* old = ef_sync_find(eventFlag);
    if (old) ef_sync_free(old);

    memset(eventFlag, 0, sizeof(CellSpursEventFlag));
    eventFlag->initialized = 1;
    eventFlag->clearMode = (u16)clearMode;
    eventFlag->direction = (u16)direction;
    eventFlag->bits = 0;

    EventFlagSync* sync = ef_sync_alloc(eventFlag);
    if (!sync) {
        printf("[cellSpurs] EventFlagInitialize: no free sync slots!\n");
        return CELL_SPURS_TASK_ERROR_NOMEM;
    }

    printf("[cellSpurs] EventFlagInitialize(clearMode=%u, direction=%u)\n",
           clearMode, direction);
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
    eventFlag = GUEST_PTR(eventFlag, CellSpursEventFlag*);
    if (!eventFlag)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    if (!eventFlag->initialized)
        return CELL_SPURS_TASK_ERROR_STAT;

    EventFlagSync* sync = ef_sync_find(eventFlag);
    if (!sync)
        return CELL_SPURS_TASK_ERROR_STAT;

    ef_lock(sync);
    eventFlag->bits |= bits;
    ef_broadcast(sync);
    ef_unlock(sync);

    return CELL_OK;
}

s32 cellSpursEventFlagWait(CellSpursEventFlag* eventFlag, u16* bits,
                           u32 mode)
{
    eventFlag = GUEST_PTR(eventFlag, CellSpursEventFlag*);
    bits = GUEST_PTR(bits, u16*);
    if (!eventFlag || !bits)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    if (!eventFlag->initialized)
        return CELL_SPURS_TASK_ERROR_STAT;

    EventFlagSync* sync = ef_sync_find(eventFlag);
    if (!sync)
        return CELL_SPURS_TASK_ERROR_STAT;

    u16 pattern = *bits;

    ef_lock(sync);

    /* Block until the requested bit pattern is satisfied */
    /* SPU-completion shim: the bits this wait expects are normally set by an SPU
     * workload via cellSpursEventFlagSet. We don't execute SPU code, so after a
     * short grace period with no signal we force-satisfy the pattern (as if the
     * SPU finished) so the title's boot proceeds. Correct fix is SPU execution. */
    unsigned timeouts = 0;
    for (;;) {
        u16 current = eventFlag->bits;

        if (mode == CELL_SPURS_EVENT_FLAG_AND) {
            if ((current & pattern) == pattern)
                break;
        } else {
            /* OR mode: any requested bit set */
            if ((current & pattern) != 0)
                break;
        }

        if (!ef_wait_timed(sync, 50) && ++timeouts >= 4) {
            fprintf(stderr, "[cellSpurs] EventFlagWait: no SPU signal for pattern "
                            "0x%04X -- force-satisfying (SPU not executed)\n", pattern);
            eventFlag->bits |= pattern;
        }
    }

    *bits = eventFlag->bits;

    /* Auto-clear if configured */
    if (eventFlag->clearMode == CELL_SPURS_EVENT_FLAG_CLEAR_AUTO)
        eventFlag->bits &= ~pattern;

    ef_unlock(sync);

    return CELL_OK;
}

s32 cellSpursEventFlagTryWait(CellSpursEventFlag* eventFlag, u16* bits,
                              u32 mode)
{
    eventFlag = GUEST_PTR(eventFlag, CellSpursEventFlag*);
    bits = GUEST_PTR(bits, u16*);
    if (!eventFlag || !bits)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    if (!eventFlag->initialized)
        return CELL_SPURS_TASK_ERROR_STAT;

    EventFlagSync* sync = ef_sync_find(eventFlag);
    if (!sync)
        return CELL_SPURS_TASK_ERROR_STAT;

    u16 pattern = *bits;

    ef_lock(sync);

    u16 current = eventFlag->bits;

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

    *bits = current;

    if (eventFlag->clearMode == CELL_SPURS_EVENT_FLAG_CLEAR_AUTO)
        eventFlag->bits &= ~pattern;

    ef_unlock(sync);
    return CELL_OK;
}

s32 cellSpursEventFlagClear(CellSpursEventFlag* eventFlag, u16 bits)
{
    eventFlag = GUEST_PTR(eventFlag, CellSpursEventFlag*);
    if (!eventFlag)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    if (!eventFlag->initialized)
        return CELL_SPURS_TASK_ERROR_STAT;

    EventFlagSync* sync = ef_sync_find(eventFlag);
    if (!sync)
        return CELL_SPURS_TASK_ERROR_STAT;

    ef_lock(sync);
    eventFlag->bits &= ~bits;
    ef_unlock(sync);

    return CELL_OK;
}

s32 cellSpursEventFlagGetDirection(CellSpursEventFlag* eventFlag,
                                   u32* direction)
{
    eventFlag = GUEST_PTR(eventFlag, CellSpursEventFlag*);
    direction = GUEST_PTR(direction, u32*);
    if (!eventFlag || !direction)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    *direction = eventFlag->direction;
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

/* cellSpursRunJobChain — start a job chain execution */
s32 cellSpursRunJobChain(void* spurs, void* jobChain)
{
    (void)spurs; (void)jobChain;
    printf("[cellSpurs] RunJobChain() — stub (no SPU execution)\n");
    /* Job chains run on SPUs. Without SPU execution, we stub this.
     * Games that depend on job chain completion will need the jobs
     * to be HLE'd or run on host threads. */
    return CELL_OK;
}

/* cellSpursKickJobChain — kick a running job chain */
s32 cellSpursKickJobChain(void* spurs, void* jobChain)
{
    (void)spurs; (void)jobChain;
    return CELL_OK;
}
