/*
 * D3D12 live-draw test: fragment depth export, and the texel size of a
 * depth texture that reads a zeta snapshot larger than itself.
 *
 *   1. a 16x16 pass writes depth 0.5 into Z24S8 zeta Z;
 *   2. a 4x4 pass into Z with SHADER_CONTROL depth export (0xE) writes
 *      depth = r1.z = tc0.z, 0.1 at the left edge to 0.9 at the right,
 *      so columns 0..3 hold 0.2, 0.4, 0.6, 0.8;
 *   3. Z read as a 16x16 DEPTH24_D8 texture with normalised coordinates:
 *      the top-left 4x4 holds the exported depths, the rest 0.5;
 *   4. Z read as a 4x4 DEPTH24_D8 texture with texel (unnormalised)
 *      coordinates 0..4: it reads Z's top-left 4x4, as it does in VRAM.
 *
 * Without depth export the 4x4 pass keeps the quad's depth (0.5); dividing
 * the texel coordinates of step 4 by the texture size (4) instead of the
 * snapshot's (16) stretches the whole 16x16 zeta over the 4x4 target.
 * Build and run: see live_draw_test_harness.h.
 */
#include "live_draw_test_harness.h"
#include <math.h>

#define T "test_live_draw_depth_export"

/* Local memory. */
#define SURF_BIG 0x00000000u   /* 16x16 A8R8G8B8, pitch 64 */
#define SURF_S   0x00002000u   /* 4x4, the depth-export pass's colour */
#define SURF_E   0x00004000u   /* 16x16, step 3 result */
#define SURF_F   0x00006000u   /* 4x4, step 4 result */
#define ZETA     0x00100000u   /* Z24S8, 16x16, pitch 64 */
#define ZETA2    0x00102000u   /* zeta of the read passes (depth test off) */
/* Main memory. */
#define VTX_A    0x00000000u
#define VTX_B    0x00000400u
#define FP_DE    0x00001000u   /* MOV r1, TC0 ; MOV r0, COL0 */
#define FP_TX    0x00001100u   /* TEX r0, TC0, unit 0 */
#define FP_COL   0x00001200u   /* MOV r0, COL0 */

#define FMT_ARGB_Z24 (0x8u | (0x2u << 5) | (0x1u << 8))   /* A8R8G8B8, Z24S8, linear */
#define TEXF_DEPTH24_D8 0x90u
#define TEXF_LN 0x20u
#define TEXF_UN 0x40u

static int near_u8(uint8_t got, float want) { return fabsf(got / 255.0f - want) <= 3.0f / 255.0f; }

int main(void)
{
    if (ldh_init(T) != 0) return 2;
    uint8_t* fpm = ldh_main;
    rsx_test_fp_instr(fpm + FP_DE,
                      RSX_TEST_FP_OPC(RSX_TEST_FP_OP_MOV) | RSX_TEST_FP_MASK_XYZW |
                      RSX_TEST_FP_INSRC_TC0 | RSX_TEST_FP_OUTREG(1),
                      RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
    rsx_test_fp_mov_r0_col0(fpm + FP_DE + 16, RSX_TEST_FP_SWZ_IDENT);
    rsx_test_fp_tex_r0_tc0(fpm + FP_TX, 0);
    rsx_test_fp_mov_r0_col0(fpm + FP_COL, RSX_TEST_FP_SWZ_IDENT);
    const u32 X32 = 0x40u, ALWAYS = 0x207u;

    ldh_setup_quad(VTX_A, 1, 1, 0.1f, 0.9f);
    ldh_surface(FMT_ARGB_Z24, SURF_BIG, 64, ZETA, 64, 16, 16);
    ldh_draw(FP_COL, X32, 1, 1, ALWAYS);                        /* 1: depth 0.5 */
    ldh_surface(FMT_ARGB_Z24, SURF_S, 64, ZETA, 64, 4, 4);
    ldh_draw(FP_DE, X32 | 0xEu, 1, 1, ALWAYS);                  /* 2: depth = tc0.z */

    ldh_surface(FMT_ARGB_Z24, SURF_E, 64, ZETA2, 64, 16, 16);
    ldh_texture0(ZETA, TEXF_DEPTH24_D8 | TEXF_LN, 16, 16, 64);
    ldh_draw(FP_TX, X32, 0, 0, ALWAYS);                         /* 3: normalised read */

    ldh_setup_quad(VTX_B, 4, 4, 0, 0);
    ldh_surface(FMT_ARGB_Z24, SURF_F, 64, ZETA2, 64, 4, 4);
    ldh_texture0(ZETA, TEXF_DEPTH24_D8 | TEXF_LN | TEXF_UN, 4, 4, 64);
    ldh_draw(FP_TX, X32, 0, 0, ALWAYS);                         /* 4: texel read */

    static uint8_t rgb[16 * 16 * 3];
    u32 w = 0, h = 0;
    int ok = ldh_read_rgb(SURF_E, "depth_export_e.ppm", rgb, 16, 16, &w, &h) == 0 && w == 16 && h == 16;
    ldh_check("step 3 surface read back (16x16)", ok);
    if (ok) {
        int good = 0;
        for (u32 y = 0; y < 4; y++) {
            printf("[" T "] exported row %u:", y);
            for (u32 x = 0; x < 4; x++) {
                const uint8_t r = rgb[(y * 16 + x) * 3];
                printf(" %.3f", r / 255.0f);
                good += near_u8(r, 0.2f + 0.2f * x);
            }
            printf("\n");
        }
        ldh_check("depth export: 16 exported depths 0.2/0.4/0.6/0.8 by column", good == 16);
        ldh_check("depth export: depth outside the 4x4 pass stays 0.5",
                  near_u8(rgb[(10 * 16 + 10) * 3], 0.5f) && near_u8(rgb[(2 * 16 + 12) * 3], 0.5f));
    }
    ok = ldh_read_rgb(SURF_F, "depth_export_f.ppm", rgb, 16, 16, &w, &h) == 0 && w == 4 && h == 4;
    ldh_check("step 4 surface read back (4x4)", ok);
    if (ok) {
        int good = 0;
        for (u32 y = 0; y < 4; y++) {
            printf("[" T "] texel-read row %u:", y);
            for (u32 x = 0; x < 4; x++) {
                const uint8_t r = rgb[(y * 4 + x) * 3];
                printf(" %.3f", r / 255.0f);
                good += near_u8(r, 0.2f + 0.2f * x);
            }
            printf("\n");
        }
        ldh_check("texel coordinates address the snapshot: 4x4 corner read 1:1", good == 16);
    }
    return ldh_done(T);
}
