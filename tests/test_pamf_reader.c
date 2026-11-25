/*
 * test_pamf_reader.c -- cellPamf reader on guest memory.
 *
 * Getters on a reader that was never initialized answer INVALID_ARG (or 0)
 * instead of dereferencing a NULL PAMF address; cellPamfStreamTypeToEsFilterId
 * writes through its guest EA; cellPamfReaderGetStreamInfo stores the u16/u32
 * fields of the Cell*Info structs big-endian. Run: python tools/test_pamf_reader.py
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"
#include "ppu/ppu_memory.h"
#include "cellPamf.h"

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

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); fails++; } \
} while (0)

static void test_pamf_reader(void)
{
    /* 1. Uninitialized reader must not NULL-dereference pamfAddr. */
    const u32 uninit_r = 0x2600;
    const u32 ts_ea    = 0x2640;
    const u32 nep_ea   = 0x2648;
    const u32 ep_ea    = 0x2650;
    CHECK((u32)cellPamfReaderGetPresentationStartTime((CellPamfReader*)(uintptr_t)uninit_r,
                                                      (u64*)(uintptr_t)ts_ea) == CELL_PAMF_ERROR_INVALID_ARG,
          "Pamf GetPresentationStartTime(uninit) returns INVALID_ARG");
    CHECK((u32)cellPamfReaderGetPresentationEndTime((CellPamfReader*)(uintptr_t)uninit_r,
                                                    (u64*)(uintptr_t)ts_ea) == CELL_PAMF_ERROR_INVALID_ARG,
          "Pamf GetPresentationEndTime(uninit) returns INVALID_ARG");
    CHECK(cellPamfReaderGetMuxRateBound((CellPamfReader*)(uintptr_t)uninit_r) == 0,
          "Pamf GetMuxRateBound(uninit) returns 0");
    CHECK((u32)cellPamfReaderGetNumberOfEp((CellPamfReader*)(uintptr_t)uninit_r,
                                           (s32*)(uintptr_t)nep_ea) == CELL_PAMF_ERROR_INVALID_ARG,
          "Pamf GetNumberOfEp(uninit) returns INVALID_ARG");
    CHECK((u32)cellPamfReaderGetEp((CellPamfReader*)(uintptr_t)uninit_r, 0,
                                   (CellPamfEp*)(uintptr_t)ep_ea) == CELL_PAMF_ERROR_INVALID_ARG,
          "Pamf GetEp(uninit) returns INVALID_ARG");

    /* 2. cellPamfStreamTypeToEsFilterId translates guest EA safely. */
    const u32 flt_ea = 0x2660;
    vm_write32(flt_ea, 0);
    CHECK(cellPamfStreamTypeToEsFilterId(CELL_PAMF_STREAM_TYPE_AVC, 2, (void*)(uintptr_t)flt_ea) == CELL_OK,
          "cellPamfStreamTypeToEsFilterId succeeds with guest EA");
    CHECK(vm_read8(flt_ea + 0) == CELL_PAMF_STREAM_TYPE_AVC && vm_read8(flt_ea + 1) == 2,
          "cellPamfStreamTypeToEsFilterId writes guest memory");

    /* 3. cellPamfReaderGetStreamInfo writes big-endian u16/u32 fields. */
    const u32 pamf_ea = 0x2700;
    const u32 r_ea    = 0x2800;
    const u32 info_ea = 0x2840;
    memset(s_guest_mem + pamf_ea, 0, 0x80);
    vm_write32(pamf_ea + 0x00, CELL_PAMF_MAGIC);
    vm_write32(pamf_ea + 0x04, CELL_PAMF_VERSION);
    vm_write32(pamf_ea + 0x08, 0x80);
    vm_write32(pamf_ea + 0x0C, 0x1000);
    vm_write32(pamf_ea + 0x10, 0x30); /* streamTableOffset */
    vm_write16(pamf_ea + 0x14, 1);    /* numStreams = 1 */
    /* Stream 0: rawType=1 (AVC), 1280x720 */
    vm_write8 (pamf_ea + 0x30 + 0, 1);    /* AVC */
    vm_write8 (pamf_ea + 0x30 + 2, 100);  /* High profile */
    vm_write8 (pamf_ea + 0x30 + 3, 41);   /* level 4.1 */
    vm_write16(pamf_ea + 0x30 + 6, 1);    /* sarWidth */
    vm_write16(pamf_ea + 0x30 + 8, 1);    /* sarHeight */
    vm_write16(pamf_ea + 0x30 + 14, 1280);
    vm_write16(pamf_ea + 0x30 + 16, 720);
    CHECK(cellPamfReaderInitialize((CellPamfReader*)(uintptr_t)r_ea,
                                   (void*)(uintptr_t)pamf_ea, 0x80, 0) == CELL_OK,
          "cellPamfReaderInitialize succeeds");
    CHECK(cellPamfReaderGetStreamInfo((CellPamfReader*)(uintptr_t)r_ea,
                                      (void*)(uintptr_t)info_ea, sizeof(CellPamfAvcInfo)) == CELL_OK,
          "cellPamfReaderGetStreamInfo(AVC) succeeds");
    CHECK(vm_read16(info_ea + 2) == 1280 && vm_read16(info_ea + 4) == 720,
          "CellPamfAvcInfo horizontalSize/verticalSize written big-endian");
}


int main(void)
{
    test_pamf_reader();
    if (fails) {
        fprintf(stderr, "FAILED: %d check(s)\n", fails);
        return 1;
    }
    puts("PASS: cellPamf reader on guest memory");
    return 0;
}
