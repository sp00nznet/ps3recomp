/*
 * ps3recomp - tests/sync_stress/main.c
 *
 * Deterministic lv2 sync-primitive stress suite.
 * Exercises runtime/syscalls/sys_mutex.c, sys_cond.c, sys_semaphore.c,
 * sys_event.c directly (as C functions taking a ppu_context*), on Win32
 * threads -- real ones on Windows, runtime/platform/win32_compat.h's
 * elsewhere -- with NO dependency on the yakuza runner or the recompiled game.
 *
 * Usage: sync_stress [seed]   (default seed 0)
 *
 * Exit code 0 = all tests passed. Nonzero = at least one test FAILed (see
 * stderr for the FAIL dump: which test, which thread/object, and counters).
 */

/* Win32 threads and interlocked ops on every host: a passthrough to
 * <windows.h> on Windows, the runtime's own shim elsewhere. */
#include "../../runtime/platform/win32_compat.h"
#include "../../runtime/platform/msvc_compat.h"   /* __declspec(thread), __stdcall */
#ifdef _WIN32
#include <process.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#include "sys_mutex.h"
#include "sys_cond.h"
#include "sys_semaphore.h"
#include "sys_event.h"
#include "sys_rwlock.h"
#include "ppu/ppu_context.h"

/* ---------------------------------------------------------------------------
 * Minimal guest-memory + thread-id shims the runtime sync sources need.
 * (Mirrors the pattern already used by runtime/spu/tests/test_dma_main.c:
 * a plain host buffer stands in for guest memory; vm_to_host() in vm.h does
 * `vm_base + addr`, so any host buffer works as long as addresses used here
 * stay inside it.)
 * -----------------------------------------------------------------------*/
#define GUEST_MEM_SIZE (4u * 1024u * 1024u)
static uint8_t g_guest_mem[GUEST_MEM_SIZE];
uint8_t* vm_base = g_guest_mem;

/* Simple bump allocator for "guest addresses" (attr structs, out-params). */
static LONG volatile g_guest_bump = 4096; /* keep addr 0 reserved/invalid */
static uint32_t guest_alloc(uint32_t size)
{
    LONG size_al = (LONG)((size + 15u) & ~15u);
    LONG addr = InterlockedExchangeAdd(&g_guest_bump, size_al);
    if ((uint32_t)(addr + size_al) >= GUEST_MEM_SIZE) {
        fprintf(stderr, "FATAL: guest_mem exhausted\n");
        exit(97);
    }
    return (uint32_t)addr;
}

/* Per-host-thread guest thread id (the runtime keys mutex ownership / cond
 * ownership checks off ppu_context.thread_id, and sys_cond.c / sys_semaphore.c
 * / sys_event.c's diagnostic levers key off yz_thread_current_id()). */
static __declspec(thread) uint32_t t_tid = 0;
uint32_t yz_thread_current_id(void) { return t_tid; }

static LONG volatile g_next_tid = 1;
static uint32_t alloc_tid(void) { return (uint32_t)InterlockedIncrement(&g_next_tid); }

/* ---------------------------------------------------------------------------
 * Link-time stubs for sys_event.c's title-diagnostic side paths.
 *
 * sys_event.c's sys_event_queue_receive() carries a few instrumentation
 * branches that call into the SPU workload dispatcher and a PPU guest-stack
 * dumper. This suite links only the four lv2 sync .c files (deliberately,
 * to stay a fast standalone unit test with no PPU/SPU runtime dependency),
 * so those symbols are otherwise undefined at link time. None of the three
 * branches can fire here: the SPU-dispatch path is gated on an environment
 * variable this suite never sets, and the guest-stack dump path takes a
 * live ppu_context produced only by the real recompiled-game loader (this
 * suite builds its own minimal ppu_context, see ctx_init_for_thread()) and
 * self-limits to a handful of calls. These bodies exist purely to satisfy
 * the linker and are never expected to execute.
 * -----------------------------------------------------------------------*/
size_t spu_elf_image_size(const uint8_t* image, size_t max_avail)
{
    (void)image; (void)max_avail;
    return 0;
}

int spu_workload_dispatch_async(const uint8_t* image, uint32_t image_size, uint32_t args_ea)
{
    (void)image; (void)image_size; (void)args_ea;
    return 0;
}

void ppu_dump_guest_stack(ppu_context* ctx, const char* tag)
{
    (void)ctx; (void)tag;
}

/* Build a ppu_context for the calling host thread, tagged with a guest tid. */
static void ctx_init_for_thread(ppu_context* ctx, uint32_t tid)
{
    ppu_context_init(ctx);
    ctx->thread_id = tid;
}

/* ---------------------------------------------------------------------------
 * be32 helpers for reading/writing "guest" out-params in the test harness
 * (the runtime writes IDs/values big-endian via its own write_be32; we read
 * them back the same way here without reaching into runtime internals).
 * -----------------------------------------------------------------------*/
static uint32_t read_be32(uint32_t addr)
{
    uint8_t* p = g_guest_mem + addr;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void write_be32_host(uint32_t addr, uint32_t val)
{
    uint8_t* p = g_guest_mem + addr;
    p[0] = (uint8_t)(val >> 24);
    p[1] = (uint8_t)(val >> 16);
    p[2] = (uint8_t)(val >> 8);
    p[3] = (uint8_t)val;
}

/* ---------------------------------------------------------------------------
 * Small syscall-call wrappers: set gpr[3..], invoke the handler, return its
 * signed 32-bit result the way the guest would see it in gpr[3].
 * -----------------------------------------------------------------------*/
static int32_t call1(int64_t (*fn)(ppu_context*), ppu_context* ctx, uint64_t a0)
{
    ctx->gpr[3] = a0;
    return (int32_t)fn(ctx);
}
static int32_t call2(int64_t (*fn)(ppu_context*), ppu_context* ctx, uint64_t a0, uint64_t a1)
{
    ctx->gpr[3] = a0; ctx->gpr[4] = a1;
    return (int32_t)fn(ctx);
}
static int32_t call3(int64_t (*fn)(ppu_context*), ppu_context* ctx, uint64_t a0, uint64_t a1, uint64_t a2)
{
    ctx->gpr[3] = a0; ctx->gpr[4] = a1; ctx->gpr[5] = a2;
    return (int32_t)fn(ctx);
}
static int32_t call4(int64_t (*fn)(ppu_context*), ppu_context* ctx, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    ctx->gpr[3] = a0; ctx->gpr[4] = a1; ctx->gpr[5] = a2; ctx->gpr[6] = a3;
    return (int32_t)fn(ctx);
}

/* ---------------------------------------------------------------------------
 * Deterministic PRNG (xorshift32) seeded from argv -- NOT rand(), so runs are
 * reproducible across platforms/CRTs given the same seed.
 * -----------------------------------------------------------------------*/
static uint32_t g_seed;
static uint32_t xorshift32(uint32_t* s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

/* ---------------------------------------------------------------------------
 * Guest attribute-struct builders (offsets per sys_mutex.h / sys_semaphore.h
 * / sys_event.h comments and the create() handlers' own field reads).
 * -----------------------------------------------------------------------*/
static uint32_t make_mutex_attr(uint32_t protocol, int recursive)
{
    uint32_t addr = guest_alloc(32); /* sizeof(sys_mutex_attribute_t) rounded */
    write_be32_host(addr + 0, protocol);
    write_be32_host(addr + 4, recursive ? SYS_SYNC_RECURSIVE : SYS_SYNC_NOT_RECURSIVE);
    return addr;
}

static uint32_t make_sema_attr(uint32_t protocol)
{
    uint32_t addr = guest_alloc(16);
    write_be32_host(addr + 0, protocol);
    return addr;
}

static uint32_t make_equeue_attr(uint32_t protocol, uint32_t qtype)
{
    uint32_t addr = guest_alloc(16);
    write_be32_host(addr + 0, protocol);
    write_be32_host(addr + 4, qtype);
    return addr;
}

/* ---------------------------------------------------------------------------
 * Failure bookkeeping
 * -----------------------------------------------------------------------*/
static LONG volatile g_fail_count = 0;
static void fail(const char* test, const char* fmt, ...)
{
    InterlockedIncrement(&g_fail_count);
    fprintf(stderr, "[FAIL] %s: ", test);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    fflush(stderr);
}

static ULONGLONG now_ms(void) { return GetTickCount64(); }

/* =========================================================================
 * TEST 1 -- No lost wakeup
 *
 * N producers signal a cond (under the associated mutex, as lv2 requires:
 * producers lock, mutate a shared "produced" counter, signal, unlock).
 * M consumers each wait on the SAME cond/mutex pair for "produced > consumed"
 * and, on wake, consume one item (bump `consumed`) -- a classic monitor
 * pattern. Total items = iterations. A consumer that cannot make progress
 * for > 2s is a lost-wakeup FAIL (the recorded case this whole test exists
 * to catch: sys_cond_signal's rendezvous window, see sys_cond.c comments).
 * ========================================================================= */
#define T1_PRODUCERS   4
#define T1_CONSUMERS   4
#define T1_ITERS       10000  /* total items produced across all producers */
#define T1_STALL_MS    2000

typedef struct {
    uint32_t mutex_id;
    uint32_t cond_id;
    LONG volatile produced;
    LONG volatile consumed;
    LONG volatile done_producing; /* set once all producers finish */
    /* Global last-progress timestamp, updated ONLY when `consumed` actually
     * advances (NOT on every wait attempt/timeout -- a consumer polling
     * every 500ms on a dropped wakeup must still be flagged once outstanding
     * work sits unconsumed for T1_STALL_MS, not reset every poll cycle). */
    LONG volatile last_progress_ms;
} t1_shared;

typedef struct {
    t1_shared* sh;
    int my_index; /* consumer slot, -1 for producers */
    int iters_for_me;
} t1_arg;

static unsigned __stdcall t1_producer(void* p)
{
    t1_arg* arg = (t1_arg*)p;
    t1_shared* sh = arg->sh;
    uint32_t tid = alloc_tid();
    t_tid = tid;
    ppu_context ctx; ctx_init_for_thread(&ctx, tid);

    for (int i = 0; i < arg->iters_for_me; i++) {
        int32_t rc = call2((int64_t(*)(ppu_context*))sys_mutex_lock, &ctx, sh->mutex_id, 0);
        if (rc != CELL_OK) { fail("test1", "producer lock rc=0x%08X", (unsigned)rc); return 1; }

        InterlockedIncrement(&sh->produced);

        rc = call1((int64_t(*)(ppu_context*))sys_cond_signal, &ctx, sh->cond_id);
        if (rc != CELL_OK) { fail("test1", "producer signal rc=0x%08X", (unsigned)rc); return 1; }

        rc = call1((int64_t(*)(ppu_context*))sys_mutex_unlock, &ctx, sh->mutex_id);
        if (rc != CELL_OK) { fail("test1", "producer unlock rc=0x%08X", (unsigned)rc); return 1; }
    }
    return 0;
}

static unsigned __stdcall t1_consumer(void* p)
{
    t1_arg* arg = (t1_arg*)p;
    t1_shared* sh = arg->sh;
    int idx = arg->my_index;
    uint32_t tid = alloc_tid();
    t_tid = tid;
    ppu_context ctx; ctx_init_for_thread(&ctx, tid);

    for (;;) {
        int32_t rc = call2((int64_t(*)(ppu_context*))sys_mutex_lock, &ctx, sh->mutex_id, 0);
        if (rc != CELL_OK) { fail("test1", "consumer lock rc=0x%08X", (unsigned)rc); return 1; }

        while (sh->produced <= sh->consumed) {
            if (sh->done_producing && sh->produced <= sh->consumed) {
                /* No more work will ever arrive. Exit cleanly. */
                call1((int64_t(*)(ppu_context*))sys_mutex_unlock, &ctx, sh->mutex_id);
                return 0;
            }
            /* INFINITE wait deliberately: sys_cond_wait's own timed-wait path
             * re-polls `pending` on every internal timer tick until its
             * timeout elapses (see sys_cond.c), so a BOUNDED wait here would
             * still (eventually, spuriously) notice a `pending` bump made
             * without a real OS wake -- which is exactly what happened in an
             * earlier draft: a seeded "drop WakeConditionVariable but still
             * increment pending" bug only added latency (bounded by the
             * wait's own timeout), it never actually hung, so a timed wait
             * here could not distinguish "slow" from "truly lost forever".
             * An infinite wait genuinely blocks forever on a true lost
             * wakeup, which the external watchdog thread in
             * test1_no_lost_wakeup (below) is responsible for detecting via
             * `last_progress_ms` from OUTSIDE this thread and hard-failing
             * the process -- this mirrors how a real lost wakeup actually
             * manifests in the game (a thread parked forever), not a slow
             * poll loop. */
            rc = call2((int64_t(*)(ppu_context*))sys_cond_wait, &ctx, sh->cond_id, (uint64_t)0 /* infinite */);
            if (rc != CELL_OK) {
                fail("test1", "consumer wait rc=0x%08X", (unsigned)rc);
                call1((int64_t(*)(ppu_context*))sys_mutex_unlock, &ctx, sh->mutex_id);
                return 1;
            }
        }

        InterlockedIncrement(&sh->consumed);
        InterlockedExchange(&sh->last_progress_ms, (LONG)now_ms());

        int32_t urc = call1((int64_t(*)(ppu_context*))sys_mutex_unlock, &ctx, sh->mutex_id);
        if (urc != CELL_OK) { fail("test1", "consumer unlock rc=0x%08X", (unsigned)urc); return 1; }
    }
}

/* External stall watchdog: a lost wakeup parks a consumer FOREVER in an
 * infinite sys_cond_wait (see t1_consumer's comment on why the wait must be
 * infinite for this test to be meaningful). This thread is the only piece
 * that can observe and report that stall promptly -- it polls the shared
 * `last_progress_ms` from OUTSIDE any consumer, and if outstanding work
 * (produced > consumed) sits with zero progress for T1_STALL_MS, it prints
 * the FAIL+dump the spec asks for and hard-exits the process immediately
 * (a stuck consumer thread can't be safely cancelled out of a CRITICAL_
 * SECTION/CV wait, so "detect and report, then terminate" is the only sound
 * way to bound this test's runtime on a genuine lost wakeup). */
static unsigned __stdcall t1_watchdog(void* p)
{
    t1_shared* sh = (t1_shared*)p;
    for (;;) {
        Sleep(100);
        if (sh->done_producing && sh->produced <= sh->consumed) return 0; /* test finished cleanly */
        if (sh->produced > sh->consumed) {
            ULONGLONG stalled_for = now_ms() - (ULONGLONG)sh->last_progress_ms;
            if (stalled_for > T1_STALL_MS) {
                fprintf(stderr, "[FAIL] test1_no_lost_wakeup: STUCK: no consumption progress for "
                        ">%dms with produced=%ld consumed=%ld (possible lost wakeup)\n",
                        T1_STALL_MS, (long)sh->produced, (long)sh->consumed);
                fflush(stderr);
                InterlockedIncrement(&g_fail_count);
                fflush(stdout);
                _exit(1); /* a parked consumer can't be joined; report and terminate now */
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Rendezvous sub-check: guarantees the waiter is ALREADY PARKED inside the
 * host condvar wait before the signal fires.
 *
 * The bulk producer/consumer stress scenario below is a legitimate, valuable
 * test on its own, but it turned out to be a WEAK oracle for a dropped
 * WakeConditionVariable specifically: sys_cond_wait's rendezvous design
 * (sys_cond.c) checks `while (c->pending == 0)` BEFORE ever calling
 * SleepConditionVariableCS, and `pending++` is untouched by that class of
 * bug -- so a waiter that re-enters sys_cond_wait AFTER the signal already
 * bumped `pending` sails through without ever needing an OS wake. With 4
 * producers/4 consumers churning the same mutex, most items are drained via
 * that fast path, so a genuinely-parked waiter (the case an OS-level dropped
 * wake actually breaks) was rare -- confirmed via an independent code-
 * reading review of sys_cond_wait/sys_cond_signal before adding this check
 * (see the T10 completion report for the full mechanism). This sub-check
 * closes that gap directly: one dedicated consumer thread that is given a
 * generous head start to actually block inside sys_cond_wait, THEN one
 * signal fires. If the wake is dropped, the consumer hangs forever and the
 * watchdog below reports it -- no fast-path escape is possible because
 * nothing touches `pending` before the deliberate delay elapses. */
#define T1B_PARK_DELAY_MS  300
#define T1B_STALL_MS       2000

typedef struct {
    uint32_t mutex_id;
    uint32_t cond_id;
    LONG volatile woke;         /* 1 once the consumer's wait returns */
    LONG volatile parked_at_ms; /* set just before the consumer waits */
} t1b_shared;

static unsigned __stdcall t1b_consumer(void* p)
{
    t1b_shared* sh = (t1b_shared*)p;
    uint32_t tid = alloc_tid(); t_tid = tid;
    ppu_context ctx; ctx_init_for_thread(&ctx, tid);

    int32_t rc = call2((int64_t(*)(ppu_context*))sys_mutex_lock, &ctx, sh->mutex_id, 0);
    if (rc != CELL_OK) { fail("test1b", "lock rc=0x%08X", (unsigned)rc); return 1; }

    InterlockedExchange(&sh->parked_at_ms, (LONG)now_ms());
    rc = call2((int64_t(*)(ppu_context*))sys_cond_wait, &ctx, sh->cond_id, (uint64_t)0 /* infinite */);
    if (rc != CELL_OK) { fail("test1b", "wait rc=0x%08X", (unsigned)rc); return 1; }

    InterlockedExchange(&sh->woke, 1);
    call1((int64_t(*)(ppu_context*))sys_mutex_unlock, &ctx, sh->mutex_id);
    return 0;
}

static unsigned __stdcall t1b_watchdog(void* p)
{
    t1b_shared* sh = (t1b_shared*)p;
    for (;;) {
        Sleep(50);
        if (sh->woke) return 0;
        if (sh->parked_at_ms != 0) {
            ULONGLONG since_park = now_ms() - (ULONGLONG)sh->parked_at_ms;
            if (since_park > (ULONGLONG)(T1B_PARK_DELAY_MS + T1B_STALL_MS)) {
                fprintf(stderr, "[FAIL] test1b_rendezvous_park: consumer parked >%dms ago and "
                        "never woke (signal delivered to an already-sleeping waiter was lost)\n",
                        T1B_PARK_DELAY_MS + T1B_STALL_MS);
                fflush(stderr);
                InterlockedIncrement(&g_fail_count);
                fflush(stdout);
                _exit(1);
            }
        }
    }
}

static int test1b_rendezvous_park(void)
{
    const char* name = "test1b_rendezvous_park";
    LONG before = g_fail_count;
    ppu_context ctx; ctx_init_for_thread(&ctx, alloc_tid());

    uint32_t mattr = make_mutex_attr(SYS_SYNC_FIFO, 0);
    uint32_t mid_addr = guest_alloc(4);
    int32_t rc = call2((int64_t(*)(ppu_context*))sys_mutex_create, &ctx, mid_addr, mattr);
    if (rc != CELL_OK) { fail(name, "mutex_create rc=0x%08X", (unsigned)rc); return 1; }
    uint32_t mid = read_be32(mid_addr);

    uint32_t cid_addr = guest_alloc(4);
    rc = call3((int64_t(*)(ppu_context*))sys_cond_create, &ctx, cid_addr, mid, 0);
    if (rc != CELL_OK) { fail(name, "cond_create rc=0x%08X", (unsigned)rc); return 1; }
    uint32_t cid = read_be32(cid_addr);

    t1b_shared sh; memset(&sh, 0, sizeof(sh));
    sh.mutex_id = mid;
    sh.cond_id = cid;

    HANDLE consumer = (HANDLE)_beginthreadex(NULL, 0, t1b_consumer, &sh, 0, NULL);
    HANDLE watchdog = (HANDLE)_beginthreadex(NULL, 0, t1b_watchdog, &sh, 0, NULL);

    /* Generous head start so the consumer is provably already parked inside
     * SleepConditionVariableCS (holds no lock while parked, so this sleep on
     * the signalling side does not deadlock anything) before we signal. */
    Sleep(T1B_PARK_DELAY_MS);

    rc = call2((int64_t(*)(ppu_context*))sys_mutex_lock, &ctx, mid, 0);
    if (rc != CELL_OK) { fail(name, "signaler lock rc=0x%08X", (unsigned)rc); }
    rc = call1((int64_t(*)(ppu_context*))sys_cond_signal, &ctx, cid);
    if (rc != CELL_OK) { fail(name, "signal rc=0x%08X", (unsigned)rc); }
    call1((int64_t(*)(ppu_context*))sys_mutex_unlock, &ctx, mid);

    DWORD wr = WaitForSingleObject(consumer, T1B_STALL_MS + 3000);
    if (wr == WAIT_TIMEOUT) {
        fail(name, "consumer never woke after signal to an already-parked waiter (hard hang)");
    }
    WaitForSingleObject(watchdog, 500);
    CloseHandle(consumer);
    CloseHandle(watchdog);

    if (!sh.woke) fail(name, "consumer did not report waking");

    int ok = (g_fail_count == before);
    printf("[%s] woke=%ld -> %s\n", name, (long)sh.woke, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

static int test1_no_lost_wakeup(void)
{
    const char* name = "test1_no_lost_wakeup";
    printf("[%s] %d producers x %d consumers, %d items...\n", name, T1_PRODUCERS, T1_CONSUMERS, T1_ITERS);
    LONG before = g_fail_count;

    ppu_context boot_ctx; ctx_init_for_thread(&boot_ctx, alloc_tid());

    uint32_t mutex_attr = make_mutex_attr(SYS_SYNC_FIFO, 0);
    uint32_t mutex_id_addr = guest_alloc(4);
    int32_t rc = call2((int64_t(*)(ppu_context*))sys_mutex_create, &boot_ctx, mutex_id_addr, mutex_attr);
    if (rc != CELL_OK) { fail(name, "mutex_create rc=0x%08X", (unsigned)rc); return 1; }
    uint32_t mutex_id = read_be32(mutex_id_addr);

    uint32_t cond_id_addr = guest_alloc(4);
    rc = call3((int64_t(*)(ppu_context*))sys_cond_create, &boot_ctx, cond_id_addr, mutex_id, 0);
    if (rc != CELL_OK) { fail(name, "cond_create rc=0x%08X", (unsigned)rc); return 1; }
    uint32_t cond_id = read_be32(cond_id_addr);

    t1_shared sh;
    memset(&sh, 0, sizeof(sh));
    sh.mutex_id = mutex_id;
    sh.cond_id = cond_id;
    sh.last_progress_ms = (LONG)now_ms();

    HANDLE producers[T1_PRODUCERS];
    HANDLE consumers[T1_CONSUMERS];
    t1_arg pargs[T1_PRODUCERS];
    t1_arg cargs[T1_CONSUMERS];

    int base = T1_ITERS / T1_PRODUCERS;
    int rem = T1_ITERS % T1_PRODUCERS;
    for (int i = 0; i < T1_PRODUCERS; i++) {
        pargs[i].sh = &sh;
        pargs[i].my_index = -1;
        pargs[i].iters_for_me = base + (i < rem ? 1 : 0);
        producers[i] = (HANDLE)_beginthreadex(NULL, 0, t1_producer, &pargs[i], 0, NULL);
    }
    for (int i = 0; i < T1_CONSUMERS; i++) {
        cargs[i].sh = &sh;
        cargs[i].my_index = i;
        consumers[i] = (HANDLE)_beginthreadex(NULL, 0, t1_consumer, &cargs[i], 0, NULL);
    }
    HANDLE watchdog = (HANDLE)_beginthreadex(NULL, 0, t1_watchdog, &sh, 0, NULL);

    WaitForMultipleObjects(T1_PRODUCERS, producers, TRUE, INFINITE);
    /* Wake every consumer once: with an infinite per-consumer wait, only
     * signal_all (unmutated by the seeded-bug self-test) reliably drains the
     * "no more work" tail; sys_cond_signal alone (mutated in the self-test)
     * would leave any consumer that raced past the produced>consumed check
     * parked forever, which is exactly what the watchdog above exists to
     * catch mid-run.
     *
     * Under the lv2 mutex, like the producers' signals. A consumer reads
     * done_producing under that mutex and decides to wait on it; the flag
     * must flip and the broadcast go out under the same mutex, or a consumer
     * that has decided but not yet parked misses the only wake it will ever
     * get. Issued outside the mutex this parked one consumer forever on a
     * loaded Linux runner. That is also how a title has to use lv2: nothing
     * is remembered for a thread that is not waiting yet. */
    ppu_context wctx; ctx_init_for_thread(&wctx, alloc_tid());
    rc = call2((int64_t(*)(ppu_context*))sys_mutex_lock, &wctx, mutex_id, 0);
    if (rc != CELL_OK) fail(name, "tail lock rc=0x%08X", (unsigned)rc);
    InterlockedExchange(&sh.done_producing, 1);
    call1((int64_t(*)(ppu_context*))sys_cond_signal_all, &wctx, cond_id);
    rc = call1((int64_t(*)(ppu_context*))sys_mutex_unlock, &wctx, mutex_id);
    if (rc != CELL_OK) fail(name, "tail unlock rc=0x%08X", (unsigned)rc);

    DWORD wr = WaitForMultipleObjects(T1_CONSUMERS, consumers, TRUE, 10000);
    if (wr == WAIT_TIMEOUT) {
        fail(name, "one or more consumers never returned (hard hang)");
    }
    InterlockedExchange(&sh.done_producing, 1); /* tell the watchdog we're wrapping up */
    WaitForSingleObject(watchdog, 2000);
    CloseHandle(watchdog);

    for (int i = 0; i < T1_PRODUCERS; i++) CloseHandle(producers[i]);
    for (int i = 0; i < T1_CONSUMERS; i++) CloseHandle(consumers[i]);

    if (sh.produced != T1_ITERS) {
        fail(name, "produced=%ld expected=%d", sh.produced, T1_ITERS);
    }
    if (sh.consumed != T1_ITERS) {
        fail(name, "consumed=%ld expected=%d (lost item(s))", sh.consumed, T1_ITERS);
    }

    int ok = (g_fail_count == before);
    printf("[%s] produced=%ld consumed=%ld -> %s\n", name, (long)sh.produced, (long)sh.consumed, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* =========================================================================
 * TEST 2 -- Timed-wait semantics
 *
 * cond timed waits must return ETIMEDOUT on expiry and 0 (CELL_OK) on a
 * real signal, and must NEVER hang past their timeout (bounded by the host
 * wait itself, but we also assert the wall-clock elapsed matches order of
 * magnitude). Semaphore timed waits get the same treatment.
 * ========================================================================= */
static unsigned __stdcall t2_signal_after_delay(void* p);

static int test2_timed_wait_semantics(void)
{
    const char* name = "test2_timed_wait_semantics";
    LONG before = g_fail_count;
    ppu_context ctx; ctx_init_for_thread(&ctx, alloc_tid());

    /* --- cond: expiry case --- */
    uint32_t mattr = make_mutex_attr(SYS_SYNC_FIFO, 0);
    uint32_t mid_addr = guest_alloc(4);
    int32_t rc = call2((int64_t(*)(ppu_context*))sys_mutex_create, &ctx, mid_addr, mattr);
    if (rc != CELL_OK) { fail(name, "mutex_create rc=0x%08X", (unsigned)rc); return 1; }
    uint32_t mid = read_be32(mid_addr);

    uint32_t cid_addr = guest_alloc(4);
    rc = call3((int64_t(*)(ppu_context*))sys_cond_create, &ctx, cid_addr, mid, 0);
    if (rc != CELL_OK) { fail(name, "cond_create rc=0x%08X", (unsigned)rc); return 1; }
    uint32_t cid = read_be32(cid_addr);

    for (int trial = 0; trial < 50; trial++) {
        rc = call2((int64_t(*)(ppu_context*))sys_mutex_lock, &ctx, mid, 0);
        if (rc != CELL_OK) { fail(name, "lock rc=0x%08X", (unsigned)rc); return 1; }

        ULONGLONG t0 = now_ms();
        rc = call2((int64_t(*)(ppu_context*))sys_cond_wait, &ctx, cid, (uint64_t)50000 /* 50ms, no signaler */);
        ULONGLONG elapsed = now_ms() - t0;

        if (rc != (int32_t)CELL_ETIMEDOUT) {
            fail(name, "cond timed-wait trial %d: expected ETIMEDOUT got 0x%08X", trial, (unsigned)rc);
        }
        if (elapsed > 5000) {
            fail(name, "cond timed-wait trial %d: took %llums (expected ~50ms, never-hang bound blown)",
                 trial, (unsigned long long)elapsed);
        }

        rc = call1((int64_t(*)(ppu_context*))sys_mutex_unlock, &ctx, mid);
        if (rc != CELL_OK) { fail(name, "unlock rc=0x%08X", (unsigned)rc); return 1; }
    }

    /* --- cond: real signal beats the timeout, rc must be CELL_OK --- */
    typedef struct { uint32_t mid, cid; } signaler_arg;
    signaler_arg sarg = { mid, cid };
    /* Signaler thread: sleep a bit, then signal (real lv2 sys_cond_signal
     * does not require the caller to hold the mutex -- see sys_cond.c). */
    HANDLE sig_th = (HANDLE)_beginthreadex(NULL, 0, t2_signal_after_delay, &sarg, 0, NULL);

    rc = call2((int64_t(*)(ppu_context*))sys_mutex_lock, &ctx, mid, 0);
    if (rc != CELL_OK) { fail(name, "signalled-case lock rc=0x%08X", (unsigned)rc); }
    ULONGLONG t0 = now_ms();
    rc = call2((int64_t(*)(ppu_context*))sys_cond_wait, &ctx, cid, (uint64_t)5000000 /* 5s */);
    ULONGLONG elapsed = now_ms() - t0;
    if (rc != CELL_OK) {
        fail(name, "signalled-case: expected CELL_OK got 0x%08X after %llums", (unsigned)rc, (unsigned long long)elapsed);
    }
    call1((int64_t(*)(ppu_context*))sys_mutex_unlock, &ctx, mid);
    WaitForSingleObject(sig_th, INFINITE);
    CloseHandle(sig_th);

    /* --- semaphore: expiry case --- */
    uint32_t sattr = make_sema_attr(SYS_SYNC_FIFO);
    uint32_t sid_addr = guest_alloc(4);
    rc = call4((int64_t(*)(ppu_context*))sys_semaphore_create, &ctx, sid_addr, sattr, 0, 4);
    if (rc != CELL_OK) { fail(name, "sema_create rc=0x%08X", (unsigned)rc); return 1; }
    uint32_t sid = read_be32(sid_addr);

    for (int trial = 0; trial < 50; trial++) {
        ULONGLONG s0 = now_ms();
        rc = call2((int64_t(*)(ppu_context*))sys_semaphore_wait, &ctx, sid, (uint64_t)30000 /* 30ms */);
        ULONGLONG selapsed = now_ms() - s0;
        if (rc != (int32_t)CELL_ETIMEDOUT) {
            fail(name, "sema timed-wait trial %d: expected ETIMEDOUT got 0x%08X", trial, (unsigned)rc);
        }
        if (selapsed > 5000) {
            fail(name, "sema timed-wait trial %d: took %llums (never-hang bound blown)",
                 trial, (unsigned long long)selapsed);
        }
    }

    /* --- semaphore: post before wait must return CELL_OK immediately --- */
    rc = call2((int64_t(*)(ppu_context*))sys_semaphore_post, &ctx, sid, 1);
    if (rc != CELL_OK) { fail(name, "sema_post rc=0x%08X", (unsigned)rc); }
    rc = call2((int64_t(*)(ppu_context*))sys_semaphore_wait, &ctx, sid, (uint64_t)1000000);
    if (rc != CELL_OK) { fail(name, "sema wait-after-post rc=0x%08X", (unsigned)rc); }

    int ok = (g_fail_count == before);
    printf("[%s] -> %s\n", name, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

static unsigned __stdcall t2_signal_after_delay(void* p)
{
    typedef struct { uint32_t mid, cid; } signaler_arg;
    signaler_arg* a = (signaler_arg*)p;
    Sleep(100);
    uint32_t tid = alloc_tid();
    t_tid = tid;
    ppu_context ctx; ctx_init_for_thread(&ctx, tid);
    call1((int64_t(*)(ppu_context*))sys_cond_signal, &ctx, a->cid);
    return 0;
}

/* =========================================================================
 * TEST 3 -- Semaphore counting invariant under post/wait storms
 *
 * P posters each post `count` in bursts; C waiters each wait/decrement.
 * At all times 0 <= value <= max_value must hold (checked via
 * sys_semaphore_get_value from a monitor thread) and, at the end (all
 * threads joined, posts==waits in total units), value must return to its
 * starting point exactly -- no lost or phantom counts.
 * ========================================================================= */
#define T3_POSTERS   4
#define T3_WAITERS   4
#define T3_MAXVAL    64
#define T3_ITERS     10000 /* total post units == total wait units */

typedef struct {
    uint32_t sem_id;
    LONG volatile posted_units;
    LONG volatile waited_units;
    LONG volatile invariant_violations;
    LONG volatile stop;
} t3_shared;

static unsigned __stdcall t3_poster(void* p)
{
    typedef struct { t3_shared* sh; int units_for_me; uint32_t seed; } t3_parg;
    t3_parg* arg = (t3_parg*)p;
    t3_shared* sh = arg->sh;
    uint32_t tid = alloc_tid(); t_tid = tid;
    ppu_context ctx; ctx_init_for_thread(&ctx, tid);
    uint32_t rng = arg->seed;

    int remaining = arg->units_for_me;
    while (remaining > 0) {
        int burst = 1 + (int)(xorshift32(&rng) % 3);
        if (burst > remaining) burst = remaining;
        int32_t rc;
        for (;;) {
            rc = call2((int64_t(*)(ppu_context*))sys_semaphore_post, &ctx, sh->sem_id, burst);
            if (rc == CELL_OK) break;
            if (rc == (int32_t)CELL_EBUSY) { Sleep(0); continue; } /* would exceed max, retry smaller */
            fail("test3", "poster: unexpected post rc=0x%08X", (unsigned)rc);
            return 1;
        }
        InterlockedAdd(&sh->posted_units, burst);
        remaining -= burst;
    }
    return 0;
}

static unsigned __stdcall t3_waiter(void* p)
{
    typedef struct { t3_shared* sh; int units_for_me; } t3_warg;
    t3_warg* arg = (t3_warg*)p;
    t3_shared* sh = arg->sh;
    uint32_t tid = alloc_tid(); t_tid = tid;
    ppu_context ctx; ctx_init_for_thread(&ctx, tid);

    for (int i = 0; i < arg->units_for_me; i++) {
        int32_t rc = call2((int64_t(*)(ppu_context*))sys_semaphore_wait, &ctx, sh->sem_id, (uint64_t)2000000 /* 2s bound */);
        if (rc != CELL_OK) {
            fail("test3", "waiter wait rc=0x%08X (possible stuck waiter)", (unsigned)rc);
            return 1;
        }
        InterlockedIncrement(&sh->waited_units);
    }
    return 0;
}

static unsigned __stdcall t3_monitor(void* p)
{
    t3_shared* sh = (t3_shared*)p;
    uint32_t tid = alloc_tid(); t_tid = tid;
    ppu_context ctx; ctx_init_for_thread(&ctx, tid);
    uint32_t out_addr = guest_alloc(4);

    while (!sh->stop) {
        int32_t rc = call2((int64_t(*)(ppu_context*))sys_semaphore_get_value, &ctx, sh->sem_id, out_addr);
        if (rc == CELL_OK) {
            int32_t val = (int32_t)read_be32(out_addr);
            if (val < 0 || val > T3_MAXVAL) {
                InterlockedIncrement(&sh->invariant_violations);
                fail("test3", "invariant violated: value=%d not in [0,%d]", val, T3_MAXVAL);
            }
        }
        Sleep(0);
    }
    return 0;
}

static int test3_semaphore_counting(void)
{
    const char* name = "test3_semaphore_counting";
    LONG before = g_fail_count;
    ppu_context ctx; ctx_init_for_thread(&ctx, alloc_tid());

    uint32_t sattr = make_sema_attr(SYS_SYNC_FIFO);
    uint32_t sid_addr = guest_alloc(4);
    int32_t rc = call4((int64_t(*)(ppu_context*))sys_semaphore_create, &ctx, sid_addr, sattr, 0, T3_MAXVAL);
    if (rc != CELL_OK) { fail(name, "sema_create rc=0x%08X", (unsigned)rc); return 1; }
    uint32_t sid = read_be32(sid_addr);

    t3_shared sh; memset(&sh, 0, sizeof(sh)); sh.sem_id = sid;

    HANDLE mon = (HANDLE)_beginthreadex(NULL, 0, t3_monitor, &sh, 0, NULL);

    typedef struct { t3_shared* sh; int units_for_me; uint32_t seed; } t3_parg;
    typedef struct { t3_shared* sh; int units_for_me; } t3_warg;
    HANDLE posters[T3_POSTERS];
    HANDLE waiters[T3_WAITERS];
    t3_parg pargs[T3_POSTERS];
    t3_warg wargs[T3_WAITERS];

    int pbase = T3_ITERS / T3_POSTERS, prem = T3_ITERS % T3_POSTERS;
    for (int i = 0; i < T3_POSTERS; i++) {
        pargs[i].sh = &sh;
        pargs[i].units_for_me = pbase + (i < prem ? 1 : 0);
        pargs[i].seed = g_seed ^ (0x9E3779B9u * (uint32_t)(i + 1));
        posters[i] = (HANDLE)_beginthreadex(NULL, 0, t3_poster, &pargs[i], 0, NULL);
    }
    int wbase = T3_ITERS / T3_WAITERS, wrem = T3_ITERS % T3_WAITERS;
    for (int i = 0; i < T3_WAITERS; i++) {
        wargs[i].sh = &sh;
        wargs[i].units_for_me = wbase + (i < wrem ? 1 : 0);
        waiters[i] = (HANDLE)_beginthreadex(NULL, 0, t3_waiter, &wargs[i], 0, NULL);
    }

    WaitForMultipleObjects(T3_POSTERS, posters, TRUE, INFINITE);
    DWORD wr = WaitForMultipleObjects(T3_WAITERS, waiters, TRUE, 30000);
    if (wr == WAIT_TIMEOUT) fail(name, "one or more waiters never returned (hard hang)");

    InterlockedExchange(&sh.stop, 1);
    WaitForSingleObject(mon, INFINITE);

    for (int i = 0; i < T3_POSTERS; i++) CloseHandle(posters[i]);
    for (int i = 0; i < T3_WAITERS; i++) CloseHandle(waiters[i]);
    CloseHandle(mon);

    if (sh.posted_units != T3_ITERS) fail(name, "posted_units=%ld expected=%d", sh.posted_units, T3_ITERS);
    if (sh.waited_units != T3_ITERS) fail(name, "waited_units=%ld expected=%d", sh.waited_units, T3_ITERS);

    uint32_t out_addr = guest_alloc(4);
    rc = call2((int64_t(*)(ppu_context*))sys_semaphore_get_value, &ctx, sid, out_addr);
    int32_t final_val = (int32_t)read_be32(out_addr);
    if (rc != CELL_OK || final_val != 0) {
        fail(name, "final value=%d expected=0 (posts=%ld waits=%ld) rc=0x%08X",
             final_val, sh.posted_units, sh.waited_units, (unsigned)rc);
    }

    int ok = (g_fail_count == before);
    printf("[%s] posted=%ld waited=%ld final_value=%d violations=%ld -> %s\n",
           name, (long)sh.posted_units, (long)sh.waited_units, final_val, (long)sh.invariant_violations, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* =========================================================================
 * TEST 4 -- Event queue: FIFO-per-source + exactly-one-receiver-per-event
 *
 * Two sub-tests against the SAME queue implementation, each isolating one
 * half of the spec clause (they need different concurrency shapes to be
 * checkable at all from OUTSIDE the runtime's own lock):
 *
 * 4a (FIFO-per-source): S producer threads push a monotonically increasing
 *   per-source sequence number concurrently into one shared queue; ONE
 *   dedicated receiver thread drains it. With a single consumer, the order
 *   in which events are handed to guest code IS the order the receiver's
 *   own call sequence observes them (no second racing thread to introduce
 *   a scheduling artifact between "dequeued" and "recorded") -- so this is
 *   a sound way to check the queue preserves each source's push order.
 *   NOTE: an earlier draft of this test tried to check ordering with
 *   MULTIPLE concurrent receivers, each recording under its own bookkeeping
 *   lock after sys_event_queue_receive returned. That is NOT sound: the
 *   queue's internal CRITICAL_SECTION only serializes the dequeue itself
 *   (sys_event.c's `q->lock`); once two receiver threads' calls return,
 *   nothing pins the thread that got the earlier item ahead of the thread
 *   that got the later item at any SECOND, unrelated lock -- that measures
 *   OS scheduling, not queue order (confirmed via independent code-reading
 *   review of sys_event_queue_receive / event_queue_push before committing
 *   to this design; see the test's final report for the discarded output).
 *
 * 4b (exactly-one-receiver-per-event): R receiver threads pop concurrently
 *   from one shared queue fed by S producers. This property (no duplicate
 *   delivery, no silent drop) is a set-membership fact, NOT a timing fact,
 *   so it stays sound under full receiver concurrency: every (source,seq)
 *   must end up marked exactly once across all receivers, however they
 *   interleave.
 * ========================================================================= */
#define T4_SOURCES        6
#define T4_PER_SOURCE     1667  /* 4a: * 6 sources ~= 10000 events, 1 receiver */
#define T4_RECEIVERS      4
#define T4B_PER_SOURCE    417   /* 4b: * 6 sources ~= 2500 events, 4 receivers */
#define T4_QUEUE_CAP      96    /* < SYS_EVENT_QUEUE_BUF_MAX(127); push retries on full */

/* --- 4a: FIFO-per-source, single receiver --- */
typedef struct {
    uint32_t queue_id;
    LONG volatile total_events;
    LONG volatile order_violations;
    LONG volatile last_seq[T4_SOURCES];
} t4a_shared;

static unsigned __stdcall t4a_source(void* p)
{
    typedef struct { t4a_shared* sh; int source_idx; } t4a_sarg;
    t4a_sarg* arg = (t4a_sarg*)p;
    t4a_shared* sh = arg->sh;

    for (int seq = 0; seq < T4_PER_SOURCE; seq++) {
        for (;;) {
            int r = sys_event_queue_push_by_id(sh->queue_id,
                                                (uint64_t)(arg->source_idx + 1) /* source, 1-based */,
                                                (uint64_t)seq, 0, 0);
            if (r == 0) break;
            Sleep(0); /* queue full, back off and retry (never drop) */
        }
        InterlockedIncrement(&sh->total_events);
    }
    return 0;
}

static unsigned __stdcall t4a_receiver(void* p)
{
    t4a_shared* sh = (t4a_shared*)p;
    uint32_t tid = alloc_tid(); t_tid = tid;
    ppu_context ctx; ctx_init_for_thread(&ctx, tid);
    uint32_t event_addr = guest_alloc(32);
    LONG total_expected = T4_SOURCES * T4_PER_SOURCE;
    LONG received = 0;

    while (received < total_expected) {
        int32_t rc = call3((int64_t(*)(ppu_context*))sys_event_queue_receive, &ctx, sh->queue_id, event_addr, (uint64_t)1000000 /* 1s */);
        if (rc == (int32_t)CELL_ETIMEDOUT) continue;
        if (rc != CELL_OK) { fail("test4a", "receive rc=0x%08X", (unsigned)rc); return 1; }

        uint64_t source = ctx.gpr[4];
        uint64_t seq64 = ctx.gpr[5];
        int src_idx = (int)source - 1;
        int seq = (int)seq64;

        if (src_idx < 0 || src_idx >= T4_SOURCES || seq < 0 || seq >= T4_PER_SOURCE) {
            fail("test4a", "out-of-range event source=%llu seq=%llu",
                 (unsigned long long)source, (unsigned long long)seq64);
            received++;
            continue;
        }

        /* Sole receiver: this thread's own call sequence IS the delivery
         * order, so a plain (non-atomic) compare is sound here. */
        LONG prev = sh->last_seq[src_idx];
        if (seq <= prev) {
            sh->order_violations++;
            fail("test4a", "FIFO violation source=%d: got seq=%d after seq=%ld", src_idx, seq, prev);
        } else {
            sh->last_seq[src_idx] = seq;
        }
        received++;
    }
    return 0;
}

static int test4a_fifo_per_source(void)
{
    const char* name = "test4a_fifo_per_source";
    LONG before = g_fail_count;
    ppu_context ctx; ctx_init_for_thread(&ctx, alloc_tid());

    uint32_t qattr = make_equeue_attr(SYS_SYNC_FIFO, SYS_PPU_QUEUE);
    uint32_t qid_addr = guest_alloc(4);
    int32_t rc = call4((int64_t(*)(ppu_context*))sys_event_queue_create, &ctx, qid_addr, qattr, 0, T4_QUEUE_CAP);
    if (rc != CELL_OK) { fail(name, "queue_create rc=0x%08X", (unsigned)rc); return 1; }
    uint32_t qid = read_be32(qid_addr);

    t4a_shared sh; memset(&sh, 0, sizeof(sh));
    sh.queue_id = qid;
    for (int i = 0; i < T4_SOURCES; i++) sh.last_seq[i] = -1;

    typedef struct { t4a_shared* sh; int source_idx; } t4a_sarg;
    HANDLE sources[T4_SOURCES];
    t4a_sarg sargs[T4_SOURCES];

    HANDLE receiver = (HANDLE)_beginthreadex(NULL, 0, t4a_receiver, &sh, 0, NULL);
    for (int i = 0; i < T4_SOURCES; i++) {
        sargs[i].sh = &sh;
        sargs[i].source_idx = i;
        sources[i] = (HANDLE)_beginthreadex(NULL, 0, t4a_source, &sargs[i], 0, NULL);
    }

    WaitForMultipleObjects(T4_SOURCES, sources, TRUE, INFINITE);
    DWORD wr = WaitForSingleObject(receiver, 30000);
    if (wr == WAIT_TIMEOUT) fail(name, "receiver never returned (hard hang)");

    for (int i = 0; i < T4_SOURCES; i++) CloseHandle(sources[i]);
    CloseHandle(receiver);

    int ok = (g_fail_count == before);
    printf("[%s] total_events=%ld order_violations=%ld -> %s\n",
           name, (long)sh.total_events, (long)sh.order_violations, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* --- 4b: exactly-one-receiver-per-event, concurrent receivers --- */
typedef struct {
    uint32_t queue_id;
    LONG volatile total_received;
    LONG volatile total_events;
    LONG volatile duplicate_violations;
    uint8_t* seen[T4_SOURCES];       /* T4B_PER_SOURCE bytes each */
    CRITICAL_SECTION seen_lock;      /* bookkeeping only -- NOT an ordering oracle */
} t4b_shared;

static unsigned __stdcall t4b_source(void* p)
{
    typedef struct { t4b_shared* sh; int source_idx; } t4b_sarg;
    t4b_sarg* arg = (t4b_sarg*)p;
    t4b_shared* sh = arg->sh;

    for (int seq = 0; seq < T4B_PER_SOURCE; seq++) {
        for (;;) {
            int r = sys_event_queue_push_by_id(sh->queue_id,
                                                (uint64_t)(arg->source_idx + 1),
                                                (uint64_t)seq, 0, 0);
            if (r == 0) break;
            Sleep(0);
        }
        InterlockedIncrement(&sh->total_events);
    }
    return 0;
}

static unsigned __stdcall t4b_receiver(void* p)
{
    t4b_shared* sh = (t4b_shared*)p;
    uint32_t tid = alloc_tid(); t_tid = tid;
    ppu_context ctx; ctx_init_for_thread(&ctx, tid);
    uint32_t event_addr = guest_alloc(32);
    LONG total_expected = T4_SOURCES * T4B_PER_SOURCE;

    for (;;) {
        if (sh->total_received >= total_expected) return 0;

        int32_t rc = call3((int64_t(*)(ppu_context*))sys_event_queue_receive, &ctx, sh->queue_id, event_addr, (uint64_t)1000000 /* 1s */);
        if (rc == (int32_t)CELL_ETIMEDOUT) continue;
        if (rc != CELL_OK) { fail("test4b", "receive rc=0x%08X", (unsigned)rc); return 1; }

        uint64_t source = ctx.gpr[4];
        uint64_t seq64 = ctx.gpr[5];
        int src_idx = (int)source - 1;
        int seq = (int)seq64;

        if (src_idx < 0 || src_idx >= T4_SOURCES || seq < 0 || seq >= T4B_PER_SOURCE) {
            fail("test4b", "out-of-range event source=%llu seq=%llu",
                 (unsigned long long)source, (unsigned long long)seq64);
            InterlockedIncrement(&sh->total_received);
            continue;
        }

        /* Set-membership check only -- timing-independent, sound under
         * full receiver concurrency (see block comment above). */
        EnterCriticalSection(&sh->seen_lock);
        if (sh->seen[src_idx][seq]) {
            sh->duplicate_violations++;
            fail("test4b", "DUPLICATE delivery source=%d seq=%d", src_idx, seq);
        } else {
            sh->seen[src_idx][seq] = 1;
        }
        LeaveCriticalSection(&sh->seen_lock);

        InterlockedIncrement(&sh->total_received);
    }
}

static int test4b_exactly_once_delivery(void)
{
    const char* name = "test4b_exactly_once_delivery";
    LONG before = g_fail_count;
    ppu_context ctx; ctx_init_for_thread(&ctx, alloc_tid());

    uint32_t qattr = make_equeue_attr(SYS_SYNC_FIFO, SYS_PPU_QUEUE);
    uint32_t qid_addr = guest_alloc(4);
    int32_t rc = call4((int64_t(*)(ppu_context*))sys_event_queue_create, &ctx, qid_addr, qattr, 0, T4_QUEUE_CAP);
    if (rc != CELL_OK) { fail(name, "queue_create rc=0x%08X", (unsigned)rc); return 1; }
    uint32_t qid = read_be32(qid_addr);

    t4b_shared sh; memset(&sh, 0, sizeof(sh));
    sh.queue_id = qid;
    InitializeCriticalSection(&sh.seen_lock);
    for (int i = 0; i < T4_SOURCES; i++) sh.seen[i] = (uint8_t*)calloc(T4B_PER_SOURCE, 1);

    typedef struct { t4b_shared* sh; int source_idx; } t4b_sarg;
    HANDLE sources[T4_SOURCES];
    t4b_sarg sargs[T4_SOURCES];
    HANDLE receivers[T4_RECEIVERS];

    for (int i = 0; i < T4_RECEIVERS; i++) {
        receivers[i] = (HANDLE)_beginthreadex(NULL, 0, t4b_receiver, &sh, 0, NULL);
    }
    for (int i = 0; i < T4_SOURCES; i++) {
        sargs[i].sh = &sh;
        sargs[i].source_idx = i;
        sources[i] = (HANDLE)_beginthreadex(NULL, 0, t4b_source, &sargs[i], 0, NULL);
    }

    WaitForMultipleObjects(T4_SOURCES, sources, TRUE, INFINITE);
    DWORD wr = WaitForMultipleObjects(T4_RECEIVERS, receivers, TRUE, 30000);
    if (wr == WAIT_TIMEOUT) fail(name, "one or more receivers never returned (hard hang)");

    for (int i = 0; i < T4_SOURCES; i++) CloseHandle(sources[i]);
    for (int i = 0; i < T4_RECEIVERS; i++) CloseHandle(receivers[i]);
    DeleteCriticalSection(&sh.seen_lock);

    int expected_total = T4_SOURCES * T4B_PER_SOURCE;
    if (sh.total_received != expected_total) {
        fail(name, "total_received=%ld expected=%d (dropped or over-consumed)", sh.total_received, expected_total);
    }
    int missing = 0;
    for (int i = 0; i < T4_SOURCES; i++) {
        for (int s = 0; s < T4B_PER_SOURCE; s++) {
            if (!sh.seen[i][s]) missing++;
        }
        free(sh.seen[i]);
    }
    if (missing > 0) fail(name, "%d (source,seq) events never delivered to any receiver", missing);

    int ok = (g_fail_count == before);
    printf("[%s] total_events=%ld total_received=%ld dup_violations=%ld missing=%d -> %s\n",
           name, (long)sh.total_events, (long)sh.total_received, (long)sh.duplicate_violations, missing, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

static int test4_event_queue(void)
{
    int a = test4a_fifo_per_source();
    int b = test4b_exactly_once_delivery();
    return (a || b) ? 1 : 0;
}

/* =========================================================================
 * TEST 6 -- recursive mutex re-entry through trylock
 *
 * A guest lock and its host acquisition are one to one: unlock releases the
 * host mutex once per guest unlock, so every guest lock has to take it once.
 * A recursive re-entry through trylock is a guest lock like any other. The
 * POSIX branch used to bump lock_count and skip the host lock, so a mutex
 * held twice by the guest was held once by pthreads and the FIRST guest
 * unlock handed it to whoever was waiting.
 *
 * The probe is the window between the two unlocks: the owner is still one
 * level deep, so a foreign trylock must still be EBUSY.
 * ========================================================================= */
#define T6_THREADS  4
#define T6_ITERS    5000

typedef struct {
    uint32_t mutex_id;
    HANDLE   owner_re_entered;   /* owner holds it twice */
    HANDLE   probe_one_done;     /* helper probed at depth 2 */
    HANDLE   owner_dropped_one;  /* owner unlocked once, still holds it */
    HANDLE   probe_two_done;
    LONG volatile counter;
} t6_shared;

static unsigned __stdcall t6_owner(void* p)
{
    t6_shared* sh = (t6_shared*)p;
    uint32_t tid = alloc_tid(); t_tid = tid;
    ppu_context ctx; ctx_init_for_thread(&ctx, tid);

    int32_t rc = call2((int64_t(*)(ppu_context*))sys_mutex_lock, &ctx, sh->mutex_id, 0);
    if (rc != CELL_OK) { fail("test6", "owner lock rc=0x%08X", (unsigned)rc); return 1; }

    rc = call1((int64_t(*)(ppu_context*))sys_mutex_trylock, &ctx, sh->mutex_id);
    if (rc != CELL_OK) { fail("test6", "owner recursive trylock rc=0x%08X", (unsigned)rc); return 1; }

    SetEvent(sh->owner_re_entered);
    WaitForSingleObject(sh->probe_one_done, 10000);

    rc = call1((int64_t(*)(ppu_context*))sys_mutex_unlock, &ctx, sh->mutex_id);
    if (rc != CELL_OK) fail("test6", "owner first unlock rc=0x%08X", (unsigned)rc);

    SetEvent(sh->owner_dropped_one);
    WaitForSingleObject(sh->probe_two_done, 10000);

    rc = call1((int64_t(*)(ppu_context*))sys_mutex_unlock, &ctx, sh->mutex_id);
    if (rc != CELL_OK) fail("test6", "owner second unlock rc=0x%08X", (unsigned)rc);
    return 0;
}

static unsigned __stdcall t6_prober(void* p)
{
    t6_shared* sh = (t6_shared*)p;
    uint32_t tid = alloc_tid(); t_tid = tid;
    ppu_context ctx; ctx_init_for_thread(&ctx, tid);

    WaitForSingleObject(sh->owner_re_entered, 10000);
    int32_t rc = call1((int64_t(*)(ppu_context*))sys_mutex_trylock, &ctx, sh->mutex_id);
    if (rc != (int32_t)CELL_EBUSY)
        fail("test6", "trylock at owner depth 2 rc=0x%08X, want CELL_EBUSY", (unsigned)rc);
    SetEvent(sh->probe_one_done);

    WaitForSingleObject(sh->owner_dropped_one, 10000);
    rc = call1((int64_t(*)(ppu_context*))sys_mutex_trylock, &ctx, sh->mutex_id);
    if (rc != (int32_t)CELL_EBUSY)
        fail("test6", "trylock at owner depth 1 rc=0x%08X, want CELL_EBUSY "
                      "(one guest unlock released the host mutex outright)", (unsigned)rc);
    SetEvent(sh->probe_two_done);

    /* Once the owner is out, it is ours. */
    for (int i = 0; i < 1000; i++) {
        rc = call1((int64_t(*)(ppu_context*))sys_mutex_trylock, &ctx, sh->mutex_id);
        if (rc == CELL_OK) {
            call1((int64_t(*)(ppu_context*))sys_mutex_unlock, &ctx, sh->mutex_id);
            return 0;
        }
        Sleep(1);
    }
    fail("test6", "mutex never became free after the owner released both levels");
    return 1;
}

static unsigned __stdcall t6_worker(void* p)
{
    t6_shared* sh = (t6_shared*)p;
    uint32_t tid = alloc_tid(); t_tid = tid;
    ppu_context ctx; ctx_init_for_thread(&ctx, tid);

    for (int i = 0; i < T6_ITERS; i++) {
        int32_t rc = call2((int64_t(*)(ppu_context*))sys_mutex_lock, &ctx, sh->mutex_id, 0);
        if (rc != CELL_OK) { fail("test6", "worker lock rc=0x%08X", (unsigned)rc); return 1; }
        rc = call1((int64_t(*)(ppu_context*))sys_mutex_trylock, &ctx, sh->mutex_id);
        if (rc != CELL_OK) { fail("test6", "worker re-entry rc=0x%08X", (unsigned)rc); return 1; }

        /* Non-atomic on purpose: the mutex is what makes it safe. */
        LONG v = sh->counter;
        sh->counter = v + 1;

        call1((int64_t(*)(ppu_context*))sys_mutex_unlock, &ctx, sh->mutex_id);
        call1((int64_t(*)(ppu_context*))sys_mutex_unlock, &ctx, sh->mutex_id);
    }
    return 0;
}

static int test6_recursive_mutex(void)
{
    const char* name = "test6_recursive_mutex";
    LONG before = g_fail_count;
    ppu_context ctx; ctx_init_for_thread(&ctx, alloc_tid());

    uint32_t attr = make_mutex_attr(SYS_SYNC_FIFO, 1 /* recursive */);
    uint32_t id_addr = guest_alloc(4);
    int32_t rc = call2((int64_t(*)(ppu_context*))sys_mutex_create, &ctx, id_addr, attr);
    if (rc != CELL_OK) { fail(name, "mutex_create rc=0x%08X", (unsigned)rc); return 1; }

    t6_shared sh; memset(&sh, 0, sizeof(sh));
    sh.mutex_id          = read_be32(id_addr);
    sh.owner_re_entered  = CreateEventA(NULL, TRUE, FALSE, NULL);
    sh.probe_one_done    = CreateEventA(NULL, TRUE, FALSE, NULL);
    sh.owner_dropped_one = CreateEventA(NULL, TRUE, FALSE, NULL);
    sh.probe_two_done    = CreateEventA(NULL, TRUE, FALSE, NULL);

    HANDLE owner  = (HANDLE)_beginthreadex(NULL, 0, t6_owner, &sh, 0, NULL);
    HANDLE prober = (HANDLE)_beginthreadex(NULL, 0, t6_prober, &sh, 0, NULL);
    if (WaitForSingleObject(owner, 20000) == WAIT_TIMEOUT)
        fail(name, "owner thread never returned");
    if (WaitForSingleObject(prober, 20000) == WAIT_TIMEOUT)
        fail(name, "prober thread never returned");
    CloseHandle(owner); CloseHandle(prober);
    CloseHandle(sh.owner_re_entered); CloseHandle(sh.probe_one_done);
    CloseHandle(sh.owner_dropped_one); CloseHandle(sh.probe_two_done);

    /* And the lock/re-enter/unlock/unlock cycle still excludes under load. */
    HANDLE th[T6_THREADS];
    for (int i = 0; i < T6_THREADS; i++)
        th[i] = (HANDLE)_beginthreadex(NULL, 0, t6_worker, &sh, 0, NULL);
    if (WaitForMultipleObjects(T6_THREADS, th, TRUE, 60000) == WAIT_TIMEOUT)
        fail(name, "worker threads never returned");
    for (int i = 0; i < T6_THREADS; i++) CloseHandle(th[i]);

    if (sh.counter != (LONG)(T6_THREADS * T6_ITERS))
        fail(name, "counter=%ld, want %d (the recursive mutex did not exclude)",
             (long)sh.counter, T6_THREADS * T6_ITERS);

    call1((int64_t(*)(ppu_context*))sys_mutex_destroy, &ctx, sh.mutex_id);

    int ok = (g_fail_count == before);
    printf("[%s] counter=%ld -> %s\n", name, (long)sh.counter, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* =========================================================================
 * TEST 5 -- rwlock write ownership
 *
 * The write lock has an owner, and three answers depend on it: a second
 * wlock or trywlock by the owner is CELL_EDEADLK (never a block, because the
 * host lock is not recursive), and a wunlock by anyone else is CELL_EPERM
 * with the owner's lock left standing. The POSIX branch used to carry none
 * of that: the re-lock blocked forever on Darwin and a foreign wunlock
 * released whatever the caller happened to hold.
 *
 * The owner's sequence runs on its own thread so a regression shows up as a
 * FAIL with a message rather than wedging the whole suite.
 * ========================================================================= */
#define T5_WRITERS   4
#define T5_READERS   4
#define T5_ITERS     2000

typedef struct {
    uint32_t rw_id;
    HANDLE   owner_has_lock;    /* owner acquired, helper may probe */
    HANDLE   helper_done;       /* helper finished probing */
    LONG volatile counter;      /* guarded by the write lock */
    LONG volatile torn_reads;
} t5_shared;

static unsigned __stdcall t5_owner(void* p)
{
    t5_shared* sh = (t5_shared*)p;
    uint32_t tid = alloc_tid(); t_tid = tid;
    ppu_context ctx; ctx_init_for_thread(&ctx, tid);

    int32_t rc = call2((int64_t(*)(ppu_context*))sys_rwlock_wlock, &ctx, sh->rw_id, 0);
    if (rc != CELL_OK) { fail("test5", "owner wlock rc=0x%08X", (unsigned)rc); return 1; }

    /* Re-entry by the owner: an answer, not a block. */
    rc = call2((int64_t(*)(ppu_context*))sys_rwlock_wlock, &ctx, sh->rw_id, 0);
    if (rc != (int32_t)CELL_EDEADLK)
        fail("test5", "owner re-wlock rc=0x%08X, want CELL_EDEADLK", (unsigned)rc);

    rc = call1((int64_t(*)(ppu_context*))sys_rwlock_trywlock, &ctx, sh->rw_id);
    if (rc != (int32_t)CELL_EDEADLK)
        fail("test5", "owner trywlock rc=0x%08X, want CELL_EDEADLK", (unsigned)rc);

    SetEvent(sh->owner_has_lock);
    WaitForSingleObject(sh->helper_done, 10000);

    rc = call1((int64_t(*)(ppu_context*))sys_rwlock_wunlock, &ctx, sh->rw_id);
    if (rc != CELL_OK) fail("test5", "owner wunlock rc=0x%08X", (unsigned)rc);
    return 0;
}

static unsigned __stdcall t5_helper(void* p)
{
    t5_shared* sh = (t5_shared*)p;
    uint32_t tid = alloc_tid(); t_tid = tid;
    ppu_context ctx; ctx_init_for_thread(&ctx, tid);

    WaitForSingleObject(sh->owner_has_lock, 10000);

    int32_t rc = call1((int64_t(*)(ppu_context*))sys_rwlock_trywlock, &ctx, sh->rw_id);
    if (rc != (int32_t)CELL_EBUSY)
        fail("test5", "non-owner trywlock rc=0x%08X, want CELL_EBUSY", (unsigned)rc);

    /* The unlock a non-owner is not entitled to. */
    rc = call1((int64_t(*)(ppu_context*))sys_rwlock_wunlock, &ctx, sh->rw_id);
    if (rc != (int32_t)CELL_EPERM)
        fail("test5", "non-owner wunlock rc=0x%08X, want CELL_EPERM", (unsigned)rc);

    /* ... and which must not have taken the lock away from its owner. */
    rc = call1((int64_t(*)(ppu_context*))sys_rwlock_trywlock, &ctx, sh->rw_id);
    if (rc != (int32_t)CELL_EBUSY)
        fail("test5", "trywlock after a refused wunlock rc=0x%08X, want CELL_EBUSY "
                      "(the refused unlock released the owner's lock)", (unsigned)rc);

    SetEvent(sh->helper_done);
    return 0;
}

static unsigned __stdcall t5_writer(void* p)
{
    t5_shared* sh = (t5_shared*)p;
    uint32_t tid = alloc_tid(); t_tid = tid;
    ppu_context ctx; ctx_init_for_thread(&ctx, tid);

    for (int i = 0; i < T5_ITERS; i++) {
        int32_t rc = call2((int64_t(*)(ppu_context*))sys_rwlock_wlock, &ctx, sh->rw_id, 0);
        if (rc != CELL_OK) { fail("test5", "writer wlock rc=0x%08X", (unsigned)rc); return 1; }
        /* Under the exclusive lock the read-modify-write needs no atomic;
         * that is exactly what is being asserted. */
        LONG v = sh->counter;
        sh->counter = v + 1;
        rc = call1((int64_t(*)(ppu_context*))sys_rwlock_wunlock, &ctx, sh->rw_id);
        if (rc != CELL_OK) { fail("test5", "writer wunlock rc=0x%08X", (unsigned)rc); return 1; }
    }
    return 0;
}

static unsigned __stdcall t5_reader(void* p)
{
    t5_shared* sh = (t5_shared*)p;
    uint32_t tid = alloc_tid(); t_tid = tid;
    ppu_context ctx; ctx_init_for_thread(&ctx, tid);

    for (int i = 0; i < T5_ITERS; i++) {
        int32_t rc = call2((int64_t(*)(ppu_context*))sys_rwlock_rlock, &ctx, sh->rw_id, 0);
        if (rc != CELL_OK) { fail("test5", "reader rlock rc=0x%08X", (unsigned)rc); return 1; }
        LONG a = sh->counter;
        LONG b = sh->counter;
        if (a != b) InterlockedIncrement(&sh->torn_reads);
        rc = call1((int64_t(*)(ppu_context*))sys_rwlock_runlock, &ctx, sh->rw_id);
        if (rc != CELL_OK) { fail("test5", "reader runlock rc=0x%08X", (unsigned)rc); return 1; }
    }
    return 0;
}

static int test5_rwlock_ownership(void)
{
    const char* name = "test5_rwlock_ownership";
    LONG before = g_fail_count;
    ppu_context ctx; ctx_init_for_thread(&ctx, alloc_tid());

    uint32_t id_addr = guest_alloc(4);
    int32_t rc = call2((int64_t(*)(ppu_context*))sys_rwlock_create, &ctx, id_addr, 0);
    if (rc != CELL_OK) { fail(name, "rwlock_create rc=0x%08X", (unsigned)rc); return 1; }

    t5_shared sh; memset(&sh, 0, sizeof(sh));
    sh.rw_id = read_be32(id_addr);
    sh.owner_has_lock = CreateEventA(NULL, TRUE, FALSE, NULL);
    sh.helper_done    = CreateEventA(NULL, TRUE, FALSE, NULL);

    /* Nobody holds it yet, so this unlock belongs to nobody either. */
    rc = call1((int64_t(*)(ppu_context*))sys_rwlock_wunlock, &ctx, sh.rw_id);
    if (rc != (int32_t)CELL_EPERM)
        fail(name, "wunlock with no writer rc=0x%08X, want CELL_EPERM", (unsigned)rc);

    HANDLE owner  = (HANDLE)_beginthreadex(NULL, 0, t5_owner, &sh, 0, NULL);
    HANDLE helper = (HANDLE)_beginthreadex(NULL, 0, t5_helper, &sh, 0, NULL);
    if (WaitForSingleObject(owner, 20000) == WAIT_TIMEOUT)
        fail(name, "owner thread never returned (the owner's re-lock blocked)");
    if (WaitForSingleObject(helper, 20000) == WAIT_TIMEOUT)
        fail(name, "helper thread never returned");
    CloseHandle(owner); CloseHandle(helper);
    CloseHandle(sh.owner_has_lock); CloseHandle(sh.helper_done);

    /* The lock still works after all that, and still excludes. */
    HANDLE th[T5_WRITERS + T5_READERS];
    for (int i = 0; i < T5_WRITERS; i++)
        th[i] = (HANDLE)_beginthreadex(NULL, 0, t5_writer, &sh, 0, NULL);
    for (int i = 0; i < T5_READERS; i++)
        th[T5_WRITERS + i] = (HANDLE)_beginthreadex(NULL, 0, t5_reader, &sh, 0, NULL);
    if (WaitForMultipleObjects(T5_WRITERS + T5_READERS, th, TRUE, 60000) == WAIT_TIMEOUT)
        fail(name, "writer/reader threads never returned");
    for (int i = 0; i < T5_WRITERS + T5_READERS; i++) CloseHandle(th[i]);

    if (sh.counter != (LONG)(T5_WRITERS * T5_ITERS))
        fail(name, "counter=%ld, want %d (the write lock did not exclude)",
             (long)sh.counter, T5_WRITERS * T5_ITERS);
    if (sh.torn_reads != 0)
        fail(name, "%ld torn reads under the read lock", (long)sh.torn_reads);

    call1((int64_t(*)(ppu_context*))sys_rwlock_destroy, &ctx, sh.rw_id);

    int ok = (g_fail_count == before);
    printf("[%s] counter=%ld torn_reads=%ld -> %s\n",
           name, (long)sh.counter, (long)sh.torn_reads, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* =========================================================================
 * main
 * ========================================================================= */
int main(int argc, char** argv)
{
    g_seed = 0;
    if (argc > 1) {
        g_seed = (uint32_t)strtoul(argv[1], NULL, 10);
    }
    /* xorshift32 requires a nonzero state. */
    if (g_seed == 0) g_seed = 0x2545F491u;
    printf("=== ps3recomp sync_stress -- seed=%u ===\n", (unsigned)(argc > 1 ? strtoul(argv[1], NULL, 10) : 0));

    ULONGLONG t_start = now_ms();

    int rc1a = test1b_rendezvous_park();
    int rc1 = test1_no_lost_wakeup();
    int rc2 = test2_timed_wait_semantics();
    int rc3 = test3_semaphore_counting();
    int rc4 = test4_event_queue();
    int rc5 = test5_rwlock_ownership();
    int rc6 = test6_recursive_mutex();

    ULONGLONG total_ms = now_ms() - t_start;
    printf("=== total wall time: %llums ===\n", (unsigned long long)total_ms);

    int failed = rc1a || rc1 || rc2 || rc3 || rc4 || rc5 || rc6 || (g_fail_count != 0);
    printf("=== RESULT: %s (fail_count=%ld) ===\n", failed ? "FAIL" : "PASS", (long)g_fail_count);
    return failed ? 1 : 0;
}
