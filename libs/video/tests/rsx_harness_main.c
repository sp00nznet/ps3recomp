/*
 * ps3recomp - RSX / D3D12 GPU-path harness
 *
 * Manual / visual verification for the D3D12 backend: the PSO cache (blend),
 * and the NV40 fragment-program decompiler integration. It bypasses the
 * (not-yet-wired) PPU/SPU + cellGcm stack and drives the RSX command processor
 * directly with hand-issued NV4097 methods over a fake guest memory.
 *
 * What it renders: one triangle textured from a 2x2 texture (red / green /
 * blue / yellow corners), sampled with TEXCOORD0 as UV. The fragment program
 * is `TEX r0, TC0` on texture unit 0. If texture upload + sampling works you
 * see a smooth multi-color gradient filling the triangle; if the upload
 * failed you'd see black (the null SRV samples 0). Verifies increment (c):
 * texture upload + binding + sampling.
 *
 * Build (MinGW-w64 UCRT):
 *   gcc -std=c11 -O2 -I../../../include -I.. \
 *       rsx_harness_main.c ../rsx_d3d12_backend.c ../rsx_commands.c \
 *       ../rsx_fp_decompiler.c \
 *       -ld3d12 -ldxgi -ld3dcompiler -lgdi32 -luser32 -o rsx_harness.exe
 */

#include "rsx_commands.h"
#include "rsx_d3d12_backend.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The backend reads guest memory through this. We own it here (no vm.c). */
uint8_t* vm_base = NULL;

#define VM_SIZE     (32u * 1024 * 1024)
#define FP_ADDR     0x1000u    /* fragment program */
#define POS_ADDR    0x2000u    /* vertex positions (BE float3) */
#define COL_ADDR    0x3000u    /* vertex colors (ubyte4) */
#define TC_ADDR     0x4000u    /* vertex texcoord0 (BE float4) */
#define TEX_ADDR    0x5000u    /* texture pixels (A8R8G8B8, B,G,R,A bytes) */

/* --- little helpers to lay out guest data ------------------------------- */

/* Store a fragment-program word so the backend's BE-load + half-word-swap
 * recovers host word `h` (see rsx_fp_read_word). */
static void put_fp_word(uint32_t off, uint32_t h)
{
    uint32_t be = (h << 16) | (h >> 16);
    vm_base[off+0] = (uint8_t)(be >> 24); vm_base[off+1] = (uint8_t)(be >> 16);
    vm_base[off+2] = (uint8_t)(be >> 8);  vm_base[off+3] = (uint8_t)(be);
}

/* Store a big-endian float (vertex attribs are read via be32). */
static void put_bef(uint32_t off, float f)
{
    uint32_t v; memcpy(&v, &f, 4);
    vm_base[off+0] = (uint8_t)(v >> 24); vm_base[off+1] = (uint8_t)(v >> 16);
    vm_base[off+2] = (uint8_t)(v >> 8);  vm_base[off+3] = (uint8_t)(v);
}

static rsx_state g_state;
static void M(uint32_t method, uint32_t data) { rsx_process_method(&g_state, method, data); }

static void build_guest_memory(void)
{
    /* Fragment program: TEX r0, TC0 (unit 0, END). Samples texture unit 0 at
     * texcoord0. */
    uint32_t o = FP_ADDR;
    uint32_t w0 = (0x17u << 24)          /* opcode TEX            */
                | (0xFu  << 9)           /* write mask xyzw       */
                | (0x4u  << 13)          /* INPUT_SRC = TC0       */
                | (0x0u  << 17)          /* TEX_UNIT = 0          */
                | 1u;                    /* PROGRAM_END           */
    /* SRC0 (texcoord): type INPUT(1), identity swizzle x,y,z,w. */
    uint32_t w1 = 1u
                | (0u << 9) | (1u << 11) | (2u << 13) | (3u << 15);
    put_fp_word(o + 0,  w0);
    put_fp_word(o + 4,  w1);
    put_fp_word(o + 8,  0);
    put_fp_word(o + 12, 0);

    /* Positions (clip space; identity MVP). float3, stride 12. */
    put_bef(POS_ADDR + 0,  0.0f);  put_bef(POS_ADDR + 4,  0.5f);  put_bef(POS_ADDR + 8,  0.0f);
    put_bef(POS_ADDR + 12, 0.5f);  put_bef(POS_ADDR + 16,-0.5f);  put_bef(POS_ADDR + 20, 0.0f);
    put_bef(POS_ADDR + 24,-0.5f);  put_bef(POS_ADDR + 28,-0.5f);  put_bef(POS_ADDR + 32, 0.0f);

    /* COLOR0 ubyte4, stride 4: white at every vertex (so an RGB result must
     * come from TC0, not COL0). */
    uint8_t cols[12] = { 255,255,255,255,  255,255,255,255,  255,255,255,255 };
    memcpy(vm_base + COL_ADDR, cols, sizeof(cols));

    /* TEXCOORD0 float4, stride 16: UV per vertex (.xy = uv; .zw = 0,1). */
    put_bef(TC_ADDR + 0,  0.5f); put_bef(TC_ADDR + 4,  0.0f); put_bef(TC_ADDR + 8,  0.0f); put_bef(TC_ADDR + 12, 1.0f);
    put_bef(TC_ADDR + 16, 1.0f); put_bef(TC_ADDR + 20, 1.0f); put_bef(TC_ADDR + 24, 0.0f); put_bef(TC_ADDR + 28, 1.0f);
    put_bef(TC_ADDR + 32, 0.0f); put_bef(TC_ADDR + 36, 1.0f); put_bef(TC_ADDR + 40, 0.0f); put_bef(TC_ADDR + 44, 1.0f);

    /* 2x2 A8R8G8B8 texture. DXGI maps this to B8G8R8A8_UNORM, so store bytes in
     * B,G,R,A order: (0,0)=red (1,0)=green (0,1)=blue (1,1)=yellow. */
    uint8_t tex[16] = {
        0,0,255,255,   0,255,0,255,     /* row 0: red,   green  */
        255,0,0,255,   0,255,255,255,   /* row 1: blue,  yellow */
    };
    memcpy(vm_base + TEX_ADDR, tex, sizeof(tex));
}

static void issue_setup_methods(void)
{
    /* Fragment program at FP_ADDR (location bit 1 = main memory). */
    M(NV4097_SET_SHADER_PROGRAM, FP_ADDR | 0x1u);

    /* Vertex attrib 0 = position: type 2 (float32), size 3, stride 12. */
    M(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 0 * 4, 0x2u | (3u << 4) | (12u << 8));
    M(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 0 * 4, POS_ADDR);

    /* Vertex attrib 3 = COLOR0: type 4 (ubyte), size 4, stride 4. */
    M(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 3 * 4, 0x4u | (4u << 4) | (4u << 8));
    M(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 3 * 4, COL_ADDR);

    /* Vertex attrib 8 = TEXCOORD0: type 2 (float32), size 4, stride 16. */
    M(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 8 * 4, 0x2u | (4u << 4) | (16u << 8));
    M(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 8 * 4, TC_ADDR);

    /* Texture unit 0: A8R8G8B8 (fmt 0x85, not swizzled), 2x2, at TEX_ADDR. */
    M(NV4097_SET_TEXTURE_OFFSET     + 0 * 0x20, TEX_ADDR);
    M(NV4097_SET_TEXTURE_FORMAT     + 0 * 0x20, 0x8500u);
    M(NV4097_SET_TEXTURE_IMAGE_RECT + 0 * 0x20, (2u << 16) | 2u);

    /* Blend: SRC_ALPHA / ONE_MINUS_SRC_ALPHA -- exercises a non-default PSO. */
    M(NV4097_SET_BLEND_ENABLE,       1);
    M(NV4097_SET_BLEND_FUNC_SFACTOR, 0x0302);
    M(NV4097_SET_BLEND_FUNC_DFACTOR, 0x0303);

    /* Clear color: dark blue so the triangle stands out. */
    M(NV4097_SET_COLOR_CLEAR_VALUE, 0xFF101830);
}

static void draw_one_triangle(void)
{
    M(NV4097_SET_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    M(NV4097_DRAW_ARRAYS,   (2u << 24) | 0u);   /* count = 2+1 = 3, first = 0 */
    M(NV4097_SET_BEGIN_END, 0);
}

int main(void)
{
    vm_base = (uint8_t*)calloc(1, VM_SIZE);
    if (!vm_base) { printf("alloc failed\n"); return 1; }

    rsx_state_init(&g_state);
    build_guest_memory();

    if (rsx_d3d12_backend_init(1280, 720, "ps3recomp RSX harness") != 0) {
        printf("backend init failed\n");
        return 1;
    }

    issue_setup_methods();

    printf("\n[harness] Running. FP = TEX r0, TC0  (samples a 2x2 texture).\n");
    printf("          Expect a smooth multi-color gradient (red/green/blue/yellow) in the triangle.\n");
    printf("          (solid black => texture upload/sampling failed; null SRV sampled 0.)\n");
    printf("          ESC or close window to quit.\n\n");

    for (;;) {
        if (rsx_d3d12_backend_pump_messages() != 0) break;
        draw_one_triangle();          /* draw records reset each present */
        rsx_d3d12_backend_present();
    }

    rsx_d3d12_backend_shutdown();
    free(vm_base);
    return 0;
}
