#ifndef PS3RECOMP_RSX_VK_THREAD_H
#define PS3RECOMP_RSX_VK_THREAD_H
/* Mutex and clock primitives for the Vulkan backend.
 *
 * Off Windows this is just <pthread.h> and <time.h>. On Windows the backend
 * uses the small subset below -- non-recursive mutexes, CLOCK_MONOTONIC and
 * CLOCK_REALTIME -- mapped onto SRW locks and the performance counter. The
 * names are macros onto rsx_vk_-prefixed definitions, and this header is
 * included only by the Vulkan backend, so nothing collides with a real
 * pthreads header another translation unit may use. */
#ifndef _WIN32
#include <pthread.h>
#include <time.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <time.h>

typedef SRWLOCK rsx_vk_mutex_t;
#define pthread_mutex_t rsx_vk_mutex_t
#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT

static inline int rsx_vk_mutex_init(rsx_vk_mutex_t *m, const void *attr)
{ (void)attr; InitializeSRWLock(m); return 0; }
static inline int rsx_vk_mutex_destroy(rsx_vk_mutex_t *m) { (void)m; return 0; }
static inline int rsx_vk_mutex_lock(rsx_vk_mutex_t *m) { AcquireSRWLockExclusive(m); return 0; }
static inline int rsx_vk_mutex_unlock(rsx_vk_mutex_t *m) { ReleaseSRWLockExclusive(m); return 0; }
/* 16 is EBUSY, the only failure callers distinguish (nonzero = not taken). */
static inline int rsx_vk_mutex_trylock(rsx_vk_mutex_t *m)
{ return TryAcquireSRWLockExclusive(m) ? 0 : 16; }
#define pthread_mutex_init rsx_vk_mutex_init
#define pthread_mutex_destroy rsx_vk_mutex_destroy
#define pthread_mutex_lock rsx_vk_mutex_lock
#define pthread_mutex_unlock rsx_vk_mutex_unlock
#define pthread_mutex_trylock rsx_vk_mutex_trylock

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
static inline int rsx_vk_clock_gettime(int clock, struct timespec *ts)
{
    if (clock == CLOCK_MONOTONIC) {
        LARGE_INTEGER f, c;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        ts->tv_sec = (time_t)(c.QuadPart / f.QuadPart);
        ts->tv_nsec = (long)((c.QuadPart % f.QuadPart) * 1000000000LL / f.QuadPart);
        return 0;
    }
    /* FILETIME: 100 ns units since 1601-01-01; the Unix epoch is 11644473600 s later. */
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    const unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    const unsigned long long unix100 = t - 116444736000000000ULL;
    ts->tv_sec = (time_t)(unix100 / 10000000ULL);
    ts->tv_nsec = (long)((unix100 % 10000000ULL) * 100ULL);
    return 0;
}
#undef clock_gettime
#define clock_gettime rsx_vk_clock_gettime
#endif
#endif
