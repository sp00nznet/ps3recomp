/* cellSysconfOpen / cellSysconfAbort: Open finishes at once and its callback
 * (result, userdata) runs on the next cellSysutilCheckCallback, not inside
 * Open; Abort with nothing open succeeds.
 *
 *   clang -std=gnu17 -I include libs/system/tests/test_sysconf.c libs/system/cellSysutil.c \
 *         -Wl,-dead_strip -pthread -o /tmp/test_sysconf && /tmp/test_sysconf
 */
#include "../cellMsgDialog.c"   /* what cellSysutil.c links against */
#include <assert.h>
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
void np_psnr_pump(void) {}
void ps3_np_eula_tick(void) {}

static unsigned calls;
static u64 last_result, last_user;
static void callback(u32 opd, u64 result, u64 user, u64 c, u64 d,
                     u64 e, u64 f, u64 g, u64 h)
{
    assert(opd == 0x400 && !c && !d && !e && !f && !g && !h);
    calls++; last_result = result; last_user = user;
}

int main(void)
{
    vm_base = calloc(1, 65536); assert(vm_base);
    g_ps3_guest_caller = callback;

    assert(cellSysconfAbort() == CELL_OK);
    assert(cellSysconfOpen(1, 0x400, 0xBEEF, 0, 0) == CELL_OK);
    assert(calls == 0);                       /* not from inside Open */
    cellSysutilCheckCallback();
    assert(calls == 1 && last_result == CELL_OK && last_user == 0xBEEF);
    cellSysutilCheckCallback();
    assert(calls == 1);                       /* once */
    assert(cellSysconfOpen(2, 0, 0, 0, 0) == CELL_OK);   /* no callback: fine */
    cellSysutilCheckCallback();
    assert(calls == 1);
    assert(cellSysconfAbort() == CELL_OK);

    free(vm_base);
    puts("cellSysconf checks passed");
    return 0;
}
