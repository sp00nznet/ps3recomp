/*
 * ps3recomp - counting semaphore for POSIX hosts
 *
 * <semaphore.h> is the obvious tool and the wrong one: Darwin implements
 * sem_init as a stub that fails with ENOSYS (only named semaphores work), and
 * ships no sem_timedwait at all. A mutex and a condition variable give exact
 * semantics, a real timed wait, and one code path on every host -- so the
 * Linux CI run exercises the same lines a Mac executes.
 *
 * The pthread objects live in one heap block behind a pointer, not inline.
 * Some callers embed the semaphore in a guest-visible structure of fixed size
 * (CellSync2Semaphore is 128 bytes), and Darwin's pthread_mutex_t (64 B) plus
 * pthread_cond_t (48 B) do not fit there beside the caller's own fields. The
 * handle is eight bytes everywhere; zero-initialised means "not initialised".
 *
 * Deadlines are absolute CLOCK_REALTIME, matching sem_timedwait, so a caller
 * that computed one for sem_timedwait passes it here unchanged.
 */
#ifndef PS3RECOMP_POSIX_SEM_H
#define PS3RECOMP_POSIX_SEM_H

#ifndef _WIN32

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

typedef struct ps3_sem_impl {
    pthread_mutex_t m;
    pthread_cond_t  c;
    unsigned        count;
} ps3_sem_impl;

typedef struct { ps3_sem_impl* p; } ps3_sem_t;

static inline int ps3_sem_init(ps3_sem_t* s, unsigned initial)
{
    ps3_sem_impl* p = (ps3_sem_impl*)calloc(1, sizeof *p);
    if (!p) { errno = ENOMEM; return -1; }
    int rc = pthread_mutex_init(&p->m, NULL);
    if (rc != 0) { free(p); errno = rc; return -1; }
    rc = pthread_cond_init(&p->c, NULL);
    if (rc != 0) { pthread_mutex_destroy(&p->m); free(p); errno = rc; return -1; }
    p->count = initial;
    s->p = p;
    return 0;
}

static inline int ps3_sem_destroy(ps3_sem_t* s)
{
    ps3_sem_impl* p = s->p;
    if (!p) return 0;
    pthread_cond_destroy(&p->c);
    pthread_mutex_destroy(&p->m);
    free(p);
    s->p = NULL;
    return 0;
}

static inline int ps3_sem_post(ps3_sem_t* s)
{
    ps3_sem_impl* p = s->p;
    pthread_mutex_lock(&p->m);
    p->count++;
    pthread_cond_signal(&p->c);
    pthread_mutex_unlock(&p->m);
    return 0;
}

static inline int ps3_sem_wait(ps3_sem_t* s)
{
    ps3_sem_impl* p = s->p;
    pthread_mutex_lock(&p->m);
    while (p->count == 0) pthread_cond_wait(&p->c, &p->m);
    p->count--;
    pthread_mutex_unlock(&p->m);
    return 0;
}

static inline int ps3_sem_trywait(ps3_sem_t* s)
{
    ps3_sem_impl* p = s->p;
    pthread_mutex_lock(&p->m);
    int ok = p->count > 0;
    if (ok) p->count--;
    pthread_mutex_unlock(&p->m);
    if (!ok) { errno = EAGAIN; return -1; }
    return 0;
}

static inline int ps3_sem_getvalue(ps3_sem_t* s, int* value)
{
    ps3_sem_impl* p = s->p;
    pthread_mutex_lock(&p->m);
    *value = (int)p->count;
    pthread_mutex_unlock(&p->m);
    return 0;
}

/* Returns 0 on success, -1 with errno = ETIMEDOUT when the deadline passes. */
static inline int ps3_sem_timedwait(ps3_sem_t* s, const struct timespec* abs_deadline)
{
    ps3_sem_impl* p = s->p;
    pthread_mutex_lock(&p->m);
    int rc = 0;
    while (p->count == 0 && rc == 0)
        rc = pthread_cond_timedwait(&p->c, &p->m, abs_deadline);
    int ok = p->count > 0;         /* a post that raced the deadline still counts */
    if (ok) p->count--;
    pthread_mutex_unlock(&p->m);
    if (!ok) { errno = ETIMEDOUT; return -1; }
    return 0;
}

#endif /* !_WIN32 */
#endif /* PS3RECOMP_POSIX_SEM_H */
