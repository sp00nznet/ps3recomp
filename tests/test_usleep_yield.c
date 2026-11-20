/* sys_timer_usleep on POSIX hosts, with and without the opt-in
 * PS3RECOMP_USLEEP_SPIN_US short-sleep yield (runtime/syscalls/sys_timer.c).
 *
 * nanosleep and sched_yield are counted through wrappers, so the test checks
 * which path each sleep took rather than how long the host scheduler took:
 *   off      option unset: every nonzero usleep is one nanosleep, no yields
 *   spin     PS3RECOMP_USLEEP_SPIN_US=100: usleep(1..99) yields to its
 *            deadline (never returning early) and never calls nanosleep;
 *            usleep(100) and longer still nanosleep; usleep(0) does neither
 * Both modes return CELL_OK. Latency is printed for information only.
 *
 * tools/test_usleep_yield.py builds it and runs each mode in its own process
 * (the option is read once). Synthetic: no game files.
 */
#define nanosleep   counted_nanosleep
#define sched_yield counted_sched_yield
#include "../runtime/syscalls/sys_timer.c"
#undef nanosleep
#undef sched_yield

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

uint8_t* vm_base;
/* What sys_timer_usleep calls besides the clock, stubbed. */
void ppu_thread_note_block(uint64_t tid) { (void)tid; }
unsigned long long ps3_qpc_us(void) { return 0; }
void ppu_log_host_chain(const char* tag) { (void)tag; }

static unsigned long s_nanosleeps, s_yields;

int counted_nanosleep(const struct timespec* req, struct timespec* rem)
{
    (void)rem;
    s_nanosleeps++;
    /* The real nanosleep is renamed in this file; usleep sleeps as long. */
    return usleep((useconds_t)(req->tv_sec * 1000000 + (req->tv_nsec + 999) / 1000));
}

int counted_sched_yield(void)
{
    s_yields++;
    return 0;   /* keep spinning on the clock; the count is what matters */
}

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* One guest usleep(usec); returns host nanoseconds spent. */
static uint64_t guest_usleep(uint64_t usec)
{
    ppu_context ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = usec;
    const uint64_t t0 = mono_ns();
    CHECK(sys_timer_usleep(&ctx) == CELL_OK);
    return mono_ns() - t0;
}

static void expect(uint64_t usec, unsigned long nanosleeps, int yields)
{
    s_nanosleeps = s_yields = 0;
    const uint64_t ns = guest_usleep(usec);
    CHECK(s_nanosleeps == nanosleeps);
    CHECK(yields ? s_yields > 0 : s_yields == 0);
    CHECK(ns >= usec * 1000ull);   /* neither path returns early */
}

static double median_us(uint64_t usec)
{
    enum { N = 101 };
    uint64_t t[N];
    for (int i = 0; i < N; i++) t[i] = guest_usleep(usec);
    for (int i = 1; i < N; i++)   /* insertion sort, N is small */
        for (int j = i; j > 0 && t[j - 1] > t[j]; j--) { uint64_t x = t[j]; t[j] = t[j - 1]; t[j - 1] = x; }
    return t[N / 2] / 1e3;
}

int main(int argc, char** argv)
{
    const int spin = argc > 1 && strcmp(argv[1], "spin") == 0;
    if (spin) {
        expect(1, 0, 1);
        expect(10, 0, 1);
        expect(99, 0, 1);
        expect(100, 1, 0);
        expect(2000, 1, 0);
    } else {
        expect(1, 1, 0);
        expect(10, 1, 0);
        expect(2000, 1, 0);
    }
    expect(0, 0, 0);
    printf("%s: median usleep(1) %.1f us, usleep(10) %.1f us\n",
           spin ? "spin" : "off", median_us(1), median_us(10));
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("PASS (%s)\n", spin ? "PS3RECOMP_USLEEP_SPIN_US=100" : "option unset");
    return 0;
}
