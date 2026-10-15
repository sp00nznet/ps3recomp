/* CPU-only fixture for the real dialog and sysutil callback queue. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <math.h>

static int errors;
static int fail_allocation_after = -1;
static uint64_t fake_now_ms = 1000;
static jmp_buf strict_jump;
static const char* strict_category;
static int strict_exit;

static void expect(int condition, const char* label)
{
    if (!condition) { ++errors; fprintf(stderr, "FAIL: %s\n", label); }
}

static void* fixture_alloc(size_t size)
{
    if (fail_allocation_after == 0) return NULL;
    if (fail_allocation_after > 0) --fail_allocation_after;
    return malloc(size);
}

/* Trap production strict stops before they terminate this fixture. A real
 * ps3_strict_boot_guard ends the process with code 86. */
#ifndef FOLKLORE_REAL_STRICT_GUARD
#define PS3EMU_STRICT_BOOT_H
static int ps3_strict_boot_enabled(void) { return 1; }
static void ps3_strict_boot_guard(const char* category, const char* format, ...)
{
    (void)format;
    strict_category = category;
    strict_exit = 86;
    longjmp(strict_jump, 1);
}
#endif

/* The sysutil file declares its own external VM store ABI. Rename only those
 * declarations so the dialog can include its real inline PPU memory helpers. */
#define vm_write8 fixture_vm_write8
#define vm_write32 fixture_vm_write32
#define malloc fixture_alloc
#include "../libs/system/cellSysutil.c"
#undef malloc
#undef vm_write8
#undef vm_write32

void fixture_vm_write8(uint64_t addr, uint8_t value) { (void)addr; (void)value; }
void fixture_vm_write32(uint64_t addr, uint32_t value) { (void)addr; (void)value; }
void np_psnr_pump(void) {}
const char* cellGame_get_title_id(void) { return "TEST00001"; }

#include "../runtime/platform/win32_compat.h"
static ULONGLONG fixture_tick(void) { return (ULONGLONG)fake_now_ms; }
#define GetTickCount64 fixture_tick
#include "../libs/system/cellMsgDialog.c"
#undef GetTickCount64

uint8_t* vm_base;
uint32_t ppu_vm_size = 2048;

/* These symbols are used only by PPU memory write helpers, which the dialog
 * itself does not call in the lifecycle tests. They keep the fixture linkable. */
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
int spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t addr) { (void)addr; }
void ps3_ww_report_inline(uint32_t addr, uint64_t value, int width)
{ (void)addr; (void)value; (void)width; }

typedef struct { u32 opd; u64 arg[8]; } Call;
static Call calls[32];
static int call_count;
static void caller(u32 opd, u64 a0, u64 a1, u64 a2, u64 a3,
                   u64 a4, u64 a5, u64 a6, u64 a7)
{
    if (call_count >= 32) { ++errors; return; }
    Call* c = &calls[call_count++];
    c->opd = opd;
    c->arg[0] = a0; c->arg[1] = a1; c->arg[2] = a2;
    c->arg[3] = a3; c->arg[4] = a4; c->arg[5] = a5;
    c->arg[6] = a6; c->arg[7] = a7;
}

#define SYS_OPD 0x1010u
#define SYS_USER 0x2020u
#define DLG_OPD 0x3030u
#define DLG_USER 0x4040u
#define TYPE_NONE 0x00000001u
#define BUSY_ABI ((s32)0x8002B105u)
#define PARAM_ABI ((s32)0x8002B301u)
#define NOT_OPEN_ABI ((s32)0x8002B302u)

static s32 open_none(void)
{
    return cellMsgDialogOpen2(TYPE_NONE, (const char*)(uintptr_t)0x100,
        (CellMsgDialogCallback)(uintptr_t)DLG_OPD,
        (void*)(uintptr_t)DLG_USER, NULL);
}

static void reset_calls(void) { call_count = 0; memset(calls, 0, sizeof(calls)); }

static void expect_event(int index, u32 code)
{
    expect(index < call_count, "event exists");
    if (index < call_count) {
        expect(calls[index].opd == SYS_OPD, "sysutil callback OPD");
        expect(calls[index].arg[0] == code, "sysutil event status and order");
        expect(calls[index].arg[1] == 0 && calls[index].arg[2] == SYS_USER,
               "sysutil event arguments");
    }
}

static void expect_dialog_none(int index)
{
    expect(index < call_count, "dialog callback exists");
    if (index < call_count) {
        expect(calls[index].opd == DLG_OPD, "dialog callback OPD");
        expect(calls[index].arg[0] == UINT64_MAX,
               "BUTTON_NONE is sign extended minus one in guest r3");
        expect(calls[index].arg[1] == DLG_USER, "dialog userdata");
    }
}

static void poll_and_expect_begin(void)
{
    expect(call_count == 0, "Open2 never calls guest synchronously");
    cellSysutilCheckCallback();
    expect(call_count == 1, "drawing begin delivered on poll");
    expect_event(0, 0x121);
}

static void test_open_close(void)
{
    reset_calls(); fake_now_ms = 1000;
    expect(open_none() == 0, "strict no-button Open2 succeeds");
    expect(open_none() == BUSY_ABI, "reopen returns busy without replacing dialog");
    poll_and_expect_begin();
    fake_now_ms += 20000;
    cellSysutilCheckCallback();
    expect(call_count == 1, "no-button dialog ignores auto-dismiss setting");
    expect(cellMsgDialogClose(0.0f) == 0, "zero-delay close accepted");
    expect(call_count == 1, "close callbacks remain deferred");
    cellSysutilCheckCallback();
    expect(call_count == 3, "close delivers drawing end then completion");
    expect_event(1, 0x122);
    expect_dialog_none(2);
    expect(cellMsgDialogClose(0.0f) == NOT_OPEN_ABI, "close without open ABI");
    expect(cellMsgDialogAbort() == 0, "abort absent dialog is harmless");
}

static void test_delayed_close(void)
{
    reset_calls(); fake_now_ms = 30000;
    expect(open_none() == 0, "delayed scenario opens");
    poll_and_expect_begin();
    expect(cellMsgDialogClose(50.0f) == 0, "positive delay accepted");
    cellSysutilCheckCallback();
    expect(call_count == 1, "close waits before deadline");
    fake_now_ms += 49;
    cellSysutilCheckCallback();
    expect(call_count == 1, "close still pending before deadline");
    fake_now_ms += 1;
    cellSysutilCheckCallback();
    expect(call_count == 3, "close completes at deadline");
    expect_event(1, 0x122);
    expect_dialog_none(2);
    cellSysutilCheckCallback();
    expect(call_count == 3, "completion occurs once");

    reset_calls();
    expect(open_none() == 0, "negative delay scenario opens");
    poll_and_expect_begin();
    expect(cellMsgDialogClose(-10.0f) == 0, "negative delay clamps to zero");
    cellSysutilCheckCallback();
    expect(call_count == 3, "negative delay completes on next poll");
    expect_event(1, 0x122);
    expect_dialog_none(2);
}

static void test_abort_and_invalid(void)
{
    reset_calls();
    expect(cellMsgDialogOpen2(0xffffffffu, (const char*)(uintptr_t)0x100,
        NULL, NULL, NULL) == PARAM_ABI, "invalid dialog type ABI");
    expect(cellMsgDialogOpen2(TYPE_NONE, NULL, NULL, NULL, NULL) == PARAM_ABI,
           "missing message ABI");
    expect(open_none() == 0, "abort scenario opens");
    poll_and_expect_begin();
    expect(cellMsgDialogClose(INFINITY) == PARAM_ABI,
           "nonfinite close delay rejected while dialog remains open");
    expect(cellMsgDialogAbort() == 0, "abort accepted");
    expect(call_count == 1, "abort callback remains deferred");
    cellSysutilCheckCallback();
    expect(call_count == 2, "abort delivers only drawing end");
    expect_event(1, 0x122);
    expect(cellMsgDialogAbort() == 0, "repeated abort remains harmless");
}

static void test_strict_user_input_stop(void)
{
    reset_calls(); strict_category = NULL; strict_exit = 0;
    if (setjmp(strict_jump) == 0) {
        (void)cellMsgDialogOpen2(TYPE_NONE | CELL_MSGDIALOG_TYPE_BUTTON_TYPE_YESNO,
            (const char*)(uintptr_t)0x100,
            (CellMsgDialogCallback)(uintptr_t)DLG_OPD, NULL, NULL);
        expect(0, "button dialog must strict-stop without native input UI");
    } else {
        expect(strict_exit == 86, "strict stop uses process exit 86");
        expect(strict_category && strcmp(strict_category,
               "msgdialog-user-input-unimplemented") == 0,
               "strict stop category names unsupported user input");
    }
    expect(call_count == 0, "button dialog cannot auto-answer guest callback");
}

static void test_string_bounds_and_progress(void)
{
    reset_calls();
    uint32_t saved_size=ppu_vm_size;
    ppu_vm_size=0;
    expect(open_none()==0,"zero VM size denotes full guest address space");
    expect(cellMsgDialogAbort()==0,"full VM sentinel scenario aborts");
    cellSysutilCheckCallback();
    ppu_vm_size=saved_size;
    reset_calls();
    expect(cellMsgDialogOpen2(TYPE_NONE,(const char*)(uintptr_t)(saved_size-1),NULL,NULL,NULL)==0,
           "terminated final VM byte is a valid empty message");
    expect(cellMsgDialogAbort()==0,"final-byte scenario aborts");cellSysutilCheckCallback();
    memset(vm_base+0x300,'A',512);
    expect(cellMsgDialogOpen2(TYPE_NONE,(const char*)(uintptr_t)0x300,NULL,NULL,NULL)==PARAM_ABI,
           "unterminated 512 byte message rejected");
    memset(vm_base+0x300,0,512);
    reset_calls();
    expect(cellMsgDialogOpen2(TYPE_NONE|CELL_MSGDIALOG_TYPE_PROGRESSBAR_SINGLE,
        (const char*)(uintptr_t)0x100,NULL,NULL,NULL)==0,"progress dialog opens");
    expect(cellMsgDialogProgressBarSetMsg(0,NULL)==PARAM_ABI,"null progress string rejected");
    memset(vm_base+0x300,'B',64);
    expect(cellMsgDialogProgressBarSetMsg(0,(const char*)(uintptr_t)0x300)==PARAM_ABI,
           "unterminated 64 byte progress string rejected");
    vm_base[0x33f]=0;
    expect(cellMsgDialogProgressBarSetMsg(0,(const char*)(uintptr_t)0x300)==0,
           "63 character progress string accepted");
    expect(cellMsgDialogProgressBarReset(1)==PARAM_ABI,"single bar rejects second index");
    expect(cellMsgDialogProgressBarInc(0,10)==0 && cellMsgDialogProgressBarInc(0,UINT32_MAX)==0,
           "large progress increment accepted and clamped");
    expect(s_progress[0].value==100,"progress addition cannot wrap");
    expect(cellMsgDialogClose(0x1p64f)==PARAM_ABI,"unrepresentable close deadline rejected");
    expect(cellMsgDialogAbort()==0,"progress scenario aborts");cellSysutilCheckCallback();
}

static void test_allocation_stop(int completion)
{
    reset_calls(); strict_category = NULL; strict_exit = 0;
    expect(open_none() == 0, "allocation scenario opens");
    poll_and_expect_begin();
    fail_allocation_after = completion ? 1 : 0;
    if (setjmp(strict_jump) == 0) {
        (void)cellMsgDialogClose(0.0f);
        expect(0, "failed close enqueue must strict-stop");
    } else {
        expect(strict_exit == 86, "allocation strict stop uses exit 86");
        expect(strict_category && strcmp(strict_category,
            completion ? "msgdialog-completion-allocation-failed" :
                         "msgdialog-end-allocation-failed") == 0,
            "allocation strict stop names failed stage");
    }
    fail_allocation_after = -1;
    expect(call_count == 1, "failed enqueue never delivers a synchronous callback");
}

int main(int argc, char** argv)
{
    uint8_t guest_memory[2048] = {0};
    vm_base = guest_memory;
    memcpy(guest_memory + 0x100, "Synthetic notice", sizeof("Synthetic notice"));
    g_ps3_guest_caller = caller;
    expect(cellMsgDialogProgressBarReset(0)==(s32)0x8002B302u,"reset without open dialog");
    expect(cellMsgDialogProgressBarInc(0,1)==(s32)0x8002B302u,"increment without open dialog");
    expect(cellMsgDialogProgressBarSetMsg(0,(const char*)(uintptr_t)0x100)==(s32)0x8002B302u,"valid message without open dialog");
    expect(cellSysutilRegisterCallback(0,
        (CellSysutilCallback)(uintptr_t)SYS_OPD,
        (void*)(uintptr_t)SYS_USER) == 0, "register synthetic sysutil slot");
    if (argc == 2 && strcmp(argv[1], "end-allocation") == 0)
        test_allocation_stop(0);
    else if (argc == 2 && strcmp(argv[1], "completion-allocation") == 0)
        test_allocation_stop(1);
    else if (argc == 1) {
        test_open_close();
        test_delayed_close();
        test_abort_and_invalid();
        test_string_bounds_and_progress();
        test_strict_user_input_stop();
    } else {
        fprintf(stderr, "usage: %s [end-allocation|completion-allocation]\n", argv[0]);
        return 2;
    }
    printf("message dialog lifecycle: %s\n", errors ? "FAIL" : "PASS");
    return errors ? 1 : 0;
}
