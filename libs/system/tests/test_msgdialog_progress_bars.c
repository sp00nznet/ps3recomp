/* Progress-bar calls check the open dialog: no dialog (or one without bars)
 * answers DIALOG_NOT_OPENED, an index past the dialog's own bar count answers
 * PARAM. Run: python tools/test_msgdialog_progress_bars.py */
#include "../cellMsgDialog.c"
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
void np_psnr_pump(void) {}
void ps3_np_eula_tick(void) {}

static unsigned failures;
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); failures++; \
} } while (0)

static void callback(u32 opd, u64 result, u64 user, u64 c, u64 d,
                     u64 e, u64 f, u64 g, u64 h)
{
    (void)opd; (void)result; (void)user; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h;
}

int main(void)
{
    vm_base = calloc(1, 65536);
    if (!vm_base) return 2;
    memcpy(vm_base + 0x200, "Progress", 9);
    g_ps3_guest_caller = callback;

    /* No dialog open. */
    CHECK(cellMsgDialogProgressBarSetMsg(0, (void*)0x200) == CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED);
    CHECK(cellMsgDialogProgressBarInc(0, 10) == CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED);
    CHECK(cellMsgDialogProgressBarReset(0) == CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED);

    /* One bar: index 0 works, index 1 is out of range. */
    CHECK(cellMsgDialogOpen2(CELL_MSGDIALOG_TYPE_PROGRESSBAR_SINGLE,
        (void*)0x200, (void*)0x100, (void*)0x1234, NULL) == CELL_OK);
    CHECK(cellMsgDialogProgressBarSetMsg(0, (void*)0x200) == CELL_OK);
    CHECK(cellMsgDialogProgressBarInc(0, 10) == CELL_OK);
    CHECK(cellMsgDialogProgressBarReset(0) == CELL_OK);
    CHECK(cellMsgDialogProgressBarSetMsg(1, (void*)0x200) == CELL_MSGDIALOG_ERROR_PARAM);
    CHECK(cellMsgDialogProgressBarInc(1, 10) == CELL_MSGDIALOG_ERROR_PARAM);
    CHECK(cellMsgDialogProgressBarReset(1) == CELL_MSGDIALOG_ERROR_PARAM);
    CHECK(cellMsgDialogAbort() == CELL_OK);
    cellSysutilCheckCallback();

    /* Closed again. */
    CHECK(cellMsgDialogProgressBarInc(0, 10) == CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED);

    /* Two bars: index 1 works, index 2 is out of range. */
    CHECK(cellMsgDialogOpen2(CELL_MSGDIALOG_TYPE_PROGRESSBAR_DOUBLE,
        (void*)0x200, (void*)0x100, (void*)0x1234, NULL) == CELL_OK);
    CHECK(cellMsgDialogProgressBarInc(1, 10) == CELL_OK);
    CHECK(cellMsgDialogProgressBarInc(2, 10) == CELL_MSGDIALOG_ERROR_PARAM);
    CHECK(cellMsgDialogAbort() == CELL_OK);
    cellSysutilCheckCallback();

    free(vm_base);
    if (failures) return 1;
    puts("Progress-bar calls check the open dialog and its bar count");
    return 0;
}
