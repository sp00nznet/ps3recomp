/*
 * ps3recomp - Kernel mutex syscalls (implementation)
 */

#include "sys_mutex.h"
#include "../memory/vm.h"
#include <string.h>
#include <stdio.h>

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

#ifdef _WIN32
    if (timeout_us == 0) {
        /* Infinite wait */
        EnterCriticalSection(&m->cs);
    } else {
        /* Timed lock via TryEnterCriticalSection + spin/sleep */
        DWORD timeout_ms = (DWORD)(timeout_us / 1000);
        if (timeout_ms == 0) timeout_ms = 1;
        DWORD start = GetTickCount();
        while (!TryEnterCriticalSection(&m->cs)) {
            DWORD elapsed = GetTickCount() - start;
            if (elapsed >= timeout_ms) {
                return (int64_t)(int32_t)CELL_ETIMEDOUT;
            }
            Sleep(1);
        }
    }
#else
    if (timeout_us == 0) {
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
            return (int64_t)(int32_t)CELL_ETIMEDOUT;
        }
    }
#endif

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

    /* Recursive re-entry */
    if (m->recursive && m->owner_tid == caller_tid && m->lock_count > 0) {
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
        return (int64_t)(int32_t)CELL_EMUTEX_NOT_OWNED;
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
