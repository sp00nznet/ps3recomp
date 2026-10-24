/* The FIFO walker passes over runs of NOP words at once (cellGcmSys.c).
 *
 * A run of zero words is skipped in one step, up to put or the end of the
 * 1 MB IO page. The methods around the runs must still arrive in order with
 * their data, a run must stop at put when the zeros go on past it (and the
 * next pass must carry on from there), and a run that reaches the end of an
 * IO page must not skip what the next page holds.
 *
 * gcc -std=gnu17 -I include -I libs/video
 *   libs/video/tests/test_gcm_nop_runs.c -ffunction-sections
 *   -fdata-sections -Wl,--gc-sections -pthread -o /tmp/test_gcm_nop_runs
 *   (clang on macOS: -Wl,-dead_strip)
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
/* The walker's other consumers: no graphics backend, no lifted program. */
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
u32 g_rsx_last_reference;
void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
void ppu_dump_bctrl_ring(uint32_t n, const char* why) { (void)n; (void)why; }
void ppu_dump_guest_stack(ppu_context* c, const char* why) { (void)c; (void)why; }
void rsx_state_init(rsx_state* state) { (void)state; }
/* Every method the walker delivers, in order. */
#define PROBE_METHOD 0x1EE0u
static u32 seen_m[16], seen_d[16], seen_n;
int  rsx_process_vp_run(rsx_state* state, u32 method, const u8* src, u32 count)
{ (void)state; (void)method; (void)src; (void)count; return 0; }
int  rsx_process_method(rsx_state* state, u32 method, u32 data)
{
    (void)state;
    if (seen_n < 16) { seen_m[seen_n] = method; seen_d[seen_n] = data; }
    seen_n++;
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
#define IO_SIZE   0x00200000u     /* two IO pages */
#define CMD_SIZE  0x00010000u
#define PAGE2     0x00100000u     /* IO offset of the second page */

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

    /* Pass 1: probe 0, 50 NOPs, a two-word incrementing run, 3 NOPs, and put
     * five words into the zeros that follow. */
    u32 io = put_probe(0, 0);
    io += 50 * 4;
    gw32(IO_ADDR + io, (2u << 18) | PROBE_METHOD);
    gw32(IO_ADDR + io + 4, 1);
    gw32(IO_ADDR + io + 8, 2);
    io += 12 + 3 * 4;
    const u32 put1 = io + 5 * 4;

    s_fifo_getoff = 0;
    vm_write32(ctrl + 4, 0);
    vm_write32(ctrl + 0, put1);
    cellGcm_rsx_process_fifo();

    check(seen_n == 3, "pass 1: three method words delivered");
    check(seen_m[0] == PROBE_METHOD && seen_d[0] == 0, "pass 1: probe before the NOP run");
    check(seen_m[1] == PROBE_METHOD && seen_d[1] == 1 &&
          seen_m[2] == PROBE_METHOD + 4 && seen_d[2] == 2, "pass 1: the run after the NOPs, in order");
    check(s_fifo_getoff == put1, "pass 1: the NOP run stops at put, inside the zeros");

    /* Pass 2: more zeros, then probe 3. */
    const u32 p3 = put1 + 6 * 4;
    const u32 put2 = put_probe(p3, 3);
    vm_write32(ctrl + 0, put2);
    cellGcm_rsx_process_fifo();
    check(seen_n == 4 && seen_m[3] == PROBE_METHOD && seen_d[3] == 3, "pass 2: carries on from put through the zeros");
    check(s_fifo_getoff == put2, "pass 2: get reaches put");

    /* Pass 3: JUMP near the end of the first IO page, zeros to its end, and
     * probe 5 as the first word of the second page. */
    const u32 tail = PAGE2 - 0x40;
    gw32(IO_ADDR + put2, 0x20000000u | tail);
    const u32 put3 = put_probe(PAGE2, 5);
    vm_write32(ctrl + 0, put3);
    cellGcm_rsx_process_fifo();
    check(seen_n == 5 && seen_m[4] == PROBE_METHOD && seen_d[4] == 5,
          "pass 3: the first word of the next IO page is not skipped");
    check(s_fifo_getoff == put3, "pass 3: get reaches put");

    printf("[gcm_nop_runs] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}
