/*
 * test_rtc_export_sheap.c -- cellRtc tick arithmetic, export/import utility
 * callbacks, and cellSheap block addresses.
 *
 * cellRtcTickAddYears/Months convert through host helpers instead of calling
 * the guest-facing cellRtcSetTick/GetTick on host stack addresses; the
 * Music/Photo/Video export and Photo import Start calls queue their guest
 * callback (an OPD EA) through sysutil rather than calling it as a host
 * function; cellSheapAllocate returns the block's 32-bit guest EA, not a
 * host pointer. Run: python tools/test_rtc_export_sheap.py
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"
#include "ppu/ppu_memory.h"
#include "cellRtc.h"
#include "cellMusicExport.h"
#include "cellPhotoExport.h"
#include "cellPhotoImport.h"
#include "cellVideoExport.h"
#include "cellSheap.h"

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

/* The sysutil callback queue: record what the utilities hand it. */
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

static void test_rtc_export_sheap(void)
{
    /* 1. cellRtc: TickAddYears/Months without guest-entry calls on host stack */
    CHECK(cellRtcGetTick(NULL, NULL) != CELL_OK,
          "cellRtcGetTick(NULL, NULL) fails");
    CHECK(cellRtcTickAddYears(NULL, NULL, 1) != CELL_OK,
          "cellRtcTickAddYears(NULL, NULL, 1) fails");

    const u32 dt_ea   = 0x4100;
    const u32 t0_ea   = 0x4120;
    const u32 t1_ea   = 0x4128;
    const u32 dt2_ea  = 0x4140;
    /* 2024-02-29 12:00:00.000000 UTC -> +1 year clamps to 2025-02-28 */
    vm_write16(dt_ea + 0, 2024);
    vm_write16(dt_ea + 2, 2);
    vm_write16(dt_ea + 4, 29);
    vm_write16(dt_ea + 6, 12);
    vm_write16(dt_ea + 8, 0);
    vm_write16(dt_ea + 10, 0);
    vm_write32(dt_ea + 12, 0);
    CHECK(cellRtcGetTick((const CellRtcDateTime*)(uintptr_t)dt_ea,
                         (CellRtcTick*)(uintptr_t)t0_ea) == CELL_OK,
          "cellRtcGetTick(2024-02-29) succeeds");
    CHECK(cellRtcTickAddYears((CellRtcTick*)(uintptr_t)t1_ea,
                              (const CellRtcTick*)(uintptr_t)t0_ea, 1) == CELL_OK,
          "cellRtcTickAddYears(+1) succeeds");
    CHECK(cellRtcSetTick((CellRtcDateTime*)(uintptr_t)dt2_ea,
                         (const CellRtcTick*)(uintptr_t)t1_ea) == CELL_OK,
          "cellRtcSetTick succeeds");
    CHECK(vm_read16(dt2_ea + 0) == 2025 && vm_read16(dt2_ea + 2) == 2 && vm_read16(dt2_ea + 4) == 28,
          "cellRtcTickAddYears clamps 2024-02-29 + 1y -> 2025-02-28");

    CHECK(cellRtcTickAddMonths((CellRtcTick*)(uintptr_t)t1_ea,
                               (const CellRtcTick*)(uintptr_t)t0_ea, 2) == CELL_OK,
          "cellRtcTickAddMonths(+2) succeeds");
    CHECK(cellRtcSetTick((CellRtcDateTime*)(uintptr_t)dt2_ea,
                         (const CellRtcTick*)(uintptr_t)t1_ea) == CELL_OK,
          "cellRtcSetTick after AddMonths succeeds");
    CHECK(vm_read16(dt2_ea + 0) == 2024 && vm_read16(dt2_ea + 2) == 4 && vm_read16(dt2_ea + 4) == 29,
          "cellRtcTickAddMonths 2024-02-29 + 2m -> 2024-04-29");

    /* 2. Export/Import utilities queue guest OPD callbacks instead of calling host ptrs */
    CHECK(cellMusicExportInit() == CELL_OK, "cellMusicExportInit");
    s_sysutil_cb_opd = 0;
    CHECK(cellMusicExportStart(NULL, (CellMusicExportCallback)(uintptr_t)0x0070A100u,
                               (void*)(uintptr_t)0x1111u) == (s32)CELL_MUSIC_EXPORT_ERROR_NOT_SUPPORTED,
          "cellMusicExportStart returns NOT_SUPPORTED without host crash");
    CHECK(s_sysutil_cb_opd == 0x0070A100u && s_sysutil_cb_args[1] == 0x1111u,
          "cellMusicExportStart queued guest OPD callback");
    CHECK(cellMusicExportEnd() == CELL_OK, "cellMusicExportEnd");

    CHECK(cellPhotoExportInit() == CELL_OK, "cellPhotoExportInit");
    s_sysutil_cb_opd = 0;
    CHECK(cellPhotoExportStart(NULL, (CellPhotoExportCallback)(uintptr_t)0x0070A200u,
                               (void*)(uintptr_t)0x2222u) == (s32)CELL_PHOTO_EXPORT_ERROR_NOT_SUPPORTED,
          "cellPhotoExportStart returns NOT_SUPPORTED without host crash");
    CHECK(s_sysutil_cb_opd == 0x0070A200u && s_sysutil_cb_args[1] == 0x2222u,
          "cellPhotoExportStart queued guest OPD callback");
    CHECK(cellPhotoExportEnd() == CELL_OK, "cellPhotoExportEnd");

    CHECK(cellPhotoImportInit() == CELL_OK, "cellPhotoImportInit");
    s_sysutil_cb_opd = 0;
    CHECK(cellPhotoImportStart((CellPhotoImportCallback)(uintptr_t)0x0070A300u,
                               (void*)(uintptr_t)0x3333u) == (s32)CELL_PHOTO_IMPORT_ERROR_NOT_SUPPORTED,
          "cellPhotoImportStart returns NOT_SUPPORTED without host crash");
    CHECK(s_sysutil_cb_opd == 0x0070A300u && s_sysutil_cb_args[2] == 0x3333u,
          "cellPhotoImportStart queued guest OPD callback");
    CHECK(cellPhotoImportEnd() == CELL_OK, "cellPhotoImportEnd");

    CHECK(cellVideoExportInit() == CELL_OK, "cellVideoExportInit");
    s_sysutil_cb_opd = 0;
    CHECK(cellVideoExportStart(NULL, (CellVideoExportCallback)(uintptr_t)0x0070A400u,
                               (void*)(uintptr_t)0x4444u) == (s32)CELL_VIDEO_EXPORT_ERROR_NOT_SUPPORTED,
          "cellVideoExportStart returns NOT_SUPPORTED without host crash");
    CHECK(s_sysutil_cb_opd == 0x0070A400u && s_sysutil_cb_args[1] == 0x4444u,
          "cellVideoExportStart queued guest OPD callback");
    CHECK(cellVideoExportEnd() == CELL_OK, "cellVideoExportEnd");

    /* 3. cellSheap: 32-bit guest EA return (no host pointer leak) */
    const u32 sattr_ea = 0x5200;
    const u32 shdl_ea  = 0x5240;
    const u32 sheap_ea = 0x5400;
    CHECK((u32)cellSheapInitialize(NULL, (CellSheapHandle*)(uintptr_t)shdl_ea) == (u32)CELL_SHEAP_ERROR_INVAL,
          "cellSheapInitialize(NULL) returns CELL_SHEAP_ERROR_INVAL");
    vm_write32(sattr_ea + 0, sheap_ea);
    vm_write32(sattr_ea + 4, 0x400u);
    vm_write32(sattr_ea + 8, 3u); /* non-power-of-two alignment */
    CHECK((u32)cellSheapInitialize((const CellSheapAttr*)(uintptr_t)sattr_ea,
                                   (CellSheapHandle*)(uintptr_t)shdl_ea) == (u32)CELL_SHEAP_ERROR_ALIGN,
          "cellSheapInitialize(bad align) returns CELL_SHEAP_ERROR_ALIGN");
    vm_write32(sattr_ea + 8, 16u);
    CHECK(cellSheapInitialize((const CellSheapAttr*)(uintptr_t)sattr_ea,
                              (CellSheapHandle*)(uintptr_t)shdl_ea) == CELL_OK,
          "cellSheapInitialize succeeds");
    u32 shdl = vm_read32(shdl_ea);
    void* blk = cellSheapAllocate(shdl, 64u);
    u64 blk_val = (u64)(uintptr_t)blk;
    CHECK(blk_val == (u64)sheap_ea,
          "cellSheapAllocate returns 32-bit guest EA (not 64-bit host pointer)");
    CHECK(cellSheapFree(shdl, blk) == CELL_OK, "cellSheapFree accepts guest EA");
    CHECK(cellSheapFinalize(shdl) == CELL_OK, "cellSheapFinalize succeeds");
}


int main(void)
{
    test_rtc_export_sheap();
    if (fails) {
        fprintf(stderr, "FAILED: %d check(s)\n", fails);
        return 1;
    }
    puts("PASS: cellRtc tick arithmetic, export/import callbacks, cellSheap EAs");
    return 0;
}
