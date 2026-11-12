/* clang -std=gnu17 -I include -I libs/video
 *   libs/video/tests/test_gcm_last_flip_time.c -Wl,-dead_strip -pthread
 *   -o /tmp/test_gcm_last_flip_time && /tmp/test_gcm_last_flip_time
 *
 * cellGcmGetLastFlipTime is the guest system time (microseconds, the clock
 * sys_time_get_system_time returns) of the last flip that completed. Titles
 * pace their frames as period - (system time - last flip time), so the stamp
 * has to be on that clock and be taken when the flip is shown, not when it is
 * requested. The system clock is faked here so each stamp is exact.
 */
#include "../cellGcmSys.c"
#include "../rsx_tiling.c"
#include <assert.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
ps3_guest_caller_fn g_ps3_guest_caller;
/* cellGcmSetFlipCommand's GCM_FLIP_BT dump, unused here. */
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
void ppu_dump_bctrl_ring(uint32_t n, const char* why) { (void)n; (void)why; }
void ppu_dump_guest_stack(ppu_context* c, const char* why) { (void)c; (void)why; }
/* No graphics backend: a direct flip only names its buffer to the draw engine. */
int  rsx_draw_engine_enabled(void) { return 0; }
void rsx_draw_engine_note_flip(u32 buffer_id) { (void)buffer_id; }
static uint64_t now_us;
uint64_t ps3_system_time_us(void) { return now_us; }
/* What the drain does when it reaches a flip queued in the command stream. */
static void flip_executed(void) { s_fifo_flips_reached++; }
int main(void)
{
    s_display_buffer_set[1] = 1;

    /* A direct flip: the request does not stamp, the tick that shows it does. */
    now_us = 1000;
    assert(cellGcmSetFlipCommand(1) == CELL_OK);
    assert(cellGcmGetLastFlipTime() == 0);
    now_us = 17000;
    cellGcmTickFlip();
    assert(cellGcmGetLastFlipTime() == 17000);
    ppu_gcm_pump();
    assert(cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_DONE);

    /* A beat with no flip outstanding shows nothing new. */
    now_us = 33000;
    cellGcmTickFlip();
    cellGcm_request_tick();
    ppu_gcm_pump();
    assert(cellGcmGetLastFlipTime() == 17000);

    /* The vblank ticker's path stamps the same way. */
    now_us = 40000;
    assert(cellGcmSetFlipCommand(1) == CELL_OK);
    assert(cellGcmGetLastFlipTime() == 17000);
    now_us = 50000;
    cellGcm_request_tick();
    ppu_gcm_pump();
    assert(cellGcmGetLastFlipTime() == 50000);

    /* A flip in the command stream is shown on the first tick after the
     * drain reaches it, and stamped then. */
    cellGcmResetFlipStatus();
    now_us = 60000;
    cellGcmTickFlip();
    assert(cellGcmGetLastFlipTime() == 50000);
    flip_executed();
    now_us = 67000;
    cellGcmTickFlip();
    ppu_gcm_pump();
    assert(cellGcmGetLastFlipTime() == 67000);
    assert(cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_DONE);

    /* A frame limiter's sleep is then a part of its period, not all of it. */
    now_us = 72000;
    const int64_t period = 29366, sleep = period - (int64_t)(now_us - cellGcmGetLastFlipTime());
    assert(sleep > 0 && sleep < period);

    puts("GCM last flip time checks passed");
}
