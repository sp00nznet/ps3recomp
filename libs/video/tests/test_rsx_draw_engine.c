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

/* No cellGcmSys here: nothing was registered before the engine came up. */
void cellGcm_replay_display_buffers_to_engine(void) {}

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
    rsx_be_render_state pipeline_rs;   /* the last pipeline's render state */
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
    float captured_vertices[16][64];
    u32 captured_indices[24];
    float draw_first_vertex[4];   /* ATTR0 of the first vertex handed over */
    rsx_topology draw_topology;
    u32 scissor[4];
    u32 stencil_ref;
    float depth_bounds[2];
    int n_depth_bounds;
    u32 last_created_texture, last_created_color;
    u32 last_released;
    float last_clear_depth;
    int n_depth_readback;
    u32 color_w, color_h;         /* size of the last colour surface created */
    float vp_xf[8];               /* viewport epilogue after the VS constants */
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
    (void)u; (void)f; (void)seed_row;
    stub.n_color_create++;
    stub.color_w = w; stub.color_h = h;
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
    stub.pipeline_rs = *rs;
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
static void stub_bind_vs(void* u, const void* d, u32 n)
{
    (void)u;
    if (d && n >= RSX_DSP_NUM_CONSTANTS * 16u + sizeof stub.vp_xf)
        memcpy(stub.vp_xf, (const u8*)d + RSX_DSP_NUM_CONSTANTS * 16u, sizeof stub.vp_xf);
}
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
static void stub_depth_bounds(void* u, float lo, float hi)
{
    (void)u;
    stub.depth_bounds[0] = lo; stub.depth_bounds[1] = hi;
    stub.n_depth_bounds++;
}

static void stub_draw(void* u, rsx_topology topology, const void* verts,
                      u32 count, u32 stride, const u32* indices,
                      u32 index_count)
{
    (void)u; (void)indices;
    stub.n_draw++;
    memset(stub.draw_first_vertex, 0, sizeof stub.draw_first_vertex);
    if (verts && stride >= sizeof stub.draw_first_vertex)
        memcpy(stub.draw_first_vertex, verts, sizeof stub.draw_first_vertex);
    memset(stub.captured_vertices, 0, sizeof stub.captured_vertices);
    memset(stub.captured_indices, 0, sizeof stub.captured_indices);
    if (verts && stride <= sizeof stub.captured_vertices[0])
        for (u32 i = 0; i < count && i < 16; i++)
            memcpy(stub.captured_vertices[i], (const u8*)verts + i * stride, stride);
    if (indices)
        memcpy(stub.captured_indices, indices,
               (index_count < 24 ? index_count : 24) * sizeof(u32));
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
{ (void)u; (void)d; (void)f; (void)s; stub.n_clear_ds++; stub.last_clear_depth = z; }

static u32 presented_surface;
static void stub_present(void* u, u32 s) { (void)u; presented_surface = s; stub.n_present++; }

/* One pixel of a surface, in the R,G,B,A order rsx_texture_decode produces:
 * the readback hook has to put those back into the guest's A8R8G8B8. */
static u8 g_stub_pixel[4] = { 0x11, 0x22, 0x33, 0x44 };
/* An FP16 surface's pixel: R,G,B,A halves 1.0, 2.0, 3.0, 4.0, little-endian
 * as a host GPU hands them back. */
static const u8 g_stub_pixel16[8] = { 0x00, 0x3C, 0x00, 0x40, 0x00, 0x42, 0x00, 0x44 };

/* With g_stub_ramp set, pixel (c, r) reads back as R,G,B,A = 4c+1, 4c+2,
 * 4c+3, 0x80+r instead, so every byte of a row is distinct. */
static int g_stub_ramp;

/* The engine asks for rows packed at the surface's own pixel size, so the
 * pitch tells the two formats apart. */
static void stub_readback(void* u, u32 s, u32 x, u32 y, u32 w, u32 h,
                          void* out, u32 pitch)
{
    (void)u; (void)s; (void)x; (void)y;
    const u32 bpp = pitch >= w * 8u ? 8u : 4u;
    for (u32 r = 0; r < h; r++)
        for (u32 c = 0; c < w; c++) {
            u8* p = (u8*)out + (size_t)r * pitch + c * bpp;
            if (bpp == 8u) { memcpy(p, g_stub_pixel16, 8); continue; }
            if (!g_stub_ramp) { memcpy(p, g_stub_pixel, 4); continue; }
            p[0] = (u8)(4u * c + 1u); p[1] = (u8)(4u * c + 2u); p[2] = (u8)(4u * c + 3u);
            p[3] = (u8)(0x80u + r);
        }
}

/* Every depth target reads back as 0.5, which Z24S8 stores as 0x800000. */
static int stub_depth_readback(void* u, u32 d, u32 x, u32 y, u32 w, u32 h,
                               float* out, u32 pitch)
{
    (void)u; (void)d; (void)x; (void)y;
    stub.n_depth_readback++;
    for (u32 r = 0; r < h; r++)
        for (u32 c = 0; c < w; c++)
            *(float*)((u8*)out + (size_t)r * pitch + c * 4u) = 0.5f;
    return 0;
}

/* The last rectangle pushed into a colour target, its first pixel and the
 * first 64 bytes of its first row. */
static struct { int n; u32 surface, x, y, w, h; u8 first[4]; u8 row[64]; } g_written;

static void stub_color_write(void* u, u32 s, u32 x, u32 y, u32 w, u32 h,
                             const void* rows, u32 row_bytes)
{
    (void)u; (void)row_bytes;
    g_written.n++;
    g_written.surface = s; g_written.x = x; g_written.y = y;
    g_written.w = w; g_written.h = h;
    memcpy(g_written.first, rows, 4);
    memcpy(g_written.row, rows, w * 4u < sizeof g_written.row ? w * 4u : sizeof g_written.row);
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
    .color_target_write = stub_color_write,
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
    .set_depth_bounds = stub_depth_bounds,
    .draw = stub_draw,
    .clear_color = stub_clear_color,
    .clear_depth_stencil = stub_clear_ds,
    .present = stub_present,
    .readback = stub_readback,
    .depth_readback = stub_depth_readback,
};

/* ---- driving the engine -------------------------------------------------- */

#define M_CONTEXT_DMA_COLOR_B 0x018C
#define M_CONTEXT_DMA_COLOR_A 0x0194
#define M_CONTEXT_DMA_COLOR_C 0x01B4
#define M_CONTEXT_DMA_COLOR_D 0x01B8
#define M_SURFACE_CLIP_H      0x0200
#define M_SURFACE_CLIP_V      0x0204
#define M_SURFACE_FORMAT      0x0208
#define M_COLOR_A_PITCH       0x020C
#define M_COLOR_A_OFFSET      0x0210
#define M_ZETA_OFFSET         0x0214
#define M_COLOR_B_OFFSET      0x0218
#define M_COLOR_TARGET        0x0220
#define M_CONTEXT_DMA_ZETA    0x0198
#define M_ZSTENCIL_CLEAR      0x1D8C
#define M_ZETA_PITCH          0x022C
#define M_COLOR_MASK          0x0324
#define M_DEPTH_WRITE         0x0A70
#define M_DEPTH_TEST          0x0A74
#define M_COLOR_C_OFFSET      0x0288
#define M_COLOR_D_OFFSET      0x028C
#define M_SCISSOR_H           0x08C0
#define M_SCISSOR_V           0x08C4
#define M_VTXBUF_OFFSET       0x1680
#define M_VTXFMT              0x1740
#define M_BEGIN_END           0x1808
#define M_DRAW_ARRAYS         0x1814
#define M_INLINE_ARRAY        0x1818
#define M_TEX_UNIT            0x1A00   /* + unit * 0x20 */
#define M_STENCIL_FUNC_REF    0x0334
#define M_DEPTH_BOUNDS_ENABLE 0x0380
#define M_DEPTH_BOUNDS_MIN    0x0384
#define M_DEPTH_BOUNDS_MAX    0x0388
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

/* A 128x128 A8R8G8B8 linear texture on unit 0: 64 KB, past the size that is
 * hashed in full every frame. */
#define BIG_TEX_OFFSET 0x00100000u
#define BIG_TEX_DIM    128u
#define BIG_TEX_BYTES  (BIG_TEX_DIM * BIG_TEX_DIM * 4u)
static void bind_big_texture(void)
{
    m(M_TEX_UNIT + 0x00, BIG_TEX_OFFSET);
    m(M_TEX_UNIT + 0x04, 2u | (2u << 4) | ((0x85u | 0x20u) << 8) | (1u << 16));
    m(M_TEX_UNIT + 0x0C, 0x80000000u);
    m(M_TEX_UNIT + 0x10, 0xAAE4u);
    m(M_TEX_UNIT + 0x18, (BIG_TEX_DIM << 16) | BIG_TEX_DIM);
    m(0x1840 /* control3: pitch */, BIG_TEX_DIM * 4u);
}

/* Draw once a frame until the cache re-decodes the texture: how many
 * presents that took, or -1 if not within `limit`. */
static int frames_to_notice(int limit)
{
    const int before = stub.n_texture_create;
    for (int f = 0; f <= limit; f++) {
        draw_triangle();
        if (stub.n_texture_create != before) return f;
        rsx_draw_engine_present();
    }
    return -1;
}

static void test_texture_sparse_rehash(void)
{
    printf("-- texture re-hash\n");
    u8* tex = vm_base + BIG_TEX_OFFSET;
    engine_up();
    bind_big_texture();
    memset(tex, 0x11, BIG_TEX_BYTES);
    draw_triangle();
    CHECK(stub.n_texture_create == 1, "the big texture is uploaded");

    /* What the per-frame sparse check sees is noticed in the next frame:
     * a whole rewrite, a byte inside one of the sampled 64-byte blocks (one
     * per 2 KB), a byte of the tail. */
    rsx_draw_engine_present();
    memset(tex, 0x22, BIG_TEX_BYTES);
    CHECK(frames_to_notice(0) == 0, "a rewrite of the whole texture is noticed at once");
    rsx_draw_engine_present();
    tex[3 * 2048 + 5] ^= 0xFF;
    CHECK(frames_to_notice(0) == 0, "a write in a sampled block is noticed at once");
    rsx_draw_engine_present();
    tex[BIG_TEX_BYTES - 1] ^= 0xFF;
    CHECK(frames_to_notice(0) == 0, "a write to the last 64 bytes is noticed at once");

    /* A byte between the sampled blocks waits for the full hash, which comes
     * 8 frames after the last one: the last full hash was the frame above,
     * so this one is seen 7 presents later, and not before. */
    rsx_draw_engine_present();
    tex[3 * 2048 + 100] ^= 0xFF;
    const int late = frames_to_notice(10);
    CHECK(late == 7, "a write between the sampled blocks is noticed by the full hash (%d frames)", late);

    /* A small texture is still hashed in full every frame. */
    rsx_draw_engine_present();
    bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET);
    draw_triangle();
    rsx_draw_engine_present();
    vm_base[TEX_OFFSET + 37] ^= 0xFF;
    CHECK(frames_to_notice(0) == 0, "a small texture is hashed in full every frame");
    engine_down();

    /* PS3RECOMP_ENG_TEXHASH_FULL=1: the full hash every frame, as before. */
    setenv("PS3RECOMP_ENG_TEXHASH_FULL", "1", 1);
    engine_up();
    bind_big_texture();
    draw_triangle();
    rsx_draw_engine_present();
    tex[5 * 2048 + 100] ^= 0xFF;
    CHECK(frames_to_notice(0) == 0, "PS3RECOMP_ENG_TEXHASH_FULL=1 hashes everything every frame");
    engine_down();
    unsetenv("PS3RECOMP_ENG_TEXHASH_FULL");
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

    b = a; b.depth_bounds_test = 1;
    CHECK(rsx_draw_engine_hash_render_state(&b, 1469598103934665603ull) != base,
          "so is the depth bounds test");

    /* MRT targets 1..3 carry their own colour mask (COLOR_MASK_MRT, 0x0370)
     * and blend enable (BLEND_ENABLE_MRT, 0x036C). inFAMOUS 2's light volumes
     * add into target 0 with targets 1 and 2 masked off; giving them target
     * 0's state added every light into the G-buffer. */
    b = a; b.color_mask_mrt = 0x0DF0u;
    CHECK(rsx_draw_engine_hash_render_state(&b, 1469598103934665603ull) != base,
          "so is the MRT colour mask");

    b = a; b.blend_mrt = 0x2u;
    CHECK(rsx_draw_engine_hash_render_state(&b, 1469598103934665603ull) != base,
          "so is the MRT blend enable");

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

    /* The depth bounds test is pipeline state; its bounds are not. A deferred
     * renderer draws its lights in [0, 0.999984] and its sky in
     * [0.999984, 1] with the same programs. */
    m(M_DEPTH_BOUNDS_ENABLE, 1u);
    draw_triangle();
    CHECK(stub.n_pipeline_create == 2,
          "enabling the depth bounds test builds another pipeline (%d)",
          stub.n_pipeline_create);
    m(M_DEPTH_BOUNDS_MIN, 0x00000000u);      /* 0.0  */
    m(M_DEPTH_BOUNDS_MAX, 0x3F7FFEF9u);      /* 0.999984 */
    draw_triangle();
    CHECK(stub.depth_bounds[0] == 0.0f && stub.depth_bounds[1] > 0.99998f &&
          stub.depth_bounds[1] < 0.99999f,
          "the bounds reach the backend as floats (%g, %g)",
          stub.depth_bounds[0], stub.depth_bounds[1]);
    m(M_DEPTH_BOUNDS_MIN, 0x3F7FFEF9u);
    m(M_DEPTH_BOUNDS_MAX, 0x3F800000u);      /* 1.0  */
    draw_triangle();
    CHECK(stub.n_pipeline_create == 2 && stub.depth_bounds[1] == 1.0f,
          "and moving them does not build a pipeline (%d)",
          stub.n_pipeline_create);
    m(M_DEPTH_BOUNDS_ENABLE, 0u);

    /* The decode carries the two MRT registers through to the backend. */
    m(0x0370, 0x0DF0);
    m(0x036C, 0x2);
    draw_triangle();
    CHECK(stub.pipeline_rs.color_mask_mrt == 0x0DF0u && stub.pipeline_rs.blend_mrt == 0x2u,
          "the pipeline is built with COLOR_MASK_MRT and BLEND_ENABLE_MRT");

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
    int presents = stub.n_present;
    rsx_draw_engine_note_flip(0);
    CHECK(stub.n_present == presents, "noting a flip does not present");
    rsx_draw_engine_present();
    CHECK(presented_surface == first, "host present follows the noted flip");
    engine_down();
}

/* A 2D transfer reads and writes guest memory; the pixels it touches live in
 * the engine's surfaces, so they are synced both ways around it. */
static void test_transfer_coherence(void)
{
    printf("-- transfer coherence\n");
    engine_up();
    m(M_COLOR_A_PITCH, 1024u);                 /* 256 pixels of A8R8G8B8 */
    draw_triangle();
    const u32 surface = stub.bound_rt[0];

    /* The guest wrote A,R,G,B bytes at (2,3): they reach the surface as R,G,B,A. */
    u8* px = vm_base + GUEST_LOCAL_EA + 0x40000u + 3u * 1024u + 2u * 4u;
    px[0] = 0x80; px[1] = 0x01; px[2] = 0x02; px[3] = 0x03;
    memset(&g_written, 0, sizeof g_written);
    rsx_draw_engine_guest_wrote(RSX_LOCATION_LOCAL, 0x40000u + 3u * 1024u + 2u * 4u,
                                1024u, 5u, 4u);
    CHECK(g_written.n == 1 && g_written.surface == surface,
          "a transfer into a surface's bytes is pushed to that surface");
    CHECK(g_written.x == 2 && g_written.y == 3 && g_written.w == 5 && g_written.h == 4,
          "at the rectangle the bytes describe (%u,%u %ux%u)",
          g_written.x, g_written.y, g_written.w, g_written.h);
    CHECK(g_written.first[0] == 0x01 && g_written.first[1] == 0x02 &&
          g_written.first[2] == 0x03 && g_written.first[3] == 0x80,
          "A8R8G8B8 guest bytes arrive as R,G,B,A");

    /* Another pitch lays the rows out elsewhere, and other memory is not a surface. */
    rsx_draw_engine_guest_wrote(RSX_LOCATION_LOCAL, 0x40000u, 2048u, 4u, 4u);
    rsx_draw_engine_guest_wrote(RSX_LOCATION_MAIN, 0x40000u, 1024u, 4u, 4u);
    CHECK(g_written.n == 1, "a different pitch or memory space touches no surface");

    /* A transfer reading a surface's bytes gets the GPU's pixels first. */
    u8* rd = vm_base + GUEST_LOCAL_EA + 0x40000u + 1024u;
    memset(rd, 0, 8);
    rsx_draw_engine_guest_read(RSX_LOCATION_LOCAL, 0x40000u + 1024u, 1024u, 2u, 1u);
    CHECK(rd[0] == 0x44 && rd[1] == 0x11 && rd[2] == 0x22 && rd[3] == 0x33 &&
          rd[4] == 0x44 && rd[7] == 0x33,
          "the surface's R,G,B,A pixels land in guest memory as A8R8G8B8");
    engine_down();
}

/* Whole rows, every pixel different, both ways: a byte swapped between two
 * pixels, or a row converted only in part, shows up here. */
static void test_transfer_rows(void)
{
    printf("-- transfer rows\n");
    engine_up();
    m(M_COLOR_A_PITCH, 1024u);
    draw_triangle();

    /* Guest A,R,G,B -> surface R,G,B,A, 13 pixels (not a multiple of a vector). */
    u8* px = vm_base + GUEST_LOCAL_EA + 0x40000u + 5u * 1024u;
    for (u32 c = 0; c < 13; c++) {
        px[4 * c + 0] = (u8)(0xA0 + c); px[4 * c + 1] = (u8)(4 * c + 1);
        px[4 * c + 2] = (u8)(4 * c + 2); px[4 * c + 3] = (u8)(4 * c + 3);
    }
    memset(&g_written, 0, sizeof g_written);
    rsx_draw_engine_guest_wrote(RSX_LOCATION_LOCAL, 0x40000u + 5u * 1024u, 1024u, 13u, 2u);
    int ok = g_written.n == 1 && g_written.w == 13;
    for (u32 c = 0; ok && c < 13; c++)
        ok = g_written.row[4 * c + 0] == 4 * c + 1 && g_written.row[4 * c + 1] == 4 * c + 2 &&
             g_written.row[4 * c + 2] == 4 * c + 3 && g_written.row[4 * c + 3] == 0xA0 + c;
    CHECK(ok, "13 distinct A8R8G8B8 pixels arrive as R,G,B,A in order");

    /* Surface R,G,B,A -> guest A,R,G,B, 3 rows of 13 pixels, and nothing past them. */
    u8* rd = vm_base + GUEST_LOCAL_EA + 0x40000u + 8u * 1024u;
    for (u32 r = 0; r < 4; r++) memset(rd + r * 1024u, 0xEE, 14 * 4);
    g_stub_ramp = 1;
    rsx_draw_engine_guest_read(RSX_LOCATION_LOCAL, 0x40000u + 8u * 1024u, 1024u, 13u, 3u);
    g_stub_ramp = 0;
    ok = 1;
    for (u32 r = 0; ok && r < 3; r++)
        for (u32 c = 0; ok && c < 13; c++) {
            const u8* p = rd + r * 1024u + 4 * c;
            ok = p[0] == 0x80 + r && p[1] == 4 * c + 1 && p[2] == 4 * c + 2 && p[3] == 4 * c + 3;
        }
    CHECK(ok, "3 rows of 13 distinct R,G,B,A pixels land as A8R8G8B8 in order");
    CHECK(rd[13 * 4] == 0xEE && rd[3 * 1024u] == 0xEE,
          "the pixel after each row and the row after the rectangle are untouched");
    engine_down();
}

/* An HDR pass renders into an FP16 RGBA target, and the title copies it out
 * with a 2D transfer that counts 32-bit pixels: Twisted Metal's bloom and
 * flames reach its SPU post-FX that way. The copy has to get the GPU's
 * pixels, as big-endian halves, and only the bytes it names. */
static void test_transfer_reads_fp16(void)
{
    printf("-- transfer reads an FP16 surface\n");
    engine_up();
    m(M_SURFACE_FORMAT, 0xBu);                 /* F_W16Z16Y16X16 */
    m(M_COLOR_A_PITCH, 2048u);                 /* 256 pixels of 8 bytes */
    draw_triangle();

    const u32 row = 0x40000u + 3u * 2048u;
    u8* rd = vm_base + GUEST_LOCAL_EA + row;
    static const u8 px[8] = { 0x3C, 0x00, 0x40, 0x00, 0x42, 0x00, 0x44, 0x00 };

    /* Four 32-bit pixels from surface pixel 2: two FP16 pixels. */
    memset(rd, 0xEE, 64);
    rsx_draw_engine_guest_read(RSX_LOCATION_LOCAL, row + 2u * 8u, 2048u, 4u, 1u);
    CHECK(memcmp(rd + 16, px, 8) == 0 && memcmp(rd + 24, px, 8) == 0,
          "two FP16 pixels land as big-endian R,G,B,A halves (%02X %02X %02X %02X)",
          rd[16], rd[17], rd[18], rd[19]);
    CHECK(rd[15] == 0xEE && rd[32] == 0xEE, "and nothing around them is written");

    /* One 32-bit pixel starting half way into a surface pixel: its B,A. */
    memset(rd, 0xEE, 64);
    rsx_draw_engine_guest_read(RSX_LOCATION_LOCAL, row + 5u * 8u + 4u, 2048u, 1u, 1u);
    CHECK(memcmp(rd + 44, px + 4, 4) == 0 && rd[43] == 0xEE && rd[48] == 0xEE,
          "a transfer starting mid-pixel gets exactly the bytes it names");

    /* The last pixel of the row, asked for past the surface's width. */
    memset(rd + 2040, 0xEE, 8);
    rsx_draw_engine_guest_read(RSX_LOCATION_LOCAL, row + 255u * 8u, 2048u, 8u, 1u);
    CHECK(memcmp(rd + 2040, px, 8) == 0, "a transfer past the width stops at the surface's edge");

    /* Bytes copied INTO an FP16 target are not pushed as A8R8G8B8 rows. */
    memset(&g_written, 0, sizeof g_written);
    rsx_draw_engine_guest_wrote(RSX_LOCATION_LOCAL, row, 2048u, 4u, 1u);
    CHECK(g_written.n == 0, "a transfer into an FP16 surface does not write it as A8R8G8B8");
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

/* NV4097_INLINE_ARRAY: the vertices arrive IN the pushbuffer.
 *
 * Nothing consumed method 0x1818 at all, so a title that builds geometry this
 * way (Guitar Hero III draws its whole 2D layer as inline QUADS) issued draws
 * that reached the backend with no vertices and were counted as empty groups.
 * The regression this guards is silent by construction -- the draw is simply
 * absent -- so the check is both that a draw happens AND that its vertices
 * came from the stream rather than from the still-bound vertex array. */
static u32 f32_bits(float f) { u32 u; memcpy(&u, &f, 4); return u; }

static void test_inline_array(void)
{
    engine_up();
    /* ATTR0 alone: float4, stride 16. The array at VTX_OFFSET holds 1.0f in
     * every component (see main), so reading the array instead of the stream
     * is a distinguishable wrong answer. */
    m(M_VTXFMT, 2u | (4u << 4) | (16u << 8));

    const int before = stub.n_draw;
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    for (u32 v = 0; v < 3; v++) {
        m(M_INLINE_ARRAY, f32_bits(10.0f + (float)v));  /* x: 10, 11, 12 */
        m(M_INLINE_ARRAY, f32_bits(20.0f));
        m(M_INLINE_ARRAY, f32_bits(30.0f));
        m(M_INLINE_ARRAY, f32_bits(1.0f));
    }
    m(M_BEGIN_END, 0u);

    CHECK(stub.n_draw == before + 1, "inline array issues a draw");
    CHECK(stub.draw_vertices == 3, "inline array draws 3 vertices (got %u)",
          stub.draw_vertices);
    CHECK(stub.draw_topology == RSX_TOPOLOGY_TRIANGLES,
          "inline array keeps the primitive type");
    CHECK(stub.draw_first_vertex[0] == 10.0f &&
          stub.draw_first_vertex[1] == 20.0f &&
          stub.draw_first_vertex[2] == 30.0f,
          "inline vertices come from the stream, not the vertex array "
          "(got %g,%g,%g)", stub.draw_first_vertex[0],
          stub.draw_first_vertex[1], stub.draw_first_vertex[2]);
    engine_down();
}

/* ---- 9. the depth clear value --------------------------------------------- */

#define ZETA_OFFSET  0x00100000u        /* local memory */

static void test_depth_clear_value(void)
{
    printf("-- depth clear value\n");
    engine_up();
    m(M_CONTEXT_DMA_ZETA, DMA_LOCAL);
    m(M_ZETA_OFFSET, ZETA_OFFSET);

    /* Z16 (depth field 1): the clear value is a bare depth16, and 0xFFFF its
     * far plane. Read as Z24S8 it would clear to 255/16777215. */
    m(M_SURFACE_FORMAT, 8u | (1u << 5));
    m(M_ZSTENCIL_CLEAR, 0xFFFFu);
    m(M_CLEAR_BUFFERS, 0x01u);
    CHECK(stub.n_clear_ds == 1 && stub.last_clear_depth == 1.0f,
          "a Z16 zeta cleared with 0xFFFF clears to 1.0 (%g)", stub.last_clear_depth);

    /* Z24S8 (depth field 2): depth24 << 8 | stencil8. */
    m(M_SURFACE_FORMAT, 8u | (2u << 5));
    m(M_ZSTENCIL_CLEAR, 0xFFFFFF00u);
    m(M_CLEAR_BUFFERS, 0x01u);
    CHECK(stub.last_clear_depth == 1.0f,
          "a Z24S8 zeta cleared with 0xFFFFFF00 clears to 1.0 (%g)", stub.last_clear_depth);
    engine_down();
}

/* ---- 9b. antialiased surfaces at sample resolution ------------------------ */

#define M_VIEWPORT_TRANSLATE  0x0A20
#define M_VIEWPORT_SCALE      0x0A30

static void mf(u32 method, float f) { u32 w; memcpy(&w, &f, 4); m(method, w); }

/* An antialiased surface holds its sample grid in memory (SURFACE_FORMAT
 * bits 12-15: 3 = 2x1, 4 and 5 = 2x2), so a texture reading it sees that
 * size. The surface is created at sample resolution; the viewport and
 * scissor registers stay in guest pixels and are scaled to match. */
static void test_antialiased_surface(void)
{
    printf("-- antialiased surface\n");
    static const struct { u32 aa, w, h, sx, sy; } cases[] = {
        { 0, 128,  64, 1, 1 },     /* CENTER_1 */
        { 3, 256,  64, 2, 1 },     /* DIAGONAL_CENTERED_2 */
        { 4, 256, 128, 2, 2 },     /* SQUARE_CENTERED_4 */
        { 5, 256, 128, 2, 2 },     /* SQUARE_ROTATED_4 */
    };
    for (u32 i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        engine_up();
        m(M_SURFACE_FORMAT, 8u | (cases[i].aa << 12));
        m(M_SURFACE_CLIP_H, 128u << 16);
        m(M_SURFACE_CLIP_V, 64u << 16);
        m(M_COLOR_A_OFFSET, 0x80000u);
        m(M_SCISSOR_H, 8u | (32u << 16));
        m(M_SCISSOR_V, 4u | (16u << 16));
        /* The whole 128x64-pixel viewport. */
        mf(M_VIEWPORT_TRANSLATE + 0, 64.0f); mf(M_VIEWPORT_TRANSLATE + 4, 32.0f);
        mf(M_VIEWPORT_SCALE + 0, 64.0f);     mf(M_VIEWPORT_SCALE + 4, -32.0f);
        draw_triangle();
        CHECK(stub.color_w == cases[i].w && stub.color_h == cases[i].h,
              "antialias %u: a 128x64 surface is created at %ux%u (got %ux%u)",
              cases[i].aa, cases[i].w, cases[i].h, stub.color_w, stub.color_h);
        CHECK(stub.scissor[0] == 8u * cases[i].sx && stub.scissor[1] == 4u * cases[i].sy &&
              stub.scissor[2] == 32u * cases[i].sx && stub.scissor[3] == 16u * cases[i].sy,
              "antialias %u: scissor 8,4 32x16 covers %u,%u %ux%u samples (got %u,%u %ux%u)",
              cases[i].aa, 8u * cases[i].sx, 4u * cases[i].sy, 32u * cases[i].sx,
              16u * cases[i].sy, stub.scissor[0], stub.scissor[1], stub.scissor[2],
              stub.scissor[3]);
        CHECK(stub.vp_xf[0] == 1.0f && stub.vp_xf[1] == 1.0f &&
              stub.vp_xf[4] == 0.0f && stub.vp_xf[5] == 0.0f,
              "antialias %u: the full-pixel viewport still maps to the whole surface "
              "(scale %g,%g offset %g,%g)", cases[i].aa, stub.vp_xf[0], stub.vp_xf[1],
              stub.vp_xf[4], stub.vp_xf[5]);
        engine_down();
    }
}

/* ---- 10. a zeta or a target read in a texture format of its own --------- */

static void test_depth_read_as_color(void)
{
    printf("-- zeta read as A8R8G8B8\n");
    engine_up();
    /* A depth-only pass, with colour A named at the zeta's own address and
     * nothing written to it: how a title points an unused colour target. */
    m(M_CONTEXT_DMA_ZETA, DMA_LOCAL);
    m(M_SURFACE_FORMAT, 8u | (2u << 5));
    m(M_ZETA_OFFSET, ZETA_OFFSET);
    m(M_ZETA_PITCH, 256u * 4u);
    m(M_COLOR_A_OFFSET, ZETA_OFFSET);
    m(M_COLOR_MASK, 0u);
    m(M_DEPTH_TEST, 1u);
    m(M_DEPTH_WRITE, 1u);
    draw_triangle();
    CHECK(stub.n_draw == 1, "the depth-only pass drew");

    /* The next pass reads that address as A8R8G8B8, the zeta still bound:
     * the bytes are the zeta's, stored to guest memory from the depth
     * target, not the untouched colour target's. */
    m(M_COLOR_MASK, 0x01010101u);
    m(M_DEPTH_TEST, 0u);
    m(M_DEPTH_WRITE, 0u);
    m(M_COLOR_A_OFFSET, 0x90000u);
    bind_texture_unit(RSX_LOCATION_LOCAL, ZETA_OFFSET);
    const int uploads = stub.n_texture_create;
    draw_triangle();
    const u8* z = vm_base + GUEST_LOCAL_EA + ZETA_OFFSET;
    CHECK(stub.n_depth_readback == 1, "the depth target was read back (%d)",
          stub.n_depth_readback);
    CHECK(z[0] == 0x80 && z[1] == 0x00 && z[2] == 0x00 && z[3] == 0x00,
          "guest memory holds depth 0.5 laid out as Z24S8 (%02X %02X %02X %02X)",
          z[0], z[1], z[2], z[3]);
    CHECK(stub.n_texture_create == uploads + 1 &&
          stub.bound_tex[0] == stub.last_created_texture,
          "the unit samples a texture decoded from those bytes");

    /* No depth write since: nothing is read back again. */
    draw_triangle();
    CHECK(stub.n_depth_readback == 1,
          "an unchanged depth target is not read back again (%d)", stub.n_depth_readback);
    engine_down();
}

/* A DEPTH24_D8 texture over a written depth target samples the backend's
 * snapshot of it. A clear invalidates that snapshot and the next read takes a
 * new one; the old one has to be released, or a shadow map cleared and read
 * every frame leaks a backend texture a frame. */
static void depth_pass(void)
{
    m(M_ZETA_OFFSET, ZETA_OFFSET);
    m(M_COLOR_MASK, 0u);
    m(M_DEPTH_TEST, 1u);
    m(M_DEPTH_WRITE, 1u);
    m(M_CLEAR_BUFFERS, 0x01u);
    draw_triangle();
}

static u32 sample_depth(void)
{
    m(M_ZETA_OFFSET, 0x00200000u);            /* another zeta: not the one read */
    m(M_COLOR_MASK, 0x01010101u);
    m(M_DEPTH_TEST, 0u);
    m(M_DEPTH_WRITE, 0u);
    bind_texture_unit(RSX_LOCATION_LOCAL, ZETA_OFFSET);
    m(M_TEX_UNIT + 0x04, 1u | (2u << 4) | ((0x90u | 0x20u) << 8) | (1u << 16));
    draw_triangle();
    return stub.bound_tex[0];
}

static void test_depth_snapshot_release(void)
{
    printf("-- depth snapshot release\n");
    engine_up();
    m(M_CONTEXT_DMA_ZETA, DMA_LOCAL);
    m(M_SURFACE_FORMAT, 8u | (2u << 5));
    m(M_ZETA_PITCH, 256u * 4u);
    m(M_COLOR_A_OFFSET, 0x90000u);

    depth_pass();
    const u32 first = sample_depth();
    CHECK(stub.n_depth_snapshot == 1 && first && stub.alive[first],
          "the first read samples a snapshot (%d, handle %u)", stub.n_depth_snapshot, first);
    CHECK(sample_depth() == first && stub.n_depth_snapshot == 1,
          "an unchanged depth target keeps its snapshot");

    depth_pass();
    const u32 second = sample_depth();
    CHECK(stub.n_depth_snapshot == 2 && second && second != first,
          "after a clear and a new depth pass the read takes a new snapshot (%d)",
          stub.n_depth_snapshot);
    CHECK(!stub.alive[first], "the stale snapshot was released");
    CHECK(stub.alive[second], "the current one is alive");

    /* A clip bigger than the canvas recreates the depth target; its
     * snapshot goes too. */
    m(M_SURFACE_CLIP_H, 2048u << 16);
    m(M_SURFACE_CLIP_V, 1024u << 16);
    m(M_ZETA_OFFSET, ZETA_OFFSET);
    m(M_DEPTH_TEST, 1u);
    m(M_DEPTH_WRITE, 1u);
    draw_triangle();
    CHECK(stub.n_depth_create >= 3 && !stub.alive[second],
          "a recreated depth target releases its old snapshot (%d depth targets)",
          stub.n_depth_create);
    engine_down();
}

static void test_color_read_as_float(void)
{
    printf("-- colour target read as X32_FLOAT\n");
    engine_up();
    m(M_COLOR_A_PITCH, 256u * 4u);
    m(M_CLEAR_BUFFERS, 0xF0u);                   /* a colour write at 0x40000 */
    const u32 surface = stub.last_created_color;

    /* Read back as X32_FLOAT, the target's 32-bit pixels are floats a pass
     * packed into it: they come from guest memory, where the target's pixels
     * are stored as A,R,G,B first. */
    m(M_COLOR_A_OFFSET, 0x90000u);
    bind_texture_unit(RSX_LOCATION_LOCAL, 0x40000u);
    m(M_TEX_UNIT + 0x04, 1u | (2u << 4) | ((0x9Cu | 0x20u) << 8) | (1u << 16));
    const int uploads = stub.n_texture_create;
    draw_triangle();
    const u8* p = vm_base + GUEST_LOCAL_EA + 0x40000u;
    CHECK(p[0] == 0x44 && p[1] == 0x11 && p[2] == 0x22 && p[3] == 0x33,
          "the target was stored to guest memory as A,R,G,B (%02X %02X %02X %02X)",
          p[0], p[1], p[2], p[3]);
    CHECK(stub.n_texture_create == uploads + 1 && stub.bound_tex[0] != surface,
          "the unit decodes those bytes instead of sampling the RGBA8 image");
    engine_down();
}

/* The register-based fullscreen compositor has no array or inline stream.
 * Capture each position completion with the attributes current at that time. */
static void immediate_vertex(float x, float y, float u, float v)
{
    m(0x18C0, f32_bits(u)); m(0x18C4, f32_bits(v)); /* ATTR8 DATA2F */
    m(0x1500, f32_bits(x)); m(0x1504, f32_bits(y));
    m(0x1508, f32_bits(-1.0f));                    /* ATTR0 DATA3F */
}

static void test_immediate_compositor(void)
{
    engine_up();
    /* Leave array declarations pointing outside readable guest memory. */
    m(M_VTXBUF_OFFSET, 0xFFFFFFF0u);
    m(0x1738, 0x7FFFFFF0u); m(0x173C, 0x7FFFFFF0u);
    m(M_VTXFMT, 2u | (4u << 4) | (16u << 8) | (7u << 16));
    immediate_vertex(99, 98, 97, 96); /* constants outside BEGIN do not draw */
    m(M_BEGIN_END, 6u);              /* retail fullscreen TRIANGLE_STRIP */
    immediate_vertex(-1, -1, 0, 1);
    immediate_vertex(3, -1, 2, 1);
    immediate_vertex(-1, 3, 0, -1);
    m(M_BEGIN_END, 0);
    CHECK(stub.n_draw == 1, "register fullscreen triangle issues a draw");
    CHECK(stub.draw_vertices == 3 && stub.draw_indices == 3,
          "register triangle uploads 3 vertices/indices, got %u/%u",
          stub.draw_vertices, stub.draw_indices);
    CHECK(stub.draw_topology == RSX_TOPOLOGY_TRIANGLES, "strip rebuilt as triangles");
    const float expected[3][4] = {{-1,-1,0,1}, {3,-1,2,1}, {-1,3,0,-1}};
    for (u32 i = 0; i < 3; i++) {
        CHECK(stub.captured_vertices[i][0] == expected[i][0] &&
              stub.captured_vertices[i][1] == expected[i][1] &&
              stub.captured_vertices[i][2] == -1 && stub.captured_vertices[i][3] == 1,
              "immediate position %u preserved independently of arrays", i);
        CHECK(stub.captured_vertices[i][32] == expected[i][2] &&
              stub.captured_vertices[i][33] == expected[i][3] &&
              stub.captured_vertices[i][34] == 0 && stub.captured_vertices[i][35] == 1,
              "immediate UV %u preserved at position completion", i);
        CHECK(stub.captured_indices[i] == i, "immediate strip index %u", i);
    }
    m(M_BEGIN_END, 8u);
    immediate_vertex(-1,-1,0,0); immediate_vertex(1,-1,1,0);
    immediate_vertex(1,1,1,1); immediate_vertex(-1,1,0,1);
    m(M_BEGIN_END, 0);
    CHECK(stub.n_draw == 2 && stub.draw_vertices == 4 && stub.draw_indices == 6,
          "immediate quad rebuilt without previous group vertices");
    m(M_BEGIN_END, 6u);
    m(0x18C0, f32_bits(8)); m(0x18C4, f32_bits(9));
    m(0x1500, f32_bits(8)); m(0x1504, f32_bits(9)); /* incomplete position */
    m(M_BEGIN_END, 0);
    CHECK(stub.n_draw == 2, "UV and incomplete position do not emit a vertex");
    /* DATA4F completes on W and preserves an explicit W. */
    m(M_BEGIN_END, 5u);
    for (u32 i = 0; i < 3; i++) {
        m(0x1C00, f32_bits((float)i)); m(0x1C04, f32_bits(2));
        m(0x1C08, f32_bits(3)); m(0x1C0C, f32_bits(4));
    }
    m(M_BEGIN_END, 0);
    CHECK(stub.n_draw == 3 && stub.draw_vertices == 3 &&
          stub.captured_vertices[2][0] == 2 && stub.captured_vertices[2][3] == 4,
          "DATA4F position emits once at W with explicit W preserved");
    m(M_BEGIN_END, 5u);
    immediate_vertex(1,2,3,4);
    m(M_DRAW_ARRAYS, 2u << 24);
    m(M_BEGIN_END, 0);
    CHECK(stub.n_draw == 3, "mixed immediate and arrays reject the entire group");
    m(M_BEGIN_END, 5u);
    for (u32 i = 0; i < 4098; i++) immediate_vertex(1,2,3,4);
    m(M_BEGIN_END, 0);
    CHECK(stub.n_draw == 3, "oversized immediate group never draws a truncated prefix");
    m(M_BEGIN_END, 6u);
    immediate_vertex(-1,-1,0,1); immediate_vertex(3,-1,2,1);
    immediate_vertex(-1,3,0,-1);
    m(M_BEGIN_END, 0);
    CHECK(stub.n_draw == 4 && stub.draw_vertices == 3,
          "BEGIN recovers after oversized or mixed groups");
    m(M_BEGIN_END, 5u);
    for (u32 attr = 0; attr < 16; attr++)
        for (u32 lane = 0; lane < 4; lane++) m(0x1C00 + attr * 16 + lane * 4, 0);
    /* The ATTR0 above emits the first vertex, then emit two more zero positions. */
    for (u32 v = 0; v < 2; v++)
        for (u32 lane = 0; lane < 4; lane++) m(0x1C00 + lane * 4, 0);
    m(M_BEGIN_END, 0);
    CHECK(stub.n_draw == 5 && stub.captured_vertices[1][3] == 0 &&
          stub.captured_vertices[1][35] == 0,
          "explicit all-zero DATA4F preserves position W and attribute alpha");
    engine_down();
}

static void test_immediate_packed_zero_and_inline(void)
{
    const u32 packed[] = {0x194Cu, 0x1998u, 0x0A98u};
    for (u32 k = 0; k < 3; k++) {
        engine_up();
        m(packed[k], 0);
        if (k) m(packed[k] + 4, 0);
        m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
        immediate_vertex(0,0,0,0); immediate_vertex(1,0,0,0);
        immediate_vertex(0,1,0,0);
        m(M_BEGIN_END, 0);
        CHECK(stub.n_draw == 1 && stub.captured_vertices[0][15] == 0,
              "packed all-zero method 0x%X preserves explicit alpha", packed[k]);
        engine_down();
    }
    engine_up();
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    m(M_INLINE_ARRAY, 0); /* shorter than declared stride */
    immediate_vertex(0,0,0,0); immediate_vertex(1,0,0,0);
    immediate_vertex(0,1,0,0);
    m(M_BEGIN_END, 0);
    CHECK(stub.n_draw == 0, "short inline stream mixed with immediate rejects group");
    m(M_VTXFMT, 0); /* no enabled inline attribute */
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    m(M_INLINE_ARRAY, 0);
    immediate_vertex(0,0,0,0); immediate_vertex(1,0,0,0);
    immediate_vertex(0,1,0,0);
    m(M_BEGIN_END, 0);
    CHECK(stub.n_draw == 0, "undeclared inline stream mixed with immediate rejects group");
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    immediate_vertex(0,0,0,0); immediate_vertex(1,0,0,0);
    immediate_vertex(0,1,0,0);
    m(M_BEGIN_END, 0);
    CHECK(stub.n_draw == 1, "BEGIN clears rejected inline source state");
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    immediate_vertex(0,0,0,0); immediate_vertex(1,0,0,0);
    immediate_vertex(0,1,0,0);
    for (u32 i = 0; i <= RSX_DSP_INLINE_MAX_BYTES / 4u; i++)
        m(M_INLINE_ARRAY, 0);
    m(M_BEGIN_END, 0);
    CHECK(stub.n_draw == 1, "overflowed inline stream mixed with immediate rejects group");
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    immediate_vertex(0,0,0,0); immediate_vertex(1,0,0,0);
    immediate_vertex(0,1,0,0);
    m(M_BEGIN_END, 0);
    CHECK(stub.n_draw == 2, "BEGIN recovers after mixed inline overflow");
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
    test_texture_sparse_rehash();
    test_pipeline_key();
    test_restart_expansion();
    test_indexed_draws();
    test_scissor();
    test_readback();
    test_custom_guest_mapping();
    test_queued_flip_buffer();
    test_transfer_coherence();
    test_transfer_rows();
    test_transfer_reads_fp16();
    test_inline_array();
    test_depth_clear_value();
    test_antialiased_surface();
    test_depth_read_as_color();
    test_depth_snapshot_release();
    test_color_read_as_float();
    test_immediate_compositor();
    test_immediate_packed_zero_and_inline();

    free(vm_base);
    printf(g_failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n",
           g_failures);
    return g_failures ? 1 : 0;
}
