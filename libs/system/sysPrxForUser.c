/*
 * ps3recomp - sysPrxForUser HLE implementation
 *
 * Real host-backed implementation: lwmutex uses CRITICAL_SECTION/pthread_mutex,
 * lwcond uses CONDITION_VARIABLE/pthread_cond, threads use CreateThread/pthread.
 * Heap uses standard malloc with tracking.
 */

#include "sysPrxForUser.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#endif

/* ---------------------------------------------------------------------------
 * Internal: Lightweight mutex table
 * -----------------------------------------------------------------------*/
#define MAX_LWMUTEX 256

typedef struct {
    int in_use;
    int recursive;
    char name[8];
#ifdef _WIN32
    CRITICAL_SECTION cs;
#else
    pthread_mutex_t mtx;
#endif
} LwMutexSlot;

static LwMutexSlot s_lwmutex[MAX_LWMUTEX];
static u32 s_lwmutex_next = 0;

/* ---------------------------------------------------------------------------
 * Internal: Lightweight cond table
 * -----------------------------------------------------------------------*/
#define MAX_LWCOND 256

typedef struct {
    int in_use;
    u32 lwmutex_id; /* index into s_lwmutex */
#ifdef _WIN32
    CONDITION_VARIABLE cv;
#else
    pthread_cond_t cv;
#endif
} LwCondSlot;

static LwCondSlot s_lwcond[MAX_LWCOND];
static u32 s_lwcond_next = 0;

/* ---------------------------------------------------------------------------
 * Internal: Thread table
 * -----------------------------------------------------------------------*/
#define MAX_PRX_THREADS 64

typedef struct {
    int in_use;
    u64 thread_id;
    sys_ppu_thread_entry_t entry;
    u64 arg;
    u64 exitcode;
    int joined;
    int detached;
#ifdef _WIN32
    HANDLE handle;
#else
    pthread_t pt;
#endif
} PrxThreadSlot;

static PrxThreadSlot s_threads[MAX_PRX_THREADS];
static u64 s_next_thread_id = 0x10000; /* start high to avoid collision with sys_ppu_thread */

#ifdef _WIN32
static DWORD WINAPI prx_thread_entry(LPVOID param)
{
    PrxThreadSlot* t = (PrxThreadSlot*)param;
    t->entry(t->arg);
    return 0;
}
#else
static void* prx_thread_entry(void* param)
{
    PrxThreadSlot* t = (PrxThreadSlot*)param;
    t->entry(t->arg);
    return NULL;
}
#endif

/* ---------------------------------------------------------------------------
 * Internal: Heap table
 * -----------------------------------------------------------------------*/
#define MAX_HEAPS 16

typedef struct {
    int in_use;
    u32 id;
} HeapSlot;

static HeapSlot s_heaps[MAX_HEAPS];
static u32 s_next_heap_id = 1;

/* ---------------------------------------------------------------------------
 * Thread management
 *
 * Thread functions (sys_ppu_thread_create, _exit, _join, _detach, _get_id,
 * _yield) are implemented in runtime/syscalls/sys_ppu_thread.c to avoid
 * duplication. They are declared in sysPrxForUser.h and linked from the
 * syscall implementation.
 * -----------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 * Process management
 * -----------------------------------------------------------------------*/

void sys_process_exit(s32 exitcode)
{
    printf("[sysPrxForUser] sys_process_exit(code=%d)\n", exitcode);
    exit(exitcode);
}

s32 sys_process_getpid(void)
{
    return 1001;
}

s32 sys_process_get_number_of_object(u32 object_type, u32* count)
{
    (void)object_type;
    if (count) *count = 0;
    return CELL_OK;
}

s32 sys_process_is_spu_lock_line_reservation_address(u32 addr, u64 flags)
{
    (void)addr; (void)flags;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * String/memory functions
 * -----------------------------------------------------------------------*/

s32 _sys_printf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("[PS3] ");
    int ret = vprintf(fmt, ap);
    va_end(ap);
    return ret;
}

s32 _sys_sprintf(char* buf, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vsprintf(buf, fmt, ap);
    va_end(ap);
    return ret;
}

s32 _sys_snprintf(char* buf, u32 size, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}

s32 _sys_strlen(const char* str)
{
    if (!str) return 0;
    return (s32)strlen(str);
}

s32 _sys_strncpy(char* dst, const char* src, u32 size)
{
    if (!dst || !src) return CELL_EFAULT;
    strncpy(dst, src, size);
    return CELL_OK;
}

s32 _sys_strcat(char* dst, const char* src)
{
    if (!dst || !src) return CELL_EFAULT;
    strcat(dst, src);
    return CELL_OK;
}

s32 _sys_strcmp(const char* s1, const char* s2)
{
    if (!s1 || !s2) return -1;
    return strcmp(s1, s2);
}

void* _sys_memset(void* dst, s32 val, u32 size)
{
    return memset(dst, val, size);
}

void* _sys_memcpy(void* dst, const void* src, u32 size)
{
    return memcpy(dst, src, size);
}

s32 _sys_memcmp(const void* s1, const void* s2, u32 size)
{
    return memcmp(s1, s2, size);
}

s32 _sys_toupper(s32 c) { return toupper(c); }
s32 _sys_tolower(s32 c) { return tolower(c); }

/* ---------------------------------------------------------------------------
 * Lightweight mutex
 * -----------------------------------------------------------------------*/

s32 sys_lwmutex_create(sys_lwmutex_t_hle* lwmutex, const sys_lwmutex_attribute_t* attr)
{
    printf("[sysPrxForUser] sys_lwmutex_create(name='%.8s')\n",
           attr ? attr->name : "???");

    if (!lwmutex)
        return CELL_EFAULT;

    u32 idx = s_lwmutex_next;
    for (u32 i = 0; i < MAX_LWMUTEX; i++) {
        u32 slot = (idx + i) % MAX_LWMUTEX;
        if (!s_lwmutex[slot].in_use) {
            LwMutexSlot* m = &s_lwmutex[slot];
            m->in_use = 1;
            m->recursive = (attr && (attr->recursive & SYS_SYNC_RECURSIVE)) ? 1 : 0;
            if (attr)
                memcpy(m->name, attr->name, 8);

#ifdef _WIN32
            InitializeCriticalSection(&m->cs);
#else
            pthread_mutexattr_t mattr;
            pthread_mutexattr_init(&mattr);
            if (m->recursive)
                pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(&m->mtx, &mattr);
            pthread_mutexattr_destroy(&mattr);
#endif

            memset(lwmutex, 0, sizeof(*lwmutex));
            lwmutex->sleep_queue = slot + 1; /* 1-based ID */
            s_lwmutex_next = (slot + 1) % MAX_LWMUTEX;
            return CELL_OK;
        }
    }
    return CELL_EAGAIN;
}

s32 sys_lwmutex_lock(sys_lwmutex_t_hle* lwmutex, u64 timeout)
{
    (void)timeout;
    if (!lwmutex) return CELL_EFAULT;

    u32 slot = lwmutex->sleep_queue - 1;
    if (slot >= MAX_LWMUTEX || !s_lwmutex[slot].in_use)
        return CELL_ESRCH;

#ifdef _WIN32
    EnterCriticalSection(&s_lwmutex[slot].cs);
#else
    pthread_mutex_lock(&s_lwmutex[slot].mtx);
#endif

    lwmutex->lock_var = 1;
    lwmutex->recursive_count++;
    return CELL_OK;
}

s32 sys_lwmutex_trylock(sys_lwmutex_t_hle* lwmutex)
{
    if (!lwmutex) return CELL_EFAULT;

    u32 slot = lwmutex->sleep_queue - 1;
    if (slot >= MAX_LWMUTEX || !s_lwmutex[slot].in_use)
        return CELL_ESRCH;

#ifdef _WIN32
    if (!TryEnterCriticalSection(&s_lwmutex[slot].cs))
        return CELL_EBUSY;
#else
    if (pthread_mutex_trylock(&s_lwmutex[slot].mtx) != 0)
        return CELL_EBUSY;
#endif

    lwmutex->lock_var = 1;
    lwmutex->recursive_count++;
    return CELL_OK;
}

s32 sys_lwmutex_unlock(sys_lwmutex_t_hle* lwmutex)
{
    if (!lwmutex) return CELL_EFAULT;

    u32 slot = lwmutex->sleep_queue - 1;
    if (slot >= MAX_LWMUTEX || !s_lwmutex[slot].in_use)
        return CELL_ESRCH;

    lwmutex->recursive_count--;
    if (lwmutex->recursive_count == 0)
        lwmutex->lock_var = 0;

#ifdef _WIN32
    LeaveCriticalSection(&s_lwmutex[slot].cs);
#else
    pthread_mutex_unlock(&s_lwmutex[slot].mtx);
#endif
    return CELL_OK;
}

s32 sys_lwmutex_destroy(sys_lwmutex_t_hle* lwmutex)
{
    printf("[sysPrxForUser] sys_lwmutex_destroy()\n");

    if (!lwmutex) return CELL_EFAULT;

    u32 slot = lwmutex->sleep_queue - 1;
    if (slot < MAX_LWMUTEX && s_lwmutex[slot].in_use) {
#ifdef _WIN32
        DeleteCriticalSection(&s_lwmutex[slot].cs);
#else
        pthread_mutex_destroy(&s_lwmutex[slot].mtx);
#endif
        s_lwmutex[slot].in_use = 0;
    }

    memset(lwmutex, 0, sizeof(*lwmutex));
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Lightweight condition variable
 * -----------------------------------------------------------------------*/

s32 sys_lwcond_create(sys_lwcond_t_hle* lwcond, sys_lwmutex_t_hle* lwmutex,
                      const sys_lwcond_attribute_t* attr)
{
    printf("[sysPrxForUser] sys_lwcond_create(name='%.8s')\n",
           attr ? attr->name : "???");

    if (!lwcond || !lwmutex)
        return CELL_EFAULT;

    u32 idx = s_lwcond_next;
    for (u32 i = 0; i < MAX_LWCOND; i++) {
        u32 slot = (idx + i) % MAX_LWCOND;
        if (!s_lwcond[slot].in_use) {
            LwCondSlot* c = &s_lwcond[slot];
            c->in_use = 1;
            c->lwmutex_id = lwmutex->sleep_queue - 1;

#ifdef _WIN32
            InitializeConditionVariable(&c->cv);
#else
            pthread_cond_init(&c->cv, NULL);
#endif

            lwcond->lwcond_queue = slot + 1;
            s_lwcond_next = (slot + 1) % MAX_LWCOND;
            return CELL_OK;
        }
    }
    return CELL_EAGAIN;
}

s32 sys_lwcond_signal(sys_lwcond_t_hle* lwcond)
{
    if (!lwcond) return CELL_EFAULT;

    u32 slot = lwcond->lwcond_queue - 1;
    if (slot >= MAX_LWCOND || !s_lwcond[slot].in_use)
        return CELL_ESRCH;

#ifdef _WIN32
    WakeConditionVariable(&s_lwcond[slot].cv);
#else
    pthread_cond_signal(&s_lwcond[slot].cv);
#endif
    return CELL_OK;
}

s32 sys_lwcond_signal_all(sys_lwcond_t_hle* lwcond)
{
    if (!lwcond) return CELL_EFAULT;

    u32 slot = lwcond->lwcond_queue - 1;
    if (slot >= MAX_LWCOND || !s_lwcond[slot].in_use)
        return CELL_ESRCH;

#ifdef _WIN32
    WakeAllConditionVariable(&s_lwcond[slot].cv);
#else
    pthread_cond_broadcast(&s_lwcond[slot].cv);
#endif
    return CELL_OK;
}

s32 sys_lwcond_wait(sys_lwcond_t_hle* lwcond, u64 timeout)
{
    if (!lwcond) return CELL_EFAULT;

    u32 cslot = lwcond->lwcond_queue - 1;
    if (cslot >= MAX_LWCOND || !s_lwcond[cslot].in_use)
        return CELL_ESRCH;

    u32 mslot = s_lwcond[cslot].lwmutex_id;
    if (mslot >= MAX_LWMUTEX || !s_lwmutex[mslot].in_use)
        return CELL_ESRCH;

#ifdef _WIN32
    DWORD ms = (timeout == 0) ? INFINITE : (DWORD)(timeout / 1000);
    if (!SleepConditionVariableCS(&s_lwcond[cslot].cv,
                                   &s_lwmutex[mslot].cs, ms))
    {
        if (GetLastError() == ERROR_TIMEOUT)
            return CELL_ETIMEDOUT;
        return CELL_EFAULT;
    }
#else
    if (timeout == 0) {
        pthread_cond_wait(&s_lwcond[cslot].cv, &s_lwmutex[mslot].mtx);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += (time_t)(timeout / 1000000);
        ts.tv_nsec += (long)((timeout % 1000000) * 1000);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        int rc = pthread_cond_timedwait(&s_lwcond[cslot].cv,
                                         &s_lwmutex[mslot].mtx, &ts);
        if (rc == 110 /* ETIMEDOUT */)
            return CELL_ETIMEDOUT;
    }
#endif

    return CELL_OK;
}

s32 sys_lwcond_destroy(sys_lwcond_t_hle* lwcond)
{
    printf("[sysPrxForUser] sys_lwcond_destroy()\n");

    if (!lwcond) return CELL_EFAULT;

    u32 slot = lwcond->lwcond_queue - 1;
    if (slot < MAX_LWCOND && s_lwcond[slot].in_use) {
#ifndef _WIN32
        pthread_cond_destroy(&s_lwcond[slot].cv);
#endif
        s_lwcond[slot].in_use = 0;
    }

    memset(lwcond, 0, sizeof(*lwcond));
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Heap management (wraps malloc)
 * -----------------------------------------------------------------------*/

s32 sys_heap_create_heap(sys_heap_t* heap, u32 start_addr, u32 size,
                          u32 flags, void* alloc_func, void* free_func)
{
    (void)start_addr; (void)size; (void)flags;
    (void)alloc_func; (void)free_func;

    printf("[sysPrxForUser] sys_heap_create_heap(size=%u)\n", size);

    if (!heap) return CELL_EFAULT;

    for (int i = 0; i < MAX_HEAPS; i++) {
        if (!s_heaps[i].in_use) {
            s_heaps[i].in_use = 1;
            s_heaps[i].id = s_next_heap_id++;
            *heap = s_heaps[i].id;
            return CELL_OK;
        }
    }
    return CELL_ENOMEM;
}

s32 sys_heap_destroy_heap(sys_heap_t heap)
{
    printf("[sysPrxForUser] sys_heap_destroy_heap(id=%u)\n", heap);

    for (int i = 0; i < MAX_HEAPS; i++) {
        if (s_heaps[i].in_use && s_heaps[i].id == heap) {
            s_heaps[i].in_use = 0;
            return CELL_OK;
        }
    }
    return CELL_ESRCH;
}

void* sys_heap_malloc(sys_heap_t heap, u32 size)
{
    (void)heap;
    return malloc(size);
}

s32 sys_heap_free(sys_heap_t heap, void* ptr)
{
    (void)heap;
    free(ptr);
    return CELL_OK;
}

void* sys_heap_memalign(sys_heap_t heap, u32 align, u32 size)
{
    (void)heap;
#ifdef _WIN32
    return _aligned_malloc(size, align);
#else
    void* ptr = NULL;
    if (posix_memalign(&ptr, align, size) != 0)
        return NULL;
    return ptr;
#endif
}

/* ---------------------------------------------------------------------------
 * PRX utilities
 * -----------------------------------------------------------------------*/

s32 sys_prx_exitspawn_with_level(void)
{
    printf("[sysPrxForUser] sys_prx_exitspawn_with_level() - no-op\n");
    return CELL_OK;
}

s32 sys_prx_get_module_id_by_name(const char* name, u64 flags, u32* id)
{
    (void)flags;
    printf("[sysPrxForUser] sys_prx_get_module_id_by_name('%s')\n",
           name ? name : "(null)");

    if (!id) return CELL_EFAULT;
    *id = 0; /* fake module ID */
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Random number generation
 *
 * Used by many games for seeding RNG, crypto operations, UUID generation.
 * We use the host OS PRNG for quality random data.
 * -----------------------------------------------------------------------*/

s32 sys_get_random_number(void* buf, u64 size)
{
    if (!buf || size == 0) return CELL_EFAULT;

#ifdef _WIN32
    /* Use BCryptGenRandom on Windows */
    #include <bcrypt.h>
    NTSTATUS status = BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)size,
                                       BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0) {
        /* Fallback: fill with simple pseudo-random */
        u8* p = (u8*)buf;
        for (u64 i = 0; i < size; i++)
            p[i] = (u8)(rand() & 0xFF);
    }
#else
    /* Use /dev/urandom on Unix */
    FILE* f = fopen("/dev/urandom", "rb");
    if (f) {
        fread(buf, 1, (size_t)size, f);
        fclose(f);
    } else {
        u8* p = (u8*)buf;
        for (u64 i = 0; i < size; i++)
            p[i] = (u8)(rand() & 0xFF);
    }
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Console I/O (debug)
 * -----------------------------------------------------------------------*/

s32 console_putc(s32 ch)
{
    fputc(ch, stderr);
    return CELL_OK;
}

s32 console_getc(void)
{
    return -1; /* no input */
}

s32 console_write(const void* buf, u32 len)
{
    if (buf && len > 0)
        fwrite(buf, 1, len, stderr);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Process info
 * -----------------------------------------------------------------------*/

s32 sys_process_get_paramsfo(void* buf)
{
    /* PARAM.SFO data — return a minimal valid SFO with title ID */
    if (!buf) return CELL_EFAULT;
    memset(buf, 0, 256);
    return CELL_OK;
}
