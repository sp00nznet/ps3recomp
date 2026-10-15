/* Standalone CPU fixture for the real sysutil deferred callback queue. */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int fail_allocation_after = -1;
static void* test_alloc(size_t size)
{
    if (fail_allocation_after == 0) return NULL;
    if (fail_allocation_after > 0) --fail_allocation_after;
    return malloc(size);
}
#define malloc test_alloc
#include "../libs/system/cellSysutil.c"
#undef malloc

uint8_t* vm_base;
void vm_write8(uint64_t addr, uint8_t value) { (void)addr; (void)value; }
void vm_write32(uint64_t addr, uint32_t value) { (void)addr; (void)value; }
void cellMsgDialog_tick(void) {}
void np_psnr_pump(void) {}
const char* cellGame_get_title_id(void) { return "TEST00001"; }

typedef struct {
    u32 opd;
    u64 args[8];
} Call;
static Call calls[128];
static int call_count;
static int enqueue_during_callback;
static int reenter_during_callback;
static int errors;

static void expect(int condition, const char* name)
{
    if (!condition) { ++errors; fprintf(stderr, "FAIL: %s\n", name); }
}

static void caller(u32 opd, u64 a0, u64 a1, u64 a2, u64 a3,
                   u64 a4, u64 a5, u64 a6, u64 a7)
{
    if (call_count >= 128) { ++errors; return; }
    Call* call = &calls[call_count++];
    call->opd = opd;
    call->args[0] = a0; call->args[1] = a1; call->args[2] = a2;
    call->args[3] = a3; call->args[4] = a4; call->args[5] = a5;
    call->args[6] = a6; call->args[7] = a7;
    if (enqueue_during_callback) {
        enqueue_during_callback = 0;
        expect(cellSysutilQueueGuestCallback(0x99, 0x1234, 0x5678) == CELL_OK,
               "callback can enqueue later work");
        if (reenter_during_callback) {
            reenter_during_callback = 0;
            cellSysutilCheckCallback();
            expect(call_count == 1, "reentrant poll does not drain");
        }
    }
}

int main(void)
{
    g_ps3_guest_caller = caller;
    expect(cellSysutilSendSystemCommand(0x100000001ULL, 0x200000002ULL) == 0,
           "no registered callbacks");
    expect(cellSysutilRegisterCallback(0, (CellSysutilCallback)(uintptr_t)0x10,
                                      (void*)(uintptr_t)0x100) == CELL_OK, "register slot 0");
    expect(cellSysutilRegisterCallback(2, (CellSysutilCallback)(uintptr_t)0x20,
                                      (void*)(uintptr_t)0x200) == CELL_OK, "register slot 2");
    expect(cellSysutilSendSystemCommand(0x100000001ULL, 0x200000002ULL) == 2,
           "broadcast captures two slots");
    expect(call_count == 0, "enqueue is asynchronous");
    expect(cellSysutilUnregisterCallback(0) == CELL_OK, "unregister after enqueue");
    expect(cellSysutilRegisterCallback(2, (CellSysutilCallback)(uintptr_t)0x21,
                                      (void*)(uintptr_t)0x201) == CELL_OK, "replace after enqueue");
    expect(cellSysutilQueueGuestCallback(0x30, 0x300000003ULL, 7) == CELL_OK,
           "queue completion in order");
    cellSysutilQueueEvent(2, 0x44, 0x55);
    expect(call_count == 0, "all queue types deferred");
    cellSysutilCheckCallback();
    expect(call_count == 4, "four ordered callbacks");
    expect(calls[0].opd == 0x10 && calls[0].args[0] == 0x100000001ULL &&
           calls[0].args[1] == 0x200000002ULL && calls[0].args[2] == 0x100,
           "first slot snapshot and full 64-bit args");
    expect(calls[1].opd == 0x20 && calls[1].args[2] == 0x200,
           "second slot snapshot survives replacement");
    expect(calls[2].opd == 0x30 && calls[2].args[0] == 0x300000003ULL,
           "completion retains global enqueue order");
    expect(calls[3].opd == 0x21 && calls[3].args[0] == 0x44 &&
           calls[3].args[1] == 0x55 && calls[3].args[2] == 0x201,
           "legacy slot helper captures current callback");
    for (int i = 3; i < 8; ++i)
        expect(calls[0].args[i] == 0, "unused sysutil args zero");

    call_count = 0;
    enqueue_during_callback = 1;
    reenter_during_callback = 1;
    expect(cellSysutilQueueGuestCallback(0x40, 1, 2) == CELL_OK,
           "queue callback that enqueues");
    cellSysutilCheckCallback();
    expect(call_count == 1 && calls[0].opd == 0x40,
           "callback-enqueued work waits until next poll");
    cellSysutilCheckCallback();
    expect(call_count == 2 && calls[1].opd == 0x99,
           "next poll drains callback-enqueued work");

    call_count = 0;
    expect(cellSysutilRegisterCallback(0, (CellSysutilCallback)(uintptr_t)0x10,
                                      (void*)(uintptr_t)0x100) == CELL_OK, "restore slot 0");
    fail_allocation_after = 1;
    expect(cellSysutilSendSystemCommand(0xaa, 0xbb) == (s32)CELL_ENOMEM,
           "broadcast allocation failure reported");
    fail_allocation_after = -1;
    cellSysutilCheckCallback();
    expect(call_count == 0, "failed broadcast admits no partial callback");
    expect(cellSysutilSendSystemCommand(0xcc, 0xdd) == 2,
           "queue remains usable after failed admission");
    cellSysutilCheckCallback();
    expect(call_count == 2, "successful broadcast still delivers");
    call_count = 0;
    for (int i = 0; i < 40; ++i) cellSysutilQueueEvent(0, (u32)i, (u32)(i + 1));
    cellSysutilCheckCallback();
    expect(call_count == 40, "queue grows beyond old fixed ring capacity");
    for (int i = 0; i < 40; ++i)
        expect(calls[i].args[0] == (u64)i && calls[i].args[1] == (u64)(i + 1),
               "large queue preserves FIFO order");
    call_count=0;
    expect(cellSysutilSendSystemCommand(CELL_SYSUTIL_DRAWING_END,0)==-1,"END without BEGIN rejected");
    fail_allocation_after=1;
    expect(cellSysutilSendSystemCommand(CELL_SYSUTIL_DRAWING_BEGIN,0)==(s32)CELL_ENOMEM,"failed BEGIN rolls back");
    fail_allocation_after=-1;
    expect(cellSysutilSendSystemCommand(CELL_SYSUTIL_DRAWING_BEGIN,0)==2,"BEGIN remains available after allocation failure");
    expect(cellSysutilSendSystemCommand(CELL_SYSUTIL_DRAWING_BEGIN,0)==(s32)0x8002B105u,"duplicate BEGIN busy");
    expect(cellSysutilSendSystemCommand(CELL_SYSUTIL_DRAWING_END,0)==2,"balanced END accepted");
    expect(cellSysutilSendSystemCommand(CELL_SYSUTIL_DRAWING_END,0)==-1,"duplicate END rejected");
    cellSysutilCheckCallback();
    expect(call_count==4 && calls[0].args[0]==0x121 && calls[2].args[0]==0x122,"only balanced lifecycle events queued");
    printf("sysutil events: %s\n", errors ? "FAIL" : "PASS");
    return errors ? 1 : 0;
}
