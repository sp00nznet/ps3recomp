/*
 * Shared harness for the D3D12 live-draw tests (Windows only).
 *
 * Drives rsx_live_draw directly with NV4097 method writes over two small
 * arenas (location 0 = local memory, location 1 = main memory), with the
 * null backend's hidden window as the swap-chain target, and reads a colour
 * surface back through rsx_live_draw_debug_dump_surface (8-bit PPM).
 *
 * Build (MinGW-w64), from the repository root, after building the runtime
 * library (cmake --build <dir> --target ps3recomp_runtime):
 *   x86_64-w64-mingw32-gcc -std=gnu17 -O1 -Iinclude -Ilibs/video -Iruntime \
 *       libs/video/tests/<test>.c runtime/host/host_stubs.c \
 *       <dir>/libps3recomp_runtime.a -ld3d12 -ldxgi -ld3dcompiler -ldxguid \
 *       -lgdi32 -luser32 -lole32 -lws2_32 -lxinput -lbcrypt -lwinmm \
 *       -lshlwapi -lsynchronization -lpsapi -ldbghelp -static -lstdc++ \
 *       -o <test>.exe
 * Run it on Windows; exit code 0 means every check passed. Under Wine or
 * Proton (vkd3d) live-draw init fails as it stands: vkd3d-shader's HLSL
 * compiler has no RWTexture2D::GetDimensions, which the depth-snapshot
 * compute shader uses.
 */
#ifndef LIVE_DRAW_TEST_HARNESS_H
#define LIVE_DRAW_TEST_HARNESS_H

#include "ps3emu/ps3types.h"
#include "rsx_commands.h"
#include "rsx_live_draw.h"
#include "rsx_null_backend.h"
#include "rsx_test_programs.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The runtime library references these; a lifted title provides them. */
uint8_t* vm_base = NULL;
struct ldh_func_entry { uint64_t addr; void* func; const char* name; };
const struct ldh_func_entry function_table[1] = { { 0, NULL, NULL } };
const uint64_t function_table_count = 0;

#define LDH_LOCAL_SIZE (8u << 20)
#define LDH_MAIN_SIZE  (1u << 20)
static uint8_t* ldh_local;
static uint8_t* ldh_main;
static int ldh_pass, ldh_fail;

static const uint8_t* ldh_guest_ptr(void* user, u32 location, u32 offset, u32 min_bytes)
{
    (void)user;
    const u32 size = location == 0 ? LDH_LOCAL_SIZE : LDH_MAIN_SIZE;
    if ((uint64_t)offset + min_bytes > size) return NULL;
    return (location == 0 ? ldh_local : ldh_main) + offset;
}

static int ldh_init(const char* name)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    _putenv("RSX_LIVE_DRAW=1");
    ldh_local = (uint8_t*)calloc(1, LDH_LOCAL_SIZE);
    ldh_main = (uint8_t*)calloc(1, LDH_MAIN_SIZE);
    if (!ldh_local || !ldh_main) return -1;
    if (rsx_null_backend_init(64, 64, name) != 0 ||
        rsx_live_draw_init(rsx_null_backend_get_hwnd(), 64, 64, ldh_guest_ptr, NULL) != 0) {
        printf("[%s] live-draw init failed\n", name);
        return -1;
    }
    rsx_null_backend_suppress_present(1);
    return 0;
}

#define ldh_m rsx_live_draw_method
static void ldh_mf(u32 method, float f) { u32 w; memcpy(&w, &f, 4); ldh_m(method, w); }

/* Main-memory helpers: big-endian, as the guest writes them. */
static void ldh_w32(u32 off, u32 v)
{
    uint8_t* p = ldh_main + off;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void ldh_wf(u32 off, float f) { u32 w; memcpy(&w, &f, 4); ldh_w32(off, w); }

/* A full-target quad: float4 position, float4 colour, float4 tc0 per vertex
 * (stride 48) at main-memory offset vtx, with tc0 = (u, v, z) where z runs
 * from z0 at the left edge to z1 at the right, u/v from 0 to tw/th. A
 * pass-through vertex program (pos, colour, tc0) is loaded at slot 0. */
static void ldh_setup_quad(u32 vtx, float tw, float th, float z0, float z1)
{
    static const float xs[4] = { -1, 1, 1, -1 }, ys[4] = { 1, 1, -1, -1 };
    for (int i = 0; i < 4; i++) {
        const float u = xs[i] < 0 ? 0.0f : tw, v = ys[i] > 0 ? 0.0f : th;
        const float a[12] = { xs[i], ys[i], 0, 1,  1, 1, 1, 1,  u, v, xs[i] < 0 ? z0 : z1, 1 };
        for (int k = 0; k < 12; k++) ldh_wf(vtx + (u32)(i * 48 + k * 4), a[k]);
    }
    u8 vp[48];
    rsx_test_vp_mov_out(vp +  0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 3, RSX_TEST_VP_SWZ_IDENT, 1, 0);
    rsx_test_vp_mov_out(vp + 32, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
    ldh_m(NV4097_SET_TRANSFORM_PROGRAM_LOAD, 0u);
    for (u32 k = 0; k < sizeof vp / 4u; k++) {
        const u8* w = vp + k * 4;
        ldh_m(NV4097_SET_TRANSFORM_PROGRAM + k * 4,
              (u32)w[0] | ((u32)w[1] << 8) | ((u32)w[2] << 16) | ((u32)w[3] << 24));
    }
    ldh_m(NV4097_SET_TRANSFORM_PROGRAM_START, 0u);
    for (u32 a = 0; a < 3; a++) {
        static const u32 attr[3] = { 0, 3, 8 };
        ldh_m(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + attr[a] * 4, 0x80000000u | (vtx + a * 16u));
        ldh_m(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + attr[a] * 4, 2u | (4u << 4) | (48u << 8));
    }
}

/* Surface state for a w x h pass: colour (location 0) at color, zeta at zeta,
 * SURFACE_FORMAT fmt, viewport depth range [0, 1]. */
static void ldh_surface(u32 fmt, u32 color, u32 cpitch, u32 zeta, u32 zpitch, u32 w, u32 h)
{
    ldh_m(NV4097_SET_CONTEXT_DMA_COLOR_A, CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER);
    ldh_m(NV4097_SET_CONTEXT_DMA_ZETA,    CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER);
    ldh_m(NV4097_SET_SURFACE_FORMAT,      fmt);
    ldh_m(NV4097_SET_SURFACE_PITCH_A,     cpitch);
    ldh_m(NV4097_SET_SURFACE_COLOR_AOFFSET, color);
    ldh_m(NV4097_SET_SURFACE_PITCH_Z,     zpitch);
    ldh_m(NV4097_SET_SURFACE_ZETA_OFFSET, zeta);
    ldh_m(NV4097_SET_SURFACE_COLOR_TARGET, CELL_GCM_SURFACE_TARGET_0);
    ldh_m(NV4097_SET_SURFACE_CLIP_HORIZONTAL, w << 16);
    ldh_m(NV4097_SET_SURFACE_CLIP_VERTICAL,   h << 16);
    ldh_m(NV4097_SET_VIEWPORT_HORIZONTAL, w << 16);
    ldh_m(NV4097_SET_VIEWPORT_VERTICAL,   h << 16);
    ldh_m(NV4097_SET_SCISSOR_HORIZONTAL,  w << 16);
    ldh_m(NV4097_SET_SCISSOR_VERTICAL,    h << 16);
    ldh_mf(0x0A20, w * 0.5f); ldh_mf(0x0A24, h * 0.5f);  ldh_mf(0x0A28, 0.5f); ldh_mf(0x0A2C, 0.0f);
    ldh_mf(0x0A30, w * 0.5f); ldh_mf(0x0A34, h * -0.5f); ldh_mf(0x0A38, 0.5f); ldh_mf(0x0A3C, 0.0f);
}

/* Depth state, shader control, fragment program (main memory) and one quad. */
static void ldh_draw(u32 fp, u32 ctrl, int dtest, int dwrite, u32 dfunc)
{
    ldh_m(NV4097_SET_DEPTH_TEST_ENABLE, dtest ? 1u : 0u);
    ldh_m(NV4097_SET_DEPTH_FUNC, dfunc);
    ldh_m(NV4097_SET_DEPTH_MASK, dwrite ? 1u : 0u);
    ldh_m(NV4097_SET_SHADER_CONTROL, ctrl);
    ldh_m(NV4097_SET_SHADER_PROGRAM, fp | 2u);
    ldh_m(NV4097_SET_BEGIN_END, RSX_PRIMITIVE_QUADS);
    ldh_m(NV4097_DRAW_ARRAYS, (3u << 24) | 0u);
    ldh_m(NV4097_SET_BEGIN_END, 0u);
}

/* Texture unit 0: a 2D texture at local offset off, format byte fmt (with
 * linear + unnormalised bits as given), w x h, pitch, point sampling,
 * clamp-to-edge. */
static void ldh_texture0(u32 off, u32 fmt, u32 w, u32 h, u32 pitch)
{
    ldh_m(NV4097_SET_TEXTURE_OFFSET,       off);
    ldh_m(NV4097_SET_TEXTURE_FORMAT,       1u | (2u << 4) | (fmt << 8) | (1u << 16));
    ldh_m(NV4097_SET_TEXTURE_ADDRESS,      3u | (3u << 8) | (3u << 16));
    ldh_m(NV4097_SET_TEXTURE_CONTROL0,     0x80000000u | ((12u << 8) << 7));
    ldh_m(NV4097_SET_TEXTURE_CONTROL1,     0xAAE4u);
    ldh_m(NV4097_SET_TEXTURE_CONTROL3,     pitch);
    ldh_m(NV4097_SET_TEXTURE_FILTER,       (1u << 16) | (1u << 24) | (1u << 13));
    ldh_m(NV4097_SET_TEXTURE_IMAGE_RECT,   (w << 16) | h);
    ldh_m(NV4097_SET_TEXTURE_BORDER_COLOR, 0u);
}

/* Read the colour surface at local offset off back as 8-bit RGB (w x h max). */
static int ldh_read_rgb(u32 off, const char* path, uint8_t* rgb, u32 max_w, u32 max_h,
                        u32* out_w, u32* out_h)
{
    rsx_live_draw_flush();
    if (rsx_live_draw_debug_dump_surface(0, off, path) != 0) return -1;
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    u32 w = 0, h = 0, mx = 0;
    if (fscanf(f, "P6 %u %u %u", &w, &h, &mx) != 3 || mx != 255 || w > max_w || h > max_h) {
        fclose(f); return -1;
    }
    fgetc(f);
    const size_t n = (size_t)w * h * 3u;
    const size_t got = fread(rgb, 1, n, f);
    fclose(f);
    if (got != n) return -1;
    *out_w = w; *out_h = h;
    return 0;
}

static void ldh_check(const char* name, int ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (ok) ldh_pass++; else ldh_fail++;
}

static int ldh_done(const char* name)
{
    printf("[%s] Results: %d passed, %d failed\n", name, ldh_pass, ldh_fail);
    return ldh_fail ? 1 : 0;
}

#endif
