/* Point size through the real draw engine and the Metal backend, on the GPU.
 * One white point at the centre of a 128x128 target, the pixels it lit counted.
 * No title data. Metal leaves a point's size undefined unless the vertex
 * function writes [[point_size]]; guest point sprites came out as huge blocks.
 *
 *   - SET_POINT_SIZE (0x1EE0) alone: a square of that many pixels;
 *   - never written: one pixel;
 *   - the vertex program's own o[6].x, when SET_VERTEX_ATTRIB_OUTPUT_MASK
 *     (0x1FF4) bit 5 is set, over the register;
 *   - a huge size: held at the 64 pixel ceiling, not the driver's.
 *
 * Built by CMake (build/test_metal_point_size) only where glslang and
 * spirv-cross were found, like test_metal_depth_clamp.
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

/* Wide enough that a 64 px point and the unclamped 5000 px one differ. */
#define TARGET 128u

typedef struct { u32 lit, w, h, minx, maxx, miny, maxy; } Lit;

/* Draw one point and count the lit pixels of the target. `reg` is
 * SET_POINT_SIZE (< 0: never written), `mask` the output mask (0 leaves it
 * unwritten), `vp_size` the size the program writes to o[6].x through
 * attribute 8's x. */
static Lit draw_point(float reg, u32 mask, float vp_size)
{
    Lit r = { 0, 0, 0, 0, 0, 0, 0 };
    if (rsx_draw_engine_init(TARGET, TARGET) != 0) { CHECK(0, "engine init"); return r; }
    method(0x0194, 0xFEED0000);                 /* CONTEXT_DMA_COLOR_A: local */
    method(0x0210, 0x40000);                    /* COLOR_A_OFFSET */
    method(0x020C, TARGET * 4);                 /* COLOR_A_PITCH */
    method(0x0200, TARGET << 16);
    method(0x0204, TARGET << 16);
    method(0x0208, 8u | (1u << 8));             /* A8R8G8B8, no zeta, linear */
    /* position, colour, texcoord: float32 x4, stride 48, in main memory */
    method(0x1740, 2u | (4u << 4) | (48u << 8)); method(0x1680, 0x80010000);
    method(0x1744, 2u | (4u << 4) | (48u << 8)); method(0x1684, 0x80010010);
    method(0x1760, 2u | (4u << 4) | (48u << 8)); method(0x16A0, 0x80010020);
    const float v[12] = { 0, 0, .5f, 1,  1, 1, 1, 1,  vp_size, 0, 0, 1 };
    for (u32 i = 0; i < 12; i++) be_float(vm_base + 0x10000 + i * 4, v[i]);
    u8 vp[48];
    rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);       /* MOV o0, v0 */
    rsx_test_vp_mov_out(vp + 16, 1, RSX_TEST_VP_SWZ_IDENT, 1, 0);  /* MOV o1, v1 */
    rsx_test_vp_mov_out(vp + 32, 8, RSX_TEST_VP_SWZ_IDENT, 6, 1);  /* MOV o6, v8 */
    method(0x1E9C, 0);
    for (u32 i = 0; i < 12; i++) { u32 w; memcpy(&w, vp + i * 4, 4); method(0x0B80 + i * 4, w); }
    method(0x1EA0, 0);
    rsx_test_fp_mov_r0_col0(vm_base + 0x60000, RSX_TEST_FP_SWZ_IDENT);
    method(0x08E4, 0x60002);
    method(0x1D60, 64);
    method(0x1D8C, 0);                          /* clear colour: transparent black */
    method(0x1D94, 0xF0);                       /* clear colour */
    if (reg >= 0.0f) method(0x1EE0, fbits(reg));
    if (mask) method(0x1FF4, mask);
    method(0x1808, RSX_PRIMITIVE_POINTS); method(0x1814, 0u); method(0x1808, 0);

    s_engine_backend.submit_and_wait(NULL, 0);
    const u32 surface = g.surfaces[0].handle;
    id<MTLTexture> t = eng_obj(surface);
    if (t) {
        const u32 w = (u32)[t width], h = (u32)[t height];
        u32* px = calloc((size_t)w * h, 4);
        [t getBytes:px bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
        r.w = w; r.h = h; r.minx = w; r.miny = h;
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++)
                if (px[y * w + x] & 0x00FFFFFFu) {
                    r.lit++;
                    if (x < r.minx) r.minx = x;
                    if (x > r.maxx) r.maxx = x;
                    if (y < r.miny) r.miny = y;
                    if (y > r.maxy) r.maxy = y;
                }
        free(px);
    }
    CHECK(t != nil, "colour target read back (%ux%u)", r.w, r.h);
    rsx_draw_engine_shutdown();
    return r;
}

static int is_square(const Lit* l, u32 side)
{
    return l->lit == side * side && l->maxx - l->minx + 1 == side && l->maxy - l->miny + 1 == side;
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

        Lit a = draw_point(8.0f, 0, 3.0f);
        CHECK(is_square(&a, 8), "SET_POINT_SIZE 8: an 8x8 square (%u lit, %ux%u)", a.lit,
              a.maxx - a.minx + 1, a.maxy - a.miny + 1);

        Lit b = draw_point(-1.0f, 0, 3.0f);
        CHECK(is_square(&b, 1), "size never written: one pixel (%u lit)", b.lit);

        /* The mask's point-size bit (0x20) hands it to the program (o[6].x =
         * 3); the register's 8 is ignored. */
        Lit c = draw_point(8.0f, RSX_VP_OUTPUT_MASK_POINT_SIZE, 3.0f);
        CHECK(is_square(&c, 3), "mask bit 5: the program's 3, not the register's 8 (%u lit)", c.lit);

        /* A mask without bit 5 (here COL0..FOGC, bits 0-4) leaves the
         * register's size. */
        Lit d = draw_point(8.0f, 0x1Fu, 3.0f);
        CHECK(is_square(&d, 8), "mask without bit 5: the register's 8 (%u lit)", d.lit);

        Lit e = draw_point(5000.0f, 0, 3.0f);
        CHECK(is_square(&e, 64), "size 5000: held at the 64 px ceiling (%u lit, %ux%u)", e.lit,
              e.maxx - e.minx + 1, e.maxy - e.miny + 1);

        rsx_draw_engine_set_backend(NULL);
        free(vm_base);
    }
    printf("%u checks, %u failed (point size)\n", checks, failures);
    return failures ? 1 : 0;
}
