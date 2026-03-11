/*
 * ps3recomp - Semaphore syscalls (implementation)
 */

#include "sys_semaphore.h"
#include "../memory/vm.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Globals
 * -----------------------------------------------------------------------*/
sys_semaphore_info g_sys_semaphores[SYS_SEMAPHORE_MAX];

#ifdef _WIN32
static CRITICAL_SECTION s_sem_table_lock;
static int              s_sem_table_lock_init = 0;
#else
static pthread_mutex_t  s_sem_table_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static void sem_table_lock(void)
{
#ifdef _WIN32
    if (!s_sem_table_lock_init) {
        InitializeCriticalSection(&s_sem_table_lock);
        s_sem_table_lock_init = 1;
    }
    EnterCriticalSection(&s_sem_table_lock);
#else
    pthread_mutex_lock(&s_sem_table_lock);
#endif
}

static void sem_table_unlock(void)
{
#ifdef _WIN32
    LeaveCriticalSection(&s_sem_table_lock);
#else
    pthread_mutex_unlock(&s_sem_table_lock);
#endif
}

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
 * sys_semaphore_create
 *
 * r3 = pointer to receive semaphore ID (u32*)
 * r4 = pointer to attribute struct
 * r5 = initial value
 * r6 = max value
 * -----------------------------------------------------------------------*/
int64_t sys_semaphore_create(ppu_context* ctx)
{
    uint32_t id_out_addr  = LV2_ARG_PTR(ctx, 0);
    uint32_t attr_addr    = LV2_ARG_PTR(ctx, 1);
    int32_t  initial      = LV2_ARG_S32(ctx, 2);
    int32_t  max_val      = LV2_ARG_S32(ctx, 3);

    if (max_val <= 0 || initial < 0 || initial > max_val)
        return (int64_t)(int32_t)CELL_EINVAL;

    sem_table_lock();

    int slot = -1;
    for (int i = 0; i < SYS_SEMAPHORE_MAX; i++) {
        if (!g_sys_semaphores[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        sem_table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }

    sys_semaphore_info* s = &g_sys_semaphores[slot];
    memset(s, 0, sizeof(*s));
    s->active    = 1;
    s->value     = initial;
    s->max_value = max_val;

    if (attr_addr != 0) {
        uint8_t* attr_raw = (uint8_t*)vm_to_host(attr_addr);
        uint32_t proto_be;
        memcpy(&proto_be, attr_raw, 4);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        proto_be = ((proto_be >> 24) & 0xFF) | ((proto_be >> 8) & 0xFF00) |
                   ((proto_be << 8) & 0xFF0000) | ((proto_be << 24) & 0xFF000000u);
#endif
        s->protocol = proto_be;
        /* name at offset 8 */
        memcpy(s->name, attr_raw + 8, 8);
    }

#ifdef _WIN32
    s->sem_handle = CreateSemaphoreA(NULL, initial, max_val, NULL);
    InitializeCriticalSection(&s->value_lock);
    if (s->sem_handle == NULL) {
        s->active = 0;
        sem_table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }
#else
    pthread_mutex_init(&s->mtx, NULL);
    pthread_cond_init(&s->cv, NULL);
#endif

    uint32_t sem_id = (uint32_t)(slot + 1);
    if (id_out_addr != 0) {
        write_be32(id_out_addr, sem_id);
    }

    sem_table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_semaphore_destroy
 *
 * r3 = sem_id
 * -----------------------------------------------------------------------*/
int64_t sys_semaphore_destroy(ppu_context* ctx)
{
    uint32_t sem_id = LV2_ARG_U32(ctx, 0);

    if (sem_id == 0 || sem_id > SYS_SEMAPHORE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sem_table_lock();

    sys_semaphore_info* s = &g_sys_semaphores[sem_id - 1];
    if (!s->active) {
        sem_table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

#ifdef _WIN32
    CloseHandle(s->sem_handle);
    DeleteCriticalSection(&s->value_lock);
#else
    pthread_cond_destroy(&s->cv);
    pthread_mutex_destroy(&s->mtx);
#endif

    s->active = 0;
    sem_table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_semaphore_wait
 *
 * r3 = sem_id
 * r4 = timeout_usec (0 = infinite)
 * -----------------------------------------------------------------------*/
int64_t sys_semaphore_wait(ppu_context* ctx)
{
    uint32_t sem_id     = LV2_ARG_U32(ctx, 0);
    uint64_t timeout_us = LV2_ARG_U64(ctx, 1);

    if (sem_id == 0 || sem_id > SYS_SEMAPHORE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_semaphore_info* s = &g_sys_semaphores[sem_id - 1];
    if (!s->active)
        return (int64_t)(int32_t)CELL_ESRCH;

#ifdef _WIN32
    DWORD ms = (timeout_us == 0) ? INFINITE : (DWORD)(timeout_us / 1000);
    if (ms == 0 && timeout_us > 0) ms = 1;

    DWORD result = WaitForSingleObject(s->sem_handle, ms);
    if (result == WAIT_TIMEOUT) {
        return (int64_t)(int32_t)CELL_ETIMEDOUT;
    }
    if (result != WAIT_OBJECT_0) {
        return (int64_t)(int32_t)CELL_EINVAL;
    }

    EnterCriticalSection(&s->value_lock);
    s->value--;
    LeaveCriticalSection(&s->value_lock);
#else
    pthread_mutex_lock(&s->mtx);

    if (timeout_us == 0) {
        while (s->value <= 0) {
            pthread_cond_wait(&s->cv, &s->mtx);
        }
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += (time_t)(timeout_us / 1000000);
        ts.tv_nsec += (long)((timeout_us % 1000000) * 1000);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        while (s->value <= 0) {
            int rc = pthread_cond_timedwait(&s->cv, &s->mtx, &ts);
            if (rc == ETIMEDOUT) {
                pthread_mutex_unlock(&s->mtx);
                return (int64_t)(int32_t)CELL_ETIMEDOUT;
            }
        }
    }

    s->value--;
    pthread_mutex_unlock(&s->mtx);
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_semaphore_trywait
 *
 * r3 = sem_id
 * -----------------------------------------------------------------------*/
int64_t sys_semaphore_trywait(ppu_context* ctx)
{
    uint32_t sem_id = LV2_ARG_U32(ctx, 0);

    if (sem_id == 0 || sem_id > SYS_SEMAPHORE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_semaphore_info* s = &g_sys_semaphores[sem_id - 1];
    if (!s->active)
        return (int64_t)(int32_t)CELL_ESRCH;

#ifdef _WIN32
    DWORD result = WaitForSingleObject(s->sem_handle, 0);
    if (result == WAIT_TIMEOUT) {
        return (int64_t)(int32_t)CELL_EBUSY;
    }
    if (result != WAIT_OBJECT_0) {
        return (int64_t)(int32_t)CELL_EINVAL;
    }
    EnterCriticalSection(&s->value_lock);
    s->value--;
    LeaveCriticalSection(&s->value_lock);
#else
    pthread_mutex_lock(&s->mtx);
    if (s->value <= 0) {
        pthread_mutex_unlock(&s->mtx);
        return (int64_t)(int32_t)CELL_EBUSY;
    }
    s->value--;
    pthread_mutex_unlock(&s->mtx);
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_semaphore_post
 *
 * r3 = sem_id
 * r4 = count (number to post)
 * -----------------------------------------------------------------------*/
int64_t sys_semaphore_post(ppu_context* ctx)
{
    uint32_t sem_id = LV2_ARG_U32(ctx, 0);
    int32_t  count  = LV2_ARG_S32(ctx, 1);

    if (sem_id == 0 || sem_id > SYS_SEMAPHORE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (count <= 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    sys_semaphore_info* s = &g_sys_semaphores[sem_id - 1];
    if (!s->active)
        return (int64_t)(int32_t)CELL_ESRCH;

#ifdef _WIN32
    EnterCriticalSection(&s->value_lock);
    if (s->value + count > s->max_value) {
        LeaveCriticalSection(&s->value_lock);
        return (int64_t)(int32_t)CELL_EINVAL;
    }
    s->value += count;
    LeaveCriticalSection(&s->value_lock);

    ReleaseSemaphore(s->sem_handle, count, NULL);
#else
    pthread_mutex_lock(&s->mtx);
    if (s->value + count > s->max_value) {
        pthread_mutex_unlock(&s->mtx);
        return (int64_t)(int32_t)CELL_EINVAL;
    }
    s->value += count;
    /* Wake waiters */
    for (int i = 0; i < count; i++) {
        pthread_cond_signal(&s->cv);
    }
    pthread_mutex_unlock(&s->mtx);
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_semaphore_get_value
 *
 * r3 = sem_id
 * r4 = pointer to receive value (s32*)
 * -----------------------------------------------------------------------*/
int64_t sys_semaphore_get_value(ppu_context* ctx)
{
    uint32_t sem_id   = LV2_ARG_U32(ctx, 0);
    uint32_t out_addr = LV2_ARG_PTR(ctx, 1);

    if (sem_id == 0 || sem_id > SYS_SEMAPHORE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_semaphore_info* s = &g_sys_semaphores[sem_id - 1];
    if (!s->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    int32_t val;
#ifdef _WIN32
    EnterCriticalSection(&s->value_lock);
    val = s->value;
    LeaveCriticalSection(&s->value_lock);
#else
    pthread_mutex_lock(&s->mtx);
    val = s->value;
    pthread_mutex_unlock(&s->mtx);
#endif

    if (out_addr != 0) {
        write_be32(out_addr, (uint32_t)val);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Registration
 * -----------------------------------------------------------------------*/
void sys_semaphore_init(lv2_syscall_table* tbl)
{
    memset(g_sys_semaphores, 0, sizeof(g_sys_semaphores));

#ifdef _WIN32
    if (!s_sem_table_lock_init) {
        InitializeCriticalSection(&s_sem_table_lock);
        s_sem_table_lock_init = 1;
    }
#endif

    lv2_syscall_register(tbl, SYS_SEMAPHORE_CREATE,    sys_semaphore_create);
    lv2_syscall_register(tbl, SYS_SEMAPHORE_DESTROY,   sys_semaphore_destroy);
    lv2_syscall_register(tbl, SYS_SEMAPHORE_WAIT,      sys_semaphore_wait);
    lv2_syscall_register(tbl, SYS_SEMAPHORE_TRYWAIT,   sys_semaphore_trywait);
    lv2_syscall_register(tbl, SYS_SEMAPHORE_POST,      sys_semaphore_post);
    lv2_syscall_register(tbl, SYS_SEMAPHORE_GET_VALUE, sys_semaphore_get_value);
}
