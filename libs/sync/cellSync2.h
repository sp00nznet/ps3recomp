/*
 * ps3recomp - cellSync2 HLE
 *
 * Extended synchronization primitives with timeout and priority support.
 * Implemented using host OS primitives (Windows SRWLOCK/CONDITION_VARIABLE,
 * pthreads mutex/cond/sem on Unix).
 */

#ifndef PS3RECOMP_CELL_SYNC2_H
#define PS3RECOMP_CELL_SYNC2_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes (shared base with cellSync)
 * -----------------------------------------------------------------------*/
#define CELL_SYNC2_ERROR_AGAIN                  (CELL_ERROR_BASE_SYNC | 0x41)
#define CELL_SYNC2_ERROR_INVAL                  (CELL_ERROR_BASE_SYNC | 0x42)
#define CELL_SYNC2_ERROR_NOMEM                  (CELL_ERROR_BASE_SYNC | 0x44)
#define CELL_SYNC2_ERROR_BUSY                   (CELL_ERROR_BASE_SYNC | 0x4A)
#define CELL_SYNC2_ERROR_STAT                   (CELL_ERROR_BASE_SYNC | 0x4F)
#define CELL_SYNC2_ERROR_NULL_POINTER           (CELL_ERROR_BASE_SYNC | 0x51)
#define CELL_SYNC2_ERROR_TIMEDOUT               (CELL_ERROR_BASE_SYNC | 0x52)
#define CELL_SYNC2_ERROR_NOT_INITIALIZED        (CELL_ERROR_BASE_SYNC | 0x53)
#define CELL_SYNC2_ERROR_OVERFLOW               (CELL_ERROR_BASE_SYNC | 0x54)
#define CELL_SYNC2_ERROR_EMPTY                  (CELL_ERROR_BASE_SYNC | 0x55)

/* ---------------------------------------------------------------------------
 * CellSync2Mutex
 * -----------------------------------------------------------------------*/

/* Opaque handle -- internally holds OS mutex */
#define CELL_SYNC2_MUTEX_SIZE   128

typedef struct CellSync2Mutex {
    u8 _opaque[CELL_SYNC2_MUTEX_SIZE];
} CellSync2Mutex;

typedef struct CellSync2MutexAttribute {
    u32 maxWaiters;
    u8  recursive;
    u8  padding[3];
} CellSync2MutexAttribute;

s32 cellSync2MutexAttributeInitialize(CellSync2MutexAttribute* attr);
s32 cellSync2MutexInit(CellSync2Mutex* mutex, const CellSync2MutexAttribute* attr);
s32 cellSync2MutexLock(CellSync2Mutex* mutex, u64 timeoutUsec);
s32 cellSync2MutexTryLock(CellSync2Mutex* mutex);
s32 cellSync2MutexUnlock(CellSync2Mutex* mutex);

/* ---------------------------------------------------------------------------
 * CellSync2Cond -- condition variable
 * -----------------------------------------------------------------------*/

#define CELL_SYNC2_COND_SIZE    128

typedef struct CellSync2Cond {
    u8 _opaque[CELL_SYNC2_COND_SIZE];
} CellSync2Cond;

typedef struct CellSync2CondAttribute {
    u32 maxWaiters;
} CellSync2CondAttribute;

s32 cellSync2CondAttributeInitialize(CellSync2CondAttribute* attr);
s32 cellSync2CondInit(CellSync2Cond* cond, CellSync2Mutex* mutex,
                      const CellSync2CondAttribute* attr);
s32 cellSync2CondWait(CellSync2Cond* cond, u64 timeoutUsec);
s32 cellSync2CondSignal(CellSync2Cond* cond);
s32 cellSync2CondSignalAll(CellSync2Cond* cond);

/* ---------------------------------------------------------------------------
 * CellSync2Semaphore
 * -----------------------------------------------------------------------*/

#define CELL_SYNC2_SEMAPHORE_SIZE   128

typedef struct CellSync2Semaphore {
    u8 _opaque[CELL_SYNC2_SEMAPHORE_SIZE];
} CellSync2Semaphore;

typedef struct CellSync2SemaphoreAttribute {
    u32 maxWaiters;
    s32 maxValue;
} CellSync2SemaphoreAttribute;

s32 cellSync2SemaphoreAttributeInitialize(CellSync2SemaphoreAttribute* attr);
s32 cellSync2SemaphoreInit(CellSync2Semaphore* sem, s32 initialValue,
                           const CellSync2SemaphoreAttribute* attr);
s32 cellSync2SemaphoreAcquire(CellSync2Semaphore* sem, u32 count,
                              u64 timeoutUsec);
s32 cellSync2SemaphoreTryAcquire(CellSync2Semaphore* sem, u32 count);
s32 cellSync2SemaphoreRelease(CellSync2Semaphore* sem, u32 count);
s32 cellSync2SemaphoreGetValue(const CellSync2Semaphore* sem, s32* value);

/* ---------------------------------------------------------------------------
 * CellSync2Queue -- bounded queue with blocking and timeout
 * -----------------------------------------------------------------------*/

#define CELL_SYNC2_QUEUE_SIZE   256

typedef struct CellSync2Queue {
    u8 _opaque[CELL_SYNC2_QUEUE_SIZE];
} CellSync2Queue;

typedef struct CellSync2QueueAttribute {
    u32 maxPushWaiters;
    u32 maxPopWaiters;
} CellSync2QueueAttribute;

s32 cellSync2QueueAttributeInitialize(CellSync2QueueAttribute* attr);
s32 cellSync2QueueInit(CellSync2Queue* queue, void* buffer,
                       u32 elemSize, u32 depth,
                       const CellSync2QueueAttribute* attr);
s32 cellSync2QueuePush(CellSync2Queue* queue, const void* data,
                       u64 timeoutUsec);
s32 cellSync2QueueTryPush(CellSync2Queue* queue, const void* data);
s32 cellSync2QueuePop(CellSync2Queue* queue, void* data,
                      u64 timeoutUsec);
s32 cellSync2QueueTryPop(CellSync2Queue* queue, void* data);
s32 cellSync2QueueSize(const CellSync2Queue* queue, u32* size);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_SYNC2_H */
