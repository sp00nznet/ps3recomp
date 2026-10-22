/* The control register's get is past a semaphore RELEASE by the time its
 * label lands (cellGcmSys.c).
 *
 * On the RSX the semaphore write happens after the command has been fetched,
 * so a title that sees a label move may read get once and take it as the
 * RSX's position. The walker wrote the label at once but published get only
 * at a JUMP and at the end of the pass, so that read returned where the pass
 * started: Ratchet & Clank: Tools of Destruction then spins until its own put
 * equals that get, which it never does.
 *
 * The probe is GCM_SET_USER_COMMAND (subchannel 7, method 0x0B00), which
 * calls rsx_raise_user_cmd from inside the pass; the stub records the label
 * and get there. A probe before any RELEASE sees neither move, a single
 * RELEASE and a two-method ACQUIRE+RELEASE each leave get past the whole
 * command, and get does not move with the methods after it. With
 * GCM_GET_EQ_PUT=1 (argument "eq-put") the RELEASE publishes nothing and get
 * is put at the end of the pass, as before.
 *
 * gcc -std=gnu17 -I include -I libs/video
 *   libs/video/tests/test_gcm_get_on_release.c -ffunction-sections
 *   -fdata-sections -Wl,--gc-sections -pthread -o /tmp/test_gcm_get_on_release
 *   (clang on macOS: -Wl,-dead_strip)
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

#define SEMA_OFF  0x10u
#define LABEL_EA  (GCM_LABEL_GUEST_BASE + SEMA_OFF)

/* Records the label and get each time a probe user command runs. */
static u32 seen_get[8], seen_label[8], seen_n;
void rsx_raise_user_cmd(uint32_t arg)
{
    if (arg < 8) {
        seen_get[arg] = vm_read32(GCM_CONTROL_GUEST_ADDR + 4);
        seen_label[arg] = vm_read32(LABEL_EA);
        seen_n++;
    }
}

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

/* An incrementing method header: count words, subchannel subch. */
static u32 put_cmd(u32 io, u32 subch, u32 method, u32 count, const u32* args)
{
    gw32(IO_ADDR + io, (count << 18) | (subch << 13) | method);
    for (u32 i = 0; i < count; i++) gw32(IO_ADDR + io + 4 + i * 4, args[i]);
    return io + 4 + count * 4;
}
static u32 put_probe(u32 io, u32 arg) { return put_cmd(io, 7, 0x0B00u, 1, &arg); }

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const int eq_put = argc > 1 && !strcmp(argv[1], "eq-put");
    if (eq_put != gcm_get_eq_put()) { puts("FAIL: GCM_GET_EQ_PUT does not match the argument"); return 1; }
    const uint32_t vm_size = ppu_hle_inject_base + 0x10000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { puts("FAIL: guest VM alloc"); return 1; }
    ppu_vm_size = vm_size;

    u32 ctx_out = host_alloc(4, 4);
    (void)cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    const uint32_t ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();

    /* probe 0, OFFSET, RELEASE 0x11, probe 1, ACQUIRE 0x11 + RELEASE 0x22 in
     * one two-method command, probe 2, probe 3; put after the last probe. */
    const u32 off = SEMA_OFF, rel1 = 0x11u, acq_rel[2] = { 0x11u, 0x22u };
    u32 io = put_probe(0, 0);
    io = put_cmd(io, 0, 0x64u, 1, &off);
    io = put_cmd(io, 0, 0x6Cu, 1, &rel1);
    const u32 end_rel1 = io;
    io = put_probe(io, 1);
    io = put_cmd(io, 0, 0x68u, 2, acq_rel);
    const u32 end_rel2 = io;
    io = put_probe(io, 2);
    io = put_probe(io, 3);
    const u32 put = io;

    s_fifo_getoff = 0;
    vm_write32(ctrl + 4, 0);
    vm_write32(ctrl + 0, put);
    cellGcm_rsx_process_fifo();

    check(seen_n == 4, "the four probe commands ran");
    check(seen_get[0] == 0 && seen_label[0] == 0, "before any RELEASE, neither get nor the label has moved");
    check(seen_label[1] == 0x11u, "the RELEASE wrote its label");
    check(seen_label[2] == 0x22u, "the RELEASE inside ACQUIRE+RELEASE wrote its label");
    if (!eq_put) {
        check(seen_get[1] == end_rel1, "get is past the RELEASE when its label is seen");
        check(seen_get[2] == end_rel2, "get is past the whole ACQUIRE+RELEASE command");
        check(seen_get[3] == end_rel2, "get does not move with the methods after it");
    } else {
        check(seen_get[1] == 0 && seen_get[2] == 0 && seen_get[3] == 0,
              "GCM_GET_EQ_PUT: a RELEASE publishes nothing mid-pass");
    }
    check(vm_read32(ctrl + 4) == put, "after the pass, get is put");

    printf("[gcm_get_on_release%s] %s (%d failed)\n", eq_put ? " eq-put" : "", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}
