/* SET_ZMIN_MAX_CONTROL's depth clamp through the real draw engine and the
 * Metal backend, on the GPU: a full-screen triangle at clip-space z -0.5, in
 * front of the near plane, drawn into a Z24S8 zeta cleared to 1.0, then the
 * depth it left read back. No title data.
 *
 *   - never written (reset value 1, rsx_dispatch_init) or 0x1: clipped, the
 *     clear's 1.0 stays;
 *   - 0x10 (clipping off, clamp on -- how shadow-map passes draw casters),
 *     0x11 (clamp on) and 0x0 (clipping off): clamped to 0.0;
 *   - PS3RECOMP_ENG_DEPTH_CLAMP=0 (macos.yml's second run): 1.0 in every
 *     case.
 *
 * Built by CMake (build/test_metal_depth_clamp) only where glslang and
 * spirv-cross were found, like test_metal_poly_offset.
 */
#include "rsx_metal_backend.m"
#define eng_texture_upload test_engine_texture_upload
#define eng_present test_engine_present
#include "rsx_draw_engine.c"
#undef eng_texture_upload
#undef eng_present
#include "rsx_test_programs.h"
#include <math.h>

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
static u32 fbits(float f) { u32 u; memcpy(&u, &f, 4); return u; }

/* The depth the draw left at the centre of a 64x64 target; zmm < 0 leaves
 * ZMIN_MAX_CONTROL unwritten. */
static float draw_depth(int zmm)
{
    if (rsx_draw_engine_init(64, 64) != 0) { CHECK(0, "engine init"); return -1.0f; }
    method(0x0194, 0xFEED0000);                 /* CONTEXT_DMA_COLOR_A: local */
    method(0x0198, 0xFEED0000);                 /* CONTEXT_DMA_ZETA: local */
    method(0x0210, 0x40000);                    /* COLOR_A_OFFSET */
    method(0x020C, 64 * 4);                     /* COLOR_A_PITCH */
    method(0x0214, 0x80000);                    /* ZETA_OFFSET */
    method(0x022C, 64 * 4);                     /* ZETA_PITCH */
    method(0x0200, 64u << 16);
    method(0x0204, 64u << 16);
    method(0x0208, 8u | (2u << 5) | (1u << 8)); /* A8R8G8B8, Z24S8, linear */
    /* position, colour, texcoord: float32 x4, stride 48, in main memory */
    method(0x1740, 2u | (4u << 4) | (48u << 8)); method(0x1680, 0x80010000);
    method(0x1744, 2u | (4u << 4) | (48u << 8)); method(0x1684, 0x80010010);
    method(0x1760, 2u | (4u << 4) | (48u << 8)); method(0x16A0, 0x80010020);
    const float v[36] = { -1, -1, -.5f, 1,  .25f, .5f, .75f, 1,  0, 0, 0, 1,
                           3, -1, -.5f, 1,  .25f, .5f, .75f, 1,  0, 0, 0, 1,
                          -1,  3, -.5f, 1,  .25f, .5f, .75f, 1,  0, 0, 0, 1 };
    for (u32 i = 0; i < 36; i++) be_float(vm_base + 0x10000 + i * 4, v[i]);
    u8 vp[48];
    rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 1, RSX_TEST_VP_SWZ_IDENT, 1, 0);
    rsx_test_vp_mov_out(vp + 32, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
    method(0x1E9C, 0);
    for (u32 i = 0; i < 12; i++) { u32 w; memcpy(&w, vp + i * 4, 4); method(0x0B80 + i * 4, w); }
    method(0x1EA0, 0);
    rsx_test_fp_mov_r0_col0(vm_base + 0x60000, RSX_TEST_FP_SWZ_IDENT);
    method(0x08E4, 0x60002);
    method(0x1D60, 64);
    method(0x0A6C, 0x207);                      /* DEPTH_FUNC ALWAYS */
    method(0x0A70, 1);                          /* DEPTH_MASK */
    method(0x0A74, 1);                          /* DEPTH_TEST_ENABLE */
    method(0x1D8C, 0xFFFFFF00u);                /* depth 1.0 */
    method(0x1D94, 0x03);                       /* clear depth and stencil */
    if (zmm >= 0) method(0x1D78, (u32)zmm);     /* SET_ZMIN_MAX_CONTROL */
    method(0x1808, RSX_PRIMITIVE_TRIANGLES); method(0x1814, 2u << 24); method(0x1808, 0);

    s_engine_backend.submit_and_wait(NULL, 0);
    float z[64 * 64];
    const u32 ds = eng_depth_snapshot(NULL, g.zdepths[0].handle, 64, 64);
    s_engine_backend.submit_and_wait(NULL, 0);
    float out = -1.0f;
    if (ds) {
        [eng_obj(ds) getBytes:z bytesPerRow:64 * 4 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
        out = z[32 * 64 + 32];
        eng_obj_release(NULL, ds);
    }
    CHECK(ds != 0, "depth snapshot of the zeta");
    rsx_draw_engine_shutdown();
    return out;
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

        const char* e = getenv("PS3RECOMP_ENG_DEPTH_CLAMP");
        const int on = !(e && *e && atoi(e) == 0);
        const float clamped = on ? 0.0f : 1.0f;
        const float reset = draw_depth(-1);
        const float clip  = draw_depth(0x01);
        const float c10   = draw_depth(0x10);
        const float c11   = draw_depth(0x11);
        const float c00   = draw_depth(0x00);
        CHECK(reset == 1.0f, "ZMIN_MAX_CONTROL never written: clipped, depth %.9f", reset);
        CHECK(clip == 1.0f, "0x1 (clip): depth %.9f", clip);
        CHECK(c10 == clamped, "0x10 (no clip, clamp): depth %.9f, want %.1f", c10, clamped);
        CHECK(c11 == clamped, "0x11 (clip and clamp): depth %.9f, want %.1f", c11, clamped);
        CHECK(c00 == clamped, "0x0 (no clip): depth %.9f, want %.1f", c00, clamped);

        rsx_draw_engine_set_backend(NULL);
        free(vm_base);
    }
    printf("%u checks, %u failed (depth clamp %s)\n", checks, failures, getenv("PS3RECOMP_ENG_DEPTH_CLAMP") ? "off" : "on");
    return failures ? 1 : 0;
}
