/* A stale size/format alias must not replace an exact current-target read. */
#ifndef FEEDBACK_RENDERER_SOURCE
#define FEEDBACK_RENDERER_SOURCE "../rsx_live_draw.c"
#endif
#include FEEDBACK_RENDERER_SOURCE
#include "live_draw_test_harness.h"

#define T "test_live_draw_feedback_alias"
#define IMAGE 0x50000u
#define RESULT 0x10000u
#define FP_COLOR 0x1000u
#define FP_TEXTURE 0x1100u

static void color_program(float red, float green, float blue)
{
    const float values[4] = { red, green, blue, 1 };
    rsx_test_fp_instr(ldh_main + FP_COLOR,
        RSX_TEST_FP_OPC(1) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_END,
        2u | RSX_TEST_FP_SWZ_IDENT);
    for (u32 i = 0; i < 4; i++) {
        u32 bits; memcpy(&bits, values + i, 4);
        rsx_test_fp_put_word(ldh_main + FP_COLOR + 16 + i * 4, bits);
    }
}

static void target(u32 format, u32 width, u32 height)
{
    ldh_surface(format | (1u << 8), IMAGE,
        width * (format == 12 ? 16u : format == 11 ? 8u : 4u), 0x200000, 64, width, height);
}

static void solid(u32 format, u32 width, u32 height, float r, float green, float b)
{
    ldh_m(NV4097_SET_TEXTURE_CONTROL0, 0);
    target(format, width, height);
    color_program(r, green, b);
    ldh_draw(FP_COLOR, 0x40, 0, 0, 0x207);
}

static void split(u32 format)
{
    solid(format, 2560, 720, 1, 0, 0);
    ldh_m(NV4097_SET_SCISSOR_HORIZONTAL, (1280u << 16) | 1280u);
    color_program(0, 1, 0);
    ldh_draw(FP_COLOR, 0x40, 0, 0, 0x207);
}

static void check_result(u32 format, int blue)
{
    ldh_surface(8u | (1u << 8), RESULT, 32, 0x200000, 64, 8, 8);
    ldh_texture0(IMAGE, format == 12 ? 0xBB : format == 11 ? 0xBA : 0xA5, 2560, 720,
        2560u * (format == 12 ? 16u : format == 11 ? 8u : 4u));
    ldh_draw(FP_TEXTURE, 0x40, 0, 0, 0x207);
    u8 rgb[8 * 8 * 3]; u32 w = 0, h = 0;
    const int read = ldh_read_rgb(RESULT, "feedback-result.ppm", rgb, 8, 8, &w, &h) == 0 && w == 8 && h == 8;
    ldh_check("read actual GPU pixels", read);
    if (read) {
        const u8* left = rgb + (4 * 8 + 1) * 3;
        const u8* right = rgb + (4 * 8 + 6) * 3;
        printf("pixels left=%u/%u/%u right=%u/%u/%u\n",
            left[0], left[1], left[2], right[0], right[1], right[2]);
        ldh_check(blue ? "different declared layout still reads the other alias" :
            "current full-width feedback preserves red/green halves", blue ?
            left[0] < 3 && left[1] < 3 && left[2] > 250 &&
            right[0] < 3 && right[1] < 3 && right[2] > 250 :
            left[0] > 250 && left[1] < 3 && left[2] < 3 &&
            right[0] < 3 && right[1] > 250 && right[2] < 3);
    }
}

int main(int argc, char** argv)
{
    const char* scenario = argc > 1 ? argv[1] : "large-first";
    if (ldh_init(T)) return 2;
    ShowWindow((HWND)rsx_null_backend_get_hwnd(), SW_HIDE);
    ldh_setup_quad(0, 1, 1, 0, 0);
    rsx_test_fp_tex_r0_tc0(ldh_main + FP_TEXTURE, 0);
    const u32 format = !strcmp(scenario, "half") ? 11u : 8u;
    const int other_size = !strcmp(scenario, "other-size");
    const int other_pitch = !strcmp(scenario, "other-pitch");
    const int other_format = !strcmp(scenario, "other-format");
    const int other_remap = !strcmp(scenario, "other-remap");
    if (!strcmp(scenario, "newer-covering")) {
        solid(8, 1280, 720, 0, 0, 1);
        ldh_m(NV4097_SET_SURFACE_PITCH_A, 10240);
        ldh_draw(FP_COLOR, 0x40, 0, 0, 0x207);
        split(8);
        target(8, 1280, 720);
        ldh_m(NV4097_SET_SURFACE_PITCH_A, 10240);
        ldh_texture0(IMAGE, 0xA5, 1280, 720, 10240);
        ldh_draw(FP_TEXTURE, 0x40, 0, 0, 0x207);
        /* Copy the just-written current resource, not another address lookup. */
        const u32 current = current_surface();
        rsx_live_draw_flush();
        ldh_check("current target has the declared padded stride",
            current < g.n_surfaces && g.surfaces[current].w == 1280 &&
            g.surfaces[current].pitch == 10240);
        /* Sampling the current-sized descriptor after the draw selects that
         * exact current resource; copying it to RESULT makes its pixels visible. */
        ldh_surface(8u | (1u << 8), RESULT, 32, 0x200000, 64, 8, 8);
        ldh_texture0(IMAGE, 0xA5, 1280, 720, 10240);
        ldh_draw(FP_TEXTURE, 0x40, 0, 0, 0x207);
        u8 rgb[8 * 8 * 3]; u32 w = 0, h = 0;
        const int read = ldh_read_rgb(RESULT, "newer-covering.ppm", rgb, 8, 8, &w, &h) == 0;
        ldh_check("read covering-writer GPU result", read);
        const u8* p = rgb + (4 * 8 + 4) * 3;
        ldh_check("newer larger same-stride view supplies red top-left pixels",
            read && p[0] > 250 && p[1] < 3 && p[2] < 3);
        rsx_live_draw_flush();
        ldh_check("renderer remains ready after GPU readback", g.ready);
        rsx_live_draw_shutdown();
        return ldh_done(T);
    }
    if (!strcmp(scenario, "small-first")) {
        solid(format, 1280, 720, 0, 0, 1);
        split(format);
    } else {
        split(format);
        if (strcmp(scenario, "no-alias"))
            solid(other_format ? 11u : format, other_format ? 2560u : 1280u, 720, 0, 0, 1);
    }
    target(format, 2560, 720);
    ldh_texture0(IMAGE, format == 11 || other_format ? 0xBA : 0xA5,
        other_size ? 1280u : 2560u, 720,
        (other_size || other_pitch ? 1280u : 2560u) *
        (format == 11 || other_format ? 8u : 4u));
    if (other_remap) ldh_m(NV4097_SET_TEXTURE_CONTROL1, 0xAA93);
    ldh_draw(FP_TEXTURE, 0x40, 0, 0, 0x207);
    const int blue = other_size || other_pitch || other_format || other_remap;
    check_result(format, blue);
    /* A second queued feedback pass must snapshot the latest current image. */
    target(format, 2560, 720);
    ldh_draw(FP_TEXTURE, 0x40, 0, 0, 0x207);
    check_result(format, blue);
    rsx_live_draw_flush();
    ldh_check("renderer remains ready after GPU readback", g.ready);
    rsx_live_draw_shutdown();
    return ldh_done(T);
}
