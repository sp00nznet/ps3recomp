/*
 * test_avconf_gamma - cellVideoOutSetGamma through its context handler.
 *
 * The gamma is a float, so the guest passes it in f1. The handler registered
 * for the NID must read it from there, keep it within 0.8..1.2, and hand it
 * to the presenter (cellAvconfExt_display_gamma), which PS3_DISPLAY_GAMMA
 * overrides.
 *
 *   cc -std=gnu17 -I include -o test_avconf_gamma libs/misc/tests/test_avconf_gamma.c
 */

#include "../cellAvconfExt.c"

#include <assert.h>

/* ppu_memory.h hooks the runtime supplies; inert here. */
uint8_t* vm_base;
int g_resv_store_active = 0;
uint32_t g_ww_lo = 0, g_ww_hi = 0;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
int  spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t addr) { (void)addr; }
void ps3_ww_report_inline(uint32_t addr, uint64_t val, int width) { (void)addr; (void)val; (void)width; }

static uint32_t s_nid;
static void (*s_fn)(ppu_context*);
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*))
{
    (void)name;
    s_nid = nid;
    s_fn = fn;
}

static int32_t set_gamma(double g)
{
    static ppu_context ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = 0;          /* CELL_VIDEO_OUT_PRIMARY */
    ctx.gpr[4] = 0xDEAD;     /* what a plain C prototype would have read */
    ctx.fpr[1] = g;
    s_fn(&ctx);
    return (int32_t)ctx.gpr[3];
}

int main(void)
{
    avconf_register_ctx();
    assert(s_nid == 0xC7020F62u && s_fn);   /* cellVideoOutSetGamma */

    assert(cellAvconfExt_display_gamma() == 1.0f);
    assert(set_gamma(1.1) == 0);
    assert(cellAvconfExt_display_gamma() == 1.1f);
    float back = 0.0f;
    /* GetGamma writes through a guest pointer: give it a guest arena. */
    static uint8_t arena[64];
    vm_base = arena;
    assert(cellVideoOutGetGamma(0, (float*)(uintptr_t)16) == 0);
    { uint32_t bits = (uint32_t)arena[16] << 24 | (uint32_t)arena[17] << 16 |
                      (uint32_t)arena[18] << 8 | arena[19];
      memcpy(&back, &bits, 4); }
    assert(back == 1.1f);

    /* Out of range: refused, and the previous value stays. */
    assert(set_gamma(1.5) == (int32_t)0x8002b223);
    assert(set_gamma(0.5) == (int32_t)0x8002b223);
    assert(cellAvconfExt_display_gamma() == 1.1f);
    assert(set_gamma(0.8) == 0 && cellAvconfExt_display_gamma() == 0.8f);
    puts("test_avconf_gamma: OK");
    return 0;
}
