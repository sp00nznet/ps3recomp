/*
 * ps3recomp - cellSpursJq HLE implementation
 *
 * SPURS Job Queue — manages job submission and tracking.
 * High-level APIs accept jobs and immediately mark them complete;
 * the _cellSpursJobQueuePort2PushJobBody* entry points below execute
 * real SPU job binaries synchronously via spu_workload / spu_interp.
 */

#include "cellSpursJq.h"
#include "ps3emu/endian.h"
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_read*, vm_write* */
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
    if (!attr) return (s32)CELL_SPURS_JQ_ERROR_NULL_POINTER;

    memset(attr, 0, sizeof(*attr));
    attr->maxJobs = ps3_bswap32(64);
    attr->maxGrab = ps3_bswap32(1);
    /* Default priority: normal on all SPUs */
    for (int i = 0; i < 8; i++)
        attr->priorities[i] = CELL_SPURS_JQ_PRIORITY_NORMAL;

    return CELL_OK;
}

s32 cellSpursJobQueueAttributeSetMaxGrab(CellSpursJobQueueAttribute* attr, u32 maxGrab)
{
    attr = GUEST_PTR(attr, CellSpursJobQueueAttribute*);
    if (!attr) return (s32)CELL_SPURS_JQ_ERROR_NULL_POINTER;
    if (maxGrab == 0) return (s32)CELL_SPURS_JQ_ERROR_INVAL;
    attr->maxGrab = ps3_bswap32(maxGrab);
    return CELL_OK;
}

s32 cellSpursCreateJobQueue(void* spurs, CellSpursJobQueue* jq,
                            const CellSpursJobQueueAttribute* attr,
                            void* buffer, u32 bufferSize)
{
    u32 jq_ea = (u32)(uintptr_t)jq;
    attr = GUEST_PTR(attr, const CellSpursJobQueueAttribute*);
    (void)spurs; (void)buffer; (void)bufferSize;
    printf("[cellSpursJq] CreateJobQueue()\n");

    if (!jq_ea || !attr) return (s32)CELL_SPURS_JQ_ERROR_NULL_POINTER;

    int slot = jq_alloc();
    if (slot < 0) return (s32)CELL_SPURS_JQ_ERROR_OUT_OF_MEMORY;

    u32 maxJobs = ps3_bswap32(attr->maxJobs);
    if (maxJobs == 0) maxJobs = 64;

    memset(&s_queues[slot], 0, sizeof(JqState));
    s_queues[slot].in_use = 1;
    s_queues[slot].maxJobs = maxJobs;
    s_queues[slot].jobCount = 0;
    s_queues[slot].nextJobId = 1;
    s_queues[slot].submitCount = 0;
    s_queues[slot].completeCount = 0;
    s_queues[slot].completedHead = 0;

    vm_write32(jq_ea, (u32)slot);
    return CELL_OK;
}

s32 cellSpursDestroyJobQueue(CellSpursJobQueue* jq)
{
    u32 jq_ea = (u32)(uintptr_t)jq;
    if (!jq_ea) return (s32)CELL_SPURS_JQ_ERROR_NULL_POINTER;

    u32 slot = vm_read32(jq_ea);
    if (slot >= CELL_SPURS_JQ_MAX_QUEUES || !s_queues[slot].in_use)
        return (s32)CELL_SPURS_JQ_ERROR_INVAL;

    printf("[cellSpursJq] DestroyJobQueue(%u)\n", slot);
    s_queues[slot].in_use = 0;
    return CELL_OK;
}

static s32 jq_push_slot(u32 slot, const void* job,
                        u32 jobSize, u32 tag, CellSpursJobId* id_ea)
{
    (void)job; (void)jobSize; (void)tag;

    if (slot >= CELL_SPURS_JQ_MAX_QUEUES || !s_queues[slot].in_use)
        return (s32)CELL_SPURS_JQ_ERROR_INVAL;

    JqState* q = &s_queues[slot];

    if (q->jobCount >= q->maxJobs)
        return (s32)CELL_SPURS_JQ_ERROR_QUEUE_FULL;

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
    u32 jq_ea = (u32)(uintptr_t)jq;
    if (!jq_ea) return (s32)CELL_SPURS_JQ_ERROR_NULL_POINTER;
    return jq_push_slot(vm_read32(jq_ea), job, jobSize, tag, id);
}

s32 cellSpursJobQueuePushJob(CellSpursJobQueue* jq, const CellSpursJob256* job,
                             u32 tag, CellSpursJobId* id)
{
    return cellSpursJobQueuePush(jq, job, sizeof(CellSpursJob256), tag, id);
}

/* SDK order is (pPort, pJobQueue) -- job_queue_port2.h; a title passes the
 * freshly allocated 128-byte port in r3 and the queue in r4. The
 * old (jq, port) order copied the port's uninitialised first word over the
 * queue's handle. */
s32 cellSpursJobQueuePort2Create(CellSpursJobQueuePort* port, CellSpursJobQueue* jq)
{
    u32 port_ea = (u32)(uintptr_t)port;
    u32 jq_ea = (u32)(uintptr_t)jq;
    if (!jq_ea || !port_ea) return (s32)CELL_SPURS_JQ_ERROR_NULL_POINTER;
    vm_write32(port_ea, vm_read32(jq_ea)); /* Port is just a reference to the queue */
    return CELL_OK;
}

s32 cellSpursJobQueuePort2Destroy(CellSpursJobQueuePort* port)
{
    if (!port) return (s32)CELL_SPURS_JQ_ERROR_NULL_POINTER;
    return CELL_OK;
}

s32 cellSpursJobQueuePort2PushSync(CellSpursJobQueuePort* port,
                                   const void* job, u32 jobSize,
                                   u32 tag, CellSpursJobId* id)
{
    u32 port_ea = (u32)(uintptr_t)port;
    if (!port_ea) return (s32)CELL_SPURS_JQ_ERROR_NULL_POINTER;
    return jq_push_slot(vm_read32(port_ea), job, jobSize, tag, id);
}

s32 cellSpursJobQueueSendSignal(CellSpursJobQueue* jq, CellSpursJobId id)
{
    (void)jq; (void)id;
    /* No-op: jobs are already complete */
    return CELL_OK;
}

s32 cellSpursJobQueueWait(CellSpursJobQueue* jq, CellSpursJobId id)
{
    u32 jq_ea = (u32)(uintptr_t)jq;
    if (!jq_ea) return (s32)CELL_SPURS_JQ_ERROR_NULL_POINTER;

    u32 slot = vm_read32(jq_ea);
    if (slot >= CELL_SPURS_JQ_MAX_QUEUES || !s_queues[slot].in_use)
        return (s32)CELL_SPURS_JQ_ERROR_INVAL;

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
    u32 jq_ea = (u32)(uintptr_t)jq;
    if (!jq_ea) return (s32)CELL_SPURS_JQ_ERROR_NULL_POINTER;

    u32 slot = vm_read32(jq_ea);
    if (slot >= CELL_SPURS_JQ_MAX_QUEUES || !s_queues[slot].in_use)
        return (s32)CELL_SPURS_JQ_ERROR_INVAL;

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
    u32 jq_ea = (u32)(uintptr_t)jq;
    if (!jq_ea || !count) return (s32)CELL_SPURS_JQ_ERROR_NULL_POINTER;

    u32 slot = vm_read32(jq_ea);
    if (slot >= CELL_SPURS_JQ_MAX_QUEUES || !s_queues[slot].in_use)
        return (s32)CELL_SPURS_JQ_ERROR_INVAL;

    vm_write32((u32)(uintptr_t)count, s_queues[slot].jobCount);
    return CELL_OK;
}

s32 cellSpursJobQueueSemaphoreTryAcquire(CellSpursJobQueue* jq)
{
    (void)jq;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Jobs that run.
 *
 * Everything above marks a job complete without running it, which is enough
 * for a title that only checks completion. A title's loader can push real
 * work through a port (_cellSpursJobQueuePort2PushJobBody with
 * isSync=1, then cellSpursJobQueuePort2Sync) and needs its results, so these
 * entry points execute the job.
 *
 * A job-queue job is the same thing as a jobchain job: a CellSpursJob128/256
 * whose first 48 bytes are a CellSpursJobHeader (eaBinary, sizeBinary,
 * input DMA list, ...), entered as cellSpursJobMain2(CellSpursJobContext2*,
 * CellSpursJob256*). spu_run_spurs_job (runtime/spu/spurs_job.c) already
 * stages that the way jm2 does, so the job runs SYNCHRONOUSLY on the pushing
 * PPU thread: through its lifted image when one is registered, otherwise in
 * the SPU interpreter. Port2Sync then has nothing left to wait for.
 *
 * The queue handle is the first word of the guest CellSpursJobQueue (a slot
 * in s_queues) and the port's first word copies it -- the guest structures
 * are opaque to the title.
 * -------------------------------------------------------------------------*/

#include "spu_workload.h"   /* spu_workload_dispatch_job, spu_run_spurs_job */
#include "spu_interp.h"     /* spu_interp_run_until */
#include <stdlib.h>

/* Image id for interpreted jobs: no lifted code is registered under it, so the
 * interpreter never "rejoins" another image's function at the same LSA. */
#define JQ_INTERP_IMAGE_ID 0x7A00
/* Where spurs_job.c points a job's link register: the job is finished. */
#define JQ_JOB_RETURN_LS   0x3FF00u

static void jq_interp_entry(spu_context* ctx)
{
    spu_interp_run_until(ctx, 0, JQ_JOB_RETURN_LS);
}

static int jq_trace(void)
{
    static int t = -1;
    if (t < 0) { const char* e = getenv("SPURS_JQ_TRACE"); t = e ? atoi(e) : 0; }
    return t;
}

static s32 jq_slot_of_port(u32 port_ea, JqState** out)
{
    if (!port_ea || (port_ea & 0x7F))
        return (s32)(port_ea ? 0x80410A10u /* ALIGN */ : 0x80410A11u /* NULL_POINTER */);
    u32 slot = vm_read32(port_ea);
    if (slot >= CELL_SPURS_JQ_MAX_QUEUES || !s_queues[slot].in_use)
        return (s32)CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT;
    *out = &s_queues[slot];
    return CELL_OK;
}

static s32 jq_run_job(u32 port_ea, u32 job_ea, u32 size_desc, const char* who)
{
    JqState* q = NULL;
    s32 rc = jq_slot_of_port(port_ea, &q);
    if (rc != CELL_OK) return rc;
    if (!job_ea) return (s32)0x80410A11u;
    if (job_ea & 0xF) return (s32)0x80410A10u;

    u32 ea_bin   = (u32)(vm_read64(job_ea) & ~1ull);
    u32 size_bin = (u32)vm_read16(job_ea + 8) << 4;

    static unsigned long long n_jobs = 0, n_interp = 0;
    unsigned long long n = __atomic_add_fetch(&n_jobs, 1, __ATOMIC_RELAXED);
    int lifted = 0;
    if (ea_bin && size_bin)
        lifted = spu_workload_dispatch_job(vm_base + ea_bin, size_bin, job_ea, size_desc);
    if (!lifted && ea_bin && size_bin) {
        __atomic_add_fetch(&n_interp, 1, __ATOMIC_RELAXED);
        spu_run_spurs_job(jq_interp_entry, JQ_INTERP_IMAGE_ID, job_ea, size_desc);
    }
    if (jq_trace() || n <= 4 || (n % 1000) == 0)
        fprintf(stderr, "[cellSpursJq] %s #%llu job=0x%08X bin=0x%08X+%u desc=%u %s (%llu interpreted)\n",
                who, n, job_ea, ea_bin, size_bin, size_desc,
                lifted ? "lifted" : "interp", n_interp);

    __atomic_add_fetch(&q->submitCount, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&q->completeCount, 1, __ATOMIC_RELAXED);
    return CELL_OK;
}

static s32 jq_create(u32 jq_ea, u32 attr_ea, const char* name)
{
    if (!jq_ea) return (s32)0x80410A11u;
    int slot = jq_alloc();
    if (slot < 0) return (s32)CELL_SPURS_JQ_ERROR_OUT_OF_MEMORY;
    memset(&s_queues[slot], 0, sizeof(JqState));
    s_queues[slot].in_use = 1;
    s_queues[slot].maxJobs = 0xFFFFFFFFu;
    s_queues[slot].nextJobId = 1;
    vm_write32(jq_ea, (u32)slot);
    fprintf(stderr, "[cellSpursJq] CreateJobQueue jq=0x%08X attr=0x%08X name=\"%s\" -> slot %d\n",
            jq_ea, attr_ea, name ? name : "", slot);
    return CELL_OK;
}

s32 _cellSpursCreateJobQueueWithJobDescriptorPool(u32 jqRevision, u32 sdkRevision,
                                                  u32 spurs, u32 jq, u32 attr,
                                                  u32 pool, u32 descPool, u32 name)
{
    (void)jqRevision; (void)sdkRevision; (void)spurs; (void)pool; (void)descPool;
    return jq_create(jq, attr, name ? (const char*)(vm_base + name) : NULL);
}

s32 _cellSpursCreateJobQueue(u32 jqRevision, u32 sdkRevision, u32 spurs, u32 jq,
                             u32 attr, u32 pool, u32 name)
{
    (void)jqRevision; (void)sdkRevision; (void)spurs; (void)pool;
    return jq_create(jq, attr, name ? (const char*)(vm_base + name) : NULL);
}

s32 cellSpursShutdownJobQueue(u32 jq)
{
    if (!jq) return (s32)0x80410A11u;
    fprintf(stderr, "[cellSpursJq] ShutdownJobQueue(0x%08X)\n", jq);
    return CELL_OK;
}

s32 cellSpursJoinJobQueue(u32 jq, u32 exitCode)
{
    if (!jq) return (s32)0x80410A11u;
    u32 slot = vm_read32(jq);
    if (slot < CELL_SPURS_JQ_MAX_QUEUES) s_queues[slot].in_use = 0;
    if (exitCode) vm_write32(exitCode, CELL_OK);
    fprintf(stderr, "[cellSpursJq] JoinJobQueue(0x%08X)\n", jq);
    return CELL_OK;
}

s32 _cellSpursJobQueuePort2PushJobBody(u32 port, u32 job, u32 sizeJobDesc,
                                       u32 tag, u32 isSync)
{
    (void)tag; (void)isSync;
    return jq_run_job(port, job, sizeJobDesc, "Port2PushJob");
}

s32 _cellSpursJobQueuePort2CopyPushJobBody(u32 port, u32 job, u32 sizeJobDesc,
                                           u32 sizeCopy, u32 tag, u32 isSync)
{
    (void)sizeCopy; (void)tag; (void)isSync;
    return jq_run_job(port, job, sizeJobDesc, "Port2CopyPushJob");
}

s32 cellSpursJobQueuePort2Sync(u32 port, u32 flag)
{
    (void)flag;
    JqState* q = NULL;
    return jq_slot_of_port(port, &q);   /* every pushed job already ran */
}
