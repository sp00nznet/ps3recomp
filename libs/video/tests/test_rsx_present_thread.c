/* The present thread's ring and drop policy, with synthetic hooks: no GPU, no
 * window. Built and run by tools/test_rsx_present_thread.py. */
#undef NDEBUG
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "rsx_present_thread.h"

typedef struct fake {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    uint32_t slot_surface[RSX_PRESENT_THREAD_MAX_DEPTH];
    uint32_t shown[64], n_shown;
    uint32_t discarded[64], n_discarded;
    uint32_t entered;          /* shows begun */
    int gate_closed;           /* shows wait while set */
    int fail_stage_of;         /* surface whose stage fails, 0 none */
    int show_returns_zero;     /* simulate "no display image" */
    pthread_t show_thread;
    int show_thread_seen;
} fake;

static int fake_stage(void *user, uint32_t slot, uint32_t surface)
{
    fake *f = (fake *)user;
    pthread_mutex_lock(&f->mu);
    if (f->show_thread_seen) assert(!pthread_equal(pthread_self(), f->show_thread));
    const int fail = (int)surface == f->fail_stage_of;
    if (!fail) f->slot_surface[slot] = surface;
    pthread_mutex_unlock(&f->mu);
    return !fail;
}

static int fake_show(void *user, uint32_t slot, rsx_present_show_timing *t)
{
    fake *f = (fake *)user;
    pthread_mutex_lock(&f->mu);
    f->show_thread = pthread_self(); f->show_thread_seen = 1;
    f->entered++;
    pthread_cond_broadcast(&f->cv);
    while (f->gate_closed) pthread_cond_wait(&f->cv, &f->mu);
    if (!f->show_returns_zero) f->shown[f->n_shown++] = f->slot_surface[slot];
    const int r = !f->show_returns_zero;
    pthread_mutex_unlock(&f->mu);
    t->acquire_ns = 1000000;  /* 1 ms */
    t->blit_ns = 500000;      /* 0.5 ms */
    return r;
}

static void fake_discard(void *user, uint32_t slot)
{
    fake *f = (fake *)user;
    pthread_mutex_lock(&f->mu);
    f->discarded[f->n_discarded++] = f->slot_surface[slot];
    pthread_mutex_unlock(&f->mu);
}

static void fake_init(fake *f)
{
    memset(f, 0, sizeof *f);
    pthread_mutex_init(&f->mu, NULL);
    pthread_cond_init(&f->cv, NULL);
}

static rsx_present_hooks hooks_for(fake *f)
{
    rsx_present_hooks h = { f, fake_stage, fake_show, fake_discard };
    return h;
}

static void wait_entered(fake *f, uint32_t n)
{
    pthread_mutex_lock(&f->mu);
    while (f->entered < n) pthread_cond_wait(&f->cv, &f->mu);
    pthread_mutex_unlock(&f->mu);
}

static void wait_shown(fake *f, uint32_t n)
{
    for (int i = 0; i < 5000; i++) {
        pthread_mutex_lock(&f->mu);
        const uint32_t s = f->n_shown;
        pthread_mutex_unlock(&f->mu);
        if (s >= n) return;
        usleep(1000);
    }
    assert(!"frames were not shown");
}

static void set_gate(fake *f, int closed)
{
    pthread_mutex_lock(&f->mu);
    f->gate_closed = closed;
    pthread_cond_broadcast(&f->cv);
    pthread_mutex_unlock(&f->mu);
}

/* Shows every staged frame, in order, on a thread other than the stager's. */
static void test_in_order(void)
{
    fake f; fake_init(&f);
    rsx_present_hooks h = hooks_for(&f);
    rsx_present_thread *pt = rsx_present_thread_create(&h, 3);
    assert(pt);
    for (uint32_t s = 1; s <= 20; s++) {
        assert(rsx_present_thread_submit(pt, s) == 1);
        wait_shown(&f, s);
    }
    for (uint32_t i = 0; i < 20; i++) assert(f.shown[i] == i + 1);
    rsx_present_thread_stats st;
    rsx_present_thread_get_stats(pt, &st);
    assert(st.staged == 20 && st.presented == 20 && st.dropped == 0 && st.depth == 3);
    assert(st.acquire_ns_total == 20ull * 1000000 && st.blit_ns_total == 20ull * 500000);
    assert(st.acquire_ns_max == 1000000 && st.blit_ns_max == 500000);
    assert(st.max_occupancy >= 1);
    rsx_present_thread_destroy(pt);
    assert(f.n_discarded == 0);
    assert(f.show_thread_seen && !pthread_equal(f.show_thread, pthread_self()));
}

/* A show that does not return (no display image yet) never holds up the
 * stager: with every slot taken the oldest READY frame is dropped. */
static void test_drop_oldest(uint32_t depth)
{
    fake f; fake_init(&f);
    f.gate_closed = 1;
    rsx_present_hooks h = hooks_for(&f);
    rsx_present_thread *pt = rsx_present_thread_create(&h, depth);
    assert(pt);
    assert(rsx_present_thread_submit(pt, 1) == 1);
    wait_entered(&f, 1);                     /* frame 1 is SHOWING, blocked */
    const uint32_t n = 10;
    for (uint32_t s = 2; s <= n; s++) assert(rsx_present_thread_submit(pt, s) == 1);
    rsx_present_thread_stats st;
    rsx_present_thread_get_stats(pt, &st);
    /* depth-1 READY slots survive: the newest frames. */
    const uint32_t ready = depth - 1;
    assert(st.staged == n);
    assert(st.dropped == (n - 1) - ready);
    assert(st.max_occupancy == depth && st.occupancy == depth);
    assert(f.n_discarded == st.dropped);
    for (uint32_t i = 0; i < f.n_discarded; i++) assert(f.discarded[i] == i + 2);   /* oldest first */
    set_gate(&f, 0);
    wait_shown(&f, 1 + ready);
    assert(f.shown[0] == 1);
    for (uint32_t i = 0; i < ready; i++) assert(f.shown[1 + i] == n - ready + 1 + i);
    rsx_present_thread_get_stats(pt, &st);
    assert(st.presented == 1 + ready);
    rsx_present_thread_destroy(pt);
}

typedef struct { rsx_present_thread *pt; } destroy_arg;
static void *destroy_main(void *p) { rsx_present_thread_destroy(((destroy_arg *)p)->pt); return NULL; }

/* Shutdown: the show in progress finishes, the thread joins, and frames still
 * staged are discarded rather than shown. */
static void test_shutdown_drains(void)
{
    fake f; fake_init(&f);
    f.gate_closed = 1;
    rsx_present_hooks h = hooks_for(&f);
    rsx_present_thread *pt = rsx_present_thread_create(&h, 3);
    assert(pt);
    assert(rsx_present_thread_submit(pt, 1) == 1);
    wait_entered(&f, 1);
    assert(rsx_present_thread_submit(pt, 2) == 1);
    assert(rsx_present_thread_submit(pt, 3) == 1);
    destroy_arg a = { pt };
    pthread_t t;
    assert(!pthread_create(&t, NULL, destroy_main, &a));
    usleep(20000);                            /* destroy is waiting for the show */
    set_gate(&f, 0);
    assert(!pthread_join(t, NULL));
    assert(f.n_shown == 1 && f.shown[0] == 1);
    assert(f.n_discarded == 2);
    assert((f.discarded[0] == 2 && f.discarded[1] == 3) || (f.discarded[0] == 3 && f.discarded[1] == 2));
}

/* A failed stage frees its slot and shows nothing; a show without a display
 * image counts as show_failed; the ring keeps working after both. */
static void test_failures(void)
{
    fake f; fake_init(&f);
    f.fail_stage_of = 2;
    rsx_present_hooks h = hooks_for(&f);
    rsx_present_thread *pt = rsx_present_thread_create(&h, 2);
    assert(pt);
    assert(rsx_present_thread_submit(pt, 1) == 1);
    wait_shown(&f, 1);
    assert(rsx_present_thread_submit(pt, 2) == 0);
    pthread_mutex_lock(&f.mu); f.show_returns_zero = 1; pthread_mutex_unlock(&f.mu);
    assert(rsx_present_thread_submit(pt, 3) == 1);
    for (int i = 0; i < 5000; i++) {
        rsx_present_thread_stats st; rsx_present_thread_get_stats(pt, &st);
        if (st.show_failed) break;
        usleep(1000);
    }
    pthread_mutex_lock(&f.mu); f.show_returns_zero = 0; pthread_mutex_unlock(&f.mu);
    assert(rsx_present_thread_submit(pt, 4) == 1);
    wait_shown(&f, 2);
    assert(f.shown[0] == 1 && f.shown[1] == 4);
    rsx_present_thread_stats st;
    rsx_present_thread_get_stats(pt, &st);
    assert(st.staged == 3 && st.stage_failed == 1 && st.show_failed == 1 && st.presented == 2);
    rsx_present_thread_destroy(pt);
}

static void test_config(void)
{
    assert(rsx_present_thread_depth_from(NULL) == 3);
    assert(rsx_present_thread_depth_from("") == 3);
    assert(rsx_present_thread_depth_from("1") == 2);
    assert(rsx_present_thread_depth_from("2") == 2);
    assert(rsx_present_thread_depth_from("3") == 3);
    assert(rsx_present_thread_depth_from("9") == 3);
    rsx_present_hooks bad = { NULL, fake_stage, NULL, fake_discard };
    assert(!rsx_present_thread_create(&bad, 3));
    assert(!rsx_present_thread_create(NULL, 3));
    assert(rsx_present_thread_submit(NULL, 1) == 0);
    rsx_present_thread_destroy(NULL);
}

int main(void)
{
    setenv("PS3RECOMP_RSX_PRESENT_THREAD_REPORT_S", "0", 1);
    test_config();
    test_in_order();
    test_drop_oldest(3);
    test_drop_oldest(2);
    test_shutdown_drains();
    test_failures();
    printf("rsx present thread ring PASS: in order, drop oldest (depth 3 and 2), shutdown drain, failures\n");
    return 0;
}
