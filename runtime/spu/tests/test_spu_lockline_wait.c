/* Real coherence lock: a descheduled owner must not make waiters burn a core.
 * Also exercise exclusion and publication with concurrent synthetic writers. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "../spu_coherency.c"

void spu_ch_wake(spu_context* ctx) { (void)ctx; }

static atomic_int waiting;
static double waiter_cpu, waiter_wall;
static uint64_t counter, mirror = UINT64_MAX;

static double seconds(clockid_t clock)
{
    struct timespec t;
    assert(clock_gettime(clock, &t) == 0);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static void* waiter(void* unused)
{
    (void)unused;
    double cpu = seconds(CLOCK_THREAD_CPUTIME_ID);
    double wall = seconds(CLOCK_MONOTONIC);
    atomic_store_explicit(&waiting, 1, memory_order_release);
    spu_lockline_lock();
    waiter_cpu = seconds(CLOCK_THREAD_CPUTIME_ID) - cpu;
    waiter_wall = seconds(CLOCK_MONOTONIC) - wall;
    assert(counter == 42 && mirror == ~UINT64_C(42));
    spu_lockline_unlock();
    return NULL;
}

static void* writer(void* unused)
{
    (void)unused;
    for (unsigned i = 0; i < 50000; ++i) {
        spu_lockline_lock();
        assert(mirror == ~counter);
        ++counter;
        mirror = ~counter;
        spu_lockline_unlock();
    }
    return NULL;
}

int main(void)
{
    pthread_t thread;
    spu_lockline_lock();
    assert(pthread_create(&thread, NULL, waiter, NULL) == 0);
    while (!atomic_load_explicit(&waiting, memory_order_acquire)) {
        struct timespec delay = {0, 1000000};
        nanosleep(&delay, NULL);
    }
    struct timespec hold = {0, 250000000};
    while (nanosleep(&hold, &hold) != 0) {}
    counter = 42;
    mirror = ~counter;
    spu_lockline_unlock();
    assert(pthread_join(thread, NULL) == 0);
    printf("owner hold: waiter wall %.6fs, CPU %.6fs\n", waiter_wall, waiter_cpu);
    fflush(stdout);
    assert(waiter_wall >= .20);
    assert(waiter_cpu < .05);

    pthread_t writers[8];
    counter = 0;
    mirror = ~counter;
    for (unsigned i = 0; i < 8; ++i)
        assert(pthread_create(&writers[i], NULL, writer, NULL) == 0);
    for (unsigned i = 0; i < 8; ++i)
        assert(pthread_join(writers[i], NULL) == 0);
    assert(counter == 400000 && mirror == ~UINT64_C(400000));
    puts("coherence lock wait, exclusion and publication passed");
    return 0;
}
