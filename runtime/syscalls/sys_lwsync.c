/*
 * ps3recomp - the kernel half of lightweight mutexes and condition variables:
 * syscalls 95-99 and 117 (_sys_lwmutex_*) and 111-116 (_sys_lwcond_*).
 * RPCS3's lv2 (sys_lwmutex.cpp, sys_lwcond.cpp) is the reference.
 *
 * liblv2 keeps an lwmutex's lock word in guest memory and enters the kernel
 * only when it is contended. The kernel object is a sleep queue plus a
 * "signaled" word: an unlock with nobody asleep leaves a signal for the next
 * locker to consume (bit 0 for an ordinary unlock, the sign bit for
 * _sys_lwmutex_unlock2, whose consumer is told EBUSY). An lwcond is another
 * sleep queue whose waiters, when signalled while the mutex is held, move to
 * the mutex's queue and wake owning it.
 *
 * One lock covers every object of both kinds, so moving a waiter between a
 * condition's queue and a mutex's is atomic. Each sleeping thread waits on its
 * own condition variable and is woken by whoever dequeues it, with the result
 * it is to return already set.
 */
#include "lv2_syscall_table.h"
#include "../../include/ps3emu/error_codes.h"
#include "../platform/win32_compat.h"
#include <limits.h>
#include <string.h>

extern uint8_t* vm_base;
int32_t ppu_thread_priority_of(uint64_t tid);   /* sys_ppu_thread.c */

#define SYNC_FIFO     1
#define SYNC_PRIORITY 2
#define SYNC_RETRY    3

typedef struct waiter {
    struct waiter*     next;
    uint64_t           tid;
    uint64_t           order;
    int32_t            ret;
    int                woken;
    CONDITION_VARIABLE cv;
} waiter;

typedef struct {
    int      in_use;
    uint32_t id;
    uint32_t protocol;
    int32_t  signaled;
    int32_t  lwcond_waiters;   /* sign bit: a destroy is waiting for these */
    waiter*  sq;
} lwmutex;

typedef struct {
    int      in_use;
    uint32_t id;
    uint32_t protocol;
    uint32_t lwmutex_id;
    int32_t  lwmutex_waiters;  /* sign bit: a destroy is waiting for these */
    waiter*  sq;
} lwcond;

#define LW_MAX 1024
static lwmutex s_mtx[LW_MAX];
static lwcond  s_cnd[LW_MAX];
static SRWLOCK s_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_destroy_cv = CONDITION_VARIABLE_INIT;
static uint64_t s_order;
static uint32_t s_next_mtx = 1, s_next_cnd = 1;

static void wr32(uint32_t ea, uint32_t v) { uint8_t* p = vm_base + ea; p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static lwmutex* find_mtx(uint32_t id)
{
    if ((id >> 24) != 0x95) return NULL;
    lwmutex* m = &s_mtx[(id & 0xFFFF) % LW_MAX];
    return m->in_use && m->id == id ? m : NULL;
}

static lwcond* find_cnd(uint32_t id)
{
    if ((id >> 24) != 0x97) return NULL;
    lwcond* c = &s_cnd[(id & 0xFFFF) % LW_MAX];
    return c->in_use && c->id == id ? c : NULL;
}

static void enqueue(waiter** q, waiter* w)
{
    w->order = ++s_order;
    w->next = NULL;
    while (*q) q = &(*q)->next;
    *q = w;
}

static int unqueue(waiter** q, waiter* w)
{
    for (; *q; q = &(*q)->next)
        if (*q == w) { *q = w->next; w->next = NULL; return 1; }
    return 0;
}

/* Take the next waiter by protocol: the oldest (FIFO), or the highest
 * priority -- lowest number -- oldest first among equals. */
static waiter* schedule(waiter** q, uint32_t protocol)
{
    if (!*q) return NULL;
    waiter* best = *q;
    if (protocol == SYNC_PRIORITY) {
        int32_t bp = ppu_thread_priority_of(best->tid);
        for (waiter* w = best->next; w; w = w->next) {
            const int32_t p = ppu_thread_priority_of(w->tid);
            if (p < bp) { best = w; bp = p; }
        }
    }
    unqueue(q, best);
    return best;
}

static void awake(waiter* w)
{
    w->woken = 1;
    WakeConditionVariable(&w->cv);
}

/* Sleep until woken, or until `timeout_us` passes (0 = forever). On a timeout
 * the caller dequeues the waiter if it is still queued. Returns 1 if woken. */
static int sleep_on(waiter* w, uint64_t timeout_us)
{
    if (!timeout_us) {
        while (!w->woken) SleepConditionVariableSRW(&w->cv, &s_lock, INFINITE, 0);
        return 1;
    }
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    while (!w->woken) {
        QueryPerformanceCounter(&t);
        const uint64_t el = (uint64_t)(t.QuadPart - t0.QuadPart) * 1000000ull / (uint64_t)f.QuadPart;
        if (el >= timeout_us) return 0;
        const uint64_t ms = (timeout_us - el + 999) / 1000;
        SleepConditionVariableSRW(&w->cv, &s_lock, (DWORD)(ms ? ms : 1), 0);
    }
    return 1;
}

static void waiter_init(waiter* w, ppu_context* ctx)
{
    memset(w, 0, sizeof *w);
    w->tid = ctx->thread_id;
    InitializeConditionVariable(&w->cv);
}

/* lv2_lwmutex::try_own: consume a pending signal, or queue `w`. */
static int32_t mtx_try_own(lwmutex* m, waiter* w)
{
    if (m->signaled) {
        const int32_t s = m->signaled;
        m->signaled = 0;
        return s;
    }
    enqueue(&m->sq, w);
    if ((uint32_t)m->lwcond_waiters > 0x80000000u) {
        /* a destroy waits on lwcond waiters; a new mutex waiter ends that wait */
        m->lwcond_waiters &= 0x7FFFFFFF;
        WakeAllConditionVariable(&s_destroy_cv);
    }
    return 0;
}

/* lv2_lwmutex::reown: hand the mutex to the next waiter, or leave a signal. */
static waiter* mtx_reown(lwmutex* m, int unlock2)
{
    waiter* w = schedule(&m->sq, m->protocol);
    if (!w) m->signaled |= unlock2 ? INT_MIN : 1;
    return w;
}

/* _sys_lwmutex_create(u32* id, u32 protocol, sys_lwmutex_t* control, s32 has_name, u64 name) */
static int64_t sys_lwmutex_create(ppu_context* ctx)
{
    const uint32_t out = LV2_ARG_PTR(ctx, 0), protocol = LV2_ARG_U32(ctx, 1);
    if (protocol != SYNC_FIFO && protocol != SYNC_RETRY && protocol != SYNC_PRIORITY)
        return (int32_t)CELL_EINVAL;
    AcquireSRWLockExclusive(&s_lock);
    for (int i = 0; i < LW_MAX; i++) {
        const uint32_t n = s_next_mtx++;
        lwmutex* m = &s_mtx[n % LW_MAX];
        if (m->in_use) continue;
        memset(m, 0, sizeof *m);
        m->in_use = 1;
        m->id = 0x95000000u | (n & 0xFFFF);
        m->protocol = protocol;
        ReleaseSRWLockExclusive(&s_lock);
        wr32(out, m->id);
        return CELL_OK;
    }
    ReleaseSRWLockExclusive(&s_lock);
    return (int32_t)CELL_EAGAIN;
}

/* _sys_lwmutex_destroy(id): EBUSY with sleepers; waits out lwcond waiters. */
static int64_t sys_lwmutex_destroy(ppu_context* ctx)
{
    const uint32_t id = LV2_ARG_U32(ctx, 0);
    AcquireSRWLockExclusive(&s_lock);
    for (;;) {
        lwmutex* m = find_mtx(id);
        if (!m) { ReleaseSRWLockExclusive(&s_lock); return (int32_t)CELL_ESRCH; }
        if (m->sq) { ReleaseSRWLockExclusive(&s_lock); return (int32_t)CELL_EBUSY; }
        m->lwcond_waiters |= INT_MIN;
        if (m->lwcond_waiters == INT_MIN) {
            m->in_use = 0;
            ReleaseSRWLockExclusive(&s_lock);
            return CELL_OK;
        }
        while ((uint32_t)m->lwcond_waiters > 0x80000000u)
            SleepConditionVariableSRW(&s_destroy_cv, &s_lock, INFINITE, 0);
    }
}

/* _sys_lwmutex_lock(id, u64 timeout_us) */
static int64_t sys_lwmutex_lock(ppu_context* ctx)
{
    const uint32_t id = LV2_ARG_U32(ctx, 0);
    const uint64_t timeout = LV2_ARG_U64(ctx, 1);
    waiter w;
    waiter_init(&w, ctx);
    AcquireSRWLockExclusive(&s_lock);
    lwmutex* m = find_mtx(id);
    if (!m) { ReleaseSRWLockExclusive(&s_lock); return (int32_t)CELL_ESRCH; }
    const int32_t s = mtx_try_own(m, &w);
    if (s) {
        ReleaseSRWLockExclusive(&s_lock);
        return (s & 1) ? CELL_OK : (int32_t)CELL_EBUSY;
    }
    if (!sleep_on(&w, timeout) && unqueue(&m->sq, &w))
        w.ret = (int32_t)CELL_ETIMEDOUT;
    ReleaseSRWLockExclusive(&s_lock);
    return w.ret;
}

/* _sys_lwmutex_trylock(id): only an ordinary unlock's signal is taken. */
static int64_t sys_lwmutex_trylock(ppu_context* ctx)
{
    AcquireSRWLockExclusive(&s_lock);
    lwmutex* m = find_mtx(LV2_ARG_U32(ctx, 0));
    int64_t r = CELL_OK;
    if (!m) r = (int32_t)CELL_ESRCH;
    else if (m->signaled & 1) m->signaled = 0;
    else r = (int32_t)CELL_EBUSY;
    ReleaseSRWLockExclusive(&s_lock);
    return r;
}

static int64_t lwmutex_unlock(ppu_context* ctx, int unlock2)
{
    AcquireSRWLockExclusive(&s_lock);
    lwmutex* m = find_mtx(LV2_ARG_U32(ctx, 0));
    if (!m) { ReleaseSRWLockExclusive(&s_lock); return (int32_t)CELL_ESRCH; }
    waiter* w = mtx_reown(m, unlock2);
    if (w) {
        if (unlock2) w->ret = (int32_t)CELL_EBUSY;
        awake(w);
    }
    ReleaseSRWLockExclusive(&s_lock);
    return CELL_OK;
}

static int64_t sys_lwmutex_unlock(ppu_context* ctx)  { return lwmutex_unlock(ctx, 0); }
static int64_t sys_lwmutex_unlock2(ppu_context* ctx) { return lwmutex_unlock(ctx, 1); }

/* _sys_lwcond_create(u32* id, u32 lwmutex_id, sys_lwcond_t* control, u64 name) */
static int64_t sys_lwcond_create(ppu_context* ctx)
{
    const uint32_t out = LV2_ARG_PTR(ctx, 0), mid = LV2_ARG_U32(ctx, 1);
    AcquireSRWLockExclusive(&s_lock);
    lwmutex* m = find_mtx(mid);
    if (!m) { ReleaseSRWLockExclusive(&s_lock); return (int32_t)CELL_ESRCH; }
    const uint32_t protocol = m->protocol == SYNC_RETRY ? SYNC_PRIORITY : m->protocol;
    for (int i = 0; i < LW_MAX; i++) {
        const uint32_t n = s_next_cnd++;
        lwcond* c = &s_cnd[n % LW_MAX];
        if (c->in_use) continue;
        memset(c, 0, sizeof *c);
        c->in_use = 1;
        c->id = 0x97000000u | (n & 0xFFFF);
        c->protocol = protocol;
        c->lwmutex_id = mid;
        ReleaseSRWLockExclusive(&s_lock);
        wr32(out, c->id);
        return CELL_OK;
    }
    ReleaseSRWLockExclusive(&s_lock);
    return (int32_t)CELL_EAGAIN;
}

/* _sys_lwcond_destroy(id): EBUSY with sleepers; waits out threads still
 * inside _sys_lwcond_queue_wait on their way back to the mutex. */
static int64_t sys_lwcond_destroy(ppu_context* ctx)
{
    const uint32_t id = LV2_ARG_U32(ctx, 0);
    AcquireSRWLockExclusive(&s_lock);
    for (;;) {
        lwcond* c = find_cnd(id);
        if (!c) { ReleaseSRWLockExclusive(&s_lock); return (int32_t)CELL_ESRCH; }
        if (c->sq) { ReleaseSRWLockExclusive(&s_lock); return (int32_t)CELL_EBUSY; }
        c->lwmutex_waiters |= INT_MIN;
        if (c->lwmutex_waiters == INT_MIN) {
            c->in_use = 0;
            ReleaseSRWLockExclusive(&s_lock);
            return CELL_OK;
        }
        while ((uint32_t)c->lwmutex_waiters > 0x80000000u)
            SleepConditionVariableSRW(&s_destroy_cv, &s_lock, INFINITE, 0);
    }
}

/* _sys_lwcond_signal(id, lwmutex_id, u64 ppu_thread_id, u32 mode)
 *   mode 1: the caller holds the mutex -- the woken thread moves to the
 *           mutex's queue and wakes when it is unlocked;
 *   mode 2: the caller does not hold it -- the thread wakes with EBUSY;
 *   mode 3: the mutex was taken by force -- the thread wakes owning it, or
 *           queues behind the mutex's sleepers.
 * ppu_thread_id ~0 picks by protocol; otherwise that thread, if waiting.
 * Nobody to wake: mode 1 EPERM, mode 2 OK, mode 3 ENOENT (a named thread
 * not waiting: EPERM). */
static int64_t sys_lwcond_signal(ppu_context* ctx)
{
    const uint32_t id = LV2_ARG_U32(ctx, 0), mid = LV2_ARG_U32(ctx, 1);
    const uint32_t tid = (uint32_t)LV2_ARG_U64(ctx, 2), mode = LV2_ARG_U32(ctx, 3);
    if (mode < 1 || mode > 3) return (int32_t)CELL_EINVAL;
    AcquireSRWLockExclusive(&s_lock);
    lwcond* c = find_cnd(id);
    lwmutex* m = mode != 2 ? find_mtx(mid) : NULL;
    if (!c || (mode != 2 && !m) || (tid != 0xFFFFFFFFu && ppu_thread_priority_of(tid) == INT_MIN)) {
        ReleaseSRWLockExclusive(&s_lock);
        return (int32_t)CELL_ESRCH;
    }
    waiter* w = NULL;
    if (tid != 0xFFFFFFFFu) {
        for (waiter* x = c->sq; x; x = x->next)
            if (x->tid == tid) { w = x; break; }
        if (w) unqueue(&c->sq, w);
    } else {
        w = schedule(&c->sq, c->protocol);
    }
    if (!w) {
        ReleaseSRWLockExclusive(&s_lock);
        if (tid != 0xFFFFFFFFu) return (int32_t)CELL_EPERM;
        return mode == 3 ? (int32_t)CELL_ENOENT : mode == 2 ? CELL_OK : (int32_t)CELL_EPERM;
    }
    if (mode == 2) {
        w->ret = (int32_t)CELL_EBUSY;
        awake(w);
    } else if (mode == 3 && m->sq) {
        enqueue(&m->sq, w);                 /* behind the mutex's sleepers */
        waiter* w2 = mtx_reown(m, 0);
        if (w2) awake(w2);
    } else if (mode == 1) {
        enqueue(&m->sq, w);
    } else {
        awake(w);
    }
    ReleaseSRWLockExclusive(&s_lock);
    return CELL_OK;
}

/* _sys_lwcond_signal_all(id, lwmutex_id, u32 mode): as signal, for every
 * waiter in protocol order; mode 1 returns how many moved. */
static int64_t sys_lwcond_signal_all(ppu_context* ctx)
{
    const uint32_t id = LV2_ARG_U32(ctx, 0), mid = LV2_ARG_U32(ctx, 1), mode = LV2_ARG_U32(ctx, 2);
    if (mode < 1 || mode > 2) return (int32_t)CELL_EINVAL;
    AcquireSRWLockExclusive(&s_lock);
    lwcond* c = find_cnd(id);
    lwmutex* m = mode == 1 ? find_mtx(mid) : NULL;
    if (!c || (mode == 1 && !m)) { ReleaseSRWLockExclusive(&s_lock); return (int32_t)CELL_ESRCH; }
    int32_t n = 0;
    for (waiter* w; (w = schedule(&c->sq, c->protocol)); n++) {
        if (mode == 2) { w->ret = (int32_t)CELL_EBUSY; awake(w); }
        else enqueue(&m->sq, w);
    }
    ReleaseSRWLockExclusive(&s_lock);
    return mode == 1 ? n : CELL_OK;
}

/* _sys_lwcond_queue_wait(id, lwmutex_id, u64 timeout_us): queue on the
 * condition and release the mutex (handing it to its next sleeper, or leaving
 * a signal), then sleep until signalled -- woken owning the mutex, or with
 * EBUSY if signalled in mode 2 -- or until the timeout (ETIMEDOUT). */
static int64_t sys_lwcond_queue_wait(ppu_context* ctx)
{
    const uint32_t id = LV2_ARG_U32(ctx, 0), mid = LV2_ARG_U32(ctx, 1);
    const uint64_t timeout = LV2_ARG_U64(ctx, 2);
    waiter w;
    waiter_init(&w, ctx);
    AcquireSRWLockExclusive(&s_lock);
    lwcond* c = find_cnd(id);
    lwmutex* m = find_mtx(mid);
    if (!c || !m) { ReleaseSRWLockExclusive(&s_lock); return (int32_t)CELL_ESRCH; }
    m->lwcond_waiters++;
    c->lwmutex_waiters++;
    enqueue(&c->sq, &w);
    if (!m->sq) m->signaled |= 1;
    else { waiter* w2 = mtx_reown(m, 0); if (w2) awake(w2); }

    if (!sleep_on(&w, timeout) && (unqueue(&c->sq, &w) || unqueue(&m->sq, &w)))
        w.ret = (int32_t)CELL_ETIMEDOUT;

    if (--m->lwcond_waiters == INT_MIN) WakeAllConditionVariable(&s_destroy_cv);
    if (--c->lwmutex_waiters == INT_MIN) WakeAllConditionVariable(&s_destroy_cv);
    ReleaseSRWLockExclusive(&s_lock);
    return w.ret;
}

void sys_lwsync_register(lv2_syscall_table* tbl)
{
    lv2_syscall_register(tbl, 95,  sys_lwmutex_create);
    lv2_syscall_register(tbl, 96,  sys_lwmutex_destroy);
    lv2_syscall_register(tbl, 97,  sys_lwmutex_lock);
    lv2_syscall_register(tbl, 98,  sys_lwmutex_unlock);
    lv2_syscall_register(tbl, 99,  sys_lwmutex_trylock);
    lv2_syscall_register(tbl, 117, sys_lwmutex_unlock2);
    lv2_syscall_register(tbl, 111, sys_lwcond_create);
    lv2_syscall_register(tbl, 112, sys_lwcond_destroy);
    lv2_syscall_register(tbl, 113, sys_lwcond_queue_wait);
    lv2_syscall_register(tbl, 115, sys_lwcond_signal);
    lv2_syscall_register(tbl, 116, sys_lwcond_signal_all);
}
