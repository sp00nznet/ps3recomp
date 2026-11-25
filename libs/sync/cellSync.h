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
 * CellSyncBarrier -- 4-byte counter-based barrier (big-endian {u16 value, u16 count})
 * -----------------------------------------------------------------------*/
typedef struct CellSyncBarrier {
    atomic_uint word;   /* guest BE: (value << 16) | count; bit 15 of value = drain phase */
} CellSyncBarrier;

s32 cellSyncBarrierInitialize(CellSyncBarrier* barrier, u16 totalCount);
s32 cellSyncBarrierNotify(CellSyncBarrier* barrier);
s32 cellSyncBarrierTryNotify(CellSyncBarrier* barrier);
s32 cellSyncBarrierWait(CellSyncBarrier* barrier);
s32 cellSyncBarrierTryWait(CellSyncBarrier* barrier);

/* ---------------------------------------------------------------------------
 * CellSyncRwm -- 16-byte reader-writer memory (big-endian guest layout)
 *   +0x00: BE u16 readers, BE u16 writers
 *   +0x04: BE u32 buffer_size
 *   +0x08: BE u64 ea_buffer (32-bit guest EA)
 * -----------------------------------------------------------------------*/
typedef struct CellSyncRwm {
    atomic_uint ctrl;
    u32         size_be;
    u64         buffer_ea_be;
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

/* Opaque: guest big-endian layout, accessed by EA -- see cellSync.c. */
typedef struct CellSyncQueue {
    u8 raw[32];
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

/* Guest layout: one 128-byte, 128-aligned line. The guest-visible header is
 * big-endian at the offsets Sony firmware 4.93 libsync.sprx writes from
 * cellSyncLFQueueInitialize(size=16, depth=8, dir=2) on a zeroed line:
 *   +0x10 u32 size   +0x14 u32 depth   +0x18 u64 buffer EA   +0x24 u32 direction
 * (+0x20 = 0xFFFFFFFF and +0x2C = 2 were also observed for dir=2; their
 * meaning is not modelled.) +0x70 u64 eaSignal per the SPURS LFQueue line.
 * The HLE's own ring state is host-endian in +0x40..+0x4F, which Sony leaves
 * zero after init; it is private to this implementation. */
#define CELL_SYNC_LFQUEUE_SIZE   128u
#define CELL_SYNC_LFQUEUE_ALIGN  128u

typedef struct CellSyncLFQueue {
    u8          sony_00[0x10];  /* +0x00 pop1/push1 (Sony-internal, zero)    */
    u32         size_be;        /* +0x10 entry size, BE                      */
    u32         depth_be;       /* +0x14 depth, BE                           */
    u64         buffer_be;      /* +0x18 32-bit guest buffer EA as BE u64    */
    u32         sony_20;        /* +0x20                                      */
    u32         direction_be;   /* +0x24 direction, BE                       */
    u8          sony_28[0x18];  /* +0x28..+0x3F                               */
    atomic_uint head;           /* +0x40 HLE-private, host-endian            */
    atomic_uint tail;           /* +0x44                                      */
    atomic_uint count;          /* +0x48                                      */
    atomic_uint lock;           /* +0x4C                                      */
    u8          sony_50[0x20];  /* +0x50..+0x6F                               */
    u64         eaSignal_be;    /* +0x70 BE                                   */
    u8          sony_78[0x08];  /* +0x78..+0x7F                               */
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
