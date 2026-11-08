/* RSX FIFO busy accounting (rsx_fifo_busy.h) on a scripted clock: exclusive
 * nested categories that add up to wall time, idle selected by the last pass
 * stop, per-update busy, extension categories, other threads ignored, one
 * state across translation units, and the default-off path.
 * Built with test_rsx_fifo_busy_tu2.c by tools/test_rsx_fifo_busy.py. */
#include "../rsx_fifo_busy.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void fifo_busy_tu2_enter(int category);
void fifo_busy_tu2_leave(int category);
const void* fifo_busy_tu2_state(void);

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static uint64_t now_ns;
static uint64_t test_clock(void) { return now_ns; }
static char last_line[4096];
static int lines;
static void test_sink(const char* line) { snprintf(last_line, sizeof last_line, "%s", line); ++lines; }
static void advance_ms(double ms) { now_ns += (uint64_t)(ms * 1e6); }

static double field(const char* name)
{
    char key[64];
    snprintf(key, sizeof key, " %s=", name);
    const char* at = strstr(last_line, key);
    if (!at) return -1;
    return strtod(at + strlen(key), NULL);
}

static void* other_thread(void* unused)
{
    (void)unused;
    rsx_fifo_busy_idle_begin();             /* not the consumer: ignored */
    rsx_fifo_busy_enter(RSX_FIFO_BUSY_DRAW);
    advance_ms(0);
    rsx_fifo_busy_leave(RSX_FIFO_BUSY_DRAW);
    rsx_fifo_busy_flip();
    return NULL;
}

/* One 20 ms update: 5 idle; inside the walk 2 drain, 4 draw (opened in the
 * other translation unit), 1 extension with 3 flush inside it; then a host
 * present call of 1 around a present of 1 around a 1 ms drawable wait; then
 * 2 of event pump. */
static void one_update(int main_category)
{
    rsx_fifo_busy_pass_stopped(RSX_FIFO_BUSY_STOP_PUT);
    rsx_fifo_busy_idle_begin(); advance_ms(5); rsx_fifo_busy_idle_end();
    rsx_fifo_busy_enter(RSX_FIFO_BUSY_DRAIN); advance_ms(2);
    fifo_busy_tu2_enter(RSX_FIFO_BUSY_DRAW); advance_ms(4); fifo_busy_tu2_leave(RSX_FIFO_BUSY_DRAW);
    rsx_fifo_busy_enter(main_category); advance_ms(1);
    rsx_fifo_busy_enter(RSX_FIFO_BUSY_FLUSH); advance_ms(3); rsx_fifo_busy_leave(RSX_FIFO_BUSY_FLUSH);
    rsx_fifo_busy_leave(main_category);
    rsx_fifo_busy_flip();
    rsx_fifo_busy_leave(RSX_FIFO_BUSY_DRAIN);
    rsx_fifo_busy_enter(RSX_FIFO_BUSY_PRESENT_HOST); advance_ms(1);
    rsx_fifo_busy_enter(RSX_FIFO_BUSY_PRESENT); advance_ms(1);
    rsx_fifo_busy_enter(RSX_FIFO_BUSY_DRAWABLE); advance_ms(1); rsx_fifo_busy_leave(RSX_FIFO_BUSY_DRAWABLE);
    rsx_fifo_busy_leave(RSX_FIFO_BUSY_PRESENT);
    rsx_fifo_busy_leave(RSX_FIFO_BUSY_PRESENT_HOST);
    rsx_fifo_busy_enter(RSX_FIFO_BUSY_PUMP); advance_ms(2); rsx_fifo_busy_leave(RSX_FIFO_BUSY_PUMP);
}

int main(void)
{
    CHECK(fifo_busy_tu2_state() == (const void*)&g_rsx_fifo_busy);

    /* Off: nothing is recorded or printed and categories are refused. */
    rsx_fifo_busy_test_configure(0, test_clock, test_sink, 100000000ull);
    CHECK(!rsx_fifo_busy_enabled());
    CHECK(rsx_fifo_busy_category("main") == -1);
    rsx_fifo_busy_idle_begin(); advance_ms(500); rsx_fifo_busy_idle_end();
    rsx_fifo_busy_enter(RSX_FIFO_BUSY_DRAW); rsx_fifo_busy_flip(); rsx_fifo_busy_leave(RSX_FIFO_BUSY_DRAW);
    CHECK(lines == 0);

    /* On, 100 ms period. */
    now_ns = 1000000000ull;
    rsx_fifo_busy_test_configure(1, test_clock, test_sink, 100000000ull);
    const int main_category = rsx_fifo_busy_category("main");
    CHECK(main_category == RSX_FIFO_BUSY_BUILTIN_COUNT);
    CHECK(rsx_fifo_busy_category("main") == main_category);
    CHECK(rsx_fifo_busy_category("depth") == main_category + 1);
    CHECK(rsx_fifo_busy_category("") == -1);

    rsx_fifo_busy_idle_begin(); rsx_fifo_busy_idle_end();   /* claim the consumer at t=1 s */
    pthread_t t;
    pthread_create(&t, NULL, other_thread, NULL);
    pthread_join(t, NULL);

    for (int i = 0; i < 5; ++i) one_update(main_category);   /* 100 ms */
    rsx_fifo_busy_idle_begin(); rsx_fifo_busy_idle_end();    /* report at the period */
    CHECK(lines == 1);
    fprintf(stderr, "%s\n", last_line);
    CHECK(strstr(last_line, "[rsx fifo busy] ") == last_line);
    CHECK(strstr(last_line, "updates=5 per=update") != NULL);
    CHECK(field("wall") == 20.0);
    CHECK(field("busy") == 15.0);
    CHECK(field("busy_pct") == 75.0);
    CHECK(field("idle_put") == 5.0);
    CHECK(field("drain") == 2.0);
    CHECK(field("draw") == 4.0);
    CHECK(field("flush") == 3.0);
    CHECK(field("main") == 1.0);
    CHECK(field("present_host") == 1.0);
    CHECK(field("present") == 1.0);
    CHECK(field("drawable") == 1.0);
    CHECK(field("pump") == 2.0);
    CHECK(field("other") == 0.0);
    CHECK(field("depth") == -1);                 /* never used: not printed */
    CHECK(field("unbalanced") == 0);
    /* Updates after the first are flip to flip: 20 ms wall, 5 ms idle. */
    CHECK(field("busy_p50") == 15.0);
    CHECK(field("busy_max") == 15.0);
    CHECK(strstr(last_line, "busy_over_16.7=0/4") != NULL);

    /* Idle follows the last stop; unaccounted time is "other". A leave of an
     * outer span closes the inner one and counts as unbalanced. */
    rsx_fifo_busy_pass_stopped(RSX_FIFO_BUSY_STOP_SEMA);
    rsx_fifo_busy_idle_begin(); advance_ms(30); rsx_fifo_busy_idle_end();
    rsx_fifo_busy_pass_stopped(RSX_FIFO_BUSY_STOP_REPORT);
    rsx_fifo_busy_idle_begin(); advance_ms(20); rsx_fifo_busy_idle_end();
    advance_ms(10);                                           /* other */
    rsx_fifo_busy_enter(RSX_FIFO_BUSY_DRAIN);
    rsx_fifo_busy_enter(RSX_FIFO_BUSY_DRAW); advance_ms(40);
    rsx_fifo_busy_leave(RSX_FIFO_BUSY_DRAIN);
    rsx_fifo_busy_leave(RSX_FIFO_BUSY_DRAW);                  /* already closed: ignored */
    rsx_fifo_busy_pass_stopped(RSX_FIFO_BUSY_STOP_PUT);
    rsx_fifo_busy_idle_begin(); rsx_fifo_busy_idle_end();
    CHECK(lines == 2);
    fprintf(stderr, "%s\n", last_line);
    CHECK(strstr(last_line, "updates=0 per=second") != NULL);
    CHECK(field("wall") == 1000.0);                           /* 100 ms per 0.1 s */
    CHECK(field("idle_sema") == 300.0);
    CHECK(field("idle_report") == 200.0);
    CHECK(field("other") == 100.0);
    CHECK(field("draw") == 400.0);
    CHECK(field("unbalanced") == 1);

    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    printf("rsx_fifo_busy: ok\n");
    return 0;
}
