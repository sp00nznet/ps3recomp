/*
 * D3D12 live-draw test: a depth buffer read as a texture returns its depth.
 *
 *   A. a 4x4 pass writes depth into Z24S8 zeta ZA (the zeta and its R32F
 *      snapshot are created with a 4x4 clip);
 *   B. a 16x16 pass whose colour target is ZA itself (colour writes masked)
 *      writes depth 0.1 at the left edge to 0.9 at the right into ZA;
 *   C. a 16x16 pass into colour surface E with ZA still bound as its zeta,
 *      depth test on and depth write off, reads ZA as a 16x16 DEPTH24_D8
 *      texture: E at column x must be 0.1 + 0.8 * (x + 0.5) / 16.
 * C needs all three: the snapshot grown to 16x16 (it was created 4x4), the
 * depth read preferring the zeta over the colour surface at the same
 * address, and a draw that reads its own zeta without writing it reading
 * the snapshot instead of guest memory.
 * Build and run: see live_draw_test_harness.h.
 */
#include "live_draw_test_harness.h"
#include <math.h>

#define T "test_live_draw_depth_textures"

/* Local memory. */
#define SURF_A  0x00000000u   /* 4x4 A8R8G8B8 */
#define SURF_E  0x00004000u   /* 16x16, the read result */
#define ZA      0x00100000u   /* Z24S8 zeta, and in B also the colour target */
/* Main memory. */
#define VTX     0x00000000u
#define FP_TX   0x00001000u   /* TEX r0, TC0, unit 0 */
#define FP_COL  0x00001100u   /* MOV r0, COL0 */

#define FMT_ARGB_Z24 (0x8u | (0x2u << 5) | (0x1u << 8))   /* A8R8G8B8, Z24S8, linear */

int main(void)
{
    if (ldh_init(T) != 0) return 2;
    rsx_test_fp_tex_r0_tc0(ldh_main + FP_TX, 0);
    rsx_test_fp_mov_r0_col0(ldh_main + FP_COL, RSX_TEST_FP_SWZ_IDENT);
    ldh_setup_quad(VTX, 1, 1, 0, 0);
    /* Clip-space z from -0.8 (left) to 0.8 (right): depth 0.1 to 0.9. */
    for (u32 i = 0; i < 4; i++) {
        const float x = (i == 0 || i == 3) ? -1.0f : 1.0f;
        ldh_wf(VTX + i * 48u + 8u, 0.8f * x);
    }
    const u32 X32 = 0x40u, ALWAYS = 0x207u;

    ldh_surface(FMT_ARGB_Z24, SURF_A, 64, ZA, 64, 4, 4);
    ldh_draw(FP_COL, X32, 1, 1, ALWAYS);                        /* A */

    ldh_surface(FMT_ARGB_Z24, ZA, 64, ZA, 64, 16, 16);
    ldh_m(NV4097_SET_COLOR_MASK, 0u);
    ldh_draw(FP_COL, X32, 1, 1, ALWAYS);                        /* B */
    ldh_m(NV4097_SET_COLOR_MASK, 0x01010101u);

    ldh_surface(FMT_ARGB_Z24, SURF_E, 64, ZA, 64, 16, 16);
    ldh_texture0(ZA, 0x90u | 0x20u, 16, 16, 64);                /* DEPTH24_D8, linear */
    ldh_draw(FP_TX, X32, 1, 0, ALWAYS);                         /* C */

    static uint8_t rgb[16 * 16 * 3];
    u32 w = 0, h = 0;
    const int ok = ldh_read_rgb(SURF_E, "depth_textures_e.ppm", rgb, 16, 16, &w, &h) == 0 &&
                   w == 16 && h == 16;
    ldh_check("surface E read back (16x16)", ok);
    if (ok) {
        int good = 0;
        for (u32 y = 0; y < 16; y++)
            for (u32 x = 0; x < 16; x++) {
                const float want = 0.1f + 0.8f * (x + 0.5f) / 16.0f;
                good += fabsf(rgb[(y * 16 + x) * 3] / 255.0f - want) <= 3.0f / 255.0f;
            }
        printf("[" T "] row 8:");
        for (u32 x = 0; x < 16; x++) printf(" %.3f", rgb[(8 * 16 + x) * 3] / 255.0f);
        printf("\n");
        char name[96];
        snprintf(name, sizeof name, "E holds ZA's depth (%d of 256 texels)", good);
        ldh_check(name, good == 256);
    }
    return ldh_done(T);
}
