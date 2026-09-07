/*
 * ps3recomp - the platform-neutral halves of the RSX draw engine
 *
 * Everything here runs with no GPU and no host graphics API: the engine is
 * driven through rsx_draw_engine_method(), exactly as the FIFO walker drives
 * it, against a stub backend that records what it was asked for. That is the
 * only way to check the four pieces whose bugs are silent -- a surface that is
 * reallocated when it should not be, a texture unit that samples guest memory
 * instead of the render target at the same address, a texture that never
 * updates because its content hash is only taken once, and a pipeline that is
 * recompiled every time a constant changes.
 *
 * Builds and runs on Linux as well as macOS; nothing in it is Apple-specific.
 */
#include "rsx_draw_engine.h"
#include "rsx_primitives.h"   /* RSX_PRIMITIVE_* */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (cond) {                                                           \
            printf("[PASS] " __VA_ARGS__); printf("\n");                      \
        } else {                                                              \
            printf("[FAIL] " __VA_ARGS__);                                    \
            printf("   (%s:%d)\n", __FILE__, __LINE__);                       \
            g_failures++;                                                     \
        }                                                                     \
    } while (0)

/* ---- the guest memory the engine reads ---------------------------------- */
/*
 * Two number spaces, as on the hardware: an RSX offset means one thing in the
 * IO-mapped main-memory context and another in local video memory. The test's
 * map puts main at the bottom of the buffer and local above it, which is what
 * lets the surface-keying check bind two different objects at one offset.
 */
#define GUEST_BYTES     (16u << 20)
#define GUEST_LOCAL_EA  (8u << 20)

u8* vm_base;
u32 ppu_vm_size;

u32 cellGcmResolveLocated(int local, u32 offset)
{
    return local ? GUEST_LOCAL_EA + offset : offset;
}

u32 cellGcmResolveIO(u32 offset)
{
    return offset < GUEST_LOCAL_EA ? offset : 0;
}

/* ---- the stub backend ---------------------------------------------------- */

#define STUB_MAX_OBJECTS 8192
/* Colour clears kept by surface, so a test can say which targets were hit. */
#define STUB_MAX_CLEARED 8

typedef struct {
    u32 next_handle;
    u8  alive[STUB_MAX_OBJECTS];

    int n_texture_create, n_texture_release, n_texture_upload;
    int n_color_create, n_color_release, n_color_seeded;
    int n_depth_create, n_depth_snapshot;
    int n_pipeline_create;
    int n_draw, n_clear_color, n_clear_ds, n_present;
    u32 cleared[STUB_MAX_CLEARED];

    /* the last draw's bindings */
    u32 bound_tex[RSX_BE_MAX_TEXTURES];
    u32 bound_mask;
    u32 bound_rt[RSX_BE_MAX_COLOR_TARGETS];
    u32 bound_rt_count;
    u32 pipeline_rt_count;
    u32 bound_surface, bound_depth, bound_pipeline;
    u32 draw_vertices, draw_indices, draw_stride;
    rsx_topology draw_topology;
    u32 scissor[4];
    u32 stencil_ref;
    u32 last_created_texture, last_created_color;
    u32 last_released;
} StubState;

static StubState stub;

static u32 stub_handle(void)
{
    if (stub.next_handle + 1 >= STUB_MAX_OBJECTS) return 0;
    const u32 h = ++stub.next_handle;
    stub.alive[h] = 1;
    return h;
}

static int stub_init(void* u, u32 w, u32 h) { (void)u; (void)w; (void)h; return 0; }
static void stub_shutdown(void* u) { (void)u; }
static void stub_submit(void* u, u32 reason) { (void)u; (void)reason; }

static u32 stub_texture_create(void* u, rsx_be_format f, u32 w, u32 h, u32 mips,
                               u32 faces, u32 remap, u32 rsx_fmt)
{
    (void)u; (void)f; (void)w; (void)h; (void)mips; (void)faces;
    (void)remap; (void)rsx_fmt;
    stub.n_texture_create++;
    stub.last_created_texture = stub_handle();
    return stub.last_created_texture;
}

static void stub_texture_upload(void* u, u32 t, u32 face, u32 mip, u32 w, u32 h,
                                const void* src, u32 row, u32 rows)
{
    (void)u; (void)t; (void)face; (void)mip; (void)w; (void)h;
    (void)src; (void)row; (void)rows;
    stub.n_texture_upload++;
}

static void stub_release(void* u, u32 handle)
{
    (void)u;
    if (handle && handle < STUB_MAX_OBJECTS) stub.alive[handle] = 0;
    stub.last_released = handle;
}

static void stub_texture_release(void* u, u32 handle)
{ stub.n_texture_release++; stub_release(u, handle); }

static u32 stub_color_create(void* u, rsx_be_format f, u32 w, u32 h,
                             const void* seed, u32 seed_row)
{
    (void)u; (void)f; (void)w; (void)h; (void)seed_row;
    stub.n_color_create++;
    if (seed) stub.n_color_seeded++;
    stub.last_created_color = stub_handle();
    return stub.last_created_color;
}

static void stub_color_release(void* u, u32 handle)
{ stub.n_color_release++; stub_release(u, handle); }

static u32 stub_surface_view(void* u, u32 surface, u32 remap, u32 fmt)
{ (void)u; (void)remap; (void)fmt; return surface; }

static u32 stub_depth_create(void* u, u32 w, u32 h)
{ (void)u; (void)w; (void)h; stub.n_depth_create++; return stub_handle(); }

static u32 stub_depth_snapshot(void* u, u32 d, u32 w, u32 h)
{ (void)u; (void)d; (void)w; (void)h; stub.n_depth_snapshot++; return stub_handle(); }

static u32 stub_pipeline_create(void* u, const char* vs, const char* ps,
                                const rsx_be_render_state* rs,
                                const rsx_vertex_layout_plan* layout,
                                u32 stride, rsx_be_format rt, u32 rt_count)
{
    (void)u; (void)rs; (void)layout; (void)stride; (void)rt;
    if (!vs || !ps || !*vs || !*ps) return 0;
    stub.n_pipeline_create++;
    stub.pipeline_rt_count = rt_count;
    return stub_handle();
}

static void stub_pipeline_release(void* u, u32 p) { (void)u; (void)p; }

static void stub_bind_targets(void* u, const u32* s, u32 count, u32 d)
{
    (void)u;
    stub.bound_surface = count ? s[0] : 0;
    stub.bound_rt_count = count;
    for (u32 i = 0; i < RSX_BE_MAX_COLOR_TARGETS; i++)
        stub.bound_rt[i] = (i < count) ? s[i] : 0;
    stub.bound_depth = d;
}
static void stub_bind_pipeline(void* u, u32 p) { (void)u; stub.bound_pipeline = p; }
static void stub_bind_vs(void* u, const void* d, u32 n) { (void)u; (void)d; (void)n; }
static void stub_bind_ps(void* u, const void* d, u32 n) { (void)u; (void)d; (void)n; }

static void stub_bind_textures(void* u, const u32* tex,
                               const rsx_be_sampler_desc* smp, u32 mask)
{
    (void)u; (void)smp;
    memcpy(stub.bound_tex, tex, sizeof stub.bound_tex);
    stub.bound_mask = mask;
}

static void stub_bind_vtextures(void* u, const u32* tex,
                                const rsx_be_sampler_desc* smp, u32 mask)
{ (void)u; (void)tex; (void)smp; (void)mask; }

static void stub_viewport(void* u, float x, float y, float w, float h)
{ (void)u; (void)x; (void)y; (void)w; (void)h; }

static void stub_scissor(void* u, u32 x, u32 y, u32 w, u32 h)
{
    (void)u;
    stub.scissor[0] = x; stub.scissor[1] = y;
    stub.scissor[2] = w; stub.scissor[3] = h;
}

static void stub_stencil_ref(void* u, u32 r) { (void)u; stub.stencil_ref = r; }

static void stub_draw(void* u, rsx_topology topology, const void* verts,
                      u32 count, u32 stride, const u32* indices,
                      u32 index_count)
{
    (void)u; (void)verts; (void)indices;
    stub.n_draw++;
    stub.draw_topology = topology;
    stub.draw_vertices = count;
    stub.draw_stride = stride;
    stub.draw_indices = index_count;
}

static void stub_clear_color(void* u, u32 s, const float rgba[4])
{
    (void)u; (void)rgba;
    if (stub.n_clear_color < (int)STUB_MAX_CLEARED)
        stub.cleared[stub.n_clear_color] = s;
    stub.n_clear_color++;
}

static void stub_clear_ds(void* u, u32 d, u32 f, float z, u8 s)
{ (void)u; (void)d; (void)f; (void)z; (void)s; stub.n_clear_ds++; }

static u32 presented_surface;
static void stub_present(void* u, u32 s) { (void)u; presented_surface = s; stub.n_present++; }

/* One pixel of a surface, in the R,G,B,A order rsx_texture_decode produces:
 * the readback hook has to put those back into the guest's A8R8G8B8. */
static u8 g_stub_pixel[4] = { 0x11, 0x22, 0x33, 0x44 };

static void stub_readback(void* u, u32 s, u32 x, u32 y, u32 w, u32 h,
                          void* out, u32 pitch)
{
    (void)u; (void)s; (void)x; (void)y; (void)pitch;
    if (w == 1 && h == 1) memcpy(out, g_stub_pixel, 4);
}

static const rsx_draw_backend g_stub_backend = {
    .user = NULL,
    .init = stub_init,
    .shutdown = stub_shutdown,
    .submit_and_wait = stub_submit,
    .texture_create = stub_texture_create,
    .texture_upload = stub_texture_upload,
    .texture_release = stub_texture_release,
    .color_target_create = stub_color_create,
    .color_target_release = stub_color_release,
    .surface_view = stub_surface_view,
    .depth_target_create = stub_depth_create,
    .depth_target_release = stub_release,
    .depth_snapshot = stub_depth_snapshot,
    .pipeline_create = stub_pipeline_create,
    .pipeline_release = stub_pipeline_release,
    .bind_targets = stub_bind_targets,
    .bind_pipeline = stub_bind_pipeline,
    .bind_vs_constants = stub_bind_vs,
    .bind_ps_constants = stub_bind_ps,
    .bind_textures = stub_bind_textures,
    .bind_vertex_textures = stub_bind_vtextures,
    .set_viewport = stub_viewport,
    .set_scissor = stub_scissor,
    .set_stencil_ref = stub_stencil_ref,
    .draw = stub_draw,
    .clear_color = stub_clear_color,
    .clear_depth_stencil = stub_clear_ds,
    .present = stub_present,
    .readback = stub_readback,
};

/* ---- driving the engine -------------------------------------------------- */

#define M_CONTEXT_DMA_COLOR_B 0x018C
#define M_CONTEXT_DMA_COLOR_A 0x0194
#define M_CONTEXT_DMA_COLOR_C 0x01B4
#define M_CONTEXT_DMA_COLOR_D 0x01B8
#define M_SURFACE_CLIP_H      0x0200
#define M_SURFACE_CLIP_V      0x0204
#define M_SURFACE_FORMAT      0x0208
#define M_COLOR_A_OFFSET      0x0210
#define M_ZETA_OFFSET         0x0214
#define M_COLOR_B_OFFSET      0x0218
#define M_COLOR_TARGET        0x0220
#define M_COLOR_C_OFFSET      0x0288
#define M_COLOR_D_OFFSET      0x028C
#define M_SCISSOR_H           0x08C0
#define M_SCISSOR_V           0x08C4
#define M_VTXBUF_OFFSET       0x1680
#define M_VTXFMT              0x1740
#define M_BEGIN_END           0x1808
#define M_DRAW_ARRAYS         0x1814
#define M_TEX_UNIT            0x1A00   /* + unit * 0x20 */
#define M_STENCIL_FUNC_REF    0x0334
#define M_CLEAR_BUFFERS       0x1D94
#define M_VP_UPLOAD_CONST_ID  0x1EFC
#define M_VP_UPLOAD_CONST     0x1F00

#define DMA_LOCAL  0xFEED0000u
#define DMA_MAIN   0xFEED0001u

#define VTX_OFFSET  0x00010000u        /* main memory, well clear of ea 0 */
#define TEX_OFFSET  0x00020000u

static void m(u32 method, u32 arg) { rsx_draw_engine_method(method, arg); }

static void engine_up(void)
{
    memset(&stub, 0, sizeof stub);
    rsx_draw_engine_set_backend(&g_stub_backend);
    if (rsx_draw_engine_init(1280, 720) != 0) {
        printf("[FAIL] engine init\n");
        exit(1);
    }
    /* One 256x256 surface in local memory, and a vertex array in main. */
    m(M_CONTEXT_DMA_COLOR_A, DMA_LOCAL);
    m(M_SURFACE_CLIP_H, 256u << 16);
    m(M_SURFACE_CLIP_V, 256u << 16);
    m(M_COLOR_A_OFFSET, 0x40000u);
    /* type 2 (float32), size 4, stride 16; bit 31 of the offset = main. */
    m(M_VTXFMT, 2u | (4u << 4) | (16u << 8));
    m(M_VTXBUF_OFFSET, 0x80000000u | VTX_OFFSET);
}

static void engine_down(void)
{
    rsx_draw_engine_shutdown();
    rsx_draw_engine_set_backend(NULL);
}

static void draw_triangle(void)
{
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    m(M_DRAW_ARRAYS, 0u | ((3u - 1u) << 24));
    m(M_BEGIN_END, 0u);
}

/* Bind unit 0 at `offset` in `location` (0 = local, 1 = main), 4x4 A8R8G8B8
 * linear with one level. */
static void bind_texture_unit(u32 location, u32 offset)
{
    const u32 dma = (location == RSX_LOCATION_MAIN) ? 2u : 1u;
    m(M_TEX_UNIT + 0x00, offset);
    m(M_TEX_UNIT + 0x04, dma | (2u << 4) | ((0x85u | 0x20u) << 8) | (1u << 16));
    m(M_TEX_UNIT + 0x0C, 0x80000000u);        /* control0: unit enable */
    m(M_TEX_UNIT + 0x10, 0xAAE4u);            /* identity crossbar */
    m(M_TEX_UNIT + 0x18, (4u << 16) | 4u);    /* image rect 4x4 */
}

/* ---- 1. the surface set -------------------------------------------------- */

static void test_surface_set(void)
{
    printf("-- surface set\n");
    engine_up();

    m(M_CLEAR_BUFFERS, 0xF0u);
    CHECK(stub.n_color_create == 1 && stub.n_clear_color == 1,
          "a clear registers the bound colour surface (%d create, %d clear)",
          stub.n_color_create, stub.n_clear_color);
    const u32 first = stub.last_created_color;

    m(M_CLEAR_BUFFERS, 0xF0u);
    CHECK(stub.n_color_create == 1,
          "the same (context, offset, size, format) is the same surface");

    /* A size change reallocates and retires the old target. */
    m(M_SURFACE_CLIP_H, 512u << 16);
    m(M_SURFACE_CLIP_V, 512u << 16);
    m(M_CLEAR_BUFFERS, 0xF0u);
    CHECK(stub.n_color_create == 2 && stub.n_color_release == 1 &&
          stub.alive[first] == 0,
          "a size change reallocates and releases the old target");

    /* An offset change is a different surface, and does not disturb the
     * first. */
    m(M_COLOR_A_OFFSET, 0x80000u);
    m(M_CLEAR_BUFFERS, 0xF0u);
    CHECK(stub.n_color_create == 3 && stub.n_color_release == 1,
          "a different offset is a different surface");

    /* The same offset in the OTHER context DMA is also a different surface:
     * RSX offsets are two overlapping number spaces. */
    m(M_CONTEXT_DMA_COLOR_A, DMA_MAIN);
    m(M_CLEAR_BUFFERS, 0xF0u);
    CHECK(stub.n_color_create == 4 && stub.n_color_release == 1,
          "the same offset in another context DMA is another surface");

    /* Main memory that the IO table maps is seeded from the guest's own
     * bytes; local memory is not, because nothing guarantees it is backed. */
    CHECK(stub.n_color_seeded == 1,
          "only the main-memory surface was seeded from guest bytes (%d)",
          stub.n_color_seeded);

    /* An implausible clip does not destroy a live target. */
    const int creates = stub.n_color_create;
    m(M_SURFACE_CLIP_H, 60000u << 16);
    m(M_CLEAR_BUFFERS, 0xF0u);
    CHECK(stub.n_color_create == creates,
          "an implausible surface declaration keeps the existing target");

    engine_down();
}

/* ---- 2. surface aliasing on texture bind --------------------------------- */

static void test_surface_aliasing(void)
{
    printf("-- surface aliasing\n");
    engine_up();

    /* Render into a surface in MAIN memory, then sample a texture unit bound
     * at the same offset in the same context: the unit must read the live
     * target, not the guest bytes behind it. That is what every reflection,
     * shadow composite and post-process pass depends on. */
    m(M_CONTEXT_DMA_COLOR_A, DMA_MAIN);
    m(M_COLOR_A_OFFSET, TEX_OFFSET);
    m(M_CLEAR_BUFFERS, 0xF0u);
    const u32 surface = stub.last_created_color;

    /* Now draw into a DIFFERENT surface while sampling that one. */
    m(M_COLOR_A_OFFSET, 0x90000u);
    bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET);
    draw_triangle();
    CHECK(stub.n_draw == 1, "the draw executed");
    CHECK(stub.bound_mask == 1u, "unit 0 is the only bound unit (0x%X)",
          stub.bound_mask);
    CHECK(stub.bound_tex[0] == surface,
          "the unit samples the registered surface (%u, want %u)",
          stub.bound_tex[0], surface);
    CHECK(stub.n_texture_create == 0,
          "and nothing was uploaded from guest memory (%d uploads)",
          stub.n_texture_create);

    /* A unit naming the surface the pass is drawing INTO must not alias it:
     * reading a target a pass is writing is undefined on every API. */
    m(M_COLOR_A_OFFSET, TEX_OFFSET);
    draw_triangle();
    CHECK(stub.bound_tex[0] != surface,
          "a unit naming its own render target does not alias it");

    /* The same offset in the other context DMA is a different object. */
    m(M_COLOR_A_OFFSET, 0x90000u);
    bind_texture_unit(RSX_LOCATION_LOCAL, TEX_OFFSET);
    const int before = stub.n_texture_create;
    draw_triangle();
    CHECK(stub.n_texture_create == before + 1 && stub.bound_tex[0] != surface,
          "a unit in the other context uploads instead of aliasing");

    engine_down();
}

/* ---- 2b. the MRT set ----------------------------------------------------- */

static void test_mrt_set(void)
{
    printf("-- MRT set\n");
    engine_up();

    /* Targets B, C and D alongside the A engine_up declared, then MRT3 --
     * the way a deferred pass declares its G-buffer. */
    m(M_CONTEXT_DMA_COLOR_B, DMA_LOCAL);
    m(M_CONTEXT_DMA_COLOR_C, DMA_LOCAL);
    m(M_CONTEXT_DMA_COLOR_D, DMA_LOCAL);
    m(M_COLOR_B_OFFSET, 0x50000u);
    m(M_COLOR_C_OFFSET, 0x60000u);
    m(M_COLOR_D_OFFSET, 0x70000u);
    m(M_COLOR_TARGET, CELL_GCM_SURFACE_TARGET_MRT3);

    m(M_CLEAR_BUFFERS, 0xF0u);
    CHECK(stub.n_color_create == 4,
          "an MRT set registers all four targets (%d)", stub.n_color_create);
    CHECK(stub.n_clear_color == 4,
          "and one CLEAR_SURFACE clears every one of them (%d)",
          stub.n_clear_color);
    CHECK(stub.cleared[0] != stub.cleared[1] &&
          stub.cleared[1] != stub.cleared[2] &&
          stub.cleared[2] != stub.cleared[3] &&
          stub.cleared[0] != stub.cleared[3],
          "four distinct surfaces, not target A four times (%u,%u,%u,%u)",
          stub.cleared[0], stub.cleared[1], stub.cleared[2], stub.cleared[3]);

    draw_triangle();
    CHECK(stub.bound_rt_count == 4 && stub.bound_rt[0] == stub.cleared[0],
          "the draw binds all four, A first (%u bound)", stub.bound_rt_count);
    CHECK(stub.pipeline_rt_count == 4,
          "and its pipeline is built for four attachments (%u)",
          stub.pipeline_rt_count);

    /* A texture unit naming target C must not alias it: reading a target the
     * pass writes is undefined, and an MRT set writes more than one. */
    bind_texture_unit(RSX_LOCATION_LOCAL, 0x60000u);
    const int before = stub.n_texture_create;
    draw_triangle();
    CHECK(stub.n_texture_create == before + 1 &&
          stub.bound_tex[0] != stub.bound_rt[2],
          "a unit naming target C uploads instead of aliasing it");

    /* Back to target A alone: one attachment, and a pipeline of its own,
     * because the attachment count is part of the pipeline key. */
    const int pipelines = stub.n_pipeline_create;
    m(M_COLOR_TARGET, CELL_GCM_SURFACE_TARGET_0);
    draw_triangle();
    CHECK(stub.bound_rt_count == 1 && stub.pipeline_rt_count == 1 &&
          stub.n_pipeline_create == pipelines + 1,
          "target A alone binds one and builds its own pipeline (%u bound, %d)",
          stub.bound_rt_count, stub.n_pipeline_create);

    engine_down();
}

/* ---- 3. the texture cache ------------------------------------------------ */

static void test_texture_cache(void)
{
    printf("-- texture cache\n");
    engine_up();
    bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET);
    memset(vm_base + TEX_OFFSET, 0x11, 4 * 4 * 4);

    draw_triangle();
    CHECK(stub.n_texture_create == 1 && stub.n_texture_upload == 1,
          "the first bind uploads the texture");
    const u32 first = stub.last_created_texture;

    draw_triangle();
    draw_triangle();
    CHECK(stub.n_texture_create == 1,
          "a repeat bind in the same frame reuses it (%d uploads)",
          stub.n_texture_create);

    /* The guest rewrites the image in place -- an animated UI, a video frame,
     * a surface the 2D engine wrote. The cache must notice, and it costs one
     * content hash per entry per frame, not one per draw. */
    rsx_draw_engine_present();
    memset(vm_base + TEX_OFFSET, 0x22, 4 * 4 * 4);
    draw_triangle();
    CHECK(stub.n_texture_create == 2 && stub.alive[first] == 0,
          "changed guest bytes are re-decoded and the old resource retired");

    rsx_draw_engine_present();
    draw_triangle();
    CHECK(stub.n_texture_create == 2,
          "unchanged bytes in a later frame do not re-decode");

    /* Fill the cache past its capacity with distinct descriptors, then come
     * back to the first: it must have been evicted, not still resident, and
     * the most recent one must still be there. Returning nothing instead of
     * evicting is what made a recovered scene render as flat geometry. */
    for (u32 i = 0; i < RSX_DRAW_ENGINE_TEXTURE_CACHE; i++) {
        rsx_draw_engine_present();
        bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET + 0x1000u + i * 0x100u);
        draw_triangle();
    }
    CHECK(stub.n_texture_release >= 1,
          "the full cache evicts rather than giving up (%d evictions)",
          stub.n_texture_release);
    const u32 newest = stub.last_created_texture;
    const int creates = stub.n_texture_create;
    draw_triangle();
    CHECK(stub.n_texture_create == creates && stub.bound_tex[0] == newest,
          "the most recently used entry survived");

    rsx_draw_engine_present();
    bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET);
    draw_triangle();
    CHECK(stub.n_texture_create == creates + 1,
          "the least recently used entry was the one evicted");

    engine_down();
}

/* ---- 4. the pipeline key ------------------------------------------------- */

static void test_pipeline_key(void)
{
    printf("-- pipeline key\n");

    /* The key over the render state directly: what must and must not change
     * it. The alpha REFERENCE lives in the fragment constant block, so a
     * title fading something out must not compile a pipeline per frame. */
    rsx_be_render_state a;
    memset(&a, 0, sizeof a);
    a.alpha_test_enable = 1;
    a.alpha_func = 0x0204;
    a.alpha_ref_raw = 0x40;
    a.depth_func = 0x0201;
    const u64 base = rsx_draw_engine_hash_render_state(&a, 1469598103934665603ull);

    rsx_be_render_state b = a;
    b.alpha_ref_raw = 0xC0;
    CHECK(rsx_draw_engine_hash_render_state(&b, 1469598103934665603ull) == base,
          "the alpha reference is not part of the key");

    b = a; b.alpha_func = 0x0202;
    CHECK(rsx_draw_engine_hash_render_state(&b, 1469598103934665603ull) != base,
          "the alpha compare mode is");

    b = a; b.depth_func = 0x0203;
    CHECK(rsx_draw_engine_hash_render_state(&b, 1469598103934665603ull) != base,
          "so is the depth compare mode");

    b = a; b.color_mask = 0x01010101u;
    CHECK(rsx_draw_engine_hash_render_state(&b, 1469598103934665603ull) != base,
          "so is the colour mask");

    b = a; b.rt_fp16 = 1;
    CHECK(rsx_draw_engine_hash_render_state(&b, 1469598103934665603ull) != base,
          "so is an FP16 render target");

    /* And through the engine: transform constants change every draw of every
     * frame, and must not multiply pipelines. */
    engine_up();
    draw_triangle();
    CHECK(stub.n_pipeline_create == 1, "the first draw builds a pipeline");

    for (u32 i = 0; i < 8; i++) {
        m(M_VP_UPLOAD_CONST_ID, i);
        for (u32 w = 0; w < 4; w++) m(M_VP_UPLOAD_CONST + w * 4, 0x3F800000u + i);
        draw_triangle();
    }
    CHECK(stub.n_pipeline_create == 1,
          "changing transform constants does not (%d pipelines)",
          stub.n_pipeline_create);

    /* The stencil reference is encoder state, not pipeline state. */
    m(M_STENCIL_FUNC_REF, 0x7Fu);
    draw_triangle();
    CHECK(stub.n_pipeline_create == 1 && stub.stencil_ref == 0x7Fu,
          "nor does the stencil reference, which reaches the encoder (%u)",
          stub.stencil_ref);

    engine_down();
}

/* ---- 5. restart cuts and topology expansion ------------------------------ */

static void test_restart_expansion(void)
{
    printf("-- restart cuts\n");

    /* Eight strip vertices with the guest's restart sentinel after the
     * fourth. Without the cut the expansion joins the two runs with two
     * triangles that were never in the mesh -- upstream's "exploded spiky
     * mesh that occluded the scene". */
    const u32 cuts[1] = { 4 };
    u32 idx[64];

    const u32 whole = rsx_draw_engine_topology_index_count(
        RSX_PRIMITIVE_TRIANGLE_STRIP, 8, NULL, 0);
    const u32 cut = rsx_draw_engine_topology_index_count(
        RSX_PRIMITIVE_TRIANGLE_STRIP, 8, cuts, 1);
    CHECK(whole == (8 - 2) * 3, "an uncut strip of 8 is 6 triangles (%u)", whole);
    CHECK(cut == (4 - 2) * 3 * 2,
          "a strip cut in half is two runs of 2 triangles (%u)", cut);

    memset(idx, 0xFF, sizeof idx);
    rsx_draw_engine_write_topology_indices(RSX_PRIMITIVE_TRIANGLE_STRIP, 8,
                                           cuts, 1, NULL, idx);
    int spans = 0;
    for (u32 t = 0; t < cut / 3; t++) {
        int below = 0, above = 0;
        for (u32 k = 0; k < 3; k++)
            if (idx[t * 3 + k] < 4) below++; else above++;
        if (below && above) spans = 1;
    }
    CHECK(!spans, "no triangle spans the cut");

    /* Winding alternates per triangle within a segment, and each segment
     * starts over: a strip's second triangle swaps its first two vertices. */
    CHECK(idx[0] == 0 && idx[1] == 1 && idx[2] == 2,
          "strip triangle 0 is 0,1,2 (%u,%u,%u)", idx[0], idx[1], idx[2]);
    CHECK(idx[3] == 2 && idx[4] == 1 && idx[5] == 3,
          "strip triangle 1 swaps to 2,1,3 (%u,%u,%u)", idx[3], idx[4], idx[5]);
    CHECK(idx[6] == 4 && idx[7] == 5 && idx[8] == 6,
          "the segment after the cut starts its winding again (%u,%u,%u)",
          idx[6], idx[7], idx[8]);

    /* Fans and quads are cut the same way. */
    const u32 fan = rsx_draw_engine_topology_index_count(
        RSX_PRIMITIVE_TRIANGLE_FAN, 8, cuts, 1);
    CHECK(fan == (4 - 2) * 3 * 2, "a cut fan is two runs of 2 triangles (%u)", fan);
    rsx_draw_engine_write_topology_indices(RSX_PRIMITIVE_TRIANGLE_FAN, 8,
                                           cuts, 1, NULL, idx);
    CHECK(idx[0] == 0 && idx[3] == 0 && idx[6] == 4 && idx[9] == 4,
          "each fan segment fans around its own first vertex");

    const u32 quads = rsx_draw_engine_topology_index_count(
        RSX_PRIMITIVE_QUADS, 8, NULL, 0);
    CHECK(quads == 12, "two quads are four triangles (%u)", quads);

    /* A quad strip advances two vertices per quad, so eight vertices are
     * three quads whole and one quad per half once the strip is cut. */
    const u32 qstrip = rsx_draw_engine_topology_index_count(
        RSX_PRIMITIVE_QUAD_STRIP, 8, NULL, 0);
    const u32 qstrip_cut = rsx_draw_engine_topology_index_count(
        RSX_PRIMITIVE_QUAD_STRIP, 8, cuts, 1);
    CHECK(qstrip == 3 * 6, "an uncut quad strip of 8 is three quads (%u)", qstrip);
    CHECK(qstrip_cut == 2 * 6,
          "a quad strip cut in half is one quad per half (%u)", qstrip_cut);

    memset(idx, 0xFF, sizeof idx);
    rsx_draw_engine_write_topology_indices(RSX_PRIMITIVE_QUAD_STRIP, 8,
                                           cuts, 1, NULL, idx);
    CHECK(idx[0] == 0 && idx[1] == 1 && idx[2] == 2 &&
          idx[3] == 1 && idx[4] == 3 && idx[5] == 2,
          "a quad is split along the diagonal between its pairs "
          "(%u,%u,%u %u,%u,%u)", idx[0], idx[1], idx[2], idx[3], idx[4], idx[5]);
    CHECK(idx[6] == 4 && idx[7] == 5 && idx[8] == 6,
          "the segment after the cut pairs from its own first vertex "
          "(%u,%u,%u)", idx[6], idx[7], idx[8]);
    spans = 0;
    for (u32 t = 0; t < qstrip_cut / 3; t++) {
        int below = 0, above = 0;
        for (u32 k = 0; k < 3; k++)
            if (idx[t * 3 + k] < 4) below++; else above++;
        if (below && above) spans = 1;
    }
    CHECK(!spans, "and no quad spans the cut");

    /* A polygon is a fan around its own first vertex, and a cut starts a
     * second polygon rather than joining the two. */
    const u32 poly = rsx_draw_engine_topology_index_count(
        RSX_PRIMITIVE_POLYGON, 5, NULL, 0);
    const u32 poly_cut = rsx_draw_engine_topology_index_count(
        RSX_PRIMITIVE_POLYGON, 8, cuts, 1);
    CHECK(poly == (5 - 2) * 3, "a pentagon is three triangles (%u)", poly);
    CHECK(poly_cut == (4 - 2) * 3 * 2,
          "a cut polygon is two runs of 2 triangles (%u)", poly_cut);

    memset(idx, 0xFF, sizeof idx);
    rsx_draw_engine_write_topology_indices(RSX_PRIMITIVE_POLYGON, 8,
                                           cuts, 1, NULL, idx);
    CHECK(idx[0] == 0 && idx[1] == 1 && idx[2] == 2 &&
          idx[3] == 0 && idx[4] == 2 && idx[5] == 3,
          "the first polygon fans around vertex 0 (%u,%u,%u %u,%u,%u)",
          idx[0], idx[1], idx[2], idx[3], idx[4], idx[5]);
    CHECK(idx[6] == 4 && idx[9] == 4,
          "the one after the cut fans around vertex 4 (%u, %u)", idx[6], idx[9]);

    /* An occurrence-to-unique map is applied to every emitted index, which is
     * what lets a remapped draw share one uploaded vertex per reference. */
    u32 remap[8];
    for (u32 i = 0; i < 8; i++) remap[i] = 7u - i;
    rsx_draw_engine_write_topology_indices(RSX_PRIMITIVE_TRIANGLES, 6,
                                           NULL, 0, remap, idx);
    CHECK(idx[0] == 7 && idx[5] == 2, "the remap is applied to every index");
}

/* ---- 6. what reaches the backend as an indexed draw ---------------------- */

static void test_indexed_draws(void)
{
    printf("-- indexed draws\n");
    engine_up();

    /* A triangle list of distinct vertices has nothing to share, so it is
     * drawn straight. */
    draw_triangle();
    CHECK(stub.draw_indices == 0 && stub.draw_vertices == 3,
          "a triangle list of distinct vertices is not indexed (%u verts, %u indices)",
          stub.draw_vertices, stub.draw_indices);

    /* A strip is expanded into a triangle list through an INDEX buffer: six
     * vertices become twelve indices over six uploaded vertices, rather than
     * twelve duplicated ones. */
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLE_STRIP);
    m(M_DRAW_ARRAYS, 0u | ((6u - 1u) << 24));
    m(M_BEGIN_END, 0u);
    CHECK(stub.draw_indices == (6 - 2) * 3 && stub.draw_vertices == 6,
          "a strip is indexed, not duplicated (%u verts, %u indices)",
          stub.draw_vertices, stub.draw_indices);

    /* Only the attributes the program reads are fetched, so the stride is the
     * layout's rather than all sixteen registers. The built-in program reads
     * every register, so this is the full one; a guest program that reads two
     * would halve it. */
    CHECK(stub.draw_stride == 16u * 16u,
          "the built-in program's layout is all sixteen attributes (%u bytes)",
          stub.draw_stride);

    /* Quads have no host equivalent and become an indexed triangle list. */
    m(M_BEGIN_END, RSX_PRIMITIVE_QUADS);
    m(M_DRAW_ARRAYS, 0u | ((8u - 1u) << 24));
    m(M_BEGIN_END, 0u);
    CHECK(stub.draw_topology == RSX_TOPOLOGY_TRIANGLES &&
          stub.draw_indices == 12,
          "two quads become twelve triangle indices (%u)", stub.draw_indices);

    /* So do quad strips and polygons, the two the engine used to drop. Six
     * quad-strip vertices are two quads, and a five-vertex polygon is a fan
     * of three triangles. */
    m(M_BEGIN_END, RSX_PRIMITIVE_QUAD_STRIP);
    m(M_DRAW_ARRAYS, 0u | ((6u - 1u) << 24));
    m(M_BEGIN_END, 0u);
    CHECK(stub.draw_topology == RSX_TOPOLOGY_TRIANGLES &&
          stub.draw_indices == 12 && stub.draw_vertices == 6,
          "a quad strip of six becomes twelve indices over six vertices "
          "(%u verts, %u indices)", stub.draw_vertices, stub.draw_indices);

    m(M_BEGIN_END, RSX_PRIMITIVE_POLYGON);
    m(M_DRAW_ARRAYS, 0u | ((5u - 1u) << 24));
    m(M_BEGIN_END, 0u);
    CHECK(stub.draw_topology == RSX_TOPOLOGY_TRIANGLES &&
          stub.draw_indices == 9 && stub.draw_vertices == 5,
          "a five-vertex polygon becomes nine indices (%u verts, %u indices)",
          stub.draw_vertices, stub.draw_indices);

    /* Points and lines are drawn as they are, in the order they arrived: an
     * index buffer would be pointless and collapsing repeats would reorder
     * them. */
    m(M_BEGIN_END, RSX_PRIMITIVE_LINES);
    m(M_DRAW_ARRAYS, 0u | ((4u - 1u) << 24));
    m(M_BEGIN_END, 0u);
    CHECK(stub.draw_topology == RSX_TOPOLOGY_LINES && stub.draw_indices == 0 &&
          stub.draw_vertices == 4,
          "lines pass through unindexed (%u verts, %u indices)",
          stub.draw_vertices, stub.draw_indices);

    m(M_BEGIN_END, RSX_PRIMITIVE_POINTS);
    m(M_DRAW_ARRAYS, 0u | ((2u - 1u) << 24));
    m(M_BEGIN_END, 0u);
    CHECK(stub.draw_topology == RSX_TOPOLOGY_POINTS && stub.draw_vertices == 2,
          "so do points (%u verts)", stub.draw_vertices);

    engine_down();
}

/* ---- 7. the guest scissor ------------------------------------------------ */

static void test_scissor(void)
{
    printf("-- scissor\n");
    engine_up();

    draw_triangle();
    CHECK(stub.scissor[2] == 256 && stub.scissor[3] == 256,
          "an unwritten scissor register is the whole surface (%ux%u)",
          stub.scissor[2], stub.scissor[3]);

    m(M_SCISSOR_H, (64u << 16) | 16u);
    m(M_SCISSOR_V, (32u << 16) | 8u);
    draw_triangle();
    CHECK(stub.scissor[0] == 16 && stub.scissor[1] == 8 &&
          stub.scissor[2] == 64 && stub.scissor[3] == 32,
          "a guest scissor narrows it (%u,%u %ux%u)",
          stub.scissor[0], stub.scissor[1], stub.scissor[2], stub.scissor[3]);

    m(M_SCISSOR_H, (4096u << 16) | 0u);
    m(M_SCISSOR_V, (4096u << 16) | 0u);
    draw_triangle();
    CHECK(stub.scissor[2] == 256 && stub.scissor[3] == 256,
          "a scissor larger than the surface is clipped to it (%ux%u)",
          stub.scissor[2], stub.scissor[3]);

    engine_down();
}

/* ---- 8. the presented pixel ---------------------------------------------- */

static void test_readback(void)
{
    printf("-- readback\n");
    engine_up();
    m(M_CLEAR_BUFFERS, 0xF0u);
    rsx_draw_engine_present();
    CHECK(stub.n_present == 1, "the flip presented a surface");
    CHECK(rsx_draw_engine_readback_center() == 0x44112233u,
          "the presented pixel comes back as A8R8G8B8 (0x%08X)",
          rsx_draw_engine_readback_center());
    engine_down();
}

static void test_queued_flip_buffer(void)
{
    engine_up();
    draw_triangle();
    u32 first = stub.bound_rt[0];
    rsx_draw_engine_set_display_buffer(0, RSX_LOCATION_LOCAL, 0x40000, 1024, 256, 256);
    m(M_COLOR_A_OFFSET, 0x60000);
    draw_triangle();
    u32 second = stub.bound_rt[0];
    rsx_draw_engine_set_display_buffer(1, RSX_LOCATION_LOCAL, 0x60000, 1024, 256, 256);
    rsx_draw_engine_present_buffer(0);
    CHECK(presented_surface == first, "queued flip selects first buffer over current target");
    rsx_draw_engine_present_buffer(1);
    CHECK(presented_surface == second && first != second, "queued flip selects second buffer");
    rsx_draw_engine_present();
    CHECK(presented_surface == second, "host present retains last queued buffer");
    engine_down();
}

static unsigned custom_reads;
static const u8* custom_guest_read(void* user, u32 location, u32 offset, u32 bytes)
{
    CHECK(user == &custom_reads, "custom memory resolver receives its user data");
    ++custom_reads;
    u32 ea = location == 0 ? GUEST_LOCAL_EA + offset : offset;
    if ((u64)ea + bytes > GUEST_BYTES) return NULL;
    return vm_base + ea;
}

static void test_custom_guest_mapping(void)
{
    engine_up();
    rsx_draw_engine_set_guest_memory(custom_guest_read, &custom_reads);
    custom_reads = 0;
    draw_triangle();
    CHECK(custom_reads > 0, "draw resolves vertices through runner's memory map");
    unsigned calls = custom_reads;
    rsx_draw_engine_set_guest_memory(NULL, NULL);
    draw_triangle();
    CHECK(custom_reads == calls, "clearing resolver restores default mapping");
    engine_down();
}

int main(void)
{
    vm_base = (u8*)calloc(1, GUEST_BYTES);
    if (!vm_base) { printf("[FAIL] guest memory\n"); return 1; }
    ppu_vm_size = GUEST_BYTES;
    /* Three vertices of float4 position, so the fetch has something real to
     * read: the engine treats an unreadable ATTR0 as a failed group. */
    for (u32 i = 0; i < 3 * 4; i++) {
        u8* p = vm_base + VTX_OFFSET + i * 4;
        p[0] = 0x3F; p[1] = 0x80; p[2] = 0; p[3] = 0;    /* 1.0f big-endian */
    }

    test_surface_set();
    test_surface_aliasing();
    test_mrt_set();
    test_texture_cache();
    test_pipeline_key();
    test_restart_expansion();
    test_indexed_draws();
    test_scissor();
    test_readback();
    test_custom_guest_mapping();
    test_queued_flip_buffer();

    free(vm_base);
    printf(g_failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n",
           g_failures);
    return g_failures ? 1 : 0;
}
