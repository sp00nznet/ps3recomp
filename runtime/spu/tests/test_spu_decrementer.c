/*
 * The SPU decrementer counts down at the PS3 timebase from the host clock
 * (runtime/spu/spu_channels.c, SPU_WrDec / SPU_RdDec and spu_host_ns).
 *
 * SPU code times its waits with it, so this checks what such code relies on:
 * after a write it reads back the written value less the elapsed time, it
 * never counts up, it tracks a known sleep at 79.8 MHz, and it moves in steps
 * well under a microsecond (80 ticks) -- the host clock behind it is read at
 * nanosecond resolution on every platform this test runs on.
 *
 * spu_channels.c is compiled in rather than linked, as in
 * test_spu_taskset_wake.c, whose build line this test shares (with this file
 * in place of that one); tools/test_spu_decrementer.py runs it.
 *
 * Exit code: 0 if all passed, 1 if any failed.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint8_t g_guest_mem[1u << 16];
uint8_t* vm_base = g_guest_mem;
uint32_t ppu_vm_size = 0;

#include "../spu_channels.c"

static int g_pass, g_fail;
#define CHECK(cond) do {                                                    \
    if (cond) g_pass++;                                                     \
    else { g_fail++; fprintf(stderr, "FAIL at %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static spu_context g_spu;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint32_t rddec(void) { return spu_rdch(&g_spu, SPU_RdDec)._u32[0]; }
static void wrdec(uint32_t v) { spu_wrch(&g_spu, SPU_WrDec, spu_make_preferred_u32(v)); }

int main(void)
{
    spu_context_init(&g_spu, 0);

    /* Read back just after a write: the written value, less a little. */
    wrdec(0x80000000u);
    uint32_t d = 0x80000000u - rddec();
    CHECK(d < SPU_DECREMENTER_HZ / 100u);          /* under 10 ms */

    /* Never counts up; and its smallest step is far below 80 ticks. */
    uint32_t prev = rddec(), min_step = ~0u;
    for (int i = 0; i < 2000000 && min_step > 1u; i++) {
        uint32_t cur = rddec();
        if (cur > prev) { g_fail++; fprintf(stderr, "FAIL: counted up\n"); break; }
        if (cur != prev && prev - cur < min_step) min_step = prev - cur;
        prev = cur;
    }
    printf("smallest decrementer step: %u ticks\n", min_step);
    CHECK(min_step < 40u);

    /* Tracks a sleep: 50 ms is 3.99 M ticks. Allow for an oversleep. */
    wrdec(0xFFFFFFFFu);
    const uint32_t a = rddec();
    struct timespec ts = { 0, 50 * 1000000L };
    const uint64_t t0 = now_ms();
    nanosleep(&ts, NULL);
    const uint64_t slept_ms = now_ms() - t0;
    const uint32_t ticks = a - rddec();
    printf("50 ms sleep (%llu ms measured): %u ticks\n", (unsigned long long)slept_ms, ticks);
    CHECK(ticks >= (uint32_t)(SPU_DECREMENTER_HZ / 1000u * 49u));
    CHECK(ticks <= (uint32_t)(SPU_DECREMENTER_HZ / 1000u * (slept_ms + 2u)));

    printf("SPU decrementer: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * The runtime beyond the SPU channel layer, stubbed as in
 * test_spu_lockline_peer.c. None of it is on the path this test drives; the
 * clock only feeds the WAIT_SIGNAL log line.
 * -----------------------------------------------------------------------*/
void ppu_resv_break_store(uint64_t ea)                 { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w)
{
    (void)a; (void)v; (void)w;
}
unsigned long long ps3_ms_now(void)                    { return now_ms(); }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int      g_resv_store_active = 0;
uint32_t g_barrier_sync_watch = 0;

void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0;
uint32_t g_ydkj_real_spurs_ea = 0;
uint32_t g_ydkj_real_taskset_ea = 0;
uint32_t g_ydkj_real_taskid = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void)                   { }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d)
{
    (void)c; (void)a; (void)b; (void)d;
}

uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }
