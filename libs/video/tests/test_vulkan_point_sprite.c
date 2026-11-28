/* Synthetic points through the production engine, Vulkan and readback.
 * No device/translator is a failure: CI supplies lavapipe and glslang. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../rsx_vulkan_backend.c"
#include "../rsx_draw_engine.c"
#include "../rsx_test_programs.h"
#include <assert.h>
u8* vm_base; u32 ppu_vm_size;
u32 cellGcmResolveLocated(int local, u32 offset) { (void)local; return offset; }
u32 cellGcmResolveIO(u32 offset) { return offset < ppu_vm_size ? offset : 0; }
void cellGcm_replay_display_buffers_to_engine(void) {}
static rsx_backend* fixture_backend;
void rsx_set_backend(rsx_backend* b) { fixture_backend=b; }
rsx_backend* rsx_get_backend(void) { return fixture_backend; }
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
static Lit draw_point(float reg, u32 mask, float vp_size, u32 control, u32 aa, int coords)
{
    Lit r = { 0, 0, 0, 0, 0, 0, 0 };
    if (rsx_draw_engine_init(TARGET, TARGET) != 0) { CHECK(0, "engine init"); return r; }
    method(0x0194, 0xFEED0000);                 /* CONTEXT_DMA_COLOR_A: local */
    method(0x0210, 0x40000);                    /* COLOR_A_OFFSET */
    method(0x020C, TARGET * 4);                 /* COLOR_A_PITCH */
    method(0x0200, TARGET << 16);
    method(0x0204, TARGET << 16);
    method(0x0208, 8u | (1u << 8) | (aa << 12));             /* A8R8G8B8, no zeta, linear */
    /* position, colour, texcoord: float32 x4, stride 48, in main memory */
    method(0x1740, 2u | (4u << 4) | (48u << 8)); method(0x1680, 0x80010000);
    method(0x1744, 2u | (4u << 4) | (48u << 8)); method(0x1684, 0x80010010);
    method(0x1760, 2u | (4u << 4) | (48u << 8)); method(0x16A0, 0x80010020);
    const float v[12] = { 0, 0, .5f, 1,  1, 1, 1, 1,  vp_size, 0, 0, 1 };
    for (u32 i = 0; i < 12; i++) be_float(vm_base + 0x10000 + i * 4, v[i]);
    u8 vp[64];
    rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);       /* MOV o0, v0 */
    rsx_test_vp_mov_out(vp + 16, 1, RSX_TEST_VP_SWZ_IDENT, 1, 0);  /* MOV o1, v1 */
    rsx_test_vp_mov_out(vp + 32, 8, RSX_TEST_VP_SWZ_IDENT, 6, 0);
    rsx_test_vp_mov_out(vp + 48, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);  /* MOV o6, v8 */
    method(0x1E9C, 0);
    for (u32 i = 0; i < 16; i++) { u32 w; memcpy(&w, vp + i * 4, 4); method(0x0B80 + i * 4, w); }
    method(0x1EA0, 0);
    rsx_test_fp_mov_r0_col0(vm_base + 0x60000, RSX_TEST_FP_SWZ_IDENT);
    /* Keep blue opaque so a valid (0,0) coordinate is not mistaken for clear. */
    if (coords) rsx_test_fp_instr(vm_base + 0x60000,
        RSX_TEST_FP_OPC(RSX_TEST_FP_OP_MOV) | RSX_TEST_FP_MASK_XYZW |
        RSX_TEST_FP_INSRC_TC0 | RSX_TEST_FP_END,
        RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ(0,1,3,3));
    method(0x1EE8, control);
    method(0x08E4, 0x60002);
    method(0x1D60, 64);
    method(0x1D8C, 0);                          /* clear colour: transparent black */
    method(0x1D94, 0xF0);                       /* clear colour */
    if (reg >= 0.0f) method(0x1EE0, fbits(reg));
    if (mask) method(0x1FF4, mask);
    method(0x1808, RSX_PRIMITIVE_POINTS); method(0x1814, 0u); method(0x1808, 0);

    vk_eng_submit_and_wait(NULL, 0);
    const u32 surface = g.surfaces[0].handle;
    vk_eng_obj* t = vk_eng_get(surface, VK_ENG_COLOR);
    if (t) {
        const u32 w = t->w, h = t->h;
        u32* px = calloc((size_t)w * h, 4);
        vk_eng_readback(NULL, surface, 0, 0, w, h, px, w * 4);
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
        if (coords && (control & 0x101u) == 0x101u) {
            const u32 midy = (r.miny + r.maxy) / 2;
            const u8* left = (const u8*)&px[midy*w + r.minx];
            const u8* right = (const u8*)&px[midy*w + r.maxx];
            CHECK(left[0] < right[0] && left[2] == 255 && right[2] == 255,
                  "selected TC0 has a horizontal PointCoord gradient (%u -> %u, coverage blue %u/%u)", left[0],right[0],left[2],right[2]);
        } else if (coords) {
            const u8* centre = (const u8*)&px[(h/2)*w + w/2];
            CHECK(centre[0] == 255 && centre[1] == 0,
                  "disabled or unselected TC0 keeps the vertex varying");
        }
        free(px);
    }
    CHECK(t != NULL, "colour target read back (%ux%u)", r.w, r.h);
    rsx_draw_engine_shutdown();
    return r;
}

static int is_square(const Lit* l, u32 side)
{
    return l->lit == side * side && l->maxx - l->minx + 1 == side && l->maxy - l->miny + 1 == side;
}

int main(void)
{
    setenv("PS3RECOMP_RSX_ENGINE", "dispatch", 1);
    setenv("PS3RECOMP_VK_GUEST_PROGRAMS", "1", 1);
    setenv("PS3RECOMP_VK_WINDOW", "0", 1);
    vm_base = calloc(1, 1u << 20); ppu_vm_size = 1u << 20;
    if (rsx_vulkan_backend_init(TARGET,TARGET,"point sprite regression") != 0) return 1;
    CHECK(s_vk.eng_active && s_g.on && s_vk.large_points_ok, "engine, translator and large points available");
    if (failures) return 1;
    Lit a=draw_point(8,0,3,0,0,0); CHECK(is_square(&a,8), "register size: 8x8 (%u lit)",a.lit);
    Lit b=draw_point(-1,0,3,0,0,0); CHECK(is_square(&b,1), "default size: 1 pixel (%u lit)",b.lit);
    Lit c=draw_point(8,0x20,3,0,0,0); CHECK(is_square(&c,3), "VP size overrides register: 3x3 (%u lit)",c.lit);
    Lit d=draw_point(5000,0,3,0,0,0); CHECK(is_square(&d,64), "guest ceiling: 64x64 (%u lit)",d.lit);
    Lit e=draw_point(8,0,3,0x101,0,1); CHECK(is_square(&e,8), "selected coordinates: 8x8 (%u lit)",e.lit);
    Lit odd=draw_point(3,0,3,0x101,0,1); CHECK(is_square(&odd,3), "odd-sized selected coordinates: 3x3 (%u lit)",odd.lit);
    Lit f=draw_point(8,0,3,0x100,0,1); CHECK(is_square(&f,8), "sprite disabled: 8x8 (%u lit)",f.lit);
    Lit h=draw_point(8,0,3,0x1001,0,1); CHECK(is_square(&h,8), "unselected TC0: 8x8 (%u lit)",h.lit);
    Lit i=draw_point(8,0,3,0x101,3,1); CHECK(i.lit==128 && i.maxx-i.minx+1==16 && i.maxy-i.miny+1==8, "2x1 sample grid: 16x8 (%u lit)",i.lit);
    Lit j=draw_point(8,0,3,0x101,4,1); CHECK(is_square(&j,16), "2x2 sample grid: 16x16 (%u lit)",j.lit);
    rsx_vulkan_backend_shutdown(); free(vm_base);
    printf("%u checks, %u failed (Vulkan point sprites)\n",checks,failures);
    return failures ? 1 : 0;
}
