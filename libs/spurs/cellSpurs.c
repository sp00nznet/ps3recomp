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
#include <stdio.h>
#include <string.h>

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

static inline void ef_wait(EventFlagSync* s)
{
#ifdef _WIN32
    SleepConditionVariableCS(&s->cv, &s->cs, INFINITE);
#else
    pthread_cond_wait(&s->cond, &s->mtx);
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

    printf("[cellSpurs] InitializeWithAttribute(nSpus=%u, prefix=\"%.15s\")\n",
           attr->nSpus, attr->prefix);

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
    (void)spuPriority; (void)ppuPriority; (void)exitIfNoWork;

    if (!attr)
        return CELL_SPURS_CORE_ERROR_NULL_POINTER;

    memset(attr, 0, sizeof(CellSpursAttribute));
    attr->nSpus = (nSpus > 0 && nSpus <= CELL_SPURS_MAX_SPU)
                  ? (u32)nSpus : 1;

    for (int i = 0; i < CELL_SPURS_MAX_SPU; i++)
        attr->spuPriority[i] = spuPriority;

    printf("[cellSpurs] AttributeInitialize(nSpus=%d)\n", nSpus);
    return CELL_OK;
}

s32 cellSpursAttributeSetNamePrefix(CellSpursAttribute* attr,
                                    const char* prefix, u32 size)
{
    if (!attr)
        return CELL_SPURS_CORE_ERROR_NULL_POINTER;

    if (prefix && size > 0) {
        u32 copyLen = size < sizeof(attr->prefix) ? size : sizeof(attr->prefix) - 1;
        memcpy(attr->prefix, prefix, copyLen);
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

    if (!spurs->initialized)
        return CELL_SPURS_CORE_ERROR_STAT;

    *nThreads = spurs->nSpus;
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
    if (wid >= CELL_SPURS_MAX_WORKLOAD) return CELL_SPURS_CORE_ERROR_INVAL;
    if (!s_workloads[wid].in_use) return CELL_SPURS_CORE_ERROR_SRCH;

    memcpy(s_workloads[wid].priority, priorities, CELL_SPURS_MAX_SPU);
    return CELL_OK;
}

s32 cellSpursAttachLv2EventQueue(CellSpurs* spurs, u32 queue, u8* port,
                                 s32 isDynamic)
{
    (void)queue; (void)isDynamic;

    if (!spurs || !port) return CELL_SPURS_CORE_ERROR_NULL_POINTER;

    *port = 0; /* give it port 0 */
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

    if (!spurs || !taskset)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    if (!spurs->initialized)
        return CELL_SPURS_CORE_ERROR_STAT;

    memset(taskset, 0, sizeof(CellSpursTaskset));
    taskset->initialized = 1;
    taskset->spurs = spurs;

    printf("[cellSpurs] CreateTaskset()\n");
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
    if (!taskset)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    printf("[cellSpurs] DestroyTaskset()\n");
    taskset->initialized = 0;
    return CELL_OK;
}

s32 cellSpursShutdownTaskset(CellSpursTaskset* taskset)
{
    if (!taskset)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    printf("[cellSpurs] ShutdownTaskset()\n");
    taskset->shutdownRequested = 1;
    return CELL_OK;
}

s32 cellSpursJoinTaskset(CellSpursTaskset* taskset)
{
    if (!taskset)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    printf("[cellSpurs] JoinTaskset()\n");
    /* In a full implementation, wait for all tasks to complete */
    return CELL_OK;
}

s32 cellSpursTasksetAttributeInitialize(CellSpursTasksetAttribute* attr)
{
    if (!attr) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
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

    if (!taskset)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    if (!taskset->initialized)
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

            if (taskId) *taskId = s_tasks[i].id;
            taskset->taskCount++;

            printf("[cellSpurs] CreateTask(id=%u, entry=%p) - task logged\n",
                   s_tasks[i].id, elf);
            return CELL_OK;
        }
    }

    return CELL_SPURS_TASK_ERROR_NOMEM;
}

s32 cellSpursJoinTask(CellSpursTaskset* taskset, CellSpursTaskId taskId,
                      s32* exitCode)
{
    (void)taskset;

    printf("[cellSpurs] JoinTask(id=%u)\n", taskId);

    /* Find the task and mark as completed */
    for (u32 i = 0; i < CELL_SPURS_MAX_TASK; i++) {
        if (s_tasks[i].in_use && s_tasks[i].id == taskId) {
            s_tasks[i].completed = 1;
            s_tasks[i].active = 0;
            if (exitCode)
                *exitCode = s_tasks[i].exitCode;
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

            if (priority)
                memcpy(s_workloads[i].priority, priority, CELL_SPURS_MAX_SPU);
            else
                memset(s_workloads[i].priority, 0, CELL_SPURS_MAX_SPU);

            *wid = i;
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

    memset(attr, 0, sizeof(CellSpursWorkloadAttribute));
    attr->revision = revision;
    attr->sdkVersion = sdkVersion;
    attr->pm = (u64)(uintptr_t)pm;
    attr->sizePm = sizePm;
    attr->data = data;
    attr->minContention = minContention;
    attr->maxContention = maxContention;

    if (priority)
        memcpy(attr->priority, priority, CELL_SPURS_MAX_SPU);

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
    if (!eventFlag) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    printf("[cellSpurs] EventFlagAttachLv2EventQueue()\n");
    return CELL_OK;
}

s32 cellSpursEventFlagSet(CellSpursEventFlag* eventFlag, u16 bits)
{
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

        ef_wait(sync);
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
    if (!eventFlag || !direction)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    *direction = eventFlag->direction;
    return CELL_OK;
}
