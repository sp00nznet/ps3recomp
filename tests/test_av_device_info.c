/*
 * test_av_device_info.c -- cellVideoOut / cellAudioOut device-info layouts.
 *
 * CellVideoOutDeviceInfo is 0x120 bytes with a 20-byte CellVideoOutColorInfo
 * at +0x0C and availableModes from +0x20; CellAudioOutDeviceInfo is 0x88
 * bytes with a BE u16 latency at +0x06 and 8-byte CellAudioOutSoundMode
 * entries (BE u32 layout at +4) from +0x08. Offsets are what Sony's own
 * modules write for the same calls, recorded under RPCS3 on 0xAA-filled
 * memory. Run: python tools/test_av_device_info.py
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"
#include "ppu/ppu_memory.h"
#include "cellVideoOut.h"
#include "cellAvconfExt.h"

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
/* cellAvconfExt registers its HLE entry points at load; nothing dispatches them here. */
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(void*)) { (void)nid; (void)name; (void)fn; }

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); fails++; } \
} while (0)

static void test_video_out_state(void)
{
    const u32 st_ea = 0x2200;
    CHECK(cellVideoOutGetState(CELL_VIDEO_OUT_PRIMARY, 0, (CellVideoOutState*)(uintptr_t)st_ea) == CELL_OK,
          "cellVideoOutGetState(PRIMARY) succeeds");
    CHECK(vm_read8(st_ea + 0) == 0, "PRIMARY state == ENABLED (0)");
    CHECK(cellVideoOutGetState(CELL_VIDEO_OUT_SECONDARY, 0, (CellVideoOutState*)(uintptr_t)st_ea) == CELL_OK,
          "cellVideoOutGetState(SECONDARY) succeeds");
    CHECK(vm_read8(st_ea + 0) == 1, "SECONDARY state == DISABLED (1)");
}

static void test_device_info_layouts(void)
{
    /* 1. CellVideoOutDeviceInfo (288 bytes = 0x120, colorInfo @+0x0C, availableModes @+0x20) */
    const u32 vinfo_ea = 0x3000;
    CHECK(sizeof(CellVideoOutDeviceInfo) == 0x120, "sizeof(CellVideoOutDeviceInfo) == 0x120");
    memset(s_guest_mem + vinfo_ea, 0xAA, 0x130);
    CHECK(cellVideoOutGetDeviceInfo(CELL_VIDEO_OUT_PRIMARY, 0,
                                    (CellVideoOutDeviceInfo*)(uintptr_t)vinfo_ea) == CELL_OK,
          "cellVideoOutGetDeviceInfo(PRIMARY) succeeds");
    CHECK(vm_read8(vinfo_ea + 0x00) == 1, "VideoOutDeviceInfo.portType == HDMI (1)");
    CHECK(vm_read16(vinfo_ea + 0x02) == 100, "VideoOutDeviceInfo.latency == BE 100");
    CHECK(vm_read8(vinfo_ea + 0x04) == 4, "VideoOutDeviceInfo.availableModeCount == 4");
    CHECK(vm_read8(vinfo_ea + 0x05) == 1, "VideoOutDeviceInfo.state == AVAILABLE (1)");
    CHECK(vm_read32(vinfo_ea + 0x1C) == 100, "VideoOutDeviceInfo.colorInfo.gamma == BE 100 @+0x1C");
    CHECK(vm_read8(vinfo_ea + 0x20) == CELL_VIDEO_OUT_RESOLUTION_480,
          "VideoOutDeviceInfo.availableModes[0] starts at +0x20");
    CHECK(vm_read16(vinfo_ea + 0x26) == 0x0005u,
          "VideoOutDeviceInfo.availableModes[0].refreshRates == BE 0x0005");

    /* 2. CellAudioOutDeviceInfo (136 bytes = 0x88, latency @+0x06, 8-byte modes @+0x08) */
    const u32 ainfo_ea = 0x3200;
    CHECK(sizeof(CellAudioOutSoundMode) == 8, "sizeof(CellAudioOutSoundMode) == 8");
    CHECK(sizeof(CellAudioOutDeviceInfo) == 0x88, "sizeof(CellAudioOutDeviceInfo) == 0x88");
    memset(s_guest_mem + ainfo_ea, 0xAA, 0x90);
    CHECK(cellAudioOutGetDeviceInfo(CELL_AUDIO_OUT_PRIMARY, 0,
                                    (CellAudioOutDeviceInfo*)(uintptr_t)ainfo_ea) == CELL_OK,
          "cellAudioOutGetDeviceInfo(PRIMARY) succeeds");
    CHECK(vm_read8(ainfo_ea + 0x01) == 3, "AudioOutDeviceInfo.availableModeCount == 3");
    CHECK(vm_read8(ainfo_ea + 0x02) == 1, "AudioOutDeviceInfo.state == AVAILABLE (1)");
    CHECK(vm_read16(ainfo_ea + 0x06) == 13, "AudioOutDeviceInfo.latency == BE 13 @+0x06");
    CHECK(vm_read8(ainfo_ea + 0x08) == CELL_AUDIO_OUT_CODING_TYPE_LPCM &&
          vm_read8(ainfo_ea + 0x09) == CELL_AUDIO_OUT_CHNUM_2 &&
          vm_read32(ainfo_ea + 0x0C) == CELL_AUDIO_OUT_SPEAKER_LAYOUT_2CH,
          "AudioOutDeviceInfo.availableModes[0] @+0x08 with BE layout @+0x0C");
    CHECK(vm_read8(ainfo_ea + 0x11) == CELL_AUDIO_OUT_CHNUM_6 &&
          vm_read32(ainfo_ea + 0x14) == CELL_AUDIO_OUT_SPEAKER_LAYOUT_6CH_LREClr,
          "AudioOutDeviceInfo.availableModes[1] @+0x10 with BE 6ch layout @+0x14");
    CHECK(vm_read8(ainfo_ea + 0x19) == CELL_AUDIO_OUT_CHNUM_8 &&
          vm_read32(ainfo_ea + 0x1C) == CELL_AUDIO_OUT_SPEAKER_LAYOUT_8CH_LREClrxy,
          "AudioOutDeviceInfo.availableModes[2] @+0x18 with BE 8ch layout @+0x1C");
}

int main(void)
{
    test_video_out_state();
    test_device_info_layouts();
    if (fails) {
        fprintf(stderr, "FAILED: %d check(s)\n", fails);
        return 1;
    }
    puts("PASS: cellVideoOut/cellAudioOut device-info layouts");
    return 0;
}
