/*
 * ps3recomp - Kernel mutex syscalls (implementation)
 */

#include "sys_mutex.h"
#include "../platform/darwin_compat.h"
#include "sys_timer.h"   /* lv2_usec_deadline: sub-ms timed waits */
#include "../memory/vm.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>   /* getenv */
#ifndef _WIN32
#include <unistd.h>   /* sleep: MUTEX_WATCH=2 reporter */
#endif

/* ---------------------------------------------------------------------------
 * Globals
 * -----------------------------------------------------------------------*/
sys_mutex_info g_sys_mutexes[SYS_MUTEX_MAX];

#ifdef _WIN32
static CRITICAL_SECTION s_mtx_table_lock;
static int              s_mtx_table_lock_init = 0;
#else
static pthread_mutex_t  s_mtx_table_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static void mtx_table_lock(void)
{
#ifdef _WIN32
    if (!s_mtx_table_lock_init) {
        InitializeCriticalSection(&s_mtx_table_lock);
        s_mtx_table_lock_init = 1;
    }
    EnterCriticalSection(&s_mtx_table_lock);
#else
    pthread_mutex_lock(&s_mtx_table_lock);
#endif
}

static void mtx_table_unlock(void)
{
#ifdef _WIN32
    LeaveCriticalSection(&s_mtx_table_lock);
#else
    pthread_mutex_unlock(&s_mtx_table_lock);
#endif
}

static int mtx_find_free(void)
{
    for (int i = 0; i < SYS_MUTEX_MAX; i++) {
        if (!g_sys_mutexes[i].active)
            return i;
    }
    return -1;
}

/* Byteswap helper for writing u32 to guest memory */
static void write_be32(uint32_t addr, uint32_t val)
{
    uint32_t* p = (uint32_t*)vm_to_host(addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    val = ((val >> 24) & 0xFF) | ((val >> 8) & 0xFF00) |
          ((val <<  8) & 0xFF0000) | ((val << 24) & 0xFF000000u);
#endif
    *p = val;
}

/* ---------------------------------------------------------------------------
 * sys_mutex_create
 *
 * r3 = pointer to receive mutex ID (u32*)
 * r4 = pointer to attribute struct
 * -----------------------------------------------------------------------*/
int64_t sys_mutex_create(ppu_context* ctx)
{
    uint32_t id_out_addr = LV2_ARG_PTR(ctx, 0);
    uint32_t attr_addr   = LV2_ARG_PTR(ctx, 1);

    mtx_table_lock();

    int slot = mtx_find_free();
    if (slot < 0) {
        mtx_table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }

    sys_mutex_info* m = &g_sys_mutexes[slot];
    memset(m, 0, sizeof(*m));
    m->active = 1;

    /* Read attributes from guest memory */
    if (attr_addr != 0) {
        uint8_t* attr_raw = (uint8_t*)vm_to_host(attr_addr);
        /* protocol is at offset 0 (u32 BE) */
        uint32_t proto_be;
        memcpy(&proto_be, attr_raw + 0, 4);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        proto_be = ((proto_be >> 24) & 0xFF) | ((proto_be >> 8) & 0xFF00) |
                   ((proto_be << 8) & 0xFF0000) | ((proto_be << 24) & 0xFF000000u);
#endif
        m->protocol = proto_be;

        /* recursive flag at offset 4 (u32 BE) */
        uint32_t rec_be;
        memcpy(&rec_be, attr_raw + 4, 4);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        rec_be = ((rec_be >> 24) & 0xFF) | ((rec_be >> 8) & 0xFF00) |
                 ((rec_be << 8) & 0xFF0000) | ((rec_be << 24) & 0xFF000000u);
#endif
        m->recursive = (rec_be == SYS_SYNC_RECURSIVE) ? 1 : 0;

        /* name at offset 24 */
        memcpy(m->name, attr_raw + 24, 8);
    }

    /* Initialize host mutex */
#ifdef _WIN32
    InitializeCriticalSection(&m->cs);
    m->wait_handle = CreateEventA(NULL, FALSE, TRUE, NULL);
#else
    {
        pthread_mutexattr_t mattr;
        pthread_mutexattr_init(&mattr);
        if (m->recursive) {
            pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
        } else {
            pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_ERRORCHECK);
        }
        pthread_mutex_init(&m->mtx, &mattr);
        pthread_mutexattr_destroy(&mattr);
    }
#endif

    /* Write mutex ID (slot + 1) to guest */
    uint32_t mutex_id = (uint32_t)(slot + 1);
    if (id_out_addr != 0) {
        write_be32(id_out_addr, mutex_id);
    }

    mtx_table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_mutex_destroy
 *
 * r3 = mutex_id
 * -----------------------------------------------------------------------*/
int64_t sys_mutex_destroy(ppu_context* ctx)
{
    uint32_t mutex_id = LV2_ARG_U32(ctx, 0);

    if (mutex_id == 0 || mutex_id > SYS_MUTEX_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    mtx_table_lock();

    sys_mutex_info* m = &g_sys_mutexes[mutex_id - 1];
    if (!m->active) {
        mtx_table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    /* lv2 refuses to destroy a mutex that is held or has waiters (EBUSY).
     * Destroying it anyway freed the host mutex under a thread blocked on it,
     * and the next sys_mutex_create in the same slot initialised a new one
     * over it: everyone else then used the new mutex and the blocked thread
     * waited on the dead one for good. */
    if (m->lock_count > 0 || __atomic_load_n(&m->waiters, __ATOMIC_SEQ_CST) > 0) {
        static int n = 0;
        if (n++ < 16)
            fprintf(stderr, "[mutex] destroy of mutex %u '%.8s' refused: owner tid=%llu "
                    "count=%d waiters=%d (EBUSY)\n", mutex_id, m->name,
                    (unsigned long long)m->owner_tid, m->lock_count,
                    __atomic_load_n(&m->waiters, __ATOMIC_SEQ_CST));
        mtx_table_unlock();
        return (int64_t)(int32_t)CELL_EBUSY;
    }

#ifdef _WIN32
    DeleteCriticalSection(&m->cs);
    if (m->wait_handle) CloseHandle(m->wait_handle);
#else
    pthread_mutex_destroy(&m->mtx);
#endif

    m->active = 0;
    mtx_table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_mutex_lock
 *
 * r3 = mutex_id
 * r4 = timeout_usec (0 = infinite)
 * -----------------------------------------------------------------------*/
#ifndef _WIN32
/* MUTEX_WATCH=2 bookkeeping: who is blocked on which mutex since when. */
#define MUTEX_WATCH_SLOTS 64
static struct { volatile int used; uint64_t tid; uint32_t id, lr; struct timespec t0; }
    s_mw[MUTEX_WATCH_SLOTS];

static void* mutex_watch_reporter(void* arg)
{
    (void)arg;
    for (;;) {
        sleep(5);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        for (int i = 0; i < MUTEX_WATCH_SLOTS; i++) {
            if (!s_mw[i].used) continue;
            long waited = (long)(now.tv_sec - s_mw[i].t0.tv_sec);
            if (waited < 5) continue;
            const sys_mutex_info* m = &g_sys_mutexes[s_mw[i].id - 1];
            fprintf(stderr, "[mutex] tid=%llu blocked %lds on mutex %u '%.8s' lr=0x%08X; "
                    "owner tid=%llu count=%d\n",
                    (unsigned long long)s_mw[i].tid, waited, s_mw[i].id, m->name,
                    s_mw[i].lr, (unsigned long long)m->owner_tid, m->lock_count);
        }
    }
    return NULL;
}

static int mutex_watch_enter(uint64_t tid, uint32_t id, uint32_t lr)
{
    static int started = 0;
    if (!__atomic_exchange_n(&started, 1, __ATOMIC_SEQ_CST)) {
        pthread_t t;
        if (pthread_create(&t, NULL, mutex_watch_reporter, NULL) == 0) pthread_detach(t);
    }
    for (int i = 0; i < MUTEX_WATCH_SLOTS; i++) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&s_mw[i].used, &expected, 2, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            s_mw[i].tid = tid; s_mw[i].id = id; s_mw[i].lr = lr;
            clock_gettime(CLOCK_MONOTONIC, &s_mw[i].t0);
            __atomic_store_n(&s_mw[i].used, 1, __ATOMIC_SEQ_CST);
            return i;
        }
    }
    return -1;
}

static void mutex_watch_leave(int slot)
{
    if (slot >= 0) __atomic_store_n(&s_mw[slot].used, 0, __ATOMIC_SEQ_CST);
}
#endif

int64_t sys_mutex_lock(ppu_context* ctx)
{
    uint32_t mutex_id    = LV2_ARG_U32(ctx, 0);
    uint64_t timeout_us  = LV2_ARG_U64(ctx, 1);
    if (mutex_id == 0 || mutex_id > SYS_MUTEX_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_mutex_info* m = &g_sys_mutexes[mutex_id - 1];
    if (!m->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    uint64_t caller_tid = ctx->thread_id;

    /* Check for deadlock on non-recursive mutex */
    if (!m->recursive && m->owner_tid == caller_tid && m->lock_count > 0) {
        return (int64_t)(int32_t)CELL_EDEADLK;
    }

    __atomic_add_fetch(&m->waiters, 1, __ATOMIC_SEQ_CST);   /* see sys_mutex_destroy */
#ifdef _WIN32
    if (timeout_us == 0) {
        /* Infinite wait */
        EnterCriticalSection(&m->cs);
    } else {
        /* Timed lock via TryEnterCriticalSection to a QPC deadline. The old
         * GetTickCount/Sleep(1) loop measured elapsed time in ~15.6 ms ticks
         * and slept a full tick per retry, overshooting sub-ms timeouts by
         * orders of magnitude. Yield (not Sleep) for sub-ms timeouts so the
         * holder can run; Sleep(1) only for waits long enough to absorb it.
         * Safe to poll: TryEnterCriticalSection's success IS the atomic
         * acquire, so there's no separate signal to miss between polls. */
        int64_t deadline = lv2_usec_deadline(timeout_us);
        while (!TryEnterCriticalSection(&m->cs)) {
            if (lv2_deadline_passed(deadline)) {
                __atomic_sub_fetch(&m->waiters, 1, __ATOMIC_SEQ_CST);
                return (int64_t)(int32_t)CELL_ETIMEDOUT;
            }
            if (timeout_us < 1000) SwitchToThread();
            else                   Sleep(1);
        }
    }
#else
    static int s_watch = -1;
    if (s_watch < 0) { const char* e = getenv("MUTEX_WATCH"); s_watch = e ? atoi(e) : 0; }
    if (timeout_us == 0 && s_watch == 2) {
        /* MUTEX_WATCH=2: the wait stays a real blocking lock (the polling of
         * =1 changes the timing enough to hide some hangs), and a reporter
         * thread names every waiter older than 5 s and the mutex's owner. */
        int slot = mutex_watch_enter(caller_tid, mutex_id, (uint32_t)ctx->lr);
        pthread_mutex_lock(&m->mtx);
        mutex_watch_leave(slot);
    } else if (timeout_us == 0 && s_watch) {
        /* MUTEX_WATCH=1: an infinite lock waits in 5 s slices and says who it
         * is waiting on, so a guest deadlock names its two threads. */
        unsigned waited = 0;
        for (;;) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 5;
            if (pthread_mutex_timedlock(&m->mtx, &ts) == 0) break;
            waited += 5;
            fprintf(stderr, "[mutex] tid=%llu waiting %us on mutex %u '%.8s' "
                    "owner tid=%llu count=%u lr=0x%08X\n",
                    (unsigned long long)caller_tid, waited, mutex_id, m->name,
                    (unsigned long long)m->owner_tid, (unsigned)m->lock_count,
                    (uint32_t)ctx->lr);
        }
    } else if (timeout_us == 0) {
        pthread_mutex_lock(&m->mtx);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += (time_t)(timeout_us / 1000000);
        ts.tv_nsec += (long)((timeout_us % 1000000) * 1000);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        int rc = pthread_mutex_timedlock(&m->mtx, &ts);
        if (rc == ETIMEDOUT) {
            __atomic_sub_fetch(&m->waiters, 1, __ATOMIC_SEQ_CST);
            return (int64_t)(int32_t)CELL_ETIMEDOUT;
        }
    }
#endif
    __atomic_sub_fetch(&m->waiters, 1, __ATOMIC_SEQ_CST);

    m->owner_tid = caller_tid;
    m->lock_count++;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_mutex_trylock
 *
 * r3 = mutex_id
 * -----------------------------------------------------------------------*/
int64_t sys_mutex_trylock(ppu_context* ctx)
{
    uint32_t mutex_id = LV2_ARG_U32(ctx, 0);

    if (mutex_id == 0 || mutex_id > SYS_MUTEX_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_mutex_info* m = &g_sys_mutexes[mutex_id - 1];
    if (!m->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    uint64_t caller_tid = ctx->thread_id;

    /* Recursive re-entry. MUST still acquire the host primitive: unlock
     * calls LeaveCriticalSection unconditionally per guest unlock, so every
     * guest lock (including a recursive re-entry) must Enter exactly once
     * or the Enter/Leave pairing breaks: the paired unlock releases the CS
     * while the guest logically still holds the mutex, and the final
     * unlock calls Leave on a non-held CS (undefined behavior, corrupts
     * the CS and causes random forever-blocks on later Enters).
     * EnterCriticalSection by the current owner is itself recursive and
     * never blocks, so this is safe and cheap. */
    if (m->recursive && m->owner_tid == caller_tid && m->lock_count > 0) {
#ifdef _WIN32
        EnterCriticalSection(&m->cs);
#else
        /* Same reasoning, and it applies here: create() gives a recursive
         * guest mutex a PTHREAD_MUTEX_RECURSIVE host mutex, so the owner
         * re-locking it bumps the host recursion count and never blocks.
         * The comment this replaces claimed the host mutex was the default
         * non-recursive kind and skipped the lock, which left one host
         * acquisition behind two guest ones: the first guest unlock then
         * dropped the host mutex entirely and another thread could enter
         * while the guest still believed it held the mutex. */
        pthread_mutex_lock(&m->mtx);
#endif
        m->lock_count++;
        return CELL_OK;
    }

#ifdef _WIN32
    if (!TryEnterCriticalSection(&m->cs)) {
        return (int64_t)(int32_t)CELL_EBUSY;
    }
#else
    int rc = pthread_mutex_trylock(&m->mtx);
    if (rc != 0) {
        return (int64_t)(int32_t)CELL_EBUSY;
    }
#endif

    m->owner_tid = caller_tid;
    m->lock_count++;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_mutex_unlock
 *
 * r3 = mutex_id
 * -----------------------------------------------------------------------*/
int64_t sys_mutex_unlock(ppu_context* ctx)
{
    uint32_t mutex_id = LV2_ARG_U32(ctx, 0);

    if (mutex_id == 0 || mutex_id > SYS_MUTEX_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_mutex_info* m = &g_sys_mutexes[mutex_id - 1];
    if (!m->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    uint64_t caller_tid = ctx->thread_id;

    if (m->owner_tid != caller_tid) {
        /* Real lv2 returns EPERM for a non-owner unlock (RPCS3
         * sys_mutex.h's lv2_mutex::try_unlock, oracle, no code copied:
         * "if (it.owner != cpu.id) return CELL_EPERM;"), not a made up
         * CELL_EMUTEX_NOT_OWNED code that guest branches on this
         * contract would never match. */
        return (int64_t)(int32_t)CELL_EPERM;
    }

    m->lock_count--;
    if (m->lock_count == 0) {
        m->owner_tid = 0;
    }

#ifdef _WIN32
    LeaveCriticalSection(&m->cs);
#else
    pthread_mutex_unlock(&m->mtx);
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Registration
 * -----------------------------------------------------------------------*/
void sys_mutex_init(lv2_syscall_table* tbl)
{
    memset(g_sys_mutexes, 0, sizeof(g_sys_mutexes));

#ifdef _WIN32
    if (!s_mtx_table_lock_init) {
        InitializeCriticalSection(&s_mtx_table_lock);
        s_mtx_table_lock_init = 1;
    }
#endif

    lv2_syscall_register(tbl, SYS_MUTEX_CREATE,  sys_mutex_create);
    lv2_syscall_register(tbl, SYS_MUTEX_DESTROY, sys_mutex_destroy);
    lv2_syscall_register(tbl, SYS_MUTEX_LOCK,    sys_mutex_lock);
    lv2_syscall_register(tbl, SYS_MUTEX_TRYLOCK, sys_mutex_trylock);
    lv2_syscall_register(tbl, SYS_MUTEX_UNLOCK,  sys_mutex_unlock);
}
