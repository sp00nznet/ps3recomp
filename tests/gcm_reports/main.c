/*
 * ps3recomp - tests/gcm_reports
 *
 * NV4097_SET_CONTEXT_DMA_REPORT (0x1A8) and NV4097_GET_REPORT (0x1800) through
 * the cellGcm FIFO walker. A small command ring is built in guest memory and
 * drained once with cellGcm_rsx_process_fifo; no graphics backend is involved.
 *
 *   - default context (0xFEED0000, local): the report lands in the host report
 *     area (cellGcmGetReportDataAddress); guest memory at that offset is not
 *     touched;
 *   - 0xFEED0001 (main memory): the report is 16 bytes, big-endian, at the
 *     guest address the IO offset maps to: u64 timestamp, u32 value (ZPASS,
 *     type 1, reads 0xFFFF "visible"; other types 0), u32 0;
 *   - timestamps rise strictly from one report to the next;
 *   - an offset with no IO mapping is skipped, and switching back to
 *     0xFEED0000 restores the local path.
 *
 * Exit code 0 when every check passes.
 */
#include "cellGcmSys.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void cellGcm_rsx_process_fifo(void);
extern void vm_write32(uint64_t addr, uint32_t v);
extern uint32_t vm_read32(uint64_t addr);
extern uint32_t ppu_vm_size;
extern uint32_t ppu_hle_inject_base;

uint8_t* vm_base = NULL;

#define IO_ADDR    0x00100000u      /* command buffer + IO window, io 0 */
#define IO_SIZE    0x00200000u
#define CMD_SIZE   0x00010000u
#define HEAP_BASE  0x00400000u

static void gw32(uint32_t ea, uint32_t v)
{
    uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint32_t gr32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t gr64(uint32_t ea) { return ((uint64_t)gr32(ea) << 32) | gr32(ea + 4); }

static uint32_t g_heap = HEAP_BASE;
static u32 host_alloc(u32 size, u32 align)
{
    if (!align) align = 4;
    g_heap = (g_heap + align - 1u) & ~(align - 1u);
    u32 r = g_heap;
    g_heap += size;
    return r;
}
static void host_w32(u32 ea, u32 v) { gw32(ea, v); }

static uint32_t g_head;
static void emit_word(uint32_t v) { gw32(IO_ADDR + g_head, v); g_head += 4; }
static void emit(uint32_t m, uint32_t d) { emit_word((1u << 18) | (m & 0x1FFCu)); emit_word(d); }

static int g_fail;
#define CHECK(c, ...) do { if (c) printf("[PASS] " __VA_ARGS__); \
                           else { g_fail++; printf("[FAIL] " __VA_ARGS__); } printf("\n"); } while (0)

#define SENTINEL   0xA5A5A5A5u
#define LOCAL_OFF  0x07F0u          /* local report: host slot 0x7F */
#define MAIN_OFF1  0x50390u         /* main-memory reports, inside the IO window */
#define MAIN_OFF2  0x4D2A0u
#define MAIN_OFF3  0x4D2B0u         /* type 2: value 0 */
#define BAD_OFF    0xF00000u        /* IO page 15: not mapped (IO_SIZE is 2 MB) */
#define LOCAL_OFF2 0x0800u

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    /* Up to the HLE inject window, where cellGcmSetupContext puts the control
     * register and the offset tables; calloc is lazy, so this costs address
     * space rather than memory. */
    const uint32_t vm_size = ppu_hle_inject_base + 0x10000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { printf("[FAIL] guest VM alloc\n"); return 1; }
    ppu_vm_size = vm_size;

    u32 ctx_out = host_alloc(4, 4);
    u32 cdata = cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    uint32_t ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();

    const uint32_t offs[] = { LOCAL_OFF, MAIN_OFF1, MAIN_OFF2, MAIN_OFF3, LOCAL_OFF2, MAIN_OFF1 + 0x10 };
    for (unsigned i = 0; i < sizeof offs / sizeof offs[0]; i++)
        for (unsigned k = 0; k < 16; k += 4) gw32(IO_ADDR + offs[i] + k, SENTINEL);

    g_head = 0;
    emit(0x1800, (1u << 24) | LOCAL_OFF);               /* default context: local */
    emit(0x01A8, 0xFEED0001u);                           /* SET_CONTEXT_DMA_REPORT: main */
    emit(0x1800, (1u << 24) | MAIN_OFF1);
    emit(0x1800, (1u << 24) | MAIN_OFF2);
    emit(0x1800, (2u << 24) | MAIN_OFF3);
    emit(0x1800, (1u << 24) | BAD_OFF);
    emit(0x1800, (1u << 24) | (MAIN_OFF1 + 0x10));
    emit(0x01A8, 0xFEED0000u);                           /* back to local */
    emit(0x1800, (1u << 24) | LOCAL_OFF2);
    gw32(cdata + 0x8, IO_ADDR + g_head);                 /* context current */
    vm_write32(ctrl + 0, g_head);                        /* put */
    cellGcm_rsx_process_fifo();
    CHECK(vm_read32(ctrl + 4) == g_head, "walker drained the ring (get 0x%X, put 0x%X)",
          vm_read32(ctrl + 4), g_head);

    CellGcmReportData* r = cellGcmGetReportDataAddress(LOCAL_OFF / 16u);
    CHECK(r && r->value == 0xFFFFu && r->timestamp != 0, "local report: host slot 0x%X value 0x%X",
          LOCAL_OFF / 16u, r ? r->value : 0);
    CHECK(gr32(IO_ADDR + LOCAL_OFF) == SENTINEL && gr32(IO_ADDR + LOCAL_OFF + 8) == SENTINEL,
          "local report: guest memory at IO 0x%X untouched", LOCAL_OFF);

    uint64_t t1 = gr64(IO_ADDR + MAIN_OFF1), t2 = gr64(IO_ADDR + MAIN_OFF2), t3 = gr64(IO_ADDR + MAIN_OFF3);
    uint64_t t4 = gr64(IO_ADDR + MAIN_OFF1 + 0x10);
    const uint64_t untouched = (uint64_t)SENTINEL << 32 | SENTINEL;
    CHECK(gr32(IO_ADDR + MAIN_OFF1 + 8) == 0xFFFFu && gr32(IO_ADDR + MAIN_OFF1 + 12) == 0u &&
          t1 != 0 && t1 != untouched,
          "main report at IO 0x%X: value 0x%X pad 0x%X", MAIN_OFF1,
          gr32(IO_ADDR + MAIN_OFF1 + 8), gr32(IO_ADDR + MAIN_OFF1 + 12));
    CHECK(gr32(IO_ADDR + MAIN_OFF2 + 8) == 0xFFFFu && gr32(IO_ADDR + MAIN_OFF2 + 12) == 0u,
          "main report at IO 0x%X: value 0x%X (ZPASS visible)", MAIN_OFF2, gr32(IO_ADDR + MAIN_OFF2 + 8));
    CHECK(gr32(IO_ADDR + MAIN_OFF3 + 8) == 0u && gr32(IO_ADDR + MAIN_OFF3 + 12) == 0u,
          "main report at IO 0x%X: type 2 value 0x%X", MAIN_OFF3, gr32(IO_ADDR + MAIN_OFF3 + 8));
    CHECK(t1 != untouched && t1 < t2 && t2 < t3 && t3 < t4,
          "timestamps strictly rising: %llu < %llu < %llu < %llu",
          (unsigned long long)t1, (unsigned long long)t2, (unsigned long long)t3, (unsigned long long)t4);
    CHECK(cellGcmGetReportDataAddress(MAIN_OFF1 / 16u) == NULL ||
          cellGcmGetReportDataAddress(MAIN_OFF1 / 16u)->timestamp == 0,
          "main report left the host report area alone");
    r = cellGcmGetReportDataAddress(LOCAL_OFF2 / 16u);
    CHECK(r && r->value == 0xFFFFu && r->timestamp > t4 && gr32(IO_ADDR + LOCAL_OFF2) == SENTINEL,
          "back on 0xFEED0000: host slot 0x%X written after the main reports, guest untouched",
          LOCAL_OFF2 / 16u);

    printf("[gcm_reports] %s (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
    free(vm_base);
    return g_fail ? 1 : 0;
}
