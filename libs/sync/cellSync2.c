/*
 * ps3recomp - cellSync2 HLE implementation
 *
 * Extended sync primitives using host OS primitives.
 * Windows: SRWLOCK, CONDITION_VARIABLE
 * Unix: pthread_mutex, pthread_cond, sem_t
 */

#include "cellSync2.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <time.h>
#endif

/* ---------------------------------------------------------------------------
 * Internal layout overlaid on opaque buffers
 * -----------------------------------------------------------------------*/

typedef struct Sync2MutexInternal {
    u32 initialized;
    u32 recursive;
    u32 lockCount;    /* for recursive */
    u32 ownerThread;  /* simplified - not truly thread-safe ID */
#ifdef _WIN32
    CRITICAL_SECTION cs;
#else
    pthread_mutex_t mtx;
#endif
} Sync2MutexInternal;

typedef struct Sync2CondInternal {
    u32 initialized;
    Sync2MutexInternal* mutex;
#ifdef _WIN32
    CONDITION_VARIABLE cv;
#else
    pthread_cond_t cv;
#endif
} Sync2CondInternal;

typedef struct Sync2SemInternal {
    u32 initialized;
    s32 value;
    s32 maxValue;
#ifdef _WIN32
    HANDLE sem;
#else
    sem_t sem;
#endif
} Sync2SemInternal;

typedef struct Sync2QueueInternal {
    u32 initialized;
    u32 head;
    u32 tail;
    u32 count;
    u32 depth;
    u32 elemSize;
    u8* buffer;
#ifdef _WIN32
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE notFull;
    CONDITION_VARIABLE notEmpty;
#else
    pthread_mutex_t mtx;
    pthread_cond_t notFull;
    pthread_cond_t notEmpty;
#endif
} Sync2QueueInternal;

/* Compile-time size checks */
_Static_assert(sizeof(Sync2MutexInternal) <= CELL_SYNC2_MUTEX_SIZE,
               "Sync2MutexInternal too large");
_Static_assert(sizeof(Sync2CondInternal) <= CELL_SYNC2_COND_SIZE,
               "Sync2CondInternal too large");
_Static_assert(sizeof(Sync2SemInternal) <= CELL_SYNC2_SEMAPHORE_SIZE,
               "Sync2SemInternal too large");
_Static_assert(sizeof(Sync2QueueInternal) <= CELL_SYNC2_QUEUE_SIZE,
               "Sync2QueueInternal too large");

/* =========================================================================
 * Mutex
 * =====================================================================*/

s32 cellSync2MutexAttributeInitialize(CellSync2MutexAttribute* attr)
{
    if (!attr) return CELL_SYNC2_ERROR_NULL_POINTER;
    attr->maxWaiters = 32;
    attr->recursive = 0;
    return CELL_OK;
}

s32 cellSync2MutexInit(CellSync2Mutex* mutex, const CellSync2MutexAttribute* attr)
{
    if (!mutex) return CELL_SYNC2_ERROR_NULL_POINTER;

    Sync2MutexInternal* m = (Sync2MutexInternal*)mutex;
    memset(m, 0, sizeof(Sync2MutexInternal));
    m->recursive = attr ? attr->recursive : 0;
    m->initialized = 1;

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

    return CELL_OK;
}

s32 cellSync2MutexLock(CellSync2Mutex* mutex, u64 timeoutUsec)
{
    (void)timeoutUsec; /* TODO: implement timeout */

    if (!mutex) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2MutexInternal* m = (Sync2MutexInternal*)mutex;
    if (!m->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

#ifdef _WIN32
    EnterCriticalSection(&m->cs);
#else
    pthread_mutex_lock(&m->mtx);
#endif

    return CELL_OK;
}

s32 cellSync2MutexTryLock(CellSync2Mutex* mutex)
{
    if (!mutex) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2MutexInternal* m = (Sync2MutexInternal*)mutex;
    if (!m->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

#ifdef _WIN32
    if (!TryEnterCriticalSection(&m->cs))
        return CELL_SYNC2_ERROR_BUSY;
#else
    if (pthread_mutex_trylock(&m->mtx) != 0)
        return CELL_SYNC2_ERROR_BUSY;
#endif

    return CELL_OK;
}

s32 cellSync2MutexUnlock(CellSync2Mutex* mutex)
{
    if (!mutex) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2MutexInternal* m = (Sync2MutexInternal*)mutex;
    if (!m->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

#ifdef _WIN32
    LeaveCriticalSection(&m->cs);
#else
    pthread_mutex_unlock(&m->mtx);
#endif

    return CELL_OK;
}

/* =========================================================================
 * Condition Variable
 * =====================================================================*/

s32 cellSync2CondAttributeInitialize(CellSync2CondAttribute* attr)
{
    if (!attr) return CELL_SYNC2_ERROR_NULL_POINTER;
    attr->maxWaiters = 32;
    return CELL_OK;
}

s32 cellSync2CondInit(CellSync2Cond* cond, CellSync2Mutex* mutex,
                      const CellSync2CondAttribute* attr)
{
    (void)attr;

    if (!cond || !mutex) return CELL_SYNC2_ERROR_NULL_POINTER;

    Sync2CondInternal* c = (Sync2CondInternal*)cond;
    memset(c, 0, sizeof(Sync2CondInternal));
    c->mutex = (Sync2MutexInternal*)mutex;
    c->initialized = 1;

#ifdef _WIN32
    InitializeConditionVariable(&c->cv);
#else
    pthread_cond_init(&c->cv, NULL);
#endif

    return CELL_OK;
}

s32 cellSync2CondWait(CellSync2Cond* cond, u64 timeoutUsec)
{
    if (!cond) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2CondInternal* c = (Sync2CondInternal*)cond;
    if (!c->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

#ifdef _WIN32
    DWORD ms = (timeoutUsec == 0) ? INFINITE : (DWORD)(timeoutUsec / 1000);
    if (!SleepConditionVariableCS(&c->cv, &c->mutex->cs, ms))
        return (GetLastError() == ERROR_TIMEOUT)
               ? CELL_SYNC2_ERROR_TIMEDOUT
               : CELL_SYNC2_ERROR_STAT;
#else
    if (timeoutUsec == 0) {
        pthread_cond_wait(&c->cv, &c->mutex->mtx);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += (time_t)(timeoutUsec / 1000000);
        ts.tv_nsec += (long)((timeoutUsec % 1000000) * 1000);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        int rc = pthread_cond_timedwait(&c->cv, &c->mutex->mtx, &ts);
        if (rc == ETIMEDOUT)
            return CELL_SYNC2_ERROR_TIMEDOUT;
    }
#endif

    return CELL_OK;
}

s32 cellSync2CondSignal(CellSync2Cond* cond)
{
    if (!cond) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2CondInternal* c = (Sync2CondInternal*)cond;
    if (!c->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

#ifdef _WIN32
    WakeConditionVariable(&c->cv);
#else
    pthread_cond_signal(&c->cv);
#endif

    return CELL_OK;
}

s32 cellSync2CondSignalAll(CellSync2Cond* cond)
{
    if (!cond) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2CondInternal* c = (Sync2CondInternal*)cond;
    if (!c->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

#ifdef _WIN32
    WakeAllConditionVariable(&c->cv);
#else
    pthread_cond_broadcast(&c->cv);
#endif

    return CELL_OK;
}

/* =========================================================================
 * Semaphore
 * =====================================================================*/

s32 cellSync2SemaphoreAttributeInitialize(CellSync2SemaphoreAttribute* attr)
{
    if (!attr) return CELL_SYNC2_ERROR_NULL_POINTER;
    attr->maxWaiters = 32;
    attr->maxValue = 0x7FFFFFFF;
    return CELL_OK;
}

s32 cellSync2SemaphoreInit(CellSync2Semaphore* sem, s32 initialValue,
                           const CellSync2SemaphoreAttribute* attr)
{
    if (!sem) return CELL_SYNC2_ERROR_NULL_POINTER;

    Sync2SemInternal* s = (Sync2SemInternal*)sem;
    memset(s, 0, sizeof(Sync2SemInternal));
    s->value = initialValue;
    s->maxValue = attr ? attr->maxValue : 0x7FFFFFFF;
    s->initialized = 1;

#ifdef _WIN32
    s->sem = CreateSemaphoreW(NULL, initialValue, s->maxValue, NULL);
    if (!s->sem) return CELL_SYNC2_ERROR_STAT;
#else
    if (sem_init(&s->sem, 0, (unsigned)initialValue) != 0)
        return CELL_SYNC2_ERROR_STAT;
#endif

    return CELL_OK;
}

s32 cellSync2SemaphoreAcquire(CellSync2Semaphore* sem, u32 count,
                              u64 timeoutUsec)
{
    if (!sem) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2SemInternal* s = (Sync2SemInternal*)sem;
    if (!s->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

    for (u32 i = 0; i < count; i++) {
#ifdef _WIN32
        DWORD ms = (timeoutUsec == 0) ? INFINITE : (DWORD)(timeoutUsec / 1000);
        DWORD rc = WaitForSingleObject(s->sem, ms);
        if (rc == WAIT_TIMEOUT) return CELL_SYNC2_ERROR_TIMEDOUT;
        if (rc != WAIT_OBJECT_0) return CELL_SYNC2_ERROR_STAT;
#else
        if (timeoutUsec == 0) {
            sem_wait(&s->sem);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec  += (time_t)(timeoutUsec / 1000000);
            ts.tv_nsec += (long)((timeoutUsec % 1000000) * 1000);
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            if (sem_timedwait(&s->sem, &ts) != 0) {
                if (errno == ETIMEDOUT) return CELL_SYNC2_ERROR_TIMEDOUT;
                return CELL_SYNC2_ERROR_STAT;
            }
        }
#endif
    }

    return CELL_OK;
}

s32 cellSync2SemaphoreTryAcquire(CellSync2Semaphore* sem, u32 count)
{
    if (!sem) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2SemInternal* s = (Sync2SemInternal*)sem;
    if (!s->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

    for (u32 i = 0; i < count; i++) {
#ifdef _WIN32
        DWORD rc = WaitForSingleObject(s->sem, 0);
        if (rc == WAIT_TIMEOUT) return CELL_SYNC2_ERROR_BUSY;
        if (rc != WAIT_OBJECT_0) return CELL_SYNC2_ERROR_STAT;
#else
        if (sem_trywait(&s->sem) != 0) {
            if (errno == EAGAIN) return CELL_SYNC2_ERROR_BUSY;
            return CELL_SYNC2_ERROR_STAT;
        }
#endif
    }

    return CELL_OK;
}

s32 cellSync2SemaphoreRelease(CellSync2Semaphore* sem, u32 count)
{
    if (!sem) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2SemInternal* s = (Sync2SemInternal*)sem;
    if (!s->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

    for (u32 i = 0; i < count; i++) {
#ifdef _WIN32
        ReleaseSemaphore(s->sem, 1, NULL);
#else
        sem_post(&s->sem);
#endif
    }

    return CELL_OK;
}

s32 cellSync2SemaphoreGetValue(const CellSync2Semaphore* sem, s32* value)
{
    if (!sem || !value) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2SemInternal* s = (Sync2SemInternal*)sem;
    if (!s->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

#ifdef _WIN32
    /* Windows doesn't have a direct "get value" for semaphores.
     * Use a 0-timeout wait + release trick, or track internally. */
    *value = s->value; /* approximate */
#else
    int v;
    sem_getvalue(&s->sem, &v);
    *value = (s32)v;
#endif

    return CELL_OK;
}

/* =========================================================================
 * Queue
 * =====================================================================*/

s32 cellSync2QueueAttributeInitialize(CellSync2QueueAttribute* attr)
{
    if (!attr) return CELL_SYNC2_ERROR_NULL_POINTER;
    attr->maxPushWaiters = 32;
    attr->maxPopWaiters  = 32;
    return CELL_OK;
}

s32 cellSync2QueueInit(CellSync2Queue* queue, void* buffer,
                       u32 elemSize, u32 depth,
                       const CellSync2QueueAttribute* attr)
{
    (void)attr;

    if (!queue || !buffer) return CELL_SYNC2_ERROR_NULL_POINTER;
    if (elemSize == 0 || depth == 0) return CELL_SYNC2_ERROR_INVAL;

    Sync2QueueInternal* q = (Sync2QueueInternal*)queue;
    memset(q, 0, sizeof(Sync2QueueInternal));
    q->depth = depth;
    q->elemSize = elemSize;
    q->buffer = (u8*)buffer;
    q->initialized = 1;

    memset(buffer, 0, (size_t)elemSize * depth);

#ifdef _WIN32
    InitializeCriticalSection(&q->cs);
    InitializeConditionVariable(&q->notFull);
    InitializeConditionVariable(&q->notEmpty);
#else
    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->notFull, NULL);
    pthread_cond_init(&q->notEmpty, NULL);
#endif

    return CELL_OK;
}

s32 cellSync2QueuePush(CellSync2Queue* queue, const void* data,
                       u64 timeoutUsec)
{
    if (!queue || !data) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2QueueInternal* q = (Sync2QueueInternal*)queue;
    if (!q->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

#ifdef _WIN32
    EnterCriticalSection(&q->cs);
    DWORD ms = (timeoutUsec == 0) ? INFINITE : (DWORD)(timeoutUsec / 1000);
    while (q->count >= q->depth) {
        if (!SleepConditionVariableCS(&q->notFull, &q->cs, ms)) {
            LeaveCriticalSection(&q->cs);
            return CELL_SYNC2_ERROR_TIMEDOUT;
        }
    }

    memcpy(q->buffer + (size_t)q->tail * q->elemSize, data, q->elemSize);
    q->tail = (q->tail + 1) % q->depth;
    q->count++;

    WakeConditionVariable(&q->notEmpty);
    LeaveCriticalSection(&q->cs);
#else
    pthread_mutex_lock(&q->mtx);
    while (q->count >= q->depth) {
        if (timeoutUsec == 0) {
            pthread_cond_wait(&q->notFull, &q->mtx);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec  += (time_t)(timeoutUsec / 1000000);
            ts.tv_nsec += (long)((timeoutUsec % 1000000) * 1000);
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            if (pthread_cond_timedwait(&q->notFull, &q->mtx, &ts) == ETIMEDOUT) {
                pthread_mutex_unlock(&q->mtx);
                return CELL_SYNC2_ERROR_TIMEDOUT;
            }
        }
    }

    memcpy(q->buffer + (size_t)q->tail * q->elemSize, data, q->elemSize);
    q->tail = (q->tail + 1) % q->depth;
    q->count++;

    pthread_cond_signal(&q->notEmpty);
    pthread_mutex_unlock(&q->mtx);
#endif

    return CELL_OK;
}

s32 cellSync2QueueTryPush(CellSync2Queue* queue, const void* data)
{
    if (!queue || !data) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2QueueInternal* q = (Sync2QueueInternal*)queue;
    if (!q->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

#ifdef _WIN32
    if (!TryEnterCriticalSection(&q->cs))
        return CELL_SYNC2_ERROR_BUSY;

    if (q->count >= q->depth) {
        LeaveCriticalSection(&q->cs);
        return CELL_SYNC2_ERROR_OVERFLOW;
    }

    memcpy(q->buffer + (size_t)q->tail * q->elemSize, data, q->elemSize);
    q->tail = (q->tail + 1) % q->depth;
    q->count++;

    WakeConditionVariable(&q->notEmpty);
    LeaveCriticalSection(&q->cs);
#else
    if (pthread_mutex_trylock(&q->mtx) != 0)
        return CELL_SYNC2_ERROR_BUSY;

    if (q->count >= q->depth) {
        pthread_mutex_unlock(&q->mtx);
        return CELL_SYNC2_ERROR_OVERFLOW;
    }

    memcpy(q->buffer + (size_t)q->tail * q->elemSize, data, q->elemSize);
    q->tail = (q->tail + 1) % q->depth;
    q->count++;

    pthread_cond_signal(&q->notEmpty);
    pthread_mutex_unlock(&q->mtx);
#endif

    return CELL_OK;
}

s32 cellSync2QueuePop(CellSync2Queue* queue, void* data,
                      u64 timeoutUsec)
{
    if (!queue || !data) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2QueueInternal* q = (Sync2QueueInternal*)queue;
    if (!q->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

#ifdef _WIN32
    EnterCriticalSection(&q->cs);
    DWORD ms = (timeoutUsec == 0) ? INFINITE : (DWORD)(timeoutUsec / 1000);
    while (q->count == 0) {
        if (!SleepConditionVariableCS(&q->notEmpty, &q->cs, ms)) {
            LeaveCriticalSection(&q->cs);
            return CELL_SYNC2_ERROR_TIMEDOUT;
        }
    }

    memcpy(data, q->buffer + (size_t)q->head * q->elemSize, q->elemSize);
    q->head = (q->head + 1) % q->depth;
    q->count--;

    WakeConditionVariable(&q->notFull);
    LeaveCriticalSection(&q->cs);
#else
    pthread_mutex_lock(&q->mtx);
    while (q->count == 0) {
        if (timeoutUsec == 0) {
            pthread_cond_wait(&q->notEmpty, &q->mtx);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec  += (time_t)(timeoutUsec / 1000000);
            ts.tv_nsec += (long)((timeoutUsec % 1000000) * 1000);
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            if (pthread_cond_timedwait(&q->notEmpty, &q->mtx, &ts) == ETIMEDOUT) {
                pthread_mutex_unlock(&q->mtx);
                return CELL_SYNC2_ERROR_TIMEDOUT;
            }
        }
    }

    memcpy(data, q->buffer + (size_t)q->head * q->elemSize, q->elemSize);
    q->head = (q->head + 1) % q->depth;
    q->count--;

    pthread_cond_signal(&q->notFull);
    pthread_mutex_unlock(&q->mtx);
#endif

    return CELL_OK;
}

s32 cellSync2QueueTryPop(CellSync2Queue* queue, void* data)
{
    if (!queue || !data) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2QueueInternal* q = (Sync2QueueInternal*)queue;
    if (!q->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

#ifdef _WIN32
    if (!TryEnterCriticalSection(&q->cs))
        return CELL_SYNC2_ERROR_BUSY;

    if (q->count == 0) {
        LeaveCriticalSection(&q->cs);
        return CELL_SYNC2_ERROR_EMPTY;
    }

    memcpy(data, q->buffer + (size_t)q->head * q->elemSize, q->elemSize);
    q->head = (q->head + 1) % q->depth;
    q->count--;

    WakeConditionVariable(&q->notFull);
    LeaveCriticalSection(&q->cs);
#else
    if (pthread_mutex_trylock(&q->mtx) != 0)
        return CELL_SYNC2_ERROR_BUSY;

    if (q->count == 0) {
        pthread_mutex_unlock(&q->mtx);
        return CELL_SYNC2_ERROR_EMPTY;
    }

    memcpy(data, q->buffer + (size_t)q->head * q->elemSize, q->elemSize);
    q->head = (q->head + 1) % q->depth;
    q->count--;

    pthread_cond_signal(&q->notFull);
    pthread_mutex_unlock(&q->mtx);
#endif

    return CELL_OK;
}

s32 cellSync2QueueSize(const CellSync2Queue* queue, u32* size)
{
    if (!queue || !size) return CELL_SYNC2_ERROR_NULL_POINTER;
    const Sync2QueueInternal* q = (const Sync2QueueInternal*)queue;
    if (!q->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

    *size = q->count;
    return CELL_OK;
}
