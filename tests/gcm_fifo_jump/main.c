/*
 * ps3recomp - tests/gcm_fifo_jump
 *
 * JUMP decode in the cellGcm FIFO walker. A small command ring is built in
 * guest memory and drained with cellGcm_rsx_process_fifo; no graphics backend
 * is involved. Each segment ends in a JUMP over a "skipped" stretch that holds
 * a GET_REPORT, and lands on a GET_REPORT of its own. A walker that follows
 * the JUMP writes the landing report and leaves the skipped slot alone.
 *
 *   - old form 0x20000000 | offset;
 *   - new form offset | 1 (a count-0 method header if it is not decoded: the
 *     walker then runs into the skipped stretch).
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

#define IO_ADDR    0x00100000u
#define IO_SIZE    0x00200000u
#define CMD_SIZE   0x00010000u
#define HEAP_BASE  0x00400000u

static void gw32(uint32_t ea, uint32_t v)
{
    uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

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
static void report(uint32_t slot) { emit(0x1800, (1u << 24) | (slot * 16u)); }

static int g_fail;
#define CHECK(c, ...) do { if (c) printf("[PASS] " __VA_ARGS__); \
                           else { g_fail++; printf("[FAIL] " __VA_ARGS__); } printf("\n"); } while (0)

/* Local report slots (the default report context). */
static int written(uint32_t slot)
{
    CellGcmReportData* r = cellGcmGetReportDataAddress(slot);
    return r && r->timestamp != 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const uint32_t vm_size = ppu_hle_inject_base + 0x10000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { printf("[FAIL] guest VM alloc\n"); return 1; }
    ppu_vm_size = vm_size;

    u32 ctx_out = host_alloc(4, 4);
    u32 cdata = cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    uint32_t ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();

    /* Report slots: 1 before the jumps, 2 skipped by the old-form jump, 3 landed on after it, 4 skipped by the
     * new-form jump, 5 landed on after it. */
    g_head = 0;
    report(1);
    {   /* old form: JUMP over one report (3 words) to the next */
        const uint32_t tgt = g_head + 4u + 8u;
        emit_word(0x20000000u | tgt);
        report(2);
        report(3);
    }
    {   /* new form: offset | 1 */
        const uint32_t tgt = g_head + 4u + 8u;
        emit_word(tgt | 1u);
        report(4);
        report(5);
    }
    gw32(cdata + 0x8, IO_ADDR + g_head);
    vm_write32(ctrl + 0, g_head);
    cellGcm_rsx_process_fifo();

    CHECK(vm_read32(ctrl + 4) == g_head, "walker drained the ring (get 0x%X, put 0x%X)",
          vm_read32(ctrl + 4), g_head);
    CHECK(written(1), "report before the jumps written");
    CHECK(!written(2) && written(3), "old-form JUMP: skipped report %s, landing report %s",
          written(2) ? "written" : "not written", written(3) ? "written" : "not written");
    CHECK(!written(4) && written(5), "new-form JUMP: skipped report %s, landing report %s",
          written(4) ? "written" : "not written", written(5) ? "written" : "not written");

    printf("[gcm_fifo_jump] %s (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
    free(vm_base);
    return g_fail ? 1 : 0;
}
