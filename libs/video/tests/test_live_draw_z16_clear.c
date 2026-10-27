/*
 * D3D12 live-draw test: ZSTENCIL_CLEAR_VALUE is decoded by the zeta format.
 *
 * For a Z16 zeta (SURFACE_FORMAT depth field 1) the clear value is a bare
 * depth16: 0xFFFF is the far plane. Each case clears colour to black and
 * depth with the given value, then draws a white quad at depth 0.5 with
 * depth test LESS: the quad is visible only if the cleared depth lies
 * behind 0.5.
 *   Z16   0x0000FFFF  -> depth 1.0, quad visible
 *   Z16   0x0000A000  -> depth 0.625, quad (0.5) visible
 *   Z16   0x00004000  -> depth ~0.25, quad hidden
 *   Z24S8 0xFFFFFF00  -> depth 1.0, quad visible
 * Read as Z24S8, the Z16 0xFFFF clears to 255/16777215 and hides the quad.
 * Build and run: see live_draw_test_harness.h.
 */
#include "live_draw_test_harness.h"

#define T "test_live_draw_z16_clear"

#define VTX     0x00000000u    /* main memory */
#define FP_COL  0x00001000u
#define ZETA16  0x00100000u    /* local memory */
#define ZETA24  0x00102000u

static int run_case(const char* name, u32 surf, u32 zfmt, u32 zeta, u32 zpitch, u32 zclear,
                    int want_visible)
{
    ldh_surface(0x8u | (zfmt << 5) | (0x1u << 8), surf, 64, zeta, zpitch, 16, 16);
    ldh_m(NV4097_SET_COLOR_CLEAR_VALUE, 0xFF000000u);
    ldh_m(NV4097_SET_ZSTENCIL_CLEAR_VALUE, zclear);
    ldh_m(NV4097_CLEAR_SURFACE, 0xF3u);                 /* colour + depth + stencil */
    ldh_draw(FP_COL, 0x40u, 1, 1, 0x201u);               /* LESS */
    static uint8_t rgb[16 * 16 * 3];
    u32 w = 0, h = 0;
    char path[64];
    snprintf(path, sizeof path, "z16_clear_%08X.ppm", surf);
    if (ldh_read_rgb(surf, path, rgb, 16, 16, &w, &h) != 0 || w != 16 || h != 16) {
        printf("[" T "] %s: surface read back failed\n", name);
        ldh_check(name, 0);
        return 0;
    }
    int lit = 0;
    for (u32 i = 0; i < 16u * 16u; i++) lit += rgb[i * 3] > 200;
    printf("[" T "] %s: clear 0x%08X, %d of 256 pixels drawn (want %s)\n",
           name, zclear, lit, want_visible ? "256" : "0");
    ldh_check(name, want_visible ? lit == 256 : lit == 0);
    return 1;
}

int main(void)
{
    if (ldh_init(T) != 0) return 2;
    rsx_test_fp_mov_r0_col0(ldh_main + FP_COL, RSX_TEST_FP_SWZ_IDENT);
    ldh_setup_quad(VTX, 1, 1, 0, 0);
    run_case("Z16 cleared to 0xFFFF is the far plane",       0x00000000u, 1u, ZETA16, 32, 0x0000FFFFu, 1);
    run_case("Z16 cleared to 0xA000 is behind 0.5",          0x00002000u, 1u, ZETA16, 32, 0x0000A000u, 1);
    run_case("Z16 cleared to 0x4000 hides a draw at 0.5",     0x00004000u, 1u, ZETA16, 32, 0x00004000u, 0);
    run_case("Z24S8 cleared to 0xFFFFFF00 is the far plane",  0x00006000u, 2u, ZETA24, 64, 0xFFFFFF00u, 1);
    return ldh_done(T);
}
