/*
 * ps3recomp - cellSpursJq HLE implementation
 *
 * SPURS Job Queue — manages job submission and tracking.
 * Jobs are accepted and immediately marked as complete (no SPU execution).
 * This satisfies games that check for job completion but don't depend
 * on SPU side effects.
 */

#include "cellSpursJq.h"
#include <stdio.h>
#include <string.h>

/* Internal state */

typedef struct {
    int in_use;
    u32 maxJobs;
    u32 jobCount;
    u32 nextJobId;
} JqState;

static JqState s_queues[CELL_SPURS_JQ_MAX_QUEUES];

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
    if (!attr) return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;
    attr->maxGrab = maxGrab;
    return CELL_OK;
}

s32 cellSpursCreateJobQueue(void* spurs, CellSpursJobQueue* jq,
                            const CellSpursJobQueueAttribute* attr,
                            void* buffer, u32 bufferSize)
{
    (void)spurs; (void)buffer; (void)bufferSize;
    printf("[cellSpursJq] CreateJobQueue()\n");

    if (!jq || !attr) return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    int slot = jq_alloc();
    if (slot < 0) return (s32)CELL_SPURS_JQ_ERROR_OUT_OF_MEMORY;

    s_queues[slot].in_use = 1;
    s_queues[slot].maxJobs = attr->maxJobs;
    s_queues[slot].jobCount = 0;
    s_queues[slot].nextJobId = 1;

    *jq = (u32)slot;
    return CELL_OK;
}

s32 cellSpursDestroyJobQueue(CellSpursJobQueue* jq)
{
    if (!jq) return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    u32 slot = *jq;
    if (slot >= CELL_SPURS_JQ_MAX_QUEUES || !s_queues[slot].in_use)
        return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    printf("[cellSpursJq] DestroyJobQueue(%u)\n", slot);
    s_queues[slot].in_use = 0;
    return CELL_OK;
}

s32 cellSpursJobQueuePush(CellSpursJobQueue* jq, const void* job,
                          u32 jobSize, u32 tag, CellSpursJobId* id)
{
    (void)job; (void)jobSize; (void)tag;

    if (!jq) return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    u32 slot = *jq;
    if (slot >= CELL_SPURS_JQ_MAX_QUEUES || !s_queues[slot].in_use)
        return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    JqState* q = &s_queues[slot];

    if (q->jobCount >= q->maxJobs)
        return (s32)CELL_SPURS_JQ_ERROR_QUEUE_FULL;

    u32 jobId = q->nextJobId++;
    q->jobCount++;

    if (id) *id = jobId;

    /* Job is immediately "complete" since we don't execute SPU code */
    return CELL_OK;
}

s32 cellSpursJobQueuePushJob(CellSpursJobQueue* jq, const CellSpursJob256* job,
                             u32 tag, CellSpursJobId* id)
{
    return cellSpursJobQueuePush(jq, job, sizeof(CellSpursJob256), tag, id);
}

s32 cellSpursJobQueuePort2Create(CellSpursJobQueue* jq, CellSpursJobQueuePort* port)
{
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
    if (!port) return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;
    CellSpursJobQueue jq = *port;
    return cellSpursJobQueuePush(&jq, job, jobSize, tag, id);
}

s32 cellSpursJobQueueSendSignal(CellSpursJobQueue* jq, CellSpursJobId id)
{
    (void)jq; (void)id;
    /* No-op: jobs are already complete */
    return CELL_OK;
}

s32 cellSpursJobQueueWait(CellSpursJobQueue* jq, CellSpursJobId id)
{
    (void)jq; (void)id;
    /* Jobs complete immediately, nothing to wait for */
    return CELL_OK;
}

s32 cellSpursJobQueueTryWait(CellSpursJobQueue* jq, CellSpursJobId id)
{
    (void)jq; (void)id;
    return CELL_OK; /* Already done */
}

s32 cellSpursJobQueueGetCount(CellSpursJobQueue* jq, u32* count)
{
    if (!jq || !count) return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    u32 slot = *jq;
    if (slot >= CELL_SPURS_JQ_MAX_QUEUES || !s_queues[slot].in_use)
        return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;

    *count = 0; /* All jobs complete instantly */
    return CELL_OK;
}

s32 cellSpursJobQueueSemaphoreTryAcquire(CellSpursJobQueue* jq)
{
    (void)jq;
    return CELL_OK;
}
