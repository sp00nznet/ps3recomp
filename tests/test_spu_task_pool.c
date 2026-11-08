/* runtime/spu/spu_task_pool.h, the opt-in host thread pool for async SPU
 * tasks (PS3RECOMP_SPU_TASK_POOL). Synthetic: plain host tasks, no SPU code.
 *
 *   reuse      back-to-back tasks run on one reused thread
 *   no-queue   tasks that wait for each other all start at once (each task
 *              blocks until every one of them is running), and a task
 *              submitted while every pool thread is busy gets a new thread
 *   stress     submitters on several threads race completions, hand-offs
 *              and retirements; every task runs exactly once, and each
 *              task's record may be freed by the task itself
 *   retire     idle threads exit after idle_ms, leaving none behind
 *
 * tools/test_spu_task_pool.py builds and runs it.
 */
#include "spu_task_pool.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_cv = PTHREAD_COND_INITIALIZER;
static unsigned long   s_done, s_running, s_peak, s_started;

typedef struct {
    spu_task_pool_item item;      /* embedded, as spu_async_job does */
    unsigned  id;
    unsigned  gate;               /* no-queue: wait until this many started */
    unsigned* runs;               /* stress: per-id run counter */
} task;

static void task_run(void* p)
{
    task* t = (task*)p;
    pthread_mutex_lock(&s_mu);
    s_running++;
    s_started++;
    if (s_running > s_peak) s_peak = s_running;
    pthread_cond_broadcast(&s_cv);
    if (t->gate) {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += 10;   /* a deadlock fails the test instead of hanging */
        while (s_started < t->gate)
            if (pthread_cond_timedwait(&s_cv, &s_mu, &until) == ETIMEDOUT) break;
    }
    if (t->runs) __atomic_add_fetch(&t->runs[t->id], 1, __ATOMIC_RELAXED);
    s_running--;
    s_done++;
    pthread_cond_broadcast(&s_cv);
    pthread_mutex_unlock(&s_mu);
    free(t);   /* the task owns its record, item included */
}

static task* new_task(unsigned id, unsigned gate, unsigned* runs)
{
    task* t = (task*)calloc(1, sizeof *t);
    t->id = id; t->gate = gate; t->runs = runs;
    return t;
}

static void wait_done(unsigned long n)
{
    pthread_mutex_lock(&s_mu);
    while (s_done < n) pthread_cond_wait(&s_cv, &s_mu);
    pthread_mutex_unlock(&s_mu);
}

/* Wait until `idle` pool threads are parked (so the next submit can reuse). */
static int wait_idle(spu_task_pool* pool, unsigned idle)
{
    for (int i = 0; i < 2000; i++) {
        pthread_mutex_lock(&pool->mu);
        const unsigned now = pool->idle;
        pthread_mutex_unlock(&pool->mu);
        if (now >= idle) return 1;
        usleep(1000);
    }
    return 0;
}

static int wait_threads(spu_task_pool* pool, unsigned threads, int ms)
{
    for (int i = 0; i < ms; i++) {
        pthread_mutex_lock(&pool->mu);
        const unsigned now = pool->threads;
        pthread_mutex_unlock(&pool->mu);
        if (now == threads) return 1;
        usleep(1000);
    }
    return 0;
}

static void reset(void) { s_done = s_running = s_peak = s_started = 0; }

static void test_reuse(void)
{
    static spu_task_pool pool = SPU_TASK_POOL_INIT(5000u, 1u << 20);
    reset();
    for (unsigned i = 0; i < 100; i++) {
        task* t = new_task(i, 0, NULL);
        CHECK(spu_task_pool_submit(&pool, &t->item, task_run, t) == 1);
        wait_done(i + 1);
        CHECK(wait_idle(&pool, 1));
    }
    CHECK(pool.created == 1);
    CHECK(pool.reused == 99);
    CHECK(pool.threads == 1);
    printf("reuse: 100 tasks, created=%llu reused=%llu\n", pool.created, pool.reused);
}

static void test_no_queue(void)
{
    static spu_task_pool pool = SPU_TASK_POOL_INIT(5000u, 1u << 20);
    enum { K = 16 };
    reset();
    /* Warm the pool with K idle threads... */
    for (unsigned i = 0; i < K; i++) {
        task* t = new_task(i, K, NULL);
        CHECK(spu_task_pool_submit(&pool, &t->item, task_run, t) == 1);
    }
    wait_done(K);
    CHECK(s_peak == K);
    CHECK(wait_idle(&pool, K));
    /* ...then 2K mutually waiting tasks: K reuse idle threads, K need new
     * ones, and none may wait in the queue behind another. */
    reset();
    for (unsigned i = 0; i < 2 * K; i++) {
        task* t = new_task(i, 2 * K, NULL);
        CHECK(spu_task_pool_submit(&pool, &t->item, task_run, t) == 1);
    }
    wait_done(2 * K);
    CHECK(s_peak == 2 * K);
    CHECK(pool.created == 2 * K);
    CHECK(pool.reused == K);
    printf("no-queue: %u mutually waiting tasks all ran at once (created=%llu reused=%llu)\n",
           2 * K, pool.created, pool.reused);
}

typedef struct { spu_task_pool* pool; unsigned first, count; unsigned* runs; } submitter;

static void* submit_many(void* p)
{
    submitter* s = (submitter*)p;
    for (unsigned i = 0; i < s->count; i++) {
        task* t = new_task(s->first + i, 0, s->runs);
        if (spu_task_pool_submit(s->pool, &t->item, task_run, t) != 1) { free(t); abort(); }
        if ((i & 63) == 0) usleep(50);   /* let some threads go idle */
    }
    return NULL;
}

static void test_stress_and_retire(void)
{
    static spu_task_pool pool = SPU_TASK_POOL_INIT(20u, 1u << 20);
    enum { SUBMITTERS = 4, PER = 5000, N = SUBMITTERS * PER };
    unsigned* runs = (unsigned*)calloc(N, sizeof *runs);
    reset();
    for (int round = 0; round < 2; round++) {
        pthread_t th[SUBMITTERS];
        submitter sub[SUBMITTERS];
        memset(runs, 0, N * sizeof *runs);
        const unsigned long base = s_done;
        for (int k = 0; k < SUBMITTERS; k++) {
            sub[k].pool = &pool; sub[k].first = (unsigned)k * PER; sub[k].count = PER; sub[k].runs = runs;
            CHECK(pthread_create(&th[k], NULL, submit_many, &sub[k]) == 0);
        }
        for (int k = 0; k < SUBMITTERS; k++) pthread_join(th[k], NULL);
        wait_done(base + N);
        unsigned bad = 0;
        for (unsigned i = 0; i < N; i++) bad += runs[i] != 1;
        CHECK(bad == 0);
        /* Every idle thread retires after idle_ms; the next round starts empty. */
        CHECK(wait_threads(&pool, 0, 3000));
        pthread_mutex_lock(&pool.mu);
        CHECK(pool.idle == 0 && pool.queued == 0 && pool.head == NULL);
        CHECK(pool.retired == pool.created);
        pthread_mutex_unlock(&pool.mu);
    }
    CHECK(pool.created + pool.reused == 2ull * N);
    CHECK(pool.reused > 0);
    printf("stress: %u tasks from %d submitters, created=%llu reused=%llu retired=%llu\n",
           2 * N, SUBMITTERS, pool.created, pool.reused, pool.retired);
    free(runs);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_reuse();
    test_no_queue();
    test_stress_and_retire();
    if (failures) { fprintf(stderr, "spu task pool: %d failure(s)\n", failures); return 1; }
    puts("PASS: spu task pool reuses idle threads, never queues a task behind another, retires idle threads");
    return 0;
}
