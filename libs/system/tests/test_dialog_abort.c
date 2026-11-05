/* macOS: clang -std=gnu17 -I include libs/system/tests/test_dialog_abort.c
 * libs/system/cellSysutil.c -Wl,-dead_strip -pthread -o /tmp/test_dialog_abort && /tmp/test_dialog_abort */
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

static unsigned failures, calls;
static u32 last_opd;
static u64 last_result, last_user;
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); failures++; \
} } while (0)

static void callback(u32 opd, u64 result, u64 user, u64 c, u64 d,
                     u64 e, u64 f, u64 g, u64 h)
{
    CHECK(!c && !d && !e && !f && !g && !h);
    calls++; last_opd=opd; last_result=result; last_user=user;
}

int main(void)
{
    vm_base=calloc(1,65536);
    if (!vm_base) return 2;
    memcpy(vm_base+0x200,"Progress",9);
    g_ps3_guest_caller=callback;

    CHECK(cellMsgDialogOpen2(CELL_MSGDIALOG_TYPE_PROGRESSBAR_SINGLE,
        (void*)0x200,(void*)0x100,(void*)0x1234,NULL)==CELL_OK);
    CHECK(cellMsgDialogAbort()==CELL_OK);
    cellSysutilCheckCallback();
    CHECK(calls==0);
    CHECK(cellMsgDialogClose(0)==CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED);
    CHECK(cellMsgDialogAbort()==CELL_OK);
    CHECK(cellMsgDialogAbort()==CELL_OK);
    cellSysutilCheckCallback();
    CHECK(calls==0);

    /* Closing the replacement must deliver only its own OPD and userdata. */
    calls=0;
    CHECK(cellMsgDialogOpen2(CELL_MSGDIALOG_TYPE_PROGRESSBAR_SINGLE,
        (void*)0x200,(void*)0x300,(void*)0x9876,NULL)==CELL_OK);
    CHECK(cellMsgDialogClose(0)==CELL_OK);
    CHECK(calls==0);
    cellSysutilCheckCallback();
    CHECK(calls==1 && last_opd==0x300 && last_user==0x9876);
    CHECK(last_result==CELL_MSGDIALOG_BUTTON_NONE);
    CHECK(cellMsgDialogAbort()==CELL_OK);
    cellSysutilCheckCallback();
    CHECK(calls==1);

    free(vm_base);
    if (failures) return 1;
    puts("Dialog abort suppresses callbacks; ordinary close preserves callback identity");
    return 0;
}
