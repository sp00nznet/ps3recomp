/*
 * ps3recomp - a host thread pool for async SPU tasks (POSIX)
 *
 * spu_workload_dispatch_async_owned runs each SPU task on its own detached host
 * thread, and a title can start several per frame. Each new thread pays the
 * creation, a cold stack and fresh thread-local state again. This pool runs
 * the tasks on reused threads without changing what a task can rely on:
 *
 *  - Every task still starts at once, on a thread of its own. A task is handed
 *    to the pool only when more pool threads are idle and waiting than tasks
 *    are already queued for them; otherwise a new pool thread is created for
 *    it, exactly as without the pool. So a task that waits for another task
 *    can never be queued behind it, and there are never fewer threads than
 *    running tasks.
 *  - A pool thread that has been idle for idle_ms without work exits.
 *
 * The pool owns no task memory. spu_task_pool_submit() takes an item the
 * caller embeds in its own task record; the item must stay valid until the
 * task starts running, and the run function may free it.
 *
 * Header-only so the synthetic test (tests/test_spu_task_pool.c) can drive it
 * directly. POSIX only: Windows keeps one thread per task.
 */
#ifndef PS3RECOMP_SPU_TASK_POOL_H
#define PS3RECOMP_SPU_TASK_POOL_H

#if !defined(_WIN32)
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <time.h>

struct spu_task_pool;

typedef struct spu_task_pool_item {
    void (*run)(void* arg);
    void* arg;
    struct spu_task_pool* pool;
    struct spu_task_pool_item* next;
} spu_task_pool_item;

typedef struct spu_task_pool {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    spu_task_pool_item* head;
    spu_task_pool_item* tail;
    unsigned queued;               /* handed off, not yet picked up */
    unsigned idle;                 /* pool threads waiting for work */
    unsigned threads;              /* pool threads alive */
    unsigned long long reused;     /* tasks handed to an idle thread */
    unsigned long long created;    /* tasks that got a new thread */
    unsigned long long retired;    /* threads that exited idle */
    unsigned idle_ms;              /* idle time before a thread exits */
    size_t   stack_size;           /* per-thread stack reservation; 0 = default */
} spu_task_pool;

#define SPU_TASK_POOL_INIT(idle_ms, stack_size)                          \
    { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, NULL, NULL,   \
      0, 0, 0, 0, 0, 0, (idle_ms), (stack_size) }

static inline void* spu_task_pool_thread(void* p)
{
    spu_task_pool_item* it = (spu_task_pool_item*)p;
    spu_task_pool* pool = it->pool;
    for (;;) {
        /* Copy out first: the task may free the record the item lives in. */
        void (*run)(void*) = it->run;
        void* arg = it->arg;
        run(arg);

        pthread_mutex_lock(&pool->mu);
        pool->idle++;
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec  += (time_t)(pool->idle_ms / 1000u);
        until.tv_nsec += (long)(pool->idle_ms % 1000u) * 1000000L;
        if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
        int timed_out = 0;
        while (!pool->head && !timed_out)
            timed_out = pthread_cond_timedwait(&pool->cv, &pool->mu, &until) == ETIMEDOUT;
        pool->idle--;
        /* A queued task is taken even after a timeout: it was handed off
         * while this thread still counted as idle. */
        if (!pool->head) {
            pool->threads--;
            pool->retired++;
            pthread_mutex_unlock(&pool->mu);
            return NULL;
        }
        it = pool->head;
        pool->head = it->next;
        if (!pool->head) pool->tail = NULL;
        pool->queued--;
        pthread_mutex_unlock(&pool->mu);
    }
}

/* Start run(arg) now: on an idle pool thread if one is free, else on a new
 * one. 1 when the task is running or about to, 0 when no thread could be
 * created (the item is then unused and the caller still owns its task). */
static inline int spu_task_pool_submit(spu_task_pool* pool, spu_task_pool_item* it,
                                void (*run)(void*), void* arg)
{
    it->run = run;
    it->arg = arg;
    it->pool = pool;
    it->next = NULL;

    pthread_mutex_lock(&pool->mu);
    if (pool->idle > pool->queued) {
        if (pool->tail) pool->tail->next = it; else pool->head = it;
        pool->tail = it;
        pool->queued++;
        pool->reused++;
        pthread_cond_signal(&pool->cv);
        pthread_mutex_unlock(&pool->mu);
        return 1;
    }
    /* Counted before the thread runs, so a later hand-off sees it. */
    pool->threads++;
    pool->created++;
    pthread_mutex_unlock(&pool->mu);

    pthread_t th;
    int rc = EINVAL;
    if (pool->stack_size) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        if (pthread_attr_setstacksize(&attr, pool->stack_size) == 0)
            rc = pthread_create(&th, &attr, spu_task_pool_thread, it);
        pthread_attr_destroy(&attr);
    }
    /* A host that will not reserve that much still gets the thread, on the
     * default stack, as the thread-per-task path does. */
    if (rc != 0)
        rc = pthread_create(&th, NULL, spu_task_pool_thread, it);
    if (rc != 0) {
        pthread_mutex_lock(&pool->mu);
        pool->threads--;
        pool->created--;
        pthread_mutex_unlock(&pool->mu);
        return 0;
    }
    pthread_detach(th);
    return 1;
}
#endif /* !_WIN32 */

#endif /* PS3RECOMP_SPU_TASK_POOL_H */
