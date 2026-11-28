/* Exercise the real wait loop with a deterministic clock, independently of host
 * scheduling. No production clock or timeout is changed. */
#include "../runtime/platform/win32_compat.h"
static ULONGLONG test_tick(void);
static void test_sleep(DWORD ms);
static BOOL test_yield(void);
#define GetTickCount64 test_tick
#define Sleep test_sleep
#define SwitchToThread test_yield
#ifndef PPU_THREAD_IMPLEMENTATION
#define PPU_THREAD_IMPLEMENTATION "../runtime/syscalls/sys_ppu_thread.c"
#endif
#include PPU_THREAD_IMPLEMENTATION
#undef GetTickCount64
#undef Sleep
#undef SwitchToThread

uint8_t* vm_base;
uint32_t ppu_vm_size;
void ppu_resv_register(ppu_context* c) { (void)c; }
int ps3_intr_is_handler(unsigned long long tid) { (void)tid; return 0; }
int ps3_intr_wait(unsigned long long tid) { (void)tid; return 0; }

static ppu_thread_info target;
static ULONGLONG tick;
static unsigned steps, transition_at, transition, sleeps, yields, clock_reads;
static int fails;
#define EXPECT(c) do { if (!(c)) { ++fails; fprintf(stderr, "FAIL wait line %d: %s\n", __LINE__, #c); } } while (0)
static ULONGLONG test_tick(void) { ++clock_reads; return tick; }
static void advance(void)
{
    /* A broken cap must fail instead of hanging the test. */
    if (++steps > PRIO_START_TIMEOUT_MS + 1) {
        fprintf(stderr, "FAIL wait: loop exceeded the controlled timeout\n");
        exit(1);
    }
    ++tick;
    if (steps == transition_at) {
        if (transition == 1) ppu_thread_note_block(1);
        if (transition == 2) target.state = PPU_THREAD_STATE_FINISHED;
        if (transition == 3) target.state = PPU_THREAD_STATE_FREE;
    }
}
static void test_sleep(DWORD ms) { EXPECT(ms == 1); ++sleeps; advance(); }
static BOOL test_yield(void) { ++yields; advance(); return TRUE; }

static void check(const char* name, int state, int blocked, unsigned change,
                  unsigned at, unsigned expected)
{
    memset(&target, 0, sizeof(target));
    target.state = state;
    target.blocked = blocked;
    tick = 1000; steps = sleeps = yields = clock_reads = 0;
    transition = change; transition_at = at;
    /* note_block writes the real global slot, so use that slot for this case. */
    if (change == 1) {
        g_ppu_threads[0] = target;
        /* advance uses note_block, while the wait observes the same slot. */
        wait_first_block(&g_ppu_threads[0], 1);
    } else {
        wait_first_block(&target, 1);
    }
    EXPECT(tick - 1000 == expected);
    EXPECT(steps == expected);
    EXPECT(clock_reads == expected + ((change || blocked || state == PPU_THREAD_STATE_FINISHED || state == PPU_THREAD_STATE_FREE) ? 1 : 2));
    EXPECT(yields == (expected < PRIO_START_SPIN_MS ? expected : PRIO_START_SPIN_MS));
    EXPECT(sleeps == (expected > PRIO_START_SPIN_MS ? expected - PRIO_START_SPIN_MS : 0));
    printf("wait case %s: virtual_ms=%llu steps=%u\n", name,
           (unsigned long long)(tick - 1000), steps);
}
int main(void)
{
    check("already blocked", PPU_THREAD_STATE_RUNNING, 1, 0, 0, 0);
    check("already finished", PPU_THREAD_STATE_FINISHED, 0, 0, 0, 0);
    check("already free", PPU_THREAD_STATE_FREE, 0, 0, 0, 0);
    check("publish then block", PPU_THREAD_STATE_RUNNING, 0, 1, 3, 3);
    check("finish while waiting", PPU_THREAD_STATE_RUNNING, 0, 2, 3, 3);
    check("free while waiting", PPU_THREAD_STATE_RUNNING, 0, 3, 3, 3);
    check("detached is still live", PPU_THREAD_STATE_DETACHED, 0, 2, 7, 7);
    check("running reaches cap", PPU_THREAD_STATE_RUNNING, 0, 0, 0, PRIO_START_TIMEOUT_MS);
    check("detached reaches cap", PPU_THREAD_STATE_DETACHED, 0, 0, 0, PRIO_START_TIMEOUT_MS);
    check("completion at cap wins", PPU_THREAD_STATE_RUNNING, 0, 2, PRIO_START_TIMEOUT_MS, PRIO_START_TIMEOUT_MS);
    if (fails) return 1;
    puts("PASS: PPU wait state transitions and exact virtual 200ms cap");
    return 0;
}
