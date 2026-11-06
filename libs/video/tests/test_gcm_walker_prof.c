/* RSX_WALKER_PROF (cellGcmSys.c), the in-process walker sampler, on a host
 * fixture: with RSX_WALKER_PROF=<file> and RSX_WALKER_PROF_SECONDS=1 the
 * first thread to drain the FIFO is sampled and the file holds the samples.
 * The fixture stretches every nominal 1 ms sleep to prove the requested
 * duration follows elapsed time rather than a count of sleeps. Without the
 * variable nothing is armed and no file appears. macOS arm64 only (the sampler
 * is compiled out elsewhere and this test only checks that). Diagnostic
 * coverage; it never starts a graphics backend.
 *
 * clang -std=gnu17 -I include -I libs/video libs/video/tests/test_gcm_walker_prof.c
 *   -ffunction-sections -fdata-sections -Wl,-dead_strip -o /tmp/test_gcm_walker_prof
 * Run: /tmp/test_gcm_walker_prof on <file>   (RSX_WALKER_PROF=<file> RSX_WALKER_PROF_SECONDS=1)
 *      /tmp/test_gcm_walker_prof off <file>  (RSX_WALKER_PROF unset)
 */
#include <time.h>
#if defined(__APPLE__) && defined(__aarch64__)
static int wprof_test_nanosleep(const struct timespec* req, struct timespec* rem);
#define nanosleep wprof_test_nanosleep
#endif
#include "../cellGcmSys.c"
#if defined(__APPLE__) && defined(__aarch64__)
#undef nanosleep
/* Model timer coalescing: duration follows wall time, not 1000 sleeps. */
static int wprof_test_nanosleep(const struct timespec* req, struct timespec* rem)
{
    struct timespec delayed = *req;
    if (delayed.tv_sec == 0 && delayed.tv_nsec == 1000000)
        delayed.tv_nsec = 5000000;
    return nanosleep(&delayed, rem);
}
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
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
int  rsx_process_method(rsx_state* state, u32 method, u32 data) { (void)state; (void)method; (void)data; return 0; }
int  rsx_draw_engine_enabled(void) { return 0; }
/* The walker hands vertex-program runs here (#554) and asks for a vtable
 * backend (#466); this fixture has neither. */
int  rsx_process_vp_run(rsx_state* state, u32 method, const u8* src, u32 count)
{ (void)state; (void)method; (void)src; (void)count; return 0; }
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
static void host_w32(u32 ea, u32 v)
{
    uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static int fails;
static void check(int ok, const char* what)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Drains (an empty FIFO) and burns a little CPU between drains, so the
 * sampled thread is running when the signals land. */
static volatile unsigned sink;
static void walker_loop(double seconds)
{
    const double end = now_s() + seconds;
    while (now_s() < end) {
        cellGcm_rsx_process_fifo();
        for (unsigned k = 0; k < 20000; k++) sink += k * 2654435761u;
#if defined(__APPLE__) && defined(__aarch64__)
        if (atomic_load(&s_wprof_state) == 2) break;
#endif
    }
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) { puts("usage: test_gcm_walker_prof on|off <file>"); return 2; }
    const int on = strcmp(argv[1], "on") == 0;
    const char* path = argv[2];
    const uint32_t vm_size = ppu_hle_inject_base + 0x10000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { puts("FAIL: guest VM alloc"); return 1; }
    ppu_vm_size = vm_size;
    u32 ctx_out = host_alloc(4, 4);
    (void)cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);

#if defined(__APPLE__) && defined(__aarch64__)
    if (!on) {
        walker_loop(0.2);
        check(atomic_load(&s_wprof_state) == 2, "without RSX_WALKER_PROF the sampler is not armed");
        FILE* f = fopen(path, "r");
        check(f == NULL, "without RSX_WALKER_PROF no profile is written");
        if (f) fclose(f);
    } else {
        walker_loop(4.0);
        check(atomic_load(&s_wprof_state) == 2, "the sampler finishes after RSX_WALKER_PROF_SECONDS");
        FILE* f = fopen(path, "r");
        check(f != NULL, "the profile file is written");
        int n = -1, lines = 0, frames_ok = 1;
        char line[4096];
        if (f && fgets(line, sizeof line, f)) sscanf(line, "# walker samples %d", &n);
        while (f && fgets(line, sizeof line, f)) {
            lines++;
            /* innermost first: hex image offsets or @symbol, space separated */
            if (line[0] == '\n' || !(isxdigit((unsigned char)line[0]) || line[0] == '@')) frames_ok = 0;
        }
        if (f) fclose(f);
        printf("  %d samples, %d lines\n", n, lines);
        check(n >= 10 && lines == n, "periodic samples are written one per line");
        check(frames_ok, "every sample has frames");
    }
#else
    (void)on; (void)path;
    walker_loop(0.05);
    check(1, "sampler compiled out on this host");
#endif
    printf("[gcm_walker_prof] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}
