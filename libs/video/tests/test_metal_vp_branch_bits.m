/* SET_TRANSFORM_BRANCH_BITS through the real draw engine and the Metal
 * backend, on the GPU: one vertex program whose BRB picks the colour, drawn
 * under three values of the bits, the colour read back after each. No title
 * data.
 *
 *   0 MOV o0, v0        position
 *   1 MOV o1, v1        COLOR0 = green
 *   2 BRB b5 -> 4       taken when bit 5 is set
 *   3 MOV o1, v2        COLOR0 = red
 *   4 MOV o0, v0 (END)
 *
 * The program reads the bits at run time (vp_posoffset.w), so all three draws
 * share one translated and compiled vertex function; when the decompiler
 * baked them in, each value was a program of its own (Resistance 3: 70 of a
 * 543-slot program, ~0.4 s each on the RSX thread).
 *
 * Built by CMake (build/test_metal_vp_branch_bits) only where glslang and
 * spirv-cross were found, like test_metal_depth_clamp.
 */
#include "rsx_metal_backend.m"
#define eng_texture_upload test_engine_texture_upload
#define eng_present test_engine_present
#include "rsx_draw_engine.c"
#undef eng_texture_upload
#undef eng_present
#include "rsx_test_programs.h"

u8* vm_base; u32 ppu_vm_size;
u32 cellGcmResolveLocated(int local, u32 offset) { (void)local; return offset; }
u32 cellGcmResolveIO(u32 offset) { return offset < ppu_vm_size ? offset : 0; }
void cellGcm_replay_display_buffers_to_engine(void) {}
float cellAvconfExt_display_gamma(void) { return 1.0f; }
/* The backend's own (non-engine) draw path, which this test never takes. */
u32 cellGcmResolveOffset(u32 offset) { return offset; }
int cellGcmOffsetIsDisplay(u32 offset) { (void)offset; return 0; }
void rsx_fetch_attrib(const rsx_state* st, int idx, u32 vi, float out[4])
{ (void)st; (void)idx; (void)vi; out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f; }

static unsigned checks, failures;
#define CHECK(x, ...) do { checks++; if (x) { printf("[PASS] " __VA_ARGS__); printf("\n"); } \
                           else { failures++; printf("[FAIL] " __VA_ARGS__); printf("\n"); } } while (0)

static void method(u32 a, u32 v) { rsx_draw_engine_method(a, v); }
static void be_float(u8* p, float f) { u32 v; memcpy(&v, &f, 4); p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* Draw the full-screen triangle under `bits`; the R, G, B, A bytes of the
 * centre pixel go to px. */
static void draw_with_bits(u32 bits, u8 px[4])
{
    method(0x1FF8, bits);                       /* SET_TRANSFORM_BRANCH_BITS */
    method(0x1D90, 0x000000FFu);                /* clear colour: blue */
    method(0x1D94, 0xF0);                       /* clear RGBA */
    method(0x1808, RSX_PRIMITIVE_TRIANGLES); method(0x1814, 2u << 24); method(0x1808, 0);
    s_engine_backend.submit_and_wait(NULL, 0);
    static u8 pixels[64 * 64 * 4];
    s_engine_backend.readback(NULL, g.surfaces[0].handle, 0, 0, 64, 64, pixels, 64 * 4);
    memcpy(px, pixels + (32 * 64 + 32) * 4, 4);
}

int main(void)
{
    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice();
        if (!s_dev) { puts("no Metal device: skipped"); return 0; }
        s_queue = [s_dev newCommandQueue];
        s_headless = 1; s_guest_shaders = 1;
        vm_base = calloc(1, 1u << 20); ppu_vm_size = 1u << 20;
        rsx_draw_engine_set_backend(&s_engine_backend);
        if (rsx_draw_engine_init(64, 64) != 0) { CHECK(0, "engine init"); return 1; }

        method(0x0194, 0xFEED0000);             /* CONTEXT_DMA_COLOR_A: local */
        method(0x0210, 0x40000);                /* COLOR_A_OFFSET */
        method(0x020C, 64 * 4);                 /* COLOR_A_PITCH */
        method(0x0200, 64u << 16);
        method(0x0204, 64u << 16);
        method(0x0208, 8u | (1u << 8));         /* A8R8G8B8, no zeta, linear */
        /* position, green, red: float32 x4, stride 48, in main memory */
        method(0x1740, 2u | (4u << 4) | (48u << 8)); method(0x1680, 0x80010000);
        method(0x1744, 2u | (4u << 4) | (48u << 8)); method(0x1684, 0x80010010);
        method(0x1748, 2u | (4u << 4) | (48u << 8)); method(0x1688, 0x80010020);
        const float v[36] = { -1, -1, .5f, 1,  0, 1, 0, 1,  1, 0, 0, 1,
                               3, -1, .5f, 1,  0, 1, 0, 1,  1, 0, 0, 1,
                              -1,  3, .5f, 1,  0, 1, 0, 1,  1, 0, 0, 1 };
        for (u32 i = 0; i < 36; i++) be_float(vm_base + 0x10000 + i * 4, v[i]);

        u8 vp[5 * 16];
        rsx_test_vp_mov_out(vp + 0 * 16, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
        rsx_test_vp_mov_out(vp + 1 * 16, 1, RSX_TEST_VP_SWZ_IDENT, 1, 0);
        rsx_test_vp_branch(vp + 2 * 16, 0x11, 4, 5, 1);
        rsx_test_vp_mov_out(vp + 3 * 16, 2, RSX_TEST_VP_SWZ_IDENT, 1, 0);
        rsx_test_vp_mov_out(vp + 4 * 16, 0, RSX_TEST_VP_SWZ_IDENT, 0, 1);
        method(0x1E9C, 0);                      /* TRANSFORM_PROGRAM_LOAD */
        for (u32 i = 0; i < 20; i++) { u32 w; memcpy(&w, vp + i * 4, 4); method(0x0B80 + i * 4, w); }
        method(0x1EA0, 0);                      /* TRANSFORM_PROGRAM_START */
        rsx_test_fp_mov_r0_col0(vm_base + 0x60000, RSX_TEST_FP_SWZ_IDENT);
        method(0x08E4, 0x60002);
        method(0x1D60, 64);

        static const u8 green[4] = { 0, 255, 0, 255 }, red[4] = { 255, 0, 0, 255 };
        /* 0xFFFFFFDF is a NaN as a float: the bits must reach the shader as
         * they are. */
        u8 px[4];
        draw_with_bits(0x00000020u, px);
        CHECK(!memcmp(px, green, 4), "bit 5 set: the branch skips the red write (RGBA %u %u %u %u)",
              px[0], px[1], px[2], px[3]);
        const u32 funcs = s_eng_func_count;
        draw_with_bits(0xFFFFFFDFu, px);
        CHECK(!memcmp(px, red, 4), "every bit but 5: falls through to red (RGBA %u %u %u %u)",
              px[0], px[1], px[2], px[3]);
        draw_with_bits(0x80000020u, px);
        CHECK(!memcmp(px, green, 4), "bit 5 and bit 31: green again (RGBA %u %u %u %u)",
              px[0], px[1], px[2], px[3]);
        CHECK(s_eng_func_count == funcs,
              "no shader compiled for the other values of the bits (%u, then %u)",
              funcs, s_eng_func_count);
        CHECK(s_eng_dropped == 0, "no dropped records");

        rsx_draw_engine_shutdown();
        rsx_draw_engine_set_backend(NULL);
        free(vm_base);
    }
    printf("%u checks, %u failed\n", checks, failures);
    return failures ? 1 : 0;
}
