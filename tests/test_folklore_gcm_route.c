/* Synthetic FIFO regression: renderer ownership and flush-before-label order.
 * Uses the real GCM/legacy processor with a recording draw-engine substitute.
 * No game code or GPU is involved. Each invocation starts fresh global state. */
#include "cellGcmSys.h"
#include "rsx_commands.h"
#include "rsx_draw_engine.h"
#include "../runtime/memory/vm.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t* vm_base;
extern uint32_t ppu_vm_size;
extern void cellGcm_rsx_process_fifo(void);
extern volatile u32 g_rsx_last_reference;
static int engine_on, flushes, engine_clears, legacy_clears;
/* What the engine's register file would hold, by NV4097 method, and how many
 * methods arrived from a 2D-engine subchannel (2..6). */
static u32 engine_reg[0x2000 / 4], engine_from_2d;
static u32 fifo_bytes, heap = 0x00400000u;
#define IO_ADDR 0x00100000u
#define CTRL (VM_HLE_INJECT_BASE + 0x2000u)
#define LABEL (VM_HLE_INJECT_BASE + 0x110u)

static u32 read32(u32 address)
{
    const u8* p = vm_base + address;
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}
static void write32(u32 address, u32 value)
{
    u8* p = vm_base + address;
    p[0] = value >> 24; p[1] = value >> 16; p[2] = value >> 8; p[3] = value;
}
static u32 allocate(u32 size, u32 alignment)
{
    heap = (heap + alignment - 1) & ~(alignment - 1);
    u32 result = heap; heap += size; return result;
}
static void emit_on(u32 subch, u32 method, u32 value)
{
    write32(IO_ADDR + fifo_bytes, (1u << 18) | (subch << 13) | method);
    write32(IO_ADDR + fifo_bytes + 4, value);
    fifo_bytes += 8;
}
static void emit(u32 method, u32 value) { emit_on(0, method, value); }
int rsx_draw_engine_enabled(void) { return engine_on; }
void rsx_draw_engine_method(u32 method, u32 value)
{
    if (method == NV4097_CLEAR_SURFACE) engine_clears++;
    if (method >= 0x4000u && method < 0xE000u) engine_from_2d++;
    if (method < 0xE000u) engine_reg[(method & 0x1FFCu) >> 2] = value;
}
void rsx_draw_engine_flush(void)
{
    /* The prior label must still be visible at each submission boundary. */
    static const u32 old[] = {0xABCDEF01u, 0x12785634u, 0x89ABCDEFu, 0x0BADF00Du};
    assert(flushes < 4 && read32(LABEL) == old[flushes]);
    assert(g_rsx_last_reference == 0);
    flushes++;
}
void rsx_draw_engine_set_display_buffer(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f)
{ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; }
void rsx_draw_engine_note_flip(u32 id) { (void)id; }
void rsx_draw_engine_guest_read(u32 a,u32 b,u32 c,u32 d,u32 e)
{ (void)a;(void)b;(void)c;(void)d;(void)e; }
void rsx_draw_engine_guest_wrote(u32 a,u32 b,u32 c,u32 d,u32 e)
{ (void)a;(void)b;(void)c;(void)d;(void)e; }
static void clear_record(void* user,u32 flags,u32 color,float depth,u8 stencil)
{ (void)user;(void)flags;(void)color;(void)depth;(void)stencil;legacy_clears++; }

int main(int argc, char** argv)
{
    assert(argc == 2);
    engine_on = strcmp(argv[1], "legacy") != 0;
    ppu_vm_size = VM_HLE_INJECT_BASE + 0x10000u;
    vm_base = calloc(1, ppu_vm_size); assert(vm_base);
    cellGcmSetupContext(allocate(4,4),0x10000u,0x200000u,IO_ADDR,allocate,write32);
    rsx_backend backend = {0}; backend.clear = clear_record; rsx_set_backend(&backend);
    if (!strcmp(argv[1], "report")) {
        emit(0x1800u, 0x01000000u);
        write32(CTRL, fifo_bytes); cellGcm_rsx_process_fifo();
        return 99; /* strict mode must stop before publishing a fake report */
    }
    if (!strcmp(argv[1], "subchannels")) {
        /* NV4097 alpha test and blend on subchannel 0, then the 2D engines'
         * state words at the same offsets, as Folklore sends them between
         * draws: NV3062 surface (3), NV308A inline-transfer point and sizes
         * (5), NV3089 format, operation, clip and output (6). None of these
         * starts a transfer. The engine must keep the NV4097 values. */
        emit(0x0304u, 1u);                      /* ALPHA_TEST_ENABLE */
        emit(0x0308u, 0x0204u);                 /* ALPHA_FUNC GREATER */
        emit(0x030Cu, 0x80u);                   /* ALPHA_REF */
        emit(0x0310u, 1u);                      /* BLEND_ENABLE */
        emit_on(3, 0x0300u, 0x0000000Bu);       /* NV3062 colour format Y32 */
        emit_on(3, 0x0304u, 0x00100010u);       /* NV3062 pitch */
        emit_on(5, 0x0304u, 0x00000004u);       /* NV308A point */
        emit_on(5, 0x0308u, 0x00010004u);       /* NV308A size out */
        emit_on(5, 0x030Cu, 0x00010004u);       /* NV308A size in */
        emit_on(6, 0x0300u, 0x00000003u);       /* NV3089 colour format */
        emit_on(6, 0x0304u, 0x00000003u);       /* NV3089 operation */
        emit_on(6, 0x0310u, 0x00000000u);       /* NV3089 out point */
        emit_on(1, 0x0320u, 0x80068006u);       /* NV4097 on subchannel 1 */
        write32(CTRL, fifo_bytes); cellGcm_rsx_process_fifo();
        assert(engine_from_2d == 0);
        assert(engine_reg[0x0304u >> 2] == 1u && engine_reg[0x0308u >> 2] == 0x0204u);
        assert(engine_reg[0x030Cu >> 2] == 0x80u && engine_reg[0x0310u >> 2] == 1u);
        assert(engine_reg[0x0320u >> 2] == 0x80068006u);
        free(vm_base); puts("2D-engine subchannels kept out of the NV4097 register file"); return 0;
    }
    emit(NV4097_SET_COLOR_CLEAR_VALUE, 0xFF101830u);
    emit(NV4097_CLEAR_SURFACE, 0xF0u);
    write32(LABEL, 0xABCDEF01u);
    emit(0x1D6Cu, 0x110u);
    emit(0x1D70u, 0x12345678u); /* hardware byte-0/2 swap -> 0x12785634 */
    emit(0x1D74u, 0x89ABCDEFu);
    emit(0x64u, 0x110u);
    emit(0x6Cu, 0x0BADF00Du);
    emit(0x50u, 0x10203040u);
    write32(CTRL, fifo_bytes); cellGcm_rsx_process_fifo();
    assert(read32(LABEL) == 0x0BADF00Du);
    assert(g_rsx_last_reference == 0x10203040u);
    assert(read32(CTRL + 8) == 0x10203040u);
    assert(flushes == (engine_on ? 4 : 0));
    assert(engine_clears == (engine_on ? 1 : 0));
    assert(legacy_clears == (engine_on ? 0 : 1));
    free(vm_base); puts("GCM renderer ownership and fence ordering passed"); return 0;
}
