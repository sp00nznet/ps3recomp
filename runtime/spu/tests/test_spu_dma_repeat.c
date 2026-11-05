/* The identical-transfer watchdog in mfc_submit, driven through the real
 * engine with a synthetic GET and a test-owned millisecond clock.
 *
 *   ./fixture         back-off schedule: yield, then sleep, then halt
 *   ./fixture log     one repeat episode for SPU_DMA_REPEAT_LOG to report
 *
 * tools/test_spu_dma_repeat.py runs both and checks the stderr lines. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static uint8_t guest[1u << 20];
uint8_t* vm_base = guest;
uint32_t ppu_vm_size;
#include "../spu_channels.c"

static unsigned long long s_fake_ms = 1000;   /* the watchdog's clock */
unsigned long long ps3_ms_now(void) { return s_fake_ms; }

static spu_context spu;

static double host_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}
static void get(uint32_t ea, uint32_t lsa, uint32_t size)
{
    spu.mfc_eah = 0; spu.mfc_eal = ea; spu.mfc_lsa = lsa;
    spu.mfc_size = size; spu.mfc_tag = 3;
    assert(mfc_submit(mfc_for(&spu), &spu, 0x40) == 0);   /* MFC_GET */
}
/* Host time for n further repeats of the current transfer. */
static double repeats_ms(unsigned n)
{
    const double t0 = host_ms();
    for (unsigned i = 0; i < n; i++) get(0x20000, 0x1000, 16);
    return host_ms() - t0;
}

int main(int argc, char** argv)
{
    spu_context_init(&spu, 0);
    spu.image_id = 5;
    memset(guest + 0x20000, 0x5A, 0x200);

    if (argc > 1 && !strcmp(argv[1], "log")) {
        /* 300 repeats of a 16-byte GET (past the default limit of 256), then a
         * 128-byte GET elsewhere ends the episode. The report must name the
         * repeated transfer, including its size, not the one that ended it. */
        for (int i = 0; i < 300; i++) get(0x20000, 0x1000, 16);
        get(0x20100, 0x2000, 128);
        assert(!memcmp(spu.ls + 0x1000, guest + 0x20000, 16));
        spu_mfc_release(&spu);
        puts("PASS: one repeat episode issued");
        return 0;
    }

    /* Below the limit: plain transfers, no back-off at all. */
    assert(repeats_ms(255) < 250.0);

    /* Inside the first SPU_DMA_REPEAT_BACKOFF_MS (default 2) of the episode
     * the watchdog yields. With the clock held at the episode start, 1000
     * repeats past the limit take nowhere near the >= 1000 ms that a 1 ms
     * sleep per repeat costs. */
    const double yield_ms = repeats_ms(1000);
    printf("1000 repeats inside the yield window: %.1f ms\n", yield_ms);
    assert(yield_ms < 250.0);

    /* Past the window it backs off 1 ms per repeat, as before. */
    s_fake_ms += 2;
    const double sleep_ms = repeats_ms(20);
    printf("20 repeats past the yield window: %.1f ms\n", sleep_ms);
    assert(sleep_ms >= 15.0);

    /* And after SPU_DMA_REPEAT_SECS (default 30) it halts and re-arms: the
     * next 255 repeats are plain transfers again. */
    s_fake_ms += 30u * 1000u;
    get(0x20000, 0x1000, 16);                 /* halts (reported on stderr) */
    assert(repeats_ms(255) < 250.0);

    /* A different transfer ends the episode; the new one starts a fresh count. */
    get(0x20100, 0x2000, 128);
    assert(repeats_ms(255) < 250.0);

    spu_mfc_release(&spu);
    puts("PASS: identical-transfer watchdog yields inside the back-off window, sleeps after it, halts after the time limit");
    return 0;
}

void ppu_resv_break_store(uint64_t ea)                 { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w)
{
    (void)a; (void)v; (void)w;
}
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

/* Loader diagnostics and worker publication are outside this fixture. */
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }
