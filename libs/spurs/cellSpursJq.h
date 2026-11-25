/*
 * ps3recomp - cellSpursJq HLE
 *
 * SPURS Job Queue — advanced SPU job dispatch built on cellSpurs.
 * Provides job queue creation, enqueue, dequeue, and synchronization.
 * Stub — management APIs work, no actual SPU execution.
 */

#ifndef PS3RECOMP_CELL_SPURS_JQ_H
#define PS3RECOMP_CELL_SPURS_JQ_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes (PS3 libspurs_jq, base 0x80410A00 | SPURS_ERRNO; verified against RPCS3) */
#define CELL_SPURS_JQ_ERROR_AGAIN               (CELL_ERROR_BASE_SPURS_JQ | 0x01) /* 0x80410A01 */
#define CELL_SPURS_JQ_ERROR_INVAL               (CELL_ERROR_BASE_SPURS_JQ | 0x02) /* 0x80410A02 */
#define CELL_SPURS_JQ_ERROR_INVALID_ARGUMENT    CELL_SPURS_JQ_ERROR_INVAL
#define CELL_SPURS_JQ_ERROR_OUT_OF_MEMORY       (CELL_ERROR_BASE_SPURS_JQ | 0x04) /* 0x80410A04 */
#define CELL_SPURS_JQ_ERROR_JOB_NOT_FOUND       (CELL_ERROR_BASE_SPURS_JQ | 0x06) /* 0x80410A06 */
#define CELL_SPURS_JQ_ERROR_BUSY                (CELL_ERROR_BASE_SPURS_JQ | 0x0A) /* 0x80410A0A */
#define CELL_SPURS_JQ_ERROR_ABORT               (CELL_ERROR_BASE_SPURS_JQ | 0x0C) /* 0x80410A0C */
#define CELL_SPURS_JQ_ERROR_STAT                (CELL_ERROR_BASE_SPURS_JQ | 0x0F) /* 0x80410A0F */
#define CELL_SPURS_JQ_ERROR_NOT_INITIALIZED     CELL_SPURS_JQ_ERROR_STAT
#define CELL_SPURS_JQ_ERROR_ALIGN               (CELL_ERROR_BASE_SPURS_JQ | 0x10) /* 0x80410A10 */
#define CELL_SPURS_JQ_ERROR_NULL_POINTER        (CELL_ERROR_BASE_SPURS_JQ | 0x11) /* 0x80410A11 */
#define CELL_SPURS_JQ_ERROR_QUEUE_FULL          (CELL_ERROR_BASE_SPURS_JQ | 0x12) /* 0x80410A12 */
#define CELL_SPURS_JQ_ERROR_QUEUE_EMPTY         (CELL_ERROR_BASE_SPURS_JQ | 0x13) /* 0x80410A13 */
#define CELL_SPURS_JQ_ERROR_ALREADY_INITIALIZED (CELL_ERROR_BASE_SPURS_JQ | 0x14) /* 0x80410A14 */

/* Job types */
#define CELL_SPURS_JQ_JOB_TYPE_DEFAULT  0
#define CELL_SPURS_JQ_JOB_TYPE_MEMORY   1
#define CELL_SPURS_JQ_JOB_TYPE_GUARD    2

/* Job priority */
#define CELL_SPURS_JQ_PRIORITY_HIGH     0
#define CELL_SPURS_JQ_PRIORITY_NORMAL   8
#define CELL_SPURS_JQ_PRIORITY_LOW      15

/* Limits */
#define CELL_SPURS_JQ_MAX_QUEUES    16
#define CELL_SPURS_JQ_MAX_JOBS      256

/* Opaque handle types */
typedef u32 CellSpursJobQueue;
typedef u32 CellSpursJobQueueHandle;
typedef u32 CellSpursJobId;

/* Job descriptor */
typedef struct CellSpursJob256 {
    u64 header;
    u64 dmaList[7];
    u64 workArea;
    u64 eaJobBinary;
    u32 sizeDmaList;
    u32 sizeJobBinary;
    u32 sizeWorkArea;
    u32 reserved[3];
} CellSpursJob256;

typedef struct CellSpursJob128 {
    u64 header;
    u64 dmaList[3];
    u64 workArea;
    u64 eaJobBinary;
    u32 sizeDmaList;
    u32 reserved;
} CellSpursJob128;

/* Job queue attributes */
typedef struct CellSpursJobQueueAttribute {
    u32 maxJobs;
    u32 maxGrab;
    u8  priorities[8]; /* per-SPU priority */
    u32 flags;
    u32 reserved[4];
} CellSpursJobQueueAttribute;

/* Job queue port (for connecting event queues) */
typedef u32 CellSpursJobQueuePort;

/* Functions */
s32 cellSpursJobQueueAttributeInitialize(CellSpursJobQueueAttribute* attr);
s32 cellSpursJobQueueAttributeSetMaxGrab(CellSpursJobQueueAttribute* attr, u32 maxGrab);

s32 cellSpursCreateJobQueue(void* spurs, CellSpursJobQueue* jq,
                            const CellSpursJobQueueAttribute* attr,
                            void* buffer, u32 bufferSize);
s32 cellSpursDestroyJobQueue(CellSpursJobQueue* jq);

s32 cellSpursJobQueuePush(CellSpursJobQueue* jq, const void* job,
                          u32 jobSize, u32 tag, CellSpursJobId* id);
s32 cellSpursJobQueuePushJob(CellSpursJobQueue* jq, const CellSpursJob256* job,
                             u32 tag, CellSpursJobId* id);

s32 cellSpursJobQueuePort2Create(CellSpursJobQueue* jq, CellSpursJobQueuePort* port);
s32 cellSpursJobQueuePort2Destroy(CellSpursJobQueuePort* port);
s32 cellSpursJobQueuePort2PushSync(CellSpursJobQueuePort* port,
                                   const void* job, u32 jobSize,
                                   u32 tag, CellSpursJobId* id);

s32 cellSpursJobQueueSendSignal(CellSpursJobQueue* jq, CellSpursJobId id);
s32 cellSpursJobQueueWait(CellSpursJobQueue* jq, CellSpursJobId id);
s32 cellSpursJobQueueTryWait(CellSpursJobQueue* jq, CellSpursJobId id);

s32 cellSpursJobQueueGetCount(CellSpursJobQueue* jq, u32* count);
s32 cellSpursJobQueueSemaphoreTryAcquire(CellSpursJobQueue* jq);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_SPURS_JQ_H */
