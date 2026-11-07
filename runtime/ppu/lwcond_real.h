/*
 * ps3recomp - host-backed sys_lwcond wait/signal (PS3_LWCOND_REAL=1)
 *
 * The core of the opt-in real lwcond in ppu_sysprx.cpp, kept free of the HLE
 * so tests/sync_stress can drive it on host threads. Header-only, C and C++:
 * the storage is `static`, so each translation unit that includes this owns
 * its own table. Only ppu_sysprx.cpp (and the stress test) may include it.
 *
 * One slot per lwcond, keyed by its guest address in an open-addressed table
 * (the same scheme as the lwmutex slots). A slot is a host lock, a host
 * condition variable, and an explicit FIFO of wait records, as in
 * runtime/syscalls/sys_cond.c: a signal marks a record, so a wakeup can
 * neither be lost nor stolen by a later waiter, and signal_to can name one
 * thread. The host CV is always broadcast; only marked records return.
 *
 * No lost wakeup: the waiter enqueues its record under the slot lock BEFORE
 * it lets go of the paired lwmutex, and a signaller takes the slot lock to
 * mark a record. A signal sent after the waiter released the mutex therefore
 * finds the record. Lock order is lwmutex -> slot lock; the waiter releases
 * the lwmutex while holding the slot lock (releasing never blocks) and
 * re-takes it only after dropping the slot lock.
 *
 * Fallback wake: an infinite wait gives up after `fallback_ms` without a
 * signal and reports LWC_FALLBACK, which the caller returns as CELL_OK -- a
 * spurious wakeup the guest's while(!predicate) loop absorbs. That is what
 * keeps a signal the HLE never sees (a guest path that bypasses
 * sysPrxForUser) from parking a thread forever. Timed waits are bounded
 * already and wait out their own timeout, so ETIMEDOUT keeps its meaning.
 */
#ifndef PS3RECOMP_LWCOND_REAL_H
#define PS3RECOMP_LWCOND_REAL_H

#include <stdint.h>
#include "../platform/win32_compat.h"

#define LWC_HASH 4096u
/* A destroyed lwcond leaves a tombstone: the open-addressed probe chain past
 * it must stay intact, but the slot may be reused by a later create. */
#define LWC_TOMBSTONE ((long)-1)

enum { LWC_SIGNALLED = 0, LWC_TIMEDOUT = 1, LWC_FALLBACK = 2 };

typedef struct lwc_waiter {
    struct lwc_waiter* next;
    uint32_t tid;          /* guest thread id, for signal_to */
    int signalled;         /* written under the slot lock */
} lwc_waiter;

typedef struct lwc_slot {
    volatile long addr;    /* guest EA of the sys_lwcond_t; 0 = free */
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE cv;
    lwc_waiter* waiters;   /* FIFO, under `lock` */
} lwc_slot;

static lwc_slot g_lwc[LWC_HASH];
static SRWLOCK g_lwc_tab = SRWLOCK_INIT;
static volatile long g_lwc_fallbacks = 0;   /* waits that ended with no signal */

static inline long long lwc_now_us(void)
{
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (long long)((c.QuadPart / freq.QuadPart) * 1000000ll +
                       (c.QuadPart % freq.QuadPart) * 1000000ll / freq.QuadPart);
}

/* The slot for `addr`; created on first use when `create`. NULL for addr 0,
 * a missing slot (create == 0) or a full table. Slots are never freed: a
 * waiter may still be on one when the guest destroys or recreates the lwcond.
 * Lookups go through the table lock, which also orders the slot's init
 * before its publication on weakly ordered hosts. */
/* `fresh` only for an unused slot. A tombstoned slot keeps its already
 * initialised lock and condition variable: re-initialising a live host lock is
 * undefined, and freeing the slot does not mean a stale waiter pointer cannot
 * still be about to enter it. */
static inline void lwc_slot_init(lwc_slot* s, uint32_t addr, int fresh)
{
    if (fresh) {
        InitializeCriticalSection(&s->lock);
        InitializeConditionVariable(&s->cv);
    }
    s->waiters = NULL;
    s->addr = (long)addr;
}

static inline lwc_slot* lwc_slot_get(uint32_t addr, int create)
{
    if (!addr) return NULL;
    lwc_slot* r = NULL;
    lwc_slot* tomb = NULL;
    uint32_t h = (addr * 2654435761u) & (LWC_HASH - 1);
    AcquireSRWLockExclusive(&g_lwc_tab);
    for (uint32_t i = 0; i < LWC_HASH; i++) {
        lwc_slot* s = &g_lwc[(h + i) & (LWC_HASH - 1)];
        const long cur = s->addr;
        if (cur != LWC_TOMBSTONE && (uint32_t)cur == addr) { r = s; break; }
        if (cur == 0) {
            if (create) {
                lwc_slot* use = tomb ? tomb : s;
                lwc_slot_init(use, addr, use->addr == 0);
                r = use;
            }
            break;
        }
        if (cur == LWC_TOMBSTONE && !tomb) tomb = s;
    }
    /* A table whose only free entries are tombstones still has room. */
    if (!r && create && tomb) { lwc_slot_init(tomb, addr, 0); r = tomb; }
    ReleaseSRWLockExclusive(&g_lwc_tab);
    return r;
}

/* Free the slot for `addr` if no thread is parked on it. Returns 1 when the
 * slot was released, 0 while a waiter (marked or not) is still on the queue,
 * -1 when there is no slot. The check and the tombstone happen under the slot
 * lock, so a destroy cannot free a slot a waiter has just enqueued on. */
static inline int lwc_slot_free(uint32_t addr)
{
    if (!addr) return -1;
    int rc = -1;
    uint32_t h = (addr * 2654435761u) & (LWC_HASH - 1);
    AcquireSRWLockExclusive(&g_lwc_tab);
    for (uint32_t i = 0; i < LWC_HASH; i++) {
        lwc_slot* s = &g_lwc[(h + i) & (LWC_HASH - 1)];
        const long cur = s->addr;
        if (cur != LWC_TOMBSTONE && (uint32_t)cur == addr) {
            EnterCriticalSection(&s->lock);
            if (s->waiters) rc = 0;
            else { s->addr = LWC_TOMBSTONE; rc = 1; }
            LeaveCriticalSection(&s->lock);
            break;
        }
        if (cur == 0) break;
    }
    ReleaseSRWLockExclusive(&g_lwc_tab);
    return rc;
}

/* Wait on `s` as guest thread `tid`. `unlock` releases the paired lwmutex
 * (called once, with the slot lock held) and `relock` re-takes it (called
 * once, after). timeout_us 0 waits forever, bounded by fallback_ms (0 = no
 * fallback). Returns LWC_SIGNALLED, LWC_TIMEDOUT or LWC_FALLBACK. */
static inline int lwc_wait(lwc_slot* s, uint32_t tid, uint64_t timeout_us, uint32_t fallback_ms,
                           void (*unlock)(void*), void (*relock)(void*), void* arg)
{
    lwc_waiter w;
    w.next = NULL; w.tid = tid; w.signalled = 0;
    EnterCriticalSection(&s->lock);
    lwc_waiter** tail = &s->waiters;
    while (*tail) tail = &(*tail)->next;
    *tail = &w;
    unlock(arg);

    if (timeout_us > (1ull << 48)) timeout_us = 1ull << 48;   /* ~9 years; no overflow below */
    const long long start = lwc_now_us();
    const long long deadline = timeout_us ? start + (long long)timeout_us : 0;
    const long long fb_deadline = (!timeout_us && fallback_ms) ? start + (long long)fallback_ms * 1000 : 0;
    int result = LWC_SIGNALLED;
    while (!w.signalled) {
        const long long until = deadline ? deadline : fb_deadline;
        DWORD ms = INFINITE;
        if (until) {
            const long long left = until - lwc_now_us();
            if (left <= 0) { result = deadline ? LWC_TIMEDOUT : LWC_FALLBACK; break; }
            const unsigned long long m = ((unsigned long long)left + 999) / 1000;
            ms = m > 0xFFFFFFFEull ? 0xFFFFFFFEu : (DWORD)m;
        }
        SleepConditionVariableCS(&s->cv, &s->lock, ms);
    }
    lwc_waiter** link = &s->waiters;
    while (*link != &w) link = &(*link)->next;
    *link = w.next;
    LeaveCriticalSection(&s->lock);
    if (result == LWC_FALLBACK) InterlockedIncrement(&g_lwc_fallbacks);
    relock(arg);
    return result;
}

/* Mark waiters on `s` and wake them: the oldest one, every one (`all`), or
 * the one whose tid is `to_tid` (nonzero). Returns how many were marked. */
static inline int lwc_signal(lwc_slot* s, int all, uint32_t to_tid)
{
    int n = 0;
    EnterCriticalSection(&s->lock);
    for (lwc_waiter* w = s->waiters; w; w = w->next) {
        if (w->signalled) continue;
        if (to_tid && w->tid != to_tid) continue;
        w->signalled = 1;
        n++;
        if (!all) break;
    }
    if (n) WakeAllConditionVariable(&s->cv);
    LeaveCriticalSection(&s->lock);
    return n;
}

/* Threads still parked on `s` (not yet marked). */
static inline int lwc_waiting(lwc_slot* s)
{
    int n = 0;
    EnterCriticalSection(&s->lock);
    for (lwc_waiter* w = s->waiters; w; w = w->next) n += !w->signalled;
    LeaveCriticalSection(&s->lock);
    return n;
}

#endif /* PS3RECOMP_LWCOND_REAL_H */
