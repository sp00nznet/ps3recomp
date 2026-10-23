/* A park the walker has left is forgotten (cellGcmSys.c).
 *
 * A self-JUMP park the title has not patched yet is waited on for
 * GCM_PARK_GRACE_MS with the same put; past that the walker resyncs to put.
 * The park's start time used to be kept after the walker left the park, so
 * when the walker next stopped at the same address with the same put -- a
 * title that reuses its command blocks does that -- the new park inherited a
 * grace that had run out long before, and the resync fired at once, dropping
 * the commands the title was about to link in.
 *
 * Block A (io 0) runs a probe method and parks at io 8; put is the end of
 * block B (io 0x8000), one more probe. The park is patched (a JUMP to B) and
 * the walker runs B. Well past the grace the walker comes back to A, with
 * the same put, and finds the park standing again: it must wait on it, not
 * resync. Left unpatched past the grace, it still resyncs.
 *
 * gcc -std=gnu17 -I include -I libs/video
 *   libs/video/tests/test_gcm_park_reuse.c -ffunction-sections
 *   -fdata-sections -Wl,--gc-sections -pthread -o /tmp/test_gcm_park_reuse
 *   (clang on macOS: -Wl,-dead_strip)
 */
#include "../cellGcmSys.c"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
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
/* The walker's other consumers: no graphics backend, no lifted program. */
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
u32 g_rsx_last_reference;
void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
void ppu_dump_bctrl_ring(uint32_t n, const char* why) { (void)n; (void)why; }
void ppu_dump_guest_stack(ppu_context* c, const char* why) { (void)c; (void)why; }
void rsx_state_init(rsx_state* state) { (void)state; }
/* Counts the probe methods the walker executed. */
#define PROBE_METHOD 0x1EE0u
static u32 seen_n;
int  rsx_process_method(rsx_state* state, u32 method, u32 data)
{
    (void)state; (void)data;
    if (method == PROBE_METHOD) seen_n++;
    return 0;
}
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

void rsx_raise_user_cmd(uint32_t arg) { (void)arg; }

#define IO_ADDR   0x00100000u
#define IO_SIZE   0x00100000u
#define CMD_SIZE  0x00010000u
#define SEG_B     0x00008000u

static uint32_t g_heap = 0x00400000u;
static u32 host_alloc(u32 size, u32 align)
{
    if (!align) align = 4;
    g_heap = (g_heap + align - 1u) & ~(align - 1u);
    u32 r = g_heap;
    g_heap += size;
    return r;
}
static void gw32(uint32_t ea, uint32_t v)
{
    uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void host_w32(u32 ea, u32 v) { gw32(ea, v); }

static int fails;
static void check(int ok, const char* what)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}

/* One method, count 1, subchannel 0. */
static u32 put_probe(u32 io, u32 arg)
{
    gw32(IO_ADDR + io, (1u << 18) | PROBE_METHOD);
    gw32(IO_ADDR + io + 4, arg);
    return io + 8;
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
    const uint32_t ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();

    /* A: probe, then a park at io 8.  B: probe; put after it. */
    const u32 park = put_probe(0, 0);
    gw32(IO_ADDR + park, 0x20000000u | park);
    const u32 put = put_probe(SEG_B, 1);

    s_fifo_getoff = 0;
    vm_write32(ctrl + 4, 0);
    vm_write32(ctrl + 0, put);
    cellGcm_rsx_process_fifo();
    check(seen_n == 1 && s_fifo_getoff == park, "the walker parks at the end of A");

    /* The title links B in: the park becomes a JUMP to B. */
    gw32(IO_ADDR + park, 0x20000000u | SEG_B);
    cellGcm_rsx_process_fifo();
    check(seen_n == 2 && s_fifo_getoff == put, "the patched park is followed to put");

    /* Well past the grace, A is reused: the park stands again, and the walker
     * comes back to it with the same put. */
    usleep((useconds_t)(GCM_PARK_GRACE_MS * 3u * 1000u));
    gw32(IO_ADDR + park, 0x20000000u | park);
    s_fifo_getoff = 0;
    cellGcm_rsx_process_fifo();
    check(seen_n == 3 && s_fifo_getoff == park,
          "a park met again with the same put waits (no resync from the old park's time)");
    cellGcm_rsx_process_fifo();
    check(s_fifo_getoff == park, "...and keeps waiting within the grace");

    /* Left unpatched past the grace, the walker still gives up on it. */
    usleep((useconds_t)(GCM_PARK_GRACE_MS * 3u * 1000u));
    cellGcm_rsx_process_fifo();
    check(s_fifo_getoff == put, "an unpatched park past the grace resyncs to put");

    printf("[gcm_park_reuse] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}
