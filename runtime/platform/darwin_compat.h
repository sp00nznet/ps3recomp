/*
 * ps3recomp - POSIX functions absent on Darwin
 *
 * macOS ships no pthread_mutex_timedlock (POSIX 2001). It is emulated by
 * polling pthread_mutex_trylock against the caller's absolute CLOCK_REALTIME
 * deadline. Contended waits here are rare and short (guest sync primitives
 * with millisecond timeouts), so the poll interval costs less than pulling in
 * a full futex-style reimplementation.
 *
 * sem_timedwait is missing on Darwin as well, and sem_init is a stub that
 * fails with ENOSYS. Nothing in the tree uses POSIX semaphores any more:
 * runtime/platform/posix_sem.h is the counting semaphore, on every host.
 */
#ifndef PS3RECOMP_DARWIN_COMPAT_H
#define PS3RECOMP_DARWIN_COMPAT_H

#if defined(__APPLE__)

#include <errno.h>
#include <pthread.h>
#include <time.h>

/* Poll granularity. 200 us keeps worst-case overshoot well under the 1 ms
 * resolution the guest timeout APIs actually express. */
#define PS3_TIMEDWAIT_POLL_NS  200000L

static inline int ps3__deadline_passed(const struct timespec* abs)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    if (now.tv_sec  != abs->tv_sec) return now.tv_sec > abs->tv_sec;
    return now.tv_nsec >= abs->tv_nsec;
}

static inline void ps3__poll_sleep(void)
{
    struct timespec d = { 0, PS3_TIMEDWAIT_POLL_NS };
    nanosleep(&d, NULL);
}

static inline int pthread_mutex_timedlock(pthread_mutex_t* m,
                                          const struct timespec* abstime)
{
    for (;;) {
        int rc = pthread_mutex_trylock(m);
        if (rc != EBUSY) return rc;              /* acquired, or a real error */
        if (ps3__deadline_passed(abstime)) return ETIMEDOUT;
        ps3__poll_sleep();
    }
}

#endif /* __APPLE__ */
#endif /* PS3RECOMP_DARWIN_COMPAT_H */
