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
#include <stddef.h>   /* offsetof */
#include "ps3emu/log_first.h"

/* Yield hint for spin-wait loops */
#ifdef _WIN32
#include <windows.h>
#define SYNC_YIELD() SwitchToThread()
#else
#include <sched.h>
#define SYNC_YIELD() sched_yield()
#endif

/* CellSyncMutex is 4 bytes and must be 4-byte aligned: every entry point
 * answers CELL_SYNC_ERROR_ALIGN for a misaligned one before touching it. A word
 * that straddles two aligned words is not one atomic for the SPU side either. */
#define SYNC_MUTEX_MISALIGNED(ea) (((ea) & 3u) != 0)

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

/* SYNC_WATCH=<hex ea>: log every HLE operation on that one mutex with its word
 * before the op, so a PPU re-initialise under an SPU waiter is visible. */
static uint32_t sync_watch(void){ static int64_t v=-2; if(v==-2){const char*e=getenv("SYNC_WATCH"); v=e?(int64_t)strtoul(e,0,16):-1;} return v<0?0u:(uint32_t)v; }
#define SYNC_WATCH_LOG(op, ea, m) do { if ((ea) && (ea) == sync_watch()) { \
        static int _c; if (PS3_LOG_FIRST(_c, 400)) fprintf(stderr, "[SYNCW] %s ea=0x%08X word=%08X tid=%lu\n", op, (ea), \
            sync_bswap32(atomic_load(&(m)->lock)), SYNC_TID()); } } while (0)

/* The mutex word is shared with SPU code that takes it with GETLLAR/PUTLLC.
 * A PUTLLC compares its snapshot and then copies the whole 128-byte line,
 * under the lock-line lock; a host CAS outside that lock can land between
 * the two and be overwritten -- a PPU unlock lost that way leaves m_freed one
 * short and the next SPU waiter spins forever (GH3: Havok's integrate task
 * on 0x110C1540, mid intro movie). Change the word the way ppu_stwcx32 does:
 * under the lock, then tell any reserving SPU its line moved. */
extern void spu_lockline_lock(void);
extern void spu_lockline_unlock(void);
extern int  spu_coh_is_reserved(uint32_t ea);
extern void spu_coh_notify_write(uint32_t ea);
static int sync_cas(CellSyncMutex* m, uint32_t ea, uint32_t* expected, uint32_t desired)
{
    spu_lockline_lock();
    int ok = atomic_compare_exchange_strong(&m->lock, expected, desired);
    if (ok && spu_coh_is_reserved(ea)) spu_coh_notify_write(ea);
    spu_lockline_unlock();
    return ok;
}

s32 cellSyncMutexInitialize(CellSyncMutex* mutex)
{
    unsigned int _ea = (unsigned int)(uintptr_t)mutex;
    mutex = GUEST_PTR(mutex, CellSyncMutex*);
    if (!mutex)
        return CELL_SYNC_ERROR_NULL_POINTER;
    if (SYNC_MUTEX_MISALIGNED(_ea))
        return CELL_SYNC_ERROR_ALIGN;

    SYNC_WATCH_LOG("INIT", _ea, mutex);
    spu_lockline_lock();
    atomic_store(&mutex->lock, 0);   /* m_freed = m_order = 0 -> free */
    if (spu_coh_is_reserved(_ea)) spu_coh_notify_write(_ea);
    spu_lockline_unlock();
    return CELL_OK;
}

s32 cellSyncMutexLock(CellSyncMutex* mutex)
{
    unsigned int _ea = (unsigned int)(uintptr_t)mutex;
    mutex = GUEST_PTR(mutex, CellSyncMutex*);
    if (!mutex)
        return CELL_SYNC_ERROR_NULL_POINTER;
    if (SYNC_MUTEX_MISALIGNED(_ea))
        return CELL_SYNC_ERROR_ALIGN;
    SYNC_WATCH_LOG("LOCK", _ea, mutex);

    /* Take a ticket. */
    uint32_t raw, g;
    uint16_t ticket;
    for (;;) {
        raw = atomic_load(&mutex->lock);
        g   = sync_bswap32(raw);
        ticket = (uint16_t)g;                       /* m_order */
        uint32_t ng = (g & 0xFFFF0000u) | (uint16_t)(ticket + 1);
        if (sync_cas(mutex, _ea, &raw, sync_bswap32(ng)))
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
    if (SYNC_MUTEX_MISALIGNED(_ea))
        return CELL_SYNC_ERROR_ALIGN;
    SYNC_WATCH_LOG("TRYLOCK", _ea, mutex);

    uint32_t raw = atomic_load(&mutex->lock);
    uint32_t g   = sync_bswap32(raw);
    uint16_t freed = (uint16_t)(g >> 16), order = (uint16_t)g;

    if (freed != order)                             /* someone holds it */
        return CELL_SYNC_ERROR_BUSY;

    uint32_t ng = ((uint32_t)freed << 16) | (uint16_t)(order + 1);
    if (sync_cas(mutex, _ea, &raw, sync_bswap32(ng))) {
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
    if (SYNC_MUTEX_MISALIGNED(_ea))
        return CELL_SYNC_ERROR_ALIGN;
    SYNC_WATCH_LOG("UNLOCK", _ea, mutex);

    if (sync_trace()) fprintf(stderr, "[SYNC] UNLOCK  ea=0x%08X tid=%lu\n", _ea, SYNC_TID());
    for (;;) {                                      /* m_freed++ */
        uint32_t raw = atomic_load(&mutex->lock);
        uint32_t g   = sync_bswap32(raw);
        uint32_t ng  = ((uint32_t)(uint16_t)((g >> 16) + 1) << 16) | (uint16_t)g;
        if (sync_cas(mutex, _ea, &raw, sync_bswap32(ng)))
            return CELL_OK;
    }
}

/* =========================================================================
 * Barrier (4-byte guest BE state machine: {u16 value, u16 count})
 *   Bit 15 of value (0x8000) is set when value reaches count (drain/wait phase)
 *   and cleared when the final waiter decrements the remaining count to 0.
 *   Verified byte-for-byte against stock RPCS3 libsre.sprx.
 * =====================================================================*/

s32 cellSyncBarrierInitialize(CellSyncBarrier* barrier, u16 totalCount)
{
    uint32_t ea = (uint32_t)(uintptr_t)barrier;
    if (!ea)
        return CELL_SYNC_ERROR_NULL_POINTER;
    if (ea & 3u)
        return CELL_SYNC_ERROR_ALIGN;
    if (totalCount == 0 || totalCount > 0x7FFFu)
        return CELL_SYNC_ERROR_INVAL;

    barrier = GUEST_PTR(barrier, CellSyncBarrier*);
    atomic_store(&barrier->word, sync_bswap32((uint32_t)totalCount));
    return CELL_OK;
}

s32 cellSyncBarrierNotify(CellSyncBarrier* barrier)
{
    uint32_t ea = (uint32_t)(uintptr_t)barrier;
    if (!ea)
        return CELL_SYNC_ERROR_NULL_POINTER;
    if (ea & 3u)
        return CELL_SYNC_ERROR_ALIGN;

    barrier = GUEST_PTR(barrier, CellSyncBarrier*);
    int spins = 0;
    for (;;) {
        uint32_t raw = atomic_load(&barrier->word);
        uint32_t g   = sync_bswap32(raw);
        uint16_t val = (uint16_t)(g >> 16);
        uint16_t cnt = (uint16_t)(g & 0xFFFFu);
        if (cnt == 0)
            return CELL_SYNC_ERROR_INVAL;
        if (val & 0x8000u) {
            if (++spins > 1000) { SYNC_YIELD(); spins = 0; }
            continue;
        }
        uint16_t next = (uint16_t)(val + 1u);
        if (next >= cnt)
            next = (uint16_t)(0x8000u | cnt);
        uint32_t ng = ((uint32_t)next << 16) | cnt;
        if (atomic_compare_exchange_weak(&barrier->word, &raw, sync_bswap32(ng)))
            return CELL_OK;
    }
}

s32 cellSyncBarrierTryNotify(CellSyncBarrier* barrier)
{
    uint32_t ea = (uint32_t)(uintptr_t)barrier;
    if (!ea)
        return CELL_SYNC_ERROR_NULL_POINTER;
    if (ea & 3u)
        return CELL_SYNC_ERROR_ALIGN;

    barrier = GUEST_PTR(barrier, CellSyncBarrier*);
    for (;;) {
        uint32_t raw = atomic_load(&barrier->word);
        uint32_t g   = sync_bswap32(raw);
        uint16_t val = (uint16_t)(g >> 16);
        uint16_t cnt = (uint16_t)(g & 0xFFFFu);
        if (cnt == 0)
            return CELL_SYNC_ERROR_INVAL;
        if (val & 0x8000u)
            return CELL_SYNC_ERROR_BUSY;
        uint16_t next = (uint16_t)(val + 1u);
        if (next >= cnt)
            next = (uint16_t)(0x8000u | cnt);
        uint32_t ng = ((uint32_t)next << 16) | cnt;
        if (atomic_compare_exchange_weak(&barrier->word, &raw, sync_bswap32(ng)))
            return CELL_OK;
    }
}

s32 cellSyncBarrierWait(CellSyncBarrier* barrier)
{
    uint32_t ea = (uint32_t)(uintptr_t)barrier;
    if (!ea)
        return CELL_SYNC_ERROR_NULL_POINTER;
    if (ea & 3u)
        return CELL_SYNC_ERROR_ALIGN;

    barrier = GUEST_PTR(barrier, CellSyncBarrier*);
    int spins = 0;
    for (;;) {
        uint32_t raw = atomic_load(&barrier->word);
        uint32_t g   = sync_bswap32(raw);
        uint16_t val = (uint16_t)(g >> 16);
        uint16_t cnt = (uint16_t)(g & 0xFFFFu);
        if (cnt == 0)
            return CELL_SYNC_ERROR_INVAL;
        if (!(val & 0x8000u)) {
            if (++spins > 1000) { SYNC_YIELD(); spins = 0; }
            continue;
        }
        uint16_t rem  = (uint16_t)((val & 0x7FFFu) - 1u);
        uint16_t next = rem ? (uint16_t)(0x8000u | rem) : 0u;
        uint32_t ng   = ((uint32_t)next << 16) | cnt;
        if (atomic_compare_exchange_weak(&barrier->word, &raw, sync_bswap32(ng)))
            return CELL_OK;
    }
}

s32 cellSyncBarrierTryWait(CellSyncBarrier* barrier)
{
    uint32_t ea = (uint32_t)(uintptr_t)barrier;
    if (!ea)
        return CELL_SYNC_ERROR_NULL_POINTER;
    if (ea & 3u)
        return CELL_SYNC_ERROR_ALIGN;

    barrier = GUEST_PTR(barrier, CellSyncBarrier*);
    for (;;) {
        uint32_t raw = atomic_load(&barrier->word);
        uint32_t g   = sync_bswap32(raw);
        uint16_t val = (uint16_t)(g >> 16);
        uint16_t cnt = (uint16_t)(g & 0xFFFFu);
        if (cnt == 0)
            return CELL_SYNC_ERROR_INVAL;
        if (!(val & 0x8000u))
            return CELL_SYNC_ERROR_BUSY;
        uint16_t rem  = (uint16_t)((val & 0x7FFFu) - 1u);
        uint16_t next = rem ? (uint16_t)(0x8000u | rem) : 0u;
        uint32_t ng   = ((uint32_t)next << 16) | cnt;
        if (atomic_compare_exchange_weak(&barrier->word, &raw, sync_bswap32(ng)))
            return CELL_OK;
    }
}

/* =========================================================================
 * Reader-Writer Memory (16-byte guest BE layout, verified against RPCS3)
 *   +0x00: BE u16 readers, BE u16 writers
 *   +0x04: BE u32 buffer_size (multiple of 128, <= 16384)
 *   +0x08: BE u64 ea_buffer (32-bit guest EA)
 * =====================================================================*/

static void rwm_release_reader(CellSyncRwm* rwm)
{
    for (;;) {
        uint32_t raw = atomic_load(&rwm->ctrl);
        uint32_t g   = sync_bswap32(raw);
        uint16_t rdr = (uint16_t)(g >> 16);
        uint16_t wrt = (uint16_t)(g & 0xFFFFu);
        uint32_t ng  = ((uint32_t)(uint16_t)(rdr - 1u) << 16) | wrt;
        if (atomic_compare_exchange_weak(&rwm->ctrl, &raw, sync_bswap32(ng)))
            return;
    }
}

static void rwm_release_writer(CellSyncRwm* rwm)
{
    for (;;) {
        uint32_t raw = atomic_load(&rwm->ctrl);
        uint32_t g   = sync_bswap32(raw);
        uint16_t rdr = (uint16_t)(g >> 16);
        uint32_t ng  = ((uint32_t)rdr << 16) | 0u;
        if (atomic_compare_exchange_weak(&rwm->ctrl, &raw, sync_bswap32(ng)))
            return;
    }
}

s32 cellSyncRwmInitialize(CellSyncRwm* rwm, void* buffer, u32 size)
{
    uint32_t rwm_ea = (uint32_t)(uintptr_t)rwm;
    uint32_t buf_ea = (uint32_t)(uintptr_t)buffer;
    if (!rwm_ea || !buf_ea)
        return CELL_SYNC_ERROR_NULL_POINTER;
    if ((rwm_ea & 15u) || (buf_ea & 127u))
        return CELL_SYNC_ERROR_ALIGN;
    if (size == 0 || (size & 127u) != 0 || size > 16384u)
        return CELL_SYNC_ERROR_INVAL;

    rwm = GUEST_PTR(rwm, CellSyncRwm*);
    atomic_store(&rwm->ctrl, 0u);
    rwm->size_be      = sync_bswap32(size);
    rwm->buffer_ea_be = ps3_bswap64((u64)buf_ea);

    return CELL_OK;
}

s32 cellSyncRwmRead(CellSyncRwm* rwm, void* dst)
{
    uint32_t rwm_ea = (uint32_t)(uintptr_t)rwm;
    uint32_t dst_ea = (uint32_t)(uintptr_t)dst;
    if (!rwm_ea || !dst_ea)
        return CELL_SYNC_ERROR_NULL_POINTER;

    rwm = GUEST_PTR(rwm, CellSyncRwm*);
    dst = GUEST_PTR(dst, void*);

    int spins = 0;
    for (;;) {
        uint32_t raw = atomic_load(&rwm->ctrl);
        uint32_t g   = sync_bswap32(raw);
        uint16_t rdr = (uint16_t)(g >> 16);
        uint16_t wrt = (uint16_t)(g & 0xFFFFu);
        if (wrt != 0) {
            if (++spins > 1000) { SYNC_YIELD(); spins = 0; }
            continue;
        }
        uint32_t ng = ((uint32_t)(uint16_t)(rdr + 1u) << 16) | 0u;
        if (atomic_compare_exchange_weak(&rwm->ctrl, &raw, sync_bswap32(ng)))
            break;
    }

    uint32_t buf_ea = (uint32_t)ps3_bswap64(rwm->buffer_ea_be);
    uint32_t sz     = sync_bswap32(rwm->size_be);
    memcpy(dst, vm_base + buf_ea, sz);
    rwm_release_reader(rwm);

    return CELL_OK;
}

s32 cellSyncRwmTryRead(CellSyncRwm* rwm, void* dst)
{
    uint32_t rwm_ea = (uint32_t)(uintptr_t)rwm;
    uint32_t dst_ea = (uint32_t)(uintptr_t)dst;
    if (!rwm_ea || !dst_ea)
        return CELL_SYNC_ERROR_NULL_POINTER;

    rwm = GUEST_PTR(rwm, CellSyncRwm*);
    dst = GUEST_PTR(dst, void*);

    for (;;) {
        uint32_t raw = atomic_load(&rwm->ctrl);
        uint32_t g   = sync_bswap32(raw);
        uint16_t rdr = (uint16_t)(g >> 16);
        uint16_t wrt = (uint16_t)(g & 0xFFFFu);
        if (wrt != 0)
            return CELL_SYNC_ERROR_BUSY;
        uint32_t ng = ((uint32_t)(uint16_t)(rdr + 1u) << 16) | 0u;
        if (atomic_compare_exchange_weak(&rwm->ctrl, &raw, sync_bswap32(ng)))
            break;
    }

    uint32_t buf_ea = (uint32_t)ps3_bswap64(rwm->buffer_ea_be);
    uint32_t sz     = sync_bswap32(rwm->size_be);
    memcpy(dst, vm_base + buf_ea, sz);
    rwm_release_reader(rwm);

    return CELL_OK;
}

s32 cellSyncRwmWrite(CellSyncRwm* rwm, const void* src)
{
    uint32_t rwm_ea = (uint32_t)(uintptr_t)rwm;
    uint32_t src_ea = (uint32_t)(uintptr_t)src;
    if (!rwm_ea || !src_ea)
        return CELL_SYNC_ERROR_NULL_POINTER;

    rwm = GUEST_PTR(rwm, CellSyncRwm*);
    src = GUEST_PTR(src, const void*);

    int spins = 0;
    for (;;) {
        uint32_t raw = atomic_load(&rwm->ctrl);
        uint32_t g   = sync_bswap32(raw);
        if (g != 0u) {
            if (++spins > 1000) { SYNC_YIELD(); spins = 0; }
            continue;
        }
        if (atomic_compare_exchange_weak(&rwm->ctrl, &raw, sync_bswap32(1u)))
            break;
    }

    uint32_t buf_ea = (uint32_t)ps3_bswap64(rwm->buffer_ea_be);
    uint32_t sz     = sync_bswap32(rwm->size_be);
    memcpy(vm_base + buf_ea, src, sz);
    rwm_release_writer(rwm);

    return CELL_OK;
}

s32 cellSyncRwmTryWrite(CellSyncRwm* rwm, const void* src)
{
    uint32_t rwm_ea = (uint32_t)(uintptr_t)rwm;
    uint32_t src_ea = (uint32_t)(uintptr_t)src;
    if (!rwm_ea || !src_ea)
        return CELL_SYNC_ERROR_NULL_POINTER;

    rwm = GUEST_PTR(rwm, CellSyncRwm*);
    src = GUEST_PTR(src, const void*);

    uint32_t expected = 0u;
    if (!atomic_compare_exchange_strong(&rwm->ctrl, &expected, sync_bswap32(1u)))
        return CELL_SYNC_ERROR_BUSY;

    uint32_t buf_ea = (uint32_t)ps3_bswap64(rwm->buffer_ea_be);
    uint32_t sz     = sync_bswap32(rwm->size_be);
    memcpy(vm_base + buf_ea, src, sz);
    rwm_release_writer(rwm);

    return CELL_OK;
}

/* =========================================================================
 * Bounded Queue
 *
 * CellSyncQueue lives in guest memory and SPU code (Sony's SPU-side libsync)
 * pushes into it with GETLLAR/PUTLLC on the same words, so the HLE has to use
 * the real big-endian layout -- an invented host struct means the SPU reads
 * size=0 and a garbage buffer pointer, and its pushes never reach the PPU.
 * GH3's job completions arrive exactly this way. Layout (32 bytes):
 *
 *   +0x00 u32  _pop:8  | next:24     next = slot the next push writes
 *   +0x04 u32  _push:8 | count:24    _pop/_push = op-in-progress flags
 *   +0x08 u32  size                   element size in bytes
 *   +0x0C u32  depth
 *   +0x10 u64  buffer                 guest EA
 *
 * ctrl updates run under the SPU lock-line lock and notify, so they are one
 * transaction against an SPU PUTLLC on that line.
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

static int sq_trace(void){ static int v=-1; if(v<0) v=getenv("SYNC_QTRACE")?1:0; return v; }

/* Apply op to ctrl atomically; op returns 0 to leave ctrl untouched. */
typedef int (*sq_op)(uint32_t* x0, uint32_t* x4, uint32_t depth, uint32_t* pos);

static int sq_apply(uint32_t q, sq_op op, uint32_t* pos)
{
    uint32_t depth = vm_read32(q + 0x0C);
    spu_lockline_lock();
    uint32_t x0 = vm_read32(q), x4 = vm_read32(q + 4);
    int ok = op(&x0, &x4, depth, pos);
    if (ok) {
        uint32_t raw[2] = { ps3_bswap32(x0), ps3_bswap32(x4) };
        memcpy(vm_ptr8(q), raw, 8);
        if (spu_coh_is_reserved(q)) spu_coh_notify_write(q);
    }
    spu_lockline_unlock();
    return ok;
}

static void sq_and(uint32_t q, uint32_t m0, uint32_t m4)
{
    spu_lockline_lock();
    uint32_t raw[2] = { ps3_bswap32(vm_read32(q) & m0), ps3_bswap32(vm_read32(q + 4) & m4) };
    memcpy(vm_ptr8(q), raw, 8);
    if (spu_coh_is_reserved(q)) spu_coh_notify_write(q);
    spu_lockline_unlock();
}

static int sq_push_begin(uint32_t* x0, uint32_t* x4, uint32_t depth, uint32_t* pos)
{
    uint32_t next = *x0 & 0xFFFFFF, pop = *x0 >> 24;
    uint32_t count = *x4 & 0xFFFFFF, push = *x4 >> 24;
    if (push || count + pop >= depth) return 0;
    *pos = next;
    next = next + 1 != depth ? next + 1 : 0;
    *x0 = (pop << 24) | next;
    *x4 = (1u << 24) | (count + 1);
    return 1;
}

static int sq_pop_begin(uint32_t* x0, uint32_t* x4, uint32_t depth, uint32_t* pos)
{
    uint32_t next = *x0 & 0xFFFFFF, pop = *x0 >> 24;
    uint32_t count = *x4 & 0xFFFFFF, push = *x4 >> 24;
    if (pop || count <= push) return 0;
    *pos = (next + depth - count) % depth;
    *x0 = (1u << 24) | next;
    *x4 = (push << 24) | (count - 1);
    return 1;
}

static int sq_peek_begin(uint32_t* x0, uint32_t* x4, uint32_t depth, uint32_t* pos)
{
    uint32_t next = *x0 & 0xFFFFFF, pop = *x0 >> 24;
    uint32_t count = *x4 & 0xFFFFFF, push = *x4 >> 24;
    if (pop || count <= push) return 0;
    *pos = (next + depth - count) % depth;
    *x0 = (1u << 24) | next;
    return 1;
}

static int sq_clear_1(uint32_t* x0, uint32_t* x4, uint32_t d, uint32_t* p)
{ (void)x4; (void)d; (void)p; if (*x0 >> 24) return 0; *x0 |= 1u << 24; return 1; }
static int sq_clear_2(uint32_t* x0, uint32_t* x4, uint32_t d, uint32_t* p)
{ (void)x0; (void)d; (void)p; if (*x4 >> 24) return 0; *x4 |= 1u << 24; return 1; }

static uint32_t sq_elem(uint32_t q, uint32_t pos)
{
    return (uint32_t)vm_read64(q + 0x10) + pos * vm_read32(q + 0x08);
}

s32 cellSyncQueueInitialize(CellSyncQueue* queue, void* buffer,
                            u32 size, u32 depth)
{
    uint32_t q = (uint32_t)(uintptr_t)queue, b = (uint32_t)(uintptr_t)buffer;
    if (!q) return CELL_SYNC_ERROR_NULL_POINTER;
    if (size && !b) return CELL_SYNC_ERROR_NULL_POINTER;
    if (!depth || size % 16) return CELL_SYNC_ERROR_INVAL;
    if (q % 32 || b % 16) return CELL_SYNC_ERROR_ALIGN;

    vm_write32(q + 0x08, size);
    vm_write32(q + 0x0C, depth);
    vm_write64(q + 0x10, b);
    vm_write64(q + 0x18, 0);
    vm_write64(q, 0);
    if (sq_trace()) fprintf(stderr, "[SYNCQ] init q=0x%08X buf=0x%08X size=%u depth=%u\n", q, b, size, depth);
    return CELL_OK;
}

s32 cellSyncQueueTryPush(CellSyncQueue* queue, const void* data)
{
    uint32_t q = (uint32_t)(uintptr_t)queue, d = (uint32_t)(uintptr_t)data;
    if (!q || !d) return CELL_SYNC_ERROR_NULL_POINTER;
    uint32_t pos;
    if (!sq_apply(q, sq_push_begin, &pos)) return CELL_SYNC_ERROR_BUSY;
    uint32_t n = vm_read32(q + 0x08);
    vm_memcpy_to(sq_elem(q, pos), vm_ptr8(d), n);
    sq_and(q, 0xFFFFFFFFu, 0x00FFFFFFu);            /* clear _push */
    if (sq_trace()) fprintf(stderr, "[SYNCQ] push q=0x%08X pos=%u\n", q, pos);
    return CELL_OK;
}

s32 cellSyncQueuePush(CellSyncQueue* queue, const void* data)
{
    s32 rc;
    int spins = 0;
    while ((rc = cellSyncQueueTryPush(queue, data)) == CELL_SYNC_ERROR_BUSY)
        if (++spins > 100) { SYNC_YIELD(); spins = 0; }
    return rc;
}

static s32 sq_take(CellSyncQueue* queue, void* data, sq_op op)
{
    uint32_t q = (uint32_t)(uintptr_t)queue, d = (uint32_t)(uintptr_t)data;
    if (!q || !d) return CELL_SYNC_ERROR_NULL_POINTER;
    uint32_t pos;
    if (!sq_apply(q, op, &pos)) return CELL_SYNC_ERROR_BUSY;
    uint32_t n = vm_read32(q + 0x08);
    vm_memcpy_to(d, vm_ptr8(sq_elem(q, pos)), n);
    sq_and(q, 0x00FFFFFFu, 0xFFFFFFFFu);            /* clear _pop */
    if (sq_trace()) fprintf(stderr, "[SYNCQ] %s q=0x%08X pos=%u\n",
                            op == sq_pop_begin ? "pop " : "peek", q, pos);
    return CELL_OK;
}

s32 cellSyncQueueTryPop(CellSyncQueue* queue, void* data) { return sq_take(queue, data, sq_pop_begin); }

s32 cellSyncQueuePop(CellSyncQueue* queue, void* data)
{
    s32 rc;
    int spins = 0;
    while ((rc = sq_take(queue, data, sq_pop_begin)) == CELL_SYNC_ERROR_BUSY)
        if (++spins > 100) { SYNC_YIELD(); spins = 0; }
    return rc;
}

s32 cellSyncQueuePeek(CellSyncQueue* queue, void* data)
{
    s32 rc;
    int spins = 0;
    while ((rc = sq_take(queue, data, sq_peek_begin)) == CELL_SYNC_ERROR_BUSY)
        if (++spins > 100) { SYNC_YIELD(); spins = 0; }
    return rc;
}

s32 cellSyncQueueSize(CellSyncQueue* queue, u32* size)
{
    uint32_t q = (uint32_t)(uintptr_t)queue;
    size = GUEST_PTR(size, u32*);
    if (!q || !size) return CELL_SYNC_ERROR_NULL_POINTER;
    *size = ps3_bswap32(vm_read32(q + 4) & 0xFFFFFF);   /* guest u32, BE */
    return CELL_OK;
}

s32 cellSyncQueueClear(CellSyncQueue* queue)
{
    uint32_t q = (uint32_t)(uintptr_t)queue, p;
    if (!q) return CELL_SYNC_ERROR_NULL_POINTER;
    while (!sq_apply(q, sq_clear_1, &p)) SYNC_YIELD();
    while (!sq_apply(q, sq_clear_2, &p)) SYNC_YIELD();
    sq_and(q, 0, 0);
    return CELL_OK;
}
/* =========================================================================
 * Lock-Free Queue
 *
 * Uses the same spinlock approach as bounded queue for simplicity.
 * A true lock-free implementation would use multi-word CAS.
 * =====================================================================*/

_Static_assert(sizeof(CellSyncLFQueue) == CELL_SYNC_LFQUEUE_SIZE,
               "CellSyncLFQueue must be one 128-byte line");
_Static_assert(offsetof(CellSyncLFQueue, size_be) == 0x10 &&
               offsetof(CellSyncLFQueue, depth_be) == 0x14 &&
               offsetof(CellSyncLFQueue, buffer_be) == 0x18 &&
               offsetof(CellSyncLFQueue, direction_be) == 0x24 &&
               offsetof(CellSyncLFQueue, head) == 0x40 &&
               offsetof(CellSyncLFQueue, eaSignal_be) == 0x70,
               "CellSyncLFQueue guest offsets");

static inline u32 lfq_depth(const CellSyncLFQueue* q) { return ps3_bswap32(q->depth_be); }
static inline u32 lfq_size(const CellSyncLFQueue* q)  { return ps3_bswap32(q->size_be); }
static inline u32 lfq_buf(const CellSyncLFQueue* q)   { return (u32)ps3_bswap64(q->buffer_be); }
static inline u32 lfq_direction(const CellSyncLFQueue* q) { return ps3_bswap32(q->direction_be); }

s32 cellSyncLFQueueInitialize(CellSyncLFQueue* queue, void* buffer,
                               u32 size, u32 depth, u32 direction,
                               void* eaSignal)
{
    const u32 q_ea   = (u32)(uintptr_t)queue;
    const u32 buf_ea = (u32)(uintptr_t)buffer;
    const u32 sig_ea = (u32)(uintptr_t)eaSignal;

    if (!q_ea || !buf_ea)
        return CELL_SYNC_ERROR_NULL_POINTER;
    /* Sony libsync.sprx (fw 4.93): queue+4 -> CELL_SYNC_ERROR_ALIGN. */
    if (q_ea & (CELL_SYNC_LFQUEUE_ALIGN - 1u))
        return CELL_SYNC_ERROR_ALIGN;
    if (size == 0 || depth == 0)
        return CELL_SYNC_ERROR_INVAL;

    queue  = GUEST_PTR(queue, CellSyncLFQueue*);
    buffer = GUEST_PTR(buffer, void*);

    /* Zeroed line + big-endian header, as Sony leaves it. */
    memset(queue, 0, CELL_SYNC_LFQUEUE_SIZE);
    queue->size_be      = ps3_bswap32(size);
    queue->depth_be     = ps3_bswap32(depth);
    queue->buffer_be    = ps3_bswap64((u64)buf_ea);
    queue->direction_be = ps3_bswap32(direction);
    queue->eaSignal_be  = ps3_bswap64((u64)sig_ea);
    atomic_store(&queue->head, 0);
    atomic_store(&queue->tail, 0);
    atomic_store(&queue->count, 0);
    atomic_store(&queue->lock, 0);

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

    for (;;) {
        int spins = 0;
        while (atomic_load(&queue->count) >= lfq_depth(queue)) {
            if (++spins > 1000) { SYNC_YIELD(); spins = 0; }
        }

        queue_spinlock_acquire(&queue->lock);

        if (atomic_load(&queue->count) < lfq_depth(queue))
            break;

        queue_spinlock_release(&queue->lock);
    }

    u32 tail = atomic_load(&queue->tail);
    memcpy(vm_base + lfq_buf(queue) + (size_t)tail * lfq_size(queue),
           data, lfq_size(queue));
    atomic_store(&queue->tail, (tail + 1) % lfq_depth(queue));
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

    if (atomic_load(&queue->count) >= lfq_depth(queue))
        return CELL_SYNC_ERROR_OVERFLOW;

    unsigned int expected = 0;
    if (!atomic_compare_exchange_strong(&queue->lock, &expected, 1))
        return CELL_SYNC_ERROR_BUSY;

    if (atomic_load(&queue->count) >= lfq_depth(queue)) {
        atomic_store(&queue->lock, 0);
        return CELL_SYNC_ERROR_OVERFLOW;
    }

    u32 tail = atomic_load(&queue->tail);
    memcpy(vm_base + lfq_buf(queue) + (size_t)tail * lfq_size(queue),
           data, lfq_size(queue));
    atomic_store(&queue->tail, (tail + 1) % lfq_depth(queue));
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

    for (;;) {
        int spins = 0;
        while (atomic_load(&queue->count) == 0) {
            if (++spins > 1000) { SYNC_YIELD(); spins = 0; }
        }

        queue_spinlock_acquire(&queue->lock);

        if (atomic_load(&queue->count) > 0)
            break;

        queue_spinlock_release(&queue->lock);
    }

    u32 head = atomic_load(&queue->head);
    memcpy(data, vm_base + lfq_buf(queue) + (size_t)head * lfq_size(queue),
           lfq_size(queue));
    atomic_store(&queue->head, (head + 1) % lfq_depth(queue));
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
    memcpy(data, vm_base + lfq_buf(queue) + (size_t)head * lfq_size(queue),
           lfq_size(queue));
    atomic_store(&queue->head, (head + 1) % lfq_depth(queue));
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

    *dir = ps3_bswap32(lfq_direction(queue));            /* host dir -> BE out-param */
    return CELL_OK;
}

s32 cellSyncLFQueueDepth(const CellSyncLFQueue* queue, u32* depth)
{
    queue = GUEST_PTR(queue, const CellSyncLFQueue*);
    depth = GUEST_PTR(depth, u32*);
    if (!queue || !depth)
        return CELL_SYNC_ERROR_NULL_POINTER;

    *depth = ps3_bswap32(lfq_depth(queue));              /* host depth -> BE out-param */
    return CELL_OK;
}

s32 cellSyncLFQueueSize(CellSyncLFQueue* queue, u32* size)
{
    queue = GUEST_PTR(queue, CellSyncLFQueue*);
    size = GUEST_PTR(size, u32*);
    if (!queue || !size)
        return CELL_SYNC_ERROR_NULL_POINTER;

    *size = ps3_bswap32(atomic_load(&queue->count));
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
