/*
 * ps3recomp - cellSync HLE implementation
 *
 * SPU-safe synchronization primitives using C11 atomics.
 * On real PS3, these fit within SPU local store (128-byte aligned).
 * Here we use host atomics which are functionally equivalent.
 */

#include "cellSync.h"
#include <stdio.h>
#include <string.h>

/* Yield hint for spin-wait loops */
#ifdef _WIN32
#include <windows.h>
#define SYNC_YIELD() SwitchToThread()
#else
#include <sched.h>
#define SYNC_YIELD() sched_yield()
#endif

/* =========================================================================
 * Mutex
 * =====================================================================*/

s32 cellSyncMutexInitialize(CellSyncMutex* mutex)
{
    if (!mutex)
        return CELL_SYNC_ERROR_NULL_POINTER;

    atomic_store(&mutex->lock, 0);
    return CELL_OK;
}

s32 cellSyncMutexLock(CellSyncMutex* mutex)
{
    if (!mutex)
        return CELL_SYNC_ERROR_NULL_POINTER;

    unsigned int expected;
    int spins = 0;

    for (;;) {
        expected = 0;
        if (atomic_compare_exchange_weak(&mutex->lock, &expected, 1))
            return CELL_OK;

        if (++spins > 1000) {
            SYNC_YIELD();
            spins = 0;
        }
    }
}

s32 cellSyncMutexTryLock(CellSyncMutex* mutex)
{
    if (!mutex)
        return CELL_SYNC_ERROR_NULL_POINTER;

    unsigned int expected = 0;
    if (atomic_compare_exchange_strong(&mutex->lock, &expected, 1))
        return CELL_OK;

    return CELL_SYNC_ERROR_BUSY;
}

s32 cellSyncMutexUnlock(CellSyncMutex* mutex)
{
    if (!mutex)
        return CELL_SYNC_ERROR_NULL_POINTER;

    atomic_store(&mutex->lock, 0);
    return CELL_OK;
}

/* =========================================================================
 * Barrier
 * =====================================================================*/

s32 cellSyncBarrierInitialize(CellSyncBarrier* barrier, u16 totalCount)
{
    if (!barrier)
        return CELL_SYNC_ERROR_NULL_POINTER;

    if (totalCount == 0)
        return CELL_SYNC_ERROR_INVAL;

    atomic_store(&barrier->arrived, 0);
    barrier->total = totalCount;
    atomic_store(&barrier->phase, 0);

    return CELL_OK;
}

s32 cellSyncBarrierNotify(CellSyncBarrier* barrier)
{
    if (!barrier)
        return CELL_SYNC_ERROR_NULL_POINTER;

    unsigned int old_arrived = atomic_fetch_add(&barrier->arrived, 1);

    /* If we're the last to arrive, advance the phase and reset */
    if (old_arrived + 1 >= barrier->total) {
        atomic_store(&barrier->arrived, 0);
        atomic_fetch_add(&barrier->phase, 1);
    }

    return CELL_OK;
}

s32 cellSyncBarrierTryNotify(CellSyncBarrier* barrier)
{
    /* Same as notify for this implementation */
    return cellSyncBarrierNotify(barrier);
}

s32 cellSyncBarrierWait(CellSyncBarrier* barrier)
{
    if (!barrier)
        return CELL_SYNC_ERROR_NULL_POINTER;

    unsigned int phase = atomic_load(&barrier->phase);
    int spins = 0;

    /* Wait until the phase changes (all have arrived) */
    while (atomic_load(&barrier->phase) == phase) {
        if (++spins > 1000) {
            SYNC_YIELD();
            spins = 0;
        }
    }

    return CELL_OK;
}

s32 cellSyncBarrierTryWait(CellSyncBarrier* barrier)
{
    if (!barrier)
        return CELL_SYNC_ERROR_NULL_POINTER;

    /* Check if all have arrived (count == 0 means reset happened) */
    if (atomic_load(&barrier->arrived) != 0)
        return CELL_SYNC_ERROR_BUSY;

    return CELL_OK;
}

/* =========================================================================
 * Reader-Writer Memory
 * =====================================================================*/

s32 cellSyncRwmInitialize(CellSyncRwm* rwm, void* buffer, u32 size)
{
    if (!rwm || !buffer)
        return CELL_SYNC_ERROR_NULL_POINTER;

    if (size == 0)
        return CELL_SYNC_ERROR_INVAL;

    atomic_store(&rwm->readers, 0);
    atomic_store(&rwm->writer, 0);
    rwm->size = size;
    rwm->buffer = buffer;

    return CELL_OK;
}

s32 cellSyncRwmRead(CellSyncRwm* rwm, void* dst)
{
    if (!rwm || !dst)
        return CELL_SYNC_ERROR_NULL_POINTER;

    int spins = 0;

    /* Wait until no writer is active */
    while (atomic_load(&rwm->writer)) {
        if (++spins > 1000) { SYNC_YIELD(); spins = 0; }
    }

    atomic_fetch_add(&rwm->readers, 1);

    /* Double-check no writer started */
    while (atomic_load(&rwm->writer)) {
        atomic_fetch_sub(&rwm->readers, 1);
        while (atomic_load(&rwm->writer)) {
            if (++spins > 1000) { SYNC_YIELD(); spins = 0; }
        }
        atomic_fetch_add(&rwm->readers, 1);
    }

    memcpy(dst, rwm->buffer, rwm->size);
    atomic_fetch_sub(&rwm->readers, 1);

    return CELL_OK;
}

s32 cellSyncRwmTryRead(CellSyncRwm* rwm, void* dst)
{
    if (!rwm || !dst)
        return CELL_SYNC_ERROR_NULL_POINTER;

    if (atomic_load(&rwm->writer))
        return CELL_SYNC_ERROR_BUSY;

    atomic_fetch_add(&rwm->readers, 1);

    if (atomic_load(&rwm->writer)) {
        atomic_fetch_sub(&rwm->readers, 1);
        return CELL_SYNC_ERROR_BUSY;
    }

    memcpy(dst, rwm->buffer, rwm->size);
    atomic_fetch_sub(&rwm->readers, 1);

    return CELL_OK;
}

s32 cellSyncRwmWrite(CellSyncRwm* rwm, const void* src)
{
    if (!rwm || !src)
        return CELL_SYNC_ERROR_NULL_POINTER;

    unsigned int expected;
    int spins = 0;

    /* Acquire write lock */
    for (;;) {
        expected = 0;
        if (atomic_compare_exchange_weak(&rwm->writer, &expected, 1))
            break;
        if (++spins > 1000) { SYNC_YIELD(); spins = 0; }
    }

    /* Wait for all readers to finish */
    spins = 0;
    while (atomic_load(&rwm->readers) > 0) {
        if (++spins > 1000) { SYNC_YIELD(); spins = 0; }
    }

    memcpy(rwm->buffer, src, rwm->size);
    atomic_store(&rwm->writer, 0);

    return CELL_OK;
}

s32 cellSyncRwmTryWrite(CellSyncRwm* rwm, const void* src)
{
    if (!rwm || !src)
        return CELL_SYNC_ERROR_NULL_POINTER;

    unsigned int expected = 0;
    if (!atomic_compare_exchange_strong(&rwm->writer, &expected, 1))
        return CELL_SYNC_ERROR_BUSY;

    if (atomic_load(&rwm->readers) > 0) {
        atomic_store(&rwm->writer, 0);
        return CELL_SYNC_ERROR_BUSY;
    }

    memcpy(rwm->buffer, src, rwm->size);
    atomic_store(&rwm->writer, 0);

    return CELL_OK;
}

/* =========================================================================
 * Bounded Queue
 * =====================================================================*/

static void queue_spinlock_acquire(atomic_uint* lock)
{
    unsigned int expected;
    int spins = 0;
    for (;;) {
        expected = 0;
        if (atomic_compare_exchange_weak(lock, &expected, 1))
            return;
        if (++spins > 1000) { SYNC_YIELD(); spins = 0; }
    }
}

static void queue_spinlock_release(atomic_uint* lock)
{
    atomic_store(lock, 0);
}

s32 cellSyncQueueInitialize(CellSyncQueue* queue, void* buffer,
                            u32 size, u32 depth)
{
    if (!queue || !buffer)
        return CELL_SYNC_ERROR_NULL_POINTER;

    if (size == 0 || depth == 0 || depth > CELL_SYNC_QUEUE_MAX_DEPTH)
        return CELL_SYNC_ERROR_INVAL;

    atomic_store(&queue->head, 0);
    atomic_store(&queue->tail, 0);
    atomic_store(&queue->count, 0);
    atomic_store(&queue->lock, 0);
    queue->depth = depth;
    queue->elemSize = size;
    queue->buffer = (u8*)buffer;

    memset(buffer, 0, (size_t)size * depth);

    return CELL_OK;
}

s32 cellSyncQueuePush(CellSyncQueue* queue, const void* data)
{
    if (!queue || !data)
        return CELL_SYNC_ERROR_NULL_POINTER;

    int spins = 0;

    /* Spin until there's space */
    while (atomic_load(&queue->count) >= queue->depth) {
        if (++spins > 1000) { SYNC_YIELD(); spins = 0; }
    }

    queue_spinlock_acquire(&queue->lock);

    if (atomic_load(&queue->count) >= queue->depth) {
        queue_spinlock_release(&queue->lock);
        return cellSyncQueuePush(queue, data); /* retry */
    }

    u32 tail = atomic_load(&queue->tail);
    memcpy(queue->buffer + (size_t)tail * queue->elemSize,
           data, queue->elemSize);
    atomic_store(&queue->tail, (tail + 1) % queue->depth);
    atomic_fetch_add(&queue->count, 1);

    queue_spinlock_release(&queue->lock);
    return CELL_OK;
}

s32 cellSyncQueueTryPush(CellSyncQueue* queue, const void* data)
{
    if (!queue || !data)
        return CELL_SYNC_ERROR_NULL_POINTER;

    if (atomic_load(&queue->count) >= queue->depth)
        return CELL_SYNC_ERROR_OVERFLOW;

    unsigned int expected = 0;
    if (!atomic_compare_exchange_strong(&queue->lock, &expected, 1))
        return CELL_SYNC_ERROR_BUSY;

    if (atomic_load(&queue->count) >= queue->depth) {
        atomic_store(&queue->lock, 0);
        return CELL_SYNC_ERROR_OVERFLOW;
    }

    u32 tail = atomic_load(&queue->tail);
    memcpy(queue->buffer + (size_t)tail * queue->elemSize,
           data, queue->elemSize);
    atomic_store(&queue->tail, (tail + 1) % queue->depth);
    atomic_fetch_add(&queue->count, 1);

    atomic_store(&queue->lock, 0);
    return CELL_OK;
}

s32 cellSyncQueuePop(CellSyncQueue* queue, void* data)
{
    if (!queue || !data)
        return CELL_SYNC_ERROR_NULL_POINTER;

    int spins = 0;

    while (atomic_load(&queue->count) == 0) {
        if (++spins > 1000) { SYNC_YIELD(); spins = 0; }
    }

    queue_spinlock_acquire(&queue->lock);

    if (atomic_load(&queue->count) == 0) {
        queue_spinlock_release(&queue->lock);
        return cellSyncQueuePop(queue, data); /* retry */
    }

    u32 head = atomic_load(&queue->head);
    memcpy(data, queue->buffer + (size_t)head * queue->elemSize,
           queue->elemSize);
    atomic_store(&queue->head, (head + 1) % queue->depth);
    atomic_fetch_sub(&queue->count, 1);

    queue_spinlock_release(&queue->lock);
    return CELL_OK;
}

s32 cellSyncQueueTryPop(CellSyncQueue* queue, void* data)
{
    if (!queue || !data)
        return CELL_SYNC_ERROR_NULL_POINTER;

    if (atomic_load(&queue->count) == 0)
        return CELL_SYNC_ERROR_EMPTY;

    unsigned int expected = 0;
    if (!atomic_compare_exchange_strong(&queue->lock, &expected, 1))
        return CELL_SYNC_ERROR_BUSY;

    if (atomic_load(&queue->count) == 0) {
        atomic_store(&queue->lock, 0);
        return CELL_SYNC_ERROR_EMPTY;
    }

    u32 head = atomic_load(&queue->head);
    memcpy(data, queue->buffer + (size_t)head * queue->elemSize,
           queue->elemSize);
    atomic_store(&queue->head, (head + 1) % queue->depth);
    atomic_fetch_sub(&queue->count, 1);

    atomic_store(&queue->lock, 0);
    return CELL_OK;
}

s32 cellSyncQueuePeek(CellSyncQueue* queue, void* data)
{
    if (!queue || !data)
        return CELL_SYNC_ERROR_NULL_POINTER;

    if (atomic_load(&queue->count) == 0)
        return CELL_SYNC_ERROR_EMPTY;

    queue_spinlock_acquire(&queue->lock);

    if (atomic_load(&queue->count) == 0) {
        queue_spinlock_release(&queue->lock);
        return CELL_SYNC_ERROR_EMPTY;
    }

    u32 head = atomic_load(&queue->head);
    memcpy(data, queue->buffer + (size_t)head * queue->elemSize,
           queue->elemSize);

    queue_spinlock_release(&queue->lock);
    return CELL_OK;
}

s32 cellSyncQueueSize(CellSyncQueue* queue, u32* size)
{
    if (!queue || !size)
        return CELL_SYNC_ERROR_NULL_POINTER;

    *size = atomic_load(&queue->count);
    return CELL_OK;
}

s32 cellSyncQueueClear(CellSyncQueue* queue)
{
    if (!queue)
        return CELL_SYNC_ERROR_NULL_POINTER;

    queue_spinlock_acquire(&queue->lock);
    atomic_store(&queue->head, 0);
    atomic_store(&queue->tail, 0);
    atomic_store(&queue->count, 0);
    queue_spinlock_release(&queue->lock);

    return CELL_OK;
}

/* =========================================================================
 * Lock-Free Queue
 *
 * Uses the same spinlock approach as bounded queue for simplicity.
 * A true lock-free implementation would use multi-word CAS.
 * =====================================================================*/

s32 cellSyncLFQueueInitialize(CellSyncLFQueue* queue, void* buffer,
                               u32 size, u32 depth, u32 direction,
                               void* eaSignal)
{
    (void)eaSignal;

    if (!queue || !buffer)
        return CELL_SYNC_ERROR_NULL_POINTER;

    if (size == 0 || depth == 0)
        return CELL_SYNC_ERROR_INVAL;

    atomic_store(&queue->head, 0);
    atomic_store(&queue->tail, 0);
    atomic_store(&queue->count, 0);
    atomic_store(&queue->lock, 0);
    queue->depth = depth;
    queue->elemSize = size;
    queue->buffer = (u8*)buffer;
    queue->direction = direction;

    memset(buffer, 0, (size_t)size * depth);

    printf("[cellSync] LFQueueInitialize(size=%u, depth=%u, dir=%u)\n",
           size, depth, direction);
    return CELL_OK;
}

s32 cellSyncLFQueuePush(CellSyncLFQueue* queue, const void* data)
{
    if (!queue || !data)
        return CELL_SYNC_ERROR_NULL_POINTER;

    int spins = 0;
    while (atomic_load(&queue->count) >= queue->depth) {
        if (++spins > 1000) { SYNC_YIELD(); spins = 0; }
    }

    queue_spinlock_acquire(&queue->lock);

    if (atomic_load(&queue->count) >= queue->depth) {
        queue_spinlock_release(&queue->lock);
        return cellSyncLFQueuePush(queue, data);
    }

    u32 tail = atomic_load(&queue->tail);
    memcpy(queue->buffer + (size_t)tail * queue->elemSize,
           data, queue->elemSize);
    atomic_store(&queue->tail, (tail + 1) % queue->depth);
    atomic_fetch_add(&queue->count, 1);

    queue_spinlock_release(&queue->lock);
    return CELL_OK;
}

s32 cellSyncLFQueueTryPush(CellSyncLFQueue* queue, const void* data)
{
    if (!queue || !data)
        return CELL_SYNC_ERROR_NULL_POINTER;

    if (atomic_load(&queue->count) >= queue->depth)
        return CELL_SYNC_ERROR_OVERFLOW;

    unsigned int expected = 0;
    if (!atomic_compare_exchange_strong(&queue->lock, &expected, 1))
        return CELL_SYNC_ERROR_BUSY;

    if (atomic_load(&queue->count) >= queue->depth) {
        atomic_store(&queue->lock, 0);
        return CELL_SYNC_ERROR_OVERFLOW;
    }

    u32 tail = atomic_load(&queue->tail);
    memcpy(queue->buffer + (size_t)tail * queue->elemSize,
           data, queue->elemSize);
    atomic_store(&queue->tail, (tail + 1) % queue->depth);
    atomic_fetch_add(&queue->count, 1);

    atomic_store(&queue->lock, 0);
    return CELL_OK;
}

s32 cellSyncLFQueuePop(CellSyncLFQueue* queue, void* data)
{
    if (!queue || !data)
        return CELL_SYNC_ERROR_NULL_POINTER;

    int spins = 0;
    while (atomic_load(&queue->count) == 0) {
        if (++spins > 1000) { SYNC_YIELD(); spins = 0; }
    }

    queue_spinlock_acquire(&queue->lock);

    if (atomic_load(&queue->count) == 0) {
        queue_spinlock_release(&queue->lock);
        return cellSyncLFQueuePop(queue, data);
    }

    u32 head = atomic_load(&queue->head);
    memcpy(data, queue->buffer + (size_t)head * queue->elemSize,
           queue->elemSize);
    atomic_store(&queue->head, (head + 1) % queue->depth);
    atomic_fetch_sub(&queue->count, 1);

    queue_spinlock_release(&queue->lock);
    return CELL_OK;
}

s32 cellSyncLFQueueTryPop(CellSyncLFQueue* queue, void* data)
{
    if (!queue || !data)
        return CELL_SYNC_ERROR_NULL_POINTER;

    if (atomic_load(&queue->count) == 0)
        return CELL_SYNC_ERROR_EMPTY;

    unsigned int expected = 0;
    if (!atomic_compare_exchange_strong(&queue->lock, &expected, 1))
        return CELL_SYNC_ERROR_BUSY;

    if (atomic_load(&queue->count) == 0) {
        atomic_store(&queue->lock, 0);
        return CELL_SYNC_ERROR_EMPTY;
    }

    u32 head = atomic_load(&queue->head);
    memcpy(data, queue->buffer + (size_t)head * queue->elemSize,
           queue->elemSize);
    atomic_store(&queue->head, (head + 1) % queue->depth);
    atomic_fetch_sub(&queue->count, 1);

    atomic_store(&queue->lock, 0);
    return CELL_OK;
}

s32 cellSyncLFQueueGetDirection(const CellSyncLFQueue* queue, u32* dir)
{
    if (!queue || !dir)
        return CELL_SYNC_ERROR_NULL_POINTER;

    *dir = queue->direction;
    return CELL_OK;
}

s32 cellSyncLFQueueDepth(const CellSyncLFQueue* queue, u32* depth)
{
    if (!queue || !depth)
        return CELL_SYNC_ERROR_NULL_POINTER;

    *depth = queue->depth;
    return CELL_OK;
}

s32 cellSyncLFQueueSize(CellSyncLFQueue* queue, u32* size)
{
    if (!queue || !size)
        return CELL_SYNC_ERROR_NULL_POINTER;

    *size = atomic_load(&queue->count);
    return CELL_OK;
}

s32 cellSyncLFQueueClear(CellSyncLFQueue* queue)
{
    if (!queue)
        return CELL_SYNC_ERROR_NULL_POINTER;

    queue_spinlock_acquire(&queue->lock);
    atomic_store(&queue->head, 0);
    atomic_store(&queue->tail, 0);
    atomic_store(&queue->count, 0);
    queue_spinlock_release(&queue->lock);

    return CELL_OK;
}
