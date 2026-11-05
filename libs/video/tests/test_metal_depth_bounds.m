/* The depth-bounds test through the real draw engine and the Metal backend,
 * which has none of its own, on the GPU. No title data.
 *
 * A Z24S8 zeta gets depth 0.25 in its left half and 0.75 in its right half
 * (red), then a full-screen green triangle is drawn with the depth test
 * ALWAYS, no depth write and SET_DEPTH_BOUNDS_TEST_ENABLE on. The bounds
 * look at the STORED depth, so:
 *   - [0.5, 1.0]: only the right half turns green;
 *   - [0.0, 0.5]: only the left half;
 *   - [0.0, 1.0] (no bound) or the test off: both halves;
 *   - PS3RECOMP_ENG_DEPTH_BOUNDS=0 (macos.yml's second run): both halves in
 *     every case, as before the test was emulated.
 *
 * Built by CMake (build/test_metal_depth_bounds) only where glslang and
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

#define VB 0x10000u

/* Two triangles over x in [x0, x1], the full height, at depth z. */
static void rect(float x0, float x1, float z, float r, float gr, float b)
{
    const float c[4] = { r, gr, b, 1 };
    const float p[6][2] = { {x0,-1}, {x1,-1}, {x0,1}, {x1,-1}, {x1,1}, {x0,1} };
    for (u32 i = 0; i < 6; i++) {
        const float v[12] = { p[i][0], p[i][1], z, 1, c[0], c[1], c[2], c[3], 0, 0, 0, 1 };
        for (u32 k = 0; k < 12; k++) be_float(vm_base + VB + (i * 12 + k) * 4, v[k]);
    }
    method(0x1808, RSX_PRIMITIVE_TRIANGLES); method(0x1814, 5u << 24); method(0x1808, 0);
}

/* The green draw's result: bit 0 = left half green, bit 1 = right half. */
static int bounded(int enable, float mn, float mx)
{
    if (rsx_draw_engine_init(64, 64) != 0) { CHECK(0, "engine init"); return -1; }
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
    method(0x1740, 2u | (4u << 4) | (48u << 8)); method(0x1680, 0x80000000u | VB);
    method(0x1744, 2u | (4u << 4) | (48u << 8)); method(0x1684, 0x80000000u | (VB + 16));
    method(0x1760, 2u | (4u << 4) | (48u << 8)); method(0x16A0, 0x80000000u | (VB + 32));
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
    method(0x0A74, 1);                          /* DEPTH_TEST_ENABLE */
    method(0x1D8C, 0xFFFFFF00u);                /* depth 1.0 */
    method(0x1D94, 0xF3);                       /* clear colour, depth and stencil */
    method(0x0A70, 1);                          /* DEPTH_MASK on: lay the depth down */
    rect(-1.0f, 0.0f, 0.25f, 1, 0, 0);
    rect( 0.0f, 1.0f, 0.75f, 1, 0, 0);
    method(0x0A70, 0);                          /* DEPTH_MASK off */
    method(0x0380, enable ? 1u : 0u);           /* DEPTH_BOUNDS_TEST_ENABLE */
    method(0x0384, fbits(mn));
    method(0x0388, fbits(mx));
    rect(-1.0f, 1.0f, 0.5f, 0, 1, 0);

    s_engine_backend.submit_and_wait(NULL, 0);
    static u8 px[64 * 64 * 4];
    memset(px, 0, sizeof px);
    s_engine_backend.readback(NULL, g.surfaces[0].handle, 0, 0, 64, 64, px, 64 * 4);
    const u8* l = px + (32 * 64 + 16) * 4;
    const u8* r = px + (32 * 64 + 48) * 4;
    rsx_draw_engine_shutdown();
    const int lg = l[1] > 200 && l[0] < 50, rg = r[1] > 200 && r[0] < 50;
    const int lr = l[0] > 200 && l[1] < 50, rr = r[0] > 200 && r[1] < 50;
    CHECK((lg || lr) && (rg || rr), "both halves drawn (left %u,%u,%u right %u,%u,%u)",
          l[0], l[1], l[2], r[0], r[1], r[2]);
    return lg | (rg << 1);
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

        const char* e = getenv("PS3RECOMP_ENG_DEPTH_BOUNDS");
        const int on = !(e && *e && atoi(e) == 0);
        const int far_half  = bounded(1, 0.5f, 1.0f);
        const int near_half = bounded(1, 0.0f, 0.5f);
        const int unbound   = bounded(1, 0.0f, 1.0f);
        const int off       = bounded(0, 0.5f, 1.0f);
        CHECK(far_half == (on ? 2 : 3), "bounds [0.5, 1]: green halves 0x%X, want 0x%X", far_half, on ? 2 : 3);
        CHECK(near_half == (on ? 1 : 3), "bounds [0, 0.5]: green halves 0x%X, want 0x%X", near_half, on ? 1 : 3);
        CHECK(unbound == 3, "bounds [0, 1]: green halves 0x%X, want 0x3", unbound);
        CHECK(off == 3, "test off: green halves 0x%X, want 0x3", off);

        rsx_draw_engine_set_backend(NULL);
        free(vm_base);
    }
    printf("%u checks, %u failed (depth bounds %s)\n", checks, failures, getenv("PS3RECOMP_ENG_DEPTH_BOUNDS") ? "off" : "on");
    return failures ? 1 : 0;
}
