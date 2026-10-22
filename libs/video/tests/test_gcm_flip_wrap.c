/* The flip HLE's in-FIFO flip (gcm_flip_into_fifo, used by
 * _cellGcmSetFlipCommand) wraps a nearly used command buffer back to its
 * begin only when the runtime owns that buffer.
 *
 * Two contexts, both "nearly full" (less than GCM_FLIP_WRAP_BYTES left) with
 * the walker far past their begin:
 *   - the runtime's own ring, made by cellGcmSetupContext (its buffer-full
 *     callback is the runtime's): the flip marker is written, then a JUMP to
 *     begin, and current goes back to begin;
 *   - a title's context with its own buffer-full callback: the flip marker is
 *     written and current advances past it; no JUMP, no rewind.
 *
 * gcc -std=gnu17 -I include -I libs/video libs/video/tests/test_gcm_flip_wrap.c
 *   -ffunction-sections -fdata-sections -Wl,--gc-sections -pthread
 *   -o /tmp/test_gcm_flip_wrap        (clang on macOS: -Wl,-dead_strip)
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
rsx_backend* rsx_get_backend(void) { return NULL; }
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
/* Put ctx's current 0x10000 bytes before its end, the walker 0x28000 bytes
 * past its begin, flip, and report what happened to the buffer. */
static void flip_on(const char* what, u32 ctx, int want_wrap)
{
    const u32 begin = vm_read32(ctx), end = vm_read32(ctx + 4), cur = end - 0x10000u;
    vm_write32(ctx + 8, cur);
    vm_write32(cur, 0); vm_write32(cur + 4, 0);
    s_fifo_getoff = gcm_ea2io(begin) + 0x28000u;
    int r = gcm_flip_into_fifo(ctx, 1);
    const u32 marker = vm_read32(cur), next = vm_read32(cur + 4), now = vm_read32(ctx + 8);
    const int wrapped = next == (0x20000000u | gcm_ea2io(begin)) && now == begin;
    const int advanced = next == 0 && now == cur + 4;
    const int ok = r == 1 && marker == (GCM_FLIP_MARKER | 1u) && (want_wrap ? wrapped : advanced);
    printf("[%s] %s: marker 0x%08X, next word 0x%08X, current %s\n", ok ? "PASS" : "FAIL", what,
           marker, next, wrapped ? "rewound to begin (JUMP written)" : advanced ? "advanced past the marker" : "?");
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
    u32 rt = cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    s_display_buffer_set[1] = 1;

    /* The title's draw context: 0x30000 bytes inside the IO window, with its
     * own callback descriptor. */
    const u32 tctx = host_alloc(0x18, 16), tbuf = IO_ADDR + 0x200000u;
    vm_write32(tctx + 0x0, tbuf);
    vm_write32(tctx + 0x4, tbuf + 0x30000u);
    vm_write32(tctx + 0x8, tbuf);
    vm_write32(tctx + 0xC, tctx + 0x10);
    vm_write32(tctx + 0x10, TITLE_CB);
    vm_write32(tctx + 0x14, 0);

    flip_on("runtime ring (cellGcmSetupContext)", rt, 1);
    flip_on("title context (own callback)", tctx, 0);
    printf("[gcm_flip_wrap] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}
