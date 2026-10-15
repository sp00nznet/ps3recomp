/* Standalone, CPU-only fixture for the actual strict error-code dialog path.
 * Compile as C17 with Wall/Wextra/Werror and the same include paths as the
 * existing folklore_msgdialog_host fixture. No title assets are used. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

static int errors;
static int missing_presenter_mode;
static unsigned vm_writes;
static uint64_t fake_now_ms = 1000;

static void expect(int condition, const char* reason)
{
    if (!condition) { ++errors; fprintf(stderr, "FAIL: %s\n", reason); }
}

#ifndef FOLKLORE_REAL_STRICT_GUARD
#define PS3EMU_STRICT_BOOT_H
static int ps3_strict_boot_enabled(void) { return 1; }
static void ps3_strict_boot_guard(const char* category, const char* format, ...)
{
    (void)format;
    if (missing_presenter_mode &&
        strcmp(category, "msgdialog-user-input-unimplemented") == 0) exit(86);
    fprintf(stderr, "FAIL: unexpected strict guard: %s\n", category);
    exit(88);
}
#endif

/* Bring in the production queue and dialog implementations, with the same
 * synthetic VM stubs used by the existing host-input fixture. */
#define vm_write8 fixture_vm_write8
#define vm_write32 fixture_vm_write32
#include "../libs/system/cellSysutil.c"
#undef vm_write8
#undef vm_write32
void fixture_vm_write8(uint64_t ea, uint8_t value)
{ (void)ea; (void)value; ++vm_writes; }
void fixture_vm_write32(uint64_t ea, uint32_t value)
{ (void)ea; (void)value; ++vm_writes; }
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
static Call calls[16];
static int call_count;
static void caller(u32 opd, u64 a0, u64 a1, u64 a2, u64 a3,
                   u64 a4, u64 a5, u64 a6, u64 a7)
{
    if (call_count >= 16) { ++errors; return; }
    Call* c = &calls[call_count++];
    c->opd = opd;
    c->arg[0] = a0; c->arg[1] = a1; c->arg[2] = a2; c->arg[3] = a3;
    c->arg[4] = a4; c->arg[5] = a5; c->arg[6] = a6; c->arg[7] = a7;
}

#define SYS_OPD 0x1010u
#define SYS_USER 0x2020u
#define DLG_OPD 0x3030u
#define DLG_USER 0xF1234567u
#define BUSY_ABI ((s32)0x8002B105u)
#define NOT_OPEN_ABI ((s32)0x8002B302u)

static s32 open_error(u32 code)
{
    return cellMsgDialogOpenErrorCode(code,
        (CellMsgDialogCallback)(uintptr_t)DLG_OPD,
        (void*)(uintptr_t)DLG_USER, NULL);
}

static Ps3MsgDialogHostState snapshot(void)
{
    Ps3MsgDialogHostState state;
    memset(&state, 0xA5, sizeof(state));
    expect(ps3_msgdialog_host_snapshot(&state) == 1, "host snapshot available");
    return state;
}

static void clear_calls(void) { call_count = 0; memset(calls, 0, sizeof(calls)); }

static int human_words_present(const char* message)
{
    int letters = 0;
    for (const unsigned char* p = (const unsigned char*)message; *p; ++p)
        if (isalpha(*p)) ++letters;
    return letters >= 8;
}

static void check_begin(void)
{
    expect(call_count == 0, "open defers all guest callbacks");
    cellSysutilCheckCallback();
    expect(call_count == 1, "BEGIN delivered on poll");
    if (call_count >= 1)
        expect(calls[0].opd == SYS_OPD && calls[0].arg[0] == 0x121 &&
               calls[0].arg[1] == 0 && calls[0].arg[2] == SYS_USER,
               "BEGIN callback arguments");
}

static void check_finish(s32 result)
{
    expect(call_count == 1, "completion remains deferred until poll");
    cellSysutilCheckCallback();
    expect(call_count == 3, "END precedes one completion");
    if (call_count >= 3) {
        expect(calls[1].opd == SYS_OPD && calls[1].arg[0] == 0x122 &&
               calls[1].arg[2] == SYS_USER, "END callback precedes completion");
        expect(calls[2].opd == DLG_OPD &&
               calls[2].arg[0] == (u64)(int64_t)result &&
               calls[2].arg[1] == (u64)(uintptr_t)(void*)(uintptr_t)DLG_USER,
               "button result sign extension and userdata preserved");
    }
    cellSysutilCheckCallback();
    expect(call_count == 3, "no duplicate callback on later poll");
}

int main(int argc, char** argv)
{
    if (argc > 2 || (argc == 2 &&
        strcmp(argv[1], "--expect-missing-presenter-guard") != 0)) return 2;
    missing_presenter_mode = argc == 2;
    if (missing_presenter_mode) {
        (void)open_error(0x80010006u);
        fprintf(stderr, "FAIL: strict error dialog opened without presenter\n");
        return 1;
    }

    uint8_t guest_memory[2048];
    memset(guest_memory, 0xA7, sizeof(guest_memory));
    vm_base = guest_memory;
    g_ps3_guest_caller = caller;
    expect(cellSysutilRegisterCallback(0,
        (CellSysutilCallback)(uintptr_t)SYS_OPD,
        (void*)(uintptr_t)SYS_USER) == 0, "register sysutil callback");
    expect(ps3_msgdialog_host_enable(1) == 0, "enable host presenter");

    clear_calls();
    expect(open_error(0x80010006u) == 0, "known error opens");
    Ps3MsgDialogHostState known = snapshot();
    expect(known.open == 1 && known.type ==
        (CELL_MSGDIALOG_TYPE_SE_TYPE_ERROR | CELL_MSGDIALOG_TYPE_BUTTON_TYPE_OK),
        "error dialog has error style and OK button");
    expect(known.message[sizeof(known.message) - 1] == 0,
           "host message is bounded and terminated");
    char known_text[sizeof(known.message) + 1];
    memcpy(known_text, known.message, sizeof(known.message));
    known_text[sizeof(known.message)] = 0;
    expect(strstr(known_text, "80010006") != NULL &&
           human_words_present(known_text),
           "known code and human-readable message copied to host");
    expect(open_error(0xDEADBEEFu) == BUSY_ABI,
           "busy open returns dialog-busy code");
    Ps3MsgDialogHostState busy = snapshot();
    expect(busy.generation == known.generation &&
           memcmp(busy.message, known.message, sizeof(known.message)) == 0,
           "busy open leaves generation and message unchanged");
    check_begin();
    expect(ps3_msgdialog_host_respond(known.generation,
        CELL_MSGDIALOG_BUTTON_NO) == CELL_MSGDIALOG_ERROR_PARAM,
        "error OK dialog rejects NO");
    expect(ps3_msgdialog_host_respond(known.generation,
        CELL_MSGDIALOG_BUTTON_OK) == 0, "one host OK accepted");
    expect(ps3_msgdialog_host_respond(known.generation,
        CELL_MSGDIALOG_BUTTON_OK) == NOT_OPEN_ABI,
        "duplicate OK rejected after closure");
    check_finish(CELL_MSGDIALOG_BUTTON_OK);

    clear_calls();
    expect(open_error(0xDEADBEEFu) == 0, "unknown error opens");
    Ps3MsgDialogHostState unknown = snapshot();
    char unknown_text[sizeof(unknown.message) + 1];
    memcpy(unknown_text, unknown.message, sizeof(unknown.message));
    unknown_text[sizeof(unknown.message)] = 0;
    expect(unknown.open == 1 && strstr(unknown_text, "DEADBEEF") != NULL,
           "unknown error retains exact hexadecimal code");
    check_begin();
    expect(cellMsgDialogClose(0.0f) == 0, "title closes unknown error");
    check_finish(CELL_MSGDIALOG_BUTTON_NONE);

    clear_calls();
    expect(open_error(0x80010006u) == 0, "error reopens for abort test");
    check_begin();
    expect(ps3_msgdialog_host_shutdown() == 0,
           "host shutdown aborts active error dialog");
    expect(call_count == 1, "shutdown defers END");
    cellSysutilCheckCallback();
    expect(call_count == 2 && calls[1].opd == SYS_OPD &&
           calls[1].arg[0] == 0x122,
           "shutdown emits END without button completion");
    cellSysutilCheckCallback();
    expect(call_count == 2, "aborted dialog never completes later");
    Ps3MsgDialogHostState disabled;
    expect(ps3_msgdialog_host_snapshot(&disabled) == 0 && disabled.open == 0,
           "shutdown revokes presenter and clears snapshot");
    for (size_t i = 0; i < sizeof(guest_memory); ++i)
        if (guest_memory[i] != 0xA7) { ++errors; fprintf(stderr,
            "FAIL: error dialog modified guest VM\n"); break; }
    expect(vm_writes == 0, "error dialog never writes guest VM");
    printf("message dialog error path: %s\n", errors ? "FAIL" : "PASS");
    return errors ? 1 : 0;
}
