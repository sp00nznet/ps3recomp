/*
 * ps3recomp - RSX -> Metal backend (macOS / Apple Silicon)
 *
 * The first non-Windows render path in the project. It implements the subset
 * of the rsx_backend vtable needed to drive a guest clear/draw/flip loop:
 * `clear` captures the colour written by NV4097_CLEAR_SURFACE,
 * `set_render_target` tracks the guest's surface dimensions, the draw
 * callbacks record draws with their vertices fetched out of guest memory, and
 * present replays them into a drawable. Every dispatch site in rsx_commands.c
 * is guarded (`if (s_backend && s_backend->x)`), so a partial vtable is the
 * intended way to bring a backend up incrementally.
 *
 * Guest shaders. A draw runs the guest's own vertex and fragment programs
 * when both translate: the NV40 microcode goes through the RSX decompilers
 * (HLSL), then rsx_hlsl_to_msl (glslang + spirv-cross), then
 * -newLibraryWithSource:. What the D3D12 backend does with those programs is
 * mirrored here field for field -- where the vertex program starts, how the
 * fragment program is located and keyed, the viewport epilogue, the per-draw
 * constant blocks -- because that is the path real titles have exercised.
 * With no vertex program loaded, or with the translator compiled out, a draw
 * falls back to the built-in fixed-function shader, which is what the
 * clear/draw host checks use.
 *
 * Guest textures. A guest-program draw binds the enabled texture units the
 * way the D3D12 backend does: SET_TEXTURE_FORMAT's location bits say where
 * the bytes are, rsx_texture_layout says what shape they are,
 * rsx_texture_decode turns them into host rows, and the TEXTURE_CONTROL1
 * crossbar becomes the texture's swizzle. Every mip level the register says
 * is there is uploaded, and the sampler's mip filter and LOD range come from
 * SET_TEXTURE_FILTER and SET_TEXTURE_CONTROL0. A unit SET_TEXTURE_FORMAT
 * bit 2 marks as a cube map becomes an MTLTextureTypeCube of six faces, and
 * the fragment program is compiled against that so it samples with a
 * direction. NV4097_SET_VERTEX_TEXTURE_* units go through the same path and
 * bind to the vertex stage, so a transform program's TXL samples.
 *
 * Render targets. SET_SURFACE_COLOR_TARGET picks colour surface A, B or one of
 * the MRT sets, and a colour offset that is not a registered display buffer is
 * an offscreen surface: a texture in a registry keyed by that raw offset, kept
 * across frames because a surface rendered in one frame is sampled in the next.
 * Each record carries the target it was recorded against, and present walks the
 * records opening one render pass per contiguous run with the same target. A
 * texture unit bound at a registered surface's offset then samples that surface
 * rather than guest memory, which is what render-to-texture is.
 *
 * Why Metal rather than SDL_Renderer: SDL2's renderer is a 2D sprite API with
 * no route to a custom vertex program, depth/stencil, MRT or render-to-texture,
 * so it cannot grow into the real pipeline.
 */
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#if !TARGET_OS_IPHONE
#  import <AppKit/AppKit.h>
#endif

#include "rsx_commands.h"
#include "rsx_metal_backend.h"
#include "rsx_vertex_fetch.h"
#include "rsx_primitives.h"
#include "rsx_vp_decompiler.h"
#include "rsx_fp_decompiler.h"
#include "rsx_shader_msl.h"
#include "rsx_texture_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */

static id<MTLDevice>       s_dev;
static id<MTLCommandQueue> s_queue;
static CAMetalLayer*       s_layer;      /* windowed only   */
static id<MTLTexture>      s_offscreen;  /* headless only   */
static id<MTLTexture>      s_depth;      /* depth + stencil, both surfaces  */
static NSWindow*           s_window;
static int                 s_headless;
static int                 s_closed;
static int                 s_ready;
static u32                 s_width  = 1280;
static u32                 s_height = 720;

/* ---- vertex layout ----------------------------------------------------------
 * Every draw carries all 16 RSX vertex attributes as float4, fetched on the
 * CPU by rsx_fetch_attrib: 256 bytes per vertex, attribute i at offset i*16.
 * That is the layout a decompiled vertex program expects (ATTRn arrives at
 * [[attribute(n)]]), and the built-in shader reads slots 0, 3 and 8 of the
 * same layout, so one staging buffer and one vertex descriptor serve both.
 * --------------------------------------------------------------------------*/

#define MTL_ATTRIBS        16
#define MTL_MAX_RECORDS    4096
#define MTL_MAX_VERTS      (256u * 1024u)
#define MTL_VB_INDEX       30      /* buffer(0) and buffer(1) are the constant blocks */

typedef struct { float a[MTL_ATTRIBS][4]; } MtlVertex;

/* ---- the frame's record stream ----------------------------------------------
 * RSX work arrives while the guest builds its frame; the flip comes later. So
 * draws are recorded with their vertices already fetched out of guest memory,
 * then replayed at present time. This mirrors the D3D12 backend's
 * D3D12DrawRecord / render_frame split.
 *
 * Clears are records in the same ordered stream, because a clear in the middle
 * of a frame is something a title really does -- it draws the world, clears
 * depth, and draws the first-person weapon over it. Replaying every draw into
 * one pass that clears once at the top loses that clear entirely. So a clear
 * record ends the open render pass and starts a new one whose flagged
 * attachments load-action Clear and whose others load-action Load.
 *
 * Render targets hang off the same stream: a target change is one more reason
 * to end the pass and open the next one somewhere else.
 * --------------------------------------------------------------------------*/

/* NV4097_CLEAR_SURFACE flags: 0x01 depth, 0x02 stencil, and one bit per colour
 * channel in 0xF0 (nouveau's NV30_3D_CLEAR_BUFFERS layout). A title clears the
 * four colour channels together, so 0xF0 is treated as one colour bit here,
 * exactly as the old clear callback did. */
#define MTL_CLEAR_DEPTH    0x01u
#define MTL_CLEAR_STENCIL  0x02u
#define MTL_CLEAR_COLOR    0xF0u

/* Which surfaces a record is aimed at, latched when the record is made because
 * the guest retargets several times inside a frame and present replays it long
 * after the registers moved on. Every record carries one, draws and clears
 * alike: a clear belongs to a surface as much as a draw does. */
typedef struct {
    u32 off;                  /* 0 = a display buffer (the drawable); else the
                               * raw RSX offset an offscreen surface is keyed by */
    u32 mrt[3];               /* colour targets B, C, D, 0 = none  */
    u32 w, h;                 /* the surface's size at record time */
    u32 fmt;                  /* SET_SURFACE_FORMAT                */
    u32 zeta_off;             /* SET_SURFACE_ZETA_OFFSET           */
    /* The guest viewport rect in target pixels. Only draws use it; a clear is
     * a load action over the whole attachment. */
    u32 vp_x, vp_y, vp_w, vp_h;
} MtlTarget;

typedef struct {
    u32 base;        /* first vertex in s_verts               */
    u32 count;       /* vertex count after primitive expansion */
    MTLPrimitiveType topology;
    int blend_enable;
    u32 blend_sfactor, blend_dfactor, blend_equation;
    u32 color_mask;  /* NV4097_SET_COLOR_MASK word              */
    MTLCullMode cull;
    MTLWinding  winding;
    int ds_idx;      /* slot in s_ds_cache, or -1 for no test / no writes */
    u32 stencil_ref; /* SET_STENCIL_FUNC_REF; encoder state, not pipeline */
    /* Guest programs: cache slots, or -1 when the draw uses the built-in
     * shader. Slots are stable for the whole frame (see s_caches_full). */
    int vs_idx, fs_idx;
    u32 vp_cb_off;            /* VPConst block in s_cb                  */
    u32 fp_cb_off, fp_cb_len; /* PSConstants block in s_cb              */
    /* Per texture unit: texture and sampler cache slots, or -1 for the
     * zero texture / default sampler. Guest-program draws only. */
    int tex[RSX_MAX_TEXTURES];
    int samp[RSX_MAX_TEXTURES];
    /* The same, for the four vertex-texture units a transform program's TXL
     * samples. They share the caches with the fragment units. */
    int vtex[RSX_MAX_VERTEX_TEXTURES];
    int vsamp[RSX_MAX_VERTEX_TEXTURES];
    /* Render-to-texture: the surface-view slot a unit samples, -1 = none. It
     * takes precedence over tex[], which is the guest-memory upload. */
    int tex_rt[RSX_MAX_TEXTURES];
    MtlTarget rt;
    float mvp[16];   /* built-in path: 4 rows of the RSX vertex-constant matrix */
} MtlDraw;

/* One NV4097_CLEAR_SURFACE, with the values rsx_commands.c decoded out of
 * SET_COLOR_CLEAR_VALUE and SET_ZSTENCIL_CLEAR_VALUE. */
typedef struct {
    u32   flags;     /* CLEAR_SURFACE mask                     */
    u32   color;     /* ARGB8888                               */
    float depth;     /* [0,1]                                  */
    u8    stencil;
    MtlTarget rt;
} MtlClear;

typedef enum { MTL_REC_DRAW, MTL_REC_CLEAR } MtlRecordKind;

typedef struct {
    MtlRecordKind kind;
    union { MtlDraw draw; MtlClear clear; } u;
} MtlRecord;

static MtlVertex* s_verts;
static u32        s_vert_count;
static MtlRecord  s_records[MTL_MAX_RECORDS];
static u32        s_rec_count;
static u32        s_dropped_records;
static u32        s_guest_draws;       /* draws through guest programs this frame */
static u32        s_last_guest_draws;  /* ... in the last presented frame        */

static id<MTLLibrary> s_shader_lib;    /* built-in fixed-function shaders */

/* ---- per-draw constant blocks -----------------------------------------------
 * VPConst is the 512 float4 transform constants plus the viewport epilogue's
 * posscale/posoffset (8224 bytes); PSConstants is the fragment program's
 * inline constants plus fp_alpha. Constants change between draws in a frame,
 * so each draw snapshots its own blocks -- the D3D12 backend keeps a per-draw
 * VP_CB_STRIDE slot for the same reason. Both blocks are past the 4 KB limit
 * of setVertexBytes:, and Metal wants setVertexBuffer:offset: 256-aligned,
 * so they are appended here at record time and uploaded once per frame in
 * one buffer, exactly like the vertices.
 * --------------------------------------------------------------------------*/

#define MTL_VP_CB_BYTES  ((RSX_MAX_VERTEX_CONSTANTS + 2) * 16)
#define MTL_CB_ALIGN     256u
#define MTL_CB_MAX       (256u << 20)
static u8* s_cb;
static u32 s_cb_used, s_cb_cap;

/* ---- guest shader caches ----------------------------------------------------
 * Vertex programs are keyed on their microcode bytes, fragment programs on
 * rsx_fp_structural_hash (the code, not the inline constants, which live in
 * PSConstants and change per draw) plus the export width and the alpha test
 * that gets patched into the source. A slot with a nil function is a program
 * that failed to translate: it is remembered so the draw does not retry the
 * whole pipeline on every frame.
 *
 * Draw records hold slot indices, so slots must not move while a frame is
 * being recorded. When a cache fills, translation stops for the rest of the
 * frame (those draws use the built-in shader) and every cache is emptied
 * after the frame presents, rather than evicting under a live record.
 * --------------------------------------------------------------------------*/

#define MTL_VP_CACHE     1024
#define MTL_FP_CACHE     2048
#define MTL_PSO_CACHE    2048
#define MTL_FP_MAX_BYTES 4096u     /* bound on a fragment program, as D3D12 */

typedef struct { u32 hash; id<MTLFunction> fn; } MtlVpEntry;
typedef struct { u64 key;  id<MTLFunction> fn; u32 nconst; } MtlFpEntry;
/* A pipeline state is bound inside a render pass, so the pass's colour format
 * and target count are part of what identifies it: the same shaders against a
 * float surface and against the drawable are two pipelines. The depth format
 * is not, because every pass binds MTL_DEPTH_FORMAT. */
typedef struct {
    int vs, fs; u32 blend; u32 cmask;
    u32 color_pf, nrt;
} MtlPsoKey;
typedef struct { MtlPsoKey key; id<MTLRenderPipelineState> pso; } MtlPsoEntry;

static MtlVpEntry  s_vp_cache[MTL_VP_CACHE];
static MtlFpEntry  s_fp_cache[MTL_FP_CACHE];
static MtlPsoEntry s_pso_cache[MTL_PSO_CACHE];
static u32 s_vp_count, s_fp_count, s_pso_count;
static int s_caches_full;
static int s_guest_shaders;            /* translator present and not disabled */
static rsx_fp_constant_block s_fp_consts;

/* The decompilers' HLSL and the MSL made from it. The VP decompiler builds
 * bodies up to 192 KB, and the legalized MSL runs longer than its HLSL. */
static char s_hlsl[512 * 1024];
static char s_msl[512 * 1024];
static char s_log[8192];

/* What a fragment program samples before guest textures are bound (task:
 * textures). A D3D12 null SRV reads zero; Metal validation rejects a nil
 * texture on use, so zero is a real 1x1 texture here. */
static id<MTLTexture>      s_null_tex;
static id<MTLSamplerState> s_default_sampler;
/* What a draw gets when the depth/stencil cache is full: no test, no writes. */
static id<MTLDepthStencilState> s_ds_default;

/* ---- guest textures ---------------------------------------------------------
 * Keyed on where the bytes are and what the register says they are, plus a
 * sparse checksum of the bytes themselves, because titles animate textures
 * in place (the D3D12 backend's tex_csum exists for the same reason). A
 * changed checksum gets a fresh MTLTexture rather than an in-place write: a
 * frame in flight may still be sampling the old one. The checksum covers the
 * whole mip chain, not just level 0, so a title animating a lower level is
 * noticed. Same slot-stability rule as the shader caches. Samplers are keyed
 * on the wrap, filter and LOD registers.
 * --------------------------------------------------------------------------*/

#define MTL_TEX_CACHE   1024
#define MTL_SAMP_CACHE  64

typedef struct {
    u32 ea, w, h, format, control1, control3, csum;
    id<MTLTexture> tex;
} MtlTexEntry;
typedef struct { u64 key; id<MTLSamplerState> samp; } MtlSampEntry;

static MtlTexEntry  s_tex_cache[MTL_TEX_CACHE];
static MtlSampEntry s_samp_cache[MTL_SAMP_CACHE];
static u32 s_tex_count, s_samp_count;
static u8* s_tex_staging;
static u32 s_tex_staging_cap;

/* Frames the CPU may run ahead of the GPU.
 *
 * Presenting used to end in -waitUntilCompleted unconditionally, which is the
 * one pattern where a translation layer or a driver cannot hide its encoding
 * work: the CPU stalls on every frame instead of preparing the next one.
 * Dolphin measured exactly this shape (their bounding-box path) as the single
 * largest CPU-side penalty in their Metal/MoltenVK comparison.
 *
 * Windowed presents now bound in-flight frames with a semaphore released from
 * the command buffer's completion handler, so the CPU keeps working while the
 * GPU drains. Headless still waits, because the readback has to observe a
 * finished frame -- there the stall is the point. */
#define MTL_MAX_INFLIGHT 3
static dispatch_semaphore_t s_inflight;

/* RSX clear colour, ARGB8888, as written by NV4097_SET_COLOR_CLEAR_VALUE. */
static u32 s_clear_argb = 0xFF000000u;
static u32 s_last_present_bgra;

/* ---- rsx_backend vtable -------------------------------------------------- */

/* The draw callbacks are not handed the state, so it is latched here. Every
 * state setter caches it; set_vertex_attribs in particular always fires before
 * a draw. The D3D12 backend does the same via s_d3d.current_rsx_state. */
static const rsx_state* s_state;

/* Which surfaces the live state is aimed at; see the guest surfaces section. */
static void target_for(const rsx_state* st, MtlTarget* t);

static void mtl_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud;
    /* The debug hook reports the last colour the guest asked for, whether or
     * not the record stream had room for the clear itself. */
    if (flags & MTL_CLEAR_COLOR) s_clear_argb = color;
    if (!s_ready) return;
    if (!(flags & (MTL_CLEAR_COLOR | MTL_CLEAR_DEPTH | MTL_CLEAR_STENCIL))) return;
    if (s_rec_count >= MTL_MAX_RECORDS) { s_dropped_records++; return; }

    MtlRecord* r = &s_records[s_rec_count++];
    r->kind = MTL_REC_CLEAR;
    r->u.clear.flags   = flags;
    r->u.clear.color   = color;
    r->u.clear.depth   = depth;
    r->u.clear.stencil = stencil;
    /* A clear belongs to a surface as much as a draw does: a title clears its
     * shadow map, renders into it, then clears the display. */
    target_for(s_state, &r->u.clear.rt);
}

static int create_depth(void);

/* Is this raw RSX offset one of the buffers cellGcmSetDisplayBuffer
 * registered? The D3D12 backend takes the same private hook to tell a draw
 * aimed at the scanout from one aimed at an offscreen surface. */
extern int cellGcmOffsetIsDisplay(u32 offset);

static void mtl_set_render_target(void* ud, const rsx_state* state)
{
    (void)ud;
    if (state) s_state = state;
    if (!state) return;
    /* The surface clip is the size of whatever surface is bound, so only a
     * display buffer's clip says anything about the window: a title that
     * renders a 256x256 effect into an offscreen surface would otherwise
     * shrink the drawable to it, and never grow it back. */
    if (!cellGcmOffsetIsDisplay(state->surface_color_offset[
            state->color_target == CELL_GCM_SURFACE_TARGET_1 ? 1 : 0]))
        return;
    u32 w = state->surface_clip_w, h = state->surface_clip_h;
    if (!w || !h || (w == s_width && h == s_height)) return;
    s_width = w; s_height = h;
    if (s_layer) s_layer.drawableSize = CGSizeMake((CGFloat)w, (CGFloat)h);
    if (s_depth && create_depth() != 0)
        fprintf(stderr, "[RSX metal] depth/stencil resize to %ux%u failed; "
                        "the old buffer stays bound\n", w, h);
}

static void mtl_latch_state(void* ud, const rsx_state* state)
{ (void)ud; if (state) s_state = state; }

static void mtl_draw_arrays(void*, u32, u32, u32);
static void mtl_draw_indexed(void*, u32, u32, u32);

static rsx_backend s_backend_vtable = {
    .userdata           = NULL,
    .clear              = mtl_clear,
    .set_render_target  = mtl_set_render_target,
    .set_vertex_attribs = mtl_latch_state,
    .set_blend          = mtl_latch_state,
    .set_depth_stencil  = mtl_latch_state,
    .set_viewport       = mtl_latch_state,
    .set_color_mask     = mtl_latch_state,
    .set_alpha_test     = mtl_latch_state,
    /* Programs are resolved when the draw is recorded, from the live state:
     * SET_SHADER_PROGRAM, the transform program words and SHADER_CONTROL
     * all land there before BEGIN_END fires this. */
    .set_shader         = mtl_latch_state,
    .draw_arrays        = mtl_draw_arrays,
    .draw_indexed       = mtl_draw_indexed,
};

/* ---- helpers ------------------------------------------------------------- */

static MTLClearColor clear_color_from_argb(u32 argb)
{
    /* ARGB8888 -> normalised RGBA. sRGB conversion is deliberately skipped:
     * the D3D12 backend treats the guest value as raw UNORM too, so both
     * backends agree pixel-for-pixel. */
    const double a = (double)((argb >> 24) & 0xFF) / 255.0;
    const double r = (double)((argb >> 16) & 0xFF) / 255.0;
    const double g = (double)((argb >>  8) & 0xFF) / 255.0;
    const double b = (double)( argb        & 0xFF) / 255.0;
    return MTLClearColorMake(r, g, b, a);
}

static u32 fnv1a32(const u8* p, u32 n)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static u64 fnv1a64(const void* data, u32 n, u64 h)
{
    const u8* p = (const u8*)data;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

/* One depth/stencil texture for the display target, sized like the colour
 * attachment, shared by every pass the frame opens -- the D3D12 backend shares
 * one depth buffer the same way, because a PS3 title typically uses a single
 * zeta surface. Zeta offsets and per-target depth belong with render targets.
 *
 * D24S8 is not a format Apple GPUs have, so the depth is 32-bit float. Both
 * hold the guest's [0,1] window z, with more precision than the RSX's 24 bits
 * rather than less. The texture is never read back, so it is private.
 *
 * Assigned only on success: a failed resize keeps the old texture bound rather
 * than leaving passes and pipelines disagreeing about whether depth exists. */
#define MTL_DEPTH_FORMAT   MTLPixelFormatDepth32Float_Stencil8

static int create_depth(void)
{
    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTL_DEPTH_FORMAT
                                                           width:s_width
                                                          height:s_height
                                                       mipmapped:NO];
    td.usage       = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> t = [s_dev newTextureWithDescriptor:td];
    if (!t) return -1;
    s_depth = t;
    return 0;
}

static int create_offscreen(void)
{
    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:s_width
                                                          height:s_height
                                                       mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    s_offscreen = [s_dev newTextureWithDescriptor:td];
    return s_offscreen ? 0 : -1;
}

#if !TARGET_OS_IPHONE
static int create_window_impl(const char* title)
{
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    NSRect frame = NSMakeRect(0, 0, (CGFloat)s_width, (CGFloat)s_height);
    s_window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    if (!s_window) return -1;

    [s_window setTitle:[NSString stringWithUTF8String:(title ? title : "ps3recomp")]];
    [s_window center];

    s_layer = [CAMetalLayer layer];
    s_layer.device          = s_dev;
    s_layer.pixelFormat     = MTLPixelFormatBGRA8Unorm;
    s_layer.framebufferOnly = YES;
    s_layer.drawableSize    = CGSizeMake((CGFloat)s_width, (CGFloat)s_height);

    NSView* view = [s_window contentView];
    [view setWantsLayer:YES];
    [view setLayer:s_layer];

    [s_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    return 0;
}

static int create_window(const char* title)
{
    if ([NSThread isMainThread])
        return create_window_impl(title);
    __block int result = 0;
    NSString* t = title ? [NSString stringWithUTF8String:title] : nil;
    dispatch_sync(dispatch_get_main_queue(), ^{
        result = create_window_impl(t ? [t UTF8String] : NULL);
    });
    return result;
}
#endif

static int create_placeholders(void)
{
    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:1 height:1 mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    s_null_tex = [s_dev newTextureWithDescriptor:td];
    if (!s_null_tex) return -1;
    u32 zero = 0;
    [s_null_tex replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0
                    withBytes:&zero bytesPerRow:4];

    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter = MTLSamplerMinMagFilterLinear;
    sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    s_default_sampler = [s_dev newSamplerStateWithDescriptor:sd];
    if (!s_default_sampler) return -1;

    MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionAlways;
    dd.depthWriteEnabled    = NO;
    s_ds_default = [s_dev newDepthStencilStateWithDescriptor:dd];
    return s_ds_default ? 0 : -1;
}

/* ---- guest vertex fetch --------------------------------------------------
 * rsx_vertex_fetch.c holds the one definition of how a vertex comes out of
 * guest memory (this file used to carry a drifted copy). Every attribute is
 * fetched, used or not: a disabled array yields the constant attribute
 * register, which is what the hardware feeds a program that reads it.
 * --------------------------------------------------------------------------*/

static void fetch_vertex(const rsx_state* st, u32 vi, MtlVertex* out)
{
    for (int i = 0; i < MTL_ATTRIBS; i++)
        rsx_fetch_attrib(st, i, vi, out->a[i]);
}

/* ---- primitive conversion -------------------------------------------------
 * Metal, like D3D12, has no quads, quad strips, triangle fans or polygons, so
 * those are expanded to triangle lists on the CPU while the vertices are being
 * fetched. Everything else maps directly.
 * --------------------------------------------------------------------------*/

/* Which primitives need CPU expansion, and what they end up drawn as, both
 * come from rsx_primitives.h now -- this file used to answer the first
 * question with its own list, which disagreed with the one in that header
 * about LINE_LOOP and POLYGON. Only the Metal enum mapping stays here. */
static MTLPrimitiveType topo_to_metal(rsx_topology t)
{
    switch (t) {
    case RSX_TOPOLOGY_POINTS:         return MTLPrimitiveTypePoint;
    case RSX_TOPOLOGY_LINES:          return MTLPrimitiveTypeLine;
    case RSX_TOPOLOGY_LINE_STRIP:     return MTLPrimitiveTypeLineStrip;
    case RSX_TOPOLOGY_TRIANGLE_STRIP: return MTLPrimitiveTypeTriangleStrip;
    default:                          return MTLPrimitiveTypeTriangle;
    }
}

/* Emit `count` vertices starting at `first`, expanding fans/quads/polygons.
 * `resolve` maps a sequence position to a guest vertex index, so the same code
 * serves both array and indexed draws. Returns vertices written. */
typedef u32 (*IndexResolver)(const rsx_state*, u32 /*seq*/, void* /*ctx*/);

static u32 emit_vertices(const rsx_state* st, u32 prim, u32 count,
                         IndexResolver resolve, void* ctx)
{
    u32 wrote = 0;
    #define PUSH(seq) do {                                                     \
        if (s_vert_count >= MTL_MAX_VERTS) return wrote;                       \
        fetch_vertex(st, resolve(st, (seq), ctx), &s_verts[s_vert_count++]);   \
        wrote++;                                                               \
    } while (0)

    if (prim == RSX_PRIMITIVE_QUADS) {
        for (u32 q = 0; q + 3 < count; q += 4) {
            PUSH(q); PUSH(q+1); PUSH(q+2);
            PUSH(q); PUSH(q+2); PUSH(q+3);
        }
    } else if (prim == RSX_PRIMITIVE_QUAD_STRIP) {
        for (u32 q = 0; q + 3 < count; q += 2) {
            PUSH(q); PUSH(q+1); PUSH(q+2);
            PUSH(q+1); PUSH(q+3); PUSH(q+2);
        }
    } else if (prim == RSX_PRIMITIVE_TRIANGLE_FAN || prim == RSX_PRIMITIVE_POLYGON) {
        for (u32 t = 1; t + 1 < count; t++) { PUSH(0); PUSH(t); PUSH(t+1); }
    } else {
        for (u32 v = 0; v < count; v++) PUSH(v);
    }
    #undef PUSH
    return wrote;
}

static u32 resolve_linear(const rsx_state* st, u32 seq, void* ctx)
{
    (void)st; return *(u32*)ctx + seq;
}

static u32 resolve_indexed(const rsx_state* st, u32 seq, void* ctx)
{
    u32 base = *(u32*)ctx;
    /* index_array_offset bits [7:4] select the type: 0 = u32, 1 = u16. */
    int u16type = ((st->index_array_offset >> 4) & 0xFu) == 1;
    u32 off = st->index_array_offset & ~0xFFu;
    u32 ea  = cellGcmResolveOffset(off) + (base + seq) * (u16type ? 2u : 4u);
    if (!vm_base) return 0;
    const u8* p = vm_base + ea;
    return u16type ? (u32)((p[0] << 8) | p[1])
                   : (u32)((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

/* ---- guest shader translation -------------------------------------------- */

/* PS3RECOMP_METAL_SHADER_DUMP=<dir>: write every translated program's HLSL and
 * MSL there, named by the cache key. The first thing to look at when a title
 * draws wrong. */
static void dump_shader(const char* name, const char* text)
{
    static const char* dir = (const char*)1;
    if (dir == (const char*)1) dir = getenv("PS3RECOMP_METAL_SHADER_DUMP");
    if (!dir || !*dir) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE* f = fopen(path, "w");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

static MTLCompileOptions* guest_compile_options(void)
{
    MTLCompileOptions* o = [MTLCompileOptions new];
    /* IEEE comparisons. The decompilers flush a NaN result to zero with
     * `x == x`, and the alpha test compares with isunordered; Metal's default
     * fast math is free to fold both away. */
#if defined(MAC_OS_VERSION_15_0) && MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_VERSION_15_0
    if (@available(macOS 15.0, *)) {
        o.mathMode = MTLMathModeSafe;
        return o;
    }
#endif
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    o.fastMathEnabled = NO;
#pragma clang diagnostic pop
    return o;
}

/* MSL source -> its `main0` function, or nil with the compiler's diagnostic
 * on stderr. */
static id<MTLFunction> compile_guest_function(const char* msl, const char* what)
{
    NSError* err = nil;
    id<MTLLibrary> lib = [s_dev newLibraryWithSource:[NSString stringWithUTF8String:msl]
                                             options:guest_compile_options()
                                               error:&err];
    if (!lib) {
        fprintf(stderr, "[RSX metal] %s: MSL compile failed: %s\n", what,
                [[err localizedDescription] UTF8String]);
        return nil;
    }
    id<MTLFunction> fn = [lib newFunctionWithName:@"main0"];
    if (!fn) fprintf(stderr, "[RSX metal] %s: no main0 in the compiled MSL\n", what);
    return fn;
}

/* The vertex program the draw would run: a slot in s_vp_cache, or -1 when
 * there is none, it could not be translated, or the cache is full. Follows
 * the D3D12 backend's vp_get_vs: the program starts at the instruction
 * SET_TRANSFORM_PROGRAM_START selects (the store holds every resident
 * program), falling back to 0 when that lies past what was uploaded. */
static int vp_slot_for(const rsx_state* st, u32 vtex_mask)
{
    if (st->vp_ucode_bytes < 16) return -1;
    u32 vstart = st->transform_program_start * 16u;
    if (vstart >= st->vp_ucode_bytes) vstart = 0;
    const u8* uc = st->vp_ucode + vstart;
    const u32 avail = st->vp_ucode_bytes - vstart;
    /* Hash the program's own bytes, to its end bit, not everything after it
     * in the store: another program uploaded behind it must not miss. */
    const u32 instrs = rsx_vp_program_size_instrs(uc, avail);
    const u32 len = instrs ? instrs * 16u : avail;
    /* The bound vertex-texture units are part of the key: an unbound unit's
     * TXL decompiles to a defined zero and declares no sampler at all, so the
     * same microcode with a different mask is a different program. */
    const u32 hash = fnv1a32(uc, len) ^ (vtex_mask * 0x9E3779B9u);

    for (u32 i = 0; i < s_vp_count; i++)
        if (s_vp_cache[i].hash == hash) return s_vp_cache[i].fn ? (int)i : -1;
    if (s_vp_count >= MTL_VP_CACHE) { s_caches_full = 1; return -1; }

    char name[64];
    snprintf(name, sizeof name, "vertex program %08X", hash);
    id<MTLFunction> fn = nil;
    const int ni = rsx_vp_decompile_ex(uc, len, vtex_mask, s_hlsl, sizeof s_hlsl);
    if (ni <= 0) {
        fprintf(stderr, "[RSX metal] %s: decompile failed (%d)\n", name, ni);
    } else {
        snprintf(name, sizeof name, "vp_%08x.hlsl", hash); dump_shader(name, s_hlsl);
        if (rsx_hlsl_to_msl(s_hlsl, RSX_SHADER_STAGE_VERTEX, s_msl, sizeof s_msl,
                            s_log, sizeof s_log) != 0) {
            fprintf(stderr, "[RSX metal] vertex program %08X: %s\n", hash, s_log);
        } else {
            snprintf(name, sizeof name, "vp_%08x.msl", hash); dump_shader(name, s_msl);
            snprintf(name, sizeof name, "vertex program %08X", hash);
            fn = compile_guest_function(s_msl, name);
        }
    }
    { static int n = 0; if (n++ < 32)
        fprintf(stderr, "[RSX metal] vertex program %08X: %d instrs -> %s\n",
                hash, ni, fn ? "ok" : "FAILED (built-in shader instead)"); }

    const int slot = (int)s_vp_count++;
    s_vp_cache[slot].hash = hash;
    s_vp_cache[slot].fn   = fn;
    return fn ? slot : -1;
}

/* Which texture units are cube maps, and which vertex-texture units are
 * bound. Defined with the rest of the texture code below; declared here
 * because the guest programs are compiled against them. */
static u32 cube_mask_for(const rsx_state* st);
static u32 vtex_mask_for(const rsx_state* st);

/* The fragment program the draw would run, resolved as the D3D12 backend's
 * vp_get_fp_pso does: SET_SHADER_PROGRAM's low bits are the location (1 =
 * local, 2 = main), the rest the offset. Keyed on the program's structure
 * (its inline constants are hoisted into PSConstants, so the compiled shader
 * is invariant under constant changes), the export width from
 * SHADER_CONTROL, and the alpha test patched into the source. Returns the
 * slot or -1; `*uc` receives the program bytes for the constant collection.
 * `cube_mask` says which units are cube maps: those are declared TextureCube
 * and sampled with a direction rather than a 2D coordinate, so it changes the
 * compiled program and belongs in the key -- the D3D12 backend's PSO key
 * carries cube_mask for the same reason. */
static int fp_slot_for(const rsx_state* st, const u8** uc, u32 cube_mask)
{
    if (!vm_base || st->shader_program == 0) return -1;
    const u32 off = cellGcmResolveLocated((st->shader_program & 3u) == 1u,
                                          st->shader_program & ~3u);
    if (off == 0xFFFFFFFFu) return -1;
    *uc = vm_base + off;

    u64 key = rsx_fp_structural_hash(*uc, MTL_FP_MAX_BYTES, 1469598103934665603ull);
    if (!key) return -1;                    /* malformed or unterminated */
    const u32 ctrl      = st->shader_control;
    const u32 ctrl_key  = ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS;
    const u32 alpha_en  = (st->alpha_test_enable && st->alpha_func != 0x0207u) ? 1u : 0u;
    const u32 alpha_fn  = alpha_en ? st->alpha_func : 0u;
    key = fnv1a64(&ctrl_key, sizeof ctrl_key, key);
    key = fnv1a64(&alpha_en, sizeof alpha_en, key);
    key = fnv1a64(&alpha_fn, sizeof alpha_fn, key);
    key = fnv1a64(&cube_mask, sizeof cube_mask, key);

    for (u32 i = 0; i < s_fp_count; i++)
        if (s_fp_cache[i].key == key) return s_fp_cache[i].fn ? (int)i : -1;
    if (s_fp_count >= MTL_FP_CACHE) { s_caches_full = 1; return -1; }

    char name[64];
    id<MTLFunction> fn = nil;
    u32 nconst = 0;
    int ni = rsx_fp_decompile_buffered_ex(*uc, MTL_FP_MAX_BYTES, ctrl, cube_mask,
                                          s_hlsl, sizeof s_hlsl, &nconst);
    if (ni > 0 && alpha_en &&
        rsx_fp_apply_alpha_test_buffered(s_hlsl, sizeof s_hlsl, st->alpha_func) < 0)
        ni = -1;
    if (ni <= 0) {
        fprintf(stderr, "[RSX metal] fragment program %016llX: decompile failed (%d)\n",
                (unsigned long long)key, ni);
    } else {
        snprintf(name, sizeof name, "fp_%016llx.hlsl", (unsigned long long)key);
        dump_shader(name, s_hlsl);
        if (rsx_hlsl_to_msl(s_hlsl, RSX_SHADER_STAGE_FRAGMENT, s_msl, sizeof s_msl,
                            s_log, sizeof s_log) != 0) {
            fprintf(stderr, "[RSX metal] fragment program %016llX: %s\n",
                    (unsigned long long)key, s_log);
        } else {
            snprintf(name, sizeof name, "fp_%016llx.msl", (unsigned long long)key);
            dump_shader(name, s_msl);
            snprintf(name, sizeof name, "fragment program %016llX", (unsigned long long)key);
            fn = compile_guest_function(s_msl, name);
        }
    }
    { static int n = 0; if (n++ < 32)
        fprintf(stderr, "[RSX metal] fragment program %016llX: %d instrs, %u constants -> %s\n",
                (unsigned long long)key, ni, nconst,
                fn ? "ok" : "FAILED (built-in shader instead)"); }

    const int slot = (int)s_fp_count++;
    s_fp_cache[slot].key    = key;
    s_fp_cache[slot].fn     = fn;
    s_fp_cache[slot].nconst = nconst;
    return fn ? slot : -1;
}

/* Reserve `bytes` of constant staging at a 256-byte boundary. */
static int cb_alloc(u32 bytes, u32* off)
{
    const u32 start = (s_cb_used + MTL_CB_ALIGN - 1u) & ~(MTL_CB_ALIGN - 1u);
    if (start + bytes > s_cb_cap) {
        u32 cap = s_cb_cap ? s_cb_cap : (4u << 20);
        while (start + bytes > cap) {
            if (cap >= MTL_CB_MAX) return 0;
            cap *= 2u;
        }
        u8* n = (u8*)realloc(s_cb, cap);
        if (!n) return 0;
        s_cb = n; s_cb_cap = cap;
    }
    *off = start;
    s_cb_used = start + bytes;
    return 1;
}

/* Resolve both guest programs for a draw and snapshot their constant blocks.
 * Returns 1 with the record's shader fields filled in, 0 when the draw must
 * use the built-in shader (no vertex program loaded, a translation failure,
 * or a full cache). Both programs or neither: a guest vertex program's
 * outputs do not link with the built-in fragment shader's inputs. */
static int guest_programs_for(const rsx_state* st, MtlDraw* d)
{
    const int vs = vp_slot_for(st, vtex_mask_for(st));
    if (vs < 0) return 0;
    const u8* fp_uc = NULL;
    const int fs = fp_slot_for(st, &fp_uc, cube_mask_for(st));
    if (fs < 0) return 0;

    /* PSConstants: the program's inline constants as host-order bit patterns,
     * one float4 per slot, then fp_alpha. The count must match what the
     * shader was compiled against. */
    if (rsx_fp_collect_constants(fp_uc, MTL_FP_MAX_BYTES, &s_fp_consts) < 0 ||
        s_fp_consts.count != s_fp_cache[fs].nconst)
        return 0;
    const u32 nslots = s_fp_consts.count ? s_fp_consts.count : 1u;
    const u32 fp_len = (nslots + 1u) * 16u;

    u32 vp_off, fp_off;
    if (!cb_alloc(MTL_VP_CB_BYTES, &vp_off)) return 0;
    if (!cb_alloc(fp_len, &fp_off)) return 0;

    /* VPConst: the 512 transform constants, then the viewport epilogue
     * (see the D3D12 backend's vp_record_cb): x/y identity, and the z lane
     * remaps GL clip z to [0,1] when the guest programmed a z scale. Metal
     * clip space matches D3D's here, so the mapping transfers unchanged. */
    u8* vp = s_cb + vp_off;
    memcpy(vp, st->vertex_constants, RSX_MAX_VERTEX_CONSTANTS * 16);
    float* vpx = (float*)(vp + RSX_MAX_VERTEX_CONSTANTS * 16);
    vpx[0] = vpx[1] = vpx[3] = 1.0f;
    vpx[4] = vpx[5] = vpx[7] = 0.0f;
    if (st->viewport_scale[2] != 0.0f) { vpx[2] = st->viewport_scale[2]; vpx[6] = st->viewport_offset[2]; }
    else                                { vpx[2] = 1.0f;                  vpx[6] = 0.0f; }

    u8* fp = s_cb + fp_off;
    memset(fp, 0, fp_len);
    if (s_fp_consts.count)
        memcpy(fp, s_fp_consts.values, s_fp_consts.count * 16u);
    float* alpha = (float*)(fp + nslots * 16u);
    alpha[0] = rsx_fp_alpha_ref(st->alpha_ref, st->surface_format & 0x1Fu);
    alpha[1] = alpha[2] = alpha[3] = 0.0f;

    d->vs_idx = vs; d->fs_idx = fs;
    d->vp_cb_off = vp_off;
    d->fp_cb_off = fp_off; d->fp_cb_len = fp_len;
    return 1;
}

/* ---- guest textures ------------------------------------------------------ */

static MTLPixelFormat texfmt_to_metal(rsx_texfmt f)
{
    switch (f) {
    case RSX_TEXFMT_R8G8:     return MTLPixelFormatRG8Unorm;
    case RSX_TEXFMT_R8G8B8A8: return MTLPixelFormatRGBA8Unorm;
    case RSX_TEXFMT_BC1:      return MTLPixelFormatBC1_RGBA;
    case RSX_TEXFMT_BC2:      return MTLPixelFormatBC2_RGBA;
    case RSX_TEXFMT_BC3:      return MTLPixelFormatBC3_RGBA;
    case RSX_TEXFMT_R16:      return MTLPixelFormatR16Unorm;
    case RSX_TEXFMT_R16G16:   return MTLPixelFormatRG16Unorm;
    case RSX_TEXFMT_R16G16F:  return MTLPixelFormatRG16Float;
    case RSX_TEXFMT_R16G16B16A16F: return MTLPixelFormatRGBA16Float;
    case RSX_TEXFMT_R32F:     return MTLPixelFormatR32Float;
    case RSX_TEXFMT_R32G32B32A32F: return MTLPixelFormatRGBA32Float;
    default:                  return MTLPixelFormatR8Unorm;
    }
}

/* A crossbar selector (0..3 = the uploaded R,G,B,A; RSX_REMAP_ZERO/ONE) as
 * a Metal swizzle source. */
static MTLTextureSwizzle swizzle_sel(u8 sel)
{
    switch (sel) {
    case 0: return MTLTextureSwizzleRed;
    case 1: return MTLTextureSwizzleGreen;
    case 2: return MTLTextureSwizzleBlue;
    case 3: return MTLTextureSwizzleAlpha;
    case RSX_REMAP_ONE: return MTLTextureSwizzleOne;
    default: return MTLTextureSwizzleZero;
    }
}

/* Sparse FNV-1a over a texture's source bytes: enough to notice an animated
 * surface changing, cheap enough to run on every draw that binds it. */
static u32 tex_csum(const u8* base, u32 nbytes)
{
    u32 h = 2166136261u;
    const u32 step = nbytes > 4096u ? nbytes / 1024u : 4u;
    for (u32 i = 0; i + 3 < nbytes; i += step) {
        u32 w32; memcpy(&w32, base + i, sizeof w32);
        h ^= w32; h *= 16777619u;
    }
    return h;
}

/* A unit is a cube map when SET_TEXTURE_FORMAT bit 2 says so. A cube face is
 * square by construction, so a non-square image cannot be one. The check
 * lives in one place because two things read it and they must not disagree:
 * the fragment program is compiled against a cube unit's sampler, and a
 * `texturecube` slot filled with a 2D texture is a validation failure, not a
 * wrong pixel. */
static int texture_is_cube(const rsx_texture_state* t)
{
    const u32 w = (t->image_rect >> 16) & 0xFFFFu, h = t->image_rect & 0xFFFFu;
    return (t->format & 4u) && w && w == h;
}

/* The enabled cube units as a bit mask, which is what the fragment program is
 * decompiled against. Built from the registers rather than from the resolved
 * textures, as the D3D12 backend's dr_cube_mask is, so a unit whose bytes
 * could not be uploaded still compiles to the sampler type the register
 * asked for. */
static u32 cube_mask_for(const rsx_state* st)
{
    u32 m = 0;
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++)
        if ((st->textures[u].control0 & 0x80000000u) && texture_is_cube(&st->textures[u]))
            m |= 1u << u;
    return m;
}

/* The enabled vertex-texture units. The vertex program is decompiled against
 * this: an unmasked unit's TXL becomes a defined zero and declares nothing,
 * so a program compiled with the wrong mask samples the wrong thing or reads
 * a slot the backend never bound. */
static u32 vtex_mask_for(const rsx_state* st)
{
    u32 m = 0;
    for (u32 u = 0; u < RSX_MAX_VERTEX_TEXTURES; u++)
        if (st->vertex_textures[u].control0 & 0x80000000u)
            m |= 1u << u;
    return m;
}

/* How many guest bytes a texture occupies: the whole mip chain, times six for
 * a cube map. That is what the checksum has to cover -- a title that animates
 * only a lower level, or only one face, would otherwise keep the stale
 * upload. */
static u32 texture_source_span(u32 fmt, u32 w, u32 h, u32 levels, u32 pitch,
                               int cube)
{
    if (cube) return rsx_texture_cube_face_stride(fmt, w, h, levels, pitch) * 6u;
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 n = rsx_texture_mip_chain(fmt, w, h, levels, pitch, lv);
    if (!n) return 0;
    return lv[n - 1].offset + lv[n - 1].tl.face_bytes;
}

/* Room for one decoded level. Levels shrink as the chain descends, so the
 * buffer is sized once by level 0 and reused. */
static int tex_staging_reserve(u32 bytes)
{
    if (s_tex_staging_cap >= bytes) return 1;
    u8* n = (u8*)realloc(s_tex_staging, bytes);
    if (!n) return 0;
    s_tex_staging = n; s_tex_staging_cap = bytes;
    return 1;
}

/* Decode a guest texture out of guest memory into a new texture whose swizzle
 * applies the TEXTURE_CONTROL1 crossbar. `fmt` is the format byte with its
 * LN/UN flags, `levels` SET_TEXTURE_FORMAT's level count, `pitch`
 * SET_TEXTURE_CONTROL3's row pitch, and `cube` its bit 2. */
static id<MTLTexture> upload_texture(const u8* src, u32 w, u32 h, u32 fmt,
                                     u32 control1, u32 levels, u32 pitch,
                                     int cube)
{
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 nlv = rsx_texture_mip_chain(fmt, w, h, levels, pitch, lv);
    if (!nlv || lv[0].tl.face_bytes == 0) return nil;
    /* Faces are stored face-major, each one a whole mip pyramid rather than a
     * single image, so the stride between them is the pyramid rounded up to
     * 128 bytes. */
    const u32 nfaces = cube ? 6u : 1u;
    const u32 face_stride = cube ? rsx_texture_cube_face_stride(fmt, w, h, levels, pitch) : 0u;

    MTLTextureDescriptor* td = [MTLTextureDescriptor new];
    td.textureType       = cube ? MTLTextureTypeCube : MTLTextureType2D;
    td.pixelFormat       = texfmt_to_metal(lv[0].tl.fmt);
    td.width             = w;
    td.height            = h;
    td.mipmapLevelCount  = nlv;
    td.usage             = MTLTextureUsageShaderRead;
    /* The crossbar's selectors arrive in its own field order A,R,G,B; the
     * D3D12 backend maps them to destR = out[1], destG = out[2],
     * destB = out[3], destA = out[0], and so does this. */
    u8 remap[4];
    rsx_texture_component_remap(control1, fmt & 0x9Fu, remap);
    td.swizzle = MTLTextureSwizzleChannelsMake(swizzle_sel(remap[1]), swizzle_sel(remap[2]),
                                               swizzle_sel(remap[3]), swizzle_sel(remap[0]));
    id<MTLTexture> tex = [s_dev newTextureWithDescriptor:td];
    if (!tex) return nil;

    for (u32 f = 0; f < nfaces; f++) {
        const u8* face = src + (size_t)f * face_stride;
        for (u32 m = 0; m < nlv; m++) {
            const rsx_tex_layout* tl = &lv[m].tl;
            if (!tex_staging_reserve(tl->dst_row_bytes * tl->rows)) return nil;
            /* TEX_RGBA is consulted unconditionally, as the D3D12 backend
             * does: the decode only reads it for the two formats whose bytes
             * are A,R,G,B. */
            rsx_texture_decode(s_tex_staging, tl->dst_row_bytes,
                               face + lv[m].offset, lv[m].w, lv[m].h, tl,
                               rsx_texture_argb_is_rgba());
            /* bytesPerImage is for 3D textures only; a cube face is a slice. */
            [tex replaceRegion:MTLRegionMake2D(0, 0, lv[m].w, lv[m].h)
                   mipmapLevel:m
                         slice:f
                     withBytes:s_tex_staging
                   bytesPerRow:tl->dst_row_bytes
                 bytesPerImage:0];
        }
    }
    return tex;
}

/* The texture a unit samples: a slot in s_tex_cache, or -1 for the zero
 * texture (unit disabled, unresolvable, or a full cache). Resolved as the
 * D3D12 backend's bind_texture does it: the location is SET_TEXTURE_FORMAT's
 * low two bits (1 = local, 2 = main), the format byte its bits [15:8]. */
static int tex_slot_for(const rsx_texture_state* t)
{
    if (!(t->control0 & 0x80000000u) || !vm_base) return -1;
    const u32 w = (t->image_rect >> 16) & 0xFFFFu, h = t->image_rect & 0xFFFFu;
    if (!w || !h || w > 4096u || h > 4096u) return -1;
    const u32 ea = cellGcmResolveLocated((t->format & 3u) == 1u, t->offset);
    if (ea == 0xFFFFFFFFu) return -1;
    const u32 fmt = (t->format >> 8) & 0xFFu;
    /* SET_TEXTURE_FORMAT's level count. gcm packs it into bits [19:16], but
     * the D3D12 backend and the method dispatcher both read the whole field
     * above bit 15; rsx_texture_mip_chain clamps whatever comes out to the
     * levels the dimensions allow, so the two readings agree. */
    const u32 levels = (t->format >> 16) & 0xFFFFu;
    const u32 pitch  = t->control3 & 0xFFFFFu;
    const int cube   = texture_is_cube(t);
    const u32 span   = texture_source_span(fmt, w, h, levels, pitch, cube);
    if (!span) return -1;
    const u32 csum = tex_csum(vm_base + ea, span);

    for (u32 i = 0; i < s_tex_count; i++) {
        MtlTexEntry* e = &s_tex_cache[i];
        /* `format` carries the level count and the cube bit as well as the
         * format byte, so the key covers both; the pitch has its own
         * register and has to be compared on its own. */
        if (e->ea != ea || e->w != w || e->h != h || e->format != t->format ||
            e->control1 != t->control1 || e->control3 != t->control3)
            continue;
        if (e->csum != csum) {
            id<MTLTexture> fresh = upload_texture(vm_base + ea, w, h, fmt,
                                                  t->control1, levels, pitch, cube);
            if (!fresh) return -1;
            e->tex = fresh; e->csum = csum;
        }
        return e->tex ? (int)i : -1;
    }
    if (s_tex_count >= MTL_TEX_CACHE) { s_caches_full = 1; return -1; }

    id<MTLTexture> tex = upload_texture(vm_base + ea, w, h, fmt, t->control1,
                                        levels, pitch, cube);
    { static int n = 0; if (n++ < 16)
        fprintf(stderr, "[RSX metal] texture %ux%u fmt 0x%02X %u level(s)%s at 0x%08X (%s) -> %s\n",
                w, h, fmt, levels ? levels : 1u, cube ? " cube" : "", ea,
                (t->format & 3u) == 1u ? "local" : "main",
                tex ? "ok" : "FAILED"); }
    const int slot = (int)s_tex_count++;
    MtlTexEntry* e = &s_tex_cache[slot];
    e->ea = ea; e->w = w; e->h = h; e->format = t->format; e->control1 = t->control1;
    e->control3 = t->control3; e->csum = csum; e->tex = tex;
    return tex ? slot : -1;
}

/* NV4097_SET_TEXTURE_ADDRESS wrap field (one per axis): 1 = WRAP, 2 =
 * MIRROR, 3 = CLAMP_TO_EDGE, 4 = BORDER, 5 = CLAMP, 6..8 = the MIRROR_ONCE
 * family. The same table the live draw engine's gcm_wrap uses. */
static MTLSamplerAddressMode gcm_wrap_to_metal(u32 w)
{
    switch (w & 0xFu) {
    case 1:  return MTLSamplerAddressModeRepeat;
    case 2:  return MTLSamplerAddressModeMirrorRepeat;
    case 4:  return MTLSamplerAddressModeClampToBorderColor;
    case 6: case 7: case 8:
             return MTLSamplerAddressModeMirrorClampToEdge;
    default: return MTLSamplerAddressModeClampToEdge;   /* 3, 5, unset */
    }
}

/* Sampler from the unit's wrap, filter and LOD registers, as the live draw
 * engine's decode_sampler reads them: SET_TEXTURE_FILTER min at [18:16]
 * (1 NEAREST, 2 LINEAR, then 3..6, which are the four combinations of a
 * nearest/linear minification with a nearest/linear mip filter), mag at
 * [26:24], and SET_TEXTURE_CONTROL0's max LOD at [18:7] and min LOD at
 * [30:19], both 4.8 fixed point.
 *
 * RSX's LOD bias is SET_TEXTURE_FILTER [12:0] and is not applied: Metal has
 * no sampler-side bias -- it is an argument to the sampling call, which means
 * patching the fragment program rather than the sampler -- and the live draw
 * engine leaves MipLODBias at zero too. */
static int samp_slot_for(const rsx_texture_state* t)
{
    const u32 minf = (t->filter >> 16) & 7u, magf = (t->filter >> 24) & 7u;
    const u32 lod  = (t->control0 >> 7) & 0xFFFFFFu;   /* max then min LOD */
    const u64 key = (u64)minf | ((u64)magf << 3)
                  | ((u64)(t->address & 0x000F0F0Fu) << 6)
                  | ((u64)lod << 26);
    for (u32 i = 0; i < s_samp_count; i++)
        if (s_samp_cache[i].key == key) return (int)i;
    if (s_samp_count >= MTL_SAMP_CACHE) return -1;

    const int mip_present = (minf >= 3);
    const int mip_linear  = (minf == 5 || minf == 6);

    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter = (minf == 2 || minf == 4 || minf == 6) ? MTLSamplerMinMagFilterLinear
                                                         : MTLSamplerMinMagFilterNearest;
    sd.magFilter = (magf == 2) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.mipFilter = !mip_present ? MTLSamplerMipFilterNotMipmapped
                 : mip_linear   ? MTLSamplerMipFilterLinear
                                : MTLSamplerMipFilterNearest;
    /* A min filter with no mip term samples level 0 only, which is what
     * pinning the LOD range to the minimum says. */
    const float min_lod = (float)((t->control0 >> 19) & 0xFFFu) / 256.0f;
    float max_lod = mip_present ? (float)((t->control0 >> 7) & 0xFFFu) / 256.0f : 0.0f;
    if (max_lod < min_lod) max_lod = min_lod;
    sd.lodMinClamp = min_lod;
    sd.lodMaxClamp = max_lod;
    sd.sAddressMode = gcm_wrap_to_metal(t->address);
    sd.tAddressMode = gcm_wrap_to_metal(t->address >> 8);
    sd.rAddressMode = gcm_wrap_to_metal(t->address >> 16);
    sd.borderColor  = MTLSamplerBorderColorTransparentBlack;
    id<MTLSamplerState> samp = [s_dev newSamplerStateWithDescriptor:sd];
    if (!samp) return -1;
    const int slot = (int)s_samp_count++;
    s_samp_cache[slot].key  = key;
    s_samp_cache[slot].samp = samp;
    return slot;
}

/* ---- guest surfaces ---------------------------------------------------------
 * A title does not render into the scanout. It renders into its own colour
 * surfaces, samples them, and composites the result; SET_SURFACE_COLOR_TARGET
 * says which of A, B or an MRT set is bound, and the offsets in
 * SET_SURFACE_COLOR_[ABCD]OFFSET say where they are.
 *
 * A surface is keyed by its RAW RSX offset, as the D3D12 backend's
 * current_rt_off keys them: surface and texture registers share the offset
 * space, so a texture bound at a surface's offset is that same buffer, and
 * matching on the raw value sidesteps guessing which context DMA the surface
 * lives in. An offset cellGcmSetDisplayBuffer registered is the display and
 * still draws into the drawable.
 *
 * Surfaces outlive the frame that created them -- a surface rendered in one
 * frame is sampled in the next -- so they are not part of clear_caches. A size
 * or format change gets a fresh texture; replacing the reference is safe
 * because a command buffer retains what it uses.
 * --------------------------------------------------------------------------*/

#define MTL_MAX_SURFACES  32
#define MTL_MAX_ZETA      16
#define MTL_RTVIEW_CACHE  64

typedef struct {
    u32 off;               /* raw RSX colour offset: the key      */
    u32 w, h, fmt;
    u32 gen;               /* bumped whenever the texture is replaced */
    id<MTLTexture> tex;
} MtlSurface;
typedef struct { u32 off, w, h; id<MTLTexture> tex; } MtlZeta;
typedef struct { int surf; u32 gen, control1, format; id<MTLTexture> view; } MtlRtView;

static MtlSurface s_surf[MTL_MAX_SURFACES];
static MtlZeta    s_zeta[MTL_MAX_ZETA];
static MtlRtView  s_rtview[MTL_RTVIEW_CACHE];
static u32 s_surf_count, s_zeta_count, s_rtview_count;

/* RSX surface colour format (SET_SURFACE_FORMAT bits [4:0]) -> Metal, as the
 * D3D12 backend's rsx_surface_dxgi maps it. The float targets are the reason
 * this is not one format: a title's height fields and differential planes hold
 * signed values that an 8-bit target clamps to zero. Everything else is the
 * drawable's BGRA8Unorm, so the clear colour and the headless readback keep
 * the conventions they already have. */
static MTLPixelFormat surface_format_to_metal(u32 fmt)
{
    switch (fmt & 0x1Fu) {
    case 0x0B: return MTLPixelFormatRGBA16Float;   /* F_W16Z16Y16X16 */
    case 0x0C: return MTLPixelFormatRGBA32Float;   /* F_W32Z32Y32X32 */
    case 0x0D: return MTLPixelFormatR32Float;      /* F_X32          */
    default:   return MTLPixelFormatBGRA8Unorm;
    }
}

/* Which colour surface the current state renders to, as current_rt_off decides
 * it: 0 for a display buffer, else the surface's raw offset, with the MRT set
 * and the surface size alongside. */
static u32 current_rt_off(const rsx_state* st, u32* out_w, u32* out_h, u32 out_mrt[3])
{
    *out_w = 0; *out_h = 0;
    out_mrt[0] = out_mrt[1] = out_mrt[2] = 0;
    if (!st) return 0;
    /* SET_SURFACE_COLOR_TARGET: 1 = A, 2 = B, 0x13 = MRT1 (A+B), 0x17 = MRT2
     * (A+B+C), 0x1F = MRT3 (A+B+C+D). */
    const int sel = (st->color_target == CELL_GCM_SURFACE_TARGET_1) ? 1 : 0;
    const u32 raw = st->surface_color_offset[sel];
    if (st->color_target >= CELL_GCM_SURFACE_TARGET_MRT1) out_mrt[0] = st->surface_color_offset[1];
    if (st->color_target >= CELL_GCM_SURFACE_TARGET_MRT2) out_mrt[1] = st->surface_color_offset[2];
    if (st->color_target >= CELL_GCM_SURFACE_TARGET_MRT3) out_mrt[2] = st->surface_color_offset[3];
    if (cellGcmOffsetIsDisplay(raw)) return 0;
    /* The clip dims are the surface's size when they are sane, else the window.
     * Any size works, since passes draw normalised full-surface geometry; this
     * only picks the resolution. */
    u32 w = st->surface_clip_w, h = st->surface_clip_h;
    if (w < 16 || w > 2048 || h < 16 || h > 2048) { w = 0; h = 0; }
    *out_w = w; *out_h = h;
    return raw;
}

/* The registry slot holding a live texture for this raw offset, or -1. */
static int surface_find(u32 off)
{
    if (!off) return -1;
    for (u32 i = 0; i < s_surf_count; i++)
        if (s_surf[i].tex && s_surf[i].off == off) return (int)i;
    return -1;
}

/* Make sure a surface exists for this offset at this size and format, and
 * return its slot. */
static int surface_get(u32 off, u32 w, u32 h, u32 fmt)
{
    if (!off || !s_dev) return -1;
    if (!w) w = s_width;
    if (!h) h = s_height;
    const MTLPixelFormat pf = surface_format_to_metal(fmt);

    int slot = -1;
    for (u32 i = 0; i < s_surf_count; i++)
        if (s_surf[i].off == off) { slot = (int)i; break; }
    if (slot >= 0) {
        MtlSurface* s = &s_surf[slot];
        if (s->tex && s->w == w && s->h == h &&
            surface_format_to_metal(s->fmt) == pf)
            return slot;
    } else {
        if (s_surf_count >= MTL_MAX_SURFACES) return -1;
        slot = (int)s_surf_count++;
    }

    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf
                                                           width:w height:h mipmapped:NO];
    /* PixelFormatView because a draw that samples the surface wears the unit's
     * TEXTURE_CONTROL1 crossbar as a view swizzle. */
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead |
               MTLTextureUsagePixelFormatView;
    id<MTLTexture> tex = [s_dev newTextureWithDescriptor:td];
    { static int n = 0; if (n++ < 16)
        fprintf(stderr, "[RSX metal] surface 0x%08X %ux%u fmt 0x%02X -> %s\n",
                off, w, h, fmt & 0x1Fu, tex ? "ok" : "FAILED"); }

    MtlSurface* s = &s_surf[slot];
    s->off = off; s->w = w; s->h = h; s->fmt = fmt;
    s->gen++;               /* invalidates the views cut from the old texture */
    s->tex = tex;
    return tex ? slot : -1;
}

/* The depth/stencil texture an offscreen surface renders against: one per
 * zeta offset and size, since a title points several surfaces of one size at
 * the same zeta buffer. Only the attachment is set up here; the compare and
 * write state belong to the depth/stencil path. */
static id<MTLTexture> zeta_get(u32 off, u32 w, u32 h)
{
    if (!s_dev || !w || !h) return nil;
    for (u32 i = 0; i < s_zeta_count; i++)
        if (s_zeta[i].off == off && s_zeta[i].w == w && s_zeta[i].h == h)
            return s_zeta[i].tex;
    if (s_zeta_count >= MTL_MAX_ZETA) return nil;

    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                                           width:w height:h mipmapped:NO];
    td.usage       = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> tex = [s_dev newTextureWithDescriptor:td];
    if (!tex) return nil;
    const u32 slot = s_zeta_count++;
    s_zeta[slot].off = off; s_zeta[slot].w = w; s_zeta[slot].h = h;
    s_zeta[slot].tex = tex;
    return tex;
}

/* A view of a surface carrying the sampling unit's TEXTURE_CONTROL1 crossbar
 * as its swizzle, so a render-to-texture bind reads the channels the guest
 * asked for. Cached per (surface, generation, crossbar, format) and, like the
 * shader and texture caches, slot-stable for the whole frame, because a draw
 * record holds the slot index. A recreated surface bumps its generation, so
 * the views cut from the old texture simply stop matching. */
static int rtview_for(int surf, u32 control1, u32 format)
{
    MtlSurface* s = &s_surf[surf];
    if (!s->tex) return -1;
    for (u32 i = 0; i < s_rtview_count; i++) {
        const MtlRtView* v = &s_rtview[i];
        if (v->surf == surf && v->gen == s->gen &&
            v->control1 == control1 && v->format == format)
            return v->view ? (int)i : -1;
    }
    if (s_rtview_count >= MTL_RTVIEW_CACHE) { s_caches_full = 1; return -1; }

    /* Same field order as an uploaded texture's swizzle: the crossbar's own
     * A,R,G,B order becomes destR = out[1] .. destA = out[0]. Its selectors
     * index the sampled vector as R,G,B,A, which is what a surface reads back
     * as whatever byte order it stores. */
    u8 remap[4];
    rsx_texture_component_remap(control1, (format >> 8) & 0x9Fu, remap);
    id<MTLTexture> view =
        [s->tex newTextureViewWithPixelFormat:[s->tex pixelFormat]
                                  textureType:MTLTextureType2D
                                       levels:NSMakeRange(0, 1)
                                       slices:NSMakeRange(0, 1)
                                      swizzle:MTLTextureSwizzleChannelsMake(
                                                  swizzle_sel(remap[1]), swizzle_sel(remap[2]),
                                                  swizzle_sel(remap[3]), swizzle_sel(remap[0]))];
    const int slot = (int)s_rtview_count++;
    s_rtview[slot].surf     = surf;
    s_rtview[slot].gen      = s->gen;
    s_rtview[slot].control1 = control1;
    s_rtview[slot].format   = format;
    s_rtview[slot].view     = view;
    return view ? slot : -1;
}

/* Is this offset one of the colour targets the record itself renders into?
 * Reading a surface a pass is writing is undefined on any API. */
static int rt_is_own_target(const MtlDraw* d, u32 off)
{
    if (!off) return 0;
    if (off == d->rt.off) return 1;
    for (int m = 0; m < 3; m++) if (off == d->rt.mrt[m]) return 1;
    return 0;
}

/* Fill in the colour target, MRT set, zeta and viewport a record was made
 * against, and make sure the surfaces it names exist. Registering here rather
 * than at present time is what lets a draw later in the same frame find the
 * surface an earlier draw rendered into (the D3D12 backend gets the same
 * effect from its render-to-texture pre-pass). */
static void target_for(const rsx_state* st, MtlTarget* t)
{
    memset(t, 0, sizeof *t);
    if (!st) return;
    t->off      = current_rt_off(st, &t->w, &t->h, t->mrt);
    t->fmt      = st->surface_format;
    t->zeta_off = st->surface_zeta_offset;
    t->vp_x = st->viewport_x; t->vp_y = st->viewport_y;
    t->vp_w = st->viewport_w; t->vp_h = st->viewport_h;
    if (!t->off) return;
    surface_get(t->off, t->w, t->h, t->fmt);
    for (int m = 0; m < 3; m++)
        if (t->mrt[m]) surface_get(t->mrt[m], t->w, t->h, t->fmt);
}

/* Resolve every enabled texture unit for a guest-program draw. */
static void textures_for(const rsx_state* st, MtlDraw* d)
{
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        const rsx_texture_state* t = &st->textures[u];
        if (!(t->control0 & 0x80000000u)) continue;
        /* Render-to-texture, the D3D12 backend's tex_rt rule: a unit whose raw
         * offset is a registered surface samples that surface. Uploading from
         * guest memory instead would read a buffer the pass that produced the
         * image never wrote, which is how a reflection or a shadow map comes
         * out black. */
        const int surf = surface_find(t->offset);
        if (surf >= 0) {
            d->samp[u] = samp_slot_for(t);
            if (rt_is_own_target(d, t->offset)) {
                { static int n = 0; if (n++ < 1)
                    fprintf(stderr, "[RSX metal] unit %u samples the surface the pass "
                                    "draws into (0x%08X); reading the zero texture\n",
                            u, t->offset); }
                continue;                 /* leaves the unit on the zero texture */
            }
            d->tex_rt[u] = rtview_for(surf, t->control1, t->format);
            continue;
        }
        d->tex[u]  = tex_slot_for(t);
        d->samp[u] = samp_slot_for(t);
    }
    /* Vertex textures go through the same resolve, upload and caches: a
     * vertex unit's registers are decoded into the same rsx_texture_state,
     * with no crossbar, so nothing below here knows the difference. */
    for (u32 u = 0; u < RSX_MAX_VERTEX_TEXTURES; u++) {
        const rsx_texture_state* t = &st->vertex_textures[u];
        if (!(t->control0 & 0x80000000u)) continue;
        d->vtex[u]  = tex_slot_for(t);
        d->vsamp[u] = samp_slot_for(t);
    }
}

/* Guest face culling, packed as the D3D12 backend's rsx_cull_key reads it:
 * CULL_FACE FRONT=0x0404 BACK=0x0405 FRONT_AND_BACK=0x0408 (FRONT here, as
 * there); FRONT_FACE CW=0x0900 CCW=0x0901. rsx_commands seeds cull_face and
 * front_face with plain 1/0 before any register arrives, which reads as
 * BACK/CW. Metal's default winding is clockwise, so the mapping is direct. */
static void cull_from_state(const rsx_state* st, MtlDraw* d)
{
    d->cull = MTLCullModeNone;
    if (st->cull_face_enable)
        d->cull = (st->cull_face == 0x0404u || st->cull_face == 0x0408u)
                      ? MTLCullModeFront : MTLCullModeBack;
    d->winding = (st->front_face == 0x0901u) ? MTLWindingCounterClockwise
                                             : MTLWindingClockwise;
}

/* ---- depth/stencil state -------------------------------------------------
 * The NV4097 comparison functions are the GL enums, 0x200 NEVER .. 0x207
 * ALWAYS, and the stencil ops are GL's too. Both tables are copied from the
 * live draw engine's gcm_cmp / gcm_stencil_op, which is what a title has
 * actually run through.
 *
 * Note that rsx_state_init seeds depth_func with 1 and calls it LESS, which is
 * not the GL enum for anything; like the reference, an unrecognised function
 * decodes as ALWAYS. Nothing reaches the depth test with that value unless the
 * guest enabled the test without programming SET_DEPTH_FUNC.
 * --------------------------------------------------------------------------*/

static MTLCompareFunction gcm_compare_to_metal(u32 f)
{
    switch (f) {
    case 0x0200: return MTLCompareFunctionNever;
    case 0x0201: return MTLCompareFunctionLess;
    case 0x0202: return MTLCompareFunctionEqual;
    case 0x0203: return MTLCompareFunctionLessEqual;
    case 0x0204: return MTLCompareFunctionGreater;
    case 0x0205: return MTLCompareFunctionNotEqual;
    case 0x0206: return MTLCompareFunctionGreaterEqual;
    default:     return MTLCompareFunctionAlways;
    }
}

/* GL_ZERO is 0, which is also what the register reads before a title ever
 * writes it -- and GL_KEEP is the value that leaves such a stream alone, so
 * zero maps to ZERO only because the guest asked for it explicitly; an
 * unrecognised op keeps, as in the reference. */
static MTLStencilOperation gcm_stencil_op_to_metal(u32 op)
{
    switch (op) {
    case 0x0000: return MTLStencilOperationZero;              /* GL_ZERO       */
    case 0x1E01: return MTLStencilOperationReplace;
    case 0x1E02: return MTLStencilOperationIncrementClamp;    /* GL_INCR       */
    case 0x1E03: return MTLStencilOperationDecrementClamp;    /* GL_DECR       */
    case 0x150A: return MTLStencilOperationInvert;
    case 0x8507: return MTLStencilOperationIncrementWrap;
    case 0x8508: return MTLStencilOperationDecrementWrap;
    default:     return MTLStencilOperationKeep;              /* GL_KEEP 0x1E00 */
    }
}

/* MTLDepthStencilState objects are immutable, so one is built per distinct
 * combination of the registers and kept for the frame, exactly like the
 * pipeline states. The stencil reference value is not part of the object --
 * Metal sets it on the encoder -- so it is not part of the key either. */
#define MTL_DS_CACHE 256

typedef struct {
    u8  depth_test, depth_mask, stencil_test;
    u32 depth_func;
    u32 stencil_func, stencil_mask;
    u32 op_fail, op_zfail, op_zpass;
} MtlDsKey;
typedef struct { MtlDsKey key; id<MTLDepthStencilState> state; } MtlDsEntry;

static MtlDsEntry s_ds_cache[MTL_DS_CACHE];
static u32        s_ds_count;

static int ds_slot_for(const rsx_state* st)
{
    MtlDsKey key;
    memset(&key, 0, sizeof key);
    key.depth_test   = (u8)(st->depth_test_enable ? 1 : 0);
    key.depth_mask   = (u8)(st->depth_mask ? 1 : 0);
    key.stencil_test = (u8)(st->stencil_test_enable ? 1 : 0);
    key.depth_func   = st->depth_func;
    key.stencil_func = st->stencil_func;
    key.stencil_mask = st->stencil_mask;
    key.op_fail      = st->stencil_op_fail;
    key.op_zfail     = st->stencil_op_zfail;
    key.op_zpass     = st->stencil_op_zpass;

    for (u32 i = 0; i < s_ds_count; i++)
        if (memcmp(&s_ds_cache[i].key, &key, sizeof key) == 0) return (int)i;
    if (s_ds_count >= MTL_DS_CACHE) { s_caches_full = 1; return -1; }

    MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
    /* The attachment is always bound, so "depth test off" is compare Always
     * with writes off -- which is also what the hardware does: with the test
     * disabled the RSX writes no depth, whatever SET_DEPTH_MASK says. */
    dd.depthCompareFunction = key.depth_test ? gcm_compare_to_metal(key.depth_func)
                                             : MTLCompareFunctionAlways;
    dd.depthWriteEnabled    = (key.depth_test && key.depth_mask) ? YES : NO;
    if (key.stencil_test) {
        MTLStencilDescriptor* sd = [MTLStencilDescriptor new];
        sd.stencilCompareFunction    = gcm_compare_to_metal(key.stencil_func);
        sd.stencilFailureOperation   = gcm_stencil_op_to_metal(key.op_fail);
        sd.depthFailureOperation     = gcm_stencil_op_to_metal(key.op_zfail);
        sd.depthStencilPassOperation = gcm_stencil_op_to_metal(key.op_zpass);
        sd.readMask                  = key.stencil_mask & 0xFFu;
        /* SET_STENCIL_FUNC_MASK is the read mask. The write mask lives in
         * NV4097_SET_STENCIL_MASK (0x032C), which rsx_commands.c does not
         * decode and rsx_state has no field for, so every plane is writable
         * here. It reads back wrong for a title that masks stencil planes;
         * fixing it means adding the register upstream, not guessing here. */
        sd.writeMask                 = 0xFFu;
        /* Two-sided stencil (SET_BACK_STENCIL_*) is not decoded upstream
         * either; nv40 applies the front state to both faces when it is off,
         * which is what this does. */
        dd.frontFaceStencil = sd;
        dd.backFaceStencil  = sd;
    } else {
        dd.frontFaceStencil = nil;
        dd.backFaceStencil  = nil;
    }

    id<MTLDepthStencilState> state = [s_dev newDepthStencilStateWithDescriptor:dd];
    if (!state) return -1;
    const int slot = (int)s_ds_count++;
    s_ds_cache[slot].key   = key;
    s_ds_cache[slot].state = state;
    return slot;
}

/* ---- draw recording ------------------------------------------------------ */

static void record_draw(const rsx_state* st, u32 prim, u32 base, u32 count,
                        IndexResolver resolve)
{
    if (!s_ready || !s_verts || !st || count == 0) return;
    if (s_rec_count >= MTL_MAX_RECORDS) { s_dropped_records++; return; }

    MtlRecord* r = &s_records[s_rec_count];
    r->kind = MTL_REC_DRAW;
    MtlDraw* d = &r->u.draw;
    memset(d, 0, sizeof *d);
    d->vs_idx = d->fs_idx = d->ds_idx = -1;
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++)
        d->tex[u] = d->samp[u] = d->tex_rt[u] = -1;
    for (u32 u = 0; u < RSX_MAX_VERTEX_TEXTURES; u++) d->vtex[u] = d->vsamp[u] = -1;

    /* The target before anything else: the surfaces it names have to be
     * registered before the textures are resolved against them. */
    target_for(st, &d->rt);

    /* Programs first: their translation is what can fail, and a draw that
     * drops to the built-in shader must still fetch its vertices the same
     * way. Constant staging reserved here for a draw that then fails on
     * vertices is simply left unused until the frame resets it. */
    const int guest = s_guest_shaders && guest_programs_for(st, d);
    if (guest) textures_for(st, d);

    u32 first_vert = s_vert_count;
    u32 wrote = emit_vertices(st, prim, count, resolve, &base);
    if (wrote == 0) { s_dropped_records++; return; }
    s_rec_count++;
    if (guest) s_guest_draws++;

    d->base  = first_vert;
    d->count = wrote;
    /* Expanded primitives always come out as a triangle list. */
    d->topology = rsx_primitive_needs_expansion(prim) ? MTLPrimitiveTypeTriangle
                                             : topo_to_metal(rsx_primitive_topology(prim));
    d->blend_enable   = st->blend_enable;
    d->blend_sfactor  = st->blend_sfactor;
    d->blend_dfactor  = st->blend_dfactor;
    d->blend_equation = st->blend_equation;
    d->color_mask     = st->color_mask;
    d->ds_idx         = ds_slot_for(st);
    d->stencil_ref    = st->stencil_ref;
    cull_from_state(st, d);

    /* Built-in path: RSX vertex constant slots 0..3 hold the MVP rows when no
     * vertex program has been translated. An all-zero matrix would collapse
     * every vertex to the origin, so fall back to identity. */
    int nonzero = 0;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float v = st->vertex_constants[r][c];
            d->mvp[r * 4 + c] = v;
            if (v != 0.0f) nonzero = 1;
        }
    if (!nonzero) {
        memset(d->mvp, 0, sizeof(d->mvp));
        d->mvp[0] = d->mvp[5] = d->mvp[10] = d->mvp[15] = 1.0f;
    }
}

static void mtl_draw_arrays(void* ud, u32 primitive, u32 first, u32 count)
{
    (void)ud;
    record_draw(s_state, primitive, first, count, resolve_linear);
}

static void mtl_draw_indexed(void* ud, u32 primitive, u32 index_offset, u32 count)
{
    (void)ud;
    record_draw(s_state, primitive, index_offset, count, resolve_indexed);
}

/* ---- pipeline state ------------------------------------------------------ */

static MTLBlendFactor blend_factor_to_metal(u32 f)
{
    switch (f) {
    case 0x0000: return MTLBlendFactorZero;
    case 0x0001: return MTLBlendFactorOne;
    case 0x0300: return MTLBlendFactorSourceColor;
    case 0x0301: return MTLBlendFactorOneMinusSourceColor;
    case 0x0302: return MTLBlendFactorSourceAlpha;
    case 0x0303: return MTLBlendFactorOneMinusSourceAlpha;
    case 0x0304: return MTLBlendFactorDestinationAlpha;
    case 0x0305: return MTLBlendFactorOneMinusDestinationAlpha;
    case 0x0306: return MTLBlendFactorDestinationColor;
    case 0x0307: return MTLBlendFactorOneMinusDestinationColor;
    case 0x0308: return MTLBlendFactorSourceAlphaSaturated;
    default:     return MTLBlendFactorOne;
    }
}

static MTLBlendOperation blend_equation_to_metal(u32 e)
{
    switch (e) {
    case 0x8007: return MTLBlendOperationMin;
    case 0x8008: return MTLBlendOperationMax;
    case 0x800A: return MTLBlendOperationSubtract;
    case 0x800B: return MTLBlendOperationReverseSubtract;
    default:     return MTLBlendOperationAdd;
    }
}

/* NV4097_SET_COLOR_MASK: bit 0 = B, 8 = G, 16 = R, 24 = A. */
static MTLColorWriteMask color_mask_to_metal(u32 m)
{
    MTLColorWriteMask w = MTLColorWriteMaskNone;
    if (m & 0x00010000u) w |= MTLColorWriteMaskRed;
    if (m & 0x00000100u) w |= MTLColorWriteMaskGreen;
    if (m & 0x00000001u) w |= MTLColorWriteMaskBlue;
    if (m & 0x01000000u) w |= MTLColorWriteMaskAlpha;
    return w;
}

/* Built-in shaders: transform by the RSX vertex-constant matrix and interpolate
 * the diffuse colour. The rows are dotted explicitly rather than using float4x4
 * so there is no column-major/row-major ambiguity with the guest's layout.
 * Reads position, diffuse colour and texcoord0 from RSX attribute slots 0, 3
 * and 8 of the shared vertex layout. The guest-shader path replaces this, it
 * does not extend it. */
static NSString* const kBuiltinMSL = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VIn  { float4 pos [[attribute(0)]]; float4 col [[attribute(3)]]; float4 tc [[attribute(8)]]; };\n"
"struct VOut { float4 pos [[position]]; float4 col; float4 tc; };\n"
"struct VU   { float4 mvp[4]; };\n"
"vertex VOut vs_main(VIn v [[stage_in]], constant VU& u [[buffer(0)]]) {\n"
"    VOut o;\n"
"    o.pos = float4(dot(u.mvp[0], v.pos), dot(u.mvp[1], v.pos),\n"
"                   dot(u.mvp[2], v.pos), dot(u.mvp[3], v.pos));\n"
"    o.col = v.col; o.tc = v.tc;\n"
"    return o;\n"
"}\n"
"fragment float4 fs_main(VOut in [[stage_in]]) { return in.col; }\n";

static u32 blend_key(const MtlDraw* d)
{
    if (!d->blend_enable) return 0u;
    return 1u | ((d->blend_sfactor & 0xFFFu) << 1)
              | ((d->blend_dfactor & 0xFFFu) << 13)
              | ((d->blend_equation & 0x7u) << 25);
}

/* All 16 RSX attributes as float4 at i*16, stride 256, from MTL_VB_INDEX.
 * A guest program declares only the attributes it reads; Metal ignores the
 * rest of the descriptor. */
static MTLVertexDescriptor* vertex_descriptor(void)
{
    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
    for (int i = 0; i < MTL_ATTRIBS; i++) {
        vd.attributes[i].format      = MTLVertexFormatFloat4;
        vd.attributes[i].offset      = (NSUInteger)(i * 16);
        vd.attributes[i].bufferIndex = MTL_VB_INDEX;
    }
    vd.layouts[MTL_VB_INDEX].stride = sizeof(MtlVertex);
    return vd;
}

static id<MTLRenderPipelineState> pso_for(const MtlDraw* d, MTLPixelFormat color_pf,
                                          u32 nrt)
{
    MtlPsoKey key;
    memset(&key, 0, sizeof key);
    key.vs = d->vs_idx; key.fs = d->fs_idx;
    key.blend = blend_key(d); key.cmask = d->color_mask;
    key.color_pf = (u32)color_pf; key.nrt = nrt;
    for (u32 i = 0; i < s_pso_count; i++)
        if (memcmp(&s_pso_cache[i].key, &key, sizeof key) == 0) return s_pso_cache[i].pso;
    if (s_pso_count >= MTL_PSO_CACHE) { s_caches_full = 1; return nil; }

    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    if (d->vs_idx >= 0) {
        pd.vertexFunction   = s_vp_cache[d->vs_idx].fn;
        pd.fragmentFunction = s_fp_cache[d->fs_idx].fn;
    } else {
        pd.vertexFunction   = [s_shader_lib newFunctionWithName:@"vs_main"];
        pd.fragmentFunction = [s_shader_lib newFunctionWithName:@"fs_main"];
    }
    pd.vertexDescriptor = vertex_descriptor();
    /* A depth/stencil attachment is bound on every pass -- the shared one for
     * the display, the target's own zeta for an offscreen surface, both
     * MTL_DEPTH_FORMAT -- so every pipeline declares it; whether a draw tests
     * or writes is the MTLDepthStencilState's business, not the pipeline's. */
    pd.depthAttachmentPixelFormat   = MTL_DEPTH_FORMAT;
    pd.stencilAttachmentPixelFormat = MTL_DEPTH_FORMAT;
    /* Every bound colour target takes target A's blend and colour mask, which
     * is what the D3D12 backend does with RTVFormats[1..]: a zero-initialised
     * secondary attachment writes nothing at all, and a deferred pass would
     * come back with only its first G-buffer plane filled in. The fragment
     * decompiler feeds them now -- a program writing r2/r3/r4 (or h4/h6/h8)
     * returns that many SV_TARGETs. The two counts need not agree: Metal
     * accepts a function with fewer outputs than the pipeline has attachments
     * AND one with more, discarding the surplus, which is how the guest's own
     * hardware behaves when a program exports past the bound set. */
    for (u32 r = 0; r < nrt; r++) {
        MTLRenderPipelineColorAttachmentDescriptor* ca = pd.colorAttachments[r];
        ca.pixelFormat = color_pf;
        ca.writeMask   = color_mask_to_metal(d->color_mask);
        if (d->blend_enable) {
            ca.blendingEnabled             = YES;
            ca.sourceRGBBlendFactor        = blend_factor_to_metal(d->blend_sfactor);
            ca.destinationRGBBlendFactor   = blend_factor_to_metal(d->blend_dfactor);
            ca.sourceAlphaBlendFactor      = blend_factor_to_metal(d->blend_sfactor);
            ca.destinationAlphaBlendFactor = blend_factor_to_metal(d->blend_dfactor);
            ca.rgbBlendOperation           = blend_equation_to_metal(d->blend_equation);
            ca.alphaBlendOperation         = blend_equation_to_metal(d->blend_equation);
        }
    }

    NSError* err = nil;
    id<MTLRenderPipelineState> pso =
        [s_dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!pso) {
        fprintf(stderr, "[RSX metal] pipeline state failed (vs %d, fs %d): %s\n",
                d->vs_idx, d->fs_idx, [[err localizedDescription] UTF8String]);
        return nil;
    }
    s_pso_cache[s_pso_count].key = key;
    s_pso_cache[s_pso_count].pso = pso;
    s_pso_count++;
    return pso;
}

/* ---- replay --------------------------------------------------------------
 * Everything a render pass has to bind before its first draw. Encoder state
 * does not outlive endEncoding, so a clear in the middle of a frame -- or a
 * switch to another render target -- pays for this again on the pass it opens.
 * --------------------------------------------------------------------------*/

typedef struct {
    id<MTLBuffer> vb;
    id<MTLBuffer> cbuf;
    int tex[RSX_MAX_TEXTURES];    /* what each unit currently holds, or -1 */
    int samp[RSX_MAX_TEXTURES];
    int vtex[RSX_MAX_VERTEX_TEXTURES];
    int vsamp[RSX_MAX_VERTEX_TEXTURES];
    /* The target this pass is aimed at, and what the attachments turned out
     * to be. A record whose target differs ends the pass and opens the next. */
    MtlTarget rt;
    double w, h;                  /* the target's size, for the viewport */
    MTLPixelFormat color_pf;
    u32 nrt;                      /* bound colour attachments */
} MtlPassState;

/* Do two records belong in the same pass? Only the colour targets decide it:
 * everything else about a record is encoder or pipeline state. */
static int target_same(const MtlTarget* a, const MtlTarget* b)
{
    return a->off == b->off && memcmp(a->mrt, b->mrt, sizeof a->mrt) == 0;
}

/* What a unit binds, as one identity covering both sources: a surface view, an
 * uploaded texture, or the zero texture. Surface views are numbered above the
 * texture cache so the "already bound?" test stays a single compare. */
static int tex_binding_id(const MtlDraw* d, int u)
{
    return d->tex_rt[u] >= 0 ? MTL_TEX_CACHE + d->tex_rt[u] : d->tex[u];
}

static id<MTLTexture> tex_binding(const MtlDraw* d, int u)
{
    if (d->tex_rt[u] >= 0 && s_rtview[d->tex_rt[u]].view)
        return s_rtview[d->tex_rt[u]].view;
    if (d->tex[u] >= 0) return s_tex_cache[d->tex[u]].tex;
    return s_null_tex;
}

/* Where a vertex texture binds. The VP decompiler declares unit N's texture
 * at HLSL register t(16+N) and its sampler at sN, and spirv-cross carries
 * both numbers into the vertex function -- so the two do NOT share an index.
 * test_shader_msl asserts exactly that. */
#define MTL_VTEX_INDEX(n) ((NSUInteger)(16u + (n)))
#define MTL_VSAMP_INDEX(n) ((NSUInteger)(n))

/* Open a render pass on the record's target: the drawable for a display
 * buffer, else the offscreen surface registered at that raw offset, with its
 * MRT set and its own zeta in place of the shared depth buffer. The
 * attachments `clear` flags name load-action Clear with its values; the rest
 * load-action Load, so what an earlier pass in the same frame left behind
 * survives -- which is how a title fills one surface across several runs of
 * records. */
static id<MTLRenderCommandEncoder> begin_pass(id<MTLCommandBuffer> cb,
                                              id<MTLTexture> display,
                                              const MtlTarget* t,
                                              const MtlClear* clear,
                                              MtlPassState* ps)
{
    /* Latched before anything can fail, so a target that cannot be bound is
     * still this pass's target: the rest of its run is skipped rather than
     * reopened once per record. */
    ps->rt = *t;

    const int surf = surface_find(t->off);
    id<MTLTexture> color = (surf >= 0) ? s_surf[surf].tex : display;
    if (!color) return nil;

    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture     = color;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (clear->flags & MTL_CLEAR_COLOR) {
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = clear_color_from_argb(clear->color);
    } else {
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    }

    /* MRT B, C and D, up to the first one that is not a registered surface --
     * the D3D12 backend stops at the same gap. Metal wants every attachment
     * the same size, and surfaces are registered at target A's size, so a set
     * whose members disagree is one this frame cannot honour. */
    ps->nrt = 1;
    for (int m = 0; m < 3 && t->mrt[m]; m++) {
        const int ms = surface_find(t->mrt[m]);
        if (ms < 0 || [s_surf[ms].tex width]  != [color width]
                   || [s_surf[ms].tex height] != [color height]) break;
        rp.colorAttachments[ps->nrt].texture     = s_surf[ms].tex;
        rp.colorAttachments[ps->nrt].storeAction = MTLStoreActionStore;
        if (clear->flags & MTL_CLEAR_COLOR) {
            rp.colorAttachments[ps->nrt].loadAction = MTLLoadActionClear;
            rp.colorAttachments[ps->nrt].clearColor = clear_color_from_argb(clear->color);
        } else {
            rp.colorAttachments[ps->nrt].loadAction = MTLLoadActionLoad;
        }
        ps->nrt++;
    }

    /* Both halves of one Depth32Float_Stencil8 texture: the shared one for the
     * display, the surface's own zeta otherwise, so an offscreen pass does not
     * trample the depth the display pass is building. Stored rather than
     * discarded: a later pass in the same frame loads what this one wrote. */
    id<MTLTexture> depth = s_depth;
    if (surf >= 0) {
        id<MTLTexture> z = zeta_get(t->zeta_off, s_surf[surf].w, s_surf[surf].h);
        if (z) depth = z;
    }
    rp.depthAttachment.texture       = depth;
    rp.depthAttachment.storeAction   = MTLStoreActionStore;
    rp.depthAttachment.loadAction    = (clear->flags & MTL_CLEAR_DEPTH)
                                           ? MTLLoadActionClear : MTLLoadActionLoad;
    rp.depthAttachment.clearDepth    = (double)clear->depth;
    rp.stencilAttachment.texture     = depth;
    rp.stencilAttachment.storeAction = MTLStoreActionStore;
    rp.stencilAttachment.loadAction  = (clear->flags & MTL_CLEAR_STENCIL)
                                           ? MTLLoadActionClear : MTLLoadActionLoad;
    rp.stencilAttachment.clearStencil = clear->stencil;

    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    if (!enc) return nil;
    ps->w        = (double)[color width];
    ps->h        = (double)[color height];
    ps->color_pf = [color pixelFormat];

    if (ps->vb) [enc setVertexBuffer:ps->vb offset:0 atIndex:MTL_VB_INDEX];
    /* Every fragment texture unit starts on the zero texture -- a guest
     * program may sample a unit nothing was bound to, and Metal validation
     * rejects a nil texture on use. Units are then rebound per draw only where
     * the draw's slot differs. */
    for (int u = 0; u < RSX_MAX_TEXTURES; u++) {
        [enc setFragmentTexture:s_null_tex atIndex:(NSUInteger)u];
        [enc setFragmentSamplerState:s_default_sampler atIndex:(NSUInteger)u];
        ps->tex[u] = ps->samp[u] = -1;
    }
    /* The vertex stage's units get the same treatment, for the same reason. */
    for (int u = 0; u < RSX_MAX_VERTEX_TEXTURES; u++) {
        [enc setVertexTexture:s_null_tex atIndex:MTL_VTEX_INDEX(u)];
        [enc setVertexSamplerState:s_default_sampler atIndex:MTL_VSAMP_INDEX(u)];
        ps->vtex[u] = ps->vsamp[u] = -1;
    }
    /* The target's own size, which is not the window's once a title renders
     * into a surface of its own. Each draw narrows it to the guest rect. */
    MTLViewport vp = { 0.0, 0.0, ps->w, ps->h, 0.0, 1.0 };
    [enc setViewport:vp];
    return enc;
}

static void encode_draw(id<MTLRenderCommandEncoder> enc, const MtlDraw* d,
                        MtlPassState* ps)
{
    id<MTLRenderPipelineState> pso = pso_for(d, ps->color_pf, ps->nrt);
    if (!pso) return;
    [enc setRenderPipelineState:pso];
    [enc setCullMode:d->cull];
    [enc setFrontFacingWinding:d->winding];
    [enc setDepthStencilState:(d->ds_idx >= 0 ? s_ds_cache[d->ds_idx].state
                                              : s_ds_default)];
    [enc setStencilReferenceValue:d->stencil_ref];
    /* The guest viewport rect when it fits inside the target, else the whole
     * target -- render_frame's rule. Sub-viewport layouts place their quads
     * with this rect and nothing else, so a forced full-target viewport draws
     * every one of them target-sized. The scissor tracks it. */
    MTLViewport vp = { 0.0, 0.0, ps->w, ps->h, 0.0, 1.0 };
    if (d->rt.vp_w >= 2 && d->rt.vp_h >= 2 &&
        (double)(d->rt.vp_x + d->rt.vp_w) <= ps->w + 0.5 &&
        (double)(d->rt.vp_y + d->rt.vp_h) <= ps->h + 0.5) {
        vp.originX = (double)d->rt.vp_x; vp.originY = (double)d->rt.vp_y;
        vp.width   = (double)d->rt.vp_w; vp.height  = (double)d->rt.vp_h;
    }
    [enc setViewport:vp];
    [enc setScissorRect:(MTLScissorRect){
        (NSUInteger)vp.originX, (NSUInteger)vp.originY,
        (NSUInteger)vp.width,   (NSUInteger)vp.height }];
    if (d->vs_idx >= 0 && ps->cbuf) {
        [enc setVertexBuffer:ps->cbuf offset:d->vp_cb_off atIndex:0];
        [enc setFragmentBuffer:ps->cbuf offset:d->fp_cb_off atIndex:1];
        for (int u = 0; u < RSX_MAX_TEXTURES; u++) {
            const int want = tex_binding_id(d, u);
            if (want != ps->tex[u]) {
                [enc setFragmentTexture:tex_binding(d, u) atIndex:(NSUInteger)u];
                ps->tex[u] = want;
            }
            if (d->samp[u] != ps->samp[u]) {
                [enc setFragmentSamplerState:(d->samp[u] >= 0 ? s_samp_cache[d->samp[u]].samp
                                                              : s_default_sampler)
                                     atIndex:(NSUInteger)u];
                ps->samp[u] = d->samp[u];
            }
        }
        for (int u = 0; u < RSX_MAX_VERTEX_TEXTURES; u++) {
            if (d->vtex[u] != ps->vtex[u]) {
                [enc setVertexTexture:(d->vtex[u] >= 0 ? s_tex_cache[d->vtex[u]].tex
                                                       : s_null_tex)
                              atIndex:MTL_VTEX_INDEX(u)];
                ps->vtex[u] = d->vtex[u];
            }
            if (d->vsamp[u] != ps->vsamp[u]) {
                [enc setVertexSamplerState:(d->vsamp[u] >= 0 ? s_samp_cache[d->vsamp[u]].samp
                                                             : s_default_sampler)
                                   atIndex:MTL_VSAMP_INDEX(u)];
                ps->vsamp[u] = d->vsamp[u];
            }
        }
    } else {
        [enc setVertexBytes:d->mvp length:sizeof(d->mvp) atIndex:0];
    }
    [enc drawPrimitives:d->topology vertexStart:d->base vertexCount:d->count];
}

static void clear_caches(void)
{
    for (u32 i = 0; i < s_vp_count;   i++) s_vp_cache[i].fn     = nil;
    for (u32 i = 0; i < s_fp_count;   i++) s_fp_cache[i].fn     = nil;
    for (u32 i = 0; i < s_pso_count;  i++) s_pso_cache[i].pso   = nil;
    for (u32 i = 0; i < s_tex_count;  i++) s_tex_cache[i].tex   = nil;
    for (u32 i = 0; i < s_samp_count; i++) s_samp_cache[i].samp = nil;
    for (u32 i = 0; i < s_ds_count;   i++) s_ds_cache[i].state  = nil;
    for (u32 i = 0; i < s_rtview_count; i++) s_rtview[i].view   = nil;
    s_vp_count = s_fp_count = s_pso_count = s_tex_count = s_samp_count = 0;
    s_ds_count = s_rtview_count = 0;
    s_caches_full = 0;
}

/* Surfaces survive a cache clear -- they hold rendered pixels a later frame
 * still samples -- so they are released only on the way out. */
static void release_surfaces(void)
{
    for (u32 i = 0; i < s_surf_count; i++) s_surf[i].tex = nil;
    for (u32 i = 0; i < s_zeta_count; i++) s_zeta[i].tex = nil;
    s_surf_count = s_zeta_count = 0;
}


/* ---- the register-file draw engine's backend --------------------------------
 * rsx_draw_engine.c implements rsx_dispatch_sink over the whole NV4097
 * register file and drives a backend through rsx_draw_backend. Everything
 * below is this file's implementation of that interface. It is a second
 * record stream rather than a reworking of the one above, because the two
 * front ends are different -- the vtable one is fed rsx_state and resolves a
 * draw's programs and textures itself, while the engine hands over resolved
 * handles -- and the vtable path stays the default until they have parity.
 *
 * What is shared is everything below the front end: the device and queue, the
 * shader translation, the texture decode, the sampler decode tables and the
 * blend, compare and stencil translations.
 *
 * Handles are indices into one object table, because the engine binds colour
 * targets, depth snapshots and uploaded textures through the same array and a
 * bind cannot tell them apart. Pipelines have their own table: only
 * bind_pipeline ever names one.
 * --------------------------------------------------------------------------*/

#include "rsx_draw_engine.h"

#define ENG_MAX_OBJECTS  4096
#define ENG_MAX_PIPES    4096
#define ENG_MAX_RECORDS  8192
#define ENG_MAX_VIEWS    256
#define ENG_MAX_FUNCS    4096
#define ENG_MAX_SAMPLERS 256
#define ENG_ALIGN        256u
/* One frame's staging. Past it the backend submits and waits rather than
 * dropping the rest of the frame: dropping "discarded 181/184 sampled a010
 * groups" once the orphanage workload filled the D3D12 engine's ring. */
#define ENG_STAGE_MAX    (384u << 20)

static int s_eng_active;

static id<MTLTexture> s_eng_obj[ENG_MAX_OBJECTS];
static u32 s_eng_obj_count;
static u32 s_eng_obj_free[ENG_MAX_OBJECTS];
static u32 s_eng_obj_free_count;
static u8 s_eng_obj_retired[ENG_MAX_OBJECTS];

typedef struct {
    id<MTLRenderPipelineState> pso;
    id<MTLDepthStencilState>   ds;
    MTLCullMode cull;
    MTLWinding  winding;
} EngPipeline;
static EngPipeline s_eng_pipe[ENG_MAX_PIPES];
static u32 s_eng_pipe_count;

/* Compiled MSL functions, keyed on the HLSL the decompilers emitted: one
 * program is translated and compiled once however many pipeline variants
 * reference it. */
typedef struct { u64 hash; id<MTLFunction> fn; } EngFunc;
static EngFunc s_eng_func[ENG_MAX_FUNCS];
static u32 s_eng_func_count;

typedef struct { u64 key; id<MTLSamplerState> samp; } EngSampler;
static EngSampler s_eng_samp[ENG_MAX_SAMPLERS];
static u32 s_eng_samp_count;

typedef struct { u32 surface, remap, format, view; } EngView;
static EngView s_eng_view[ENG_MAX_VIEWS];
static u32 s_eng_view_count;

typedef enum {
    ENG_REC_DRAW, ENG_REC_CLEAR_COLOR, ENG_REC_CLEAR_DS, ENG_REC_DEPTH_RESOLVE
} EngRecKind;

typedef struct {
    EngRecKind kind;
    /* The colour targets a draw writes, A first, and how many of them; a
     * colour clear names one, in rt[0]. */
    u32 rt[RSX_BE_MAX_COLOR_TARGETS];
    u32 nrt;
    u32 depth;
    u32 pipeline;
    MTLPrimitiveType topology;
    u32 vb_off, stride, vertex_count;
    u32 ib_off, index_count;
    u32 vs_cb_off, vs_cb_bytes;
    u32 ps_cb_off, ps_cb_bytes;
    u32 tex[RSX_BE_MAX_TEXTURES];
    int samp[RSX_BE_MAX_TEXTURES];
    u32 vtex[RSX_BE_MAX_VERTEX_TEXTURES];
    int vsamp[RSX_BE_MAX_VERTEX_TEXTURES];
    float vp[4];
    u32   sc[4];
    u32   stencil_ref;
    float clear_rgba[4];
    u32   clear_flags;
    float clear_depth;
    u8    clear_stencil;
    u32   resolve_dst;          /* ENG_REC_DEPTH_RESOLVE target texture */
} EngRecord;

static EngRecord s_eng_rec[ENG_MAX_RECORDS];
static u32 s_eng_rec_count;
static u32 s_eng_dropped;

static u8* s_eng_stage;
static u32 s_eng_stage_used, s_eng_stage_cap;

/* What the bind_* calls have accumulated for the next draw. */
static EngRecord s_eng_pending;

/* The blit and depth-resolve helpers, and their sampler. */
static id<MTLLibrary>             s_eng_helper_lib;
static id<MTLRenderPipelineState> s_eng_blit_pso;
static MTLPixelFormat             s_eng_blit_pso_fmt;
static id<MTLRenderPipelineState> s_eng_depth_pso;
static id<MTLSamplerState>        s_eng_point_sampler;

static NSString* const kEngHelperMSL = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct BOut { float4 pos [[position]]; float2 uv; };\n"
"vertex BOut eng_fullscreen_vs(uint vid [[vertex_id]]) {\n"
"    float2 p = float2((vid << 1) & 2, vid & 2);\n"
"    BOut o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
"    o.uv = float2(p.x, 1.0 - p.y);\n"
"    return o;\n"
"}\n"
"fragment float4 eng_blit_fs(BOut i [[stage_in]],\n"
"                            texture2d<float> src [[texture(0)]],\n"
"                            sampler s [[sampler(0)]]) {\n"
"    return src.sample(s, i.uv);\n"
"}\n"
"fragment float eng_depth_fs(BOut i [[stage_in]],\n"
"                            depth2d<float> src [[texture(0)]],\n"
"                            sampler s [[sampler(0)]]) {\n"
"    return src.sample(s, i.uv);\n"
"}\n";

static u32 eng_obj_add(id<MTLTexture> t)
{
    if (!t) return 0;
    u32 slot;
    if (s_eng_obj_free_count) {
        slot = s_eng_obj_free[--s_eng_obj_free_count];
    } else {
        if (s_eng_obj_count >= ENG_MAX_OBJECTS) return 0;
        slot = s_eng_obj_count++;
    }
    s_eng_obj[slot] = t;
    return slot + 1;
}

static id<MTLTexture> eng_obj(u32 handle)
{
    return (handle && handle <= s_eng_obj_count) ? s_eng_obj[handle - 1] : nil;
}

/* Recorded commands resolve numeric handles during encoding. Do not recycle
 * a released slot until those commands have acquired their Metal resources.
 * Submitted command buffers retain the objects for the GPU's remaining work. */
static void eng_collect_retired_objects(void)
{
    if (s_eng_rec_count) return;
    for (u32 i = 0; i < s_eng_obj_count; ++i) {
        if (!s_eng_obj_retired[i]) continue;
        s_eng_obj_retired[i] = 0;
        s_eng_obj[i] = nil;
        s_eng_obj_free[s_eng_obj_free_count++] = i;
    }
}

static void eng_obj_release(void* user, u32 handle)
{
    (void)user;
    if (!handle || handle > s_eng_obj_count || !s_eng_obj[handle - 1] ||
        s_eng_obj_retired[handle - 1]) return;
    s_eng_obj_retired[handle - 1] = 1;
    /* Any view cut from this object stops being valid with it. */
    for (u32 i = 0; i < s_eng_view_count; i++)
        if (s_eng_view[i].surface == handle) {
            eng_obj_release(NULL, s_eng_view[i].view);
            s_eng_view[i] = s_eng_view[--s_eng_view_count];
            i--;
        }
    eng_collect_retired_objects();
}

static MTLPixelFormat eng_pixel_format(rsx_be_format f)
{
    switch (f) {
    case RSX_BE_FMT_R8:              return MTLPixelFormatR8Unorm;
    case RSX_BE_FMT_R8G8:            return MTLPixelFormatRG8Unorm;
    case RSX_BE_FMT_R8G8B8A8:        return MTLPixelFormatRGBA8Unorm;
    case RSX_BE_FMT_BC1:             return MTLPixelFormatBC1_RGBA;
    case RSX_BE_FMT_BC2:             return MTLPixelFormatBC2_RGBA;
    case RSX_BE_FMT_BC3:             return MTLPixelFormatBC3_RGBA;
    case RSX_BE_FMT_R16:             return MTLPixelFormatR16Unorm;
    case RSX_BE_FMT_R16G16:          return MTLPixelFormatRG16Unorm;
    case RSX_BE_FMT_R16G16F:         return MTLPixelFormatRG16Float;
    case RSX_BE_FMT_R16G16B16A16F:   return MTLPixelFormatRGBA16Float;
    case RSX_BE_FMT_R32F:            return MTLPixelFormatR32Float;
    case RSX_BE_FMT_R32G32B32A32F:   return MTLPixelFormatRGBA32Float;
    default:                         return MTLPixelFormatR8Unorm;
    }
}

/* ---- staging ------------------------------------------------------------- */

static void eng_encode_and_commit(id<MTLTexture> present_dst);

static int eng_stage_reserve(u32 bytes, u32* out_off)
{
    const u32 start = (s_eng_stage_used + ENG_ALIGN - 1u) & ~(ENG_ALIGN - 1u);
    if ((u64)start + bytes > ENG_STAGE_MAX) {
        /* Submit and wait rather than drop the rest of the frame; everything
         * already recorded has been encoded, so the arena is free again. */
        eng_encode_and_commit(nil);
        return eng_stage_reserve(bytes, out_off);
    }
    if (start + bytes > s_eng_stage_cap) {
        u32 cap = s_eng_stage_cap ? s_eng_stage_cap : (4u << 20);
        while (start + bytes > cap) cap *= 2u;
        u8* n = (u8*)realloc(s_eng_stage, cap);
        if (!n) return 0;
        s_eng_stage = n;
        s_eng_stage_cap = cap;
    }
    *out_off = start;
    s_eng_stage_used = start + bytes;
    return 1;
}

static int eng_stage_copy(const void* src, u32 bytes, u32* out_off)
{
    if (!bytes) { *out_off = 0; return 1; }
    if (!eng_stage_reserve(bytes, out_off)) return 0;
    memcpy(s_eng_stage + *out_off, src, bytes);
    return 1;
}

/* ---- lifecycle ----------------------------------------------------------- */

static int eng_init(void* user, u32 width, u32 height)
{
    (void)user; (void)width; (void)height;
    if (!s_dev) return -1;
    NSError* err = nil;
    s_eng_helper_lib = [s_dev newLibraryWithSource:kEngHelperMSL options:nil error:&err];
    if (!s_eng_helper_lib) {
        fprintf(stderr, "[rsx engine/metal] helper shader compile failed: %s\n",
                [[err localizedDescription] UTF8String]);
        return -1;
    }
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter = MTLSamplerMinMagFilterNearest;
    sd.magFilter = MTLSamplerMinMagFilterNearest;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    s_eng_point_sampler = [s_dev newSamplerStateWithDescriptor:sd];
    return s_eng_point_sampler ? 0 : -1;
}

static void eng_shutdown(void* user)
{
    (void)user;
    for (u32 i = 0; i < s_eng_obj_count; i++) s_eng_obj[i] = nil;
    for (u32 i = 0; i < s_eng_pipe_count; i++) {
        s_eng_pipe[i].pso = nil;
        s_eng_pipe[i].ds = nil;
    }
    for (u32 i = 0; i < s_eng_func_count; i++) s_eng_func[i].fn = nil;
    for (u32 i = 0; i < s_eng_samp_count; i++) s_eng_samp[i].samp = nil;
    s_eng_obj_count = s_eng_obj_free_count = 0;
    memset(s_eng_obj_retired, 0, sizeof s_eng_obj_retired);
    s_eng_pipe_count = s_eng_func_count = s_eng_samp_count = s_eng_view_count = 0;
    s_eng_rec_count = 0;
    free(s_eng_stage); s_eng_stage = NULL;
    s_eng_stage_used = s_eng_stage_cap = 0;
    s_eng_helper_lib = nil;
    s_eng_blit_pso = nil;
    s_eng_depth_pso = nil;
    s_eng_point_sampler = nil;
    s_eng_active = 0;
}

static void eng_submit_and_wait(void* user, u32 reason)
{
    (void)user; (void)reason;
    eng_encode_and_commit(nil);
}

/* ---- resources ----------------------------------------------------------- */

static u32 eng_texture_create(void* user, rsx_be_format fmt, u32 w, u32 h,
                              u32 mips, u32 faces, u32 remap, u32 rsx_fmt)
{
    (void)user;
    if (!s_dev || !w || !h) return 0;
    MTLTextureDescriptor* td = [MTLTextureDescriptor new];
    td.textureType      = (faces == 6) ? MTLTextureTypeCube : MTLTextureType2D;
    td.pixelFormat      = eng_pixel_format(fmt);
    td.width            = w;
    td.height           = h;
    td.mipmapLevelCount = mips ? mips : 1;
    td.usage            = MTLTextureUsageShaderRead;
    /* The crossbar's selectors arrive in its own A,R,G,B field order; destR is
     * out[1] .. destA is out[0], as everywhere else in this file. */
    u8 sel[4];
    rsx_texture_component_remap(remap, rsx_fmt & 0x9Fu, sel);
    td.swizzle = MTLTextureSwizzleChannelsMake(
        swizzle_sel(sel[1]), swizzle_sel(sel[2]),
        swizzle_sel(sel[3]), swizzle_sel(sel[0]));
    return eng_obj_add([s_dev newTextureWithDescriptor:td]);
}

static void eng_texture_upload(void* user, u32 handle, u32 face, u32 mip,
                               u32 w, u32 h, const void* src, u32 row_bytes,
                               u32 rows)
{
    (void)user; (void)rows;
    id<MTLTexture> t = eng_obj(handle);
    if (!t || !src || !row_bytes) return;
    [t replaceRegion:MTLRegionMake2D(0, 0, w, h)
         mipmapLevel:mip
               slice:face
           withBytes:src
         bytesPerRow:row_bytes
       bytesPerImage:0];
}

static u32 eng_color_target_create(void* user, rsx_be_format fmt, u32 w, u32 h,
                                   const void* seed, u32 seed_row_bytes)
{
    (void)user;
    if (!s_dev || !w || !h) return 0;
    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:eng_pixel_format(fmt)
                                                           width:w height:h
                                                       mipmapped:NO];
    /* PixelFormatView because a unit sampling this target wears its own
     * TEXTURE_CONTROL1 crossbar as a view swizzle. */
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead |
               MTLTextureUsagePixelFormatView;
    id<MTLTexture> t = [s_dev newTextureWithDescriptor:td];
    if (!t) return 0;
    if (seed && seed_row_bytes)
        [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0
               withBytes:seed bytesPerRow:seed_row_bytes];
    return eng_obj_add(t);
}

static u32 eng_surface_view(void* user, u32 surface, u32 remap, u32 rsx_format)
{
    (void)user;
    id<MTLTexture> t = eng_obj(surface);
    if (!t) return 0;
    for (u32 i = 0; i < s_eng_view_count; i++)
        if (s_eng_view[i].surface == surface && s_eng_view[i].remap == remap &&
            s_eng_view[i].format == rsx_format)
            return s_eng_view[i].view;
    if (s_eng_view_count >= ENG_MAX_VIEWS) return 0;
    u8 sel[4];
    rsx_texture_component_remap(remap, rsx_format & 0x9Fu, sel);
    id<MTLTexture> v =
        [t newTextureViewWithPixelFormat:[t pixelFormat]
                             textureType:MTLTextureType2D
                                  levels:NSMakeRange(0, 1)
                                  slices:NSMakeRange(0, 1)
                                 swizzle:MTLTextureSwizzleChannelsMake(
                                             swizzle_sel(sel[1]), swizzle_sel(sel[2]),
                                             swizzle_sel(sel[3]), swizzle_sel(sel[0]))];
    const u32 handle = eng_obj_add(v);
    if (!handle) return 0;
    s_eng_view[s_eng_view_count].surface = surface;
    s_eng_view[s_eng_view_count].remap   = remap;
    s_eng_view[s_eng_view_count].format  = rsx_format;
    s_eng_view[s_eng_view_count].view    = handle;
    s_eng_view_count++;
    return handle;
}

static u32 eng_depth_target_create(void* user, u32 w, u32 h)
{
    (void)user;
    if (!s_dev || !w || !h) return 0;
    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTL_DEPTH_FORMAT
                                                           width:w height:h
                                                       mipmapped:NO];
    /* ShaderRead as well as RenderTarget: depth-as-texture resolves through a
     * pass that samples this as a depth2d, so it cannot be write-only. */
    td.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    return eng_obj_add([s_dev newTextureWithDescriptor:td]);
}

/* The sampleable copy of a depth target. The copy is a record in the stream,
 * not an immediate blit, because it has to observe the depth the passes
 * recorded so far actually wrote. */
static u32 eng_depth_snapshot(void* user, u32 depth, u32 w, u32 h)
{
    (void)user;
    if (!eng_obj(depth) || !s_eng_helper_lib) return 0;
    if (s_eng_rec_count >= ENG_MAX_RECORDS) { s_eng_dropped++; return 0; }
    if (!s_eng_depth_pso) {
        MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction   = [s_eng_helper_lib newFunctionWithName:@"eng_fullscreen_vs"];
        pd.fragmentFunction = [s_eng_helper_lib newFunctionWithName:@"eng_depth_fs"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatR32Float;
        NSError* err = nil;
        s_eng_depth_pso = [s_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!s_eng_depth_pso) {
            fprintf(stderr, "[rsx engine/metal] depth resolve pipeline failed: %s\n",
                    [[err localizedDescription] UTF8String]);
            return 0;
        }
    }
    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
                                                           width:w height:h
                                                       mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    const u32 dst = eng_obj_add([s_dev newTextureWithDescriptor:td]);
    if (!dst) return 0;

    EngRecord* r = &s_eng_rec[s_eng_rec_count++];
    memset(r, 0, sizeof *r);
    r->kind = ENG_REC_DEPTH_RESOLVE;
    r->depth = depth;
    r->resolve_dst = dst;
    return dst;
}

/* ---- pipelines ----------------------------------------------------------- */

static id<MTLFunction> eng_function(const char* hlsl, int stage,
                                    const char* what)
{
    const u64 hash = fnv1a64(hlsl, (u32)strlen(hlsl), 1469598103934665603ull);
    for (u32 i = 0; i < s_eng_func_count; i++)
        if (s_eng_func[i].hash == hash) return s_eng_func[i].fn;
    if (s_eng_func_count >= ENG_MAX_FUNCS) return nil;

    id<MTLFunction> fn = nil;
    char name[64];
    snprintf(name, sizeof name, "%s_%016llx.hlsl", what, (unsigned long long)hash);
    dump_shader(name, hlsl);
    if (rsx_hlsl_to_msl(hlsl, stage, s_msl, sizeof s_msl, s_log, sizeof s_log) != 0) {
        fprintf(stderr, "[rsx engine/metal] %s %016llx: %s\n", what,
                (unsigned long long)hash, s_log);
    } else {
        snprintf(name, sizeof name, "%s_%016llx.msl", what, (unsigned long long)hash);
        dump_shader(name, s_msl);
        snprintf(name, sizeof name, "%s %016llx", what, (unsigned long long)hash);
        fn = compile_guest_function(s_msl, name);
    }
    s_eng_func[s_eng_func_count].hash = hash;
    s_eng_func[s_eng_func_count].fn   = fn;
    s_eng_func_count++;
    return fn;
}

static u32 eng_pipeline_create(void* user, const char* vs_hlsl, const char* ps_hlsl,
                               const rsx_be_render_state* rs,
                               const rsx_vertex_layout_plan* layout,
                               u32 vertex_stride, rsx_be_format rt_fmt,
                               u32 rt_count)
{
    (void)user;
    if (!s_dev || !s_guest_shaders || !vertex_stride) return 0;
    if (!rt_count) rt_count = 1;
    if (rt_count > RSX_BE_MAX_COLOR_TARGETS) rt_count = RSX_BE_MAX_COLOR_TARGETS;
    if (s_eng_pipe_count >= ENG_MAX_PIPES) return 0;
    id<MTLFunction> vs = eng_function(vs_hlsl, RSX_SHADER_STAGE_VERTEX, "vp");
    if (!vs) return 0;
    id<MTLFunction> fs = eng_function(ps_hlsl, RSX_SHADER_STAGE_FRAGMENT, "fp");
    if (!fs) return 0;

    /* Attribute index is the layout SLOT, not the guest's ATTRn number.
     * glslang numbers an HLSL input struct's members in DECLARATION order --
     * a compact VSInput declaring ATTR0 and ATTR3 puts a3 at location 1 --
     * and spirv-cross carries those locations into [[attribute(n)]]. The two
     * agree for the all-sixteen layout, where slot is the attribute number,
     * which is why the vtable path above can index by either. */
    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
    for (u32 slot = 0; slot < layout->count; slot++) {
        vd.attributes[slot].format      = MTLVertexFormatFloat4;
        vd.attributes[slot].offset      = (NSUInteger)(slot * 16u);
        vd.attributes[slot].bufferIndex = MTL_VB_INDEX;
    }
    vd.layouts[MTL_VB_INDEX].stride = vertex_stride;

    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction   = vs;
    pd.fragmentFunction = fs;
    pd.vertexDescriptor = vd;
    pd.depthAttachmentPixelFormat   = MTL_DEPTH_FORMAT;
    pd.stencilAttachmentPixelFormat = MTL_DEPTH_FORMAT;
    /* nv40 COLOR_MASK byte layout: B=[0:7] G=[8:15] R=[16:23] A=[24:31], any
     * nonzero byte turning that channel on. */
    MTLColorWriteMask wm = MTLColorWriteMaskNone;
    if ((rs->color_mask >>  0) & 0xFF) wm |= MTLColorWriteMaskBlue;
    if ((rs->color_mask >>  8) & 0xFF) wm |= MTLColorWriteMaskGreen;
    if ((rs->color_mask >> 16) & 0xFF) wm |= MTLColorWriteMaskRed;
    if ((rs->color_mask >> 24) & 0xFF) wm |= MTLColorWriteMaskAlpha;
    /* Every attachment of an MRT set takes target A's blend and colour mask,
     * as the vtable path's pso_for does with RTVFormats[1..]: a
     * zero-initialised secondary attachment writes nothing at all. The count
     * is rt_count, the set the engine resolved, and the fragment function's
     * own output count is whatever its program exports; Metal accepts either
     * side being the larger one, so the two do not have to agree. */
    for (u32 r = 0; r < rt_count; r++) {
        MTLRenderPipelineColorAttachmentDescriptor* ca = pd.colorAttachments[r];
        ca.pixelFormat = eng_pixel_format(rt_fmt);
        ca.writeMask   = wm;
        if (rs->blend_enable) {
            ca.blendingEnabled             = YES;
            ca.sourceRGBBlendFactor        = blend_factor_to_metal(rs->sf_rgb);
            ca.destinationRGBBlendFactor   = blend_factor_to_metal(rs->df_rgb);
            ca.sourceAlphaBlendFactor      = blend_factor_to_metal(rs->sf_a);
            ca.destinationAlphaBlendFactor = blend_factor_to_metal(rs->df_a);
            ca.rgbBlendOperation           = blend_equation_to_metal(rs->eq_rgb);
            ca.alphaBlendOperation         = blend_equation_to_metal(rs->eq_a);
        }
    }

    NSError* err = nil;
    id<MTLRenderPipelineState> pso =
        [s_dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!pso) {
        fprintf(stderr, "[rsx engine/metal] pipeline state failed: %s\n",
                [[err localizedDescription] UTF8String]);
        return 0;
    }

    /* Depth and stencil are pipeline state in D3D12 and an encoder object in
     * Metal, so the engine's render state splits here rather than upstream. */
    MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = rs->depth_test ? gcm_compare_to_metal(rs->depth_func)
                                             : MTLCompareFunctionAlways;
    dd.depthWriteEnabled    = (rs->depth_test && rs->depth_write) ? YES : NO;
    if (rs->stencil_enable) {
        MTLStencilDescriptor* fsd = [MTLStencilDescriptor new];
        fsd.stencilCompareFunction    = gcm_compare_to_metal(rs->s_func);
        fsd.stencilFailureOperation   = gcm_stencil_op_to_metal(rs->s_fail);
        fsd.depthFailureOperation     = gcm_stencil_op_to_metal(rs->s_zfail);
        fsd.depthStencilPassOperation = gcm_stencil_op_to_metal(rs->s_zpass);
        fsd.readMask                  = rs->s_func_mask & 0xFFu;
        fsd.writeMask                 = rs->s_write_mask & 0xFFu;
        dd.frontFaceStencil = fsd;
        if (rs->stencil_two_sided) {
            MTLStencilDescriptor* bsd = [MTLStencilDescriptor new];
            bsd.stencilCompareFunction    = gcm_compare_to_metal(rs->bs_func);
            bsd.stencilFailureOperation   = gcm_stencil_op_to_metal(rs->bs_fail);
            bsd.depthFailureOperation     = gcm_stencil_op_to_metal(rs->bs_zfail);
            bsd.depthStencilPassOperation = gcm_stencil_op_to_metal(rs->bs_zpass);
            bsd.readMask                  = rs->s_func_mask & 0xFFu;
            bsd.writeMask                 = rs->s_write_mask & 0xFFu;
            dd.backFaceStencil = bsd;
        } else {
            /* Two-sided off: nv40 applies the front state to both faces. */
            dd.backFaceStencil = fsd;
        }
    }
    id<MTLDepthStencilState> ds = [s_dev newDepthStencilStateWithDescriptor:dd];
    if (!ds) return 0;

    const u32 slot = s_eng_pipe_count++;
    s_eng_pipe[slot].pso = pso;
    s_eng_pipe[slot].ds  = ds;
    /* CULL_FACE FRONT=0x0404 BACK=0x0405 FRONT_AND_BACK=0x0408 (front here,
     * as in the D3D12 engine); FRONT_FACE CW=0x0900 CCW=0x0901. */
    s_eng_pipe[slot].cull = MTLCullModeNone;
    if (rs->cull_enable && rs->cull_face)
        s_eng_pipe[slot].cull = (rs->cull_face == 0x0404u || rs->cull_face == 0x0408u)
                                    ? MTLCullModeFront
                                    : (rs->cull_face == 0x0405u ? MTLCullModeBack
                                                                : MTLCullModeNone);
    s_eng_pipe[slot].winding = (rs->front_face == 0x0901u)
                                   ? MTLWindingCounterClockwise
                                   : MTLWindingClockwise;
    return slot + 1;
}

static void eng_pipeline_release(void* user, u32 pipeline)
{
    (void)user;
    if (pipeline && pipeline <= s_eng_pipe_count) {
        s_eng_pipe[pipeline - 1].pso = nil;
        s_eng_pipe[pipeline - 1].ds  = nil;
    }
}

/* ---- per-draw binding ---------------------------------------------------- */

static int eng_sampler_slot(const rsx_be_sampler_desc* d)
{
    const u64 key = (u64)d->min_linear | ((u64)d->mag_linear << 1)
                  | ((u64)d->mip_linear << 2) | ((u64)d->mip_present << 3)
                  | ((u64)d->wrap_s << 4) | ((u64)d->wrap_t << 8)
                  | ((u64)d->wrap_r << 12)
                  | ((u64)(u32)(d->min_lod * 256.0f) << 16)
                  | ((u64)(u32)(d->max_lod * 256.0f) << 32);
    for (u32 i = 0; i < s_eng_samp_count; i++)
        if (s_eng_samp[i].key == key) return (int)i;
    if (s_eng_samp_count >= ENG_MAX_SAMPLERS) return -1;
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter = d->min_linear ? MTLSamplerMinMagFilterLinear
                                 : MTLSamplerMinMagFilterNearest;
    sd.magFilter = d->mag_linear ? MTLSamplerMinMagFilterLinear
                                 : MTLSamplerMinMagFilterNearest;
    sd.mipFilter = !d->mip_present ? MTLSamplerMipFilterNotMipmapped
                 : d->mip_linear   ? MTLSamplerMipFilterLinear
                                   : MTLSamplerMipFilterNearest;
    sd.lodMinClamp = d->min_lod;
    sd.lodMaxClamp = d->max_lod;
    sd.sAddressMode = gcm_wrap_to_metal(d->wrap_s);
    sd.tAddressMode = gcm_wrap_to_metal(d->wrap_t);
    sd.rAddressMode = gcm_wrap_to_metal(d->wrap_r);
    sd.borderColor  = MTLSamplerBorderColorTransparentBlack;
    id<MTLSamplerState> s = [s_dev newSamplerStateWithDescriptor:sd];
    if (!s) return -1;
    const int slot = (int)s_eng_samp_count++;
    s_eng_samp[slot].key  = key;
    s_eng_samp[slot].samp = s;
    return slot;
}

static void eng_bind_targets(void* user, const u32* surfaces, u32 count,
                             u32 depth)
{
    (void)user;
    if (count > RSX_BE_MAX_COLOR_TARGETS) count = RSX_BE_MAX_COLOR_TARGETS;
    for (u32 i = 0; i < RSX_BE_MAX_COLOR_TARGETS; i++)
        s_eng_pending.rt[i] = (i < count) ? surfaces[i] : 0;
    s_eng_pending.nrt   = count;
    s_eng_pending.depth = depth;
}

static void eng_bind_pipeline(void* user, u32 pipeline)
{ (void)user; s_eng_pending.pipeline = pipeline; }

static void eng_bind_vs_constants(void* user, const void* data, u32 bytes)
{
    (void)user;
    if (!eng_stage_copy(data, bytes, &s_eng_pending.vs_cb_off)) bytes = 0;
    s_eng_pending.vs_cb_bytes = bytes;
}

static void eng_bind_ps_constants(void* user, const void* data, u32 bytes)
{
    (void)user;
    if (!eng_stage_copy(data, bytes, &s_eng_pending.ps_cb_off)) bytes = 0;
    s_eng_pending.ps_cb_bytes = bytes;
}

static void eng_bind_textures(void* user, const u32* textures,
                              const rsx_be_sampler_desc* samplers, u32 mask)
{
    (void)user;
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
        s_eng_pending.tex[u]  = ((mask >> u) & 1u) ? textures[u] : 0;
        s_eng_pending.samp[u] = ((mask >> u) & 1u) ? eng_sampler_slot(&samplers[u]) : -1;
    }
}

static void eng_bind_vertex_textures(void* user, const u32* textures,
                                     const rsx_be_sampler_desc* samplers, u32 mask)
{
    (void)user;
    for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++) {
        s_eng_pending.vtex[u]  = ((mask >> u) & 1u) ? textures[u] : 0;
        s_eng_pending.vsamp[u] = ((mask >> u) & 1u) ? eng_sampler_slot(&samplers[u]) : -1;
    }
}

static void eng_set_viewport(void* user, float x, float y, float w, float h)
{
    (void)user;
    s_eng_pending.vp[0] = x; s_eng_pending.vp[1] = y;
    s_eng_pending.vp[2] = w; s_eng_pending.vp[3] = h;
}

static void eng_set_scissor(void* user, u32 x, u32 y, u32 w, u32 h)
{
    (void)user;
    s_eng_pending.sc[0] = x; s_eng_pending.sc[1] = y;
    s_eng_pending.sc[2] = w; s_eng_pending.sc[3] = h;
}

static void eng_set_stencil_ref(void* user, u32 ref)
{ (void)user; s_eng_pending.stencil_ref = ref; }

static void eng_draw(void* user, rsx_topology topology, const void* vertices,
                     u32 vertex_count, u32 stride, const u32* indices,
                     u32 index_count)
{
    (void)user;
    if (!vertex_count || !stride) return;
    if (s_eng_rec_count >= ENG_MAX_RECORDS) { s_eng_dropped++; return; }
    u32 vb_off = 0, ib_off = 0;
    if (!eng_stage_copy(vertices, vertex_count * stride, &vb_off)) return;
    if (indices && index_count &&
        !eng_stage_copy(indices, index_count * (u32)sizeof(u32), &ib_off))
        return;

    EngRecord* r = &s_eng_rec[s_eng_rec_count++];
    *r = s_eng_pending;
    r->kind         = ENG_REC_DRAW;
    r->topology     = topo_to_metal(topology);
    r->vb_off       = vb_off;
    r->stride       = stride;
    r->vertex_count = vertex_count;
    r->ib_off       = ib_off;
    r->index_count  = indices ? index_count : 0;
}

static void eng_clear_color(void* user, u32 surface, const float rgba[4])
{
    (void)user;
    /* The debug hook reports the last colour the guest asked for whether or
     * not the record stream had room for the clear. */
    s_clear_argb = ((u32)(rgba[3] * 255.0f + 0.5f) << 24) |
                   ((u32)(rgba[0] * 255.0f + 0.5f) << 16) |
                   ((u32)(rgba[1] * 255.0f + 0.5f) <<  8) |
                    (u32)(rgba[2] * 255.0f + 0.5f);
    if (s_eng_rec_count >= ENG_MAX_RECORDS) { s_eng_dropped++; return; }
    EngRecord* r = &s_eng_rec[s_eng_rec_count++];
    memset(r, 0, sizeof *r);
    r->kind  = ENG_REC_CLEAR_COLOR;
    r->rt[0] = surface;
    r->nrt   = 1;
    memcpy(r->clear_rgba, rgba, sizeof r->clear_rgba);
}

static void eng_clear_depth_stencil(void* user, u32 depth, u32 flags,
                                    float depth_value, u8 stencil)
{
    (void)user;
    if (s_eng_rec_count >= ENG_MAX_RECORDS) { s_eng_dropped++; return; }
    EngRecord* r = &s_eng_rec[s_eng_rec_count++];
    memset(r, 0, sizeof *r);
    r->kind = ENG_REC_CLEAR_DS;
    r->depth = depth;
    r->clear_flags = flags;
    r->clear_depth = depth_value;
    r->clear_stencil = stencil;
}

/* ---- replay -------------------------------------------------------------- */

static void eng_encode_draw(id<MTLRenderCommandEncoder> enc, const EngRecord* r,
                            id<MTLBuffer> stage)
{
    if (!r->pipeline || r->pipeline > s_eng_pipe_count) return;
    const EngPipeline* p = &s_eng_pipe[r->pipeline - 1];
    if (!p->pso) return;
    [enc setRenderPipelineState:p->pso];
    [enc setDepthStencilState:p->ds];
    [enc setCullMode:p->cull];
    [enc setFrontFacingWinding:p->winding];
    [enc setStencilReferenceValue:r->stencil_ref];
    MTLViewport vp = { r->vp[0], r->vp[1], r->vp[2], r->vp[3], 0.0, 1.0 };
    [enc setViewport:vp];
    if (r->sc[2] && r->sc[3])
        [enc setScissorRect:(MTLScissorRect){ r->sc[0], r->sc[1], r->sc[2], r->sc[3] }];
    [enc setVertexBuffer:stage offset:r->vb_off atIndex:MTL_VB_INDEX];
    if (r->vs_cb_bytes) [enc setVertexBuffer:stage offset:r->vs_cb_off atIndex:0];
    if (r->ps_cb_bytes) [enc setFragmentBuffer:stage offset:r->ps_cb_off atIndex:1];
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
        id<MTLTexture> t = eng_obj(r->tex[u]);
        [enc setFragmentTexture:(t ? t : s_null_tex) atIndex:(NSUInteger)u];
        [enc setFragmentSamplerState:(r->samp[u] >= 0 ? s_eng_samp[r->samp[u]].samp
                                                      : s_default_sampler)
                             atIndex:(NSUInteger)u];
    }
    for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++) {
        id<MTLTexture> t = eng_obj(r->vtex[u]);
        [enc setVertexTexture:(t ? t : s_null_tex) atIndex:MTL_VTEX_INDEX(u)];
        [enc setVertexSamplerState:(r->vsamp[u] >= 0 ? s_eng_samp[r->vsamp[u]].samp
                                                     : s_default_sampler)
                           atIndex:MTL_VSAMP_INDEX(u)];
    }
    /* An indexed draw is really indexed here: the index buffer is what keeps a
     * strip's shared vertices from being fetched and uploaded per triangle.
     * Only a rebuilt triangle list is ever indexed; points and lines arrive as
     * the guest issued them. */
    if (r->index_count)
        [enc drawIndexedPrimitives:r->topology
                        indexCount:r->index_count
                         indexType:MTLIndexTypeUInt32
                       indexBuffer:stage
                 indexBufferOffset:r->ib_off];
    else
        [enc drawPrimitives:r->topology
                vertexStart:0 vertexCount:r->vertex_count];
}

/* Is this record aimed at exactly these colour attachments, in this order? */
static int eng_record_targets_are(const EngRecord* r, const u32* rt, u32 nrt)
{
    if (r->nrt != nrt) return 0;
    for (u32 k = 0; k < nrt; k++) if (r->rt[k] != rt[k]) return 0;
    return 1;
}

/* One pass per contiguous run of draws against the same attachments, with any
 * clears immediately ahead of it folded into its load actions. A clear whose
 * targets the following draw does not share becomes a pass of its own, which
 * is what keeps a mid-frame clear ordered against the draws around it. */
static void eng_encode_records(id<MTLCommandBuffer> cb, id<MTLBuffer> stage)
{
    u32 i = 0;
    while (i < s_eng_rec_count) {
        if (s_eng_rec[i].kind == ENG_REC_DEPTH_RESOLVE) {
            const EngRecord* r = &s_eng_rec[i++];
            id<MTLTexture> src = eng_obj(r->depth);
            id<MTLTexture> dst = eng_obj(r->resolve_dst);
            if (!src || !dst || !s_eng_depth_pso) continue;
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture     = dst;
            rp.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
            if (!e) continue;
            [e setRenderPipelineState:s_eng_depth_pso];
            [e setFragmentTexture:src atIndex:0];
            [e setFragmentSamplerState:s_eng_point_sampler atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [e endEncoding];
            continue;
        }

        /* The clears queued ahead of this pass. A colour clear names one
         * surface and an MRT set arrives as one clear per target, so they are
         * collected rather than compared: they become load actions on the
         * pass below when the draw that follows writes the same targets. */
        u32 n_cc = 0;
        u32 cc_surf[RSX_BE_MAX_COLOR_TARGETS];
        float cc_rgba[RSX_BE_MAX_COLOR_TARGETS][4];
        int have_clear_ds = 0;
        u32 cd = 0, ds_flags = 0;
        float cdepth = 1.0f;
        u8 cstencil = 0;
        while (i < s_eng_rec_count && s_eng_rec[i].kind != ENG_REC_DRAW &&
               s_eng_rec[i].kind != ENG_REC_DEPTH_RESOLVE) {
            const EngRecord* r = &s_eng_rec[i];
            if (r->kind == ENG_REC_CLEAR_COLOR) {
                u32 k = 0;
                while (k < n_cc && cc_surf[k] != r->rt[0]) k++;
                if (k == n_cc) {
                    if (n_cc == RSX_BE_MAX_COLOR_TARGETS) break;
                    cc_surf[n_cc++] = r->rt[0];
                }
                memcpy(cc_rgba[k], r->clear_rgba, sizeof cc_rgba[k]);
            } else {
                if (have_clear_ds && r->depth != cd) break;
                cd = r->depth;
                ds_flags |= r->clear_flags;
                cdepth = r->clear_depth;
                cstencil = r->clear_stencil;
                have_clear_ds = 1;
            }
            i++;
        }

        /* What the pass attaches: the following draw's targets when it writes
         * every surface the clears named, else the cleared surfaces on their
         * own -- which is what keeps a mid-frame clear ordered against the
         * draws around it. */
        u32 rt[RSX_BE_MAX_COLOR_TARGETS];
        u32 nrt = n_cc;
        for (u32 k = 0; k < n_cc; k++) rt[k] = cc_surf[k];
        u32 depth = have_clear_ds ? cd : 0;
        if (i < s_eng_rec_count && s_eng_rec[i].kind == ENG_REC_DRAW &&
            (!have_clear_ds || s_eng_rec[i].depth == cd)) {
            const EngRecord* d = &s_eng_rec[i];
            int covered = 1;
            for (u32 k = 0; k < n_cc && covered; k++) {
                int found = 0;
                for (u32 j = 0; j < d->nrt; j++)
                    if (d->rt[j] == cc_surf[k]) found = 1;
                covered = found;
            }
            if (covered) {
                nrt = d->nrt;
                for (u32 k = 0; k < nrt; k++) rt[k] = d->rt[k];
                depth = d->depth;
            }
        }

        /* Metal wants every attachment the same size, and the engine registers
         * an MRT set's members at target A's, so a set whose members disagree
         * is one this frame cannot honour: it stops at the gap, as the vtable
         * path's begin_pass does. */
        id<MTLTexture> tex[RSX_BE_MAX_COLOR_TARGETS];
        u32 attach = 0;
        for (u32 k = 0; k < nrt; k++) {
            id<MTLTexture> t = eng_obj(rt[k]);
            if (!t) break;
            if (attach && ([t width]  != [tex[0] width] ||
                           [t height] != [tex[0] height])) break;
            tex[attach++] = t;
        }
        id<MTLTexture> zbuf = eng_obj(depth);
        if (!attach && !zbuf) { if (i < s_eng_rec_count && s_eng_rec[i].kind == ENG_REC_DRAW) i++; continue; }
        /* A pipeline always declares the depth attachment, so a draw pass
         * always has to have one; the display's shared buffer is the fallback
         * for a guest that never declared a zeta. */
        if (attach && !zbuf) zbuf = s_depth;

        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        for (u32 k = 0; k < attach; k++) {
            rp.colorAttachments[k].texture     = tex[k];
            rp.colorAttachments[k].storeAction = MTLStoreActionStore;
            int cleared = -1;
            for (u32 c = 0; c < n_cc; c++) if (cc_surf[c] == rt[k]) cleared = (int)c;
            if (cleared >= 0) {
                rp.colorAttachments[k].loadAction = MTLLoadActionClear;
                rp.colorAttachments[k].clearColor =
                    MTLClearColorMake(cc_rgba[cleared][0], cc_rgba[cleared][1],
                                      cc_rgba[cleared][2], cc_rgba[cleared][3]);
            } else {
                rp.colorAttachments[k].loadAction = MTLLoadActionLoad;
            }
        }
        if (zbuf) {
            const int clear_here = have_clear_ds && depth == cd;
            rp.depthAttachment.texture       = zbuf;
            rp.depthAttachment.storeAction   = MTLStoreActionStore;
            rp.depthAttachment.loadAction    =
                (clear_here && (ds_flags & RSX_BE_CLEAR_DEPTH))
                    ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.depthAttachment.clearDepth    = (double)cdepth;
            rp.stencilAttachment.texture     = zbuf;
            rp.stencilAttachment.storeAction = MTLStoreActionStore;
            rp.stencilAttachment.loadAction  =
                (clear_here && (ds_flags & RSX_BE_CLEAR_STENCIL))
                    ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.stencilAttachment.clearStencil = cstencil;
        }
        if (!attach) {
            rp.renderTargetWidth  = [zbuf width];
            rp.renderTargetHeight = [zbuf height];
        }

        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        /* A draw's pipeline declares as many colour attachments as its record
         * named, so a pass that could not bind them all takes its clears and
         * skips those draws rather than encoding a mismatch. */
        if (!enc || attach != nrt) {
            if (enc) [enc endEncoding];
            while (i < s_eng_rec_count && s_eng_rec[i].kind == ENG_REC_DRAW &&
                   eng_record_targets_are(&s_eng_rec[i], rt, nrt)) i++;
            continue;
        }
        while (i < s_eng_rec_count && s_eng_rec[i].kind == ENG_REC_DRAW &&
               eng_record_targets_are(&s_eng_rec[i], rt, nrt) &&
               (s_eng_rec[i].depth == depth ||
                (!s_eng_rec[i].depth && zbuf == s_depth))) {
            eng_encode_draw(enc, &s_eng_rec[i], stage);
            i++;
        }
        [enc endEncoding];
    }
}

/* Put a surface on the drawable. A straight blit would need matching formats
 * and sizes; a full-screen sample needs neither, and the guest's surfaces are
 * routinely a different size from the window. */
static void eng_blit_to_display(id<MTLCommandBuffer> cb, id<MTLTexture> src,
                                id<MTLTexture> dst)
{
    if (!src || !dst || !s_eng_helper_lib) return;
    if (!s_eng_blit_pso || s_eng_blit_pso_fmt != [dst pixelFormat]) {
        MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction   = [s_eng_helper_lib newFunctionWithName:@"eng_fullscreen_vs"];
        pd.fragmentFunction = [s_eng_helper_lib newFunctionWithName:@"eng_blit_fs"];
        pd.colorAttachments[0].pixelFormat = [dst pixelFormat];
        NSError* err = nil;
        s_eng_blit_pso = [s_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!s_eng_blit_pso) {
            fprintf(stderr, "[rsx engine/metal] present pipeline failed: %s\n",
                    [[err localizedDescription] UTF8String]);
            return;
        }
        s_eng_blit_pso_fmt = [dst pixelFormat];
    }
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture     = dst;
    rp.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    if (!e) return;
    [e setRenderPipelineState:s_eng_blit_pso];
    [e setFragmentTexture:src atIndex:0];
    [e setFragmentSamplerState:s_eng_point_sampler atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}

/* Encode everything recorded so far and commit it.
 *
 * The staging arena is recycled the moment this returns, which is safe
 * without a wait: newBufferWithBytes COPIES, so what the GPU reads is the
 * MTLBuffer and not the arena, and a command buffer retains every resource it
 * uses. So only headless waits, where the readback has to observe a finished
 * frame -- there the stall is the point. A windowed present is paced by the
 * in-flight semaphore instead, the same bound the vtable path uses, because
 * waiting on every frame is the one pattern where a driver cannot hide its
 * encoding work. */
static void eng_encode_and_commit(id<MTLTexture> present_dst)
{
    @autoreleasepool {
        id<CAMetalDrawable> drawable = nil;
        id<MTLTexture> dst = nil;
        const int windowed_present = (present_dst && !s_headless);
        if (windowed_present) dispatch_semaphore_wait(s_inflight, DISPATCH_TIME_FOREVER);
        if (present_dst) {
            if (s_headless) {
                dst = s_offscreen;
            } else {
                drawable = [s_layer nextDrawable];
                if (!drawable) {           /* compositor is busy; skip */
                    dispatch_semaphore_signal(s_inflight);
                    return;
                }
                dst = [drawable texture];
            }
        }

        id<MTLBuffer> stage = nil;
        if (s_eng_stage_used)
            stage = [s_dev newBufferWithBytes:s_eng_stage length:s_eng_stage_used
                                      options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [s_queue commandBuffer];
        eng_encode_records(cb, stage);
        if (present_dst && dst) eng_blit_to_display(cb, present_dst, dst);
        if (drawable) [cb presentDrawable:drawable];
        if (windowed_present) {
            dispatch_semaphore_t sem = s_inflight;
            [cb addCompletedHandler:^(id<MTLCommandBuffer> _unused) {
                (void)_unused;
                dispatch_semaphore_signal(sem);
            }];
            [cb commit];
        } else {
            [cb commit];
            [cb waitUntilCompleted];
        }

        if (s_eng_dropped) {
            fprintf(stderr, "[rsx engine/metal] dropped %u record(s) (cap %d)\n",
                    s_eng_dropped, ENG_MAX_RECORDS);
            s_eng_dropped = 0;
        }
        s_eng_rec_count  = 0;
        s_eng_stage_used = 0;
        eng_collect_retired_objects();
    }
}

/* Opt-in readback of the game surface, useful on a locked Mac or in CI.
 * Headless submits have completed; windowed captures fence the same queue. */
static void eng_dump_frame(id<MTLTexture> src)
{
    const char* path = getenv("PS3RECOMP_METAL_FRAME_DUMP");
    if (!path || !*path) return;
    static unsigned frame;
    if ((++frame % 120) != 0) return;
    if ([src pixelFormat] != MTLPixelFormatRGBA8Unorm &&
        [src pixelFormat] != MTLPixelFormatBGRA8Unorm) return;
    size_t w = [src width], h = [src height];
    if (!w || !h || w > 16384 || h > 16384) return;
    unsigned char* rgba = malloc(w * h * 4);
    if (!rgba) return;
    if (!s_headless) {
        id<MTLCommandBuffer> fence = [s_queue commandBuffer];
        [fence commit];
        [fence waitUntilCompleted];
    }
    [src getBytes:rgba bytesPerRow:w * 4
      fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    FILE* f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%zu %zu\n255\n", w, h);
        int bgra = [src pixelFormat] == MTLPixelFormatBGRA8Unorm;
        for (size_t p = 0; p < w * h; ++p) {
            unsigned char rgb[3] = {rgba[p*4 + (bgra ? 2 : 0)], rgba[p*4+1],
                                    rgba[p*4 + (bgra ? 0 : 2)]};
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
        fprintf(stderr, "[rsx engine/metal] captured frame %u to %s\n", frame, path);
    }
    free(rgba);
}

static void eng_present(void* user, u32 surface)
{
    (void)user;
    id<MTLTexture> src = eng_obj(surface);
    if (!src) { eng_encode_and_commit(nil); return; }
    eng_encode_and_commit(src);
    eng_dump_frame(src);
}

static void eng_readback(void* user, u32 surface, u32 x, u32 y, u32 w, u32 h,
                         void* out, u32 out_pitch)
{
    (void)user;
    id<MTLTexture> t = eng_obj(surface);
    if (!t || !out || !out_pitch || !w || !h) return;
    if (x + w > [t width] || y + h > [t height]) return;
    [t getBytes:out bytesPerRow:out_pitch
     fromRegion:MTLRegionMake2D(x, y, w, h) mipmapLevel:0];
}

static const rsx_draw_backend s_engine_backend = {
    .user                 = NULL,
    .init                 = eng_init,
    .shutdown             = eng_shutdown,
    .submit_and_wait      = eng_submit_and_wait,
    .texture_create       = eng_texture_create,
    .texture_upload       = eng_texture_upload,
    .texture_release      = eng_obj_release,
    .color_target_create  = eng_color_target_create,
    .color_target_release = eng_obj_release,
    .surface_view         = eng_surface_view,
    .depth_target_create  = eng_depth_target_create,
    .depth_target_release = eng_obj_release,
    .depth_snapshot       = eng_depth_snapshot,
    .pipeline_create      = eng_pipeline_create,
    .pipeline_release     = eng_pipeline_release,
    .bind_targets         = eng_bind_targets,
    .bind_pipeline        = eng_bind_pipeline,
    .bind_vs_constants    = eng_bind_vs_constants,
    .bind_ps_constants    = eng_bind_ps_constants,
    .bind_textures        = eng_bind_textures,
    .bind_vertex_textures = eng_bind_vertex_textures,
    .set_viewport         = eng_set_viewport,
    .set_scissor          = eng_set_scissor,
    .set_stencil_ref      = eng_set_stencil_ref,
    .draw                 = eng_draw,
    .clear_color          = eng_clear_color,
    .clear_depth_stencil  = eng_clear_depth_stencil,
    .present              = eng_present,
    .readback             = eng_readback,
};

/* ---- public API ---------------------------------------------------------- */

int rsx_metal_backend_init(u32 width, u32 height, const char* title)
{
    @autoreleasepool {
        if (width)  s_width  = width;
        if (height) s_height = height;

        const char* hl = getenv("PS3RECOMP_METAL_HEADLESS");
        s_headless = (hl && *hl && *hl != '0');

        s_dev = MTLCreateSystemDefaultDevice();
        if (!s_dev) {
            fprintf(stderr, "[RSX metal] no Metal device available\n");
            return -1;
        }
        s_queue = [s_dev newCommandQueue];
        if (!s_queue) {
            fprintf(stderr, "[RSX metal] could not create command queue\n");
            return -1;
        }

        int rc;
#if TARGET_OS_IPHONE
        s_headless = 1;
        rc = create_offscreen();
#else
        rc = s_headless ? create_offscreen() : create_window(title);
#endif
        if (rc != 0) {
            fprintf(stderr, "[RSX metal] surface creation failed\n");
            return -1;
        }
        if (create_depth() != 0) {
            fprintf(stderr, "[RSX metal] depth/stencil buffer creation failed\n");
            return -1;
        }
        if (create_placeholders() != 0) {
            fprintf(stderr, "[RSX metal] placeholder texture creation failed\n");
            return -1;
        }

        s_verts = (MtlVertex*)malloc(sizeof(MtlVertex) * MTL_MAX_VERTS);
        if (!s_verts) {
            fprintf(stderr, "[RSX metal] vertex staging alloc failed\n");
            return -1;
        }

        NSError* serr = nil;
        s_shader_lib = [s_dev newLibraryWithSource:kBuiltinMSL options:nil error:&serr];
        if (!s_shader_lib) {
            fprintf(stderr, "[RSX metal] built-in shader compile failed: %s\n",
                    [[serr localizedDescription] UTF8String]);
            return -1;
        }

        /* PS3RECOMP_METAL_FIXED_FUNCTION=1 pins every draw to the built-in
         * shader: the first switch to flip when a title's draws vanish, to
         * tell a translation problem from a fetch or state one. */
        const char* ff = getenv("PS3RECOMP_METAL_FIXED_FUNCTION");
        s_guest_shaders = rsx_hlsl_to_msl_available() && !(ff && *ff && *ff != '0');

        s_inflight = dispatch_semaphore_create(MTL_MAX_INFLIGHT);
        if (!s_inflight) {
            fprintf(stderr, "[RSX metal] semaphore creation failed\n");
            return -1;
        }

        /* PS3RECOMP_RSX_ENGINE=dispatch drives the register-file draw engine
         * instead of the rsx_state vtable. The vtable stays registered only on
         * the path that uses it: the FIFO walker feeds both models, so leaving
         * both live would record every draw twice. */
        s_ready  = 1;
        s_closed = 0;
        rsx_draw_engine_set_backend(&s_engine_backend);
        /* The engine is this backend's default now that every host mode
         * passes both ways. PS3RECOMP_RSX_ENGINE=vtable is the way back, and
         * it stays that way until the vtable path has nothing left to say. */
        rsx_draw_engine_set_default(1);
        if (rsx_draw_engine_enabled() &&
            rsx_draw_engine_init(s_width, s_height) == 0) {
            s_eng_active = 1;
        } else {
            rsx_set_backend(&s_backend_vtable);
        }
        fprintf(stderr, "[RSX metal] %s on %s (%ux%u), guest shaders %s, %s path\n",
                s_headless ? "headless" : "windowed",
                [[s_dev name] UTF8String], s_width, s_height,
                s_guest_shaders ? "on" : (rsx_hlsl_to_msl_available() ? "off (env)" : "off (translator not built)"),
                s_eng_active ? "register-file draw engine" : "vtable");
        return 0;
    }
}

void rsx_metal_backend_shutdown(void)
{
    @autoreleasepool {
        if (s_ready) rsx_set_backend(NULL);
        if (s_eng_active) rsx_draw_engine_shutdown();
        rsx_draw_engine_set_backend(NULL);
        /* Reclaim every in-flight slot so no command buffer is still
         * referencing the device, queue or textures when they are released,
         * then hand the slots back. libdispatch traps if a semaphore is
         * deallocated while its count is below the value it was created with,
         * so draining without restoring is a crash, not a leak. */
        if (s_inflight) {
            for (int i = 0; i < MTL_MAX_INFLIGHT; i++)
                dispatch_semaphore_wait(s_inflight, DISPATCH_TIME_FOREVER);
            for (int i = 0; i < MTL_MAX_INFLIGHT; i++)
                dispatch_semaphore_signal(s_inflight);
            s_inflight = nil;
        }
#if !TARGET_OS_IPHONE
        if (s_window) { [s_window close]; s_window = nil; }
#endif
        free(s_verts); s_verts = NULL;
        free(s_cb); s_cb = NULL; s_cb_used = s_cb_cap = 0;
        free(s_tex_staging); s_tex_staging = NULL; s_tex_staging_cap = 0;
        s_vert_count = s_rec_count = 0;
        s_guest_draws = s_last_guest_draws = 0;
        clear_caches();
        release_surfaces();
        s_shader_lib = nil;
        s_null_tex   = nil;
        s_default_sampler = nil;
        s_ds_default = nil;
        s_layer     = nil;
        s_offscreen = nil;
        s_depth     = nil;
        s_queue     = nil;
        s_dev       = nil;
        s_ready     = 0;
    }
}

static int pump_messages_impl(void)
{
#if !TARGET_OS_IPHONE
    if (s_headless || !s_ready) return 0;
    @autoreleasepool {
        for (;;) {
            NSEvent* ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                             untilDate:[NSDate distantPast]
                                                inMode:NSDefaultRunLoopMode
                                               dequeue:YES];
            if (!ev) break;
            [NSApp sendEvent:ev];
        }
        if (s_window && ![s_window isVisible]) s_closed = 1;
    }
#endif
    return s_closed ? -1 : 0;
}

int rsx_metal_backend_pump_messages(void)
{
    if (s_headless || !s_ready) return s_closed ? -1 : 0;
#if !TARGET_OS_IPHONE
    if ([NSThread isMainThread])
        return pump_messages_impl();
    __block int result = 0;
    dispatch_sync(dispatch_get_main_queue(), ^{
        result = pump_messages_impl();
    });
    return result;
#else
    return pump_messages_impl();
#endif
}

void rsx_metal_backend_present(void)
{
    if (!s_ready) return;
    /* Under the draw engine the frame belongs to it: a host that drives the
     * flip itself (the harness, the boot smoke title) calls here, and a title
     * whose FIFO carries 0xE944 presents through the engine's own sink. */
    if (s_eng_active) { rsx_draw_engine_present(); return; }
    /* Block only when MTL_MAX_INFLIGHT frames are already queued. */
    if (!s_headless) dispatch_semaphore_wait(s_inflight, DISPATCH_TIME_FOREVER);
    @autoreleasepool {
        id<MTLTexture> target = nil;
        id<CAMetalDrawable> drawable = nil;

        if (s_headless) {
            target = s_offscreen;
        } else {
            drawable = [s_layer nextDrawable];
            if (!drawable) {            /* compositor is busy; skip this frame */
                dispatch_semaphore_signal(s_inflight);
                return;
            }
            target = [drawable texture];
        }
        if (!target) {
            if (!s_headless) dispatch_semaphore_signal(s_inflight);
            return;
        }

        /* The frame's first pass is on the display and clears colour to the
         * last value the guest asked for, which is what this backend has
         * always done and what keeps a frame with no clear in it from
         * presenting an undefined drawable. Depth and stencil start at the
         * nv40 reset values, unless a clear ahead of the first draw says
         * otherwise -- those clears are folded into this pass rather than each
         * opening one of their own. Only display clears fold: one aimed at a
         * surface belongs to a pass somewhere else. */
        MtlClear first;
        memset(&first, 0, sizeof first);
        first.flags   = MTL_CLEAR_COLOR | MTL_CLEAR_DEPTH | MTL_CLEAR_STENCIL;
        first.color   = s_clear_argb;
        first.depth   = 1.0f;
        first.stencil = 0;
        u32 r = 0;
        for (; r < s_rec_count && s_records[r].kind == MTL_REC_CLEAR
                                && s_records[r].u.clear.rt.off == 0; r++) {
            const MtlClear* c = &s_records[r].u.clear;
            if (c->flags & MTL_CLEAR_DEPTH)   first.depth   = c->depth;
            if (c->flags & MTL_CLEAR_STENCIL) first.stencil = c->stencil;
        }
        /* What a pass opened by a target change asks for: no clear flags at
         * all, so every attachment loads what the surface already holds. */
        static const MtlClear load_all = { .flags = 0 };

        /* Designated rather than memset: the buffers are ARC-managed. */
        MtlPassState ps = { .vb = nil, .cbuf = nil };
        if (s_vert_count > 0) {
            ps.vb = [s_dev newBufferWithBytes:s_verts
                                       length:sizeof(MtlVertex) * s_vert_count
                                      options:MTLResourceStorageModeShared];
            if (s_cb_used)
                ps.cbuf = [s_dev newBufferWithBytes:s_cb length:s_cb_used
                                            options:MTLResourceStorageModeShared];
        }

        id<MTLCommandBuffer> cb = [s_queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = begin_pass(cb, target, &first.rt, &first, &ps);
        for (; enc && r < s_rec_count; r++) {
            const MtlRecord* rec = &s_records[r];
            if (rec->kind == MTL_REC_CLEAR) {
                [enc endEncoding];
                enc = begin_pass(cb, target, &rec->u.clear.rt, &rec->u.clear, &ps);
                continue;
            }
            /* A draw aimed somewhere else ends the pass and opens the next,
             * for the same reason a clear does: an encoder is bound to one set
             * of attachments for its whole life. */
            if (!target_same(&rec->u.draw.rt, &ps.rt)) {
                [enc endEncoding];
                enc = begin_pass(cb, target, &rec->u.draw.rt, &load_all, &ps);
                if (!enc) break;
            }
            encode_draw(enc, &rec->u.draw, &ps);
        }
        if (enc) [enc endEncoding];

        if (drawable) [cb presentDrawable:drawable];

        if (s_headless) {
            /* The readback below must see a completed frame. */
            [cb commit];
            [cb waitUntilCompleted];
        } else {
            dispatch_semaphore_t sem = s_inflight;
            [cb addCompletedHandler:^(id<MTLCommandBuffer> _unused) {
                (void)_unused;
                dispatch_semaphore_signal(sem);
            }];
            [cb commit];        /* no wait: the CPU goes on to the next frame */
        }

        if (s_dropped_records) {
            fprintf(stderr, "[RSX metal] dropped %u record(s) this frame (cap %d records / %u verts)\n",
                    s_dropped_records, MTL_MAX_RECORDS, MTL_MAX_VERTS);
            s_dropped_records = 0;
        }
        s_rec_count  = 0;
        s_vert_count = 0;
        s_cb_used    = 0;
        s_last_guest_draws = s_guest_draws;
        s_guest_draws = 0;
        /* The frame's records are gone, so slots may move now. The command
         * buffer keeps its own references to whatever it still runs. */
        if (s_caches_full) {
            fprintf(stderr, "[RSX metal] cache full (%u VP, %u FP, %u PSO, %u textures,"
                            " %u surface views); cleared\n",
                    s_vp_count, s_fp_count, s_pso_count, s_tex_count, s_rtview_count);
            clear_caches();
        }

        if (s_headless && s_offscreen) {
            u32 px = 0;
            MTLRegion r = MTLRegionMake2D(s_width / 2, s_height / 2, 1, 1);
            [s_offscreen getBytes:&px bytesPerRow:4 fromRegion:r mipmapLevel:0];
            s_last_present_bgra = px;
        }
    }
}

u32 rsx_metal_backend_debug_color(void)     { return s_clear_argb; }
/* Under the draw engine the presented image is a surface of the engine's own,
 * so the sample comes from there rather than from the drawable it was blitted
 * onto: it is the same pixel one step earlier, and it needs no window. */
u32 rsx_metal_backend_readback_center(void)
{
    return s_eng_active ? rsx_draw_engine_readback_center() : s_last_present_bgra;
}
/* The draw engine has no fixed-function fallback: every draw it executes runs
 * the guest's own programs, so its own count is the answer under that path. */
u32 rsx_metal_backend_guest_draws(void)
{
    return s_eng_active ? rsx_draw_engine_guest_draws() : s_last_guest_draws;
}
