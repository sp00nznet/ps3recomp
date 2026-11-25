/*
 * test_kb_mouse_guest.c -- cellKb / cellMouse guest buffers.
 *
 * cellKbInit/cellMouseInit accept the SDK's 127-device maximum; a read from a
 * port with no device answers NO_DEVICE and leaves the guest buffer as it
 * was (the console does not zero it); CellMouseData is 6 bytes, so GetData
 * writes exactly +0..+5 and GetDataList packs its entries at stride 6.
 * cellKbRead is the SDK's older name for cellKbGetData.
 * Run: python tools/test_kb_mouse_guest.py
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"
#include "cellKb.h"
#include "cellMouse.h"

static uint8_t s_guest_mem[0x10000];
uint8_t* vm_base = s_guest_mem;

/* cellKb.c and cellMouse.c declare the runtime's out-of-line, big-endian
 * vm_write8/16/32 themselves; this is the guest memory they write. */
void vm_write8(unsigned long long a, unsigned char v) { s_guest_mem[a] = v; }
void vm_write16(unsigned long long a, unsigned short v) { s_guest_mem[a] = (u8)(v >> 8); s_guest_mem[a + 1] = (u8)v; }
void vm_write32(unsigned long long a, unsigned int v)
{
    s_guest_mem[a] = (u8)(v >> 24); s_guest_mem[a + 1] = (u8)(v >> 16);
    s_guest_mem[a + 2] = (u8)(v >> 8); s_guest_mem[a + 3] = (u8)v;
}
static u8 vm_read8(u32 a) { return s_guest_mem[a]; }
static u32 vm_read32(u32 a)
{
    return ((u32)s_guest_mem[a] << 24) | ((u32)s_guest_mem[a + 1] << 16) |
           ((u32)s_guest_mem[a + 2] << 8) | s_guest_mem[a + 3];
}
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(void*)) { (void)nid; (void)name; (void)fn; }

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); fails++; } \
} while (0)

static void test_kb_mouse(void)
{
    /* 1. cellKb: a disconnected keyboard answers NO_DEVICE and leaves the buffer alone. */
    CHECK(cellKbInit(7) == CELL_OK, "cellKbInit(7) succeeds");
    const u32 kb_ea = 0x2000;
    memset(s_guest_mem + kb_ea, 0xA5, 32);
    s32 krc = cellKbRead(1, (CellKbData*)(uintptr_t)kb_ea);
    CHECK(krc == (s32)CELL_KB_ERROR_NO_DEVICE, "cellKbRead(disconnected) == CELL_KB_ERROR_NO_DEVICE");
    CHECK(vm_read32(kb_ea) == 0xA5A5A5A5u && vm_read32(kb_ea + 8) == 0xA5A5A5A5u,
          "cellKbRead(disconnected) leaves guest buffer untouched");
    CHECK(cellKbEnd() == CELL_OK, "cellKbEnd succeeds");

    /* 2. cellMouse: MAX_MICE == 127, 6-byte CellMouseData layout. */
    CHECK(CELL_MOUSE_MAX_MICE == 127, "CELL_MOUSE_MAX_MICE == 127");
    CHECK(cellMouseInit(7) == CELL_OK, "cellMouseInit(7) succeeds");

    const u32 ms_ea = 0x2100;
    memset(s_guest_mem + ms_ea, 0x5A, 16);
    s32 mrc = cellMouseGetData(0, (CellMouseData*)(uintptr_t)ms_ea);
    CHECK(mrc == (s32)CELL_MOUSE_ERROR_NO_DEVICE, "cellMouseGetData(disconnected) == CELL_MOUSE_ERROR_NO_DEVICE");
    CHECK(vm_read32(ms_ea) == 0x5A5A5A5Au && vm_read32(ms_ea + 4) == 0x5A5A5A5Au,
          "cellMouseGetData(disconnected) leaves guest buffer untouched");

    /* Connected mouse: writes exact 6 bytes (+0..+5), leaving +6..+7 (0x52, 0x24) untouched. */
    cellMouse_moveEvent(0, 10, -5);
    cellMouse_buttonEvent(0, CELL_MOUSE_BUTTON_1, 1);
    cellMouse_wheelEvent(0, 2);
    vm_write8(ms_ea + 6, 0x52);
    vm_write8(ms_ea + 7, 0x24);
    CHECK(cellMouseGetData(0, (CellMouseData*)(uintptr_t)ms_ea) == CELL_OK,
          "cellMouseGetData(connected) returns CELL_OK");
    CHECK(vm_read8(ms_ea + 0) == 1, "CellMouseData.update == 1");
    CHECK(vm_read8(ms_ea + 1) == CELL_MOUSE_BUTTON_1, "CellMouseData.buttons == BUTTON_1");
    CHECK((s8)vm_read8(ms_ea + 2) == 10, "CellMouseData.x_axis == 10");
    CHECK((s8)vm_read8(ms_ea + 3) == -5, "CellMouseData.y_axis == -5");
    CHECK((s8)vm_read8(ms_ea + 4) == 2, "CellMouseData.wheel == 2");
    CHECK(vm_read8(ms_ea + 5) == 0, "CellMouseData.tilt == 0");
    CHECK(vm_read8(ms_ea + 6) == 0x52 && vm_read8(ms_ea + 7) == 0x24,
          "cellMouseGetData does NOT overwrite bytes +6..+7 past sizeof(CellMouseData)==6");

    /* Disconnected GetDataList leaves guest buffer untouched. */
    const u32 list_ea = 0x2140;
    vm_write32(list_ea, 0xDEADBEEFu);
    CHECK(cellMouseGetDataList(1, (CellMouseDataList*)(uintptr_t)list_ea) == (s32)CELL_MOUSE_ERROR_NO_DEVICE,
          "cellMouseGetDataList(disconnected) == CELL_MOUSE_ERROR_NO_DEVICE");
    CHECK(vm_read32(list_ea) == 0xDEADBEEFu,
          "cellMouseGetDataList(disconnected) leaves guest buffer untouched");

    /* Connected GetDataList packs entries at stride 6. */
    memset(s_guest_mem + list_ea, 0xEE, 64);
    cellMouse_moveEvent(0, 3, 4);
    CHECK(cellMouseGetDataList(0, (CellMouseDataList*)(uintptr_t)list_ea) == CELL_OK,
          "cellMouseGetDataList(connected) returns CELL_OK");
    CHECK(vm_read32(list_ea) == 1, "CellMouseDataList.list_num == 1");
    CHECK((s8)vm_read8(list_ea + 4 + 2) == 3 && (s8)vm_read8(list_ea + 4 + 3) == 4,
          "CellMouseDataList.list[0] written at +4");
    CHECK(vm_read8(list_ea + 4 + 6) == 0xEE,
          "CellMouseDataList stride 6 leaves byte +10 untouched");
    CHECK(cellMouseEnd() == CELL_OK, "cellMouseEnd succeeds");
}


int main(void)
{
    test_kb_mouse();
    if (fails) {
        fprintf(stderr, "FAILED: %d check(s)\n", fails);
        return 1;
    }
    puts("PASS: cellKb/cellMouse guest buffers");
    return 0;
}
