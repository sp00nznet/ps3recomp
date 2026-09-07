/* ps3recomp - Kernel condition variables.
 *
 * The kernel commits a waiter before releasing its guest mutex. A host CV
 * alone does not retain a signal delivered between syscall entry and parking.
 * Keep explicit wait records under a separate lock; signals never need the
 * guest mutex, and a later waiter cannot steal an earlier waiter's wakeup.
 */
#include "sys_cond.h"
#include "../memory/vm.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

struct sys_cond_waiter {
    struct sys_cond_waiter* next;
    int signalled;
};

sys_cond_info g_sys_conds[SYS_COND_MAX];
uint32_t g_cond_id_addr[SYS_COND_MAX + 1];

#ifdef _WIN32
static SRWLOCK s_table_lock = SRWLOCK_INIT;
static void table_lock(void) { AcquireSRWLockExclusive(&s_table_lock); }
static void table_unlock(void) { ReleaseSRWLockExclusive(&s_table_lock); }
static void cond_lock(sys_cond_info* c) { EnterCriticalSection(&c->signal_lock); }
static void cond_unlock(sys_cond_info* c) { LeaveCriticalSection(&c->signal_lock); }
static void cond_wake(sys_cond_info* c) { WakeAllConditionVariable(&c->cv); }
static void guest_lock(sys_mutex_info* m) { EnterCriticalSection(&m->cs); }
static void guest_unlock(sys_mutex_info* m) { LeaveCriticalSection(&m->cs); }
#else
static pthread_mutex_t s_table_lock = PTHREAD_MUTEX_INITIALIZER;
static void table_lock(void) { pthread_mutex_lock(&s_table_lock); }
static void table_unlock(void) { pthread_mutex_unlock(&s_table_lock); }
static void cond_lock(sys_cond_info* c) { pthread_mutex_lock(&c->signal_lock); }
static void cond_unlock(sys_cond_info* c) { pthread_mutex_unlock(&c->signal_lock); }
static void cond_wake(sys_cond_info* c) { pthread_cond_broadcast(&c->cv); }
static void guest_lock(sys_mutex_info* m) { pthread_mutex_lock(&m->mtx); }
static void guest_unlock(sys_mutex_info* m) { pthread_mutex_unlock(&m->mtx); }
#endif

/* Lock ordering is table -> condition. No caller holds the condition lock
 * while acquiring a guest mutex. Native locks survive slot recycling. */
static sys_cond_info* acquire_cond(uint32_t id)
{
    if (!id || id > SYS_COND_MAX) return NULL;
    table_lock();
    sys_cond_info* c = &g_sys_conds[id - 1];
    if (!c->active) { table_unlock(); return NULL; }
    cond_lock(c);
    table_unlock();
    return c;
}

int64_t sys_cond_create(ppu_context* ctx)
{
    uint32_t out = LV2_ARG_PTR(ctx, 0);
    uint32_t mid = LV2_ARG_U32(ctx, 1);
    uint32_t attr = LV2_ARG_PTR(ctx, 2);
    if (!mid || mid > SYS_MUTEX_MAX || !g_sys_mutexes[mid - 1].active)
        return (int32_t)CELL_ESRCH;
    table_lock();
    for (unsigned i = 0; i < SYS_COND_MAX; ++i) {
        sys_cond_info* c = &g_sys_conds[i];
        if (c->active) continue;
        if (!c->initialized) {
#ifdef _WIN32
            InitializeCriticalSection(&c->signal_lock);
            InitializeConditionVariable(&c->cv);
#else
            int rc = pthread_mutex_init(&c->signal_lock, NULL);
            if (rc) { table_unlock(); return (int32_t)CELL_EAGAIN; }
            rc = pthread_cond_init(&c->cv, NULL);
            if (rc) {
                pthread_mutex_destroy(&c->signal_lock);
                table_unlock(); return (int32_t)CELL_EAGAIN;
            }
#endif
            c->initialized = 1;
        }
        c->mutex_id = mid;
        c->waiters = NULL;
        memset(c->name, 0, sizeof(c->name));
        if (attr) memcpy(c->name, (uint8_t*)vm_to_host(attr) + 8, 8);
        c->active = 1;
        g_cond_id_addr[i + 1] = out;
        if (out) {
            uint8_t* p = vm_to_host(out);
            uint32_t id = i + 1;
            p[0] = (uint8_t)(id >> 24); p[1] = (uint8_t)(id >> 16);
            p[2] = (uint8_t)(id >> 8); p[3] = (uint8_t)id;
        }
        if (getenv("PS3_SYNCLOG") || getenv("YDKJ_SYNCLOG")) {
            char name[9]; memcpy(name, c->name, 8); name[8] = 0;
            fprintf(stderr, "[SYNC] cond_create id=%u name='%s' mutex=%u id_at=0x%08X lr=0x%08X\n",
                    i + 1, name, mid, out, (uint32_t)ctx->lr);
        }
        table_unlock();
        return CELL_OK;
    }
    table_unlock();
    return (int32_t)CELL_EAGAIN;
}

int64_t sys_cond_destroy(ppu_context* ctx)
{
    uint32_t id = LV2_ARG_U32(ctx, 0);
    if (!id || id > SYS_COND_MAX) return (int32_t)CELL_ESRCH;
    table_lock();
    sys_cond_info* c = &g_sys_conds[id - 1];
    if (!c->active) { table_unlock(); return (int32_t)CELL_ESRCH; }
    cond_lock(c);
    int32_t result = CELL_OK;
    if (c->waiters) result = (int32_t)CELL_EBUSY;
    else { c->active = 0; g_cond_id_addr[id] = 0; }
    cond_unlock(c);
    table_unlock();
    return result;
}

int64_t sys_cond_wait(ppu_context* ctx)
{
    uint64_t timeout = LV2_ARG_U64(ctx, 1);
    /* Preserve upstream's opt-in wait diagnostics across the waiter rewrite. */
    const char* peek = getenv("PS3_COND_PEEK");
    if (peek) {
        uint32_t ea = (uint32_t)strtoul(peek, NULL, 16);
        if (ea) {
            const uint8_t* p = vm_to_host(ea);
            uint32_t value = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                             ((uint32_t)p[2] << 8) | p[3];
            fprintf(stderr, "[PEEK] [0x%08X]=%u (0x%08X) at cond_wait(cond=%u)\n",
                    ea, value, value, LV2_ARG_U32(ctx, 0));
        }
    }
#ifdef _WIN32
    const char* spurious = getenv("PS3_COND_SPURIOUS_MS");
    long probe_ms = spurious ? atol(spurious) : 0;
    if (probe_ms > 0 && !timeout) timeout = (uint64_t)probe_ms * 1000;
#endif
    if (timeout > ((1ull << 48) - 1)) timeout = (1ull << 48) - 1;
    sys_cond_info* c = acquire_cond(LV2_ARG_U32(ctx, 0));
    if (!c) return (int32_t)CELL_ESRCH;
    sys_mutex_info* m = &g_sys_mutexes[c->mutex_id - 1];
    if (!m->active) { cond_unlock(c); return (int32_t)CELL_ESRCH; }
    if (m->owner_tid != ctx->thread_id || m->lock_count <= 0) {
        cond_unlock(c); return (int32_t)CELL_EPERM;
    }
    struct sys_cond_waiter waiter = {NULL, 0};
    struct sys_cond_waiter** tail = &c->waiters;
    while (*tail) tail = &(*tail)->next;
    *tail = &waiter;
    int depth = m->lock_count;
    m->owner_tid = 0;
    m->lock_count = 0;
    for (int i = 0; i < depth; ++i) guest_unlock(m);

    int32_t result = CELL_OK;
#ifdef _WIN32
    ULONGLONG start = GetTickCount64();
    uint64_t duration = (timeout + 999) / 1000;
    while (!waiter.signalled) {
        DWORD ms = INFINITE;
        if (timeout) {
            uint64_t elapsed = GetTickCount64() - start;
            if (elapsed >= duration) { result = (int32_t)CELL_ETIMEDOUT; break; }
            uint64_t remaining = duration - elapsed;
            ms = remaining >= INFINITE ? INFINITE - 1 : (DWORD)remaining;
        }
        BOOL ok = SleepConditionVariableCS(&c->cv, &c->signal_lock, ms);
        if (!ok && GetLastError() != ERROR_TIMEOUT && !waiter.signalled) {
            result = (int32_t)CELL_EFAULT; break;
        }
    }
#else
    struct timespec deadline;
    if (timeout) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += (time_t)(timeout / 1000000);
        deadline.tv_nsec += (long)((timeout % 1000000) * 1000);
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++; deadline.tv_nsec -= 1000000000L;
        }
    }
    while (!waiter.signalled) {
        int rc = timeout ? pthread_cond_timedwait(&c->cv, &c->signal_lock, &deadline)
                         : pthread_cond_wait(&c->cv, &c->signal_lock);
        if (rc && !waiter.signalled) {
            result = (int32_t)(rc == ETIMEDOUT ? CELL_ETIMEDOUT : CELL_EFAULT);
            break;
        }
    }
#endif
    struct sys_cond_waiter** link = &c->waiters;
    while (*link != &waiter) link = &(*link)->next;
    *link = waiter.next;
    cond_unlock(c);
    for (int i = 0; i < depth; ++i) guest_lock(m);
    m->owner_tid = ctx->thread_id;
    m->lock_count = depth;
    return result;
}

static int64_t signal_cond(uint32_t id, int all)
{
    sys_cond_info* c = acquire_cond(id);
    if (!c) return (int32_t)CELL_ESRCH;
    int selected = 0;
    for (struct sys_cond_waiter* w = c->waiters; w; w = w->next) {
        if (w->signalled) continue;
        w->signalled = 1;
        selected = 1;
        if (!all) break;
    }
    /* Broadcast the host CV: only selected records return to the guest.
     * Waking an arbitrary host waiter could leave the selected one asleep. */
    if (selected) cond_wake(c);
    cond_unlock(c);
    return CELL_OK;
}

int64_t sys_cond_signal(ppu_context* ctx) { return signal_cond(LV2_ARG_U32(ctx, 0), 0); }
int64_t sys_cond_signal_all(ppu_context* ctx) { return signal_cond(LV2_ARG_U32(ctx, 0), 1); }

/* Preserve upstream's current broadcast fallback for signal_to. */
int64_t sys_cond_signal_to(ppu_context* ctx) { return signal_cond(LV2_ARG_U32(ctx, 0), 1); }

void sys_cond_init(lv2_syscall_table* tbl)
{
    /* Static storage starts empty. Registration must not memset live locks. */
    lv2_syscall_register(tbl, SYS_COND_CREATE, sys_cond_create);
    lv2_syscall_register(tbl, SYS_COND_DESTROY, sys_cond_destroy);
    lv2_syscall_register(tbl, SYS_COND_WAIT, sys_cond_wait);
    lv2_syscall_register(tbl, SYS_COND_SIGNAL, sys_cond_signal);
    lv2_syscall_register(tbl, SYS_COND_SIGNAL_ALL, sys_cond_signal_all);
    lv2_syscall_register(tbl, SYS_COND_SIGNAL_TO, sys_cond_signal_to);
}
