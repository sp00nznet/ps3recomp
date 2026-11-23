/* cellGcm_wait_commands_walked (cellGcmSys.c): a host writer that fills guest
 * memory the renderer samples as a texture (cellVdec's ARGB / RGBA pictures)
 * waits until the FIFO walker has executed everything the title had published
 * when it was called, so every draw queued before it has been recorded.
 *
 *   1. Nothing published and not yet walked: it returns 0 at once.
 *   2. Commands published, walker not run: it waits, and times out (-1) at the
 *      bound; a walker pass that reaches put releases it.
 *   3. An earlier pass, that ended before the commands were published, does
 *      not release it; the next one does.
 *   4. A pass that stops short of put (at a flip, one per pass) does not
 *      release it; the pass that reaches put does.
 *   5. A walker that resyncs out of unmapped IO to put releases it.
 *   6. With the walker running on its own thread, a caller that also flips
 *      every iteration is never held to the bound: it does not wait for a flip
 *      of its own.
 *
 * cc -std=gnu17 -I include -I libs/video libs/video/tests/test_gcm_wait_walked.c
 *   -ffunction-sections -fdata-sections -Wl,--gc-sections -pthread
 *   -o /tmp/test_gcm_wait_walked        (clang on macOS: -Wl,-dead_strip)
 */
#include "../cellGcmSys.c"
#include "../rsx_tiling.c"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
/* GCM_REPORT_SPU_HOLD_MS asks the SPU task layer whether a task round is
 * running; there are no SPU tasks here. */
int spu_taskset_round_busy(uint32_t* ts, uint32_t* tid) { (void)ts; (void)tid; return 0; }
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
/* gcm_fifo_next_segment wakes the drain thread; this fixture has none, and
 * runtime/platform/win32_compat.c is not linked. */
#ifndef _WIN32
BOOL SetEvent(HANDLE h) { (void)h; return 1; }
#endif
void ppu_dump_bctrl_ring(uint32_t n, const char* why) { (void)n; (void)why; }
void ppu_dump_guest_stack(ppu_context* c, const char* why) { (void)c; (void)why; }
void rsx_state_init(rsx_state* state) { (void)state; }
/* ZSTENCIL_CLEAR_VALUE writes as each consumer of the methods saw them. */
static u32 g_zs_state[4], g_zs_engine[4];
static int g_n_zs_state, g_n_zs_engine, g_engine_on;
int  rsx_process_method(rsx_state* state, u32 method, u32 data)
{
    (void)state;
    if (method == 0x1D8Cu && g_n_zs_state < 4) g_zs_state[g_n_zs_state++] = data;
    return 0;
}
int  rsx_draw_engine_enabled(void) { return g_engine_on; }
void rsx_draw_engine_method(u32 method, u32 arg)
{
    if ((method & 0x1FFCu) == 0x1D8Cu && g_n_zs_engine < 4) g_zs_engine[g_n_zs_engine++] = arg;
}

/* No microcode or constant runs here: the walker hands every word to
 * rsx_process_method instead (#554). */
int  rsx_process_vp_run(rsx_state* state, u32 method, const u8* src, u32 count)
{ (void)state; (void)method; (void)src; (void)count; return 0; }
/* cellGcmSys asks whether a vtable backend is registered (#466). */
rsx_backend* rsx_get_backend(void) { return NULL; }
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
static void check(int ok, const char* what)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}

static u32 g_ctx;
/* One NV4097 method with a data word, written at the context's current. */
static void emit_cmd(void)
{
    const u32 cur = vm_read32(g_ctx + 8);
    vm_write32(cur, (1u << 18) | 0x0304u);
    vm_write32(cur + 4, 1);
    vm_write32(g_ctx + 8, cur + 8);
}
/* What cellGcmFlush does: put to the end of what was written. */
static void publish(void) { vm_write32(GCM_CONTROL_GUEST_ADDR + 0, gcm_ea2io(vm_read32(g_ctx + 8))); }

/* A writer thread: waits for the walker the way cellVdecGetPicture does. */
static volatile int g_done, g_result;
static volatile u32 g_timeout_ms;
static void* waiter(void* arg)
{
    (void)arg;
    g_result = cellGcm_wait_commands_walked(g_timeout_ms);
    g_done = 1;
    return NULL;
}
static pthread_t start_waiter(u32 timeout_ms)
{
    pthread_t t;
    g_done = 0;
    g_result = -99;
    g_timeout_ms = timeout_ms;
    pthread_create(&t, NULL, waiter, NULL);
    return t;
}
static void sleep_ms(unsigned ms) { usleep(ms * 1000u); }
static int returns_within(unsigned ms)
{
    for (unsigned i = 0; i < ms && !g_done; i++) sleep_ms(1);
    return g_done;
}

/* The walker on a thread of its own, as a host's frame loop runs it. */
static volatile int g_walker_stop;
static void* walker_thread(void* arg)
{
    (void)arg;
    while (!g_walker_stop) { cellGcm_rsx_process_fifo(); sleep_ms(1); }
    return NULL;
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
    g_ctx = host_alloc(0x18, 16);
    const u32 buf = IO_ADDR + 0x200000u;
    vm_write32(g_ctx + 0x0, buf);
    vm_write32(g_ctx + 0x4, buf + 0x30000u);
    vm_write32(g_ctx + 0x8, buf);
    vm_write32(g_ctx + 0xC, g_ctx + 0x10);
    vm_write32(g_ctx + 0x10, TITLE_CB);
    vm_write32(g_ctx + 0x14, 0);
    const u32 io = gcm_ea2io(buf);
    s_fifo_getoff = io;
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, io);    /* put == get: nothing queued */
    (void)GCM_PENDING_TAKE();

    /* 1 */
    check(cellGcm_wait_commands_walked(1000) == 0, "nothing published: it returns 0 at once");
    emit_cmd();                                   /* written, not published */
    check(cellGcm_wait_commands_walked(1000) == 0, "written but not published: nothing to wait for");

    /* 2: published, not walked. */
    publish();
    check(cellGcm_wait_commands_walked(0) == -1, "a bound of 0 does not wait");
    check(cellGcm_wait_commands_walked(20) == -1, "the walker never runs: it times out");
    pthread_t t = start_waiter(1000);
    check(!returns_within(60), "published commands the walker has not run do not release it");
    cellGcm_rsx_process_fifo();
    check(returns_within(1500) && g_result >= 0, "a walker pass that reaches put releases it");
    pthread_join(t, NULL);
    check(g_result >= 0 && s_fifo_getoff == gcm_ea2io(vm_read32(g_ctx + 8)), "with the time it waited, and get at put");
    check(cellGcm_wait_commands_walked(1000) == 0, "and with get at put, nothing is left to wait for");

    /* 3: a pass that ended before this was published is not enough. */
    cellGcm_rsx_process_fifo();                   /* an earlier pass, caught up */
    emit_cmd();
    publish();
    check(cellGcm_wait_commands_walked(20) == -1, "a pass from before the commands were published does not count");
    t = start_waiter(1000);
    sleep_ms(20);
    cellGcm_rsx_process_fifo();
    check(returns_within(1500) && g_result >= 0, "the next pass does");
    pthread_join(t, NULL);

    /* 4: a pass that stops at a flip short of put (one flip per pass). */
    (void)_cellGcmSetFlipCommand((void*)(uintptr_t)g_ctx, 1);     /* flip marker */
    emit_cmd();                                                   /* behind it */
    publish();
    t = start_waiter(1000);
    sleep_ms(20);
    cellGcm_rsx_process_fifo();
    check(s_fifo_getoff != gcm_ea2io(vm_read32(g_ctx + 8)), "the pass stopped at the flip, short of put");
    check(!returns_within(60), "a pass that stops short of put does not release it");
    cellGcm_rsx_process_fifo();
    check(returns_within(1500) && g_result >= 0, "the pass that reaches put does");
    pthread_join(t, NULL);

    /* 5: the walker is lost in unmapped IO and resyncs to put. */
    emit_cmd();
    publish();
    t = start_waiter(1000);
    sleep_ms(20);
    s_fifo_getoff = 0x0FD00000u;              /* no mapping there */
    cellGcm_rsx_process_fifo();
    check(s_fifo_getoff == gcm_ea2io(vm_read32(g_ctx + 8)), "the walker resynced to put");
    check(returns_within(1500) && g_result >= 0, "a resync to put releases it");
    pthread_join(t, NULL);

    /* 6: the walker on its own thread; the writer flips every iteration, as a
     * movie loop on the title's render thread does. None is held to the bound. */
    pthread_t w;
    g_walker_stop = 0;
    pthread_create(&w, NULL, walker_thread, NULL);
    int slow = 0, timeouts = 0;
    for (int i = 0; i < 30; i++) {
        emit_cmd();
        (void)_cellGcmSetFlipCommand((void*)(uintptr_t)g_ctx, 1);
        publish();
        const int ms = cellGcm_wait_commands_walked(40);
        timeouts += ms < 0;
        slow += ms > 10;
    }
    g_walker_stop = 1;
    pthread_join(w, NULL);
    check(timeouts == 0, "a caller that flips every iteration never reaches the 40 ms bound");
    check(slow == 0, "and is released within a few ms");

    printf("[gcm_wait_walked] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}
