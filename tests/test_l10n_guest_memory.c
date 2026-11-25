/*
 * test_l10n_guest_memory.c -- cellL10n converters on big-endian guest memory.
 *
 * The string converters (UTF8stoUTF16s, UTF8stoUTF32s, UTF32stoUTF8s, ...)
 * take guest EAs for both buffers and for the BE u32 lengths, size-query
 * with dst == NULL, return ConversionOK / DSTExhausted, and write nothing past
 * the converted units; the single-character converters (UTF8toUTF16,
 * UTF16toUTF8, UTF8toUCS2, UCS2toUTF8) and UTF8Len/UTF16Len follow the same
 * ABI. Behaviour is what Sony's libl10n returns for the same calls under
 * RPCS3. Run: python tools/test_l10n_guest_memory.py
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"
#include "ppu/ppu_memory.h"
#include "cellL10n.h"

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

static void test_l10n(void)
{
    const u32 src_ea   = 0x3C00;
    const u32 slen_ea  = 0x3C40;
    const u32 dst_ea   = 0x3C80;
    const u32 dlen_ea  = 0x3CC0;
    const u32 tmp_ea   = 0x3D00;
    const u32 tlen_ea  = 0x3D40;

    /* "A\xE3\x81\x82" = 'A' (U+0041) + hiragana A (U+3042) */
    s_guest_mem[src_ea + 0] = 'A';
    s_guest_mem[src_ea + 1] = 0xE3;
    s_guest_mem[src_ea + 2] = 0x81;
    s_guest_mem[src_ea + 3] = 0x82;

    /* dst == NULL size query */
    vm_write32(slen_ea, 4);
    vm_write32(dlen_ea, 0);
    CHECK(UTF8stoUTF16s((const u8*)(uintptr_t)src_ea, (u32*)(uintptr_t)slen_ea,
                        NULL, (u32*)(uintptr_t)dlen_ea) == CELL_L10N_CONVERSION_OK,
          "UTF8stoUTF16s size query (dst==NULL) returns ConversionOK (0)");
    CHECK(vm_read32(slen_ea) == 4 && vm_read32(dlen_ea) == 2,
          "UTF8stoUTF16s size query writes BE src_len=4, dst_len=2");

    /* Full UTF-8 -> UTF-16BE conversion (no extra NUL past converted units) */
    memset(s_guest_mem + dst_ea, 0xAA, 16);
    vm_write32(slen_ea, 4);
    vm_write32(dlen_ea, 8);
    CHECK(UTF8stoUTF16s((const u8*)(uintptr_t)src_ea, (u32*)(uintptr_t)slen_ea,
                        (u16*)(uintptr_t)dst_ea, (u32*)(uintptr_t)dlen_ea) == CELL_L10N_CONVERSION_OK,
          "UTF8stoUTF16s convert returns ConversionOK");
    CHECK(vm_read16(dst_ea + 0) == 0x0041u && vm_read16(dst_ea + 2) == 0x3042u,
          "UTF8stoUTF16s writes BE 0041 3042");
    CHECK(vm_read16(dst_ea + 4) == 0xAAAAu, "UTF8stoUTF16s leaves bytes past converted units untouched");

    /* Buffer too small -> DSTExhausted == 2 */
    vm_write32(slen_ea, 4);
    vm_write32(dlen_ea, 1);
    CHECK(UTF8stoUTF16s((const u8*)(uintptr_t)src_ea, (u32*)(uintptr_t)slen_ea,
                        (u16*)(uintptr_t)dst_ea, (u32*)(uintptr_t)dlen_ea) == CELL_L10N_DST_EXHAUSTED,
          "UTF8stoUTF16s short dst returns DSTExhausted (2)");

    /* UTF-8 -> UTF-32BE -> UTF-8 roundtrip */
    vm_write32(slen_ea, 4);
    vm_write32(dlen_ea, 8);
    CHECK(UTF8stoUTF32s((const u8*)(uintptr_t)src_ea, (u32*)(uintptr_t)slen_ea,
                        (u32*)(uintptr_t)dst_ea, (u32*)(uintptr_t)dlen_ea) == CELL_L10N_CONVERSION_OK,
          "UTF8stoUTF32s succeeds");
    CHECK(vm_read32(dst_ea + 0) == 0x00000041u && vm_read32(dst_ea + 4) == 0x00003042u,
          "UTF8stoUTF32s writes BE 00000041 00003042");
    vm_write32(dlen_ea, 2);
    vm_write32(tlen_ea, 16);
    CHECK(UTF32stoUTF8s((const u32*)(uintptr_t)dst_ea, (u32*)(uintptr_t)dlen_ea,
                        (u8*)(uintptr_t)tmp_ea, (u32*)(uintptr_t)tlen_ea) == CELL_L10N_CONVERSION_OK,
          "UTF32stoUTF8s succeeds");
    CHECK(vm_read32(tlen_ea) == 4 && memcmp(s_guest_mem + tmp_ea, s_guest_mem + src_ea, 4) == 0,
          "UTF32stoUTF8s roundtrips exact UTF-8 bytes");

    /* Single-character converters (live RPCS3 libl10n.sprx ABI) */
    CHECK(UTF8toUTF16((const u8*)(uintptr_t)(src_ea + 1),
                      (u16*)(uintptr_t)dst_ea, (u32*)(uintptr_t)dlen_ea) == 3,
          "UTF8toUTF16(U+3042) returns 3 UTF-8 bytes consumed");
    CHECK(vm_read16(dst_ea) == 0x3042u && vm_read32(dlen_ea) == 1u,
          "UTF8toUTF16 writes BE 0x3042 and BE len=1");
    CHECK(UTF16toUTF8((const u16*)(uintptr_t)dst_ea,
                      (u8*)(uintptr_t)tmp_ea, (u32*)(uintptr_t)tlen_ea) == 1,
          "UTF16toUTF8(U+3042) returns 1 UTF-16 unit consumed");
    CHECK(vm_read32(tlen_ea) == 3u &&
          s_guest_mem[tmp_ea] == 0xE3 && s_guest_mem[tmp_ea + 1] == 0x81 && s_guest_mem[tmp_ea + 2] == 0x82,
          "UTF16toUTF8 writes E3 81 82 and BE len=3");
    CHECK(UTF8toUCS2((const u8*)(uintptr_t)(src_ea + 1), (u16*)(uintptr_t)dst_ea) == 3 &&
          vm_read16(dst_ea) == 0x3042u, "UTF8toUCS2 returns 3 and writes BE 0x3042");
    memset(s_guest_mem + tmp_ea, 0, 8);
    CHECK(UCS2toUTF8(0x3042u, (u8*)(uintptr_t)tmp_ea) == 3 &&
          s_guest_mem[tmp_ea] == 0xE3 && s_guest_mem[tmp_ea + 1] == 0x81 && s_guest_mem[tmp_ea + 2] == 0x82,
          "UCS2toUTF8(0x3042) returns 3 and writes E3 81 82");
    CHECK(UTF8Len((const u8*)(uintptr_t)(src_ea + 1)) == 3, "UTF8Len(guest EA) == 3");
    CHECK(UTF16Len((const u16*)(uintptr_t)dst_ea) == 1, "UTF16Len(guest EA) == 1");

}

int main(void)
{
    test_l10n();
    if (fails) {
        fprintf(stderr, "FAILED: %d check(s)\n", fails);
        return 1;
    }
    puts("PASS: cellL10n converters on BE guest memory");
    return 0;
}
