/*
 * ps3recomp - platform-neutral RSX draw engine (see rsx_draw_engine.h)
 *
 * The orchestration carried across from libs/video/rsx_live_draw.c, the
 * NV4097 -> D3D12 engine a title has shipped on. Line numbers in the comments
 * below are that file's, so the two can be read side by side; the file itself
 * is vendored and stays exactly as it is.
 *
 * What is deliberately left out, because it is not renderer behaviour a title
 * needs: the shader disk cache, movie mode and its compositor, the a010
 * probe, and every YZ_PERF_PROFILE block.
 */
#include "rsx_draw_engine.h"

#include "rsx_fp_decompiler.h"
#include "rsx_primitives.h"
#include "rsx_restart_cuts.h"
#include "rsx_vertex_formats.h"
#include "rsx_vp_decompiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ps3emu/log_first.h"

/* Guest memory and the RSX offset resolvers, declared the way every backend
 * that reads guest data declares them (rsx_vertex_fetch.h). */
extern u8* vm_base;
u32 cellGcmResolveLocated(int local, u32 offset);
/* cellGcmSys.c: the display buffers registered before the engine came up. */
void cellGcm_replay_display_buffers_to_engine(void);
/* Non-zero only when the offset's page is in the IO table, so a caller can
 * tell IO-mapped main memory from "assume VRAM". */
u32 cellGcmResolveIO(u32 offset);
/* The guest VM's OOB guard: the size a host backed, or 0 for "the whole 32-bit
 * space is backed and no check is needed" (runtime/memory/vm.h). */
extern u32 ppu_vm_size;

/* ------------------------------------------------------------------------- */

#define ENG_MAX_SURFACES   64
#define ENG_MAX_ZDEPTHS    64
#define ENG_MAX_TEXTURES   RSX_DRAW_ENGINE_TEXTURE_CACHE
#define ENG_MAX_VTEXTURES  64
#define ENG_MAX_PIPELINES  8192
#define ENG_MAX_BATCHES    256
#define ENG_VERT_STRIDE    (RSX_DSP_NUM_VERTEX_ATTR * 16u)
#define ENG_INVALID        0xFFFFFFFFu
/* Guest textures larger than this are a misdecoded register, not an image;
 * rsx_live_draw.c rejects the same 4096 bound (2033). */
#define ENG_MAX_TEX_DIM    4096u
/* A surface declaration beyond this is a guest pointer read as clip
 * dimensions. rsx_live_draw.c's surface-guard (2625) preserves the existing
 * target rather than destroying a live one. */
#define ENG_MAX_SURFACE_DIM 8192u

#define ENG_VP_CB_BYTES    ((RSX_DSP_NUM_CONSTANTS + 2) * 16u)
#define ENG_FP_MAX_BYTES   0x10000u

#define M_ALPHA_TEST_ENABLE   0x0304
#define M_ALPHA_FUNC          0x0308
#define M_ALPHA_REF           0x030C
#define M_BLEND_ENABLE        0x0310
#define M_BLEND_SFACTOR       0x0314
#define M_BLEND_DFACTOR       0x0318
#define M_BLEND_EQUATION      0x0320
#define M_COLOR_MASK          0x0324
#define M_STENCIL_TEST_ENABLE 0x0328
#define M_STENCIL_MASK        0x032C
#define M_STENCIL_FUNC        0x0330
#define M_STENCIL_FUNC_REF    0x0334
#define M_STENCIL_FUNC_MASK   0x0338
#define M_STENCIL_OP_FAIL     0x033C
#define M_STENCIL_OP_ZFAIL    0x0340
#define M_STENCIL_OP_ZPASS    0x0344
#define M_TWO_SIDED_STENCIL   0x0348
#define M_BACK_STENCIL_FUNC   0x0350
#define M_BACK_STENCIL_OP_FAIL  0x035C
#define M_BACK_STENCIL_OP_ZFAIL 0x0360
#define M_BACK_STENCIL_OP_ZPASS 0x0364
#define M_DEPTH_BOUNDS_ENABLE 0x0380
#define M_DEPTH_BOUNDS_MIN    0x0384
#define M_DEPTH_BOUNDS_MAX    0x0388
#define M_SCISSOR_HORIZONTAL  0x08C0
#define M_SCISSOR_VERTICAL    0x08C4
#define M_DEPTH_FUNC          0x0A6C
#define M_DEPTH_WRITE         0x0A70
#define M_DEPTH_TEST_ENABLE   0x0A74
#define M_ZSTENCIL_CLEAR      0x1D8C

/* ---- module state -------------------------------------------------------- */

typedef struct {
    u32 location, offset;
    u32 w, h;
    u32 pitch;              /* guest bytes per row, from SET_SURFACE_PITCH */
    rsx_be_format fmt;
    u32 handle;
    u32 feedback;          /* last immutable own-target copy, engine-owned */
    u64 color_serial;       /* g.write_serial of the last colour write, or 0 */
    int guest_dirty;        /* rendered since guest memory last got a copy   */
} eng_surface;

typedef struct {
    u32 location, offset;
    u32 w, h;
    u32 handle;
    u32 snapshot;          /* sampleable copy, or 0                        */
    int cleared;
    int had_write;
    int snapshot_valid;
    u64 depth_serial;      /* g.write_serial of the last depth write or clear */
    int guest_dirty;       /* written since guest memory last got a copy     */
    u32 zfmt, pitch;       /* SURFACE_FORMAT depth field and zeta pitch then */
    u32 used_w, used_h;    /* the clip that write went through              */
} eng_zdepth;

typedef struct {
    u32 location, offset, format, width, height, pitch, remap, cubemap;
    u32 handle;
    u64 content_hash;
    u64 sparse_hash;            /* eng_texture_sparse_hash, the per-frame check */
    u32 last_full_frame;        /* g.frames of the last full content hash */
    u32 last_hash_frame;
    u64 last_use_serial;
    u32 depth;                  /* slices of a 3D texture, else 1 */
} eng_texture;

typedef struct {
    u64 key;
    u32 handle;
    u8 fixed;
    u16 tex_used;           /* units the pixel program samples */
    u64 fp_hash;            /* eng_fp_debug_hash of the guest fragment program */
} eng_pipeline;

typedef struct { u32 first, count; } eng_batch;

typedef struct {
    u32 location, offset, pitch, width, height;
    int valid;
} eng_display_buffer;

static struct {
    const rsx_draw_backend* be;
    int ready;
    int default_on;
    u32 width, height;

    rsx_dispatch rsx;

    eng_surface surfaces[ENG_MAX_SURFACES];
    u32 n_surfaces;
    eng_zdepth zdepths[ENG_MAX_ZDEPTHS];
    u32 n_zdepths;
    eng_texture textures[ENG_MAX_TEXTURES];
    u32 n_textures;
    eng_pipeline pipelines[ENG_MAX_PIPELINES];
    u32 n_pipelines;
    eng_display_buffer display_buffers[8];

    u32 frames;
    u64 texture_use_serial;
    u32 guest_draws;
    u32 last_guest_draws;
    u32 last_present_surface;
    u32 last_flip_buffer;
    u64 write_serial;       /* orders colour and depth writes to one address */

    /* staging: decoded texture levels, the constant blocks, the index list */
    u8* tex_staging;
    u32 tex_staging_cap;
    u8* vp_cb;
    u8* fp_cb;
    u32 fp_cb_cap;
    u32* indices;
    u32 index_cap;

    rsx_fp_constant_block fp_constants;
} g;

/* The decompilers' HLSL. The VP decompiler builds bodies up to 192 KB. */
static char s_vs_hlsl[256 * 1024];
static char s_ps_hlsl[256 * 1024];

/* The draw accumulator: one BEGIN_END group's batches, references and cuts. */
static struct {
    eng_batch arr[ENG_MAX_BATCHES];
    u32 n_arr;
    eng_batch idx[ENG_MAX_BATCHES];
    u32 n_idx;
    u32 n_packets;

    rsx_vertex_ref* refs;
    u32 n_refs, cap_refs, n_source_refs;
    int refs_remapped;
    rsx_vertex_remap ref_remap;

    u32* cuts;
    u32 n_cuts, cap_cuts;

    u8* verts;
    u64 verts_cap;
    u32 n_verts;

    /* Canonical ATTR0..15 float4 snapshots for register-based vertices.
     * Bounded separately from compact GPU staging; never fetch stale arrays. */
    float (*immediate)[RSX_DSP_NUM_VERTEX_ATTR][4];
    u32 n_immediate, cap_immediate;

    rsx_vertex_layout_plan layout;
    rsx_vertex_fetch_plan fetch_plan;
    int fetch_ok;

    /* INLINE_ARRAY stream for this group, when the vertices came down the
     * FIFO rather than from a vertex array. */
    const u8* inl;
    u32 inl_bytes;
    u32 inline_dropped_at_begin;
} dc;

/* ---- guest memory -------------------------------------------------------- */

/* rsx_dsp_* locations are RSX_LOCATION_LOCAL = 0 / _MAIN = 1;
 * cellGcmResolveLocated's argument is the opposite sense (1 = local), and
 * getting that backwards resolves every VRAM object through the IO table. */
static rsx_vertex_guest_ptr_fn s_guest_reader;
static void* s_guest_user;

void rsx_draw_engine_set_guest_memory(rsx_vertex_guest_ptr_fn reader, void* user)
{
    s_guest_reader = reader;
    s_guest_user = user;
}

static const u8* eng_guest_ptr(void* user, u32 location, u32 offset,
                               u32 min_bytes)
{
    (void)user;
    if (!min_bytes || min_bytes > (256u << 20)) return NULL;
    if (s_guest_reader) return s_guest_reader(s_guest_user, location, offset, min_bytes);
    if (!vm_base) return NULL;
    const u32 ea = cellGcmResolveLocated(location == RSX_LOCATION_LOCAL, offset);
    if (!ea) return NULL;
    if (ppu_vm_size && (u64)ea + min_bytes > ppu_vm_size) return NULL;
    return vm_base + ea;
}

static u64 eng_fnv1a(const void* data, u32 n, u64 hash)
{
    const u8* p = (const u8*)data;
    for (u32 i = 0; i < n; i++) { hash ^= p[i]; hash *= 1099511628211ull; }
    return hash;
}

/* ---- render state -------------------------------------------------------- */

void rsx_draw_engine_decode_render_state(const rsx_dispatch* rsx,
                                         rsx_be_render_state* rs)
{
    memset(rs, 0, sizeof(*rs));
    rs->alpha_test_enable = rsx_dsp_reg(rsx, M_ALPHA_TEST_ENABLE) & 1;
    rs->alpha_func   = rsx_dsp_reg(rsx, M_ALPHA_FUNC);
    rs->alpha_ref_raw = rsx_dsp_reg(rsx, M_ALPHA_REF);
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(rsx, &sf);
    rs->alpha_ref_format = sf.color_format;
    rs->rt_fp16 = sf.color_format == RSX_SURFACE_FMT_F_W16Z16Y16X16;
    rs->blend_enable = rsx_dsp_reg(rsx, M_BLEND_ENABLE) & 1;
    const u32 sfac = rsx_dsp_reg(rsx, M_BLEND_SFACTOR);
    const u32 dfac = rsx_dsp_reg(rsx, M_BLEND_DFACTOR);
    const u32 eq   = rsx_dsp_reg(rsx, M_BLEND_EQUATION);
    rs->sf_rgb = sfac & 0xFFFF; rs->sf_a = sfac >> 16;
    rs->df_rgb = dfac & 0xFFFF; rs->df_a = dfac >> 16;
    rs->eq_rgb = eq & 0xFFFF;   rs->eq_a = eq >> 16;
    rs->depth_test  = rsx_dsp_reg(rsx, M_DEPTH_TEST_ENABLE) & 1;
    rs->depth_write = rsx_dsp_reg(rsx, M_DEPTH_WRITE) & 1;
    rs->depth_func  = rsx_dsp_reg(rsx, M_DEPTH_FUNC);
    rs->cull_enable = rsx_dsp_reg(rsx, 0x183C) & 1;
    rs->cull_face   = rsx_dsp_reg(rsx, 0x1830);
    rs->front_face  = rsx_dsp_reg(rsx, 0x1834);
    /* The RAW register: a game-written 0 is a real "write no colour channel"
     * (a depth-prime pass), and rsx_dispatch_init seeds the nv40 reset value
     * so never-written reads as all-on. */
    rs->color_mask  = rsx_dsp_reg(rsx, M_COLOR_MASK);
    rs->color_mask_mrt = rsx_dsp_reg(rsx, 0x0370) & 0xFFF0u;
    rs->blend_mrt      = rsx_dsp_reg(rsx, 0x036C) & 0xEu;
    rs->stencil_enable    = rsx_dsp_reg(rsx, M_STENCIL_TEST_ENABLE) & 1;
    rs->stencil_two_sided = rsx_dsp_reg(rsx, M_TWO_SIDED_STENCIL) & 1;
    rs->s_func       = rsx_dsp_reg(rsx, M_STENCIL_FUNC);
    rs->s_func_mask  = rsx_dsp_reg(rsx, M_STENCIL_FUNC_MASK) & 0xFF;
    rs->s_write_mask = rsx_dsp_reg(rsx, M_STENCIL_MASK) & 0xFF;
    rs->s_fail       = rsx_dsp_reg(rsx, M_STENCIL_OP_FAIL);
    rs->s_zfail      = rsx_dsp_reg(rsx, M_STENCIL_OP_ZFAIL);
    rs->s_zpass      = rsx_dsp_reg(rsx, M_STENCIL_OP_ZPASS);
    rs->bs_func      = rsx_dsp_reg(rsx, M_BACK_STENCIL_FUNC);
    rs->bs_fail      = rsx_dsp_reg(rsx, M_BACK_STENCIL_OP_FAIL);
    rs->bs_zfail     = rsx_dsp_reg(rsx, M_BACK_STENCIL_OP_ZFAIL);
    rs->bs_zpass     = rsx_dsp_reg(rsx, M_BACK_STENCIL_OP_ZPASS);
    rs->depth_bounds_test = rsx_dsp_reg(rsx, M_DEPTH_BOUNDS_ENABLE) & 1;
}

u64 rsx_draw_engine_hash_render_state(const rsx_be_render_state* rs, u64 hash)
{
#define ENG_HASH_FIELD(name) hash = eng_fnv1a(&rs->name, sizeof(rs->name), hash)
    /* The alpha REFERENCE is deliberately absent: it lives in the fragment
     * constant block, so a title animating a fade must not compile a new
     * pipeline per frame. Enable and compare mode still select a variant.
     * Every field is listed by name so struct padding can never be identity
     * (rsx_live_draw.c:3151-3185). */
    ENG_HASH_FIELD(alpha_test_enable);
    ENG_HASH_FIELD(alpha_func);
    ENG_HASH_FIELD(blend_enable);
    ENG_HASH_FIELD(sf_rgb);
    ENG_HASH_FIELD(df_rgb);
    ENG_HASH_FIELD(sf_a);
    ENG_HASH_FIELD(df_a);
    ENG_HASH_FIELD(eq_rgb);
    ENG_HASH_FIELD(eq_a);
    ENG_HASH_FIELD(depth_test);
    ENG_HASH_FIELD(depth_write);
    ENG_HASH_FIELD(depth_func);
    ENG_HASH_FIELD(cull_enable);
    ENG_HASH_FIELD(cull_face);
    ENG_HASH_FIELD(front_face);
    ENG_HASH_FIELD(color_mask);
    ENG_HASH_FIELD(color_mask_mrt);
    ENG_HASH_FIELD(blend_mrt);
    ENG_HASH_FIELD(rt_fp16);
    ENG_HASH_FIELD(stencil_enable);
    ENG_HASH_FIELD(stencil_two_sided);
    ENG_HASH_FIELD(s_func);
    ENG_HASH_FIELD(s_func_mask);
    ENG_HASH_FIELD(s_write_mask);
    ENG_HASH_FIELD(s_fail);
    ENG_HASH_FIELD(s_zfail);
    ENG_HASH_FIELD(s_zpass);
    ENG_HASH_FIELD(bs_func);
    ENG_HASH_FIELD(bs_fail);
    ENG_HASH_FIELD(bs_zfail);
    ENG_HASH_FIELD(bs_zpass);
    ENG_HASH_FIELD(depth_bounds_test);
#undef ENG_HASH_FIELD
    return hash;
}

/* ---- topology expansion -------------------------------------------------- */

/* Every expansion is bounded by the restart cuts, so a strip that the guest
 * broke with the sentinel index gets no connecting triangle across the break.
 * Missing that decode is what upstream's aa7fb63 records as "the exploded
 * spiky mesh that occluded the scene". */
u32 rsx_draw_engine_topology_index_count(u32 primitive, u32 source_refs,
                                         const u32* cuts, u32 cut_count)
{
    const u32 segments = cut_count + 1;
    switch (primitive) {
    case RSX_PRIMITIVE_TRIANGLES:
        return source_refs - source_refs % 3u;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_POLYGON: {
        u32 total = 0;
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            (void)begin;
            if (count >= 3) total += (count - 2) * 3;
        }
        return total;
    }
    case RSX_PRIMITIVE_QUAD_STRIP: {
        /* Two vertices per quad after the first pair, so a segment of n
         * carries (n - 2) / 2 quads and an odd trailing vertex is dropped. */
        u32 total = 0;
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            (void)begin;
            if (count >= 4) total += ((count - 2u) / 2u) * 6u;
        }
        return total;
    }
    case RSX_PRIMITIVE_QUADS:
        return (source_refs / 4u) * 6u;
    default:
        return 0;
    }
}

static u32 eng_topology_vertex(const u32* occurrence_to_unique, u32 occurrence)
{
    return occurrence_to_unique ? occurrence_to_unique[occurrence] : occurrence;
}

void rsx_draw_engine_write_topology_indices(u32 primitive, u32 source_refs,
                                            const u32* cuts, u32 cut_count,
                                            const u32* occurrence_to_unique,
                                            u32* indices)
{
    u32 write = 0;
    const u32 segments = cut_count + 1;
    switch (primitive) {
    case RSX_PRIMITIVE_TRIANGLES: {
        const u32 count = source_refs - source_refs % 3u;
        for (u32 o = 0; o < count; o++)
            indices[write++] = eng_topology_vertex(occurrence_to_unique, o);
        break;
    }
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            if (count < 3) continue;
            /* Odd triangles swap their first two vertices, so the whole strip
             * keeps one winding. */
            for (u32 i = 0; i + 2 < count; i++) {
                indices[write++] = eng_topology_vertex(
                    occurrence_to_unique, begin + i + (i & 1u));
                indices[write++] = eng_topology_vertex(
                    occurrence_to_unique, begin + i + 1u - (i & 1u));
                indices[write++] = eng_topology_vertex(
                    occurrence_to_unique, begin + i + 2u);
            }
        }
        break;
    /* A polygon is a convex fan around its own first vertex, which is what
     * both of this tree's other renderers make of one. */
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_POLYGON:
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            if (count < 3) continue;
            for (u32 i = 1; i + 1 < count; i++) {
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + i);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + i + 1u);
            }
        }
        break;
    case RSX_PRIMITIVE_QUAD_STRIP:
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            if (count < 4) continue;
            /* Every quad is split along the diagonal between the two pairs,
             * so each one keeps the strip's winding without alternating the
             * way a triangle strip has to; a cut simply starts the pairing
             * again from the segment's own first vertex. This is the
             * expansion the vtable path's emit_vertices performs. */
            for (u32 q = 0; q + 3 < count; q += 2) {
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q + 1u);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q + 2u);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q + 1u);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q + 3u);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q + 2u);
            }
        }
        break;
    case RSX_PRIMITIVE_QUADS:
        for (u32 q = 0; q < source_refs / 4u; q++) {
            const u32 base = q * 4u;
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base);
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base + 1u);
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base + 2u);
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base + 2u);
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base + 3u);
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base);
        }
        break;
    default:
        break;
    }
}

/* Does this primitive become an indexed triangle list, and is the index buffer
 * needed at all? rsx_vertex_topology_plan answers that for the reference
 * engine, whose own expansion stops at quads. Quad strips and polygons are
 * this engine's, so their two cases are decided here rather than in the
 * shared helper, which the vendored rsx_live_draw.c also calls. */
static int eng_topology_rebuild(u32 primitive, int refs_remapped, int* indexed)
{
    if (primitive == RSX_PRIMITIVE_QUAD_STRIP ||
        primitive == RSX_PRIMITIVE_POLYGON) {
        (void)refs_remapped;
        *indexed = 1;
        return 1;
    }
    return rsx_vertex_topology_plan(primitive, refs_remapped, indexed);
}

/* ---- surfaces ------------------------------------------------------------ */

static rsx_be_format eng_surface_format(u32 color_format)
{
    switch (color_format & 0x1Fu) {
    case RSX_SURFACE_FMT_F_W16Z16Y16X16: return RSX_BE_FMT_R16G16B16A16F;
    case 0x0C: return RSX_BE_FMT_R32G32B32A32F;
    case 0x0D: return RSX_BE_FMT_R32F;
    default:   return RSX_BE_FMT_R8G8B8A8;
    }
}

/* Decode the guest's own bytes behind a colour surface into host R,G,B,A rows
 * so a freshly created target starts with what the title CPU-initialised it
 * to. Returns the staging pointer, or NULL when the bytes are unreadable or
 * the format is not one the decoder handles. */
static const void* eng_surface_seed(u32 location, u32 offset, u32 w, u32 h,
                                    rsx_be_format fmt, u32* out_row_bytes)
{
    *out_row_bytes = 0;
    if (fmt != RSX_BE_FMT_R8G8B8A8) return NULL;
    /* Only a surface in IO-mapped MAIN memory is seeded. RSX local memory is
     * reserved and not backed by this tree's guest VM -- runtime/memory/vm.h
     * maps the whole space PROT_NONE and commits main memory and the stacks,
     * leaving a runner to commit what its own title needs -- so reading a VRAM
     * surface's backing would fault long before it could help. Both halves are
     * checked: the DMA context says which space the offset is in, and the IO
     * table says the page is really mapped. */
    if (location != RSX_LOCATION_MAIN ||
        (!s_guest_reader && !cellGcmResolveIO(offset))) return NULL;
    rsx_tex_layout tl;
    /* A8R8G8B8 with the LN bit: a surface is linear, never swizzled. */
    rsx_texture_layout(0x85u | 0x20u, w, h, &tl);
    if (!tl.face_bytes || !tl.dst_row_bytes) return NULL;
    const u8* src = eng_guest_ptr(NULL, location, offset, tl.face_bytes);
    if (!src) return NULL;
    const u32 bytes = tl.dst_row_bytes * tl.rows;
    if (g.tex_staging_cap < bytes) {
        u8* n = (u8*)realloc(g.tex_staging, bytes);
        if (!n) return NULL;
        g.tex_staging = n;
        g.tex_staging_cap = bytes;
    }
    rsx_texture_decode(g.tex_staging, tl.dst_row_bytes, src, w, h, &tl,
                       rsx_texture_argb_is_rgba());
    *out_row_bytes = tl.dst_row_bytes;
    return g.tex_staging;
}

/* The colour target for this (location, offset), created or reallocated when
 * the size or format moved. Returns a slot index, or ENG_INVALID.
 * rsx_live_draw.c's surface_get (2605-2690).
 *
 * A smaller clip on the same rows is the same memory, not a new surface: with
 * the same format and pitch, RSX draws into the top-left of what is already
 * there. God of War: Ascension renders its scene into a 1280x720 target, then
 * runs a 640x360 pass at the same address and pitch and copies the full
 * 1280x720 out for its SPU anti-aliasing; recreating the target at 640x360
 * lost the scene and left that quarter of the frame black. */
static u32 eng_surface_get(u32 location, u32 offset, u32 want_w, u32 want_h,
                          rsx_be_format want_fmt, u32 pitch)
{
    if (!want_w) want_w = g.width;
    if (!want_h) want_h = g.height;

    u32 slot = ENG_MAX_SURFACES;
    for (u32 i = 0; i < g.n_surfaces; i++)
        if (g.surfaces[i].location == location && g.surfaces[i].offset == offset) {
            const eng_surface* s = &g.surfaces[i];
            if (s->fmt == want_fmt &&
                ((s->w == want_w && s->h == want_h) ||
                 (pitch && s->pitch == pitch && s->w >= want_w && s->h >= want_h)))
                return i;
            slot = i;
            break;
        }

    /* Never destroy a usable render target because one malformed command
     * decoded a guest pointer as clip dimensions. */
    if (want_w > ENG_MAX_SURFACE_DIM || want_h > ENG_MAX_SURFACE_DIM) {
        static u32 logs = 0;
        if (PS3_LOG_FIRST(logs, 8))
            fprintf(stderr, "[rsx engine] rejected implausible surface 0x%X %ux%u;"
                            " keeping the %s target\n", offset, want_w, want_h,
                    slot < ENG_MAX_SURFACES ? "existing" : "absent");
        return slot < ENG_MAX_SURFACES ? slot : ENG_INVALID;
    }
    if (slot == ENG_MAX_SURFACES) {
        if (g.n_surfaces >= ENG_MAX_SURFACES) return ENG_INVALID;
        slot = g.n_surfaces;
    }

    u32 seed_row = 0;
    const void* seed = eng_surface_seed(location, offset, want_w, want_h,
                                        want_fmt, &seed_row);
    const u32 handle = g.be->color_target_create(g.be->user, want_fmt,
                                                 want_w, want_h, seed, seed_row);
    if (!handle)
        return (slot < g.n_surfaces && g.surfaces[slot].handle) ? slot : ENG_INVALID;

    eng_surface* s = &g.surfaces[slot];
    if (s->feedback) { g.be->texture_release(g.be->user, s->feedback); s->feedback = 0; }
    if (s->handle) g.be->color_target_release(g.be->user, s->handle);
    s->location = location; s->offset = offset;
    s->w = want_w; s->h = want_h; s->fmt = want_fmt;
    s->handle = handle;
    s->color_serial = 0;
    s->guest_dirty = 0;
    if (slot == g.n_surfaces) g.n_surfaces++;
    { static u32 logs = 0; if (PS3_LOG_FIRST(logs, 16))
        fprintf(stderr, "[rsx engine] surface %u:0x%08X %ux%u fmt %d%s\n",
                location, offset, want_w, want_h, (int)want_fmt,
                seed ? " (seeded from guest memory)" : ""); }
    return slot;
}

/* The colour targets SET_SURFACE_COLOR_TARGET names, as slots into
 * g.surfaces, with A first. One for an ordinary draw; an MRT set adds B, C
 * and D, each created at target A's size and format, which is the rule the
 * vtable path's target_for binds by. Resolution stops at the first member
 * that cannot be created, as that path stops at the first gap. Returns how
 * many were resolved, or 0. */
static u32 eng_current_target_set(u32 slots[RSX_BE_MAX_COLOR_TARGETS])
{
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);
    /* SET_SURFACE_COLOR_TARGET 2 selects B alone; every other value starts at
     * A, and the MRT selectors add B, C and D on top of it. */
    const u32 sel = (sf.color_target == CELL_GCM_SURFACE_TARGET_1) ? 1u : 0u;
    const rsx_be_format fmt = eng_surface_format(sf.color_format);
    const u32 first = eng_surface_get(sf.color_location[sel], sf.color_offset[sel],
                                      sf.clip_w, sf.clip_h, fmt, sf.color_pitch[sel]);
    if (first == ENG_INVALID) return 0;
    g.surfaces[first].pitch = sf.color_pitch[sel];
    slots[0] = first;
    u32 n = 1;

    static const u32 mrt_from[3] = {
        CELL_GCM_SURFACE_TARGET_MRT1, CELL_GCM_SURFACE_TARGET_MRT2,
        CELL_GCM_SURFACE_TARGET_MRT3
    };
    for (u32 i = 0; i < 3; i++) {
        if (sf.color_target < mrt_from[i]) break;
        /* At A's size, which may be larger than the clip (see above): the
         * framebuffer takes A's extent, and every attachment must cover it. */
        const u32 slot = eng_surface_get(sf.color_location[i + 1],
                                         sf.color_offset[i + 1],
                                         g.surfaces[first].w, g.surfaces[first].h, fmt,
                                         sf.color_pitch[i + 1]);
        if (slot == ENG_INVALID) break;
        g.surfaces[slot].pitch = sf.color_pitch[i + 1];
        /* A set naming one buffer twice would attach the same target twice,
         * which no host API allows. */
        int seen = 0;
        for (u32 k = 0; k < n; k++) if (slots[k] == slot) seen = 1;
        if (seen) break;
        slots[n++] = slot;
    }
    return n;
}

/* ---- coherence with guest memory around 2D transfers ------------------- */

/* Bytes per pixel of a surface a 2D transfer can touch: A8R8G8B8, and the
 * FP16 RGBA target an HDR pass renders into (Twisted Metal copies its bloom
 * out of one for the SPU post-FX). 0 for any other format. */
static u32 eng_transfer_bpp(rsx_be_format fmt)
{
    switch (fmt) {
    case RSX_BE_FMT_R8G8B8A8:      return 4u;
    case RSX_BE_FMT_R16G16B16A16F: return 8u;
    default:                       return 0u;
    }
}

/* The surface holding guest bytes [offset, offset + pitch * h), with the byte
 * position of `offset` inside its row (*xb) and its row (*y). Only a surface
 * laid out with the transfer's own pitch qualifies: another pitch puts the
 * rows elsewhere. The CLOSEST base at or below the address wins: surfaces'
 * guest ranges overlap, and an FP16 target right after an RGBA8 one must not
 * resolve into the RGBA8 one's rows (rsx_live_draw.c's resolve takes the same
 * rule, and its comment records copying out of a stale surface at 0x0 before
 * it did). */
static u32 eng_surface_at(u32 location, u32 offset, u32 pitch, u32* xb, u32* y)
{
    u32 best = ENG_INVALID;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        const eng_surface* s = &g.surfaces[i];
        if (!s->handle || s->location != location || offset < s->offset) continue;
        const u32 bpp = eng_transfer_bpp(s->fmt);
        if (!bpp || s->pitch != pitch) continue;
        const u32 d = offset - s->offset;
        if (d / pitch >= s->h || d % pitch >= s->w * bpp) continue;
        if (best != ENG_INVALID && g.surfaces[best].offset > s->offset) continue;
        best = i;
        *xb = d % pitch;
        *y = d / pitch;
    }
    return best;
}

static u8* s_coherence_rows;
static u32 s_coherence_cap;

/* RSX_ENGINE_COHERENCE_LOG=<n>: log the first n transfers the engine syncs,
 * with how many non-zero pixels moved -- the question when a composite comes
 * out black is whether the GPU surface had anything in it. */
static int eng_coherence_log(void)
{
    static int left = -1;
    if (left < 0) { const char* e = getenv("RSX_ENGINE_COHERENCE_LOG"); left = e ? atoi(e) : 0; }
    return left > 0 ? left-- : 0;
}

/* Pixels of `bpp` bytes whose colour (the first three quarters: R,G,B) is
 * not all zero. */
static u32 eng_nonzero_pixels(const u8* rows, u32 n, u32 bpp)
{
    u32 k = 0;
    for (u32 i = 0; i < n; i++, rows += bpp) {
        u8 any = 0;
        for (u32 b = 0; b < bpp * 3u / 4u; b++) any |= rows[b];
        k += any != 0;
    }
    return k;
}

static u8* eng_coherence_buffer(u32 bytes)
{
    if (s_coherence_cap < bytes) {
        u8* n = (u8*)realloc(s_coherence_rows, bytes);
        if (!n) return NULL;
        s_coherence_rows = n;
        s_coherence_cap = bytes;
    }
    return s_coherence_rows;
}

/* One row of 32-bit pixels between the backend's R,G,B,A and the guest's
 * A8R8G8B8 bytes (or straight through with TEX_RGBA). Copied a byte at a
 * time, with the TEX_RGBA test inside the loop, these two conversions were
 * about a quarter of the FIFO thread's time while Twisted Metal booted; at
 * the -O2 the runtime builds with, GCC vectorises neither form. Each pixel is
 * one 32-bit rotate instead: bytes s0..s3 load as s0 | s1<<8 | s2<<16 | s3<<24
 * on a little-endian host (ps3emu/endian.h), so rotating left by 8 gives
 * s3,s0,s1,s2 (to the guest) and right by 8 gives s1,s2,s3,s0 (from it). */
static void eng_row_convert(u8* dst, const u8* src, u32 w, int rgba, int to_guest)
{
    if (rgba) { memcpy(dst, src, (size_t)w * 4u); return; }
    for (u32 c = 0; c < w; c++) {
        u32 v;
        memcpy(&v, src + (size_t)c * 4u, 4);
        v = to_guest ? (v << 8) | (v >> 24) : (v >> 8) | (v << 24);
        memcpy(dst + (size_t)c * 4u, &v, 4);
    }
}

void rsx_draw_engine_guest_read(u32 location, u32 offset, u32 pitch, u32 w, u32 h)
{
    if (!g.ready || !g.be->readback || !pitch || !w || !h) return;
    u32 xb = 0, y = 0;
    const u32 slot = eng_surface_at(location, offset, pitch, &xb, &y);
    if (slot == ENG_INVALID) {
        if (eng_coherence_log())
            fprintf(stderr, "[rsx engine] guest_read %u:0x%08X %ux%u: no surface\n",
                    location, offset, w, h);
        return;
    }
    const eng_surface* s = &g.surfaces[slot];
    const u32 bpp = eng_transfer_bpp(s->fmt);
    /* The transfer counts 32-bit pixels, so w * 4 bytes per row; an FP16
     * surface pixel is two of them. Read back every surface pixel those bytes
     * touch, then store exactly the bytes. */
    u32 bytes = w * 4u;
    if (xb + bytes > s->w * bpp) bytes = s->w * bpp - xb;
    if (y + h > s->h) h = s->h - y;
    const u32 x = xb / bpp, skip = xb % bpp;
    const u32 n = (skip + bytes + bpp - 1u) / bpp;
    u8* rows = eng_coherence_buffer(n * bpp * h);
    if (!rows) return;
    g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    g.be->readback(g.be->user, s->handle, x, y, n, h, rows, n * bpp);
    if (eng_coherence_log())
        fprintf(stderr, "[rsx engine] guest_read %u:0x%08X %ux%u <- surface 0x%08X (%u bpp) at %u,%u:"
                        " %u non-zero pixels\n", location, offset, w, h, s->offset, bpp, x, y,
                eng_nonzero_pixels(rows, n * h, bpp));
    const int rgba = rsx_texture_argb_is_rgba();
    for (u32 r = 0; r < h; r++) {
        u8* dst = (u8*)eng_guest_ptr(NULL, location, offset + r * pitch, bytes);
        if (!dst) return;
        u8* src = rows + (size_t)r * n * bpp;
        if (bpp == 8u) {
            /* R,G,B,A halves, each stored big-endian: the layout
             * rsx_texture_decode reads W16_Z16_Y16_X16_FLOAT from. */
            for (u32 c = 0; c < n * 4u; c++) {
                const u8 t = src[2 * c]; src[2 * c] = src[2 * c + 1]; src[2 * c + 1] = t;
            }
        } else if (!rgba) {
            eng_row_convert(src, src, n, 0, 1);   /* in place: one pixel per load/store */
        }
        memcpy(dst, src + skip, bytes);
    }
}

void rsx_draw_engine_guest_wrote(u32 location, u32 offset, u32 pitch, u32 w, u32 h)
{
    if (!g.ready || !g.be->color_target_write || !pitch || !w || !h) return;
    u32 x = 0, y = 0;
    const u32 slot = eng_surface_at(location, offset, pitch, &x, &y);
    /* color_target_write takes R,G,B,A bytes: an A8R8G8B8 surface only. */
    if (slot == ENG_INVALID || g.surfaces[slot].fmt != RSX_BE_FMT_R8G8B8A8) {
        if (eng_coherence_log())
            fprintf(stderr, "[rsx engine] guest_wrote %u:0x%08X %ux%u: no A8R8G8B8 surface\n",
                    location, offset, w, h);
        return;
    }
    x /= 4u;
    const eng_surface* s = &g.surfaces[slot];
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    u8* rows = eng_coherence_buffer(w * 4u * h);
    if (!rows) return;
    const int rgba = rsx_texture_argb_is_rgba();
    for (u32 r = 0; r < h; r++) {
        const u8* src = eng_guest_ptr(NULL, location, offset + r * pitch, w * 4u);
        if (!src) return;
        eng_row_convert(rows + (size_t)r * w * 4u, src, w, rgba, 0);
    }
    if (eng_coherence_log())
        fprintf(stderr, "[rsx engine] guest_wrote %u:0x%08X %ux%u -> surface 0x%08X at %u,%u:"
                        " %u non-zero pixels\n", location, offset, w, h, s->offset, x, y,
                eng_nonzero_pixels(rows, w * h, 4u));
    g.be->color_target_write(g.be->user, s->handle, x, y, w, h, rows, w * 4u);
}

static u32 eng_current_surface(void)
{
    u32 slots[RSX_BE_MAX_COLOR_TARGETS];
    return eng_current_target_set(slots) ? slots[0] : ENG_INVALID;
}

/* ---- per-zeta depth ------------------------------------------------------ */

/* One depth target per guest zeta address. Sharing a single resource across a
 * title's shadow, scene and post passes cross-contaminates later depth tests;
 * upstream's ef3271b records that as "the black player-character mass". */
static u32 eng_zdepth_get(u32 location, u32 offset, u32 rt_w, u32 rt_h)
{
    /* The attachment must cover the whole canvas: an early pass declaring a
     * smaller clip than the live viewport otherwise gets a target the host
     * API rejects when a later, larger pass binds it. */
    u32 want_w = rt_w > g.width ? rt_w : g.width;
    u32 want_h = rt_h > g.height ? rt_h : g.height;
    if (want_w > ENG_MAX_SURFACE_DIM || want_h > ENG_MAX_SURFACE_DIM)
        return ENG_INVALID;

    u32 slot = ENG_MAX_ZDEPTHS;
    for (u32 i = 0; i < g.n_zdepths; i++) {
        eng_zdepth* z = &g.zdepths[i];
        if (z->location == location && z->offset == offset) {
            if (z->w >= want_w && z->h >= want_h) return i;
            slot = i;
            if (want_w < z->w) want_w = z->w;
            if (want_h < z->h) want_h = z->h;
            break;
        }
    }
    if (slot == ENG_MAX_ZDEPTHS) {
        if (g.n_zdepths >= ENG_MAX_ZDEPTHS) return ENG_INVALID;
        slot = g.n_zdepths;
    }

    const u32 handle = g.be->depth_target_create(g.be->user, want_w, want_h);
    if (!handle)
        return (slot < g.n_zdepths && g.zdepths[slot].handle) ? slot : ENG_INVALID;

    eng_zdepth* z = &g.zdepths[slot];
    if (z->handle) g.be->depth_target_release(g.be->user, z->handle);
    z->location = location; z->offset = offset;
    z->w = want_w; z->h = want_h;
    z->handle = handle;
    z->snapshot = 0;
    z->cleared = 0;
    z->had_write = 0;
    z->snapshot_valid = 0;
    z->depth_serial = 0;
    z->guest_dirty = 0;
    if (slot == g.n_zdepths) g.n_zdepths++;
    return slot;
}

/* A depth target read back as a texture, but only once the pass has executed
 * a depth-WRITING draw: a write-enable bit alone does not prove the pass
 * produced a usable depth map, and a clear-only zeta falls through to guest
 * memory instead (rsx_live_draw.c:6016-6018, 6265-6269). */
static u32 eng_zdepth_snapshot(u32 slot)
{
    eng_zdepth* z = &g.zdepths[slot];
    if (!z->handle || !z->had_write) return 0;
    if (z->snapshot_valid && z->snapshot) return z->snapshot;
    if (!g.be->depth_snapshot) return 0;
    const u32 tex = g.be->depth_snapshot(g.be->user, z->handle, z->w, z->h);
    if (!tex) return 0;
    z->snapshot = tex;
    z->snapshot_valid = 1;
    return tex;
}

/* ---- textures ------------------------------------------------------------ */

static rsx_be_format eng_texfmt(rsx_texfmt f)
{
    switch (f) {
    case RSX_TEXFMT_R8:              return RSX_BE_FMT_R8;
    case RSX_TEXFMT_R8G8:            return RSX_BE_FMT_R8G8;
    case RSX_TEXFMT_R8G8B8A8:        return RSX_BE_FMT_R8G8B8A8;
    case RSX_TEXFMT_BC1:             return RSX_BE_FMT_BC1;
    case RSX_TEXFMT_BC2:             return RSX_BE_FMT_BC2;
    case RSX_TEXFMT_BC3:             return RSX_BE_FMT_BC3;
    case RSX_TEXFMT_R16:             return RSX_BE_FMT_R16;
    case RSX_TEXFMT_R16G16:          return RSX_BE_FMT_R16G16;
    case RSX_TEXFMT_R16G16F:         return RSX_BE_FMT_R16G16F;
    case RSX_TEXFMT_R16G16B16A16F:   return RSX_BE_FMT_R16G16B16A16F;
    case RSX_TEXFMT_R32F:            return RSX_BE_FMT_R32F;
    case RSX_TEXFMT_R32G32B32A32F:   return RSX_BE_FMT_R32G32B32A32F;
    default:                         return RSX_BE_FMT_R8;
    }
}

/* How many guest bytes a texture occupies: the whole mip chain, times six for
 * a cube map. That is what the content hash has to cover, or a title
 * animating one face or one lower level keeps its stale upload. */
static u32 eng_texture_span(u32 fmt, u32 w, u32 h, u32 levels, u32 pitch, int cube)
{
    if (cube) return rsx_texture_cube_face_stride(fmt, w, h, levels, pitch) * 6u;
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 n = rsx_texture_mip_chain(fmt, w, h, levels, pitch, lv);
    if (!n) return 0;
    return lv[n - 1].offset + lv[n - 1].tl.face_bytes;
}

/* A 3D texture: each level is `depth` slices (halving with the level), one
 * face_bytes each -- consecutive for a linear image, one Morton block for a
 * swizzled one. */
static u32 eng_texture_span_3d(u32 fmt, u32 w, u32 h, u32 depth, u32 levels, u32 pitch)
{
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 n = rsx_texture_mip_chain(fmt, w, h, levels, pitch, lv);
    if (!n) return 0;
    u32 total = 0, d = depth ? depth : 1u;
    for (u32 m = 0; m < n; m++) { total += lv[m].tl.face_bytes * d; if (d > 1) d >>= 1; }
    return total;
}

/* Slice z of a swizzled 3D level, de-interleaved into row-major rows of
 * w * bpp bytes, so the 2D decode can convert it as a linear image. */
static void eng_deswizzle_3d_slice(u8* dst, const u8* src, u32 w, u32 h, u32 d, u32 z, u32 bpp)
{
    u32 lw = 0, lh = 0, ld = 0;
    while ((1u << lw) < w) lw++;
    while ((1u << lh) < h) lh++;
    while ((1u << ld) < d) ld++;
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++)
            memcpy(dst + ((size_t)y * w + x) * bpp,
                   src + (size_t)rsx_swizzle_offset_3d(x, y, z, lw, lh, ld) * bpp, bpp);
}

/* One hash per cached texture per presented frame. Word-at-a-time FNV over
 * the whole span is deliberately cheap: this is a mutation detector, not a
 * content id (rsx_live_draw.c:2002-2026). */
static u64 eng_texture_content_hash(u32 location, u32 offset, u32 span,
                                    int* readable)
{
    const u8* src = span ? eng_guest_ptr(NULL, location, offset, span) : NULL;
    if (!src) { *readable = 0; return 0; }
    u64 hash = 1469598103934665603ull;
    u32 i = 0;
    for (; i + 8 <= span; i += 8) {
        u64 word;
        memcpy(&word, src + i, sizeof(word));
        hash ^= word;
        hash *= 1099511628211ull;
    }
    for (; i < span; i++) { hash ^= src[i]; hash *= 1099511628211ull; }
    *readable = 1;
    return hash;
}

/* The cheap per-frame change check: 64 bytes out of every 2 KB, plus the
 * last 64. A rewrite of the whole texture (a video frame, a UI redraw, a
 * streamed replacement) changes it at once; a small write between the
 * sampled blocks is left to the full hash, which every texture still gets
 * every ENG_TEXHASH_FULL_EVERY frames. */
#define ENG_TEXHASH_STRIDE      2048u
#define ENG_TEXHASH_FULL_EVERY  8u          /* a power of two */
#define ENG_TEXHASH_SMALL       16384u      /* at or below: full every frame */
static u64 eng_texture_sparse_hash(u32 location, u32 offset, u32 span,
                                   int* readable)
{
    const u8* src = span ? eng_guest_ptr(NULL, location, offset, span) : NULL;
    if (!src) { *readable = 0; return 0; }
    u64 hash = 1469598103934665603ull;
    for (u32 i = 0; i + 64 <= span; i += ENG_TEXHASH_STRIDE)
        for (u32 k = 0; k < 64; k += 8) {
            u64 word;
            memcpy(&word, src + i + k, sizeof(word));
            hash = (hash ^ word) * 1099511628211ull;
        }
    for (u32 i = span > 64 ? span - 64 : 0; i < span; i++)
        hash = (hash ^ src[i]) * 1099511628211ull;
    *readable = 1;
    return hash;
}

/* Whether a cached texture gets the full hash at this use: always when it
 * is small or under PS3RECOMP_ENG_TEXHASH_FULL=1 (the old behaviour, read
 * at engine init), otherwise once ENG_TEXHASH_FULL_EVERY frames have passed
 * since its last one. New entries start staggered by slot, so the full
 * hashes of a scene's textures spread over the frames instead of landing on
 * the same one. */
static int s_texhash_full_always;
static int eng_texture_full_rehash(u32 span, u32 last_full_frame)
{
    return s_texhash_full_always || span <= ENG_TEXHASH_SMALL ||
           (u32)(g.frames - last_full_frame) >= ENG_TEXHASH_FULL_EVERY;
}

static int eng_staging_reserve(u32 bytes)
{
    if (g.tex_staging_cap >= bytes) return 1;
    u8* n = (u8*)realloc(g.tex_staging, bytes);
    if (!n) return 0;
    g.tex_staging = n;
    g.tex_staging_cap = bytes;
    return 1;
}

/* Decode a guest texture out of guest memory and hand every face and level to
 * the backend. Returns the backend handle, or 0. */
static u32 eng_texture_upload(u32 location, u32 offset, u32 fmt, u32 w, u32 h,
                              u32 levels, u32 pitch, int cube, u32 remap, u32 depth)
{
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 nlv = rsx_texture_mip_chain(fmt, w, h, levels, pitch, lv);
    if (!nlv || !lv[0].tl.face_bytes) return 0;
    /* A volume: every slice of every level decoded and uploaded on its own.
     * inFAMOUS 2's occlusion volumes read an 8x8x8 cone LUT; uploaded as its
     * first slice and sampled in 2D it darkened whole boxes round every prop. */
    if (depth > 1 && !cube && g.be->texture_create_3d && g.be->texture_upload_3d &&
        !lv[0].tl.compressed && lv[0].tl.bytes_per_texel) {
        const u32 span = eng_texture_span_3d(fmt, w, h, depth, levels, pitch);
        const u8* src = eng_guest_ptr(NULL, location, offset, span);
        if (!src) return 0;
        const u32 handle = g.be->texture_create_3d(g.be->user, eng_texfmt(lv[0].tl.fmt),
                                                   w, h, depth, nlv, remap, fmt);
        if (!handle) return 0;
        const u8* level_src = src;
        u32 d = depth;
        for (u32 m = 0; m < nlv; m++) {
            const rsx_tex_layout* tl = &lv[m].tl;
            const u32 lin_bytes = tl->swizzled ? tl->row_bytes * tl->rows : 0u;
            if (!eng_staging_reserve(tl->dst_row_bytes * tl->rows + lin_bytes)) {
                g.be->texture_release(g.be->user, handle);
                return 0;
            }
            for (u32 z = 0; z < d; z++) {
                if (tl->swizzled) {
                    u8* lin = g.tex_staging + tl->dst_row_bytes * tl->rows;
                    eng_deswizzle_3d_slice(lin, level_src, lv[m].w, lv[m].h, d, z, tl->bytes_per_texel);
                    rsx_tex_layout ltl = *tl; ltl.swizzled = 0;
                    rsx_texture_decode(g.tex_staging, tl->dst_row_bytes, lin, lv[m].w, lv[m].h,
                                       &ltl, rsx_texture_argb_is_rgba());
                } else {
                    rsx_texture_decode(g.tex_staging, tl->dst_row_bytes,
                                       level_src + (size_t)z * tl->face_bytes, lv[m].w, lv[m].h,
                                       tl, rsx_texture_argb_is_rgba());
                }
                g.be->texture_upload_3d(g.be->user, handle, z, m, lv[m].w, lv[m].h,
                                        g.tex_staging, tl->dst_row_bytes);
            }
            level_src += (size_t)tl->face_bytes * d;
            if (d > 1) d >>= 1;
        }
        return handle;
    }
    const u32 faces = cube ? 6u : 1u;
    const u32 face_stride = cube
        ? rsx_texture_cube_face_stride(fmt, w, h, levels, pitch) : 0u;
    const u32 span = eng_texture_span(fmt, w, h, levels, pitch, cube);
    const u8* src = eng_guest_ptr(NULL, location, offset, span);
    if (!src) return 0;

    const u32 handle = g.be->texture_create(g.be->user, eng_texfmt(lv[0].tl.fmt),
                                            w, h, nlv, faces, remap, fmt);
    if (!handle) return 0;

    for (u32 f = 0; f < faces; f++) {
        const u8* face = src + (size_t)f * face_stride;
        for (u32 m = 0; m < nlv; m++) {
            const rsx_tex_layout* tl = &lv[m].tl;
            if (!eng_staging_reserve(tl->dst_row_bytes * tl->rows)) {
                g.be->texture_release(g.be->user, handle);
                return 0;
            }
            rsx_texture_decode(g.tex_staging, tl->dst_row_bytes,
                               face + lv[m].offset, lv[m].w, lv[m].h, tl,
                               rsx_texture_argb_is_rgba());
            g.be->texture_upload(g.be->user, handle, f, m, lv[m].w, lv[m].h,
                                 g.tex_staging, tl->dst_row_bytes, tl->rows);
        }
    }
    return handle;
}

/* The cache slot for a texture unit's bytes, keyed on where they are and what
 * the registers say they are. An already-cached entry is re-hashed at most
 * once per presented frame and re-decoded when the guest changed it, which is
 * what makes an animated UI or a video texture update at all. A miss with the
 * cache full evicts the least recently used entry rather than returning
 * nothing: returning white "made the recovered orphanage render as flat
 * green/black geometry" (rsx_live_draw.c:2326-2334). */
static u32 eng_texture_slot(u32 location, u32 offset, u32 fmt, u32 w, u32 h,
                            u32 levels, u32 pitch, int cube, u32 remap, u32 depth)
{
    if (!w || !h || w > ENG_MAX_TEX_DIM || h > ENG_MAX_TEX_DIM) return 0;
    if (!depth) depth = 1;
    const u32 span = depth > 1 ? eng_texture_span_3d(fmt, w, h, depth, levels, pitch)
                               : eng_texture_span(fmt, w, h, levels, pitch, cube);
    if (!span) return 0;

    for (u32 i = 0; i < g.n_textures; i++) {
        eng_texture* e = &g.textures[i];
        if (e->location != location || e->offset != offset ||
            e->format != fmt || e->width != w || e->height != h ||
            e->pitch != pitch || e->remap != remap ||
            e->cubemap != (u32)(cube != 0) || e->depth != depth)
            continue;
        if (e->handle && e->last_hash_frame != g.frames) {
            /* Every cached texture used to be hashed in full every frame;
             * in a big scene that is hundreds of MB a second for the few
             * that change. Now the sparse check every frame, and the full
             * hash when it differs, when this entry's turn has come, for a
             * small texture, or after a write the engine saw. */
            int readable = 0;
            u64 hash = e->content_hash;
            const u64 sparse = eng_texture_sparse_hash(location, offset, span,
                                                       &readable);
            if (readable && (sparse != e->sparse_hash ||
                             eng_texture_full_rehash(span, e->last_full_frame))) {
                hash = eng_texture_content_hash(location, offset, span, &readable);
                e->last_full_frame = g.frames;
            }
            if (readable) e->sparse_hash = sparse;
            e->last_hash_frame = g.frames;
            if (readable && hash != e->content_hash) {
                const u32 fresh = eng_texture_upload(location, offset, fmt, w, h,
                                                     levels, pitch, cube, remap, depth);
                if (fresh) {
                    g.be->texture_release(g.be->user, e->handle);
                    e->handle = fresh;
                    e->content_hash = hash;
                }
            }
        }
        e->last_use_serial = ++g.texture_use_serial;
        return e->handle;
    }

    u32 index;
    u32 evicted = 0;
    if (g.n_textures < ENG_MAX_TEXTURES) {
        index = g.n_textures++;
    } else {
        index = 0;
        for (u32 i = 1; i < g.n_textures; i++)
            if (g.textures[i].last_use_serial < g.textures[index].last_use_serial)
                index = i;
        evicted = g.textures[index].handle;
    }

    eng_texture e;
    memset(&e, 0, sizeof(e));
    e.location = location; e.offset = offset; e.format = fmt;
    e.width = w; e.height = h; e.pitch = pitch;
    e.remap = remap; e.cubemap = (u32)(cube != 0); e.depth = depth;
    e.last_hash_frame = g.frames;
    e.last_full_frame = g.frames - (index & (ENG_TEXHASH_FULL_EVERY - 1u));
    e.last_use_serial = ++g.texture_use_serial;
    { int readable = 0;
      e.content_hash = eng_texture_content_hash(location, offset, span, &readable);
      e.sparse_hash = eng_texture_sparse_hash(location, offset, span, &readable); }
    e.handle = eng_texture_upload(location, offset, fmt, w, h, levels, pitch,
                                  cube, remap, depth);
    if (e.handle && evicted) g.be->texture_release(g.be->user, evicted);
    if (e.handle || !evicted) g.textures[index] = e;
    return e.handle;
}

/* ---- surfaces read through guest memory --------------------------------- */

/* A texture may name a surface's bytes in a format the surface was not
 * rendered in: a deferred renderer reads its D24S8 depth buffer as A8R8G8B8
 * to rebuild positions, samples its Z16 shadow map as DEPTH16, and reads back
 * as X32_FLOAT a float it packed into an RGBA8 target. On RSX that is only
 * memory. Here the surface is stored into guest memory in its own layout --
 * the bytes RSX would have left there -- and the texture is then decoded from
 * guest memory like any other, so every format the decoder knows reads them
 * the way RSX would. A surface is stored at most once per write to it. */

/* Re-hash, on their next use rather than next frame, the cached textures that
 * start inside [offset, offset + bytes): their bytes changed within this one. */
static void eng_textures_touch(u32 location, u32 offset, u32 bytes)
{
    for (u32 i = 0; i < g.n_textures; i++) {
        eng_texture* e = &g.textures[i];
        if (e->location == location && e->offset >= offset && e->offset - offset < bytes) {
            e->last_hash_frame = g.frames - 1u;
            e->last_full_frame = g.frames - ENG_TEXHASH_FULL_EVERY;   /* hash it all */
        }
    }
}

/* Formats that read an RGBA8 surface's 32-bit pixels as something else: the
 * host image cannot be sampled as them, the guest bytes can. */
static int eng_format_reinterprets_rgba8(u32 base_fmt)
{
    switch (base_fmt) {
    case RSX_TEX_FMT_DEPTH24_D8:
    case 0x91u:     /* DEPTH24_D8_FLOAT */
    case 0x92u:     /* DEPTH16          */
    case 0x93u:     /* DEPTH16_FLOAT    */
    case 0x95u:     /* Y16_X16          */
    case 0x9Cu:     /* X32_FLOAT        */
    case 0x9Fu:     /* Y16_X16_FLOAT    */
        return 1;
    default:
        return 0;
    }
}

/* Store a colour surface into guest memory, A,R,G,B per pixel at its own
 * pitch. Returns 0 when it cannot, and the caller samples as before. */
static int eng_surface_to_guest(u32 slot)
{
    eng_surface* s = &g.surfaces[slot];
    if (!s->guest_dirty) return 1;
    if (s->fmt != RSX_BE_FMT_R8G8B8A8 || s->pitch < s->w * 4u || !g.be->readback)
        return 0;
    rsx_draw_engine_guest_read(s->location, s->offset, s->pitch, s->w, s->h);
    s->guest_dirty = 0;
    eng_textures_touch(s->location, s->offset, s->pitch * s->h);
    return 1;
}

static float* s_depth_rows;
static u32 s_depth_cap;

/* One row of depth values, clamped to [0, 1] (NaN to 0), quantized and stored
 * big-endian: Z16 as 16 bits, Z24S8 as 24 bits of depth and a zero stencil
 * byte. The format test is hoisted out of the pixel loops and the clamps are
 * selects, so both loops vectorize; a large depth target can be read back
 * several times a frame. */
static void eng_depth_row_to_guest(u8* dst, const float* src, u32 w, u32 bpp)
{
    if (bpp == 2u) {
        for (u32 c = 0; c < w; c++) {
            float d = src[c];
            d = d > 0.0f ? d : 0.0f;
            d = d < 1.0f ? d : 1.0f;
            const u32 v = (u32)(d * 65535.0f + 0.5f);
            dst[2u * c] = (u8)(v >> 8); dst[2u * c + 1u] = (u8)v;
        }
        return;
    }
    for (u32 c = 0; c < w; c++) {
        float d = src[c];
        d = d > 0.0f ? d : 0.0f;
        d = d < 1.0f ? d : 1.0f;
        /* Float32 rounding at this magnitude can turn depth 1 into
         * 0x1000000. Quantize in double before storing the 24-bit value. */
        const u32 v = (u32)((double)d * 16777215.0 + 0.5);
        dst[4u * c] = (u8)(v >> 16); dst[4u * c + 1u] = (u8)(v >> 8);
        dst[4u * c + 2u] = (u8)v; dst[4u * c + 3u] = 0;
    }
}

/* Store a depth target into guest memory as its zeta format lays it out:
 * Z16 as one big-endian 16-bit value, Z24S8 as a big-endian word with the
 * depth in the top 24 bits. The stencil byte is written as 0: the host depth
 * target keeps no stencil to copy. Returns 0 when it cannot. */
static int eng_zdepth_to_guest(u32 slot)
{
    eng_zdepth* z = &g.zdepths[slot];
    if (!z->guest_dirty) return 1;
    if (!g.be->depth_readback || !z->handle) return 0;
    const u32 bpp = (z->zfmt == 1u) ? 2u : 4u;      /* SURFACE_FORMAT Z16 = 1 */
    const u32 w = z->used_w < z->w ? z->used_w : z->w;
    const u32 h = z->used_h < z->h ? z->used_h : z->h;
    if (!w || !h || z->pitch < w * bpp) return 0;
    if (s_depth_cap < w * h) {
        float* n = (float*)realloc(s_depth_rows, (size_t)w * h * sizeof(float));
        if (!n) return 0;
        s_depth_rows = n;
        s_depth_cap = w * h;
    }
    g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    if (g.be->depth_readback(g.be->user, z->handle, 0, 0, w, h, s_depth_rows,
                             w * (u32)sizeof(float)) != 0)
        return 0;
    for (u32 r = 0; r < h; r++) {
        u8* dst = (u8*)eng_guest_ptr(NULL, z->location, z->offset + r * z->pitch, w * bpp);
        if (!dst) return 0;
        eng_depth_row_to_guest(dst, s_depth_rows + (size_t)r * w, w, bpp);
    }
    z->guest_dirty = 0;
    eng_textures_touch(z->location, z->offset, z->pitch * h);
    if (eng_coherence_log())
        fprintf(stderr, "[rsx engine] depth 0x%08X (%s, pitch %u) %ux%u stored to guest memory\n",
                z->offset, bpp == 2u ? "Z16" : "Z24S8", z->pitch, w, h);
    return 1;
}

/* A depth write or clear through the current zeta, recorded with what the
 * guest-memory copy needs to lay it out. */
static void eng_zdepth_wrote(u32 slot, const rsx_dsp_surface* sf)
{
    eng_zdepth* z = &g.zdepths[slot];
    z->depth_serial = ++g.write_serial;
    z->guest_dirty = 1;
    z->zfmt = sf->depth_format;
    z->pitch = sf->zeta_pitch;
    z->used_w = sf->clip_w ? sf->clip_w : g.width;
    z->used_h = sf->clip_h ? sf->clip_h : g.height;
}

static void eng_surfaces_wrote(const u32* targets, u32 n_targets)
{
    const u64 serial = ++g.write_serial;
    for (u32 i = 0; i < n_targets; i++) {
        g.surfaces[targets[i]].color_serial = serial;
        g.surfaces[targets[i]].guest_dirty = 1;
    }
}

/* SET_TEXTURE_FILTER's min field is [18:16] (1 NEAREST, 2 LINEAR, then 3..6,
 * the four combinations of a nearest/linear minification with a
 * nearest/linear mip filter) and mag [26:24]; SET_TEXTURE_CONTROL0 carries
 * max LOD at [18:7] and min LOD at [30:19], both 4.8 fixed point. RSX's LOD
 * bias, SET_TEXTURE_FILTER [12:0], is not applied: the reference engine
 * leaves it at zero too. */
static void eng_decode_sampler(u32 filter, u32 wrap, u32 control0,
                               rsx_be_sampler_desc* out)
{
    const u32 minf = (filter >> 16) & 7u;
    const u32 magf = (filter >> 24) & 7u;
    out->min_linear  = (u8)(minf == 2 || minf == 4 || minf == 6);
    out->mag_linear  = (u8)(magf == 2);
    out->mip_present = (u8)(minf >= 3);
    out->mip_linear  = (u8)(minf == 5 || minf == 6);
    out->wrap_s = (u8)(wrap & 0xFu);
    out->wrap_t = (u8)((wrap >> 8) & 0xFu);
    out->wrap_r = (u8)((wrap >> 16) & 0xFu);
    out->min_lod = (float)((control0 >> 19) & 0xFFFu) / 256.0f;
    out->max_lod = out->mip_present ? (float)((control0 >> 7) & 0xFFFu) / 256.0f
                                    : 0.0f;
    if (out->max_lod < out->min_lod) out->max_lod = out->min_lod;
}

/* ---- pipelines ----------------------------------------------------------- */

static u32 eng_vtex_mask(void)
{
    u32 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_VERTEX_TEXTURES; u++) {
        rsx_dsp_vertex_texture vt;
        rsx_dsp_get_vertex_texture(&g.rsx, u, &vt);
        if (vt.enabled && vt.width && vt.height) mask |= 1u << u;
    }
    return mask;
}

/* Units addressed in texels (RSX_TEX_FMT_UNNORM), with each one's size in
 * dim. No host sampling mode is unnormalised for these, so the fragment
 * program divides the coordinates instead, and the divisor makes the sizes
 * pipeline identity (rsx_live_draw.c keys them the same way). */
static u32 eng_unnorm_units(u32 dim[RSX_DSP_NUM_TEXTURES][2])
{
    u32 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++) {
        rsx_dsp_texture t;
        rsx_dsp_get_texture(&g.rsx, u, &t);
        dim[u][0] = dim[u][1] = 0;
        if (!t.enabled || !(t.format & RSX_TEX_FMT_UNNORM) || !t.width || !t.height)
            continue;
        mask |= 1u << u;
        dim[u][0] = t.width;
        dim[u][1] = t.height;
    }
    return mask;
}

/* Units bound to a 3D texture (SET_TEXTURE_FORMAT dimension 3 with a
 * depth), declared Texture3D and sampled with three coordinates. Only when
 * the backend can create one: otherwise the first slice stays a 2D texture
 * and the program samples it in 2D, as before. */
static u32 eng_volume_mask(void)
{
    if (!g.be->texture_create_3d || !g.be->texture_upload_3d) return 0;
    u32 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++) {
        rsx_dsp_texture t;
        rsx_dsp_get_texture(&g.rsx, u, &t);
        if (t.enabled && !t.cubemap && t.dimension == 3 && t.depth > 1 && t.width && t.height)
            mask |= 1u << u;
    }
    return mask;
}

static u32 eng_cube_mask(void)
{
    u32 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++) {
        rsx_dsp_texture t;
        rsx_dsp_get_texture(&g.rsx, u, &t);
        /* A cube face is square by construction, so a non-square image
         * cannot be one; the fragment program is compiled against this mask
         * and a texturecube slot filled with a 2D texture is a validation
         * failure, not a wrong pixel. */
        if (t.enabled && t.cubemap && t.width && t.width == t.height)
            mask |= 1u << u;
    }
    return mask;
}

/* Are both of the guest's own programs resident? One answer, used by the
 * layout and by the pipeline, because they must not disagree: the built-in
 * program declares all sixteen inputs, so narrowing the layout for it would
 * leave the pipeline's vertex descriptor short of what the shader reads. */
static int eng_guest_programs(const u8** out_vp, u32* out_vp_instrs,
                              const u8** out_fp, u32* out_fp_size)
{
    const u8* vp_uc = NULL;
    u32 vp_instrs = 0;
    const u32 start = rsx_dsp_vp_start(&g.rsx);
    if (start < RSX_DSP_VP_INSTR) {
        vp_uc = (const u8*)(g.rsx.vp + start * 4);
        vp_instrs = rsx_vp_program_size_instrs(
            vp_uc, (RSX_DSP_VP_INSTR - start) * 16u);
    }

    /* A zero SET_SHADER_PROGRAM is "none", not "the program at offset 0": the
     * register carries the location in its low two bits, so a real program can
     * never read back as zero. */
    const u8* fp_uc = NULL;
    u32 fp_size = 0;
    if (rsx_dsp_reg(&g.rsx, 0x08E4)) {
        u32 fp_loc = 0;
        const u32 fp_off = rsx_dsp_fragment_program(&g.rsx, &fp_loc);
        fp_uc = eng_guest_ptr(NULL, fp_loc, fp_off, 16);
        fp_size = fp_uc ? rsx_fp_program_size(fp_uc, ENG_FP_MAX_BYTES) : 0;
        if (fp_size) fp_uc = eng_guest_ptr(NULL, fp_loc, fp_off, fp_size);
        if (!fp_uc) fp_size = 0;
    }

    if (out_vp) *out_vp = vp_uc;
    if (out_vp_instrs) *out_vp_instrs = vp_instrs;
    if (out_fp) *out_fp = fp_uc;
    if (out_fp_size) *out_fp_size = fp_size;
    return vp_instrs && fp_size;
}

/* The layout the active vertex program actually reads. An uncertain analysis
 * falls back to all sixteen registers (rsx_live_draw.c:3625-3643). */
static void eng_vertex_layout(rsx_vertex_layout_plan* layout)
{
    u32 mask = 0xFFFFu;
    const u8* uc = NULL;
    u32 instrs = 0;
    if (eng_guest_programs(&uc, &instrs, NULL, NULL)) {
        rsx_vp_input_analysis analysis = { 0xFFFFu, 0 };
        if (rsx_vp_analyze_inputs(uc, instrs * 16u, &analysis) == (int)instrs &&
            analysis.exact && analysis.input_mask)
            mask = analysis.input_mask;
    }
    rsx_vertex_layout_plan_init(layout, mask);
}

/* What a draw runs before a title has loaded programs of its own.
 *
 * The reference engine has no fallback and drops such a draw, because a title
 * always has both programs resident by the time it draws anything. A general
 * toolkit does not get to assume that: the host harness's fixed-function modes
 * are exactly this case, and so is the first frame of a title that clears and
 * flips before its first SET_TRANSFORM_PROGRAM. It is written in the same HLSL
 * shape the decompilers emit -- ATTRn inputs, the COLOR0/TEXCOORD varyings, the
 * VPConst block and the viewport epilogue -- so it goes through the one
 * pipeline_create the backend already implements rather than needing a second
 * entry point for a built-in shader.
 *
 * Vertex constant rows 0..3 are the transform when the guest programmed them
 * and identity when it did not: an all-zero matrix would collapse every vertex
 * onto the origin. */
static const char* const kEngFixedVS =
"struct VSInput {\n"
"    float4 a0:ATTR0;  float4 a1:ATTR1;  float4 a2:ATTR2;  float4 a3:ATTR3;\n"
"    float4 a4:ATTR4;  float4 a5:ATTR5;  float4 a6:ATTR6;  float4 a7:ATTR7;\n"
"    float4 a8:ATTR8;  float4 a9:ATTR9;  float4 a10:ATTR10; float4 a11:ATTR11;\n"
"    float4 a12:ATTR12; float4 a13:ATTR13; float4 a14:ATTR14; float4 a15:ATTR15;\n"
"};\n"
"struct VSOutput {\n"
"    float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1;\n"
"    float4 fog:FOG;\n"
"    float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
"    float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7;\n"
"    float4 t8:TEXCOORD8; float4 t9:TEXCOORD9;\n"
"};\n"
"cbuffer VPConst : register(b0) {\n"
"    float4 vp_c[512];\n"
"    float4 vp_posscale;\n"
"    float4 vp_posoffset;\n"
"};\n"
"VSOutput main(VSInput input) {\n"
"    float4 _p = float4(dot(vp_c[0], input.a0), dot(vp_c[1], input.a0),\n"
"                       dot(vp_c[2], input.a0), dot(vp_c[3], input.a0));\n"
"    float _m = dot(abs(vp_c[0]), 1.0) + dot(abs(vp_c[1]), 1.0)\n"
"             + dot(abs(vp_c[2]), 1.0) + dot(abs(vp_c[3]), 1.0);\n"
"    if (_m == 0.0) _p = input.a0;\n"
"    VSOutput Out;\n"
"    Out.pos = float4(_p.xyz * vp_posscale.xyz + _p.w * vp_posoffset.xyz, _p.w);\n"
"    Out.col0 = input.a3; Out.col1 = float4(0,0,0,1); Out.fog = (float4)0;\n"
"    Out.t0 = input.a8; Out.t1 = (float4)0; Out.t2 = (float4)0; Out.t3 = (float4)0;\n"
"    Out.t4 = (float4)0; Out.t5 = (float4)0; Out.t6 = (float4)0; Out.t7 = (float4)0;\n"
"    Out.t8 = (float4)0; Out.t9 = (float4)0;\n"
"    return Out;\n"
"}\n";

static const char* const kEngFixedPS =
"struct PSInput {\n"
"    float4 position : SV_POSITION; float4 col0 : COLOR0; float4 col1 : COLOR1;\n"
"    float4 fog : FOG;\n"
"    float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3;\n"
"    float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;\n"
"};\n"
"float4 main(PSInput input) : SV_TARGET { return input.col0; }\n";

/* The texture units a translated pixel program samples. Every sampling call
 * the decompiler emits names its unit's sampler, rsx_samp[N], whatever the
 * texture's own spelling (rsx_tex[N] or, with a cube unit, rsx_texN); the
 * declaration's rsx_samp[16] is out of range and so not counted. */
static u32 eng_sampled_units(const char* ps)
{
    u32 mask = 0;
    for (const char* q = ps; (q = strstr(q, "rsx_samp[")) != NULL; q += 9) {
        const unsigned long u = strtoul(q + 9, NULL, 10);
        if (u < RSX_BE_MAX_TEXTURES) mask |= 1u << u;
    }
    return mask;
}

/* The pipeline for this draw, built once per distinct key. A negative result
 * is cached too (handle 0), so a program pair that will not translate is not
 * retried on every draw of every frame. `out_tex_used` gets the units the
 * pixel program samples: all of them for the built-in pair, which keeps
 * every enabled unit bound as before. */
/* Up to 16 comma-separated hex hashes from an environment variable. */
static u32 eng_parse_hash_list(const char* name, u64 out[16])
{
    const char* e = getenv(name);
    u32 n = 0;
    while (e && *e && n < 16) {
        char* end;
        const u64 v = strtoull(e, &end, 16);
        if (end == e) break;
        if (v) out[n++] = v;
        if (*end != ',') break;
        e = end + 1;
    }
    return n;
}

/* The per-draw fragment-program debug knobs; nonzero drops the draw.
 *
 * RSX_ENGINE_SKIP_FP=<hex>[,<hex>...] drops every draw through those
 * fragment programs: which pass paints a wrong smear, halo or black band is
 * found by removing passes one at a time.
 *
 * RSX_ENGINE_REGDUMP_FP=<hex>[,<hex>...] prints the non-zero words of the
 * whole RSX register file at the first draw through each of those programs,
 * so the state of a broken pass can be diffed register by register against a
 * working one -- which state it sets that the engine does not honour. */
static int eng_debug_fp(u64 fp_hash, u32 pipeline)
{
    static int init;
    static u64 skip[16], dump[16];
    static u32 n_skip, n_dump, dumped;
    if (!init) {
        init = 1;
        n_skip = eng_parse_hash_list("RSX_ENGINE_SKIP_FP", skip);
        n_dump = eng_parse_hash_list("RSX_ENGINE_REGDUMP_FP", dump);
    }
    for (u32 i = 0; i < n_dump; i++) {
        if (dump[i] != fp_hash || (dumped & (1u << i))) continue;
        dumped |= 1u << i;
        fprintf(stderr, "[rsx regdump] fp %016llx pipe %u frame %u begin\n",
                (unsigned long long)fp_hash, pipeline, g.frames);
        for (u32 r = 0; r < RSX_DSP_NUM_REGS; r++)
            if (g.rsx.regs[r])
                fprintf(stderr, "[rsx regdump] 0x%04X = 0x%08X\n", r * 4u, g.rsx.regs[r]);
        fprintf(stderr, "[rsx regdump] fp %016llx end\n", (unsigned long long)fp_hash);
    }
    for (u32 i = 0; i < n_skip; i++)
        if (skip[i] == fp_hash) return 1;
    return 0;
}

/* A guest fragment program's name for the debug knobs: its structural hash
 * (inline constants excluded) seeded with the FNV offset basis, the value
 * RSX_FP_DEBUG matches in the decompiler. Unlike a backend pipeline handle it
 * does not depend on the order programs are first drawn in, so a program
 * found in one run is named the same in the next. 0 for the built-in one. */
static u64 eng_fp_debug_hash(const u8* fp_uc, u32 fp_size)
{
    return fp_uc && fp_size
        ? rsx_fp_structural_hash(fp_uc, fp_size, 1469598103934665603ull) : 0;
}

static u32 eng_pipeline_get(const rsx_vertex_layout_plan* layout,
                            const rsx_be_render_state* rs,
                            rsx_be_format rt_fmt, u32 rt_count, int* out_fixed,
                            u32* out_tex_used, u64* out_fp_hash)
{
    *out_fixed = 1;
    *out_tex_used = 0;
    *out_fp_hash = 0;
    const u8* vp_uc = NULL;
    const u8* fp_uc = NULL;
    u32 vp_instrs = 0, fp_size = 0;
    /* No resident program pair means the built-in one; see kEngFixedVS. */
    const int fixed = !eng_guest_programs(&vp_uc, &vp_instrs, &fp_uc, &fp_size);

    const u32 fp_ctrl  = rsx_dsp_shader_control(&g.rsx);
    const u32 srgb_output = !fixed && rsx_fp_srgb_output_enabled(
        rsx_dsp_reg(&g.rsx, 0x1FECu), fp_ctrl,
        rsx_dsp_reg(&g.rsx, 0x0208u) & 0x1Fu);
    const u32 cube_mask = eng_cube_mask();
    const u32 vol_mask = eng_volume_mask();
    const u32 vtex_mask = eng_vtex_mask();
    rsx_fp_shadow_desc shadow[RSX_DSP_NUM_TEXTURES] = {{0}};
    u32 shadow_mask = 0;
    const u32 sampled_units = fixed ? 0 : rsx_fp_texture_units(fp_uc, fp_size);
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++) {
        if (!(sampled_units & (1u << u))) continue;
        rsx_dsp_texture t;
        rsx_dsp_get_texture(&g.rsx, u, &t);
        rsx_fp_shadow_desc_from_texture(&shadow[u], t.format, t.dimension,
            t.cubemap, t.mipmaps, t.filter, t.wrap, t.control0);
        if (shadow[u].enabled) shadow_mask |= 1u << u;
    }
    u8 gamma_masks[RSX_DSP_NUM_TEXTURES] = {0};
    if (!fixed) for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; ++u) {
        rsx_dsp_texture t; rsx_dsp_get_texture(&g.rsx, u, &t);
        if (t.enabled) gamma_masks[u] = rsx_texture_gamma_mask(t.format, t.remap, t.filter, t.wrap);
    }
    const u32 gamma_units = fixed ? 0 : rsx_fp_texture_gamma_units(fp_uc, fp_size, gamma_masks);
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; ++u)
        if (!(gamma_units & (1u << u))) gamma_masks[u] = 0;
    /* Texture sign expansion: SET_TEXTURE_ADDRESS's UNSIGNED_REMAP_BIASED
     * asks that every fetch come back re-centred to -1..1. Same rule as
     * gamma: only units the program samples count. */
    u8 expand_masks[RSX_DSP_NUM_TEXTURES] = {0};
    if (!fixed) for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; ++u) {
        rsx_dsp_texture t; rsx_dsp_get_texture(&g.rsx, u, &t);
        if (t.enabled) expand_masks[u] = rsx_texture_expand_mask(t.format, t.remap, t.filter, t.wrap);
    }
    const u32 expand_units = fixed ? 0 : rsx_fp_texture_expand_units(fp_uc, fp_size, expand_masks);
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; ++u)
        if (!(expand_units & (1u << u))) expand_masks[u] = 0;

    u32 unnorm_dim[RSX_DSP_NUM_TEXTURES][2];
    /* Shadow helpers normalize against the bound resource themselves. Only
     * sampled ordinary UN units need baked dimensions or cache identity. */
    const u32 unnorm_mask = eng_unnorm_units(unnorm_dim) &
        (fixed ? 0xFFFFu : sampled_units) & ~shadow_mask;
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++)
        if (!(unnorm_mask & (1u << u)))
            unnorm_dim[u][0] = unnorm_dim[u][1] = 0;

    memset(&g.fp_constants, 0, sizeof g.fp_constants);
    if (!fixed && rsx_fp_collect_constants(fp_uc, fp_size, &g.fp_constants) < 0)
        return 0;

    /* Identity: the vertex program's own bytes, the fragment program's
     * STRUCTURE (its inline constants are hoisted into the buffered block, so
     * a constant change must not be a new pipeline), the export-width bit of
     * SHADER_CONTROL, the cube and vertex-texture masks, the input layout,
     * and the structural render state. */
    u64 key = 1469598103934665603ull;
    if (fixed) {
        static const u32 fixed_tag = 0x4E464958u;   /* "XIFN" */
        key = eng_fnv1a(&fixed_tag, sizeof fixed_tag, key);
    } else {
        key = eng_fnv1a(vp_uc, vp_instrs * 16u, key);
        key = rsx_fp_structural_hash(fp_uc, fp_size, key);
        if (!key) return 0;
        const u32 fp_ctrl_key = fp_ctrl & 0x4Eu; /* colour width and fragment depth export */
        key = eng_fnv1a(&fp_ctrl_key, sizeof fp_ctrl_key, key);
        if (srgb_output) key = eng_fnv1a(&srgb_output, sizeof srgb_output, key);
        if (gamma_units) key = eng_fnv1a(gamma_masks, sizeof gamma_masks, key);
        if (expand_units) key = eng_fnv1a(expand_masks, sizeof expand_masks, key);
        key = eng_fnv1a(&cube_mask, sizeof cube_mask, key);
        if (vol_mask) key = eng_fnv1a(&vol_mask, sizeof vol_mask, key);
        if (shadow_mask) {
            key = eng_fnv1a(&shadow_mask, sizeof shadow_mask, key);
            key = eng_fnv1a(shadow, sizeof shadow, key);
        }
        key = eng_fnv1a(&vtex_mask, sizeof vtex_mask, key);
        key = eng_fnv1a(&unnorm_mask, sizeof unnorm_mask, key);
        if (unnorm_mask) key = eng_fnv1a(unnorm_dim, sizeof unnorm_dim, key);
    }
    key = eng_fnv1a(&layout->mask, sizeof layout->mask, key);
    key = eng_fnv1a(&layout->stride, sizeof layout->stride, key);
    key = eng_fnv1a(&rt_fmt, sizeof rt_fmt, key);
    /* How many colour attachments the pass will have is pipeline identity:
     * a host API matches the two against each other. */
    key = eng_fnv1a(&rt_count, sizeof rt_count, key);
    key = rsx_draw_engine_hash_render_state(rs, key);

    for (u32 i = 0; i < g.n_pipelines; i++)
        if (g.pipelines[i].key == key) {
            *out_fixed = g.pipelines[i].fixed;
            *out_tex_used = g.pipelines[i].tex_used;
            *out_fp_hash = g.pipelines[i].fp_hash;
            return g.pipelines[i].handle;
        }
    if (g.n_pipelines >= ENG_MAX_PIPELINES) return 0;

    u32 handle = 0;
    int vi = 1, fi = 1;
    u32 nconst = 0;
    if (fixed) {
        snprintf(s_vs_hlsl, sizeof s_vs_hlsl, "%s", kEngFixedVS);
        snprintf(s_ps_hlsl, sizeof s_ps_hlsl, "%s", kEngFixedPS);
    } else {
        vi = rsx_vp_decompile_compact_ex(vp_uc, vp_instrs * 16u, vtex_mask,
                                         layout->mask, s_vs_hlsl,
                                         sizeof s_vs_hlsl);
        fi = rsx_fp_decompile_buffered_shadow_color_expand_vol_ex(fp_uc, fp_size, fp_ctrl, cube_mask, vol_mask,
                shadow_mask ? shadow : NULL, gamma_masks, expand_units ? expand_masks : NULL,
                s_ps_hlsl, sizeof s_ps_hlsl, &nconst);
        if (fi > 0 && nconst != g.fp_constants.count) fi = -1;
        if (fi > 0 && rsx_fp_apply_unnorm_scale(s_ps_hlsl, sizeof s_ps_hlsl, unnorm_mask,
                                                (const u32 (*)[2])unnorm_dim, cube_mask) < 0)
            fi = -1;
        if (fi > 0 && srgb_output &&
            rsx_fp_apply_srgb_output(s_ps_hlsl, sizeof s_ps_hlsl) < 0)
            fi = -1;
        if (fi > 0 && rs->alpha_test_enable &&
            rsx_fp_apply_alpha_test_buffered(s_ps_hlsl, sizeof s_ps_hlsl,
                                             rs->alpha_func) < 0)
            fi = -1;
    }
    if (vi > 0 && fi > 0)
        handle = g.be->pipeline_create(g.be->user, s_vs_hlsl, s_ps_hlsl, rs,
                                       layout, layout->stride, rt_fmt, rt_count);
    const u64 fp_hash = fixed ? 0 : eng_fp_debug_hash(fp_uc, fp_size);
    /* RSX_ENGINE_SHADER_DUMP=<existing dir>: each new pipeline's two programs
     * as <dir>/pipe<handle>.vs.hlsl and .ps.hlsl, the handle being the one
     * RSX_ENGINE_PASS_DUMP's index names, and a guest fragment program's own
     * ucode as .fp.bin with its address and debug hash (the value
     * RSX_FP_DEBUG, RSX_ENGINE_SKIP_FP and RSX_ENGINE_REGDUMP_FP take) in
     * .fp.txt. A failed pipeline is pipe0. */
    {
        const char* dir = getenv("RSX_ENGINE_SHADER_DUMP");
        if (dir && *dir) {
            char path[1024];
            FILE* f;
            snprintf(path, sizeof path, "%s/pipe%u.vs.hlsl", dir, handle);
            if ((f = fopen(path, "w"))) { fputs(s_vs_hlsl, f); fclose(f); }
            snprintf(path, sizeof path, "%s/pipe%u.ps.hlsl", dir, handle);
            if ((f = fopen(path, "w"))) { fputs(s_ps_hlsl, f); fclose(f); }
            if (!fixed && fp_uc && fp_size) {
                u32 fp_loc = 0;
                const u32 fp_off = rsx_dsp_fragment_program(&g.rsx, &fp_loc);
                snprintf(path, sizeof path, "%s/pipe%u.fp.bin", dir, handle);
                if ((f = fopen(path, "wb"))) { fwrite(fp_uc, 1, fp_size, f); fclose(f); }
                snprintf(path, sizeof path, "%s/pipe%u.fp.txt", dir, handle);
                if ((f = fopen(path, "w"))) {
                    fprintf(f, "location %u offset 0x%08X size %u ctrl 0x%08X\n"
                               "fp %016llx\n",
                            fp_loc, fp_off, fp_size, fp_ctrl,
                            (unsigned long long)fp_hash);
                    fclose(f);
                }
            }
        }
    }
    { static u32 logs = 0; if (PS3_LOG_FIRST(logs, 32))
        fprintf(stderr, "[rsx engine] pipeline %016llx: %s vp %d, fp %d,"
                        " %u constants, fp %016llx -> %s\n",
                (unsigned long long)key, fixed ? "built-in" : "guest",
                vi, fi, nconst, (unsigned long long)fp_hash,
                handle ? "ok" : "FAILED (draw dropped)"); }

    g.pipelines[g.n_pipelines].key = key;
    g.pipelines[g.n_pipelines].fp_hash = fp_hash;
    g.pipelines[g.n_pipelines].handle = handle;
    g.pipelines[g.n_pipelines].fixed = (u8)fixed;
    g.pipelines[g.n_pipelines].tex_used =
        (u16)(fixed ? 0xFFFFu : eng_sampled_units(s_ps_hlsl));
    *out_tex_used = g.pipelines[g.n_pipelines].tex_used;
    *out_fp_hash = fp_hash;
    g.n_pipelines++;
    *out_fixed = fixed;
    return handle;
}

/* ---- draw accumulation --------------------------------------------------- */

static void dc_reset(void)
{
    dc.n_arr = dc.n_idx = dc.n_packets = 0;
    dc.n_refs = dc.n_source_refs = dc.n_verts = dc.n_cuts = 0;
    dc.refs_remapped = 0;
    dc.fetch_ok = 1;
    dc.inl = NULL;
    dc.inl_bytes = 0;
    dc.n_immediate = 0;
}

static int dc_push_ref(u32 vertex_id, u32 base_index)
{
    if (dc.n_refs >= dc.cap_refs) {
        const u32 next = dc.cap_refs ? dc.cap_refs * 2u : 4096u;
        rsx_vertex_ref* r = (rsx_vertex_ref*)realloc(dc.refs,
                                                     (size_t)next * sizeof(*r));
        if (!r) return 0;
        dc.refs = r;
        dc.cap_refs = next;
    }
    dc.refs[dc.n_refs].vertex_id = vertex_id;
    dc.refs[dc.n_refs].base_index = base_index;
    dc.n_refs++;
    return 1;
}

static int dc_reserve_verts(u64 bytes)
{
    if (bytes <= dc.verts_cap) return 1;
    u64 next = dc.verts_cap ? dc.verts_cap : (1u << 20);
    while (next < bytes) next *= 2u;
    u8* v = (u8*)realloc(dc.verts, (size_t)next);
    if (!v) return 0;
    dc.verts = v;
    dc.verts_cap = next;
    return 1;
}

static int dc_reserve_indices(u32 count)
{
    if (count <= g.index_cap) return 1;
    u32 next = g.index_cap ? g.index_cap : 4096u;
    while (next < count) next *= 2u;
    u32* p = (u32*)realloc(g.indices, (size_t)next * sizeof(u32));
    if (!p) return 0;
    g.indices = p;
    g.index_cap = next;
    return 1;
}

/* Turn this group's batches into a reference list, recording a CUT rather
 * than fetching a phantom vertex wherever the restart sentinel appears, then
 * collapse repeated references and fetch each unique vertex once.
 * rsx_live_draw.c's fetch_batches_hoisted (4165-4292). */
static void dc_fetch(const rsx_vertex_layout_plan* layout, int allow_remap)
{
    if (dc.n_immediate) {
        dc.n_source_refs = dc.n_immediate;
        if (!dc_reserve_verts((u64)dc.n_immediate * layout->stride)) {
            dc.fetch_ok = 0;
            return;
        }
        for (u32 v = 0; v < dc.n_immediate; v++)
            for (u32 slot = 0; slot < layout->count; slot++)
                memcpy(dc.verts + (u64)v * layout->stride + slot * 16u,
                       dc.immediate[v][layout->attrs[slot]], 16u);
        dc.n_verts = dc.n_immediate;
        return;
    }
    for (u32 b = 0; b < dc.n_arr && dc.fetch_ok; b++)
        for (u32 i = 0; i < dc.arr[b].count && dc.fetch_ok; i++)
            if (!dc_push_ref(dc.arr[b].first + i, 0)) dc.fetch_ok = 0;

    if (dc.n_idx && dc.fetch_ok) {
        const u32 base_index = rsx_dsp_vertex_data_base_index(&g.rsx);
        rsx_dsp_index_array ia;
        rsx_dsp_get_index_array(&g.rsx, &ia);
        const u32 esz = ia.is_u32 ? 4u : 2u;
        const int restart_en = rsx_dsp_restart_index_enabled(&g.rsx, ia.is_u32);
        const u32 restart_val = rsx_dsp_restart_index(&g.rsx);
        /* The index array's location field is a DMA selector, not the
         * rsx_dsp location enum: 0 selects local memory. */
        const u32 ia_loc = ia.location ? RSX_LOCATION_MAIN : RSX_LOCATION_LOCAL;

        for (u32 b = 0; b < dc.n_idx && dc.fetch_ok; b++) {
            const u32 first = dc.idx[b].first, count = dc.idx[b].count;
            const u64 start = (u64)ia.offset + (u64)first * esz;
            const u64 bytes = (u64)count * esz;
            const u8* run = NULL;
            if (count && start <= 0xFFFFFFFFull && bytes <= 0xFFFFFFFFull &&
                start + bytes <= 0x100000000ull)
                run = eng_guest_ptr(NULL, ia_loc, (u32)start, (u32)bytes);
            for (u32 i = 0; i < count && dc.fetch_ok; i++) {
                const u8* p = run ? run + (size_t)i * esz
                    : eng_guest_ptr(NULL, ia_loc, ia.offset + (first + i) * esz, esz);
                if (!p) { dc.fetch_ok = 0; break; }
                const u32 index = ia.is_u32
                    ? (((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3])
                    : (u32)((p[0] << 8) | p[1]);
                if (restart_en && index == restart_val) {
                    if (!rsx_restart_cut_push(&dc.cuts, &dc.n_cuts, &dc.cap_cuts,
                                              dc.n_refs))
                        dc.fetch_ok = 0;
                    continue;
                }
                if (!dc_push_ref(index, base_index)) dc.fetch_ok = 0;
            }
        }
    }
    if (!dc.fetch_ok) return;

    dc.n_source_refs = dc.n_refs;
    /* Only a topology this engine rebuilds through an index buffer may share
     * repeated references. A line strip or a point list is drawn in the order
     * it arrived, so collapsing duplicates would reorder it. */
    if (allow_remap && dc.n_refs > 1) {
        u32 unique = dc.n_refs;
        if (rsx_vertex_remap_build(&dc.ref_remap, dc.refs, dc.n_refs, &unique)) {
            dc.refs_remapped = unique < dc.n_refs;
            dc.n_refs = unique;
        }
    }

    dc.layout = *layout;
    rsx_vertex_fetch_plan_init(&dc.fetch_plan, &g.rsx, layout, eng_guest_ptr, NULL);
    rsx_vertex_fetch_plan_set_inline(&dc.fetch_plan, &g.rsx, dc.inl, dc.inl_bytes);
    rsx_vertex_fetch_plan_prepare(&dc.fetch_plan, dc.refs, dc.n_refs);
    if (!dc_reserve_verts((u64)dc.n_refs * layout->stride)) {
        dc.fetch_ok = 0;
        return;
    }
    for (u32 i = 0; i < dc.n_refs; i++) {
        u8* dst = layout->stride ? dc.verts + (u64)i * layout->stride : NULL;
        if (!rsx_vertex_fetch_one(&dc.fetch_plan, &dc.refs[i], dst)) {
            dc.fetch_ok = 0;
            return;
        }
    }
    dc.n_verts = dc.n_refs;
}

/* ---- sink ---------------------------------------------------------------- */

static void sink_begin(void* user, const rsx_dispatch* r, u32 prim)
{
    (void)user; (void)prim;
    dc_reset();
    dc.inline_dropped_at_begin = r->inline_dropped;
}

/* A malformed/unbounded group must not render a truncated primitive. */
#define ENG_MAX_IMMEDIATE_VERTICES 4096u
static void sink_immediate_vertex(void* user, const rsx_dispatch* r)
{
    (void)user;
    dc.n_packets++;
    if (!dc.fetch_ok) return;
    if (dc.n_immediate == ENG_MAX_IMMEDIATE_VERTICES) {
        dc.fetch_ok = 0;
        return;
    }
    if (dc.n_immediate == dc.cap_immediate) {
        const u32 next = dc.cap_immediate ? dc.cap_immediate * 2u : 64u;
        void* p = realloc(dc.immediate, (size_t)next * sizeof(*dc.immediate));
        if (!p) { dc.fetch_ok = 0; return; }
        dc.immediate = p;
        dc.cap_immediate = next;
    }
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        float* value = dc.immediate[dc.n_immediate][attr];
        rsx_dsp_vertex_default(r, attr, value);
        /* The legacy default getter treats all-zero words as unwritten.
         * Direct and folded constants may explicitly set W/alpha to zero. */
        const u32 base = (0x1C00u + attr * 16u) >> 2;
        if (r->seen[base] || r->seen[base + 1] ||
            r->seen[base + 2] || r->seen[base + 3])
            memcpy(value, &r->regs[base], 16u);
    }
    dc.n_immediate++;
}

static void sink_draw_arrays(void* user, const rsx_dispatch* r, u32 first, u32 count)
{
    (void)user; (void)r;
    dc.n_packets++;
    if (dc.n_arr >= ENG_MAX_BATCHES) return;
    dc.arr[dc.n_arr].first = first;
    dc.arr[dc.n_arr].count = count;
    dc.n_arr++;
}

/* INLINE_ARRAY delivers one finished stream rather than a (first, count)
 * range; queue it as an ordinary consecutive batch and let the fetch plan
 * read the vertices out of it (rsx_vertex_fetch_plan_set_inline). */
static void sink_inline_array(void* user, const rsx_dispatch* r,
                              const u8* data, u32 bytes)
{
    (void)user;
    /* Record source presence even when its declaration or length is invalid,
     * so mixed immediate/inline groups cannot draw a partial prefix. */
    dc.inl = data;
    dc.inl_bytes = bytes;
    const u32 stride = rsx_vertex_inline_layout(r, NULL);
    if (!stride || bytes < stride) return;
    dc.n_packets++;
    if (dc.n_arr >= ENG_MAX_BATCHES) return;
    dc.arr[dc.n_arr].first = 0;
    dc.arr[dc.n_arr].count = bytes / stride;
    dc.n_arr++;
}

static void sink_draw_index(void* user, const rsx_dispatch* r, u32 first, u32 count)
{
    (void)user; (void)r;
    dc.n_packets++;
    if (dc.n_idx >= ENG_MAX_BATCHES) return;
    dc.idx[dc.n_idx].first = first;
    dc.idx[dc.n_idx].count = count;
    dc.n_idx++;
}

/* Which texture units this draw binds, and from where: a unit naming a
 * registered colour surface samples that live target rather than guest
 * memory (every post-process, shadow and reflection in a PS3 title is a
 * render-to-texture read back through a texture unit), a unit naming a
 * tracked zeta in DEPTH24_D8 samples its snapshot, and everything else is a
 * guest upload (rsx_live_draw.c:5986-6062). */
/* A direct copy is valid only for the entire matching physical sample
 * layout. Reinterpreted, swizzled, mipmapped and subrectangle aliases need a
 * different conversion and must not silently sample stale guest bytes. */
static int eng_color_feedback_compatible(const eng_surface* s,
                                          const rsx_dsp_texture* t,
                                          const rsx_dsp_surface* sf)
{
    const u32 base = t->format & 0x9fu;
    u32 bpp = 0, color = 0;
    switch (s->fmt) {
    case RSX_BE_FMT_R8G8B8A8:        if (base == 0x85u) { bpp = 4; color = 8; } break;
    case RSX_BE_FMT_R16G16B16A16F:   if (base == 0x9au) { bpp = 8; color = 11; } break;
    case RSX_BE_FMT_R32G32B32A32F:   if (base == 0x9bu) { bpp = 16; color = 12; } break;
    case RSX_BE_FMT_R32F:            if (base == 0x9cu) { bpp = 4; color = 13; } break;
    default: break;
    }
    if (!bpp || sf->color_format != color || sf->raster_type != 1 ||
        !(t->format & 0x20u) || t->dimension != 2 ||
        t->cubemap || t->mipmaps > 1 || t->depth > 1 ||
        t->width != s->w || t->height != s->h) return 0;
    const u64 row = (u64)s->w * bpp;
    const u64 sp = s->pitch ? s->pitch : row;
    const u64 tp = t->pitch ? t->pitch : row;
    return row && sp >= row && tp == sp;
}

static u32 eng_color_feedback_failure(u32 unit, const eng_surface* s,
                                       const rsx_dsp_texture* t)
{
    static u32 logs;
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);
    if (PS3_LOG_FIRST(logs, 8))
        fprintf(stderr, "[rsx engine] rejected colour feedback unit %u: "
                "target %ux%u fmt%u pitch%u, texture %ux%u fmt%02X pitch%u "
                "dim%u levels%u cube%u color%u raster%u\n", unit, s->w, s->h, (u32)s->fmt,
                s->pitch, t->width, t->height, t->format, t->pitch,
                t->dimension, t->mipmaps, t->cubemap, sf.color_format, sf.raster_type);
    return ENG_INVALID;
}

static u32 sink_bind_textures(const u32* target_slots, u32 n_targets,
                              u32 current_zslot, u32 tex_used,
                              u32 textures[RSX_BE_MAX_TEXTURES],
                              rsx_be_sampler_desc samplers[RSX_BE_MAX_TEXTURES])
{
    u32 mask = 0;
    u32 feedback[RSX_BE_MAX_COLOR_TARGETS] = {0};
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
        rsx_dsp_texture t;
        rsx_dsp_get_texture(&g.rsx, u, &t);
        /* A unit left enabled by an earlier draw but not sampled by this
         * program is skipped: resolving it would cost a cache check, and an
         * aliased surface a store to guest memory, for nothing. */
        if (!t.enabled || !((tex_used >> u) & 1u)) continue;
        mask |= 1u << u;
        eng_decode_sampler(t.filter, t.wrap, t.control0, &samplers[u]);

        int sampled = -1, own = 0, surface_at = -1;
        for (u32 i = 0; i < g.n_surfaces; i++) {
            if (!g.surfaces[i].handle || g.surfaces[i].location != t.location ||
                g.surfaces[i].offset != t.offset)
                continue;
            surface_at = (int)i;
            /* A current attachment requires an ordered immutable copy; it
             * cannot be sampled directly while the host pass writes it. */
            for (u32 k = 0; k < n_targets; k++) if (target_slots[k] == i) own = 1;
            if (own) break;
            sampled = (int)i;
            break;
        }
        const u32 base_fmt = t.format & RSX_TEX_FMT_BASE_MASK & ~(u32)RSX_TEX_FMT_UNNORM;
        /* The depth at this address, when it was written after any colour
         * there: a depth-only pass names its zeta as a colour target too. */
        int zsrc = -1;
        for (u32 i = 0; i < g.n_zdepths; i++) {
            const eng_zdepth* z = &g.zdepths[i];
            if (z->location != t.location || z->offset != t.offset || !z->depth_serial)
                continue;
            if (surface_at < 0 || z->depth_serial > g.surfaces[surface_at].color_serial)
                zsrc = (int)i;
            break;
        }
        if (zsrc >= 0) {
            /* DEPTH24_D8 keeps the backend's own snapshot when it has one;
             * anything else goes through guest memory. */
            if (base_fmt == RSX_TEX_FMT_DEPTH24_D8 && current_zslot != (u32)zsrc) {
                const u32 snap = eng_zdepth_snapshot((u32)zsrc);
                if (snap) { textures[u] = snap; continue; }
            }
            if (eng_zdepth_to_guest((u32)zsrc)) {
                textures[u] = eng_texture_slot(t.location, t.offset, t.format,
                                               t.width, t.height, t.mipmaps, t.pitch,
                                               (int)t.cubemap, t.remap & 0xFFFFu,
                                       (t.dimension == 3 && !t.cubemap) ? t.depth : 1u);
                continue;
            }
        }
        if (own && zsrc < 0 && g.be->color_snapshot) {
            eng_surface* s = &g.surfaces[surface_at];
            rsx_dsp_surface sf;
            rsx_dsp_get_surface(&g.rsx, &sf);
            if (!eng_color_feedback_compatible(s, &t, &sf))
                return eng_color_feedback_failure(u, s, &t);
            u32 k = 0;
            while (k < n_targets && target_slots[k] != (u32)surface_at) k++;
            if (k == n_targets) return eng_color_feedback_failure(u, s, &t);
            if (!feedback[k]) {
                const u32 copy = g.be->color_snapshot(g.be->user, s->handle);
                if (!copy) return eng_color_feedback_failure(u, s, &t);
                if (s->feedback) g.be->texture_release(g.be->user, s->feedback);
                s->feedback = feedback[k] = copy;
            }
            if (g.be->surface_view) {
                textures[u] = g.be->surface_view(g.be->user, feedback[k],
                                               t.remap & 0xffffu, t.format);
                if (!textures[u]) return eng_color_feedback_failure(u, s, &t);
            } else if ((t.remap & 0xffffu) == 0xaae4u) {
                textures[u] = feedback[k];
            } else {
                return eng_color_feedback_failure(u, s, &t);
            }
            continue;
        }
        if (sampled >= 0 && zsrc < 0 &&
            g.surfaces[sampled].fmt == RSX_BE_FMT_R8G8B8A8 &&
            eng_format_reinterprets_rgba8(base_fmt) &&
            eng_surface_to_guest((u32)sampled)) {
            textures[u] = eng_texture_slot(t.location, t.offset, t.format,
                                           t.width, t.height, t.mipmaps, t.pitch,
                                           (int)t.cubemap, t.remap & 0xFFFFu,
                                       (t.dimension == 3 && !t.cubemap) ? t.depth : 1u);
            continue;
        }
        if (sampled >= 0) {
            const u32 view = g.be->surface_view
                ? g.be->surface_view(g.be->user, g.surfaces[sampled].handle,
                                     t.remap & 0xFFFFu, t.format)
                : 0;
            textures[u] = view ? view : g.surfaces[sampled].handle;
            continue;
        }
        if (base_fmt == RSX_TEX_FMT_DEPTH24_D8) {
            for (u32 i = 0; i < g.n_zdepths; i++)
                if (g.zdepths[i].location == t.location &&
                    g.zdepths[i].offset == t.offset && current_zslot != i) {
                    const u32 snap = eng_zdepth_snapshot(i);
                    if (snap) textures[u] = snap;
                    break;
                }
            if (textures[u]) continue;
        }
        textures[u] = eng_texture_slot(t.location, t.offset, t.format,
                                       t.width, t.height, t.mipmaps, t.pitch,
                                       (int)t.cubemap, t.remap & 0xFFFFu,
                                       (t.dimension == 3 && !t.cubemap) ? t.depth : 1u);
    }
    return mask;
}

static u32 sink_bind_vertex_textures(
    u32 vtex_mask, u32 textures[RSX_BE_MAX_VERTEX_TEXTURES],
    rsx_be_sampler_desc samplers[RSX_BE_MAX_VERTEX_TEXTURES])
{
    u32 bound = 0;
    for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++) {
        if (!((vtex_mask >> u) & 1u)) continue;
        rsx_dsp_vertex_texture vt;
        rsx_dsp_get_vertex_texture(&g.rsx, u, &vt);
        eng_decode_sampler(vt.filter, vt.wrap, vt.control0, &samplers[u]);
        /* A vertex unit has no crossbar, so its remap is the identity. */
        textures[u] = eng_texture_slot(vt.location, vt.offset, vt.format,
                                       vt.width, vt.height, vt.mipmaps,
                                       vt.pitch, 0, 0xAAE4u, 1u);
        if (textures[u]) bound |= 1u << u;
    }
    return bound;
}

/* ---- per-pass capture (diagnostic) --------------------------------------- */

/* RSX_ENGINE_PASS_DUMP=<existing dir>: capture the draws pass by pass, from
 * the first draw RSX_ENGINE_PASS_DUMP_AFTER seconds after the engine's first
 * one or, without that, from the frame after the first presented frame of at
 * least RSX_ENGINE_PASS_DUMP_MINDRAWS guest draws (default 1).
 *
 * A pass is a run of draws and clears into one target set and zeta. When the
 * set changes, each colour target of the pass that ended is read back to
 * <dir>/passNNN_tK.ppm, its zeta to <dir>/passNNN_z.pgm (stretched from its
 * smallest to its largest value), and <dir>/passes.txt gets one line: the
 * targets, the zeta and its depth range, the draw and clear counts, each
 * clear's scissor, and the first draw's state and textures. Each texture
 * names the colour surface (S) and the depth target (Z) at its address with
 * the serial of their last write -- the newer one is what was sampled -- or G
 * for guest memory. Each presented surface goes to <dir>/presentNNN.ppm,
 * named after the pass count, and the capture runs on across presents until
 * RSX_ENGINE_PASS_DUMP_MAX passes (default 64). RSX_ENGINE_PASS_DUMP_DRAWS=1
 * also writes one line per draw to <dir>/draws.txt: its pipeline, colour
 * mask, blend, depth state and the textures it samples.
 *
 * The question it answers is which pass of a multi-pass frame first comes out
 * wrong. */
static struct {
    int state;              /* 0 unread, 1 waiting, 2 capturing, 3 done/off */
    const char* dir;
    u32 min_draws;
    u32 after_s;
    u32 max_passes;
    time_t start;
    FILE* index;
    FILE* draws_log;        /* RSX_ENGINE_PASS_DUMP_DRAWS: one line a draw */
    u32 pass;
    int open;
    u32 n_targets, targets[RSX_BE_MAX_COLOR_TARGETS], zslot;
    u32 draws, clears;
    char first[2048];
} s_pd;

static int pd_open_index(void)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/passes.txt", s_pd.dir);
    s_pd.index = fopen(path, "w");
    if (!s_pd.index) { s_pd.state = 3; return 0; }
    if (getenv("RSX_ENGINE_PASS_DUMP_DRAWS")) {
        snprintf(path, sizeof path, "%s/draws.txt", s_pd.dir);
        s_pd.draws_log = fopen(path, "w");
    }
    s_pd.state = 2;
    s_pd.open = 0;
    return 1;
}

static int pd_capturing(void)
{
    if (s_pd.state == 0) {
        s_pd.dir = getenv("RSX_ENGINE_PASS_DUMP");
        const char* m = getenv("RSX_ENGINE_PASS_DUMP_MINDRAWS");
        s_pd.min_draws = m ? (u32)strtoul(m, NULL, 0) : 1u;
        const char* a = getenv("RSX_ENGINE_PASS_DUMP_AFTER");
        s_pd.after_s = a ? (u32)strtoul(a, NULL, 0) : 0u;
        const char* x = getenv("RSX_ENGINE_PASS_DUMP_MAX");
        s_pd.max_passes = x ? (u32)strtoul(x, NULL, 0) : 64u;
        s_pd.start = time(NULL);
        s_pd.state = (s_pd.dir && *s_pd.dir) ? 1 : 3;
    }
    if (s_pd.state == 1 && s_pd.after_s &&
        difftime(time(NULL), s_pd.start) >= (double)s_pd.after_s && pd_open_index())
        fprintf(s_pd.index, "from a draw of frame %u, %u s in\n", g.frames, s_pd.after_s);
    return s_pd.state == 2;
}

static float pd_half(u16 h)
{
    const u32 e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    float v;
    if (e == 0)       v = (float)m / 16777216.0f;              /* m * 2^-24 */
    else if (e == 31) v = m ? 0.0f : 65504.0f;
    else {
        v = (1.0f + (float)m / 1024.0f);
        for (u32 i = e; i > 15; i--) v *= 2.0f;
        for (u32 i = e; i < 15; i++) v *= 0.5f;
    }
    return (h & 0x8000u) ? -v : v;
}

static u8 pd_unit(float v)
{
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0f) return 255;
    return (u8)(v * 255.0f + 0.5f);
}

/* One colour surface to a binary PPM, alpha dropped, floats clamped to [0,1]. */
static void pd_write_surface(u32 slot, const char* path)
{
    const eng_surface* s = &g.surfaces[slot];
    u32 bpp;
    switch (s->fmt) {
    case RSX_BE_FMT_R8G8B8A8:      bpp = 4;  break;
    case RSX_BE_FMT_R16G16B16A16F: bpp = 8;  break;
    case RSX_BE_FMT_R32G32B32A32F: bpp = 16; break;
    case RSX_BE_FMT_R32F:          bpp = 4;  break;
    default: return;
    }
    if (!s->handle || !s->w || !s->h || !g.be->readback) return;
    u8* raw = (u8*)malloc((size_t)s->w * s->h * bpp);
    u8* rgb = (u8*)malloc((size_t)s->w * s->h * 3u);
    FILE* f = (raw && rgb) ? fopen(path, "wb") : NULL;
    if (f) {
        memset(raw, 0, (size_t)s->w * s->h * bpp);
        g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        g.be->readback(g.be->user, s->handle, 0, 0, s->w, s->h, raw, s->w * bpp);
        for (size_t i = 0; i < (size_t)s->w * s->h; i++) {
            const u8* p = raw + i * bpp;
            u8* o = rgb + i * 3u;
            if (s->fmt == RSX_BE_FMT_R8G8B8A8) {
                o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
            } else if (s->fmt == RSX_BE_FMT_R16G16B16A16F) {
                for (u32 c = 0; c < 3; c++) {
                    u16 h; memcpy(&h, p + c * 2u, 2);
                    o[c] = pd_unit(pd_half(h));
                }
            } else if (s->fmt == RSX_BE_FMT_R32G32B32A32F) {
                for (u32 c = 0; c < 3; c++) {
                    float v; memcpy(&v, p + c * 4u, 4);
                    o[c] = pd_unit(v);
                }
            } else {
                float v; memcpy(&v, p, 4);
                o[0] = o[1] = o[2] = pd_unit(v);
            }
        }
        fprintf(f, "P6\n%u %u\n255\n", s->w, s->h);
        fwrite(rgb, 1, (size_t)s->w * s->h * 3u, f);
        fclose(f);
    }
    free(raw);
    free(rgb);
}

/* A depth target to a greyscale PPM, stretched from its smallest to its
 * largest value so a shallow range still shows; the range goes to the index. */
static void pd_write_depth(u32 zslot, const char* path, float* lo_out, float* hi_out)
{
    const eng_zdepth* z = &g.zdepths[zslot];
    *lo_out = *hi_out = -1.0f;
    if (!g.be->depth_readback || !z->handle) return;
    const u32 w = z->used_w && z->used_w < z->w ? z->used_w : z->w;
    const u32 h = z->used_h && z->used_h < z->h ? z->used_h : z->h;
    float* d = (float*)malloc((size_t)w * h * sizeof(float));
    u8* px = (u8*)malloc((size_t)w * h);
    FILE* f = (d && px) ? fopen(path, "wb") : NULL;
    if (f) {
        g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        if (g.be->depth_readback(g.be->user, z->handle, 0, 0, w, h, d, w * 4u) == 0) {
            float lo = 1.0f, hi = 0.0f;
            for (size_t i = 0; i < (size_t)w * h; i++) {
                if (d[i] < lo) lo = d[i];
                if (d[i] > hi) hi = d[i];
            }
            const float span = hi > lo ? hi - lo : 1.0f;
            for (size_t i = 0; i < (size_t)w * h; i++)
                px[i] = (u8)((d[i] - lo) / span * 255.0f);
            fprintf(f, "P5\n%u %u\n255\n", w, h);
            fwrite(px, 1, (size_t)w * h, f);
            *lo_out = lo; *hi_out = hi;
        }
        fclose(f);
    }
    free(d);
    free(px);
}

static void pd_close_pass(void)
{
    if (!s_pd.open) return;
    char path[1024];
    fprintf(s_pd.index, "pass %03u: draws %u clears %u |", s_pd.pass, s_pd.draws,
            s_pd.clears);
    for (u32 k = 0; k < s_pd.n_targets; k++) {
        const eng_surface* s = &g.surfaces[s_pd.targets[k]];
        fprintf(s_pd.index, " %c 0x%08X fmt %d %ux%u p%u", 'A' + (int)k, s->offset,
                (int)s->fmt, s->w, s->h, s->pitch);
        snprintf(path, sizeof path, "%s/pass%03u_t%u.ppm", s_pd.dir, s_pd.pass, k);
        pd_write_surface(s_pd.targets[k], path);
    }
    if (s_pd.zslot != ENG_INVALID) {
        float lo, hi;
        snprintf(path, sizeof path, "%s/pass%03u_z.pgm", s_pd.dir, s_pd.pass);
        pd_write_depth(s_pd.zslot, path, &lo, &hi);
        const eng_zdepth* z = &g.zdepths[s_pd.zslot];
        fprintf(s_pd.index, " | zeta 0x%08X p%u depth %.9g..%.9g (%ux%u, written %ux%u)",
                z->offset, z->pitch, lo, hi, z->w, z->h, z->used_w, z->used_h);
    }
    fprintf(s_pd.index, " |%s\n", s_pd.first);
    fflush(s_pd.index);
    s_pd.pass++;
    s_pd.open = 0;
    if (s_pd.pass >= s_pd.max_passes) {
        fprintf(s_pd.index, "stopped after %u passes\n", s_pd.pass);
        fclose(s_pd.index);
        s_pd.index = NULL;
        if (s_pd.draws_log) { fclose(s_pd.draws_log); s_pd.draws_log = NULL; }
        s_pd.state = 3;
        fprintf(stderr, "[rsx engine] pass dump: %u passes written to %s\n",
                s_pd.pass, s_pd.dir);
    }
}

/* A draw (or, with rs NULL, a clear) into this target set: close the pass
 * when the set changed, and describe the first draw of a new one. */
static void pd_note(const u32* targets, u32 n_targets, u32 zslot,
                    const rsx_be_render_state* rs, u32 pipeline, u32 tex_mask)
{
    if (!pd_capturing()) return;
    int same = s_pd.open && s_pd.n_targets == n_targets && s_pd.zslot == zslot;
    for (u32 k = 0; same && k < n_targets; k++) same = s_pd.targets[k] == targets[k];
    if (!same) {
        pd_close_pass();
        s_pd.open = 1;
        s_pd.n_targets = n_targets;
        memcpy(s_pd.targets, targets, n_targets * sizeof(u32));
        s_pd.zslot = zslot;
        s_pd.draws = s_pd.clears = 0;
        s_pd.first[0] = 0;
    }
    if (!rs) {
        s_pd.clears++;
        const u32 h = rsx_dsp_reg(&g.rsx, M_SCISSOR_HORIZONTAL);
        const u32 v = rsx_dsp_reg(&g.rsx, M_SCISSOR_VERTICAL);
        const size_t n = strlen(s_pd.first);
        snprintf(s_pd.first + n, sizeof s_pd.first - n,
                 " [clear after %u draws, scissor %u,%u %ux%u]",
                 s_pd.draws, h & 0xFFFFu, v & 0xFFFFu, h >> 16, v >> 16);
        return;
    }
    if (s_pd.draws_log) {
        fprintf(s_pd.draws_log, "pass %03u draw %u: pipe %u cmask %08X blend %u %X/%X"
                " dt %u dw %u at %u |", s_pd.pass, s_pd.draws, pipeline, rs->color_mask,
                rs->blend_enable, rs->sf_rgb, rs->df_rgb, rs->depth_test, rs->depth_write,
                rs->alpha_test_enable);
        for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
            if (!(tex_mask & (1u << u))) continue;
            rsx_dsp_texture t;
            rsx_dsp_get_texture(&g.rsx, u, &t);
            fprintf(s_pd.draws_log, " t%u %u:0x%08X f%02X %ux%u", u, t.location, t.offset,
                    t.format & 0xFFu, t.width, t.height);
        }
        fprintf(s_pd.draws_log, "\n");
    }
    if (s_pd.draws++) return;

    rsx_dsp_surface sf;
    rsx_dsp_viewport vp;
    rsx_dsp_get_surface(&g.rsx, &sf);
    rsx_dsp_get_viewport(&g.rsx, &vp);
    size_t at = strlen(s_pd.first);
    at += (size_t)snprintf(s_pd.first + at, sizeof s_pd.first - at,
        " rt_sel %u cfmt %u zfmt %u clip %ux%u | pipe %u dt %u dw %u df %u cmask %08X"
        " blend %u %X/%X stencil %u vpz %g+%g |",
        sf.color_target, sf.color_format, sf.depth_format, sf.clip_w, sf.clip_h,
        pipeline, rs->depth_test, rs->depth_write, rs->depth_func, rs->color_mask,
        rs->blend_enable, rs->sf_rgb, rs->df_rgb, rs->stencil_enable,
        vp.scale[2], vp.translate[2]);
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES && at < sizeof s_pd.first; u++) {
        if (!(tex_mask & (1u << u))) continue;
        rsx_dsp_texture t;
        rsx_dsp_get_texture(&g.rsx, u, &t);
        /* S<n> a colour surface, Z<n> a depth target, each with the serial
         * of its last write: the newer one is what the binding sampled. */
        char src[48] = "G";
        size_t sl = 0;
        for (u32 i = 0; i < g.n_surfaces; i++)
            if (g.surfaces[i].handle && g.surfaces[i].location == t.location &&
                g.surfaces[i].offset == t.offset) {
                sl = (size_t)snprintf(src, sizeof src, "S%u@%llu", i,
                                      (unsigned long long)g.surfaces[i].color_serial);
                break;
            }
        for (u32 i = 0; i < g.n_zdepths; i++)
            if (g.zdepths[i].location == t.location && g.zdepths[i].offset == t.offset) {
                snprintf(src + sl, sizeof src - sl, "%sZ%u@%llu", sl ? "," : "", i,
                         (unsigned long long)g.zdepths[i].depth_serial);
                break;
            }
        at += (size_t)snprintf(s_pd.first + at, sizeof s_pd.first - at,
            " t%u %u:0x%08X f%02X %ux%u p%u%s r%04X zf%u %s", u, t.location, t.offset,
            t.format & 0xFFu, t.width, t.height, t.pitch, t.cubemap ? " cube" : "",
            t.remap & 0xFFFFu, t.wrap >> 28, src);
    }
}

/* Called on every present: arm on the first frame heavy enough when no delay
 * was asked for, and while capturing, close the pass in progress and write
 * the presented surface. */
static void pd_present(u32 presented)
{
    if (s_pd.state == 0) pd_capturing();
    if (s_pd.state == 1 && !s_pd.after_s && g.guest_draws >= s_pd.min_draws) {
        if (pd_open_index())
            fprintf(s_pd.index, "frame %u (after a frame of %u guest draws)\n",
                    g.frames + 1, g.guest_draws);
        return;
    }
    if (s_pd.state == 2) {
        char path[1024];
        pd_close_pass();
        if (s_pd.state != 2) return;               /* that pass was the last */
        snprintf(path, sizeof path, "%s/present%03u.ppm", s_pd.dir, s_pd.pass);
        pd_write_surface(presented, path);
        fprintf(s_pd.index, "present%03u: surface %u 0x%08X, %u guest draws\n",
                s_pd.pass, presented, g.surfaces[presented].offset, g.guest_draws);
        fflush(s_pd.index);
    }
}

static void sink_end(void* user, const rsx_dispatch* r)
{
    (void)user; (void)r;
    if (!g.ready || !dc.n_packets || !dc.fetch_ok) return;
    /* Mixed geometry sources have no defined concatenation here. Reject the
     * whole group instead of drawing partial/misordered geometry. */
    if (dc.n_immediate && (dc.n_arr || dc.n_idx || dc.inl_bytes ||
                          r->inline_dropped != dc.inline_dropped_at_begin)) return;

    const u32 prim = g.rsx.current_primitive;
    /* Everything that becomes triangles -- lists, strips, fans, quads, quad
     * strips and polygons -- is REBUILT into one triangle list through an
     * index buffer. A host strip topology cannot express a restart cut, which
     * is the whole reason the reference engine rebuilds them, and rebuilding
     * is also what lets repeated references share an uploaded vertex. Points
     * and lines pass through as the guest issued them, and anything left that
     * would need an expansion this engine does not have is dropped rather
     * than drawn as something else. */
    rsx_topology topology = RSX_TOPOLOGY_TRIANGLES;
    int scratch = 0;
    const int rebuild = eng_topology_rebuild(prim, 0, &scratch) != 0;
    if (!rebuild) {
        if (rsx_primitive_needs_expansion(prim)) return;
        topology = rsx_primitive_topology(prim);
        if (topology == RSX_TOPOLOGY_UNSUPPORTED) return;
    }

    rsx_vertex_layout_plan layout;
    eng_vertex_layout(&layout);
    dc_fetch(&layout, rebuild);
    if (!dc.n_verts || !dc.fetch_ok) return;

    int indexed = 0;
    u32 n_draw = dc.n_source_refs;
    if (rebuild) {
        if (!eng_topology_rebuild(prim, dc.refs_remapped, &indexed)) return;
        n_draw = indexed
            ? rsx_draw_engine_topology_index_count(prim, dc.n_source_refs,
                                                   dc.cuts, dc.n_cuts)
            : dc.n_source_refs - dc.n_source_refs % 3u;
    }
    if (!n_draw) return;

    rsx_be_render_state rs;
    rsx_draw_engine_decode_render_state(&g.rsx, &rs);

    u32 targets[RSX_BE_MAX_COLOR_TARGETS];
    const u32 n_targets = eng_current_target_set(targets);
    if (!n_targets) return;
    const u32 target = targets[0];

    rsx_dsp_surface sf;
    rsx_dsp_viewport vp;
    rsx_dsp_get_surface(&g.rsx, &sf);
    rsx_dsp_get_viewport(&g.rsx, &vp);
    const u32 zslot = eng_zdepth_get(sf.zeta_location, sf.zeta_offset,
                                     sf.clip_w, sf.clip_h);

    /* A guest program pair that will not translate has no fallback: the draw
     * is dropped, as the reference engine drops it. Substituting the built-in
     * program would draw the geometry in the wrong colours, which is harder to
     * see than a missing object and hides the translation failure. */
    int pipeline_is_fixed = 1;
    u32 tex_used = 0;
    u64 fp_hash = 0;
    const u32 pipeline = eng_pipeline_get(&layout, &rs,
                                          eng_surface_format(sf.color_format),
                                          n_targets, &pipeline_is_fixed, &tex_used,
                                          &fp_hash);
    if (!pipeline) return;
    if (fp_hash && eng_debug_fp(fp_hash, pipeline)) return;

    /* Textures before any pipeline or target binding: a cache refresh can
     * replace a resource, and the backend is free to submit while doing it. */
    u32 textures[RSX_BE_MAX_TEXTURES];
    rsx_be_sampler_desc samplers[RSX_BE_MAX_TEXTURES];
    u32 vtextures[RSX_BE_MAX_VERTEX_TEXTURES];
    rsx_be_sampler_desc vsamplers[RSX_BE_MAX_VERTEX_TEXTURES];
    memset(textures, 0, sizeof textures);
    memset(samplers, 0, sizeof samplers);
    memset(vtextures, 0, sizeof vtextures);
    memset(vsamplers, 0, sizeof vsamplers);
    const u32 tex_mask = sink_bind_textures(targets, n_targets, zslot, tex_used,
                                            textures, samplers);
    if (tex_mask == ENG_INVALID) return;
    const u32 vtex_mask = sink_bind_vertex_textures(eng_vtex_mask(), vtextures,
                                                    vsamplers);

    if (indexed) {
        if (!dc_reserve_indices(n_draw)) return;
        rsx_draw_engine_write_topology_indices(
            prim, dc.n_source_refs, dc.cuts, dc.n_cuts,
            dc.refs_remapped ? dc.ref_remap.occurrence_to_unique : NULL,
            g.indices);
    }

    /* The transform constant block, then the viewport epilogue the vertex
     * program's tail multiplies by: RSX window coordinates mapped into the
     * host's clip space (rsx_live_draw.c:6143-6156). */
    const float W = sf.clip_w ? (float)sf.clip_w : (float)g.width;
    const float H = sf.clip_h ? (float)sf.clip_h : (float)g.height;
    /* The guest viewport is in pixels; an antialiased surface's clip is in
     * samples (aa_sx x aa_sy per pixel), so map NDC over the pixel extent. */
    const float Wv = W / (float)sf.aa_sx;
    const float Hv = H / (float)sf.aa_sy;
    float xf[8] = { 1, 1, 1, 0, 0, 0, 0, 0 };
    if (vp.scale[0] != 0.0f || vp.translate[0] != 0.0f) {
        xf[0] = vp.scale[0] / (Wv * 0.5f);
        xf[1] = -(vp.scale[1] / (Hv * 0.5f));
        xf[2] = vp.scale[2];
        xf[4] = (vp.translate[0] - Wv * 0.5f) / (Wv * 0.5f);
        xf[5] = -((vp.translate[1] - Hv * 0.5f) / (Hv * 0.5f));
        xf[6] = vp.translate[2];
    }
    memcpy(g.vp_cb, g.rsx.constants, RSX_DSP_NUM_CONSTANTS * 16u);
    memcpy(g.vp_cb + RSX_DSP_NUM_CONSTANTS * 16u, xf, sizeof xf);

    /* The buffered fragment constants, then fp_alpha: the layout
     * rsx_fp_decompile_buffered_ex compiled the shader against. */
    const u32 nslots = g.fp_constants.count ? g.fp_constants.count : 1u;
    const u32 fp_bytes = (nslots + 1u) * 16u;
    if (g.fp_cb_cap < fp_bytes) {
        u8* n = (u8*)realloc(g.fp_cb, fp_bytes);
        if (!n) return;
        g.fp_cb = n;
        g.fp_cb_cap = fp_bytes;
    }
    memset(g.fp_cb, 0, fp_bytes);
    if (g.fp_constants.count)
        memcpy(g.fp_cb, g.fp_constants.values, g.fp_constants.count * 16u);
    { float* alpha = (float*)(g.fp_cb + nslots * 16u);
      alpha[0] = rsx_fp_alpha_ref(rs.alpha_ref_raw, rs.alpha_ref_format);
      alpha[1] = alpha[2] = alpha[3] = 0.0f; }

    pd_note(targets, n_targets, zslot, &rs, pipeline, tex_mask);

    /* The depth attachment is cleared the first time it is bound, so a title
     * that never clears its shadow zeta still tests against something. */
    const u32 depth_handle = (zslot != ENG_INVALID) ? g.zdepths[zslot].handle : 0;
    if (zslot != ENG_INVALID && !g.zdepths[zslot].cleared) {
        g.be->clear_depth_stencil(g.be->user, depth_handle,
                                  RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL,
                                  1.0f, 0);
        g.zdepths[zslot].cleared = 1;
        g.zdepths[zslot].had_write = 0;
    }

    u32 handles[RSX_BE_MAX_COLOR_TARGETS];
    for (u32 i = 0; i < n_targets; i++) handles[i] = g.surfaces[targets[i]].handle;
    g.be->bind_targets(g.be->user, handles, n_targets, depth_handle);
    g.be->bind_pipeline(g.be->user, pipeline);
    g.be->bind_vs_constants(g.be->user, g.vp_cb, ENG_VP_CB_BYTES);
    g.be->bind_ps_constants(g.be->user, g.fp_cb, fp_bytes);
    g.be->bind_textures(g.be->user, textures, samplers, tex_mask);
    g.be->bind_vertex_textures(g.be->user, vtextures, vsamplers, vtex_mask);
    g.be->set_viewport(g.be->user, 0.0f, 0.0f, W, H);

    /* The guest scissor intersected with the surface. The nv40 reset is a
     * full 4096x4096 window and a never-written register reads 0 here, which
     * the w == 0 test treats as no scissor, so only a real game scissor
     * narrows anything (rsx_live_draw.c:6180-6197). */
    {
        u32 sx = 0, sy = 0;
        u32 sw = g.surfaces[target].w, sh = g.surfaces[target].h;
        const u32 h = rsx_dsp_reg(&g.rsx, M_SCISSOR_HORIZONTAL);
        const u32 v = rsx_dsp_reg(&g.rsx, M_SCISSOR_VERTICAL);
        /* The scissor is in pixels: scale it to the sample grid. */
        const u32 gx = (h & 0xFFFFu) * sf.aa_sx, gw = (h >> 16) * sf.aa_sx;
        const u32 gy = (v & 0xFFFFu) * sf.aa_sy, gh = (v >> 16) * sf.aa_sy;
        if (gw > 0 && gh > 0) {
            u32 right = sx + sw, bottom = sy + sh;
            if (gx > sx) sx = gx;
            if (gy > sy) sy = gy;
            if (gx + gw < right)  right = gx + gw;
            if (gy + gh < bottom) bottom = gy + gh;
            sw = right > sx ? right - sx : 0;
            sh = bottom > sy ? bottom - sy : 0;
        }
        g.be->set_scissor(g.be->user, sx, sy, sw, sh);
    }
    /* Dynamic, and deliberately outside the pipeline key. */
    g.be->set_stencil_ref(g.be->user,
                          rsx_dsp_reg(&g.rsx, M_STENCIL_FUNC_REF) & 0xFFu);
    /* The bounds too: a deferred renderer moves them between its light and
     * sky passes without changing anything else. Raw floats, as written. */
    if (g.be->set_depth_bounds) {
        const u32 lo = rsx_dsp_reg(&g.rsx, M_DEPTH_BOUNDS_MIN);
        const u32 hi = rsx_dsp_reg(&g.rsx, M_DEPTH_BOUNDS_MAX);
        float fmin, fmax;
        memcpy(&fmin, &lo, sizeof fmin);
        memcpy(&fmax, &hi, sizeof fmax);
        g.be->set_depth_bounds(g.be->user, fmin, fmax);
    }

    const u32 uploaded = indexed ? dc.n_verts : n_draw;
    g.be->draw(g.be->user, topology, dc.verts, uploaded, layout.stride,
               indexed ? g.indices : NULL, indexed ? n_draw : 0);
    /* Counted only when the draw ran the guest's OWN programs, which is what
     * the test hook means: a draw through the built-in pair is a draw, not
     * evidence that the decompile-translate-compile path worked. */
    if (!pipeline_is_fixed) g.guest_draws++;

    if (rs.color_mask) eng_surfaces_wrote(targets, n_targets);
    if (zslot != ENG_INVALID && rs.depth_test && rs.depth_write) {
        g.zdepths[zslot].had_write = 1;
        eng_zdepth_wrote(zslot, &sf);
    }
}

static void sink_clear(void* user, const rsx_dispatch* r, u32 mask)
{
    (void)user; (void)r;
    if (!g.ready) return;
    u32 targets[RSX_BE_MAX_COLOR_TARGETS];
    const u32 n_targets = eng_current_target_set(targets);
    if (!n_targets) return;
    if (pd_capturing()) {
        rsx_dsp_surface zs;
        rsx_dsp_get_surface(&g.rsx, &zs);
        pd_note(targets, n_targets,
                eng_zdepth_get(zs.zeta_location, zs.zeta_offset, zs.clip_w, zs.clip_h),
                NULL, 0, 0);
    }

    if (mask & (RSX_CLEAR_COLOR_R | RSX_CLEAR_COLOR_G |
                RSX_CLEAR_COLOR_B | RSX_CLEAR_COLOR_A)) {
        const u32 c = rsx_dsp_clear_color(&g.rsx);
        const float rgba[4] = {
            (float)((c >> 16) & 0xFF) / 255.0f,
            (float)((c >>  8) & 0xFF) / 255.0f,
            (float)( c        & 0xFF) / 255.0f,
            (float)((c >> 24) & 0xFF) / 255.0f,
        };
        /* Every target the set names, not only A: a deferred pass clears its
         * whole G-buffer in one CLEAR_SURFACE, and B, C and D would otherwise
         * keep whatever the last frame left in them. */
        for (u32 i = 0; i < n_targets; i++)
            g.be->clear_color(g.be->user, g.surfaces[targets[i]].handle, rgba);
        eng_surfaces_wrote(targets, n_targets);
    }
    if (mask & (RSX_CLEAR_DEPTH | RSX_CLEAR_STENCIL)) {
        rsx_dsp_surface sf;
        rsx_dsp_get_surface(&g.rsx, &sf);
        const u32 zslot = eng_zdepth_get(sf.zeta_location, sf.zeta_offset,
                                         sf.clip_w, sf.clip_h);
        if (zslot == ENG_INVALID) return;
        u32 flags = 0;
        if (mask & RSX_CLEAR_DEPTH)   flags |= RSX_BE_CLEAR_DEPTH;
        if (mask & RSX_CLEAR_STENCIL) flags |= RSX_BE_CLEAR_STENCIL;
        /* ZSTENCIL_CLEAR_VALUE is laid out as the zeta is: depth24 << 8 |
         * stencil8 for Z24S8, a bare depth16 for Z16 (SURFACE_FORMAT depth
         * field 1). Read as Z24S8, a Z16 surface's 0xFFFF -- its far plane --
         * would clear it to 255/16777215, nearly the near plane, and every
         * LESS or LEQUAL draw into it would then fail. The nv40 reset
         * 0xFFFFFF00 seeded by rsx_dispatch_init clears to 1.0 / 0. */
        const u32 zs = rsx_dsp_reg(&g.rsx, M_ZSTENCIL_CLEAR);
        const int z16 = sf.depth_format == 1u;
        const float depth = !zs ? 1.0f
                          : z16 ? (float)(zs & 0xFFFFu) / 65535.0f
                                : (float)(zs >> 8) / 16777215.0f;
        g.be->clear_depth_stencil(g.be->user, g.zdepths[zslot].handle, flags,
                                  depth, z16 ? 0 : (u8)(zs & 0xFFu));
        g.zdepths[zslot].cleared = 1;
        if (mask & RSX_CLEAR_DEPTH) {
            /* A clear invalidates the older published depth image; the next
             * texture consumer resolves the newly written pass exactly once. */
            g.zdepths[zslot].had_write = 0;
            g.zdepths[zslot].snapshot_valid = 0;
            eng_zdepth_wrote(zslot, &sf);
        }
    }
}

/* Resolve a flip's buffer id to a REGISTERED surface rather than whatever is
 * currently bound: the live target at that instant is often an offscreen
 * shadow or post-process surface, and copying it presents black despite the
 * scene's draws having executed (rsx_live_draw.c:7557-7570). */
static u32 eng_present_surface(u32 buffer_id)
{
    if (buffer_id < 8 && g.display_buffers[buffer_id].valid) {
        const eng_display_buffer* d = &g.display_buffers[buffer_id];
        for (u32 i = 0; i < g.n_surfaces; i++)
            if (g.surfaces[i].handle && g.surfaces[i].location == d->location &&
                g.surfaces[i].offset == d->offset)
                return i;
    }
    return eng_current_surface();
}

static void eng_present(u32 buffer_id)
{
    if (!g.ready) return;
    const u32 target = eng_present_surface(buffer_id);
    if (target == ENG_INVALID) return;
    if (getenv("PS3RECOMP_METAL_FRAME_DUMP") && g.frames % 120 == 0) {
        u32 current = eng_current_surface();
        fprintf(stderr, "[rsx capture] frame=%u buffer=%u selected=%u current=%u draws=%u surfaces=%u\n",
                g.frames, buffer_id, target, current, g.guest_draws, g.n_surfaces);
        for (u32 i = 0; i < g.n_surfaces; ++i)
            fprintf(stderr, "[rsx capture] surface %u loc=%u offset=%08X size=%ux%u handle=%u\n",
                    i, g.surfaces[i].location, g.surfaces[i].offset,
                    g.surfaces[i].w, g.surfaces[i].h, g.surfaces[i].handle);
    }
    pd_present(target);
    g.last_present_surface = target;
    g.be->present(g.be->user, g.surfaces[target].handle);
    g.frames++;
    g.last_guest_draws = g.guest_draws;
    g.guest_draws = 0;
}

static void sink_flip(void* user, const rsx_dispatch* r, u32 arg)
{
    (void)user; (void)r;
    g.last_flip_buffer = arg & 7u;
    eng_present(g.last_flip_buffer);
}

/* ---- public API ---------------------------------------------------------- */

void rsx_draw_engine_set_backend(const rsx_draw_backend* backend)
{
    g.be = backend;
}

void rsx_draw_engine_set_default(int on)
{
    g.default_on = on;
}

int rsx_draw_engine_enabled(void)
{
    static int cached = -1;
    static int env_seen = 0;
    static int env_on = 0;
    if (!env_seen) {
        const char* e = getenv("PS3RECOMP_RSX_ENGINE");
        env_seen = 1;
        if (e && *e) env_on = (strcmp(e, "dispatch") == 0) ? 1 : -1;
    }
    /* The environment is authoritative in both directions; without it the
     * backend's own default decides. Cached after the first backend has
     * registered, so the FIFO walker pays one compare. */
    const int on = env_on > 0 ? 1 : (env_on < 0 ? 0 : g.default_on);
    if (!g.be) return 0;
    if (cached < 0) cached = on;
    return cached;
}

int rsx_draw_engine_init(u32 width, u32 height)
{
    if (!g.be) return -1;
    g.width  = width  ? width  : 1280;
    g.height = height ? height : 720;
    { const char* e = getenv("PS3RECOMP_ENG_TEXHASH_FULL");
      s_texhash_full_always = e && *e && strcmp(e, "0") != 0; }
    g.vp_cb = (u8*)calloc(1, ENG_VP_CB_BYTES);
    if (!g.vp_cb) return -1;
    if (g.be->init && g.be->init(g.be->user, g.width, g.height) != 0) {
        free(g.vp_cb); g.vp_cb = NULL;
        return -1;
    }
    rsx_dispatch_sink sink;
    memset(&sink, 0, sizeof sink);
    sink.clear            = sink_clear;
    sink.begin            = sink_begin;
    sink.end              = sink_end;
    sink.draw_arrays      = sink_draw_arrays;
    sink.draw_index_array = sink_draw_index;
    sink.inline_array     = sink_inline_array;
    sink.immediate_vertex = sink_immediate_vertex;
    sink.flip             = sink_flip;
    rsx_dispatch_init(&g.rsx, &sink);
    g.ready = 1;
    /* Display buffers the title registered before the backend came up. */
    cellGcm_replay_display_buffers_to_engine();
    fprintf(stderr, "[rsx engine] register-file draw engine up (%ux%u)\n",
            g.width, g.height);
    return 0;
}

void rsx_draw_engine_shutdown(void)
{
    if (!g.be) return;
    if (g.ready && g.be->submit_and_wait)
        g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_SHUTDOWN);
    for (u32 i = 0; i < g.n_textures; i++)
        if (g.textures[i].handle) g.be->texture_release(g.be->user, g.textures[i].handle);
    for (u32 i = 0; i < g.n_pipelines; i++)
        if (g.pipelines[i].handle) g.be->pipeline_release(g.be->user, g.pipelines[i].handle);
    for (u32 i = 0; i < g.n_surfaces; i++) {
        if (g.surfaces[i].feedback) g.be->texture_release(g.be->user, g.surfaces[i].feedback);
        if (g.surfaces[i].handle) g.be->color_target_release(g.be->user, g.surfaces[i].handle);
    }
    for (u32 i = 0; i < g.n_zdepths; i++)
        if (g.zdepths[i].handle) g.be->depth_target_release(g.be->user, g.zdepths[i].handle);
    if (g.ready && g.be->shutdown) g.be->shutdown(g.be->user);

    rsx_draw_engine_set_guest_memory(NULL, NULL);
    free(dc.refs); free(dc.cuts); free(dc.verts); free(dc.immediate);
    rsx_vertex_remap_destroy(&dc.ref_remap);
    memset(&dc, 0, sizeof dc);
    free(g.tex_staging); free(g.vp_cb); free(g.fp_cb); free(g.indices);
    const rsx_draw_backend* be = g.be;
    memset(&g, 0, sizeof g);
    g.be = be;
}

void rsx_draw_engine_method(u32 method, u32 arg)
{
    if (!g.ready) return;
    /* The subchannel is a binding slot, not an engine selector, and a title
     * whose SPU-built command lists bind NV4097 elsewhere would otherwise
     * store its state in the wrong register bank.  Standard NV4097 methods
     * (< 0x2000) strip the subchannel with & 0x1FFC.  Driver methods
     * (0xE9xx, 0xEBxx) encode the subchannel bits as part of the address;
     * preserve them with the wider mask. */
    u32 m = (method >= 0xE000u) ? (method & 0xFFFCu) : (method & 0x1FFCu);
    rsx_dispatch_method(&g.rsx, m, arg);
}

void rsx_draw_engine_set_display_buffer(u32 buffer_id, u32 location, u32 offset,
                                        u32 pitch, u32 width, u32 height)
{
    if (buffer_id >= 8) return;
    eng_display_buffer* d = &g.display_buffers[buffer_id];
    d->location = location;
    d->offset = offset;
    d->pitch = pitch;
    d->width = width;
    d->height = height;
    d->valid = width && height;
}

void rsx_draw_engine_flush(void)
{
    if (g.ready && g.be->submit_and_wait)
        g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
}

void rsx_draw_engine_present(void)
{
    /* A host that drives the flip itself gets whichever buffer the guest's
     * own last flip named, and display buffer 0 before there has been one:
     * a runner that both consumes 0xE944 and calls here would otherwise
     * re-present a double-buffered title's OTHER scanout. Presenting twice is
     * harmless either way -- the engine never clears the surface, so a second
     * present just blits the same image again. */
    eng_present(g.last_flip_buffer);
}

void rsx_draw_engine_present_buffer(u32 buffer_id)
{
    g.last_flip_buffer = buffer_id & 7u;
    eng_present(g.last_flip_buffer);
}

void rsx_draw_engine_note_flip(u32 buffer_id)
{
    g.last_flip_buffer = buffer_id & 7u;
}

u32 rsx_draw_engine_guest_draws(void)
{
    return g.last_guest_draws;
}

u32 rsx_draw_engine_readback_center(void)
{
    if (!g.ready || !g.be->readback) return 0;
    const u32 slot = g.last_present_surface;
    if (slot >= g.n_surfaces) return 0;
    const eng_surface* s = &g.surfaces[slot];
    if (!s->handle || s->fmt != RSX_BE_FMT_R8G8B8A8 || !s->w || !s->h) return 0;
    u8 px[4] = { 0, 0, 0, 0 };
    g.be->readback(g.be->user, s->handle, s->w / 2u, s->h / 2u, 1, 1, px, 4);
    /* Decoded rows are R,G,B,A; the guest's own clear colour is A8R8G8B8, and
     * a host comparing the two wants them in the same order. */
    return ((u32)px[3] << 24) | ((u32)px[0] << 16) |
           ((u32)px[1] << 8)  |  (u32)px[2];
}
