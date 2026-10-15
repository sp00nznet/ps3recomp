/* CPU-only host-input fixture for the real strict message dialog/sysutil path. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>

static int errors;
static uint64_t fake_now_ms = 1000;
static jmp_buf strict_jump;
static const char* strict_category;

static void expect(int ok, const char* why)
{
    if (!ok) { ++errors; fprintf(stderr, "FAIL: %s\n", why); }
}

#define PS3EMU_STRICT_BOOT_H
static int ps3_strict_boot_enabled(void) { return 1; }
static void ps3_strict_boot_guard(const char* category, const char* format, ...)
{
    (void)format;
    strict_category = category;
    longjmp(strict_jump, 1);
}

/* Sysutil declares a VM store ABI independently of the PPU inline helpers. */
#define vm_write8 fixture_vm_write8
#define vm_write32 fixture_vm_write32
#include "../libs/system/cellSysutil.c"
#undef vm_write8
#undef vm_write32
void fixture_vm_write8(uint64_t ea, uint8_t value) { (void)ea; (void)value; }
void fixture_vm_write32(uint64_t ea, uint32_t value) { (void)ea; (void)value; }
void np_psnr_pump(void) {}
const char* cellGame_get_title_id(void) { return "TEST00001"; }

#include "../runtime/platform/win32_compat.h"
static ULONGLONG fixture_tick(void) { return (ULONGLONG)fake_now_ms; }
#define GetTickCount64 fixture_tick
#include "../libs/system/cellMsgDialog.c"
#undef GetTickCount64

uint8_t* vm_base;
uint32_t ppu_vm_size = 2048;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
int spu_coh_is_reserved(uint32_t ea) { (void)ea; return 0; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t ea, uint64_t value, int width)
{ (void)ea; (void)value; (void)width; }

typedef struct { u32 opd; u64 arg[8]; } Call;
static Call calls[64];
static int call_count;
static void caller(u32 opd, u64 a0, u64 a1, u64 a2, u64 a3,
                   u64 a4, u64 a5, u64 a6, u64 a7)
{
    if (call_count >= 64) { ++errors; return; }
    Call* c = &calls[call_count++];
    c->opd = opd;
    c->arg[0] = a0; c->arg[1] = a1; c->arg[2] = a2; c->arg[3] = a3;
    c->arg[4] = a4; c->arg[5] = a5; c->arg[6] = a6; c->arg[7] = a7;
}

#define SYS_OPD 0x1010u
#define SYS_USER 0x2020u
#define DLG_OPD 0x3030u
#define DLG_USER 0x4040u
#define PARAM_ABI ((s32)0x8002B301u)
#define NOT_OPEN_ABI ((s32)0x8002B302u)
#define BUSY_ABI ((s32)0x8002B105u)

static s32 open_dialog(u32 type)
{
    return cellMsgDialogOpen2(type, (const char*)(uintptr_t)0x100,
        (CellMsgDialogCallback)(uintptr_t)DLG_OPD,
        (void*)(uintptr_t)DLG_USER, NULL);
}

static void clear_calls(void) { call_count=0; memset(calls,0,sizeof(calls)); }
static void check_event(int i, u32 code)
{
    expect(i<call_count,"expected event exists");
    if (i<call_count) {
        expect(calls[i].opd==SYS_OPD,"event uses sysutil callback");
        expect(calls[i].arg[0]==code && calls[i].arg[1]==0 &&
               calls[i].arg[2]==SYS_USER,"event code and arguments");
    }
}
static void check_complete(int i, s32 result)
{
    expect(i<call_count,"completion exists");
    if (i<call_count) {
        expect(calls[i].opd==DLG_OPD,"completion uses dialog callback");
        expect(calls[i].arg[0]==(u64)(int64_t)result &&
               calls[i].arg[1]==DLG_USER,"completion result and userdata");
    }
}
static void begin(void)
{
    expect(call_count==0,"Open never enters guest synchronously");
    cellSysutilCheckCallback();
    expect(call_count==1,"one BEGIN on poll");
    check_event(0,0x121);
}
static void finished(s32 result)
{
    expect(call_count==1,"response never enters guest synchronously");
    cellSysutilCheckCallback();
    expect(call_count==3,"END and completion delivered on poll");
    check_event(1,0x122);
    check_complete(2,result);
    cellSysutilCheckCallback();
    expect(call_count==3,"no duplicate completion on later poll");
}
static Ps3MsgDialogHostState snapshot(void)
{
    Ps3MsgDialogHostState state;
    memset(&state,0xA5,sizeof(state));
    expect(ps3_msgdialog_host_snapshot(&state)==1,"enabled snapshot available");
    return state;
}

static void test_enable_and_copy(uint8_t* memory)
{
    Ps3MsgDialogHostState off;
    memset(&off,0xA5,sizeof(off));
    expect(ps3_msgdialog_host_snapshot(NULL)==0,"null snapshot rejected");
    expect(ps3_msgdialog_host_snapshot(&off)==0 && off.open==0,
           "disabled snapshot unavailable and cleared");
    expect(ps3_msgdialog_host_respond(0,CELL_MSGDIALOG_BUTTON_ESCAPE)==(s32)CELL_ENOSYS,
           "host response disabled by default");
    expect(ps3_msgdialog_host_enable(2)==PARAM_ABI,"invalid enable rejected");
    expect(ps3_msgdialog_host_enable(1)==0,"enable while idle");
    expect(ps3_msgdialog_host_enable(-1)==PARAM_ABI,"negative enable rejected");
    Ps3MsgDialogHostState closed=snapshot();
    expect(closed.open==0,"idle snapshot closed");
    clear_calls();
    expect(open_dialog(CELL_MSGDIALOG_TYPE_SE_TYPE_NORMAL)==0,"no-button opens");
    Ps3MsgDialogHostState s=snapshot();
    expect(s.open==1 && s.type==CELL_MSGDIALOG_TYPE_SE_TYPE_NORMAL &&
           strcmp(s.message,"Synthetic notice")==0,"open state, type, copied message");
    expect(s.generation>closed.generation,"generation increases on admission");
    expect(ps3_msgdialog_host_enable(0)==BUSY_ABI,"cannot disable during dialog");
    expect(ps3_msgdialog_host_enable(1)==BUSY_ABI,"cannot change host setting during dialog");
    strcpy(s.message,"mutated copy");
    memcpy(memory+0x100,"Changed guest\0",14);
    expect(strcmp(snapshot().message,"Synthetic notice")==0,
           "snapshot isolated from host edits and later guest memory edits");
    expect(ps3_msgdialog_host_respond(s.generation,CELL_MSGDIALOG_BUTTON_NONE)==PARAM_ABI,
           "no-button rejects fabricated NONE completion");
    begin();
    expect(ps3_msgdialog_host_respond(s.generation,CELL_MSGDIALOG_BUTTON_ESCAPE)==0,
           "cancel-enabled no-button accepts ESC");
    finished(CELL_MSGDIALOG_BUTTON_ESCAPE);
    expect(snapshot().open==0,"completion closes snapshot");
    expect(ps3_msgdialog_host_respond(s.generation,CELL_MSGDIALOG_BUTTON_ESCAPE)==NOT_OPEN_ABI,
           "duplicate response rejected");
    expect(ps3_msgdialog_host_enable(0)==0,"disable while idle");
    expect(ps3_msgdialog_host_snapshot(&off)==0 && off.open==0,
           "snapshot disabled after close");
    expect(ps3_msgdialog_host_enable(1)==0,"re-enable while idle");
    memcpy(memory+0x100,"Synthetic notice",sizeof("Synthetic notice"));
}

static void test_buttons(u32 type, s32 accepted, int allow_escape)
{
    clear_calls();
    expect(open_dialog(type)==0,"button dialog opens with host enabled");
    Ps3MsgDialogHostState s=snapshot();
    expect(s.open==1 && s.type==type,"button type visible to host");
    expect(ps3_msgdialog_host_respond(s.generation,CELL_MSGDIALOG_BUTTON_INVALID)==PARAM_ABI,
           "INVALID is never a response");
    expect(ps3_msgdialog_host_respond(s.generation,CELL_MSGDIALOG_BUTTON_NONE)==PARAM_ABI,
           "NONE is never a host response");
    if ((type & 0x30u)==CELL_MSGDIALOG_TYPE_BUTTON_TYPE_OK)
        expect(ps3_msgdialog_host_respond(s.generation,CELL_MSGDIALOG_BUTTON_NO)==PARAM_ABI,
               "OK dialog rejects NO");
    if (allow_escape)
        expect(ps3_msgdialog_host_respond(s.generation+1,CELL_MSGDIALOG_BUTTON_ESCAPE)==NOT_OPEN_ABI,
               "future generation rejected");
    else
        expect(ps3_msgdialog_host_respond(s.generation,CELL_MSGDIALOG_BUTTON_ESCAPE)==PARAM_ABI,
               "disable-cancel rejects ESC");
    expect(snapshot().open==1 && call_count==0,"invalid input has no effect");
    begin();
    fake_now_ms+=20000;
    cellSysutilCheckCallback();
    expect(call_count==1 && snapshot().open==1,"button dialog never auto-responds");
    expect(ps3_msgdialog_host_respond(s.generation,accepted)==0,"allowed button accepted");
    finished(accepted);
}

static void test_stale_abort_and_pending(void)
{
    clear_calls();
    expect(open_dialog(CELL_MSGDIALOG_TYPE_SE_TYPE_NORMAL)==0,"old dialog opens");
    u64 old=snapshot().generation;
    begin();
    expect(cellMsgDialogAbort()==0,"title aborts old dialog");
    expect(call_count==1,"abort remains deferred");
    cellSysutilCheckCallback();
    expect(call_count==2,"abort only emits END");
    check_event(1,0x122);
    clear_calls();
    expect(open_dialog(CELL_MSGDIALOG_TYPE_SE_TYPE_NORMAL)==0,"new dialog opens");
    u64 current=snapshot().generation;
    expect(current>old,"reopen gets new generation");
    expect(ps3_msgdialog_host_respond(old,CELL_MSGDIALOG_BUTTON_ESCAPE)==NOT_OPEN_ABI,
           "old response cannot close replacement");
    expect(snapshot().open==1 && call_count==0,"stale response has no effects");
    begin();
    expect(cellMsgDialogClose(50.0f)==0,"title schedules delayed close");
    cellSysutilCheckCallback();
    expect(call_count==1,"pending close has not completed");
    expect(ps3_msgdialog_host_respond(current,CELL_MSGDIALOG_BUTTON_ESCAPE)==0,
           "actual host input resolves pending close");
    finished(CELL_MSGDIALOG_BUTTON_ESCAPE);
    fake_now_ms+=100;
    cellSysutilCheckCallback();
    expect(call_count==3,"pending deadline cannot complete twice");
}

static void test_no_cancel_notice(void)
{
    clear_calls();
    expect(open_dialog(CELL_MSGDIALOG_TYPE_SE_TYPE_NORMAL |
                       CELL_MSGDIALOG_TYPE_DISABLE_CANCEL_ON)==0,
           "non-cancellable no-button notice opens");
    u64 generation=snapshot().generation;
    begin();
    expect(ps3_msgdialog_host_respond(generation,CELL_MSGDIALOG_BUTTON_ESCAPE)==PARAM_ABI,
           "disable-cancel blocks ESC on no-button notice");
    expect(ps3_msgdialog_host_respond(generation,CELL_MSGDIALOG_BUTTON_OK)==PARAM_ABI,
           "no-button notice rejects OK");
    expect(snapshot().open==1 && call_count==1,"rejected input cannot close notice");
    expect(cellMsgDialogClose(0.0f)==0,"title can close non-cancellable notice");
    finished(CELL_MSGDIALOG_BUTTON_NONE);
}

static void test_progress(uint8_t* memory)
{
    clear_calls();
    expect(open_dialog(CELL_MSGDIALOG_TYPE_SE_TYPE_NORMAL |
                       CELL_MSGDIALOG_TYPE_PROGRESSBAR_DOUBLE)==0,
           "double-progress dialog opens");
    Ps3MsgDialogHostState s=snapshot();
    begin();
    memset(memory+0x300,'P',63); memory[0x33f]=0;
    expect(cellMsgDialogProgressBarSetMsg(0,(const char*)(uintptr_t)0x300)==0,
           "maximum progress message accepted");
    expect(cellMsgDialogProgressBarInc(0,40)==0,"progress increment accepted");
    expect(cellMsgDialogProgressBarInc(0,1000)==0,"progress saturates");
    expect(cellMsgDialogProgressBarSetMsg(1,(const char*)(uintptr_t)0x100)==0,
           "second progress message accepted");
    Ps3MsgDialogHostState p=snapshot();
    expect(p.progress[0].value==100 && strlen(p.progress[0].message)==63 &&
           p.progress[0].message[63]==0,"bounded progress copy and saturation");
    expect(strcmp(p.progress[1].message,"Synthetic notice")==0,
           "second progress bar state copied");
    memset(p.progress[0].message,'X',63);
    memset(memory+0x300,'Y',63);
    expect(snapshot().progress[0].message[0]=='P',"progress snapshot owns copy");
    memset(memory+0x340,'Q',64);
    expect(cellMsgDialogProgressBarSetMsg(0,(const char*)(uintptr_t)0x340)==PARAM_ABI,
           "unterminated 64-byte progress text rejected");
    expect(snapshot().progress[0].message[0]=='P',"invalid progress write is inert");
    expect(ps3_msgdialog_host_respond(s.generation,CELL_MSGDIALOG_BUTTON_ESCAPE)==0,
           "progress notice may be cancelled when allowed");
    finished(CELL_MSGDIALOG_BUTTON_ESCAPE);
}

static void test_shutdown(void)
{
    clear_calls();
    expect(open_dialog(0x21)==0,"host dialog opens before shutdown");
    u64 epoch=0;
    expect(ps3_msgdialog_host_input_state(&epoch)==1 && epoch>0,
           "active dialog captures input with an epoch");
    u64 generation=snapshot().generation;
    begin();
    expect(ps3_msgdialog_host_shutdown()==0,"atomic shutdown succeeds with active dialog");
    Ps3MsgDialogHostState state;
    expect(ps3_msgdialog_host_snapshot(&state)==0 && state.open==0,
           "shutdown revokes host capability and closes notice");
    u64 after=0;
    expect(ps3_msgdialog_host_input_state(&after)==0 && after==epoch,
           "shutdown preserves epoch for held-input release filtering");
    expect(ps3_msgdialog_host_respond(generation,1)==(s32)CELL_ENOSYS,
           "shutdown rejects late host input");
    cellSysutilCheckCallback();
    expect(call_count==2,"shutdown queues END without button completion");
    check_event(1,0x122);
    expect(ps3_msgdialog_host_shutdown()==0,"idle shutdown is idempotent");
    expect(ps3_msgdialog_host_enable(1)==0,"host can start after shutdown");
    expect(open_dialog(0x21)==0,"new host dialog opens after restart");
    expect(snapshot().generation>generation,"restart does not reuse generation");
    expect(ps3_msgdialog_host_shutdown()==0,"restart shutdown succeeds");
    cellSysutilCheckCallback();
}

int main(void)
{
    uint8_t guest_memory[2048]={0};
    vm_base=guest_memory;
    memcpy(guest_memory+0x100,"Synthetic notice",sizeof("Synthetic notice"));
    g_ps3_guest_caller=caller;
    expect(cellSysutilRegisterCallback(0,(CellSysutilCallback)(uintptr_t)SYS_OPD,
        (void*)(uintptr_t)SYS_USER)==0,"register sysutil callback");
    if (setjmp(strict_jump)==0) {
        test_enable_and_copy(guest_memory);
        test_buttons(CELL_MSGDIALOG_TYPE_SE_TYPE_NORMAL |
                     CELL_MSGDIALOG_TYPE_BUTTON_TYPE_YESNO,CELL_MSGDIALOG_BUTTON_YES,1);
        test_buttons(CELL_MSGDIALOG_TYPE_SE_TYPE_NORMAL |
                     CELL_MSGDIALOG_TYPE_BUTTON_TYPE_YESNO |
                     CELL_MSGDIALOG_TYPE_DISABLE_CANCEL_ON,CELL_MSGDIALOG_BUTTON_NO,0);
        test_buttons(CELL_MSGDIALOG_TYPE_SE_TYPE_NORMAL |
                     CELL_MSGDIALOG_TYPE_BUTTON_TYPE_OK,CELL_MSGDIALOG_BUTTON_OK,1);
        test_buttons(CELL_MSGDIALOG_TYPE_SE_TYPE_NORMAL |
                     CELL_MSGDIALOG_TYPE_BUTTON_TYPE_OK |
                     CELL_MSGDIALOG_TYPE_DISABLE_CANCEL_ON,CELL_MSGDIALOG_BUTTON_OK,0);
        test_no_cancel_notice();
        test_stale_abort_and_pending();
        test_progress(guest_memory);
        test_shutdown();
    } else {
        ++errors;
        fprintf(stderr,"FAIL: unexpected strict guard: %s\n",
                strict_category?strict_category:"unknown");
    }
    printf("message dialog host input: %s\n",errors?"FAIL":"PASS");
    return errors?1:0;
}
