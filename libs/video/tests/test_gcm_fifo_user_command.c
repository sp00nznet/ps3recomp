/* GCM_SET_USER_COMMAND (method 0xEB00) executed by the FIFO walker reaches a
 * handler registered with the HLE cellGcmSetUserHandler.
 *
 * A command ring with one user command is built in guest memory, drained with
 * cellGcm_rsx_process_fifo and followed by ppu_gcm_pump, five times with a
 * different argument each time. The handler must run once per command, with
 * that command's argument. The lv2 route (rsx_raise_user_cmd, sys_rsx.c) is
 * stubbed here and must still be taken.
 *
 * gcc -std=gnu17 -I include -I libs/video
 *   libs/video/tests/test_gcm_fifo_user_command.c -ffunction-sections
 *   -fdata-sections -Wl,--gc-sections -pthread -o /tmp/test_gcm_fifo_user_command
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
int  rsx_process_method(rsx_state* state, u32 method, u32 data) { (void)state; (void)method; (void)data; return 0; }
/* No microcode or constant runs here: the walker hands every word to
 * rsx_process_method instead. */
int  rsx_process_vp_run(rsx_state* state, u32 method, const u8* src, u32 count)
{ (void)state; (void)method; (void)src; (void)count; return 0; }
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

static unsigned lv2_raised;
void rsx_raise_user_cmd(uint32_t arg) { (void)arg; lv2_raised++; }

#define IO_ADDR   0x00100000u
#define IO_SIZE   0x00100000u
#define CMD_SIZE  0x00010000u
#define HANDLER   0x00000100u

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

static unsigned calls;
static uint32_t last_arg, last_opd;
static void callback(uint32_t opd, uint64_t a, uint64_t b, uint64_t c,
                     uint64_t d, uint64_t e, uint64_t f, uint64_t h, uint64_t i)
{
    (void)b; (void)c; (void)d; (void)e; (void)f; (void)h; (void)i;
    calls++; last_opd = opd; last_arg = (uint32_t)a;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const uint32_t vm_size = ppu_hle_inject_base + 0x10000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { puts("FAIL: guest VM alloc"); return 1; }
    ppu_vm_size = vm_size;
    g_ps3_guest_caller = callback;

    u32 ctx_out = host_alloc(4, 4);
    u32 cdata = cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    uint32_t ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();
    cellGcmSetUserHandler((CellGcmUserHandler)(uintptr_t)HANDLER);

    int fails = 0;
    uint32_t head = 0;
    const uint32_t args[] = { 0x11, 0xC0FFEE, 0, 0x80000001u, 0x5 };
    for (unsigned n = 0; n < sizeof args / sizeof args[0]; n++) {
        /* One method, count 1, subchannel 7, method 0x0B00: GCM_SET_USER_COMMAND. */
        gw32(IO_ADDR + head, (1u << 18) | (7u << 13) | 0x0B00u); head += 4;
        gw32(IO_ADDR + head, args[n]); head += 4;
        gw32(cdata + 0x8, IO_ADDR + head);           /* context current */
        vm_write32(ctrl + 0, head);                  /* put */
        cellGcm_rsx_process_fifo();
        ppu_gcm_pump();
        int ok = vm_read32(ctrl + 4) == head && calls == n + 1 && last_opd == HANDLER &&
                 last_arg == args[n] && lv2_raised == n + 1;
        printf("[%s] user command %u (arg 0x%X): handler calls %u, last arg 0x%X, lv2 events %u\n",
               ok ? "PASS" : "FAIL", n + 1, args[n], calls, last_arg, lv2_raised);
        fails += !ok;
    }
    printf("[gcm_fifo_user_command] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}
