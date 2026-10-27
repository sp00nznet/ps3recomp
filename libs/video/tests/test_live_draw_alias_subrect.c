/*
 * D3D12 live-draw test: a texture smaller than the colour surface it aliases
 * reads the surface's top-left samples, texel for sample.
 *
 *   A. an 8x8-pixel A8R8G8B8 target with antialias 4 (2x2 samples) is held at
 *      16x16 samples, guest pitch 64. It is cleared to black and a white quad
 *      is drawn with the scissor on the left 4 pixels, so the left 8 sample
 *      columns are white and the right 8 are black.
 *   B. an 8x8 pass into a second surface samples the same address as an 8x8
 *      linear A8R8G8B8 texture with pitch 64. Texel (x, y) of that texture is
 *      sample (x, y) of A, so every texel is white. Bound to the whole 16x16
 *      surface and scaled into the 8x8 texture, the right half would be black.
 * Build and run: see live_draw_test_harness.h.
 */
#include "live_draw_test_harness.h"

#define T "test_live_draw_alias_subrect"

#define SURF_A  0x00000000u   /* 8x8 pixels, 16x16 samples, pitch 64 */
#define SURF_E  0x00010000u   /* 8x8, the result */
#define VTX     0x00000000u
#define FP_COL  0x00001000u
#define FP_TX   0x00001100u

int main(void)
{
    if (ldh_init(T) != 0) return 2;
    rsx_test_fp_mov_r0_col0(ldh_main + FP_COL, RSX_TEST_FP_SWZ_IDENT);
    rsx_test_fp_tex_r0_tc0(ldh_main + FP_TX, 0);
    ldh_setup_quad(VTX, 1, 1, 0, 0);

    ldh_surface(0x8u | (0x1u << 8) | (4u << 12), SURF_A, 64, 0x200000u, 64, 8, 8);
    ldh_m(NV4097_SET_COLOR_CLEAR_VALUE, 0xFF000000u);
    ldh_m(NV4097_CLEAR_SURFACE, 0xF0u);
    ldh_m(NV4097_SET_SCISSOR_HORIZONTAL, 4u << 16);          /* left 4 pixels = 8 samples */
    ldh_draw(FP_COL, 0x40u, 0, 0, 0x207u);                   /* A */

    ldh_surface(0x8u | (0x1u << 8), SURF_E, 32, 0x200000u, 64, 8, 8);
    ldh_m(NV4097_SET_COLOR_CLEAR_VALUE, 0xFF000000u);
    ldh_m(NV4097_CLEAR_SURFACE, 0xF0u);
    ldh_texture0(SURF_A, 0x85u | 0x20u, 8, 8, 64);           /* A8R8G8B8, linear, pitch 64 */
    ldh_draw(FP_TX, 0x40u, 0, 0, 0x207u);                    /* B */

    static uint8_t rgb[8 * 8 * 3];
    u32 w = 0, h = 0;
    const int ok = ldh_read_rgb(SURF_E, "alias_subrect_e.ppm", rgb, 8, 8, &w, &h) == 0 && w == 8 && h == 8;
    ldh_check("result surface read back (8x8)", ok);
    if (ok) {
        int good = 0;
        for (u32 i = 0; i < 64; i++) good += rgb[i * 3] > 200;
        char name[96];
        snprintf(name, sizeof name, "8x8 texture over a 16x16-sample surface reads its top-left "
                 "samples (%d of 64 white)", good);
        ldh_check(name, good == 64);
    }
    return ldh_done(T);
}
