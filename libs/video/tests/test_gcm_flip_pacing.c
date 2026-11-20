/* Flip completion pacing (cellGcmSys.c, PS3RECOMP_GCM_FLIP_PACING).
 *
 * fixed (the variable unset): an executed flip completes only on the beat
 * (cellGcmTickFlip / cellGcm_request_tick); a status poll or a wait
 * completes nothing.
 * adaptive: an executed flip completes at max(execution, last completion +
 * one period), on whichever poll comes first, and never faster than one per
 * period; the beat is then only a poll. A flip the drain has not executed
 * never completes, and the vblank count and vblank pending bit stay on the
 * ticker in both modes.
 *
 * Built and run once per mode by tools/test_gcm_flip_pacing.py. The clock is
 * substituted, so nothing here depends on real time.
 */
#include "../cellGcmSys.c"
#include "../rsx_tiling.c"
#include <stdio.h>
#include <stdlib.h>
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

static int fails;
static void check(int ok, const char* what)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}

static u64 g_now;
static u64 fake_clock(void) { return g_now; }
#define MS 1000000ull

/* What the drain does when it reaches a flip queued in the command stream. */
static void flip_executed(void) { s_fifo_flips_reached++; }

static unsigned flips;
static void callback(uint32_t opd, uint64_t a, uint64_t b, uint64_t c,
                     uint64_t d, uint64_t e, uint64_t f, uint64_t h, uint64_t i)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)h; (void)i;
    if (opd == 0x400) flips++;
}

/* Take the pending bits for a check and hand them back to the pump. */
static long take_and_deliver(void)
{
    const long p = GCM_PENDING_TAKE();
    GCM_PENDING_SET((int)p);
    ppu_gcm_pump();
    return p;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const int adaptive = argc > 1 && !strcmp(argv[1], "adaptive");
    if (adaptive) setenv("PS3RECOMP_GCM_FLIP_PACING", "adaptive", 1);
    else unsetenv("PS3RECOMP_GCM_FLIP_PACING");
    s_flip_clock = fake_clock;
    g_ps3_guest_caller = callback;
    s_flip_handler_opd = 0x400;
    (void)GCM_PENDING_TAKE();
    long p;

    /* 1: the first flip, executed at t = 0. */
    g_now = 0;
    cellGcmResetFlipStatus();
    flip_executed();
    (void)cellGcmGetFlipStatus();
    p = take_and_deliver();
    if (adaptive) check(p == 2, "adaptive: the first executed flip completes on the next poll");
    else {
        check(p == 0, "fixed: a status poll completes nothing");
        cellGcmTickFlip();
        p = take_and_deliver();
        check(p == 2, "fixed: the beat completes the executed flip");
    }
    check(flips == 1 && cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_DONE,
          "the handler runs once and the status is DONE");

    /* 2: the next flip executes 5 ms later, within the period. */
    g_now = 5 * MS;
    cellGcmResetFlipStatus();
    flip_executed();
    cellGcmTickFlip();
    (void)cellGcmGetFlipStatus();
    if (adaptive) {
        p = GCM_PENDING_TAKE();
        check(!(p & 2), "adaptive: no completion before one period has passed");
        g_now = 16 * MS;
        (void)cellGcmGetFlipStatus();
        check(!(GCM_PENDING_TAKE() & 2), "adaptive: still none at 16.0 ms");
        g_now = GCM_FLIP_PERIOD_NS;
        (void)cellGcmGetFlipStatus();
    }
    p = take_and_deliver();
    if (adaptive) check(p == 2, "adaptive: a status poll completes it at exactly one period");
    else check(p == 2, "fixed: the beat completes it whenever the beat comes");
    check(flips == 2 && cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_DONE, "and it is delivered DONE");

    /* 3: a late frame: executed 40 ms after the last completion. Adaptive
     * completes it at once; fixed waits for the beat. */
    g_now += 40 * MS;
    cellGcmResetFlipStatus();
    flip_executed();
    cellGcmSetWaitFlip();
    p = take_and_deliver();
    if (adaptive) check(p == 2, "adaptive: a late flip completes while the guest waits for it");
    else {
        check(p == 0, "fixed: a late flip still waits for the beat");
        cellGcmTickFlip();
        p = take_and_deliver();
        check(p == 2, "fixed: which completes it");
    }
    check(flips == 3, "its handler runs");

    /* 4: a flip the drain has not executed never completes, however long. */
    g_now += 100 * MS;
    cellGcmResetFlipStatus();
    cellGcmTickFlip();
    cellGcm_request_tick();
    (void)cellGcmGetFlipStatus();
    check(!(GCM_PENDING_TAKE() & 2), "an unexecuted flip does not complete");
    check(cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_WAITING, "and stays WAITING");

    /* 5: the vblank beat is untouched: count and pending bit every tick. */
    const u32 v0 = s_vblank_count;
    cellGcmTickVBlank();
    cellGcm_request_tick();
    p = GCM_PENDING_TAKE();
    check(s_vblank_count == v0 + 2 && (p & 1), "vblanks advance on the ticker in both modes");

    printf("[gcm_flip_pacing %s] %s (%d failed)\n", adaptive ? "adaptive" : "fixed",
           fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
