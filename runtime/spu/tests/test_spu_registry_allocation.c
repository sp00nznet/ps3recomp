#include <stdlib.h>
#include <string.h>
#include <stdint.h>
static int fail_allocation;
static void* registry_malloc(size_t n) { return fail_allocation ? NULL : malloc(n); }
#define malloc registry_malloc
#include "../spu_channels.c"
#include "../spu_coherency.c"
#undef malloc
uint8_t* vm_base;
uint32_t ppu_vm_size;
int main(int argc, char** argv)
{
    spu_context* ctx = calloc(1, sizeof(*ctx));
    if (!ctx || argc != 2) return 2;
    fail_allocation = 1;
    if (!strcmp(argv[1], "mfc")) (void)mfc_for(ctx);
    else {
        spu_lockline_lock();
        spu_coh_reserve(ctx, 0x10000);
        spu_lockline_unlock();
    }
    return 0; /* surviving allocation failure would fabricate working state */
}
void ppu_resv_break_store(uint64_t ea)                 { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w)
{
    (void)a; (void)v; (void)w;
}
unsigned long long ps3_ms_now(void)                    { return 0; }
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
