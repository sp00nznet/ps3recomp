/* A FIFO resync replays the label releases it skips (cellGcmSys.c).
 *
 * A resync moves get straight to put. The fences in between were already
 * swept up; a label release in between was lost, and a title that waits on
 * it waited for good. The skipped range is now walked as a command stream and
 * its NV406E (0x64/0x6C) and NV4097 (0x01A4/0x1D6C/0x1D70/0x1D74) sync writes
 * are replayed.
 *
 * io 0 holds a park (a JUMP to itself) and put lies past it, so the walker
 * parks; once the park has stood with the same put for GCM_PARK_GRACE_MS the
 * next pass resyncs to put. Between the park and put: a NOP, an NV406E
 * semaphore offset + release, a forward JUMP over garbage, and an NV4097
 * semaphore offset + back-end release.
 *
 * gcc -std=gnu17 -I include -I libs/video
 *   libs/video/tests/test_gcm_resync_labels.c -ffunction-sections
 *   -fdata-sections -Wl,--gc-sections -pthread -o /tmp/test_gcm_resync_labels
 *   (clang on macOS: -Wl,-dead_strip)
 */
#include "../cellGcmSys.c"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
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
/* The NV4097 sync methods the resync hands on, in order. */
static u32 seen_m[16], seen_v[16], seen_n;
int  rsx_process_method(rsx_state* state, u32 method, u32 data)
{
    (void)state;
    if (seen_n < 16) { seen_m[seen_n] = method; seen_v[seen_n] = data; seen_n++; }
    return 0;
}
int  rsx_draw_engine_enabled(void) { return 0; }
/* The walker hands vertex-program runs here (#554) and asks for a vtable
 * backend (#466); this fixture has neither. */
int  rsx_process_vp_run(rsx_state* state, u32 method, const u8* src, u32 count)
{ (void)state; (void)method; (void)src; (void)count; return 0; }
rsx_backend* rsx_get_backend(void) { return NULL; }
void rsx_draw_engine_method(u32 method, u32 arg) { (void)method; (void)arg; }
void rsx_draw_engine_note_flip(u32 buffer_id) { (void)buffer_id; }
void rsx_draw_engine_set_display_buffer(u32 id, u32 location, u32 offset, u32 pitch, u32 width, u32 height)
{ (void)id; (void)location; (void)offset; (void)pitch; (void)width; (void)height; }
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

/* One method, count 1, subchannel 0. */
static u32 put_method(u32 io, u32 method, u32 arg)
{
    gw32(IO_ADDR + io, (1u << 18) | method);
    gw32(IO_ADDR + io + 4, arg);
    return io + 8;
}

static void wait_past_grace(void)
{
    const unsigned long long t0 = ps3_ms_now();
    while (ps3_ms_now() - t0 <= GCM_PARK_GRACE_MS + 10u) {
#ifdef _WIN32
        Sleep(5);
#else
        struct timespec ts = {0, 5000000};
        nanosleep(&ts, NULL);
#endif
    }
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

    u32 io = 0;
    gw32(IO_ADDR + io, 0x20000000u | io); io += 4;      /* park: JUMP to itself */
    gw32(IO_ADDR + io, 0); io += 4;                     /* NOP */
    io = put_method(io, 0x0064u, 0x0030u);              /* NV406E semaphore offset */
    io = put_method(io, 0x006Cu, 0x00C0FFEEu);          /* NV406E semaphore release */
    const u32 over = io + 12;
    gw32(IO_ADDR + io, 0x20000000u | over);             /* forward JUMP ... */
    gw32(IO_ADDR + io + 4, 0xDEADBEEFu);                /* ... over two words that */
    gw32(IO_ADDR + io + 8, 0xFFFFFFFFu);                /* are not commands */
    io = over;
    io = put_method(io, 0x1D6Cu, 0x0040u);              /* NV4097 semaphore offset */
    io = put_method(io, 0x1D74u, 0x00000133u);          /* texture-read release */
    const u32 put = io;

    s_fifo_getoff = 0;
    vm_write32(ctrl + 4, 0);
    vm_write32(ctrl + 0, put);
    cellGcm_rsx_process_fifo();                         /* parks; the grace starts */
    check(s_fifo_getoff == 0, "the first pass stops on the park");
    check(seen_n == 0, "nothing past the park ran yet");

    wait_past_grace();
    cellGcm_rsx_process_fifo();                         /* still parked: resync */
    check(s_fifo_getoff == put, "the resync moved get to put");
    check(vm_read32(GCM_LABEL_GUEST_BASE + 0x30u) == 0x00C0FFEEu,
          "the skipped NV406E release was written to its label");
    check(s_sema_offset == 0x30u, "the NV406E semaphore offset follows the skipped one");
    check(seen_n == 2 && seen_m[0] == 0x1D6Cu && seen_v[0] == 0x40u &&
          seen_m[1] == 0x1D74u && seen_v[1] == 0x133u,
          "the skipped NV4097 offset and release reached rsx_process_method, in order");

    printf("[gcm_resync_labels] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}
