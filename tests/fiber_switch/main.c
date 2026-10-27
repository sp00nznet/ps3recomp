/*
 * ps3recomp - tests/fiber_switch/main.c
 *
 * cellFiber's scheduler API, on whatever mechanism the host uses to switch stacks: Windows fibers on Windows,
 * ucontext elsewhere. Apple marks the ucontext routines deprecated and hides them behind _XOPEN_SOURCE, which raises
 * the fair question of whether they still work on arm64 -- so ask, several thousand switches at a time, instead of
 * assuming either way.
 *
 * What is checked:
 *   1. Run order: the lowest priority number first, creation order among equals; each fiber starts with the argument
 *      it was created with, and r1 inside the guest stack it was given. A fiber that starts with the wrong argument is
 *      the readable symptom of a context save that landed outside the struct it was given.
 *   2. WaitSignal / SendSignal ping-pong between two fibers, thousands of switches, with a stack sentinel in each
 *      fiber that must survive every switch. A signal sent before the wait is not lost.
 *   3. JoinFiber from a fiber (it hands control back to the scheduler meanwhile) and from outside, with the exit
 *      status; Exit ends a fiber on the spot; Yield goes behind the fibers of the same priority; Self.
 *   4. A signal sent from another host thread wakes a scheduler that had nothing left to run (POSIX only).
 *   5. The error codes, and Finalize while the scheduler runs.
 *
 * The guest entry is a host function: the PPU thread trampoline is a stub that finds it by the descriptor address in
 * ctx->cia and puts its result in r3, as the real trampoline does for lifted code.
 *
 * Exit code 0 = pass.
 */
#include "cellFiber.h"
#include "../../runtime/syscalls/sys_ppu_thread.h"   /* ppu_context, ppu_thread_entry_fn */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#include <unistd.h>
#endif

/* ---------------------------------------------------------------------------
 * Guest memory. cellFiber.c writes through vm_write32/vm_memset, so a plain host buffer standing in for guest memory is
 * all it needs (the same shim tests/sync_stress uses).
 * -----------------------------------------------------------------------*/
#define GUEST_MEM_SIZE (1u * 1024u * 1024u)
static uint8_t g_guest_mem[GUEST_MEM_SIZE];
uint8_t* vm_base = g_guest_mem;

/* ppu_memory.h's inline stores reference these. Nothing here watches guest writes or holds a reservation. */
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int      g_resv_store_active = 0;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t addr, uint64_t val, int width)
{
    (void)addr; (void)val; (void)width;
}

/* PPU <-> SPU lock-line coherence (runtime/spu/spu_coherency.c), which every store consults. This suite runs no SPU, so
 * no line is ever reserved and the store path is the plain memcpy it always was. */
int  spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
void spu_lockline_lock(void)   { }
void spu_lockline_unlock(void) { }
void spu_coh_notify_write(uint32_t addr) { (void)addr; }

static uint32_t g_bump = 4096;   /* keep guest address 0 invalid */
static uint32_t guest_alloc(uint32_t size, uint32_t align)
{
    g_bump = (g_bump + align - 1u) & ~(align - 1u);
    uint32_t a = g_bump;
    g_bump += size;
    return a;
}

static uint32_t read_be32(uint32_t addr)
{
    const uint8_t* p = g_guest_mem + addr;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* ---------------------------------------------------------------------------
 * The PPU thread side the library calls into
 * -----------------------------------------------------------------------*/
PPU_THREAD_LOCAL ppu_context* g_active_ctx;

typedef int32_t (*guest_fn)(uint64_t arg);
#define ENTRY_BASE 0x1000u
#define MAX_ENTRIES 16
static guest_fn g_entries[MAX_ENTRIES];

static uint32_t register_entry(guest_fn fn)
{
    for (uint32_t i = 0; i < MAX_ENTRIES; i++)
        if (!g_entries[i]) { g_entries[i] = fn; return ENTRY_BASE + i; }
    return 0;
}

/* The ppu_context the running fiber was given: r1 is checked against the guest stack. */
static void trampoline(ppu_context* ctx)
{
    guest_fn fn = g_entries[ctx->cia - ENTRY_BASE];
    ctx->gpr[3] = (uint64_t)(uint32_t)fn(ctx->gpr[3]);
}
ppu_thread_entry_fn g_ppu_thread_entry_trampoline = trampoline;

/* ---------------------------------------------------------------------------
 * Failure bookkeeping
 * -----------------------------------------------------------------------*/
static int g_fails = 0;
#define FAILF(...) do { g_fails++; fprintf(stderr, "[FAIL] "); \
                        fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define CHECK(c) do { if (!(c)) FAILF("line %d: %s", __LINE__, #c); } while (0)
#define CHECK_RC(call, want) do { s32 rc_ = (call); \
    if (rc_ != (s32)(want)) FAILF("line %d: %s = 0x%08X, want 0x%08X", __LINE__, #call, (unsigned)rc_, (unsigned)(want)); \
    } while (0)

/* A scheduler and its fibers, all in guest memory. */
#define STACK_SIZE 0x4000u
typedef struct { uint32_t ea, stack; } fiber_ref;

static CellFiberPpuScheduler* new_scheduler(void)
{
    uint32_t ea = guest_alloc(CELL_FIBER_PPU_SCHEDULER_SIZE, CELL_FIBER_PPU_SCHEDULER_ALIGN);
    CHECK_RC(cellFiberPpuInitializeScheduler((CellFiberPpuScheduler*)(uintptr_t)ea, NULL), CELL_OK);
    return (CellFiberPpuScheduler*)(uintptr_t)ea;
}

static fiber_ref new_fiber(CellFiberPpuScheduler* s, guest_fn fn, uint64_t arg, uint32_t priority)
{
    fiber_ref r;
    r.ea    = guest_alloc(CELL_FIBER_PPU_SIZE, CELL_FIBER_PPU_ALIGN);
    r.stack = guest_alloc(STACK_SIZE, 16);
    CHECK_RC(cellFiberPpuCreateFiber(s, (CellFiberPpu*)(uintptr_t)r.ea, register_entry(fn), arg, priority,
                                     (void*)(uintptr_t)r.stack, STACK_SIZE, NULL), CELL_OK);
    return r;
}

#define FIBER(r) ((CellFiberPpu*)(uintptr_t)(r).ea)

static int32_t join_status(fiber_ref r)
{
    uint32_t out = guest_alloc(4, 4);
    CHECK_RC(cellFiberPpuJoinFiber(FIBER(r), (s32*)(uintptr_t)out), CELL_OK);
    return (int32_t)read_be32(out);
}

/* ---------------------------------------------------------------------------
 * Case 1: run order, arguments, guest stacks, exit status
 * -----------------------------------------------------------------------*/
#define ORDER_FIBERS 3
static int       order_seen[ORDER_FIBERS], order_n;
static uint64_t  order_args[ORDER_FIBERS];
static fiber_ref order_fibers[ORDER_FIBERS];

static int32_t order_entry(uint64_t arg)
{
    const ppu_context* me = g_active_ctx;
    const fiber_ref mine = order_fibers[arg & 0xFF];
    order_args[order_n] = arg;
    order_seen[order_n] = (int)(arg & 0xFF);
    /* r1 is inside the guest stack this fiber was created with, 16-byte aligned */
    CHECK(me != NULL && me->gpr[1] > mine.stack && me->gpr[1] <= mine.stack + STACK_SIZE && (me->gpr[1] & 0xF) == 0);
    order_n++;
    return (int32_t)(arg & 0xFFFF);
}

static void case_order(void)
{
    CellFiberPpuScheduler* s = new_scheduler();
    order_n = 0;
    /* priorities 5, 1, 5, so the order is 1 (priority 1), 0, 2 (creation order) */
    order_fibers[0] = new_fiber(s, order_entry, 0xA5A5000000000100ull, 5);
    order_fibers[1] = new_fiber(s, order_entry, 0x5A5A000000000101ull, 1);
    order_fibers[2] = new_fiber(s, order_entry, 0x3C3C000000000102ull, 5);
    CHECK_RC(cellFiberPpuRunFibers(s), CELL_OK);
    CHECK(order_n == 3);
    CHECK(order_seen[0] == 0x01 && order_seen[1] == 0x00 && order_seen[2] == 0x02);
    CHECK(order_args[0] == 0x5A5A000000000101ull && order_args[1] == 0xA5A5000000000100ull &&
          order_args[2] == 0x3C3C000000000102ull);
    CHECK(join_status(order_fibers[0]) == 0x0100);
    CHECK(join_status(order_fibers[1]) == 0x0101);
    CHECK(join_status(order_fibers[2]) == 0x0102);
    CHECK_RC(cellFiberPpuFinalizeScheduler(s), CELL_OK);
    printf("[case order] ok\n");
}

/* ---------------------------------------------------------------------------
 * Case 2: WaitSignal / SendSignal ping-pong
 * -----------------------------------------------------------------------*/
#define SWITCHES 4000
static fiber_ref ping, pong;
static int ping_n, pong_n, stack_damage;

/* A pattern in the fiber's own host stack that every switch must leave alone. */
static void check_sentinel(volatile uint8_t* buf, unsigned n, uint8_t seed)
{
    for (unsigned k = 0; k < n; k++)
        if (buf[k] != (uint8_t)(seed + k)) { stack_damage++; return; }
}

static int32_t ping_entry(uint64_t arg)
{
    volatile uint8_t buf[256];
    (void)arg;
    for (int i = 0; i < SWITCHES; i++) {
        for (unsigned k = 0; k < sizeof buf; k++) buf[k] = (uint8_t)(i + k);
        ping_n++;
        CHECK_RC(cellFiberPpuSendSignal(FIBER(pong), NULL), CELL_OK);   /* before pong ever waits, for the first */
        CHECK_RC(cellFiberPpuWaitSignal(), CELL_OK);
        check_sentinel(buf, sizeof buf, (uint8_t)i);
    }
    return 0;
}

static int32_t pong_entry(uint64_t arg)
{
    volatile uint8_t buf[256];
    (void)arg;
    for (int i = 0; i < SWITCHES; i++) {
        for (unsigned k = 0; k < sizeof buf; k++) buf[k] = (uint8_t)(i * 3 + k);
        CHECK_RC(cellFiberPpuWaitSignal(), CELL_OK);
        pong_n++;
        check_sentinel(buf, sizeof buf, (uint8_t)(i * 3));
        CHECK_RC(cellFiberPpuSendSignal(FIBER(ping), NULL), CELL_OK);
    }
    return 0;
}

static void case_signal_pingpong(void)
{
    CellFiberPpuScheduler* s = new_scheduler();
    ping_n = pong_n = stack_damage = 0;
    ping = new_fiber(s, ping_entry, 0, 1);
    pong = new_fiber(s, pong_entry, 0, 1);
    CHECK_RC(cellFiberPpuRunFibers(s), CELL_OK);
    CHECK(ping_n == SWITCHES && pong_n == SWITCHES);
    CHECK(stack_damage == 0);
    CHECK_RC(cellFiberPpuFinalizeScheduler(s), CELL_OK);
    printf("[case pingpong] %d round trips, stack damage %d\n", SWITCHES, stack_damage);
}

/* ---------------------------------------------------------------------------
 * Case 3: JoinFiber from a fiber, Exit, Yield, Self
 * -----------------------------------------------------------------------*/
static fiber_ref join_waiter, join_target, exit_fiber, self_fiber;
static int32_t joined_status;
static int     after_exit_ran, target_ran_before_join_returned;
static uint32_t self_seen;
static char    y_log[32];
static int     y_n;

static int32_t target_entry(uint64_t arg)
{
    (void)arg;
    target_ran_before_join_returned = 1;
    return 77;
}

static int32_t waiter_entry(uint64_t arg)
{
    (void)arg;
    uint32_t out = guest_alloc(4, 4);
    CHECK_RC(cellFiberPpuJoinFiber(FIBER(join_target), (s32*)(uintptr_t)out), CELL_OK);
    CHECK(target_ran_before_join_returned);     /* the join really waited */
    joined_status = (int32_t)read_be32(out);
    return 0;
}

static int32_t exiting_entry(uint64_t arg)
{
    (void)arg;
    cellFiberPpuExit(-5);
    after_exit_ran = 1;                          /* must never run */
    return 9;
}

static int32_t yield_entry(uint64_t arg)
{
    for (int i = 0; i < 3; i++) {
        y_log[y_n++] = (char)arg;
        CHECK_RC(cellFiberPpuYield(), CELL_OK);
    }
    return 0;
}

static int32_t self_entry(uint64_t arg)
{
    (void)arg;
    self_seen = (uint32_t)(uintptr_t)cellFiberPpuSelf();
    return 0;
}

static void case_join_exit_yield_self(void)
{
    CellFiberPpuScheduler* s = new_scheduler();
    /* the waiter has the lower number, so it runs first, blocks, and the target runs behind it */
    join_waiter = new_fiber(s, waiter_entry, 0, 1);
    join_target = new_fiber(s, target_entry, 0, 2);
    CHECK_RC(cellFiberPpuRunFibers(s), CELL_OK);
    CHECK(joined_status == 77);
    CHECK(join_status(join_waiter) == 0);

    exit_fiber = new_fiber(s, exiting_entry, 0, 1);
    CHECK_RC(cellFiberPpuRunFibers(s), CELL_OK);
    CHECK(!after_exit_ran);
    CHECK(join_status(exit_fiber) == -5);

    new_fiber(s, yield_entry, 'a', 1);
    new_fiber(s, yield_entry, 'b', 1);
    CHECK_RC(cellFiberPpuRunFibers(s), CELL_OK);
    y_log[y_n] = 0;
    if (strcmp(y_log, "ababab"))                /* each yield goes behind the other fiber of its priority */
        FAILF("yield order: %s", y_log);

    self_fiber = new_fiber(s, self_entry, 0, 1);
    CHECK_RC(cellFiberPpuRunFibers(s), CELL_OK);
    CHECK(self_seen == self_fiber.ea);
    CHECK(cellFiberPpuSelf() == NULL);           /* outside a fiber */

    CHECK_RC(cellFiberPpuFinalizeScheduler(s), CELL_OK);
    printf("[case join/exit/yield/self] ok\n");
}

/* ---------------------------------------------------------------------------
 * Case 4: a signal from another host thread
 * -----------------------------------------------------------------------*/
#ifndef _WIN32
static fiber_ref waiter4;
static volatile int woke4, sender_done;

static int32_t waiter4_entry(uint64_t arg)
{
    (void)arg;
    CHECK_RC(cellFiberPpuWaitSignal(), CELL_OK);
    woke4 = 1;
    return 4;
}

static void* sender4(void* unused)
{
    (void)unused;
    usleep(30 * 1000);                           /* RunFibers has nothing left to run by now */
    sender_done = 1;
    cellFiberPpuSendSignal(FIBER(waiter4), NULL);
    return NULL;
}

static void case_signal_from_thread(void)
{
    CellFiberPpuScheduler* s = new_scheduler();
    waiter4 = new_fiber(s, waiter4_entry, 0, 1);
    pthread_t t;
    pthread_create(&t, NULL, sender4, NULL);
    CHECK_RC(cellFiberPpuRunFibers(s), CELL_OK);
    CHECK(sender_done && woke4);                 /* RunFibers waited for the signal instead of returning */
    pthread_join(t, NULL);
    CHECK(join_status(waiter4) == 4);
    CHECK_RC(cellFiberPpuFinalizeScheduler(s), CELL_OK);
    printf("[case thread signal] ok\n");
}
#endif

/* ---------------------------------------------------------------------------
 * Case 5: errors
 * -----------------------------------------------------------------------*/
static CellFiberPpuScheduler* g_err_sched;
static int32_t finalize_inside_entry(uint64_t arg)
{
    (void)arg;
    CHECK_RC(cellFiberPpuFinalizeScheduler(g_err_sched), CELL_FIBER_ERROR_STAT);
    CHECK_RC(cellFiberPpuRunFibers(g_err_sched), CELL_FIBER_ERROR_STAT);   /* not from inside a fiber */
    return 0;
}

static int32_t nop_entry(uint64_t arg) { (void)arg; return 0; }

static void case_errors(void)
{
    /* attributes */
    uint32_t attr = guest_alloc(CELL_FIBER_PPU_ATTRIBUTE_SIZE + 16, 16);
    memset(g_guest_mem + attr, 0xA5, CELL_FIBER_PPU_ATTRIBUTE_SIZE + 16);
    CHECK_RC(_cellFiberPpuAttributeInitialize((CellFiberPpuAttribute*)(uintptr_t)attr, 0x360001), CELL_OK);
    int zeroed = 1, untouched = 1;
    for (unsigned i = 0; i < CELL_FIBER_PPU_ATTRIBUTE_SIZE; i++) if (g_guest_mem[attr + i]) zeroed = 0;
    for (unsigned i = 0; i < 16; i++) if (g_guest_mem[attr + CELL_FIBER_PPU_ATTRIBUTE_SIZE + i] != 0xA5) untouched = 0;
    CHECK(zeroed && untouched);
    memset(g_guest_mem + attr, 0xA5, CELL_FIBER_PPU_ATTRIBUTE_SIZE + 16);
    CHECK_RC(_cellFiberPpuSchedulerAttributeInitialize((CellFiberPpuSchedulerAttribute*)(uintptr_t)attr, 0x360001), CELL_OK);
    zeroed = 1; untouched = 1;
    for (unsigned i = 0; i < CELL_FIBER_PPU_SCHEDULER_ATTRIBUTE_SIZE; i++) if (g_guest_mem[attr + i]) zeroed = 0;
    for (unsigned i = 0; i < 16; i++) if (g_guest_mem[attr + CELL_FIBER_PPU_SCHEDULER_ATTRIBUTE_SIZE + i] != 0xA5) untouched = 0;
    CHECK(zeroed && untouched);
    CHECK_RC(_cellFiberPpuAttributeInitialize(NULL, 0), CELL_FIBER_ERROR_NULL_POINTER);
    CHECK_RC(_cellFiberPpuSchedulerAttributeInitialize(NULL, 0), CELL_FIBER_ERROR_NULL_POINTER);
    CHECK_RC(_cellFiberPpuInitialize(), CELL_OK);

    /* scheduler */
    uint32_t s_ea = guest_alloc(CELL_FIBER_PPU_SCHEDULER_SIZE, CELL_FIBER_PPU_SCHEDULER_ALIGN);
    CellFiberPpuScheduler* s = (CellFiberPpuScheduler*)(uintptr_t)s_ea;
    CHECK_RC(cellFiberPpuInitializeScheduler(NULL, NULL), CELL_FIBER_ERROR_NULL_POINTER);
    CHECK_RC(cellFiberPpuInitializeScheduler((CellFiberPpuScheduler*)(uintptr_t)(s_ea + 16), NULL), CELL_FIBER_ERROR_ALIGN);
    CHECK_RC(cellFiberPpuRunFibers(s), CELL_FIBER_ERROR_INVAL);                 /* not initialised */
    CHECK_RC(cellFiberPpuFinalizeScheduler(s), CELL_FIBER_ERROR_INVAL);
    CHECK_RC(cellFiberPpuInitializeScheduler(s, NULL), CELL_OK);
    CHECK_RC(cellFiberPpuInitializeScheduler(s, NULL), CELL_FIBER_ERROR_STAT);  /* twice */

    /* fibers */
    uint32_t f_ea = guest_alloc(CELL_FIBER_PPU_SIZE, CELL_FIBER_PPU_ALIGN);
    uint32_t stk = guest_alloc(STACK_SIZE, 16);
    uint32_t opd = register_entry(nop_entry);
    CellFiberPpu* f = (CellFiberPpu*)(uintptr_t)f_ea;
    CHECK_RC(cellFiberPpuCreateFiber(NULL, f, opd, 0, 1, (void*)(uintptr_t)stk, STACK_SIZE, NULL), CELL_FIBER_ERROR_NULL_POINTER);
    CHECK_RC(cellFiberPpuCreateFiber(s, NULL, opd, 0, 1, (void*)(uintptr_t)stk, STACK_SIZE, NULL), CELL_FIBER_ERROR_NULL_POINTER);
    CHECK_RC(cellFiberPpuCreateFiber(s, f, 0, 0, 1, (void*)(uintptr_t)stk, STACK_SIZE, NULL), CELL_FIBER_ERROR_NULL_POINTER);
    CHECK_RC(cellFiberPpuCreateFiber(s, f, opd, 0, 1, NULL, STACK_SIZE, NULL), CELL_FIBER_ERROR_NULL_POINTER);
    CHECK_RC(cellFiberPpuCreateFiber(s, (CellFiberPpu*)(uintptr_t)(f_ea + 16), opd, 0, 1, (void*)(uintptr_t)stk, STACK_SIZE, NULL),
             CELL_FIBER_ERROR_ALIGN);
    CHECK_RC(cellFiberPpuCreateFiber(s, f, opd, 0, 1, (void*)(uintptr_t)stk, 0x100, NULL), CELL_FIBER_ERROR_INVAL);
    CHECK_RC(cellFiberPpuCreateFiber((CellFiberPpuScheduler*)(uintptr_t)(s_ea + CELL_FIBER_PPU_SCHEDULER_ALIGN), f, opd, 0, 1,
                                     (void*)(uintptr_t)stk, STACK_SIZE, NULL), CELL_FIBER_ERROR_INVAL);
    CHECK_RC(cellFiberPpuCreateFiber(s, f, opd, 0, 1, (void*)(uintptr_t)stk, STACK_SIZE, NULL), CELL_OK);
    CHECK_RC(cellFiberPpuCreateFiber(s, f, opd, 0, 1, (void*)(uintptr_t)stk, STACK_SIZE, NULL), CELL_FIBER_ERROR_STAT);

    /* signals and joins on things that do not exist, and calls that need a fiber */
    CHECK_RC(cellFiberPpuSendSignal(NULL, NULL), CELL_FIBER_ERROR_NULL_POINTER);
    CHECK_RC(cellFiberPpuSendSignal((CellFiberPpu*)(uintptr_t)(f_ea + CELL_FIBER_PPU_ALIGN), NULL), CELL_FIBER_ERROR_INVAL);
    CHECK_RC(cellFiberPpuJoinFiber(NULL, NULL), CELL_FIBER_ERROR_NULL_POINTER);
    CHECK_RC(cellFiberPpuJoinFiber((CellFiberPpu*)(uintptr_t)(f_ea + CELL_FIBER_PPU_ALIGN), NULL), CELL_FIBER_ERROR_INVAL);
    CHECK_RC(cellFiberPpuWaitSignal(), CELL_FIBER_ERROR_PERM);
    CHECK_RC(cellFiberPpuYield(), CELL_FIBER_ERROR_PERM);
    CHECK_RC(cellFiberPpuExit(0), CELL_FIBER_ERROR_PERM);
    CHECK_RC(cellFiberPpuRunFibers(NULL), CELL_FIBER_ERROR_NULL_POINTER);

    /* Finalize and a nested Run from inside a running scheduler */
    g_err_sched = new_scheduler();
    fiber_ref inner = new_fiber(g_err_sched, finalize_inside_entry, 0, 1);
    CHECK_RC(cellFiberPpuRunFibers(g_err_sched), CELL_OK);
    CHECK(join_status(inner) == 0);
    CHECK_RC(cellFiberPpuFinalizeScheduler(g_err_sched), CELL_OK);
    CHECK_RC(cellFiberPpuJoinFiber(FIBER(inner), NULL), CELL_FIBER_ERROR_INVAL);   /* gone with its scheduler */

    /* the first scheduler still has its never-run fiber; finalizing drops it */
    CHECK_RC(cellFiberPpuFinalizeScheduler(s), CELL_OK);
    CHECK_RC(cellFiberPpuSendSignal(f, NULL), CELL_FIBER_ERROR_INVAL);
    printf("[case errors] ok\n");
}

/* ---------------------------------------------------------------------------
 * main
 * -----------------------------------------------------------------------*/
int main(void)
{
    /* A switch that lands on the wrong stack takes the process down, so keep the progress lines out of a buffer that
     * dies with it. */
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== ps3recomp fiber_switch ===\n");

    case_order();
    case_signal_pingpong();
    case_join_exit_yield_self();
#ifndef _WIN32
    case_signal_from_thread();
#endif
    case_errors();

    printf("=== RESULT: %s (fails=%d) ===\n", g_fails ? "FAIL" : "PASS", g_fails);
    return g_fails ? 1 : 0;
}
