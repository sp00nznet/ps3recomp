/*
 * test_sync2_daisy_userinfo.c -- cellSync2, cellDaisy and cellUserInfo guest ABI.
 *
 * cellSync2: the four attribute structs are Sony's 128-byte layouts with BE
 * fields (threadTypes/maxWaiters as u16), objects and queues work on guest
 * EAs, and GetValue and Size results are stored big-endian. cellDaisy: pipe handles
 * and counts are BE guest words. cellUserInfo: SelectUser_ListSet hands its
 * guest callback (an OPD EA) to the sysutil queue instead of calling it as a
 * host function pointer. Run: python tools/test_sync2_daisy_userinfo.py
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"
#include "ppu/ppu_memory.h"
#include "cellSync2.h"
#include "cellDaisy.h"
#include "cellUserInfo.h"

static uint8_t s_guest_mem[0x10000];
uint8_t* vm_base = s_guest_mem;

/* Runtime hooks the vm_write* macros reference; none may fire here. */
uint32_t g_ww_lo, g_ww_hi;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; abort(); }
int g_resv_store_active;
void ppu_resv_break_store(uint64_t addr) { (void)addr; abort(); }
int spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
void spu_coh_notify_write(uint32_t addr) { (void)addr; abort(); }
void spu_lockline_lock(void) { abort(); }
void spu_lockline_unlock(void) { abort(); }
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(void*)) { (void)nid; (void)name; (void)fn; }

/* The sysutil callback queue: record what cellUserInfo hands it. */
static u32 s_sysutil_cb_opd = 0;
static u64 s_sysutil_cb_args[8];
s32 cellSysutilQueueGuestCallbackArgs(u32 opd, const u64 args[8]) {
    s_sysutil_cb_opd = opd;
    if (args) memcpy(s_sysutil_cb_args, args, sizeof(s_sysutil_cb_args));
    return 0;
}

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); fails++; } \
} while (0)

static void test_sync2_daisy_userinfo(void)
{
    /* 1. cellSync2: guest EAs, BE attributes, zero host pointers in guest mem. */
    const u32 mattr_ea = 0x3400;
    const u32 mtx_ea   = 0x3480;
    const u32 cattr_ea = 0x3500;
    const u32 cnd_ea   = 0x3580;
    const u32 sattr_ea = 0x3600;
    const u32 sem_ea   = 0x3680;
    const u32 sval_ea  = 0x3984;
    const u32 qattr_ea = 0x3700;
    const u32 q_ea     = 0x3780;
    const u32 qbuf_ea  = 0x3880;
    const u32 qin_ea   = 0x3900;
    const u32 qout_ea  = 0x3940;
    const u32 qsz_ea   = 0x3980;

    CHECK(sizeof(CellSync2MutexAttribute) == 128 &&
          sizeof(CellSync2CondAttribute) == 128 &&
          sizeof(CellSync2SemaphoreAttribute) == 128 &&
          sizeof(CellSync2QueueAttribute) == 128,
          "all 4 CellSync2*Attribute structs are 128 bytes");

    CHECK(cellSync2MutexAttributeInitialize((CellSync2MutexAttribute*)(uintptr_t)mattr_ea) == CELL_OK,
          "cellSync2MutexAttributeInitialize succeeds");
    CHECK(vm_read16(mattr_ea + 4) == 0x010Fu && vm_read16(mattr_ea + 6) == 15,
          "CellSync2MutexAttribute writes BE threadTypes=0x010F and maxWaiters=15");
    CHECK(cellSync2MutexInit((CellSync2Mutex*)(uintptr_t)mtx_ea,
                             (const CellSync2MutexAttribute*)(uintptr_t)mattr_ea) == CELL_OK,
          "cellSync2MutexInit succeeds");
    CHECK(cellSync2MutexTryLock((CellSync2Mutex*)(uintptr_t)mtx_ea) == CELL_OK,
          "cellSync2MutexTryLock succeeds");
    CHECK(cellSync2MutexUnlock((CellSync2Mutex*)(uintptr_t)mtx_ea) == CELL_OK,
          "cellSync2MutexUnlock succeeds");

    CHECK(cellSync2CondAttributeInitialize((CellSync2CondAttribute*)(uintptr_t)cattr_ea) == CELL_OK,
          "cellSync2CondAttributeInitialize succeeds");
    CHECK(vm_read16(cattr_ea + 4) == 15, "CellSync2CondAttribute writes BE maxWaiters=15");
    CHECK(cellSync2CondInit((CellSync2Cond*)(uintptr_t)cnd_ea,
                            (CellSync2Mutex*)(uintptr_t)mtx_ea,
                            (const CellSync2CondAttribute*)(uintptr_t)cattr_ea) == CELL_OK,
          "cellSync2CondInit succeeds");
    CHECK(cellSync2CondSignal((CellSync2Cond*)(uintptr_t)cnd_ea) == CELL_OK,
          "cellSync2CondSignal succeeds");

    CHECK(cellSync2SemaphoreAttributeInitialize((CellSync2SemaphoreAttribute*)(uintptr_t)sattr_ea) == CELL_OK,
          "cellSync2SemaphoreAttributeInitialize succeeds");
    CHECK(vm_read16(sattr_ea + 6) == 1, "CellSync2SemaphoreAttribute writes BE maxWaiters=1");
    CHECK(cellSync2SemaphoreInit((CellSync2Semaphore*)(uintptr_t)sem_ea, 2,
                                 (const CellSync2SemaphoreAttribute*)(uintptr_t)sattr_ea) == CELL_OK,
          "cellSync2SemaphoreInit(2) succeeds");
    CHECK(cellSync2SemaphoreTryAcquire((CellSync2Semaphore*)(uintptr_t)sem_ea, 1) == CELL_OK,
          "cellSync2SemaphoreTryAcquire(1) succeeds");
    CHECK(cellSync2SemaphoreGetValue((const CellSync2Semaphore*)(uintptr_t)sem_ea,
                                     (s32*)(uintptr_t)sval_ea) == CELL_OK,
          "cellSync2SemaphoreGetValue succeeds");
    CHECK((s32)vm_read32(sval_ea) == 1, "cellSync2SemaphoreGetValue writes BE 1");
    CHECK(cellSync2SemaphoreRelease((CellSync2Semaphore*)(uintptr_t)sem_ea, 1) == CELL_OK,
          "cellSync2SemaphoreRelease(1) succeeds");

    CHECK(cellSync2QueueAttributeInitialize((CellSync2QueueAttribute*)(uintptr_t)qattr_ea) == CELL_OK,
          "cellSync2QueueAttributeInitialize succeeds");
    CHECK(vm_read32(qattr_ea + 8) == 16 && vm_read32(qattr_ea + 12) == 1024,
          "CellSync2QueueAttribute writes BE elementSize=16, depth=1024");
    vm_write32(qattr_ea + 12, 4); /* depth = 4 */
    CHECK(cellSync2QueueInit((CellSync2Queue*)(uintptr_t)q_ea, (void*)(uintptr_t)qbuf_ea,
                             16, 4, (const CellSync2QueueAttribute*)(uintptr_t)qattr_ea) == CELL_OK,
          "cellSync2QueueInit succeeds");
    vm_write32(qin_ea, 0xCAFEBABEu);
    CHECK(cellSync2QueueTryPush((CellSync2Queue*)(uintptr_t)q_ea, (const void*)(uintptr_t)qin_ea) == CELL_OK,
          "cellSync2QueueTryPush succeeds");
    CHECK(cellSync2QueueSize((const CellSync2Queue*)(uintptr_t)q_ea, (u32*)(uintptr_t)qsz_ea) == CELL_OK,
          "cellSync2QueueSize succeeds");
    CHECK(vm_read32(qsz_ea) == 1, "cellSync2QueueSize writes BE 1");
    CHECK(cellSync2QueueTryPop((CellSync2Queue*)(uintptr_t)q_ea, (void*)(uintptr_t)qout_ea) == CELL_OK,
          "cellSync2QueueTryPop succeeds");
    CHECK(vm_read32(qout_ea) == 0xCAFEBABEu, "cellSync2QueueTryPop roundtrips payload");

    /* 2. cellDaisy: no double-GUEST_PTR crash, BE slot handles across slots 0 & 1. */
    const u32 dattr_ea = 0x3A00;
    const u32 p0_ea    = 0x3A40;
    const u32 p1_ea    = 0x3A44;
    const u32 din_ea   = 0x3A60;
    const u32 dout_ea  = 0x3A80;
    const u32 dcnt_ea  = 0x3AA0;
    const u32 dfree_ea = 0x3AA4;
    CHECK(cellDaisyPipeAttributeInitialize((CellDaisyPipeAttribute*)(uintptr_t)dattr_ea,
                                           CELL_DAISY_DIRECTION_PPU_TO_SPU, 16, 4) == CELL_OK,
          "cellDaisyPipeAttributeInitialize succeeds");
    CHECK(vm_read32(dattr_ea + 0) == CELL_DAISY_DIRECTION_PPU_TO_SPU &&
          vm_read32(dattr_ea + 4) == 16 && vm_read32(dattr_ea + 8) == 4,
          "CellDaisyPipeAttribute writes BE direction, entrySize=16, depth=4");
    CHECK(cellDaisyCreatePipe(NULL, (CellDaisyPipe*)(uintptr_t)p0_ea,
                              (const CellDaisyPipeAttribute*)(uintptr_t)dattr_ea, NULL, 0) == CELL_OK,
          "cellDaisyCreatePipe slot 0 succeeds");
    CHECK(cellDaisyCreatePipe(NULL, (CellDaisyPipe*)(uintptr_t)p1_ea,
                              (const CellDaisyPipeAttribute*)(uintptr_t)dattr_ea, NULL, 0) == CELL_OK,
          "cellDaisyCreatePipe slot 1 succeeds");
    CHECK(vm_read32(p1_ea) == 1u, "cellDaisyCreatePipe writes BE slot 1");
    vm_write32(din_ea, 0xDEADC0DEu);
    CHECK(cellDaisyPipePush((CellDaisyPipe*)(uintptr_t)p1_ea, (const void*)(uintptr_t)din_ea) == CELL_OK,
          "cellDaisyPipePush on slot 1 succeeds");
    CHECK(cellDaisyPipeGetCount((CellDaisyPipe*)(uintptr_t)p1_ea, (u32*)(uintptr_t)dcnt_ea) == CELL_OK &&
          vm_read32(dcnt_ea) == 1u, "cellDaisyPipeGetCount writes BE 1");
    CHECK(cellDaisyPipeGetFreeCount((CellDaisyPipe*)(uintptr_t)p1_ea, (u32*)(uintptr_t)dfree_ea) == CELL_OK &&
          vm_read32(dfree_ea) == 3u, "cellDaisyPipeGetFreeCount writes BE 3");
    CHECK(cellDaisyPipePop((CellDaisyPipe*)(uintptr_t)p1_ea, (void*)(uintptr_t)dout_ea) == CELL_OK &&
          vm_read32(dout_ea) == 0xDEADC0DEu, "cellDaisyPipePop roundtrips payload");
    CHECK(cellDaisyDestroyPipe((CellDaisyPipe*)(uintptr_t)p0_ea) == CELL_OK, "DestroyPipe slot 0");
    CHECK(cellDaisyDestroyPipe((CellDaisyPipe*)(uintptr_t)p1_ea) == CELL_OK, "DestroyPipe slot 1");

    /* 3. cellUserInfo: SelectUser_ListSet queues guest OPD via sysutil instead of calling host ptr. */
    s_sysutil_cb_opd = 0;
    memset(s_sysutil_cb_args, 0, sizeof(s_sysutil_cb_args));
    CHECK(cellUserInfoSelectUser_ListSet(0, NULL,
                                         (CellUserInfoFinishCallback)(uintptr_t)0x00709990u,
                                         (void*)(uintptr_t)0x12345678u) == CELL_OK,
          "cellUserInfoSelectUser_ListSet succeeds without crashing host");
    CHECK(s_sysutil_cb_opd == 0x00709990u &&
          s_sysutil_cb_args[0] == 0 &&
          s_sysutil_cb_args[1] == CELL_USERINFO_DEFAULT_USER_ID &&
          s_sysutil_cb_args[2] == 0x12345678u,
          "cellUserInfoSelectUser_ListSet queued guest OPD + args via sysutil");
}


int main(void)
{
    test_sync2_daisy_userinfo();
    if (fails) {
        fprintf(stderr, "FAILED: %d check(s)\n", fails);
        return 1;
    }
    puts("PASS: cellSync2/cellDaisy/cellUserInfo guest ABI");
    return 0;
}
