/* A deterministic overlapping execution of the benchmark's real pm_run.
 * The next worker may run before the current worker's kick returns. This
 * models that ordering directly; it does not replace a scheduler test. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef struct { uint32_t pc; } spu_context;
#define SPURS_PM_EXIT_TO_KERNEL_LS 0x100
static uint32_t g_req[2], g_hops, g_target = 2, g_ppu_kick, g_wid[2] = {0, 1};
static uint64_t g_start_ns[1 << 16], g_kick_ns[1 << 16], ticks;
static unsigned ran, kick_depth;
static uint64_t now_ns(void) { return ++ticks; }
static void pm_run(spu_context*, uint32_t);
static void spu_indirect_branch(spu_context* c) {
    assert(c->pc == SPURS_PM_EXIT_TO_KERNEL_LS);
    ++ran;
}
static void spu_kick(spu_context* c, uint32_t wid) {
    (void)c;
    if (!kick_depth++) {
        spu_context next = {0};
        pm_run(&next, wid);
    }
    --kick_depth;
}
#include "spurs_hop_under_test.inc"
int main(void) {
    spu_context first = {0};
    g_req[0] = 1;
    pm_run(&first, 0);
    if (ran != 2 || g_hops != 2 || g_req[0] || g_req[1] ||
        !g_start_ns[0] || !g_start_ns[1]) {
        fprintf(stderr, "FAIL: two workers ran=%u, counted=%u, pending=%u/%u\n",
                ran, g_hops, g_req[0], g_req[1]);
        return 1;
    }
    puts("PASS: overlapping benchmark hops retain both completions");
    return 0;
}
