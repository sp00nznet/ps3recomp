/*
 * test_voice_port_control - cellVoiceResetPort, cellVoiceUpdatePort and
 * cellVoiceGetSignalState.
 *
 * The three were missing, so a title that resets a voice port when a
 * talker stops (or polls the port's signal state) took the unresolved-NID
 * path: CELL_OK with nothing behind it, or a halt under
 * PS3_HLE_UNRESOLVED=halt. The functions are declared weak so a build
 * without them still runs and reports them missing.
 *
 *   cc -std=gnu17 -I include -o test_voice_port_control libs/audio/tests/test_voice_port_control.c
 * Driven by tools/test_voice_port_control.py.
 */
#include "../cellVoice.h"
s32 cellVoiceResetPort(CellVoicePortId) __attribute__((weak));
s32 cellVoiceUpdatePort(CellVoicePortId, const CellVoicePortParam*) __attribute__((weak));
s32 cellVoiceGetSignalState(CellVoicePortId, u32, void*) __attribute__((weak));
#include "../cellVoice.c"
#include <stdlib.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

static int fails = 0;
#define CHECK(c, ...) do { int ok_ = (c); printf(ok_ ? "[PASS] " : "[FAIL] "); printf(__VA_ARGS__); \
                           printf("\n"); if (!ok_) fails++; } while (0)

enum { MEM = 1 << 16, INIT = 0x100, PARAM = 0x200, PORTID = 0x300, UPDATE = 0x400,
       INFO = 0x500, SIGNAL = 0x600 };

static void be16(uint32_t a, uint16_t v) { vm_base[a] = (uint8_t)(v >> 8); vm_base[a + 1] = (uint8_t)v; }

int main(void)
{
    vm_base = calloc(1, MEM);
    ppu_vm_size = MEM;
    if (!cellVoiceResetPort || !cellVoiceUpdatePort || !cellVoiceGetSignalState) {
        CHECK(0, "cellVoiceResetPort/UpdatePort/GetSignalState are defined");
        return 1;
    }

    /* Before cellVoiceInit every call reports LIBVOICE_NOT_INIT. */
    CHECK((u32)cellVoiceResetPort(0) == 0x80310801u, "ResetPort before Init -> NOT_INIT");
    CHECK((u32)cellVoiceUpdatePort(0, (const CellVoicePortParam*)(uintptr_t)UPDATE) == 0x80310801u,
          "UpdatePort before Init -> NOT_INIT");
    CHECK((u32)cellVoiceGetSignalState(0, 11, (void*)(uintptr_t)SIGNAL) == 0x80310801u,
          "GetSignalState before Init -> NOT_INIT");

    CHECK(cellVoiceInit((const CellVoiceInitParam*)(uintptr_t)INIT) == CELL_OK, "Init");

    /* An IN_VOICE port: portType 2, threshold 0x10, bMute 0, volume 1.0f,
     * bitrate 3850, in the real big-endian CellVoicePortParam layout. */
    vm_write32(PARAM + 0, 2);
    be16(PARAM + 4, 0x10); be16(PARAM + 6, 0);
    vm_write32(PARAM + 8, 0x3F800000u);
    vm_write32(PARAM + 12, 3850);
    CHECK(cellVoiceCreatePort((const CellVoicePortParam*)(uintptr_t)PARAM,
                              (CellVoicePortId*)(uintptr_t)PORTID) == CELL_OK, "CreatePort");
    u32 id = vm_read32(PORTID);

    /* Unknown ports are a topology error, as in RPCS3's access_port check. */
    CHECK((u32)cellVoiceResetPort(id + 1) == 0x80310807u, "ResetPort(unknown) -> TOPOLOGY");
    CHECK((u32)cellVoiceUpdatePort(id + 1, (const CellVoicePortParam*)(uintptr_t)UPDATE) == 0x80310807u,
          "UpdatePort(unknown) -> TOPOLOGY");
    CHECK((u32)cellVoiceGetSignalState(id + 1, 11, (void*)(uintptr_t)SIGNAL) == 0x80310807u,
          "GetSignalState(unknown) -> TOPOLOGY");

    CHECK(cellVoiceResetPort(id) == CELL_OK, "ResetPort(created) -> OK");
    CHECK((u32)cellVoiceUpdatePort(id, NULL) == 0x80310805u, "UpdatePort(NULL) -> ARGUMENT_INVALID");

    /* UpdatePort changes threshold, mute, volume and (voice ports) bitrate,
     * never the port type. */
    vm_write32(UPDATE + 0, 0);                 /* a different portType: must be ignored */
    be16(UPDATE + 4, 0x20); be16(UPDATE + 6, 1);
    vm_write32(UPDATE + 8, 0x3F000000u);       /* 0.5f */
    vm_write32(UPDATE + 12, 7300);
    CHECK(cellVoiceUpdatePort(id, (const CellVoicePortParam*)(uintptr_t)UPDATE) == CELL_OK, "UpdatePort -> OK");
    const uint8_t* st = (const uint8_t*)&s_ports[id].param;
    u32 type = ((u32)st[0] << 24) | ((u32)st[1] << 16) | ((u32)st[2] << 8) | st[3];
    u32 vol = ((u32)st[8] << 24) | ((u32)st[9] << 16) | ((u32)st[10] << 8) | st[11];
    u32 rate = ((u32)st[12] << 24) | ((u32)st[13] << 16) | ((u32)st[14] << 8) | st[15];
    CHECK(type == 2, "UpdatePort keeps portType (got %u)", type);
    CHECK(st[4] == 0 && st[5] == 0x20 && st[6] == 0 && st[7] == 1, "UpdatePort sets threshold and bMute");
    CHECK(vol == 0x3F000000u, "UpdatePort sets volume (0x%08X)", vol);
    CHECK(rate == 7300, "UpdatePort sets the voice bitrate (%u)", rate);

    /* No microphone: the signal state reports DEVICE_NOT_PRESENT and leaves
     * the output alone (RPCS3 does the same). */
    vm_write32(SIGNAL, 0xA5A5A5A5u);
    CHECK((u32)cellVoiceGetSignalState(id, 11, (void*)(uintptr_t)SIGNAL) == 0x80310812u,
          "GetSignalState -> DEVICE_NOT_PRESENT");
    CHECK(vm_read32(SIGNAL) == 0xA5A5A5A5u, "GetSignalState leaves attrValue untouched");

    CHECK(cellVoiceDeletePort(id) == CELL_OK, "DeletePort");
    CHECK((u32)cellVoiceResetPort(id) == 0x80310807u, "ResetPort(deleted) -> TOPOLOGY");
    cellVoiceEnd();

    printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
