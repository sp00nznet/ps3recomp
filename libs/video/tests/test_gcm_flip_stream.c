/* A flip queued in the command stream (cellGcmSys.c).
 *
 * _cellGcmSetFlipCommandWithWaitLabel(ctx, buffer, label, value): libgcm puts
 * NV406E SEMAPHORE_OFFSET and SEMAPHORE_ACQUIRE for the label into the context,
 * then the flip. The import used to write the label itself from the PPU and flip
 * at call time.
 *
 * And the flip completes when the drain reaches it: the 60 Hz tick used to mark
 * it done on the next beat wherever the walker was, so a title pacing itself on
 * the flip status rebuilt command segments the walker had not executed yet.
 *
 *   1. The context holds OFFSET, ACQUIRE, then the flip marker, current has
 *      moved past them, and the label in memory is untouched.
 *   2. A tick before the drain is a vblank but no flip.
 *   3. The drain executes up to the marker; the next tick completes the flip.
 *   4. A flip written but not flushed does not complete; an executed one does,
 *      even with a newer flip written behind it and not yet flushed.
 *   5. A flip the walker abandons when it resyncs out of unmapped IO does not
 *      hold back the flip status.
 *
 * gcc -std=gnu17 -I include -I libs/video libs/video/tests/test_gcm_flip_stream.c
 *   -ffunction-sections -fdata-sections -Wl,--gc-sections -pthread
 *   -o /tmp/test_gcm_flip_stream        (clang on macOS: -Wl,-dead_strip)
 */
#include "../cellGcmSys.c"
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
/* The rest of the walker's world: no graphics backend, no lifted program. */
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
u32 g_rsx_last_reference;
void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
void rsx_raise_user_cmd(uint32_t arg) { (void)arg; }
void ppu_dump_bctrl_ring(uint32_t n, const char* why) { (void)n; (void)why; }
void ppu_dump_guest_stack(ppu_context* c, const char* why) { (void)c; (void)why; }
void rsx_state_init(rsx_state* state) { (void)state; }
int  rsx_process_method(rsx_state* state, u32 method, u32 data) { (void)state; (void)method; (void)data; return 0; }
int  rsx_draw_engine_enabled(void) { return 0; }
void rsx_draw_engine_method(u32 method, u32 arg) { (void)method; (void)arg; }
void rsx_draw_engine_note_flip(u32 buffer_id) { (void)buffer_id; }
void rsx_draw_engine_guest_read(u32 l, u32 o, u32 p, u32 w, u32 h) { (void)l; (void)o; (void)p; (void)w; (void)h; }
void rsx_draw_engine_guest_wrote(u32 l, u32 o, u32 p, u32 w, u32 h) { (void)l; (void)o; (void)p; (void)w; (void)h; }
int  rsx_live_draw_enabled(void) { return 0; }
void rsx_live_draw_method(u32 method, u32 arg) { (void)method; (void)arg; }
int  rsx_live_draw_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 w, u32 h)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)w; (void)h; return 0; }
void rsx_live_draw_note_resolve(u32 a, u32 b, u32 c, u32 d) { (void)a; (void)b; (void)c; (void)d; }
int  rsx_live_draw_resolve_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 g, u32 h,
                                u32 i, u32 j, u32 k, u32 l)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; (void)j; (void)k; (void)l; return 0; }

#define IO_ADDR   0x00100000u
#define IO_SIZE   0x00400000u
#define CMD_SIZE  0x00080000u
#define TITLE_CB  0x012D6A78u      /* a title function: its own buffer-full callback */


#define IO_ADDR   0x00100000u
#define IO_SIZE   0x00400000u
#define CMD_SIZE  0x00080000u
#define TITLE_CB  0x012D6A78u      /* a title function: its own buffer-full callback */
#define LABEL     0x42u
#define VALUE     7u

static uint32_t g_heap = 0x00800000u;
static u32 host_alloc(u32 size, u32 align)
{
    if (!align) align = 4;
    g_heap = (g_heap + align - 1u) & ~(align - 1u);
    u32 r = g_heap;
    g_heap += size;
    return r;
}
static void host_w32(u32 ea, u32 v) { vm_write32(ea, v); }

static int fails;
static void check(int ok, const char* what)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const uint32_t vm_size = ppu_hle_inject_base + 0x10000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { puts("FAIL: guest VM alloc"); return 1; }
    ppu_vm_size = vm_size;

    u32 ctx_out = host_alloc(4, 4);
    (void)cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    s_display_buffer_set[1] = 1;

    /* A title context inside the IO window, with its own callback (so the
     * flip never wraps it), and the walker idle at its start. */
    const u32 ctx = host_alloc(0x18, 16), buf = IO_ADDR + 0x200000u;
    vm_write32(ctx + 0x0, buf);
    vm_write32(ctx + 0x4, buf + 0x30000u);
    vm_write32(ctx + 0x8, buf);
    vm_write32(ctx + 0xC, ctx + 0x10);
    vm_write32(ctx + 0x10, TITLE_CB);
    vm_write32(ctx + 0x14, 0);
    const u32 io = gcm_ea2io(buf);
    s_fifo_getoff = io;
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, io);    /* put == get: nothing queued */
    const u32 label_ea = GCM_LABEL_GUEST_BASE + LABEL * GCM_LABEL_STRIDE;
    vm_write32(label_ea, 0);
    (void)GCM_PENDING_TAKE();

    /* 1 */
    const s32 rc = _cellGcmSetFlipCommandWithWaitLabel((void*)(uintptr_t)ctx, 1, LABEL, VALUE);
    check(rc == CELL_OK, "WithWaitLabel returns CELL_OK");
    check(vm_read32(buf + 0) == 0x00040064u && vm_read32(buf + 4) == LABEL * GCM_LABEL_STRIDE,
          "SEMAPHORE_OFFSET for the label is in the context");
    check(vm_read32(buf + 8) == 0x00040068u && vm_read32(buf + 12) == VALUE,
          "SEMAPHORE_ACQUIRE for the value follows it");
    check(vm_read32(buf + 16) == (GCM_FLIP_MARKER | 1u), "then the flip");
    check(vm_read32(ctx + 8) == buf + 20, "current is past the flip");
    check(vm_read32(label_ea) == 0, "the label itself is not written by the PPU");

    /* 2 */
    cellGcm_request_tick();
    long p = GCM_PENDING_TAKE();
    check((p & 1) && !(p & 2), "a tick before the drain is a vblank, not a flip");

    /* 3: the title flushes; the walker runs to put (the acquire does not block
     * unless GCM_SEMA_ACQUIRE is set) and stops at the flip. */
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, io + 20);
    cellGcm_rsx_process_fifo();
    check(s_fifo_getoff == io + 20, "the drain consumed the wait and the flip");
    cellGcm_request_tick();
    p = GCM_PENDING_TAKE();
    check((p & 1) && (p & 2), "the next tick completes the flip");

    /* 4: a second flip is written but not flushed (put stays). The first one
     * has already completed, so the tick has nothing new to report... */
    const u32 cur2 = vm_read32(ctx + 8);
    (void)_cellGcmSetFlipCommandWithWaitLabel((void*)(uintptr_t)ctx, 1, LABEL, VALUE);
    cellGcm_request_tick();
    p = GCM_PENDING_TAKE();
    check((p & 1) && !(p & 2), "a written, unflushed flip does not complete");
    /* ...and once it is flushed and executed, it completes even though a third
     * one has been written behind it and not flushed: the RSX reports the flip
     * it did, not the ones still ahead of it. */
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, gcm_ea2io(cur2) + 20);
    (void)_cellGcmSetFlipCommandWithWaitLabel((void*)(uintptr_t)ctx, 1, LABEL, VALUE);
    cellGcm_rsx_process_fifo();
    cellGcm_request_tick();
    p = GCM_PENDING_TAKE();
    check((p & 1) && (p & 2), "an executed flip completes with a newer one still unflushed");

    /* 5: the walker is lost in unmapped IO and resyncs to put, abandoning the
     * flip it had not reached. That flip will never be executed, so the next
     * flip status must not wait for it. */
    const u32 cur3 = vm_read32(ctx + 8);
    (void)_cellGcmSetFlipCommandWithWaitLabel((void*)(uintptr_t)ctx, 1, LABEL, VALUE);
    cellGcm_request_tick();
    p = GCM_PENDING_TAKE();
    check(!(p & 2), "a written, unflushed flip does not complete (again)");
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, gcm_ea2io(cur3) + 20);
    s_fifo_getoff = 0x0FD00000u;              /* no mapping there */
    cellGcm_rsx_process_fifo();
    check(s_fifo_getoff == gcm_ea2io(cur3) + 20, "the walker resynced to put");
    cellGcm_request_tick();
    p = GCM_PENDING_TAKE();
    check((p & 1) && (p & 2), "a flip abandoned by a resync does not hold back the status");

    printf("[gcm_flip_stream] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}
