/*
 * ps3recomp - cellSync HLE implementation
 *
 * SPU-safe synchronization primitives using C11 atomics.
 * On real PS3, these fit within SPU local store (128-byte aligned).
 * Here we use host atomics which are functionally equivalent.
 */

#include "cellSync.h"
#include <stdint.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base (guest mem) */
/* HLE args arrive as guest effective addresses (ps3_hle_call passes raw
 * guest register values); translate to host before dereferencing. */
#define GUEST_PTR(p, T) ((T)((p) ? (void*)(vm_base + (uint32_t)(uintptr_t)(p)) : (void*)0))
#include <stdio.h>
#include <stdlib.h>   /* getenv -- sync_trace(); pulled in transitively by windows.h, absent on POSIX */
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

/* CellSyncMutex is a TICKET lock, not a 0/1 flag, and it lives in guest memory
 * that SPU code manipulates directly. Its 32-bit word is big-endian:
 *
 *     u16 m_freed;   // high half: tickets released
 *     u16 m_order;   // low  half: next ticket to hand out
 *
 * free  <=> m_freed == m_order.  Treating the word as a host-endian flag and
 * CAS-ing it from 0 means a mutex the SPU left at m_freed==m_order==1 reads as
 * 0x01000100 -- never zero -- and TryLock spins forever. Swap to interpret. */
static inline uint32_t sync_bswap32(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0x0000FF00u) | ((v << 8) & 0x00FF0000u) | (v << 24);
}

/* ponytail: the sibling primitives below (barrier / rwlock / queue) still
 * assume host-endian words. Same treatment when a title actually exercises
 * them; not fixed blind here. */

static int sync_trace(void){ static int v=-1; if(v<0){const char*e=getenv("SYNC_TRACE"); v=e?1:0;} return v; }
#ifdef _WIN32
#define SYNC_TID() ((unsigned long)GetCurrentThreadId())
#else
#define SYNC_TID() ((unsigned long)0)
#endif

s32 cellSyncMutexInitialize(CellSyncMutex* mutex)
{
    mutex = GUEST_PTR(mutex, CellSyncMutex*);
    if (!mutex)
        return CELL_SYNC_ERROR_NULL_POINTER;

    atomic_store(&mutex->lock, 0);   /* m_freed = m_order = 0 -> free */
    return CELL_OK;
}

s32 cellSyncMutexLock(CellSyncMutex* mutex)
{
    unsigned int _ea = (unsigned int)(uintptr_t)mutex;
    mutex = GUEST_PTR(mutex, CellSyncMutex*);
    if (!mutex)
        return CELL_SYNC_ERROR_NULL_POINTER;

    /* Take a ticket. */
    uint32_t raw, g;
    uint16_t ticket;
    for (;;) {
        raw = atomic_load(&mutex->lock);
        g   = sync_bswap32(raw);
        ticket = (uint16_t)g;                       /* m_order */
        uint32_t ng = (g & 0xFFFF0000u) | (uint16_t)(ticket + 1);
        if (atomic_compare_exchange_weak(&mutex->lock, &raw, sync_bswap32(ng)))
            break;
    }
    /* Wait until it is served. */
    int spins = 0;
    for (;;) {
        g = sync_bswap32(atomic_load(&mutex->lock));
        if ((uint16_t)(g >> 16) == ticket) {
            if (sync_trace()) fprintf(stderr, "[SYNC] LOCK    ea=0x%08X tid=%lu ticket=%u\n",
                                      _ea, SYNC_TID(), ticket);
            return CELL_OK;
        }
        if (++spins > 1000) { SYNC_YIELD(); spins = 0; }
    }
}

s32 cellSyncMutexTryLock(CellSyncMutex* mutex)
{
    unsigned int _ea = (unsigned int)(uintptr_t)mutex;
    mutex = GUEST_PTR(mutex, CellSyncMutex*);
    if (!mutex)
        return CELL_SYNC_ERROR_NULL_POINTER;

    uint32_t raw = atomic_load(&mutex->lock);
    uint32_t g   = sync_bswap32(raw);
    uint16_t freed = (uint16_t)(g >> 16), order = (uint16_t)g;

    if (freed != order)                             /* someone holds it */
        return CELL_SYNC_ERROR_BUSY;

    uint32_t ng = ((uint32_t)freed << 16) | (uint16_t)(order + 1);
    if (atomic_compare_exchange_strong(&mutex->lock, &raw, sync_bswap32(ng))) {
        if (sync_trace()) fprintf(stderr, "[SYNC] TRYLOCK ea=0x%08X tid=%lu OK\n", _ea, SYNC_TID());
        return CELL_OK;
    }
    return CELL_SYNC_ERROR_BUSY;                    /* lost the race */
}

s32 cellSyncMutexUnlock(CellSyncMutex* mutex)
{
    unsigned int _ea = (unsigned int)(uintptr_t)mutex;
    mutex = GUEST_PTR(mutex, CellSyncMutex*);
    if (!mutex)
        return CELL_SYNC_ERROR_NULL_POINTER;

    if (sync_trace()) fprintf(stderr, "[SYNC] UNLOCK  ea=0x%08X tid=%lu\n", _ea, SYNC_TID());
    for (;;) {                                      /* m_freed++ */
        uint32_t raw = atomic_load(&mutex->lock);
        uint32_t g   = sync_bswap32(raw);
        uint32_t ng  = ((uint32_t)(uint16_t)((g >> 16) + 1) << 16) | (uint16_t)g;
        if (atomic_compare_exchange_weak(&mutex->lock, &raw, sync_bswap32(ng)))
            return CELL_OK;
    }
}

/* =========================================================================
 * Barrier
 * =====================================================================*/

s32 cellSyncBarrierInitialize(CellSyncBarrier* barrier, u16 totalCount)
{
    barrier = GUEST_PTR(barrier, CellSyncBarrier*);
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
    barrier = GUEST_PTR(barrier, CellSyncBarrier*);
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
    /* Same as notify for this implementation. Forward the GUEST pointer as it
     * arrived -- cellSyncBarrierNotify translates it, and translating here too
     * added vm_base twice. */
    return cellSyncBarrierNotify(barrier);
}

s32 cellSyncBarrierWait(CellSyncBarrier* barrier)
{
    barrier = GUEST_PTR(barrier, CellSyncBarrier*);
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
    barrier = GUEST_PTR(barrier, CellSyncBarrier*);
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
    rwm = GUEST_PTR(rwm, CellSyncRwm*);
    buffer = GUEST_PTR(buffer, void*);
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
    rwm = GUEST_PTR(rwm, CellSyncRwm*);
    dst = GUEST_PTR(dst, void*);
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
    rwm = GUEST_PTR(rwm, CellSyncRwm*);
    dst = GUEST_PTR(dst, void*);
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
    rwm = GUEST_PTR(rwm, CellSyncRwm*);
    src = GUEST_PTR(src, const void*);
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
    rwm = GUEST_PTR(rwm, CellSyncRwm*);
    src = GUEST_PTR(src, const void*);
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
    queue = GUEST_PTR(queue, CellSyncQueue*);
    buffer = GUEST_PTR(buffer, void*);
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
    queue = GUEST_PTR(queue, CellSyncQueue*);
    data = GUEST_PTR(data, const void*);
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
    queue = GUEST_PTR(queue, CellSyncQueue*);
    data = GUEST_PTR(data, const void*);
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
    queue = GUEST_PTR(queue, CellSyncQueue*);
    data = GUEST_PTR(data, void*);
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
    queue = GUEST_PTR(queue, CellSyncQueue*);
    data = GUEST_PTR(data, void*);
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
    queue = GUEST_PTR(queue, CellSyncQueue*);
    data = GUEST_PTR(data, void*);
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
    queue = GUEST_PTR(queue, CellSyncQueue*);
    size = GUEST_PTR(size, u32*);
    if (!queue || !size)
        return CELL_SYNC_ERROR_NULL_POINTER;

    *size = atomic_load(&queue->count);
    return CELL_OK;
}

s32 cellSyncQueueClear(CellSyncQueue* queue)
{
    queue = GUEST_PTR(queue, CellSyncQueue*);
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
    queue = GUEST_PTR(queue, CellSyncLFQueue*);
    buffer = GUEST_PTR(buffer, void*);
    eaSignal = GUEST_PTR(eaSignal, void*);
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
    queue = GUEST_PTR(queue, CellSyncLFQueue*);
    data = GUEST_PTR(data, const void*);
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
    queue = GUEST_PTR(queue, CellSyncLFQueue*);
    data = GUEST_PTR(data, const void*);
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
    queue = GUEST_PTR(queue, CellSyncLFQueue*);
    data = GUEST_PTR(data, void*);
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
    queue = GUEST_PTR(queue, CellSyncLFQueue*);
    data = GUEST_PTR(data, void*);
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
    queue = GUEST_PTR(queue, const CellSyncLFQueue*);
    dir = GUEST_PTR(dir, u32*);
    if (!queue || !dir)
        return CELL_SYNC_ERROR_NULL_POINTER;

    *dir = queue->direction;
    return CELL_OK;
}

s32 cellSyncLFQueueDepth(const CellSyncLFQueue* queue, u32* depth)
{
    queue = GUEST_PTR(queue, const CellSyncLFQueue*);
    depth = GUEST_PTR(depth, u32*);
    if (!queue || !depth)
        return CELL_SYNC_ERROR_NULL_POINTER;

    *depth = queue->depth;
    return CELL_OK;
}

s32 cellSyncLFQueueSize(CellSyncLFQueue* queue, u32* size)
{
    queue = GUEST_PTR(queue, CellSyncLFQueue*);
    size = GUEST_PTR(size, u32*);
    if (!queue || !size)
        return CELL_SYNC_ERROR_NULL_POINTER;

    *size = atomic_load(&queue->count);
    return CELL_OK;
}

s32 cellSyncLFQueueClear(CellSyncLFQueue* queue)
{
    queue = GUEST_PTR(queue, CellSyncLFQueue*);
    if (!queue)
        return CELL_SYNC_ERROR_NULL_POINTER;

    queue_spinlock_acquire(&queue->lock);
    atomic_store(&queue->head, 0);
    atomic_store(&queue->tail, 0);
    atomic_store(&queue->count, 0);
    queue_spinlock_release(&queue->lock);

    return CELL_OK;
}
