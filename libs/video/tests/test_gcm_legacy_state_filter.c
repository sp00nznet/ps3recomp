/* The FIFO walker hands rsx_state every subchannel-0 method only while
 * something reads that state: the vtable backend, or the walker without the
 * draw engine. With the draw engine up and no vtable, only the methods
 * rsx_process_method acts on beyond its state reach it (cellGcmSys.c,
 * gcm_legacy_method_has_effect). Subchannel 1 (not 2D by default) reaches
 * rsx_state the same way and is filtered the same way.
 *
 * cc -std=gnu17 -I include -I libs/video
 *   libs/video/tests/test_gcm_legacy_state_filter.c
 *   -ffunction-sections -fdata-sections -Wl,-dead_strip -o /tmp/test_gcm_legacy_state_filter
 *   (gcc: -Wl,--gc-sections -pthread -lm)
 */
#include "../cellGcmSys.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
/* What the walker hands rsx_state, and the switches that decide it. */
static u32 legacy_seen[64], legacy_n;
static int engine_on;
static rsx_backend* vtable;
int  rsx_process_method(rsx_state* state, u32 method, u32 data)
{ (void)state; (void)data; if (legacy_n < 64) legacy_seen[legacy_n++] = method; return 0; }
int  rsx_draw_engine_enabled(void) { return engine_on; }
rsx_backend* rsx_get_backend(void) { return vtable; }
/* The walker hands vertex-program runs here (#554). */
int  rsx_process_vp_run(rsx_state* state, u32 method, const u8* src, u32 count)
{ (void)state; (void)method; (void)src; (void)count; return 0; }
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

/* (subchannel << 13) | method. The label write (0x1D70) lands in the label
 * window at offset 0, which the fixture's guest memory covers. */
static const u32 methods[] = { 0x0050u, 0x0A74u, 0x1EFCu, 0x1F00u, 0x0180u, 0x01A4u, 0x1D6Cu,
                               0x0208u, 0x1D70u, 0x2000u | 0x0104u, 0x2000u | 0x0300u, 0x1D74u };
enum { N_METHODS = sizeof methods / sizeof methods[0] };
/* What rsx_process_method is handed: the method without its subchannel. */
static u32 bare[N_METHODS];

/* One single-word command per method, walked in one pass. */
static void walk(u32 ctrl)
{
    u32 io = 0;
    for (u32 i = 0; i < N_METHODS; i++) {
        bare[i] = methods[i] & 0x1FFCu;
        gw32(IO_ADDR + io, (1u << 18) | methods[i]);
        gw32(IO_ADDR + io + 4, 0x10u + i);
        io += 8;
    }
    legacy_n = 0;
    s_fifo_getoff = 0;
    vm_write32(ctrl + 4, 0);
    vm_write32(ctrl + 0, io);
    cellGcm_rsx_process_fifo();
}
static int seen_exactly(const u32* want, u32 n)
{
    if (legacy_n != n) return 0;
    for (u32 i = 0; i < n; i++) if (legacy_seen[i] != want[i]) return 0;
    return 1;
}

int main(int argc, char** argv)
{
    /* "trace": run under RSX_TRACE, which must keep the full stream. */
    const int trace = argc > 1 && strcmp(argv[1], "trace") == 0;
    if (trace) setenv("RSX_TRACE", "1", 1); else unsetenv("RSX_TRACE");
    setvbuf(stdout, NULL, _IONBF, 0);
    const uint32_t vm_size = ppu_hle_inject_base + 0x10000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { puts("FAIL: guest VM alloc"); return 1; }
    ppu_vm_size = vm_size;
    u32 ctx_out = host_alloc(4, 4);
    (void)cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    const uint32_t ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();

    engine_on = 0; vtable = NULL; walk(ctrl);
    check(seen_exactly(bare, N_METHODS), "without the draw engine every method reaches rsx_state");

    static rsx_backend dummy;
    engine_on = 1; vtable = &dummy; walk(ctrl);
    check(seen_exactly(bare, N_METHODS), "with a vtable backend every method reaches rsx_state");

    static const u32 effects[] = { 0x0050u, 0x0180u, 0x01A4u, 0x1D6Cu, 0x1D70u, 0x0104u, 0x1D74u };
    engine_on = 1; vtable = NULL; walk(ctrl);
    if (trace)
        check(seen_exactly(bare, N_METHODS), "RSX_TRACE keeps every method with the draw engine and no vtable");
    else
        check(seen_exactly(effects, 7), "draw engine without a vtable: only reference, notify, DMA context and label methods");

    printf("[gcm_legacy_state_filter] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}
