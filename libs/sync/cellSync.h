/*
 * ps3recomp - cellSync HLE
 *
 * SPU-safe synchronization primitives: mutex, barrier, reader-writer,
 * bounded queue, and lock-free queue.  Implemented with C11 atomics.
 */

#ifndef PS3RECOMP_CELL_SYNC_H
#define PS3RECOMP_CELL_SYNC_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
#define CELL_SYNC_ERROR_AGAIN                   (CELL_ERROR_BASE_SYNC | 0x01)
#define CELL_SYNC_ERROR_INVAL                   (CELL_ERROR_BASE_SYNC | 0x02)
#define CELL_SYNC_ERROR_NOSYS                   (CELL_ERROR_BASE_SYNC | 0x03)
#define CELL_SYNC_ERROR_NOMEM                   (CELL_ERROR_BASE_SYNC | 0x04)
#define CELL_SYNC_ERROR_SRCH                    (CELL_ERROR_BASE_SYNC | 0x05)
#define CELL_SYNC_ERROR_NOENT                   (CELL_ERROR_BASE_SYNC | 0x06)
#define CELL_SYNC_ERROR_BUSY                    (CELL_ERROR_BASE_SYNC | 0x0A)
#define CELL_SYNC_ERROR_STAT                    (CELL_ERROR_BASE_SYNC | 0x0F)
#define CELL_SYNC_ERROR_ALIGN                   (CELL_ERROR_BASE_SYNC | 0x10)
#define CELL_SYNC_ERROR_NULL_POINTER            (CELL_ERROR_BASE_SYNC | 0x11)
#define CELL_SYNC_ERROR_PERM                    (CELL_ERROR_BASE_SYNC | 0x09)
#define CELL_SYNC_ERROR_OVERFLOW                (CELL_ERROR_BASE_SYNC | 0x12)
#define CELL_SYNC_ERROR_EMPTY                   (CELL_ERROR_BASE_SYNC | 0x13)

/* ---------------------------------------------------------------------------
 * CellSyncMutex -- 32-bit spinlock
 * -----------------------------------------------------------------------*/
typedef struct CellSyncMutex {
    atomic_uint lock;
} CellSyncMutex;

s32 cellSyncMutexInitialize(CellSyncMutex* mutex);
s32 cellSyncMutexLock(CellSyncMutex* mutex);
s32 cellSyncMutexTryLock(CellSyncMutex* mutex);
s32 cellSyncMutexUnlock(CellSyncMutex* mutex);

/* ---------------------------------------------------------------------------
 * CellSyncBarrier -- counter-based barrier
 * -----------------------------------------------------------------------*/
typedef struct CellSyncBarrier {
    atomic_uint arrived;
    u32         total;
    atomic_uint phase;
} CellSyncBarrier;

s32 cellSyncBarrierInitialize(CellSyncBarrier* barrier, u16 totalCount);
s32 cellSyncBarrierNotify(CellSyncBarrier* barrier);
s32 cellSyncBarrierTryNotify(CellSyncBarrier* barrier);
s32 cellSyncBarrierWait(CellSyncBarrier* barrier);
s32 cellSyncBarrierTryWait(CellSyncBarrier* barrier);

/* ---------------------------------------------------------------------------
 * CellSyncRwm -- reader-writer memory
 * -----------------------------------------------------------------------*/
typedef struct CellSyncRwm {
    atomic_int  readers;
    atomic_uint writer;
    u32         size;
    void*       buffer;
} CellSyncRwm;

s32 cellSyncRwmInitialize(CellSyncRwm* rwm, void* buffer, u32 size);
s32 cellSyncRwmRead(CellSyncRwm* rwm, void* dst);
s32 cellSyncRwmTryRead(CellSyncRwm* rwm, void* dst);
s32 cellSyncRwmWrite(CellSyncRwm* rwm, const void* src);
s32 cellSyncRwmTryWrite(CellSyncRwm* rwm, const void* src);

/* ---------------------------------------------------------------------------
 * CellSyncQueue -- bounded queue
 * -----------------------------------------------------------------------*/
#define CELL_SYNC_QUEUE_MAX_DEPTH   1024

typedef struct CellSyncQueue {
    atomic_uint head;
    atomic_uint tail;
    atomic_uint count;
    u32         depth;
    u32         elemSize;
    u8*         buffer;
    atomic_uint lock;  /* simple spinlock for push/pop */
} CellSyncQueue;

s32 cellSyncQueueInitialize(CellSyncQueue* queue, void* buffer,
                            u32 size, u32 depth);
s32 cellSyncQueuePush(CellSyncQueue* queue, const void* data);
s32 cellSyncQueueTryPush(CellSyncQueue* queue, const void* data);
s32 cellSyncQueuePop(CellSyncQueue* queue, void* data);
s32 cellSyncQueueTryPop(CellSyncQueue* queue, void* data);
s32 cellSyncQueuePeek(CellSyncQueue* queue, void* data);
s32 cellSyncQueueSize(CellSyncQueue* queue, u32* size);
s32 cellSyncQueueClear(CellSyncQueue* queue);

/* ---------------------------------------------------------------------------
 * CellSyncLFQueue -- lock-free queue
 * -----------------------------------------------------------------------*/
#define CELL_SYNC_LFQUEUE_DIR_PUSH  0
#define CELL_SYNC_LFQUEUE_DIR_POP   1

typedef struct CellSyncLFQueue {
    atomic_uint head;
    atomic_uint tail;
    atomic_uint count;
    u32         depth;
    u32         elemSize;
    u8*         buffer;
    u32         direction;
    atomic_uint lock;
} CellSyncLFQueue;

s32 cellSyncLFQueueInitialize(CellSyncLFQueue* queue, void* buffer,
                               u32 size, u32 depth, u32 direction,
                               void* eaSignal);
s32 cellSyncLFQueuePush(CellSyncLFQueue* queue, const void* data);
s32 cellSyncLFQueueTryPush(CellSyncLFQueue* queue, const void* data);
s32 cellSyncLFQueuePop(CellSyncLFQueue* queue, void* data);
s32 cellSyncLFQueueTryPop(CellSyncLFQueue* queue, void* data);
s32 cellSyncLFQueueGetDirection(const CellSyncLFQueue* queue, u32* dir);
s32 cellSyncLFQueueDepth(const CellSyncLFQueue* queue, u32* depth);
s32 cellSyncLFQueueSize(CellSyncLFQueue* queue, u32* size);
s32 cellSyncLFQueueClear(CellSyncLFQueue* queue);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_SYNC_H */
