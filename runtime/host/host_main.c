/*
 * ps3recomp - Host entry (runtime skeleton + end-to-end bridge verification)
 *
 * Ties the real pieces together into one executable:
 *   guest memory (vm_base)  ->  cellGcmInit (FIFO bridge)  ->  D3D12 backend
 *
 * Unlike rsx_harness_main.c (which calls rsx_process_method directly), this
 * drives the *real* path a recompiled game would take:
 *   - lays out a fragment program, vertex data and a texture in guest memory,
 *   - builds a big-endian NV4097 command buffer in IO memory,
 *   - advances the GCM control 'put' pointer,
 *   - calls cellGcmSetFlipCommand(), which makes cellGcmSys drain the FIFO
 *     (gcm_consume_fifo -> rsx_process_command_buffer -> backend) and present.
 *
 * In a real port the hand-built command buffer is replaced by the recompiled
 * game writing cellGcm methods; everything else here is the host it needs.
 *
 * Build (MinGW-w64 UCRT):
 *   gcc -std=c11 -O2 -I../../include -I../../libs/video \
 *       host_main.c ../../libs/video/cellGcmSys.c ../../libs/video/rsx_commands.c \
 *       ../../libs/video/rsx_d3d12_backend.c ../../libs/video/rsx_fp_decompiler.c \
 *       -ld3d12 -ldxgi -ld3dcompiler -ldxguid -lgdi32 -luser32 -o host.exe
 */

#include "cellGcmSys.h"
#include "rsx_commands.h"
#include "rsx_d3d12_backend.h"
#include "ps3emu/guest_call.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Symbols the HLE libs expect the host to provide. */
uint8_t* vm_base = NULL;
ps3_guest_caller_fn g_ps3_guest_caller = NULL;  /* no guest callbacks here */

#define VM_SIZE     (64u * 1024 * 1024)
#define FP_ADDR     0x1000u
#define POS_ADDR    0x2000u
#define COL_ADDR    0x3000u
#define TC_ADDR     0x4000u
#define TEX_ADDR    0x5000u
#define IO_ADDR     0x100000u   /* command buffer base (== s_cmd_buffer_ea) */
#define IO_SIZE     0x200000u
#define CMD_SIZE    0x10000u

/* --- guest data writers (big-endian where the RSX reads BE) ------------- */
static void put_bef(uint32_t off, float f)
{
    uint32_t v; memcpy(&v, &f, 4);
    vm_base[off+0]=(uint8_t)(v>>24); vm_base[off+1]=(uint8_t)(v>>16);
    vm_base[off+2]=(uint8_t)(v>>8);  vm_base[off+3]=(uint8_t)(v);
}
static void put_fp_word(uint32_t off, uint32_t h)
{
    uint32_t be = (h << 16) | (h >> 16);   /* FP half-word swap (see rsx_fp_read_word) */
    vm_base[off+0]=(uint8_t)(be>>24); vm_base[off+1]=(uint8_t)(be>>16);
    vm_base[off+2]=(uint8_t)(be>>8);  vm_base[off+3]=(uint8_t)(be);
}

/* --- big-endian command buffer writer ---------------------------------- */
static uint32_t g_cmd_len;   /* bytes written into the command buffer */
static void cmd_be32(uint32_t v)
{
    uint8_t* p = vm_base + IO_ADDR + g_cmd_len;
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)(v);
    g_cmd_len += 4;
}
static void cmd_method(uint32_t method, uint32_t data)
{
    cmd_be32((1u << 18) | (((method >> 2) & 0x7FF) << 2));  /* type 0, count 1 */
    cmd_be32(data);
}

static void build_guest_memory(void)
{
    /* Fragment program: TEX r0, TC0 (unit 0, END). */
    uint32_t w0 = (0x17u << 24) | (0xFu << 9) | (0x4u << 13) | (0x0u << 17) | 1u;
    uint32_t w1 = 1u | (0u << 9) | (1u << 11) | (2u << 13) | (3u << 15);
    put_fp_word(FP_ADDR + 0, w0); put_fp_word(FP_ADDR + 4, w1);
    put_fp_word(FP_ADDR + 8, 0);  put_fp_word(FP_ADDR + 12, 0);

    /* Positions (clip space), float3 stride 12. */
    put_bef(POS_ADDR+0, 0.0f);  put_bef(POS_ADDR+4, 0.5f);  put_bef(POS_ADDR+8, 0.0f);
    put_bef(POS_ADDR+12,0.5f);  put_bef(POS_ADDR+16,-0.5f); put_bef(POS_ADDR+20,0.0f);
    put_bef(POS_ADDR+24,-0.5f); put_bef(POS_ADDR+28,-0.5f); put_bef(POS_ADDR+32,0.0f);

    /* Colors ubyte4 (unused by the TEX shader but kept enabled). */
    uint8_t cols[12] = { 255,0,0,255, 0,255,0,255, 0,0,255,255 };
    memcpy(vm_base + COL_ADDR, cols, sizeof(cols));

    /* TEXCOORD0 float4 stride 16 = UVs. */
    put_bef(TC_ADDR+0, 0.5f); put_bef(TC_ADDR+4, 0.0f); put_bef(TC_ADDR+8, 0.0f); put_bef(TC_ADDR+12,1.0f);
    put_bef(TC_ADDR+16,1.0f); put_bef(TC_ADDR+20,1.0f); put_bef(TC_ADDR+24,0.0f); put_bef(TC_ADDR+28,1.0f);
    put_bef(TC_ADDR+32,0.0f); put_bef(TC_ADDR+36,1.0f); put_bef(TC_ADDR+40,0.0f); put_bef(TC_ADDR+44,1.0f);

    /* 2x2 A8R8G8B8 texture, bytes in B,G,R,A order: red/green/blue/yellow. */
    uint8_t tex[16] = { 0,0,255,255, 0,255,0,255,  255,0,0,255, 0,255,255,255 };
    memcpy(vm_base + TEX_ADDR, tex, sizeof(tex));
}

static void build_command_buffer(void)
{
    g_cmd_len = 0;
    cmd_method(NV4097_SET_SHADER_PROGRAM, FP_ADDR | 0x1u);

    /* Identity vertex program: one instruction, vec MOV o[0](HPOS).xyzw =
     * v[0](POSITION), END. Hand-encoded NV40 VP (D0..D3, native word values;
     * the FIFO carries them big-endian, the parser swaps back). Exercises the
     * decompiled-VP path (VS + vertex-constant CBV root signature). */
    cmd_method(NV4097_SET_TRANSFORM_PROGRAM_LOAD, 0);
    cmd_method(NV4097_SET_TRANSFORM_PROGRAM, 0x40000000u); /* D0: vec_result */
    cmd_method(NV4097_SET_TRANSFORM_PROGRAM, 0x0040000Du); /* D1: vec MOV, src0h */
    cmd_method(NV4097_SET_TRANSFORM_PROGRAM, 0x80800000u); /* D2: src0l */
    cmd_method(NV4097_SET_TRANSFORM_PROGRAM, 0x0001E001u); /* D3: dst=HPOS, wm=xyzw, end */
    cmd_method(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 0*4, 0x2u | (3u<<4) | (12u<<8));
    cmd_method(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 0*4, POS_ADDR);
    cmd_method(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 3*4, 0x4u | (4u<<4) | (4u<<8));
    cmd_method(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 3*4, COL_ADDR);
    cmd_method(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 8*4, 0x2u | (4u<<4) | (16u<<8));
    cmd_method(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 8*4, TC_ADDR);
    cmd_method(NV4097_SET_TEXTURE_OFFSET     + 0*0x20, TEX_ADDR);
    cmd_method(NV4097_SET_TEXTURE_FORMAT     + 0*0x20, 0x8500u);
    cmd_method(NV4097_SET_TEXTURE_IMAGE_RECT + 0*0x20, (2u<<16) | 2u);
    cmd_method(NV4097_SET_BLEND_ENABLE,       1);
    cmd_method(NV4097_SET_BLEND_FUNC_SFACTOR, 0x0302);
    cmd_method(NV4097_SET_BLEND_FUNC_DFACTOR, 0x0303);
    cmd_method(NV4097_SET_COLOR_CLEAR_VALUE,  0xFF101830);
    cmd_method(NV4097_SET_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    cmd_method(NV4097_DRAW_ARRAYS,   (2u << 24) | 0u);
    cmd_method(NV4097_SET_BEGIN_END, 0);
}

int main(void)
{
    vm_base = (uint8_t*)calloc(1, VM_SIZE);
    if (!vm_base) { printf("vm alloc failed\n"); return 1; }

    if (rsx_d3d12_backend_init(1280, 720, "ps3recomp host (cellGcm bridge)") != 0) {
        printf("backend init failed\n"); return 1;
    }

    cellGcmInit(CMD_SIZE, IO_SIZE, IO_ADDR);
    cellGcmSetDisplayBuffer(0, 0, 1280 * 4, 1280, 720);

    build_guest_memory();
    build_command_buffer();

    CellGcmControl* ctrl = cellGcmGetControlRegister();

    printf("\n[host] cmd buffer = %u bytes at IO 0x%X. Driving via cellGcmSetFlipCommand.\n",
           g_cmd_len, IO_ADDR);
    printf("[host] Expect a textured gradient triangle (red/green/blue/yellow).\n");
    printf("[host] This runs the REAL bridge: FIFO -> gcm_consume_fifo -> backend.\n");
    printf("[host] ESC or close window to quit.\n\n");

    for (;;) {
        if (rsx_d3d12_backend_pump_messages() != 0) break;
        /* Re-point get/put each frame so the bridge re-processes the buffer
         * (backend draw records reset on present). */
        ctrl->get = 0;
        ctrl->put = g_cmd_len;
        cellGcmSetFlipCommand(0);   /* -> gcm_flush_and_present -> bridge + present */
    }

    cellGcmTerminate();
    rsx_d3d12_backend_shutdown();
    free(vm_base);
    return 0;
}
