/* The host-backed sys_lwcond behind PS3_LWCOND_REAL=1 (runtime/ppu/
 * lwcond_real.h), on host threads: producer/consumer over two lwconds,
 * timed waits, signal / signal_all / signal_to, the fallback wake, and a
 * ping-pong that hangs (and trips the watchdog) if a wakeup is ever lost.
 *
 * The paired lwmutex is the same lock ppu_sysprx.cpp's lwm_try/lwm_acquire/
 * lwm_release build: a state word (0 free, 1 held, 2 contended) with a
 * binary semaphore as the wake channel, so the release-under-the-slot-lock
 * and re-acquire inside lwc_wait run against the real shape of it.
 *
 * Usage: test_lwcond_real [phase]   (default: every phase) */
#include "../../runtime/ppu/lwcond_real.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s line %d: %s\n", s_phase, __LINE__, #x); \
                                  InterlockedIncrement(&s_fail); } } while (0)
static volatile long s_fail;
/* Atomic loads/stores for the flags threads share outside any lock. */
static long ld(volatile long* p) { return InterlockedCompareExchange(p, 0, 0); }
static long long ld64(volatile long long* p) { return InterlockedCompareExchange64(p, 0, 0); }
static const char* volatile s_phase = "";

/* ---- the lwmutex (ppu_sysprx.cpp's slot lock) ---- */
typedef struct { volatile long state; HANDLE sem; } tmutex;
static void tm_init(tmutex* m) { m->state = 0; m->sem = CreateSemaphoreA(NULL, 0, 1, NULL); }
static int tm_try(tmutex* m) { return InterlockedCompareExchange(&m->state, 1, 0) == 0; }
static void tm_lock(tmutex* m)
{
    if (tm_try(m)) return;
    while (InterlockedExchange(&m->state, 2) != 0) WaitForSingleObject(m->sem, INFINITE);
}
static void tm_unlock(tmutex* m)
{
    if (InterlockedExchange(&m->state, 0) == 2) ReleaseSemaphore(m->sem, 1, NULL);
}
static int tm_held(tmutex* m) { return InterlockedCompareExchange(&m->state, 0, 0) != 0; }
static void cb_unlock(void* p) { tm_unlock((tmutex*)p); }
static void cb_lock(void* p) { tm_lock((tmutex*)p); }

static int cwait(lwc_slot* s, tmutex* m, uint32_t tid, uint64_t timeout_us, uint32_t fallback_ms)
{
    return lwc_wait(s, tid, timeout_us, fallback_ms, cb_unlock, cb_lock, m);
}

static HANDLE spawn(LPTHREAD_START_ROUTINE fn, void* arg) { return CreateThread(NULL, 0, fn, arg, 0, NULL); }
static void join(HANDLE h) { CHECK(WaitForSingleObject(h, 30000) == WAIT_OBJECT_0); CloseHandle(h); }
/* Spin until `n` threads are parked on `s` (their records are queued before
 * they let go of the lwmutex, so this is exact). */
static int await_parked(lwc_slot* s, int n)
{
    for (int i = 0; i < 5000; i++) { if (lwc_waiting(s) == n) return 1; Sleep(1); }
    return 0;
}

/* ---- watchdog: a lost wakeup with the fallback off hangs; fail loudly ---- */
static volatile long s_done;
static volatile long long s_phase_start;
static DWORD WINAPI watchdog(void* arg)
{
    while (!ld(&s_done)) {
        if (lwc_now_us() - ld64(&s_phase_start) > 60ll * 1000000) {
            fprintf(stderr, "FAIL %s: phase hung for 60 s (lost wakeup?)\n", s_phase);
            fflush(stderr);
            _Exit(3);
        }
        Sleep(50);
    }
    (void)arg;
    return 0;
}

/* ---- 1. producer/consumer: bounded ring, two lwconds, one lwmutex ---- */
#define PC_CAP 8
#define PC_PRODUCERS 4
#define PC_CONSUMERS 4
#define PC_ITEMS 20000          /* per producer */
static struct {
    tmutex m;
    lwc_slot *not_empty, *not_full;
    uint32_t ring[PC_CAP];
    int head, count, producers_left;
    uint32_t fallback_ms;
    volatile long long consumed, sum;
    int timed;                  /* consumers use short timed waits */
} pc;

static DWORD WINAPI producer(void* arg)
{
    uint32_t tid = 100 + (uint32_t)(uintptr_t)arg;
    for (uint32_t i = 1; i <= PC_ITEMS; i++) {
        tm_lock(&pc.m);
        while (pc.count == PC_CAP) cwait(pc.not_full, &pc.m, tid, 0, pc.fallback_ms);
        CHECK(tm_held(&pc.m));
        pc.ring[(pc.head + pc.count) % PC_CAP] = i;
        pc.count++;
        lwc_signal(pc.not_empty, 0, 0);
        tm_unlock(&pc.m);
    }
    tm_lock(&pc.m);
    if (--pc.producers_left == 0) lwc_signal(pc.not_empty, 1, 0);   /* let consumers see the end */
    tm_unlock(&pc.m);
    return 0;
}
static DWORD WINAPI consumer(void* arg)
{
    uint32_t tid = 200 + (uint32_t)(uintptr_t)arg;
    long long n = 0, sum = 0;
    for (;;) {
        tm_lock(&pc.m);
        while (pc.count == 0 && pc.producers_left) {
            if (pc.timed) {
                int r = cwait(pc.not_empty, &pc.m, tid, 2000, pc.fallback_ms);
                CHECK(r == LWC_SIGNALLED || r == LWC_TIMEDOUT);
            } else
                cwait(pc.not_empty, &pc.m, tid, 0, pc.fallback_ms);
        }
        if (pc.count == 0) { tm_unlock(&pc.m); break; }
        sum += pc.ring[pc.head];
        pc.head = (pc.head + 1) % PC_CAP;
        pc.count--;
        lwc_signal(pc.not_full, 0, 0);
        tm_unlock(&pc.m);
        n++;
    }
    InterlockedExchangeAdd64(&pc.consumed, n);
    InterlockedExchangeAdd64(&pc.sum, sum);
    return 0;
}
static void run_pc(uint32_t base, uint32_t fallback_ms, int timed)
{
    memset(&pc, 0, sizeof pc);
    tm_init(&pc.m);
    pc.not_empty = lwc_slot_get(base, 1);
    pc.not_full = lwc_slot_get(base + 8, 1);
    pc.producers_left = PC_PRODUCERS;
    pc.fallback_ms = fallback_ms;
    pc.timed = timed;
    HANDLE h[PC_PRODUCERS + PC_CONSUMERS];
    for (int i = 0; i < PC_CONSUMERS; i++) h[i] = spawn(consumer, (void*)(uintptr_t)i);
    for (int i = 0; i < PC_PRODUCERS; i++) h[PC_CONSUMERS + i] = spawn(producer, (void*)(uintptr_t)i);
    for (int i = 0; i < PC_PRODUCERS + PC_CONSUMERS; i++) join(h[i]);
    const long long want_n = (long long)PC_PRODUCERS * PC_ITEMS;
    const long long want_sum = (long long)PC_PRODUCERS * PC_ITEMS * (PC_ITEMS + 1) / 2;
    CHECK(pc.consumed == want_n);
    CHECK(pc.sum == want_sum);
    CHECK(lwc_waiting(pc.not_empty) == 0 && lwc_waiting(pc.not_full) == 0);
    CHECK(!tm_held(&pc.m));
    printf("  producer/consumer fallback=%ums timed=%d: %lld items\n", fallback_ms, timed, pc.consumed);
}
static void phase_prodcons(void)
{
    run_pc(0x10000, 0, 0);       /* signals alone must carry it: a lost one hangs */
    run_pc(0x10100, 10, 0);      /* production default */
    run_pc(0x10200, 0, 1);       /* timed waits racing signals */
}

/* ---- 2. timeouts ---- */
static struct { tmutex m; lwc_slot* s; } to;
static volatile long s_to_ok;
static DWORD WINAPI timed_waiter(void* arg)
{
    /* Random short timeouts against random signals: every return is a signal
     * or a timeout no earlier than asked, with the lwmutex held. */
    uint32_t seed = 0x9E3779B9u * (uint32_t)(uintptr_t)(arg) + 1;
    for (int i = 0; i < 200; i++) {
        seed = seed * 1103515245u + 12345u;
        uint64_t t = 200 + (seed >> 16) % 3000;   /* 0.2 .. 3.2 ms */
        tm_lock(&to.m);
        long long t0 = lwc_now_us();
        int r = cwait(to.s, &to.m, 300 + (uint32_t)(uintptr_t)arg, t, 10);
        long long dt = lwc_now_us() - t0;
        CHECK(tm_held(&to.m));
        CHECK(r == LWC_SIGNALLED || r == LWC_TIMEDOUT);
        if (r == LWC_TIMEDOUT) CHECK(dt + 50 >= (long long)t);   /* clock granularity slack */
        tm_unlock(&to.m);
        InterlockedIncrement(&s_to_ok);
    }
    return 0;
}
static volatile long s_to_stop;
static DWORD WINAPI random_signaller(void* arg)
{
    (void)arg;
    uint32_t seed = 7;
    while (!ld(&s_to_stop)) {
        seed = seed * 1103515245u + 12345u;
        tm_lock(&to.m);
        lwc_signal(to.s, (seed >> 20) & 1, 0);
        tm_unlock(&to.m);
        Sleep((seed >> 24) % 3);
    }
    return 0;
}
static DWORD WINAPI late_signal(void* arg)
{
    lwc_slot* s = (lwc_slot*)arg;
    CHECK(await_parked(s, 1));
    Sleep(5);
    tm_lock(&to.m);
    CHECK(lwc_signal(s, 0, 0) == 1);
    tm_unlock(&to.m);
    return 0;
}
static void phase_timeouts(void)
{
    tm_init(&to.m);
    to.s = lwc_slot_get(0x20000, 1);

    /* Nobody signals: ETIMEDOUT after the timeout, not before, mutex held,
     * and the fallback (shorter than the timeout) does not cut it short. */
    tm_lock(&to.m);
    long long t0 = lwc_now_us();
    int r = cwait(to.s, &to.m, 1, 50000, 10);
    long long dt = lwc_now_us() - t0;
    CHECK(r == LWC_TIMEDOUT);
    CHECK(dt >= 49000 && dt < 2000000);
    CHECK(tm_held(&to.m));
    tm_unlock(&to.m);
    printf("  timed wait, no signal: %d after %lld us\n", r, dt);

    /* A signal inside a long timeout ends the wait early. */
    HANDLE h = spawn(late_signal, to.s);
    tm_lock(&to.m);
    t0 = lwc_now_us();
    r = cwait(to.s, &to.m, 1, 5000000, 10);
    dt = lwc_now_us() - t0;
    tm_unlock(&to.m);
    join(h);
    CHECK(r == LWC_SIGNALLED && dt < 2000000);
    printf("  timed wait, signalled: %d after %lld us\n", r, dt);

    /* Infinite wait, nobody signals: the fallback lets it go as a spurious
     * wakeup after ~fallback_ms. */
    long before = g_lwc_fallbacks;
    tm_lock(&to.m);
    t0 = lwc_now_us();
    r = cwait(to.s, &to.m, 1, 0, 10);
    dt = lwc_now_us() - t0;
    CHECK(tm_held(&to.m));
    tm_unlock(&to.m);
    CHECK(r == LWC_FALLBACK && dt >= 9000 && dt < 2000000);
    CHECK(g_lwc_fallbacks == before + 1);
    printf("  infinite wait, no signal: fallback after %lld us\n", dt);

    /* Many timed waiters against a random signaller. */
    enum { N = 8 };
    HANDLE w[N];
    InterlockedExchange(&s_to_ok, 0); InterlockedExchange(&s_to_stop, 0);
    HANDLE sg = spawn(random_signaller, NULL);
    for (int i = 0; i < N; i++) w[i] = spawn(timed_waiter, (void*)(uintptr_t)(i + 1));
    for (int i = 0; i < N; i++) join(w[i]);
    InterlockedExchange(&s_to_stop, 1);
    join(sg);
    CHECK(ld(&s_to_ok) == N * 200);
    CHECK(lwc_waiting(to.s) == 0);
}

/* ---- 3. signal / signal_all / signal_to ---- */
#define K 6
static struct { tmutex m; lwc_slot* s; volatile long woke[K]; volatile long order[K]; volatile long n; } sa;
static DWORD WINAPI sa_waiter(void* arg)
{
    int i = (int)(uintptr_t)arg;
    tm_lock(&sa.m);
    int r = cwait(sa.s, &sa.m, 500 + (uint32_t)i, 0, 0);   /* no fallback: only a signal ends it */
    CHECK(r == LWC_SIGNALLED);
    CHECK(tm_held(&sa.m));
    sa.order[InterlockedIncrement(&sa.n) - 1] = i;
    InterlockedIncrement(&sa.woke[i]);
    tm_unlock(&sa.m);
    return 0;
}
static long woken(void) { long n = 0; for (int i = 0; i < K; i++) n += ld(&sa.woke[i]); return n; }
static void phase_signal(void)
{
    tm_init(&sa.m);
    sa.s = lwc_slot_get(0x30000, 1);
    HANDLE h[K];

    /* signal_all, many rounds: every parked waiter wakes, exactly once. */
    for (int round = 0; round < 200; round++) {
        memset((void*)sa.woke, 0, sizeof sa.woke); sa.n = 0;
        for (int i = 0; i < K; i++) h[i] = spawn(sa_waiter, (void*)(uintptr_t)i);
        CHECK(await_parked(sa.s, K));
        tm_lock(&sa.m);
        CHECK(lwc_signal(sa.s, 1, 0) == K);
        CHECK(lwc_signal(sa.s, 1, 0) == 0);    /* marked already: nothing left */
        tm_unlock(&sa.m);
        for (int i = 0; i < K; i++) join(h[i]);
        for (int i = 0; i < K; i++) CHECK(sa.woke[i] == 1);
    }
    printf("  signal_all: 200 rounds x %d waiters\n", K);

    /* signal: one waiter per signal, oldest first. Park them one at a time
     * so the queue order is known. */
    memset((void*)sa.woke, 0, sizeof sa.woke); sa.n = 0;
    for (int i = 0; i < K; i++) { h[i] = spawn(sa_waiter, (void*)(uintptr_t)i); CHECK(await_parked(sa.s, i + 1)); }
    for (int i = 0; i < K; i++) {
        tm_lock(&sa.m);
        CHECK(lwc_signal(sa.s, 0, 0) == 1);
        tm_unlock(&sa.m);
        for (int j = 0; j < 2000 && woken() != i + 1; j++) Sleep(1);
        Sleep(10);                               /* nobody else may follow it out */
        CHECK(woken() == i + 1);
        CHECK(ld(&sa.woke[i]) == 1);             /* FIFO */
    }
    for (int i = 0; i < K; i++) join(h[i]);
    printf("  signal: one at a time, FIFO\n");

    /* signal_to: only the named thread wakes; an unknown tid wakes nobody. */
    memset((void*)sa.woke, 0, sizeof sa.woke); sa.n = 0;
    for (int i = 0; i < K; i++) h[i] = spawn(sa_waiter, (void*)(uintptr_t)i);
    CHECK(await_parked(sa.s, K));
    const int order[K] = {3, 0, 5, 1, 4, 2};
    for (int k = 0; k < K; k++) {
        int i = order[k];
        tm_lock(&sa.m);
        CHECK(lwc_signal(sa.s, 0, 9999) == 0);
        CHECK(lwc_signal(sa.s, 0, 500 + (uint32_t)i) == 1);
        tm_unlock(&sa.m);
        for (int j = 0; j < 2000 && !ld(&sa.woke[i]); j++) Sleep(1);
        Sleep(10);
        CHECK(ld(&sa.woke[i]) == 1 && woken() == k + 1);
    }
    for (int i = 0; i < K; i++) join(h[i]);
    CHECK(lwc_waiting(sa.s) == 0);
    printf("  signal_to: named thread only\n");
}

/* ---- 4. ping-pong: strict hand-off, no fallback, a lost wakeup hangs ---- */
#define PP_ROUNDS 50000
static struct { tmutex m; lwc_slot* s; int turn; } pp;
static DWORD WINAPI pinger(void* arg)
{
    int me = (int)(uintptr_t)arg;
    for (int i = 0; i < PP_ROUNDS; i++) {
        tm_lock(&pp.m);
        while (pp.turn != me) cwait(pp.s, &pp.m, 600 + (uint32_t)me, 0, 0);
        pp.turn = !me;
        lwc_signal(pp.s, 0, 0);
        tm_unlock(&pp.m);
    }
    return 0;
}
static void phase_pingpong(void)
{
    tm_init(&pp.m);
    pp.s = lwc_slot_get(0x40000, 1);
    pp.turn = 0;
    HANDLE a = spawn(pinger, (void*)0), b = spawn(pinger, (void*)1);
    join(a); join(b);
    CHECK(lwc_waiting(pp.s) == 0);
    printf("  ping-pong: %d hand-offs\n", 2 * PP_ROUNDS);
}

/* ---- 5. table: slots are per address and stable ---- */
static void phase_table(void)
{
    CHECK(lwc_slot_get(0, 1) == NULL);
    CHECK(lwc_slot_get(0x50000, 0) == NULL);
    lwc_slot* a = lwc_slot_get(0x50000, 1);
    CHECK(a && lwc_slot_get(0x50000, 0) == a && lwc_slot_get(0x50000, 1) == a);
    CHECK(lwc_slot_get(0x50008, 1) != a);
    CHECK(lwc_signal(a, 1, 0) == 0);             /* nobody waiting: no-op */

    /* The tombstone is (long)-1, which casts to the guest address 0xFFFFFFFF.
     * A freed slot the probe chain for that address crosses must not look like
     * a live lwcond there. The freed address has the same hash bucket. */
    lwc_slot* x = lwc_slot_get(0xFFFFEFFF, 1);
    CHECK(x && lwc_slot_get(0xFFFFEFFF, 0) == x);
    CHECK(lwc_slot_free(0xFFFFEFFF) == 1);
    CHECK(lwc_slot_get(0xFFFFFFFF, 0) == NULL);
    lwc_slot* f = lwc_slot_get(0xFFFFFFFF, 1);
    CHECK(f != NULL && lwc_slot_get(0xFFFFFFFF, 0) == f);
    CHECK(lwc_slot_free(0xFFFFFFFF) == 1);
}

/* ---- 6. destroy frees an idle slot, EBUSY while a waiter is parked ---- */
static void phase_destroy(void)
{
    lwc_slot* a = lwc_slot_get(0x60000, 1);
    CHECK(a != NULL);
    CHECK(lwc_slot_free(0x60000) == 1);
    CHECK(lwc_slot_get(0x60000, 0) == NULL);
    CHECK(lwc_slot_get(0x60000, 1) != NULL);     /* tombstone reused */

    memset((void*)sa.woke, 0, sizeof sa.woke); sa.n = 0;
    tm_init(&sa.m);
    sa.s = lwc_slot_get(0x60008, 1);
    HANDLE h = spawn(sa_waiter, (void*)(uintptr_t)0);
    CHECK(await_parked(sa.s, 1));
    CHECK(lwc_slot_free(0x60008) == 0);          /* still parked -> EBUSY */
    tm_lock(&sa.m);
    CHECK(lwc_signal(sa.s, 1, 0) == 1);
    tm_unlock(&sa.m);
    join(h);
    CHECK(lwc_slot_free(0x60008) == 1);
    printf("  destroy: idle freed, parked returns EBUSY\n");
}

static const struct { const char* name; void (*fn)(void); } phases[] = {
    {"table", phase_table},
    {"destroy", phase_destroy},
    {"prodcons", phase_prodcons},
    {"timeouts", phase_timeouts},
    {"signal", phase_signal},
    {"pingpong", phase_pingpong},
};

int main(int argc, char** argv)
{
    InterlockedExchange64(&s_phase_start, lwc_now_us());
    HANDLE wd = spawn(watchdog, NULL);
    int ran = 0;
    for (size_t i = 0; i < sizeof phases / sizeof phases[0]; i++) {
        if (argc > 1 && strcmp(argv[1], phases[i].name)) continue;
        s_phase = phases[i].name;
        InterlockedExchange64(&s_phase_start, lwc_now_us());
        printf("[%s]\n", phases[i].name);
        fflush(stdout);
        phases[i].fn();
        ran++;
    }
    InterlockedExchange(&s_done, 1);
    WaitForSingleObject(wd, INFINITE); CloseHandle(wd);
    failures = (int)s_fail;
    if (!ran) { fprintf(stderr, "unknown phase %s\n", argv[1]); return 2; }
    printf("lwcond real: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
