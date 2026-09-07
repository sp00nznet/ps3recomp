/*
 * ps3recomp - platform-neutral RSX draw engine over the register file
 *
 * The vtable backends in rsx_commands.c are driven by rsx_state, a curated
 * dirty-flag struct that models no primitive restart, no vertex textures and
 * no two-sided stencil, and whose setters fire only at SET_BEGIN_END. The
 * register-file dispatcher in rsx_dispatch.c has all of it already, in
 * portable C, behind the rsx_dispatch_sink interface. This engine implements
 * that sink and drives a backend through the record-oriented interface below,
 * so a title's frame reaches the host API the way the hardware issued it.
 *
 * The engine logic is carried across from libs/video/rsx_live_draw.c, the
 * NV4097 -> D3D12 engine a title has actually shipped on. That file is
 * vendored and is NOT touched: what moves here is the orchestration, and the
 * comments name the reference line numbers so the two can be compared. The
 * mathematics is already shared and already portable -- rsx_vertex_compact.c,
 * rsx_restart_cuts.h, rsx_texture_layout.c, the two decompilers.
 *
 * The split between engine and backend is semantic, at the granularity the
 * engine thinks in, not a 1:1 wrapper over one host API's calls: resources by
 * what they are for, pipelines from the two HLSL strings plus render state
 * plus the vertex layout, and per-draw binding of what a draw names. Resource
 * state tracking, descriptor lifetime and command encoding belong entirely to
 * the backend, which is why nothing here mentions a barrier.
 *
 * Handles are small integers owned by the backend; 0 always means "none".
 */
#ifndef PS3RECOMP_RSX_DRAW_ENGINE_H
#define PS3RECOMP_RSX_DRAW_ENGINE_H

#include "rsx_dispatch.h"
#include "rsx_primitives.h"
#include "rsx_texture_layout.h"
#include "rsx_vertex_compact.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- what the engine hands a backend ------------------------------------ */

/* Surface and texture formats the engine asks for. The texture side is
 * rsx_texfmt's classification, which rsx_texture_decode already produces host
 * rows for, so a backend maps one enum rather than two. */
typedef enum {
    RSX_BE_FMT_NONE = 0,
    RSX_BE_FMT_R8,
    RSX_BE_FMT_R8G8,
    RSX_BE_FMT_R8G8B8A8,      /* decoded colour: always R,G,B,A byte order */
    RSX_BE_FMT_BC1,
    RSX_BE_FMT_BC2,
    RSX_BE_FMT_BC3,
    RSX_BE_FMT_R16,
    RSX_BE_FMT_R16G16,
    RSX_BE_FMT_R16G16F,
    RSX_BE_FMT_R16G16B16A16F, /* SET_SURFACE_FORMAT 0xB, the HDR target    */
    RSX_BE_FMT_R32F,
    RSX_BE_FMT_R32G32B32A32F
} rsx_be_format;

/* One texture unit's sampler, decoded out of SET_TEXTURE_FILTER,
 * SET_TEXTURE_ADDRESS and SET_TEXTURE_CONTROL0 the way rsx_live_draw.c's
 * decode_sampler (1464-1487) reads them. The wrap fields stay as the guest's
 * own 1..8 codes because that is what both backends already translate. */
typedef struct rsx_be_sampler_desc {
    u8  min_linear, mag_linear, mip_linear, mip_present;
    u8  wrap_s, wrap_t, wrap_r;
    float min_lod, max_lod;
} rsx_be_sampler_desc;

/* The NV4097 render state a pipeline is built from: rsx_live_draw.c's
 * render_state_t (1312-1327) with the D3D12 enums left undecoded, since the
 * guest values are the portable form and each backend has its own table.
 * The stencil REFERENCE is deliberately absent -- it is set on the encoder,
 * not baked into the pipeline, and so is not part of the pipeline key. */
typedef struct rsx_be_render_state {
    u32 alpha_test_enable, alpha_func, alpha_ref_raw, alpha_ref_format;
    u32 blend_enable, sf_rgb, df_rgb, sf_a, df_a, eq_rgb, eq_a;
    u32 depth_test, depth_write, depth_func;
    u32 cull_enable, cull_face, front_face;
    u32 color_mask;
    u32 rt_fp16;                    /* the target is an FP16 HDR surface */
    u32 stencil_enable, stencil_two_sided;
    u32 s_func, s_func_mask, s_write_mask, s_fail, s_zfail, s_zpass;
    u32 bs_func, bs_fail, bs_zfail, bs_zpass;
} rsx_be_render_state;

/* submit_and_wait reasons, for a backend that wants to log or count them. */
#define RSX_BE_FLUSH_VERTEX_RING     0
#define RSX_BE_FLUSH_GUEST_REFERENCE 1
#define RSX_BE_FLUSH_SHUTDOWN        2

/* clear_depth_stencil flags */
#define RSX_BE_CLEAR_DEPTH   0x1u
#define RSX_BE_CLEAR_STENCIL 0x2u

#define RSX_BE_MAX_TEXTURES        RSX_DSP_NUM_TEXTURES
#define RSX_BE_MAX_VERTEX_TEXTURES RSX_DSP_NUM_VERTEX_TEXTURES
/* Colour targets a draw can name: A alone, or an MRT set of A, B, C and D. */
#define RSX_BE_MAX_COLOR_TARGETS   4

/* How many distinct guest textures the engine keeps before it evicts the
 * least recently used one. Public so the eviction test can be exact about
 * where the boundary is rather than assuming a number. */
#define RSX_DRAW_ENGINE_TEXTURE_CACHE 1024

/* ---- the backend interface ---------------------------------------------- */

typedef struct rsx_draw_backend {
    void* user;

    /* Lifecycle and submission. submit_and_wait is fully synchronous: the
     * engine recycles its staging the moment it returns, which is the same
     * contract as rsx_live_draw.c's ld_flush (1659-1736). */
    int  (*init)(void* user, u32 width, u32 height);
    void (*shutdown)(void* user);
    void (*submit_and_wait)(void* user, u32 reason);

    /* Textures. `remap` is the raw TEXTURE_CONTROL1 crossbar and `rsx_fmt`
     * the SET_TEXTURE_FORMAT byte it is decoded against, because the crossbar
     * is a property of the view rather than of the pixels. Levels arrive one
     * upload each, already decoded to host rows by rsx_texture_decode. */
    u32  (*texture_create)(void* user, rsx_be_format fmt, u32 w, u32 h,
                           u32 mips, u32 faces, u32 remap, u32 rsx_fmt);
    void (*texture_upload)(void* user, u32 texture, u32 face, u32 mip,
                           u32 w, u32 h, const void* src, u32 row_bytes,
                           u32 rows);
    void (*texture_release)(void* user, u32 texture);

    /* Colour targets. `seed` is the guest's own bytes for the surface,
     * decoded to R,G,B,A rows, or NULL when they could not be resolved: a
     * title CPU-initialises its render targets and then samples them before
     * anything draws into them, which is why the D3D12 backend's off_rt_get
     * seeds a new target the same way. */
    u32  (*color_target_create)(void* user, rsx_be_format fmt, u32 w, u32 h,
                                const void* seed, u32 seed_row_bytes);
    void (*color_target_release)(void* user, u32 surface);
    /* A texture handle for a colour target a texture unit names, wearing that
     * unit's TEXTURE_CONTROL1 crossbar. An uploaded texture bakes the crossbar
     * in when it is decoded, so only a surface bind needs this; 0 means the
     * backend has no view and the engine binds the target itself. */
    u32  (*surface_view)(void* user, u32 surface, u32 remap, u32 rsx_format);

    /* One depth target per guest zeta address. */
    u32  (*depth_target_create)(void* user, u32 w, u32 h);
    void (*depth_target_release)(void* user, u32 depth);
    /* Resolve a depth target into a sampleable texture and return it; 0 when
     * the backend cannot. The engine only asks after a depth-writing draw. */
    u32  (*depth_snapshot)(void* user, u32 depth, u32 w, u32 h);

    /* Pipelines, from the decompilers' HLSL plus the state that selects a
     * variant. This is the one call that hides D3DCompile against glslang
     * plus spirv-cross plus -newLibraryWithSource:, and the input layout with
     * it. 0 means the pair could not be built; the engine caches that.
     *
     * `rt_count` is how many colour attachments the pass will have, and every
     * one of them takes target A's blend and colour mask: a zero-initialised
     * secondary attachment writes nothing at all, which would leave a
     * deferred pass with only its first G-buffer plane. It is part of the
     * engine's pipeline key, because a host API matches a pipeline's
     * attachments against the pass it is encoded in. */
    u32  (*pipeline_create)(void* user, const char* vs_hlsl, const char* ps_hlsl,
                            const rsx_be_render_state* rs,
                            const rsx_vertex_layout_plan* layout,
                            u32 vertex_stride, rsx_be_format rt_fmt,
                            u32 rt_count);
    void (*pipeline_release)(void* user, u32 pipeline);

    /* Per-draw binding. The backend owns every ring the D3D12 engine used to
     * own itself, so the engine hands over CPU-side arrays and never mentions
     * a descriptor. */
    /* The colour targets this draw writes, and the depth attachment under
     * them. `count` is 1 for an ordinary draw and up to
     * RSX_BE_MAX_COLOR_TARGETS for an MRT set, whose members are all target
     * A's size and format -- the rule both render paths bind by, and the one
     * a host API enforces on a pass's attachments. */
    void (*bind_targets)(void* user, const u32* surfaces, u32 count, u32 depth);
    void (*bind_pipeline)(void* user, u32 pipeline);
    void (*bind_vs_constants)(void* user, const void* data, u32 bytes);
    void (*bind_ps_constants)(void* user, const void* data, u32 bytes);
    void (*bind_textures)(void* user, const u32* textures,
                          const rsx_be_sampler_desc* samplers, u32 mask);
    void (*bind_vertex_textures)(void* user, const u32* textures,
                                 const rsx_be_sampler_desc* samplers, u32 mask);
    void (*set_viewport)(void* user, float x, float y, float w, float h);
    void (*set_scissor)(void* user, u32 x, u32 y, u32 w, u32 h);
    void (*set_stencil_ref)(void* user, u32 ref);
    /* One draw. `indices` is NULL for a non-indexed one. Everything the
     * hardware has and the host does not -- quads, quad strips, fans,
     * polygons -- has already been expanded into a triangle list by the time
     * it gets here, so the topology is only ever one a host API has. */
    void (*draw)(void* user, rsx_topology topology, const void* vertices,
                 u32 vertex_count, u32 stride, const u32* indices,
                 u32 index_count);

    /* Clears, which are ordered against draws rather than folded into a pass:
     * a title draws the world, clears depth, and draws the weapon over it. */
    void (*clear_color)(void* user, u32 surface, const float rgba[4]);
    void (*clear_depth_stencil)(void* user, u32 depth, u32 flags,
                                float depth_value, u8 stencil);

    /* Present the named surface, and read a rectangle of one back. The rows
     * come back in the format the target was created with, so R,G,B,A for an
     * ordinary colour surface. */
    void (*present)(void* user, u32 surface);
    void (*readback)(void* user, u32 surface, u32 x, u32 y, u32 w, u32 h,
                     void* out, u32 out_pitch);
} rsx_draw_backend;

/* ---- the engine --------------------------------------------------------- */

/* Register the backend the engine drives. Passing NULL unregisters it. */
void rsx_draw_engine_set_backend(const rsx_draw_backend* backend);

/* Is the engine the path this run takes? True when PS3RECOMP_RSX_ENGINE names
 * "dispatch" (or the backend asked for it by default) and a backend has been
 * registered. Read once and cached, so it is cheap in the FIFO walker. */
int  rsx_draw_engine_enabled(void);

/* Called by a backend that wants the engine to be the default path with no
 * environment set. PS3RECOMP_RSX_ENGINE=vtable still turns it off. */
void rsx_draw_engine_set_default(int on);

/* A runner with its own RSX IO map supplies its address resolver before init.
 * NULL restores the toolkit's cellGcm mapping. */
void rsx_draw_engine_set_guest_memory(rsx_vertex_guest_ptr_fn reader, void* user);

int  rsx_draw_engine_init(u32 width, u32 height);
void rsx_draw_engine_shutdown(void);

/* One FIFO method, with its subchannel bits still on: the engine masks them
 * out itself, as rsx_live_draw_method (6902-6976) does, because a title whose
 * SPU-built command lists bind NV4097 to another subchannel would otherwise
 * write its state into the wrong register bank. */
void rsx_draw_engine_method(u32 method, u32 arg);

/* cellGcmSetDisplayBuffer, so the flip can resolve a buffer id to a surface
 * the engine has actually rendered rather than to whatever is bound. */
void rsx_draw_engine_set_display_buffer(u32 buffer_id, u32 location, u32 offset,
                                        u32 pitch, u32 width, u32 height);

/* The guest's SET_REFERENCE sync point: submit and wait, so a title spinning
 * on its fence sees the GPU catch up. */
void rsx_draw_engine_flush(void);

/* Present whatever the frame has recorded, for a host that drives the flip
 * itself rather than through the FIFO's 0xE944. */
void rsx_draw_engine_present(void);
/* A runner retiring a queued flip names the buffer explicitly. */
void rsx_draw_engine_present_buffer(u32 buffer_id);

/* --- test hooks ---------------------------------------------------------- */

/* Draws that ran the guest's OWN programs in the last presented frame. A draw
 * through the built-in pair is not one of them. */
u32  rsx_draw_engine_guest_draws(void);

/* The centre pixel of the surface the last flip presented, as A8R8G8B8 --
 * the convention the RSX clear colour arrives in, so a host can compare the
 * two. Zero when nothing has been presented or the surface is not an
 * 8-bit-per-channel one. */
u32  rsx_draw_engine_readback_center(void);

/* ---- pieces the engine exposes for its own tests ------------------------ */
/*
 * These are the neutral parts the unit tests in libs/video/tests exercise
 * without a GPU: the surface set's keying, the texture cache's revalidation
 * and eviction, the pipeline key's stability, and restart-cut expansion.
 */

/* Decode the NV4097 render state out of the register file. */
void rsx_draw_engine_decode_render_state(const rsx_dispatch* rsx,
                                         rsx_be_render_state* out);

/* Fold the structural render state into a pipeline key. The alpha REFERENCE
 * and everything else that lives in a constant buffer are deliberately left
 * out, so a title changing constants per draw does not multiply pipelines --
 * rsx_live_draw.c's ld_hash_structural_render_state (3148-3187). */
u64  rsx_draw_engine_hash_render_state(const rsx_be_render_state* rs, u64 hash);

/* How many indices the topology expansion produces for `source_refs`
 * references cut at `cuts`, and the indices themselves. `indices` must have
 * room for the count. Triangle strips alternate winding per segment; a quad
 * strip pairs its vertices from each segment's own start, and a polygon is a
 * fan around each segment's own first vertex. */
u32  rsx_draw_engine_topology_index_count(u32 primitive, u32 source_refs,
                                          const u32* cuts, u32 cut_count);
void rsx_draw_engine_write_topology_indices(u32 primitive, u32 source_refs,
                                            const u32* cuts, u32 cut_count,
                                            const u32* occurrence_to_unique,
                                            u32* indices);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_DRAW_ENGINE_H */
