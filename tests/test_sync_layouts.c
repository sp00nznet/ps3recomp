/*
 * test_sync_layouts.c -- cellSync guest layouts and byte order.
 *
 * CellSyncBarrier (4 bytes), CellSyncRwm (16 bytes) and the CellSyncLFQueue
 * header are checked word by word against what Sony's own libsre/libsync
 * write for the same calls (recorded under RPCS3 with the firmware modules in
 * LLE): big-endian fields, a guest EA where the queue and Rwm keep their
 * buffer (never a host pointer), and nothing written past the struct end.
 * Run: python tools/test_sync_layouts.py
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"
#include "ppu/ppu_memory.h"
#include "cellSync.h"

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

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); fails++; } \
} while (0)

static void test_lfqueue(void)
{
    const u32 q_ea    = 0x2300;
    const u32 buf_ea  = 0x2400;
    const u32 in_ea   = 0x2500;
    const u32 out_ea  = 0x2540;
    const u32 dir_ea  = 0x2580;
    const u32 dep_ea  = 0x2584;
    const u32 sz_ea   = 0x2588;

    CHECK(cellSyncLFQueueInitialize((CellSyncLFQueue*)(uintptr_t)q_ea,
                                    (void*)(uintptr_t)buf_ea,
                                    16, 4, CELL_SYNC_LFQUEUE_DIR_POP, NULL) == CELL_OK,
          "cellSyncLFQueueInitialize succeeds");

    vm_write32(in_ea, 0x11223344u);
    CHECK(cellSyncLFQueuePush((CellSyncLFQueue*)(uintptr_t)q_ea, (const void*)(uintptr_t)in_ea) == CELL_OK,
          "cellSyncLFQueuePush succeeds");

    /* GetDirection, Depth, and Size must write BIG-ENDIAN guest u32s. */
    CHECK(cellSyncLFQueueGetDirection((const CellSyncLFQueue*)(uintptr_t)q_ea, (u32*)(uintptr_t)dir_ea) == CELL_OK,
          "cellSyncLFQueueGetDirection succeeds");
    CHECK(cellSyncLFQueueDepth((const CellSyncLFQueue*)(uintptr_t)q_ea, (u32*)(uintptr_t)dep_ea) == CELL_OK,
          "cellSyncLFQueueDepth succeeds");
    CHECK(cellSyncLFQueueSize((CellSyncLFQueue*)(uintptr_t)q_ea, (u32*)(uintptr_t)sz_ea) == CELL_OK,
          "cellSyncLFQueueSize succeeds");
    CHECK(vm_read32(dir_ea) == CELL_SYNC_LFQUEUE_DIR_POP, "LFQueueGetDirection writes BE u32");
    CHECK(vm_read32(dep_ea) == 4u, "LFQueueDepth writes BE u32");
    CHECK(vm_read32(sz_ea) == 1u, "LFQueueSize writes BE u32");

    CHECK(cellSyncLFQueuePop((CellSyncLFQueue*)(uintptr_t)q_ea, (void*)(uintptr_t)out_ea) == CELL_OK,
          "cellSyncLFQueuePop succeeds");
    CHECK(vm_read32(out_ea) == 0x11223344u, "LFQueuePop returns pushed payload");

    /* Guest-visible header matches Sony fw 4.93 libsync.sprx: BE size @+0x10,
     * depth @+0x14, u64 buffer EA @+0x18 (never a host pointer), dir @+0x24. */
    CHECK(sizeof(CellSyncLFQueue) == 128, "sizeof(CellSyncLFQueue) == 128");
    CHECK(vm_read32(q_ea + 0x10) == 16u, "LFQueue size is BE u32 @+0x10");
    CHECK(vm_read32(q_ea + 0x14) == 4u, "LFQueue depth is BE u32 @+0x14");
    CHECK(vm_read64(q_ea + 0x18) == (u64)buf_ea, "LFQueue buffer is BE u64 guest EA @+0x18");
    CHECK(vm_read32(q_ea + 0x24) == CELL_SYNC_LFQUEUE_DIR_POP, "LFQueue direction is BE u32 @+0x24");
    CHECK(vm_read32(q_ea + 0x00) == 0 && vm_read32(q_ea + 0x0C) == 0, "LFQueue +0x00..+0x0F zero after init");

    /* Sony: an unaligned queue -> CELL_SYNC_ERROR_ALIGN (0x80410110), NULL -> 0x80410111. */
    CHECK(cellSyncLFQueueInitialize((CellSyncLFQueue*)(uintptr_t)(q_ea + 4), (void*)(uintptr_t)buf_ea,
                                    16, 4, CELL_SYNC_LFQUEUE_DIR_POP, NULL) == (s32)0x80410110,
          "LFQueueInitialize(unaligned) -> CELL_SYNC_ERROR_ALIGN");
    CHECK(cellSyncLFQueueInitialize(NULL, (void*)(uintptr_t)buf_ea, 16, 4, 0, NULL) == (s32)0x80410111,
          "LFQueueInitialize(NULL) -> CELL_SYNC_ERROR_NULL_POINTER");
}

static void test_barrier_rwm(void)
{
    /* 1. CellSyncBarrier: exact 4-byte BE state machine {u16 value, u16 count}
     *    matching Sony libsre.sprx (verified live in RPCS3). */
    const u32 bar_ea = 0x2900;
    CHECK(sizeof(CellSyncBarrier) == 4, "sizeof(CellSyncBarrier) == 4");
    memset(s_guest_mem + bar_ea, 0xAA, 16);

    CHECK(cellSyncBarrierInitialize(NULL, 3) == (s32)CELL_SYNC_ERROR_NULL_POINTER,
          "BarrierInitialize(NULL) -> NULL_POINTER");
    CHECK(cellSyncBarrierInitialize((CellSyncBarrier*)(uintptr_t)(bar_ea + 2), 3) == (s32)CELL_SYNC_ERROR_ALIGN,
          "BarrierInitialize(unaligned) -> ALIGN");
    CHECK(cellSyncBarrierInitialize((CellSyncBarrier*)(uintptr_t)bar_ea, 0) == (s32)CELL_SYNC_ERROR_INVAL,
          "BarrierInitialize(count=0) -> INVAL");

    CHECK(cellSyncBarrierInitialize((CellSyncBarrier*)(uintptr_t)bar_ea, 3) == CELL_OK,
          "BarrierInitialize(3) succeeds");
    CHECK(vm_read32(bar_ea) == 0x00000003u, "Barrier init writes BE 0x00000003");
    CHECK(vm_read32(bar_ea + 4) == 0xAAAAAAAAu && vm_read32(bar_ea + 8) == 0xAAAAAAAAu,
          "BarrierInitialize does not overflow past 4 bytes");

    CHECK(cellSyncBarrierNotify((CellSyncBarrier*)(uintptr_t)bar_ea) == CELL_OK, "BarrierNotify #1");
    CHECK(vm_read32(bar_ea) == 0x00010003u, "Barrier after 1st notify == BE 0x00010003");
    CHECK(cellSyncBarrierTryWait((CellSyncBarrier*)(uintptr_t)bar_ea) == (s32)CELL_SYNC_ERROR_BUSY,
          "BarrierTryWait before count reached -> BUSY");

    CHECK(cellSyncBarrierNotify((CellSyncBarrier*)(uintptr_t)bar_ea) == CELL_OK, "BarrierNotify #2");
    CHECK(cellSyncBarrierNotify((CellSyncBarrier*)(uintptr_t)bar_ea) == CELL_OK, "BarrierNotify #3");
    CHECK(vm_read32(bar_ea) == 0x80030003u, "Barrier after 3rd notify enters drain phase 0x80030003");
    CHECK(cellSyncBarrierTryNotify((CellSyncBarrier*)(uintptr_t)bar_ea) == (s32)CELL_SYNC_ERROR_BUSY,
          "BarrierTryNotify during drain phase -> BUSY");

    CHECK(cellSyncBarrierTryWait((CellSyncBarrier*)(uintptr_t)bar_ea) == CELL_OK, "BarrierTryWait #1");
    CHECK(vm_read32(bar_ea) == 0x80020003u, "Barrier after 1st wait == BE 0x80020003");
    CHECK(cellSyncBarrierTryWait((CellSyncBarrier*)(uintptr_t)bar_ea) == CELL_OK, "BarrierTryWait #2");
    CHECK(cellSyncBarrierTryWait((CellSyncBarrier*)(uintptr_t)bar_ea) == CELL_OK, "BarrierTryWait #3");
    CHECK(vm_read32(bar_ea) == 0x00000003u, "Barrier resets to 0x00000003 after final waiter");

    /* 2. CellSyncRwm: exact 16-byte BE layout {u16 rdr, u16 wrt, u32 size, u64 ea_buf} */
    const u32 rwm_ea = 0x2A00; /* 16-byte aligned */
    const u32 buf_ea = 0x2B00; /* 128-byte aligned */
    const u32 src_ea = 0x2C00;
    const u32 dst_ea = 0x2D00;
    CHECK(sizeof(CellSyncRwm) == 16, "sizeof(CellSyncRwm) == 16");
    memset(s_guest_mem + rwm_ea, 0xAA, 32);

    CHECK(cellSyncRwmInitialize((CellSyncRwm*)(uintptr_t)rwm_ea, (void*)(uintptr_t)buf_ea, 16)
          == (s32)CELL_SYNC_ERROR_INVAL, "RwmInitialize(size=16) rejects non-128 multiple");
    CHECK(cellSyncRwmInitialize((CellSyncRwm*)(uintptr_t)rwm_ea, (void*)(uintptr_t)buf_ea, 128) == CELL_OK,
          "RwmInitialize(size=128) succeeds");
    CHECK(vm_read32(rwm_ea + 0) == 0u, "Rwm ctrl == 0");
    CHECK(vm_read32(rwm_ea + 4) == 128u, "Rwm size == BE 128");
    CHECK(vm_read64(rwm_ea + 8) == (u64)buf_ea, "Rwm buffer EA stored as BE u64 guest EA (no host pointer)");
    CHECK(vm_read32(rwm_ea + 16) == 0xAAAAAAAAu, "RwmInitialize does not overflow past 16 bytes");

    memset(s_guest_mem + src_ea, 0x5A, 128);
    memset(s_guest_mem + dst_ea, 0x00, 128);
    CHECK(cellSyncRwmTryWrite((CellSyncRwm*)(uintptr_t)rwm_ea, (const void*)(uintptr_t)src_ea) == CELL_OK,
          "RwmTryWrite succeeds");
    CHECK(cellSyncRwmTryRead((CellSyncRwm*)(uintptr_t)rwm_ea, (void*)(uintptr_t)dst_ea) == CELL_OK,
          "RwmTryRead succeeds");
    CHECK(s_guest_mem[dst_ea] == 0x5A && s_guest_mem[dst_ea + 127] == 0x5A,
          "Rwm TryWrite -> TryRead roundtrips 128 bytes");
}

int main(void)
{
    test_lfqueue();
    test_barrier_rwm();
    if (fails) {
        fprintf(stderr, "FAILED: %d check(s)\n", fails);
        return 1;
    }
    puts("PASS: cellSync Barrier/Rwm/LFQueue guest layouts");
    return 0;
}
