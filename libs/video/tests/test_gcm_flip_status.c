/* clang -std=gnu17 -I include -I libs/video
 *   libs/video/tests/test_gcm_flip_status.c -Wl,-dead_strip -pthread
 *   -o /tmp/test_gcm_flip_status && /tmp/test_gcm_flip_status
 *
 * The flip status against a flip completion that is delivered late: the
 * ticker marks a completion pending, but the flip handler only runs when a
 * guest thread next pumps, and the title may have requested another flip by
 * then. That newer request must stay WAITING until a tick of its own
 * completes it, and so must one that outlives cellGcmSetWaitFlip's bound.
 */
#include "../cellGcmSys.c"
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
static unsigned flips;
static void callback(uint32_t opd, uint64_t a, uint64_t b, uint64_t c,
                     uint64_t d, uint64_t e, uint64_t f, uint64_t h, uint64_t i)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)h; (void)i;
    if (opd == 0x400) flips++;
}
int main(void)
{
    g_ps3_guest_caller = callback;
    s_flip_handler_opd = 0x400;

    /* The plain case: request, tick, deliver. */
    cellGcmResetFlipStatus();
    assert(cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_WAITING);
    cellGcmTickFlip();
    ppu_gcm_pump();
    assert(flips == 1 && cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_DONE);
    /* No flip outstanding: a tick completes nothing and calls no handler. */
    cellGcmTickFlip();
    ppu_gcm_pump();
    assert(flips == 1);

    /* A newer request between the tick and the delivery: the late completion
     * runs the handler for the old flip but leaves the new one WAITING. */
    cellGcmResetFlipStatus();
    cellGcmTickFlip();
    cellGcmResetFlipStatus();
    ppu_gcm_pump();
    assert(flips == 2 && cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_WAITING);
    cellGcmTickFlip();
    ppu_gcm_pump();
    assert(flips == 3 && cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_DONE);

    /* cellGcmSetWaitFlip gives up after its bound, but the flip it waited
     * for is still outstanding and its handler still has to run. */
    cellGcmResetFlipStatus();
    cellGcmSetWaitFlip();
    assert(cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_WAITING);
    cellGcmTickFlip();
    ppu_gcm_pump();
    assert(flips == 4 && cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_DONE);

    puts("GCM flip status checks passed");
}
