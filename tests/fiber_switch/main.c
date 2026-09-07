/*
 * ps3recomp - tests/fiber_switch/main.c
 *
 * cellFiber cooperative switching, on whatever mechanism the host uses:
 * Windows fibers on Windows, ucontext elsewhere. Apple marks the ucontext
 * routines deprecated and hides them behind _XOPEN_SOURCE, which raises the
 * fair question of whether they still work on arm64 -- so ask, several
 * thousand switches at a time, instead of assuming either way.
 *
 * Two shapes matter and they are not the same code path:
 *
 *   1. scheduler -> fiber -> yield -> scheduler, with each fiber checking the
 *      argument it was created with. A fiber that starts with the wrong
 *      argument is the readable symptom of a context save that landed
 *      outside the struct it was given.
 *   2. fiber -> fiber directly. cellFiberPpuSwitchFiber called from inside a
 *      running fiber has to save the CALLER's context, not the scheduler's.
 *
 * Exit code 0 = pass.
 */
#include "cellFiber.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Guest memory. cellFiber.c writes fiber ids and attribute fields through
 * vm_write32/vm_read32, so a plain host buffer standing in for guest memory
 * is all it needs (the same shim tests/sync_stress uses).
 * -----------------------------------------------------------------------*/
#define GUEST_MEM_SIZE (1u * 1024u * 1024u)
static uint8_t g_guest_mem[GUEST_MEM_SIZE];
uint8_t* vm_base = g_guest_mem;

/* ppu_memory.h's inline stores reference these. Nothing here watches guest
 * writes or holds a reservation. */
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int      g_resv_store_active = 0;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t addr, uint64_t val, int width)
{
    (void)addr; (void)val; (void)width;
}

/* PPU <-> SPU lock-line coherence (runtime/spu/spu_coherency.c), which every
 * store now consults. This suite runs no SPU, so no line is ever reserved and
 * the store path is the plain memcpy it always was. */
int  spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
void spu_lockline_lock(void)   { }
void spu_lockline_unlock(void) { }
void spu_coh_notify_write(uint32_t addr) { (void)addr; }

static uint32_t g_bump = 4096;   /* keep guest address 0 invalid */
static uint32_t guest_alloc(uint32_t size)
{
    uint32_t a = g_bump;
    g_bump += (size + 15u) & ~15u;
    return a;
}

static uint32_t read_be32(uint32_t addr)
{
    const uint8_t* p = g_guest_mem + addr;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* ---------------------------------------------------------------------------
 * Failure bookkeeping
 * -----------------------------------------------------------------------*/
static int g_fails = 0;
#define FAILF(...) do { g_fails++; fprintf(stderr, "[FAIL] "); \
                        fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)

#define SWITCHES  4000

/* ---------------------------------------------------------------------------
 * Case 1: the scheduler runs each fiber in turn and each yields straight back.
 * -----------------------------------------------------------------------*/
static CellFiber f_a, f_b;
static long ran_a, ran_b;
static long arg_mismatches;

static void entry_a(u64 arg)
{
    if (arg != 0xA5A5A5A5A5A5A5A5ull) arg_mismatches++;
    for (;;) { ran_a++; cellFiberPpuYieldFiber(); }
}

static void entry_b(u64 arg)
{
    if (arg != 0x5A5A5A5A5A5A5A5Aull) arg_mismatches++;
    for (;;) { ran_b++; cellFiberPpuYieldFiber(); }
}

static int case_scheduler_roundtrip(void)
{
    long before = g_fails;
    for (int i = 0; i < SWITCHES; i++) {
        s32 rc = cellFiberPpuSwitchFiber(f_a);
        if (rc != CELL_OK) { FAILF("switch to A on iteration %d: 0x%08X", i, (unsigned)rc); break; }
        rc = cellFiberPpuSwitchFiber(f_b);
        if (rc != CELL_OK) { FAILF("switch to B on iteration %d: 0x%08X", i, (unsigned)rc); break; }
    }
    if (ran_a != SWITCHES) FAILF("fiber A ran %ld times, want %d", ran_a, SWITCHES);
    if (ran_b != SWITCHES) FAILF("fiber B ran %ld times, want %d", ran_b, SWITCHES);
    if (arg_mismatches)    FAILF("%ld fiber entries saw the wrong arg", arg_mismatches);

    int ok = (g_fails == before);
    printf("[scheduler_roundtrip] A=%ld B=%ld -> %s\n", ran_a, ran_b, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* ---------------------------------------------------------------------------
 * Case 2: the two fibers hand off to each other and only the last one back
 * returns to the scheduler.
 *
 * Every switch appends to a trace, so a run that produces the right counts by
 * the wrong route still fails: the sequence has to alternate.
 * -----------------------------------------------------------------------*/
static CellFiber p_a, p_b;
static long ping_a, ping_b;
static int  last_seen;              /* 1 = A ran last, 2 = B */
static long order_violations;
static long deep_stack_damage;

static void note(int who)
{
    if (last_seen == who) order_violations++;
    last_seen = who;
}

/* A local array touched either side of the switch: if the switch resumed the
 * wrong stack, these do not survive. */
static void ping_entry_a(u64 arg)
{
    (void)arg;
    for (;;) {
        volatile uint64_t canary[8];
        for (int i = 0; i < 8; i++) canary[i] = 0x1111111100000000ull + (uint64_t)i;
        note(1);
        ping_a++;
        cellFiberPpuSwitchFiber(p_b);          /* fiber -> fiber */
        for (int i = 0; i < 8; i++)
            if (canary[i] != 0x1111111100000000ull + (uint64_t)i) { deep_stack_damage++; break; }
    }
}

static void ping_entry_b(u64 arg)
{
    (void)arg;
    for (;;) {
        volatile uint64_t canary[8];
        for (int i = 0; i < 8; i++) canary[i] = 0x2222222200000000ull + (uint64_t)i;
        note(2);
        ping_b++;
        if (ping_b >= SWITCHES) cellFiberPpuYieldFiber();   /* back to the scheduler */
        else                    cellFiberPpuSwitchFiber(p_a);
        for (int i = 0; i < 8; i++)
            if (canary[i] != 0x2222222200000000ull + (uint64_t)i) { deep_stack_damage++; break; }
    }
}

static int case_fiber_to_fiber(void)
{
    long before = g_fails;

    uint32_t ha = guest_alloc(4), hb = guest_alloc(4);
    s32 rc = cellFiberPpuCreateFiber((CellFiber*)(uintptr_t)ha, ping_entry_a, 0, NULL);
    if (rc != CELL_OK) { FAILF("create ping A: 0x%08X", (unsigned)rc); return 1; }
    rc = cellFiberPpuCreateFiber((CellFiber*)(uintptr_t)hb, ping_entry_b, 0, NULL);
    if (rc != CELL_OK) { FAILF("create ping B: 0x%08X", (unsigned)rc); return 1; }
    p_a = read_be32(ha);
    p_b = read_be32(hb);

    /* Enter the chain once; it comes back only when B has had its fill. */
    rc = cellFiberPpuSwitchFiber(p_a);
    if (rc != CELL_OK) FAILF("entering the ping-pong chain: 0x%08X", (unsigned)rc);

    if (ping_a != SWITCHES) FAILF("ping A ran %ld times, want %d", ping_a, SWITCHES);
    if (ping_b != SWITCHES) FAILF("ping B ran %ld times, want %d", ping_b, SWITCHES);
    if (order_violations)   FAILF("%ld switches did not alternate A/B", order_violations);
    if (deep_stack_damage)  FAILF("%ld resumes came back on the wrong stack", deep_stack_damage);

    /* The scheduler is still the scheduler: a plain round trip must work
     * after all that. A switch that clobbered the scheduler context tends to
     * survive the loop above and die here. */
    ran_a = 0;
    rc = cellFiberPpuSwitchFiber(f_a);
    if (rc != CELL_OK) FAILF("switch after the ping-pong chain: 0x%08X", (unsigned)rc);
    if (ran_a != 1) FAILF("fiber A ran %ld times after the chain, want 1", ran_a);

    int ok = (g_fails == before);
    printf("[fiber_to_fiber] A=%ld B=%ld order_violations=%ld stack_damage=%ld -> %s\n",
           ping_a, ping_b, order_violations, deep_stack_damage, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* ---------------------------------------------------------------------------
 * main
 * -----------------------------------------------------------------------*/
int main(void)
{
    /* A switch that lands on the wrong stack takes the process down, so keep
     * the progress lines out of a buffer that dies with it. */
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== ps3recomp fiber_switch ===\n");

    s32 rc = cellFiberPpuInitialize();
    if (rc != CELL_OK) { FAILF("Initialize: 0x%08X", (unsigned)rc); return 1; }

    /* Attributes come from guest memory: {const char* name; u32 priority;
     * u32 stackSize}, three 4-byte fields. */
    uint32_t attr = guest_alloc(16);
    rc = cellFiberPpuAttributeInitialize((CellFiberAttribute*)(uintptr_t)attr);
    if (rc != CELL_OK) FAILF("AttributeInitialize: 0x%08X", (unsigned)rc);
    if (read_be32(attr + 8) != CELL_FIBER_DEFAULT_STACK_SIZE)
        FAILF("AttributeInitialize left stackSize = %u, want %u",
              read_be32(attr + 8), (unsigned)CELL_FIBER_DEFAULT_STACK_SIZE);

    uint32_t ha = guest_alloc(4), hb = guest_alloc(4);
    rc = cellFiberPpuCreateFiber((CellFiber*)(uintptr_t)ha, entry_a,
                                 0xA5A5A5A5A5A5A5A5ull,
                                 (const CellFiberAttribute*)(uintptr_t)attr);
    if (rc != CELL_OK) { FAILF("CreateFiber A: 0x%08X", (unsigned)rc); return 1; }
    rc = cellFiberPpuCreateFiber((CellFiber*)(uintptr_t)hb, entry_b,
                                 0x5A5A5A5A5A5A5A5Aull,
                                 (const CellFiberAttribute*)(uintptr_t)attr);
    if (rc != CELL_OK) { FAILF("CreateFiber B: 0x%08X", (unsigned)rc); return 1; }
    f_a = read_be32(ha);
    f_b = read_be32(hb);
    if (f_a == f_b) FAILF("both fibers got id %u", (unsigned)f_a);

    int rc1 = case_scheduler_roundtrip();
    int rc2 = case_fiber_to_fiber();

    rc = cellFiberPpuFinalize();
    if (rc != CELL_OK) FAILF("Finalize: 0x%08X", (unsigned)rc);

    int failed = rc1 || rc2 || g_fails;
    printf("=== RESULT: %s (fails=%d) ===\n", failed ? "FAIL" : "PASS", g_fails);
    return failed ? 1 : 0;
}
