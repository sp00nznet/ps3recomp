/*
 * test_imgdec_layouts.c -- cellGifDec / cellJpgDec guest struct layouts.
 *
 * Expected words are what Sony firmware 4.93 libgifdec.sprx / libjpgdec.sprx
 * wrote under stock RPCS3 (a clean-room PPU probe ELF) for the same
 * 4x2 GIF89a and 4x2 grayscale baseline JPEG fed through a BUFFER source:
 *
 *   CellGifDecInfo     04 02 01 01 00 01 00 00            (8 x BE u32)
 *   CellGifDecOutParam u64 16 | 4 | 2 | 4 | bitDepth 8 | cs | useMem
 *   CellJpgDecInfo     04 02 01 01                        (4 x BE u32)
 *   CellJpgDecOutParam u64 16 | 4 | 2 | 4 | mode | cs | downScale | useMem
 *
 * useMemorySpace is implementation-defined (Sony: decoder work memory), so it
 * is not compared. Run: python tools/test_imgdec_layouts.py
 */
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps3emu/ps3types.h"
#include "ppu/ppu_memory.h"
#include "cellGifDec.h"
#include "cellJpgDec.h"

static uint8_t s_guest_mem[0x10000];
uint8_t* vm_base = s_guest_mem;

/* Runtime hooks the vm_write* macros and cellFs path code reference; none of
 * them may fire in this test (no write watch, reservation or SPU coherency). */
uint32_t g_ww_lo, g_ww_hi;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; abort(); }
int g_resv_store_active;
void ppu_resv_break_store(uint64_t addr) { (void)addr; abort(); }
int spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
void spu_coh_notify_write(uint32_t addr) { (void)addr; abort(); }
void spu_lockline_lock(void) { abort(); }
void spu_lockline_unlock(void) { abort(); }
int cellfs_translate_path(const char* ps3_path, char* out, size_t cap) { (void)ps3_path; (void)out; (void)cap; return -1; }

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); fails++; } } while (0)

/* 4x2 GIF89a, global colour table flag set, size bits 0. */
static const u8 k_gif[] = {
    0x47,0x49,0x46,0x38,0x39,0x61, 0x04,0x00, 0x02,0x00, 0x80,0x00,0x00,
    0xFF,0x00,0x00, 0x00,0x00,0x00,
    0x2C, 0x00,0x00, 0x00,0x00, 0x04,0x00, 0x02,0x00, 0x00,
    0x02, 0x02, 0x4C, 0x01, 0x00, 0x3B
};

/* 4x2 single-component baseline JPEG. */
static const u8 k_jpg[] = {
    0xff,0xd8,0xff,0xe0,0x00,0x10,0x4a,0x46,0x49,0x46,0x00,0x01,0x01,0x01,0x00,0x48,0x00,0x48,0x00,0x00,
    0xff,0xdb,0x00,0x43,0x00,0x08,0x06,0x06,0x07,0x06,0x05,0x08,0x07,0x07,0x07,0x09,0x09,0x08,0x0a,0x0c,
    0x14,0x0d,0x0c,0x0b,0x0b,0x0c,0x19,0x12,0x13,0x0f,0x14,0x1d,0x1a,0x1f,0x1e,0x1d,0x1a,0x1c,0x1c,0x20,
    0x24,0x2e,0x27,0x20,0x22,0x2c,0x23,0x1c,0x1c,0x28,0x37,0x29,0x2c,0x30,0x31,0x34,0x34,0x34,0x1f,0x27,
    0x39,0x3d,0x38,0x32,0x3c,0x2e,0x33,0x34,0x32,
    0xff,0xc0,0x00,0x0b,0x08,0x00,0x02,0x00,0x04,0x01,0x01,0x11,0x00,
    0xff,0xc4,0x00,0x14,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x09,
    0xff,0xc4,0x00,0x14,0x10,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0xff,0xda,0x00,0x08,0x01,0x01,0x00,0x00,0x3f,0x00,0xd2,0xcf,0x20,0xff,0xd9
};

enum {
    EA_MAIN = 0x1000, EA_SUB = 0x1008, EA_SRC = 0x1100, EA_OPEN = 0x1180,
    EA_INFO = 0x1200, EA_IN = 0x1300, EA_OUT = 0x1400, EA_DATA = 0x2000
};

/* CellXxxDecSrc guest layout: srcSelect, fileName EA, u64 fileOffset,
 * fileSize, streamPtr EA, streamSize, spuThreadEnable (32 bytes). */
static void put_buffer_src(u32 data_ea, u32 size)
{
    memset(s_guest_mem + EA_SRC, 0xAA, 32);
    vm_write32(EA_SRC + 0x00, 1);          /* BUFFER */
    vm_write32(EA_SRC + 0x04, 0);
    vm_write64(EA_SRC + 0x08, 0);
    vm_write32(EA_SRC + 0x10, 0);
    vm_write32(EA_SRC + 0x14, data_ea);
    vm_write32(EA_SRC + 0x18, size);
    vm_write32(EA_SRC + 0x1C, 0);
}

static void test_gif(void)
{
    memcpy(s_guest_mem + EA_DATA, k_gif, sizeof(k_gif));
    put_buffer_src(EA_DATA, (u32)sizeof(k_gif));

    CHECK(cellGifDecCreate((CellGifDecMainHandle*)(uintptr_t)EA_MAIN, NULL, NULL) == CELL_OK, "gif Create");
    u32 mh = vm_read32(EA_MAIN);
    CHECK(cellGifDecOpen(mh, (CellGifDecSubHandle*)(uintptr_t)EA_SUB,
                         (const CellGifDecSrc*)(uintptr_t)EA_SRC,
                         (CellGifDecOpnInfo*)(uintptr_t)EA_OPEN) == CELL_OK, "gif Open (BUFFER src)");
    u32 sh = vm_read32(EA_SUB);

    memset(s_guest_mem + EA_INFO, 0xAA, 0x40);
    CHECK(cellGifDecReadHeader(mh, sh, (CellGifDecInfo*)(uintptr_t)EA_INFO) == CELL_OK, "gif ReadHeader");
    static const u32 want_info[8] = { 4, 2, 1, 1, 0, 1, 0, 0 };
    for (int i = 0; i < 8; i++) {
        char m[64]; snprintf(m, sizeof m, "CellGifDecInfo word %d matches Sony", i);
        CHECK(vm_read32(EA_INFO + 4u * i) == want_info[i], m);
    }
    CHECK(vm_read32(EA_INFO + 0x20) == 0xAAAAAAAAu, "CellGifDecInfo is exactly 32 bytes");

    /* CellGifDecInParam: commandPtr, colorSpace (ARGB=20 in SDK, any value echoes). */
    vm_write32(EA_IN + 0, 0);
    vm_write32(EA_IN + 4, 10);
    vm_write32(EA_IN + 8, 0xFF000000u);
    memset(s_guest_mem + EA_OUT, 0xAA, 0x40);
    CHECK(cellGifDecSetParameter(mh, sh, (const CellGifDecInParam*)(uintptr_t)EA_IN,
                                 (CellGifDecOutParam*)(uintptr_t)EA_OUT) == CELL_OK, "gif SetParameter");
    CHECK(vm_read64(EA_OUT + 0x00) == 16, "gif outputWidthByte (u64 @+0x00)");
    CHECK(vm_read32(EA_OUT + 0x08) == 4,  "gif outputWidth @+0x08");
    CHECK(vm_read32(EA_OUT + 0x0C) == 2,  "gif outputHeight @+0x0C");
    CHECK(vm_read32(EA_OUT + 0x10) == 4,  "gif outputComponents @+0x10");
    CHECK(vm_read32(EA_OUT + 0x14) == 8,  "gif outputBitDepth @+0x14");
    CHECK(vm_read32(EA_OUT + 0x18) == 10, "gif outputColorSpace @+0x18 echoes inParam");
    CHECK(vm_read32(EA_OUT + 0x20) == 0xAAAAAAAAu, "CellGifDecOutParam is exactly 32 bytes");

    cellGifDecClose(mh, sh);
    cellGifDecDestroy(mh);
}

static void test_jpg(void)
{
    memcpy(s_guest_mem + EA_DATA, k_jpg, sizeof(k_jpg));
    put_buffer_src(EA_DATA, (u32)sizeof(k_jpg));

    CHECK(cellJpgDecCreate((CellJpgDecMainHandle*)(uintptr_t)EA_MAIN, NULL, NULL) == CELL_OK, "jpg Create");
    u32 mh = vm_read32(EA_MAIN);
    CHECK(cellJpgDecOpen(mh, (CellJpgDecSubHandle*)(uintptr_t)EA_SUB,
                         (const CellJpgDecSrc*)(uintptr_t)EA_SRC,
                         (CellJpgDecOpnInfo*)(uintptr_t)EA_OPEN) == CELL_OK, "jpg Open (BUFFER src)");
    u32 sh = vm_read32(EA_SUB);

    memset(s_guest_mem + EA_INFO, 0xAA, 0x40);
    CHECK(cellJpgDecReadHeader(mh, sh, (CellJpgDecInfo*)(uintptr_t)EA_INFO) == CELL_OK, "jpg ReadHeader");
    CHECK(vm_read32(EA_INFO + 0x00) == 4, "CellJpgDecInfo imageWidth");
    CHECK(vm_read32(EA_INFO + 0x04) == 2, "CellJpgDecInfo imageHeight");
    CHECK(vm_read32(EA_INFO + 0x08) == 1, "CellJpgDecInfo numComponents");
    CHECK(vm_read32(EA_INFO + 0x10) == 0xAAAAAAAAu, "CellJpgDecInfo is exactly 16 bytes");

    /* CellJpgDecInParam: commandPtr, downScale, method, outputMode,
     * outputColorSpace, u8 outputColorAlpha + 3 reserved. */
    memset(s_guest_mem + EA_IN, 0, 24);
    vm_write32(EA_IN + 0x04, 1);
    vm_write32(EA_IN + 0x10, CELL_JPGDEC_RGBA);
    vm_write8(EA_IN + 0x14, 0xFF);
    memset(s_guest_mem + EA_OUT, 0xAA, 0x40);
    CHECK(cellJpgDecSetParameter(mh, sh, (const CellJpgDecInParam*)(uintptr_t)EA_IN,
                                 (CellJpgDecOutParam*)(uintptr_t)EA_OUT) == CELL_OK, "jpg SetParameter");
    CHECK(vm_read64(EA_OUT + 0x00) == 16, "jpg outputWidthByte (u64 @+0x00)");
    CHECK(vm_read32(EA_OUT + 0x08) == 4,  "jpg outputWidth @+0x08");
    CHECK(vm_read32(EA_OUT + 0x0C) == 2,  "jpg outputHeight @+0x0C");
    CHECK(vm_read32(EA_OUT + 0x10) == 4,  "jpg outputComponents @+0x10");
    CHECK(vm_read32(EA_OUT + 0x14) == 0,  "jpg outputMode @+0x14");
    CHECK(vm_read32(EA_OUT + 0x18) == CELL_JPGDEC_RGBA, "jpg outputColorSpace @+0x18");
    CHECK(vm_read32(EA_OUT + 0x1C) == 1,  "jpg downScale @+0x1C");
    CHECK(vm_read32(EA_OUT + 0x24) == 0xAAAAAAAAu, "CellJpgDecOutParam ends at +0x24");

    cellJpgDecClose(mh, sh);
    cellJpgDecDestroy(mh);
}

int main(void)
{
    test_gif();
    test_jpg();
    if (fails) {
        fprintf(stderr, "FAILED: %d check(s)\n", fails);
        return 1;
    }
    puts("PASS: cellGifDec/cellJpgDec guest layouts match Sony fw 4.93");
    return 0;
}
