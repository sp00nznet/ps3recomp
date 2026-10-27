/*
 * D3D12 live-draw test: antialiased surfaces at sample resolution.
 *
 * An RSX surface with antialiasing (SURFACE_FORMAT bits 12-15) holds its
 * sample grid in memory: 3 = 2x1 samples per pixel, 4 and 5 = 2x2. For each
 * mode an 8x8-pixel A8R8G8B8 target is cleared to black and a white quad is
 * drawn over the whole 8x8 viewport with the scissor on the left 4 pixel
 * columns. The surface must read back at its sample size with the left half
 * of the samples white and the right half black.
 * Build and run: see live_draw_test_harness.h.
 */
#include "live_draw_test_harness.h"

#define T "test_live_draw_aa_surface"

#define VTX     0x00000000u    /* main memory */
#define FP_COL  0x00001000u

int main(void)
{
    if (ldh_init(T) != 0) return 2;
    rsx_test_fp_mov_r0_col0(ldh_main + FP_COL, RSX_TEST_FP_SWZ_IDENT);
    ldh_setup_quad(VTX, 1, 1, 0, 0);
    static const struct { u32 aa, sx, sy; } cases[] = {
        { 0, 1, 1 }, { 3, 2, 1 }, { 4, 2, 2 }, { 5, 2, 2 },
    };
    for (u32 i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const u32 surf = 0x10000u * i;
        ldh_surface(0x8u | (0x1u << 8) | (cases[i].aa << 12), surf, 64, 0x200000u, 64, 8, 8);
        ldh_m(NV4097_SET_COLOR_CLEAR_VALUE, 0xFF000000u);
        ldh_m(NV4097_CLEAR_SURFACE, 0xF0u);
        ldh_m(NV4097_SET_SCISSOR_HORIZONTAL, 4u << 16);   /* left 4 pixels */
        ldh_draw(FP_COL, 0x40u, 0, 0, 0x207u);
        static uint8_t rgb[32 * 32 * 3];
        u32 w = 0, h = 0;
        char path[64], name[96];
        snprintf(path, sizeof path, "aa_surface_%u.ppm", cases[i].aa);
        const u32 ww = 8u * cases[i].sx, wh = 8u * cases[i].sy;
        const int read = ldh_read_rgb(surf, path, rgb, 32, 32, &w, &h) == 0;
        snprintf(name, sizeof name, "antialias %u: surface is %ux%u samples (got %ux%u)",
                 cases[i].aa, ww, wh, w, h);
        ldh_check(name, read && w == ww && h == wh);
        if (!read) continue;
        int good = 0;
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++) {
                const int lit = rgb[(y * w + x) * 3] > 200;
                good += lit == (x < w / 2u);
            }
        snprintf(name, sizeof name, "antialias %u: left half drawn, right half scissored "
                 "(%d of %u samples right)", cases[i].aa, good, w * h);
        ldh_check(name, good == (int)(w * h));
    }
    return ldh_done(T);
}
