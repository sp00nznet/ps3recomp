/*
 * ps3recomp - cellSpursJq HLE implementation
 *
 * SPURS Job Queue — manages job submission and tracking.
 * Jobs are accepted and immediately marked as complete (no SPU execution).
 * This satisfies games that check for job completion but don't depend
 * on SPU side effects.
 */

#include "cellSpursJq.h"
#include "ps3emu/strict_boot.h"
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_write*: guest EA -> host pointer */
#include <stdio.h>
#include <string.h>

/* Internal state */

typedef struct {
    int in_use;
    u32 maxJobs;
    u32 jobCount;       /* number of pending (not-yet-completed) jobs */
    u32 nextJobId;
    u32 submitCount;    /* total jobs submitted over lifetime */
    u32 completeCount;  /* total jobs completed over lifetime */
    /*
     * Per-job completion ring: tracks the last CELL_SPURS_JQ_MAX_JOBS
     * job IDs that have completed.  Wait/TryWait check this.
     */
    u32 completedIds[CELL_SPURS_JQ_MAX_JOBS];
    u32 completedHead;  /* next write position in the ring */
} JqState;

static JqState s_queues[CELL_SPURS_JQ_MAX_QUEUES];

/* Check whether a job ID has completed in the given queue */
static int jq_is_completed(const JqState* q, CellSpursJobId id)
{
    /* Job ID 0 is invalid / never assigned */
    if (id == 0) return 0;

    for (int i = 0; i < CELL_SPURS_JQ_MAX_JOBS; i++) {
        if (q->completedIds[i] == id)
            return 1;
    }
    return 0;
}

/* Record a job as completed */
static void jq_mark_completed(JqState* q, CellSpursJobId id)
{
    q->completedIds[q->completedHead] = id;
    q->completedHead = (q->completedHead + 1) % CELL_SPURS_JQ_MAX_JOBS;
    q->completeCount++;
    if (q->jobCount > 0)
        q->jobCount--;
}

static int jq_alloc(void)
{
    for (int i = 0; i < CELL_SPURS_JQ_MAX_QUEUES; i++) {
        if (!s_queues[i].in_use) return i;
    }
    return -1;
}

/* API */

s32 cellSpursJobQueueAttributeInitialize(CellSpursJobQueueAttribute* attr)
{
    attr = GUEST_PTR(attr, CellSpursJobQueueAttribute*);
    if (!attr) return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    memset(attr, 0, sizeof(*attr));
    attr->maxJobs = 64;
    attr->maxGrab = 1;
    /* Default priority: normal on all SPUs */
    for (int i = 0; i < 8; i++)
        attr->priorities[i] = CELL_SPURS_JQ_PRIORITY_NORMAL;

    return CELL_OK;
}

s32 cellSpursJobQueueAttributeSetMaxGrab(CellSpursJobQueueAttribute* attr, u32 maxGrab)
{
    attr = GUEST_PTR(attr, CellSpursJobQueueAttribute*);
    if (!attr) return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;
    attr->maxGrab = maxGrab;
    return CELL_OK;
}

s32 cellSpursCreateJobQueue(void* spurs, CellSpursJobQueue* jq,
                            const CellSpursJobQueueAttribute* attr,
                            void* buffer, u32 bufferSize)
{
    attr = GUEST_PTR(attr, const CellSpursJobQueueAttribute*);
    jq = GUEST_PTR(jq, CellSpursJobQueue*);
    (void)spurs; (void)buffer; (void)bufferSize;
    printf("[cellSpursJq] CreateJobQueue()\n");

    if (!jq || !attr) return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    int slot = jq_alloc();
    if (slot < 0) return (s32)CELL_SPURS_JQ_ERROR_OUT_OF_MEMORY;

    memset(&s_queues[slot], 0, sizeof(JqState));
    s_queues[slot].in_use = 1;
    s_queues[slot].maxJobs = attr->maxJobs;
    s_queues[slot].jobCount = 0;
    s_queues[slot].nextJobId = 1;
    s_queues[slot].submitCount = 0;
    s_queues[slot].completeCount = 0;
    s_queues[slot].completedHead = 0;

    *jq = (u32)slot;
    return CELL_OK;
}

s32 cellSpursDestroyJobQueue(CellSpursJobQueue* jq)
{
    jq = GUEST_PTR(jq, CellSpursJobQueue*);
    if (!jq) return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    u32 slot = *jq;
    if (slot >= CELL_SPURS_JQ_MAX_QUEUES || !s_queues[slot].in_use)
        return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    printf("[cellSpursJq] DestroyJobQueue(%u)\n", slot);
    s_queues[slot].in_use = 0;
    return CELL_OK;
}

/* Host-pointer core. cellSpursJobQueuePort2PushSync reads the queue id out of
 * the port into a LOCAL and pushes with its address, so it cannot go through
 * the entry point -- that translates its argument as a guest EA and would
 * resolve a host stack address to a nonsense queue slot. Same shape as
 * cellHttpUtil's escape core. */
static s32 jq_push_host(const u32* jq, const void* job,
                        u32 jobSize, u32 tag, u32* id_ea)
{
    (void)job; (void)jobSize; (void)tag;

    if (!jq) return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    u32 slot = *jq;
    if (slot >= CELL_SPURS_JQ_MAX_QUEUES || !s_queues[slot].in_use)
        return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    JqState* q = &s_queues[slot];

    if (q->jobCount >= q->maxJobs)
        return (s32)CELL_SPURS_JQ_ERROR_QUEUE_FULL;

    ps3_strict_boot_guard("spurs-jobqueue-unimplemented-execution", "queue=%u job=0x%llX size=%u tag=%u", slot, (unsigned long long)(uintptr_t)job, jobSize, tag);
    u32 jobId = q->nextJobId++;
    q->jobCount++;
    q->submitCount++;

    if (id_ea) vm_write32((u32)(uintptr_t)id_ea, jobId);

    /*
     * Job is immediately "complete" since we don't execute SPU code,
     * but we now record completion so Wait/TryWait can track it.
     */
    jq_mark_completed(q, jobId);

    return CELL_OK;
}

s32 cellSpursJobQueuePush(CellSpursJobQueue* jq, const void* job,
                          u32 jobSize, u32 tag, CellSpursJobId* id)
{
    return jq_push_host(GUEST_PTR(jq, const u32*), job, jobSize, tag, id);
}

s32 cellSpursJobQueuePushJob(CellSpursJobQueue* jq, const CellSpursJob256* job,
                             u32 tag, CellSpursJobId* id)
{
    return cellSpursJobQueuePush(jq, job, sizeof(CellSpursJob256), tag, id);
}

s32 cellSpursJobQueuePort2Create(CellSpursJobQueue* jq, CellSpursJobQueuePort* port)
{
    port = GUEST_PTR(port, CellSpursJobQueuePort*);
    jq = GUEST_PTR(jq, CellSpursJobQueue*);
    if (!jq || !port) return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;
    *port = *jq; /* Port is just a reference to the queue */
    return CELL_OK;
}

s32 cellSpursJobQueuePort2Destroy(CellSpursJobQueuePort* port)
{
    (void)port;
    return CELL_OK;
}

s32 cellSpursJobQueuePort2PushSync(CellSpursJobQueuePort* port,
                                   const void* job, u32 jobSize,
                                   u32 tag, CellSpursJobId* id)
{
    port = GUEST_PTR(port, CellSpursJobQueuePort*);
    if (!port) return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;
    CellSpursJobQueue jq = *port;
    return jq_push_host((const u32*)&jq, job, jobSize, tag, id);
}

s32 cellSpursJobQueueSendSignal(CellSpursJobQueue* jq, CellSpursJobId id)
{
    (void)jq; (void)id;
    /* No-op: jobs are already complete */
    return CELL_OK;
}

s32 cellSpursJobQueueWait(CellSpursJobQueue* jq, CellSpursJobId id)
{
    jq = GUEST_PTR(jq, CellSpursJobQueue*);
    if (!jq) return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    u32 slot = *jq;
    if (slot >= CELL_SPURS_JQ_MAX_QUEUES || !s_queues[slot].in_use)
        return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    const JqState* q = &s_queues[slot];

    /*
     * If no jobs have ever been submitted, treat as an error —
     * nothing can complete if nothing was pushed.
     */
    if (q->submitCount == 0)
        return (s32)CELL_SPURS_JQ_ERROR_JOB_NOT_FOUND;

    if (!jq_is_completed(q, id))
        return (s32)CELL_SPURS_JQ_ERROR_JOB_NOT_FOUND;

    return CELL_OK;
}

s32 cellSpursJobQueueTryWait(CellSpursJobQueue* jq, CellSpursJobId id)
{
    jq = GUEST_PTR(jq, CellSpursJobQueue*);
    if (!jq) return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    u32 slot = *jq;
    if (slot >= CELL_SPURS_JQ_MAX_QUEUES || !s_queues[slot].in_use)
        return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    const JqState* q = &s_queues[slot];

    /* No jobs submitted yet — return BUSY */
    if (q->submitCount == 0)
        return (s32)CELL_SPURS_JQ_ERROR_BUSY;

    if (!jq_is_completed(q, id))
        return (s32)CELL_SPURS_JQ_ERROR_BUSY;

    return CELL_OK;
}

s32 cellSpursJobQueueGetCount(CellSpursJobQueue* jq, u32* count)
{
    jq = GUEST_PTR(jq, CellSpursJobQueue*);
    if (!jq || !count) return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    u32 slot = *jq;
    if (slot >= CELL_SPURS_JQ_MAX_QUEUES || !s_queues[slot].in_use)
        return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    vm_write32((u32)(uintptr_t)count, s_queues[slot].jobCount);
    return CELL_OK;
}

s32 cellSpursJobQueueSemaphoreTryAcquire(CellSpursJobQueue* jq)
{
    (void)jq;
    return CELL_OK;
}
