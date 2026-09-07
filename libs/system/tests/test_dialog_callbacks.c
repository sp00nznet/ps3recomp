/* macOS: clang -std=gnu17 -I include libs/system/tests/test_dialog_callbacks.c
 * libs/system/cellSysutil.c -Wl,-dead_strip -pthread -o /tmp/test_dialog_callbacks && /tmp/test_dialog_callbacks */
#include "../cellMsgDialog.c"
#include <assert.h>
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

static unsigned calls;
static int waiting;
static s32 last_result;
static void callback(u32 opd, u64 result, u64 user, u64 c, u64 d,
                     u64 e, u64 f, u64 g, u64 h)
{
    assert(opd == 0x100 && user == 0x1234 && !c && !d && !e && !f && !g && !h);
    assert(waiting);
    calls++; last_result = (s32)result;
    if (calls == 1) {
        assert(cellMsgDialogOpen2(CELL_MSGDIALOG_TYPE_BUTTON_TYPE_YESNO,
            (void*)0x200, (void*)0x100, (void*)0x1234, NULL) == CELL_OK);
        cellSysutilCheckCallback();
        assert(calls == 1);
    }
}
int main(void)
{
    vm_base = calloc(1, 65536); assert(vm_base);
    memcpy(vm_base + 0x200, "Install data", 13);
    g_ps3_guest_caller = callback;
    assert(cellMsgDialogOpen2(0xA0, (void*)0x200, (void*)0x100,
                            (void*)0x1234, NULL) == CELL_OK);
    assert(calls == 0);
    waiting = 1;
    cellSysutilCheckCallback(); assert(calls == 1 && last_result == CELL_MSGDIALOG_BUTTON_OK);
    cellSysutilCheckCallback(); assert(calls == 2 && last_result == CELL_MSGDIALOG_BUTTON_YES);
    cellSysutilCheckCallback(); assert(calls == 2);
    assert(cellMsgDialogOpen2(CELL_MSGDIALOG_TYPE_PROGRESSBAR_SINGLE,
        (void*)0x200, (void*)0x100, (void*)0x1234, NULL) == CELL_OK);
    cellSysutilCheckCallback(); assert(calls == 2);
    assert(cellMsgDialogClose(0) == CELL_OK); assert(calls == 2);
    g_ps3_guest_caller = NULL;
    cellSysutilCheckCallback(); assert(calls == 2);
    g_ps3_guest_caller = callback;
    cellSysutilCheckCallback(); assert(calls == 3 && last_result == CELL_MSGDIALOG_BUTTON_NONE);
    cellSysutilCheckCallback(); assert(calls == 3);
    free(vm_base);
    puts("Deferred dialog and button-mask checks passed");
}
