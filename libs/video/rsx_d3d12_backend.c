/*
 * ps3recomp - D3D12 RSX Backend
 *
 * Translates RSX GPU state to D3D12 rendering commands.
 *
 * Phase 1 implementation:
 *   - Win32 window + D3D12 device + swap chain
 *   - Clear render target to RSX clear color
 *   - Present with vsync
 *   - Basic vertex-colored triangle rendering
 *
 * This file is C with COM calls (D3D12 is a COM API).
 * We use the C interface (__uuidof not available in C, so we
 * define GUIDs manually).
 */

#ifdef _WIN32

#include "rsx_d3d12_backend.h"
#include "rsx_primitives.h"
#include "rsx_vertex_fetch.h"
#include "rsx_texture_layout.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* D3D12 headers */
#include <d3d12.h>
#include <d3d12sdklayers.h>   /* ID3D12Debug / ID3D12InfoQueue */
#include <dxgi1_4.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>

/* We need these GUIDs — define them here to avoid uuid.lib dependency */
#include <initguid.h>

/* Link libraries */
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

#define FRAME_COUNT         2   /* double buffering */
#define MAX_VERTICES    393216  /* per-frame vertex buffer (dbgfont submits
                                 * ~7.5k verts/frame; leave generous headroom) */
#define MAX_DRAWS         2048  /* per-frame draw records. Sized from the
                                 * measured worst case (~1841, FRAME_BUDGET);
                                 * the per-draw constant buffer is
                                 * VP_CB_STRIDE * MAX_DRAWS * 2 bytes, so 16384
                                 * asked for a 264 MB upload allocation. */
/* Per-draw VP constant-buffer slot: vp_c[512] + posscale + posoffset
 * (514 vec4 = 8224 B) rounded up to D3D12's 256-byte CBV alignment.
 * Constants are snapshotted at RECORD time -- wave's passes each set their
 * own texScale/offset uniforms, so one per-frame snapshot ran every pass
 * with the LAST pass's constants. */
#define VP_CB_STRIDE      8448
/* b1 per-draw FP slot: rsx_texscale[4] + rsx_alphatest + fp_k[64]
 * = 69 float4 = 1104 B, rounded to D3D12's 256-byte CBV alignment. */
#define VP_FPCB_STRIDE    1280
#define VERTEX_STRIDE       36  /* bytes per host vertex: pos3 + col4 + uv2 */

typedef struct {
    u32 vb_byte_offset; /* offset into vb_mapped where this draw's verts live */
    u32 vertex_count;
    u32 topology;       /* D3D_PRIMITIVE_TOPOLOGY_* */
    int textured;       /* 1 = sample the bound font/atlas texture (dbgfont) */
    int is_vp;          /* 1 = real VP path: vb_byte_offset indexes vp_vb (float4) */
    /* VP path per-draw shader/texture state, captured at draw_arrays time. */
    u32 fp_addr;        /* SET_SHADER_PROGRAM value (guest FP ucode location)   */
    int fp_exp32;       /* SET_SHADER_CONTROL 32-bit-exports bit at draw time   */
    u32 alpha_ctl;      /* alpha test: enable<<16 | (func&0xFF)<<8 | ref */
    u32 begin_epoch;    /* SET_BEGIN_END generation, for batch concatenation */
    u32 cull;           /* packed face culling: bit0 enable, bit1 cull FRONT
                         * (else BACK), bit2 front face is CCW. RSX culls back
                         * faces on most solid geometry; rendering everything
                         * double-sided lets a shell's interior faces show
                         * through and shade unlit (black). */
    u32 cmask;          /* D3D write mask from SET_COLOR_MASK at draw time
                         * (wave's sim passes write single lanes of the height
                         * maps; ignoring the mask stomped persistent state) */
    /* Per-unit textures (t0-t3): decompiled FPs sample up to 4 units
     * (demosaic's interpolation passes read 3). Captured at draw time. */
    struct {
        u32 off;        /* resolved vm offset (guest upload source), 0 = none */
        u32 raw;        /* raw RSX offset (offscreen-RT matching) */
        u32 w, h, fmt;  /* dims + RSX base format */
        u32 ctrl1;      /* NV4097 TEXTURE_CONTROL1: component remap crossbar */
        u32 mips;       /* SET_TEXTURE_FORMAT bits 16..31: mipmap level count.
                         * Cube faces sit one whole mip pyramid apart, so this
                         * is what sets the face stride. */
        int cube;       /* SET_TEXTURE_FORMAT bit 2: a cube texture. Sampled
                         * with a 3-component direction, not a 2D uv -- treating
                         * one as 2D is what makes an environment-mapped chrome
                         * surface come out with black patches and banding. */
        int set;
    } tex[4];
    int tex_rt[4];      /* pre-pass: OffRT index sampled by unit, -1 = none */
    int tex_slot;       /* legacy single-slot path (atlas); -1 = none */
    int vs_idx;         /* VPVSEntry slot for this draw's vertex program (-1 = primary) */
    int blend;          /* guest blend enable at draw time */
    u32 blend_key;      /* packed guest blend state (factors+equation), PSO key */
    /* Render-to-texture: which colour surface this op targets. 0 = a display
     * buffer (the swap-chain backbuffer); else the resolved vm offset of an
     * offscreen surface (demosaic chains its effect passes through local-
     * memory buffers and composites from them). */
    u32 rt_off;
    /* Which per-draw constant slot this record's constants were written to.
     * vp_record_cb writes them keyed by the RECORD index, so any pass that
     * reorders or compacts s_d3d.draws (DRAW_KEEP_TEX, DRAW_LAST_TEX,
     * DRAW_LIMIT) would otherwise make every surviving draw read some other
     * object's MVP -- silently transforming it somewhere else entirely. */
    u32 cb_slot;
    u32 rt_mrt[3];      /* colour targets B,C,D (MRT1/2/3), 0 = none. Deferred
                         * shading G-buffers write 3-4 targets in one pass. */
    u32 rt_w, rt_h;     /* surface clip dims at record time (offscreen RT size) */
    u32 rt_fmt;         /* RSX surface colour format (SET_SURFACE_FORMAT [4:0]) */
    /* Guest viewport rect at draw time (target pixels). Sub-viewport layouts
     * (wave's debug tiles) position quads with this, not with constants --
     * forcing full-target viewports drew every tile window-sized. */
    u32 vp_x, vp_y, vp_w, vp_h;
    /* Ordered clear op (offscreen surfaces only; display clears stay the
     * frame-start backbuffer clear). is_clear records also set is_vp so the
     * legacy replay pass skips them. */
    int   is_clear;
    float cc[4];
} D3D12DrawRecord;

/* Offscreen render target (render-to-texture). Persistent across frames --
 * pass N's output is sampled by pass N+1 and possibly by later frames. */
typedef struct {
    ID3D12Resource*       res;
    ID3D12Resource*       up;          /* init-upload staging (kept alive) */
    u32                   off, w, h;   /* raw RSX offset + dims */
    u32                   dxgi;        /* DXGI_FORMAT of the resource */
    D3D12_RESOURCE_STATES st;          /* tracked resource state */
    int                   used;        /* referenced this frame */
} OffRT;
#define MAX_OFF_RTS  16  /* demosaic double-buffers its 6-surface pass chain */
#define RT_SRV_BASE  5   /* SRV heap slots 5..20 hold offscreen-RT SRVs */
/* Per-draw SRV windows: each VP draw gets 4 consecutive descriptors (t0-t3)
 * so multi-unit fragment programs see all their textures (demosaic's
 * interpolation passes sample 3 units). */
#define DRAW_SRV_BASE 32
#define SRV_HEAP_SIZE (DRAW_SRV_BASE + MAX_DRAWS * 4)

/* Per-frame VP texture slot: a guest texture uploaded for this frame's VP
 * draws (re-uploaded every frame -- gcm/cube's plasma animates in guest
 * memory). SRV lives at heap index 1+slot. */
typedef struct {
    ID3D12Resource* res;
    ID3D12Resource* up;
    u32 off, w, h, fmt; /* current contents (resource reused when dims match) */
    u32 csum;           /* sparse checksum of the source bytes last uploaded */
    int cube;           /* resource is a 6-face cube, sampled as TextureCube */
    u32 key;            /* the ORIGINAL bound offset -- the cache lookup key.
                         * off is the RESOLVED source after TEX_OFF_BIAS/TEX_REMAP
                         * shift it, so keying on off never matches the raw offset
                         * a later draw arrives with, and every draw re-uploads. */
    int used;           /* referenced this frame */
} VPTexSlot;
/* Distinct textures uploadable per frame. Four was enough for a UI/atlas-style
 * frame but not for a scene: this title binds ~20 distinct textures per frame,
 * so every draw past the fourth got slot -1 from vp_upload_tex_slot ("out of
 * slots") and drew UNTEXTURED. That reads as a correctly-lit but solid black
 * object, which is indistinguishable from a missing texture upload. */
#define VP_TEX_SLOTS 96

/* Decompiled-VS cache: one entry per distinct vertex-program ucode seen at
 * draw time (hashed). Apps switch VPs between draws (gcm/cube: its MVP cube VP
 * vs dbgfont-gcm's text VP); compiling only the first left later draws
 * transformed by the wrong program (text offscreen). */
typedef struct {
    u32 hash;               /* FNV-1a of the ucode */
    ID3DBlob* vs;
    int uses_c03;
} VPVSEntry;
#define VP_VS_CACHE 16  /* wave uses 5+ distinct VPs; at 4 the cache
                         * thrashed every frame and eviction shifted slots
                         * under recorded vs_idx values -- the display mesh
                         * drew with the WRONG vertex program (sub-rect +
                         * edge slivers instead of fullscreen) */

/* Compiled guest-FP pipeline cache: decompiled VS (by cache slot) + decompiled
 * PS (fragment ucode at fp_addr). */
typedef struct {
    u32 fp_addr;
    int vs_idx;             /* VPVSEntry slot this PSO's VS came from */
    u32 vs_hash;            /* validates the slot hasn't been evicted */
    u32 gen;
    u32 blend;              /* packed guest blend key (enable+factors+eq, PSO key) */
    int nrt;                /* bound colour target count (PSO key)       */
    u32 rtfmt;              /* DXGI format of the colour targets (PSO key) */
    int exp32;              /* 32-bit-exports control bit (PSO key)       */
    u32 ucode_hash;         /* FNV-1a of the FP ucode: apps re-patch inline
                             * constants per frame (wave's stamp position/
                             * amplitude) -- address-only keying served the
                             * stale compile forever. */
    u32 cmask;              /* colour write mask (PSO key) */
    u32 cull;               /* packed face culling (PSO key) */
    u32 cube_mask;          /* which units are cube textures (PSO key): the HLSL
                             * declares those samplers as TextureCube, so a cube
                             * and a 2D variant of the same program are different
                             * pipelines and must not share a cache entry. */
    ID3D12PipelineState* pso;
} VPFPEntry;
/* Guest-FP PSO cache. 16 entries thrashed badly: this title needs far more
 * distinct (fp, vs, blend, rt, cmask, ucode-hash) combinations than that in a
 * single frame, so the FIFO evicted entries that were needed again immediately
 * and ~17 shaders were recompiled EVERY frame -- about half the frame's CPU
 * time. Entries are small (a PSO pointer plus key fields). */
#define VP_FP_CACHE 256

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

typedef struct {
    /* Window */
    HWND hwnd;
    u32  width;
    u32  height;
    int  window_closed;

    /* D3D12 core */
    ID3D12Device*              device;
    ID3D12CommandQueue*        cmd_queue;
    IDXGISwapChain3*           swap_chain;
    ID3D12DescriptorHeap*      rtv_heap;
    u32                        rtv_descriptor_size;
    ID3D12Resource*            render_targets[FRAME_COUNT];
    ID3D12CommandAllocator*    cmd_allocators[FRAME_COUNT];
    ID3D12GraphicsCommandList* cmd_list;

    /* Synchronization */
    ID3D12Fence* fence;
    HANDLE       fence_event;
    u64          fence_values[FRAME_COUNT];
    u32          frame_index;

    /* Pipeline */
    ID3D12RootSignature*  root_signature;
    ID3D12PipelineState*  pipeline_state;         /* triangle class — default */
    ID3D12PipelineState*  pipeline_state_lines;   /* line class */
    ID3D12PipelineState*  pipeline_state_points;  /* point class */

    /* Depth/stencil */
    ID3D12DescriptorHeap* dsv_heap;
    ID3D12Resource*       depth_buffer;

    /* Dynamic vertex buffer (upload heap) */
    ID3D12Resource*       vertex_buffer;
    D3D12_VERTEX_BUFFER_VIEW vb_view;
    void*                 vb_mapped;      /* persistently mapped */
    u32                   vb_offset;      /* current write position */

    int                   pipeline_ready; /* 1 if root sig + PSO created */

    /* Debug: copy the presented backbuffer to disk as BMP for the first N
     * frames (enabled by env CELLMARK_DUMP). Lets us verify what actually
     * rendered without racing the window/process lifetime. */
    ID3D12Resource*       readback_buf;
    u32                   readback_pitch;
    int                   dump_frames_left;
    int                   dump_skip_left;   /* CELLMARK_DUMP_SKIP: presents to ignore first */

    /* Textured pipeline (dbgfont / 2D atlas quads). The font atlas is an 8-bit
     * coverage texture uploaded as R8_UNORM and sampled in the pixel shader. */
    ID3D12PipelineState*  pipeline_state_tex;   /* triangle + texture + blend */
    ID3D12DescriptorHeap* srv_heap;             /* shader-visible, 1 SRV slot  */
    ID3D12Resource*       tex_resource;         /* current atlas texture       */
    ID3D12Resource*       tex_upload;           /* staging buffer for uploads  */
    u32                   tex_w, tex_h;         /* dims of tex_resource         */
    int                   tex_ready;            /* 1 once an atlas is uploaded  */
    u32                   tex_src_offset;       /* guest RSX offset of atlas    */
    int                   tex_bound;            /* a texture is bound for draws */
    int                   tex_dirty;            /* re-upload needed this frame  */

    /* Real RSX vertex-program path: the captured VP is decompiled to HLSL and
     * used as the vertex shader, fed the raw float4 attrib0 + the vp_c[]
     * constant bank. This produces exact position + texcoord (vs. the frac
     * approximation). Used for the 2D/dbgfont quad draws. */
    ID3D12RootSignature*  vp_root_sig;          /* CBV(b0) + SRV table + sampler */
    ID3D12PipelineState*  pipeline_state_vp;    /* decompiled VS + atlas PS      */
    ID3D12PipelineState*  pipeline_state_vp_color; /* decompiled VS + colour PS (untextured 3D) */
    ID3D12Resource*       vp_vb;                /* raw float4 attrib0, per-frame */
    void*                 vp_vb_mapped;
    u32                   vp_vb_offset;
    ID3D12Resource*       vp_cb;                /* per-draw VP constant slots    */
    void*                 vp_cb_mapped;
    ID3D12Resource*       vp_fpcb;              /* per-draw FP texscale (b1)     */
    void*                 vp_fpcb_mapped;
    int                   vp_ready;             /* VS+PSO compiled ok            */
    u32                   vp_compiled_bytes;    /* ucode size when last compiled */
    ID3DBlob*             vp_vs_blob;           /* kept for guest-FP PSO builds  */
    u32                   vp_gen;               /* bumped per VP recompile       */
    int                   vp_uses_c03;          /* VS references vp_c[0..3]      */
    VPTexSlot             vp_tex[VP_TEX_SLOTS]; /* per-frame VP texture slots    */
    VPVSEntry             vp_vs[VP_VS_CACHE];   /* per-draw decompiled VS cache  */
    int                   vp_vs_n;
    VPFPEntry             vp_fp[VP_FP_CACHE];   /* guest-FP PSO cache            */
    int                   vp_fp_n;
    u32                   srv_inc;              /* CBV_SRV_UAV descriptor size   */
    /* VP path: latest texture bound per unit (t0-t3). */
    struct { u32 off, raw, w, h, fmt, ctrl1, mips; int cube; int set; } cur_texs[4];

    /* Render-to-texture: offscreen RT pool + their RTV heap. */
    OffRT                 off_rt[MAX_OFF_RTS];
    /* DRAW_ARRAYS batch merging (see d3d12_draw_arrays): the vertex index one
     * past the last batch, and whether the current BEGIN/END may still be
     * extended. */
    u32                   merge_first_end;
    int                   merge_prev_draw;
    ID3D12DescriptorHeap* rt_rtv_heap;          /* MAX_OFF_RTS RTVs (CPU only)   */

    /* Frame-parity double buffering for the per-draw upload streams (vp_vb
     * vertices, vp_cb constants, vp_fpcb texscales): records for frame N+1
     * are written while frame N's GPU work may still be reading -- writes go
     * to the other half. Toggled at the end of render_frame. */
    int                   vp_parity;

    /* Per-frame draw recording */
    int                   frame_recording; /* 1 if cmd list is open for recording */
    u32                   draw_count;      /* draws this frame */
    D3D12DrawRecord       draws[MAX_DRAWS];

    /* Pointer to current RSX state (set before draw calls) */
    const rsx_state*      current_rsx_state;

    /* Current frame state */
    float clear_color[4];  /* RGBA float */

    /* Stats */
    u64 frame_count;
    u64 last_fps_time;
    u32 fps;

    int initialized;
} D3D12State;

static D3D12State s_d3d;
char g_rsx_title_base[128] = "ps3recomp";
static u32 s_dbg_last_draws = 0;
static u64 s_req_verts = 0, s_req_draws = 0, s_drop_draws = 0;
/* Raw bind offset of the duck's texture, identified by CONTENT (duck.tga is
 * overwhelmingly yellow, avg 243,191,23). VRAM offsets move between runs, so a
 * hard-coded offset in a filter silently matches nothing and the run looks like
 * "renders nothing" -- which cost real time. DRAW_KEEP_TEX=duck resolves here. */
/* Copy of the last completed frame, bound wherever a draw samples a
 * DISPLAY-SIZED texture. On RSX this title renders its reflection into a corner
 * of the render surface and then samples that surface as a texture; our backend
 * renders into a D3D backbuffer, so the guest memory behind that sampler is
 * never written and it reads empty -- which is why the water had no reflection.
 * Feeding it the previous frame costs one frame of latency, which for a water
 * reflection is not visible. */
static ID3D12Resource* s_screen_copy = NULL;
/* Sub-viewport render-to-texture. This title renders its reflection/refraction
 * pre-pass into a REGION of the same surface (vp 0,208 512x512 and 0,208
 * 1024x512, cmask=F) and then samples a texture of exactly that size. On
 * hardware the pass lands in the buffer that texture points at; our backend
 * renders into D3D resources, so that guest buffer is never written and every
 * fluid sampler reads zero. Capture each region into a matching resource and
 * bind it for samplers whose guest source is empty and whose size matches.
 *
 * SUBVP_RTT=1 to enable. OFF by default because it cannot be validated on this
 * title yet: the fluid draws that would consume these captures never rasterize
 * (see below), so binding them changes nothing observable. The capture and
 * binding themselves are confirmed working -- SUBVP_DBG shows the 512x512 and
 * 256x256 regions captured and handed to fp 0x5DB81/0x5EA81/0x5EF81/0x5E381. */
#define SUBVP_SLOTS 4
static struct { ID3D12Resource* res; u32 x, y, w, h; } s_subvp[SUBVP_SLOTS];
static int s_subvp_n = 0;

static void subvp_note(u32 x, u32 y, u32 w, u32 h)
{
    if (!w || !h || w > s_d3d.width || h > s_d3d.height) return;
    if (w == s_d3d.width && h == s_d3d.height) return;      /* full-screen pass */
    for (int i = 0; i < s_subvp_n; i++)
        if (s_subvp[i].w == w && s_subvp[i].h == h) { s_subvp[i].x = x; s_subvp[i].y = y; return; }
    if (s_subvp_n >= SUBVP_SLOTS) return;
    s_subvp[s_subvp_n].x = x; s_subvp[s_subvp_n].y = y;
    s_subvp[s_subvp_n].w = w; s_subvp[s_subvp_n].h = h;
    s_subvp[s_subvp_n].res = NULL;
    s_subvp_n++;
}

/* Copy each noted region out of the backbuffer. Called at the same moment as
 * screen_copy_capture -- after the reduced-viewport passes and before the
 * full-screen scene overwrites them. */
static void subvp_capture(u32 fi)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("SUBVP_RTT"); en = e ? atoi(e) : 0; }
    if (!en || !s_d3d.device || !s_subvp_n) return;
    for (int i = 0; i < s_subvp_n; i++) {
        if (!s_subvp[i].res) {
            D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC td = {0};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = s_subvp[i].w; td.Height = s_subvp[i].h;
            td.DepthOrArraySize = 1; td.MipLevels = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
            td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
                    s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL,
                    &IID_ID3D12Resource, (void**)&s_subvp[i].res)))
                { s_subvp[i].res = NULL; continue; }
        }
        D3D12_RESOURCE_BARRIER bb[2] = {0};
        bb[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bb[0].Transition.pResource   = s_d3d.render_targets[fi];
        bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        bb[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bb[1] = bb[0];
        bb[1].Transition.pResource   = s_subvp[i].res;
        bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        bb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, bb);

        D3D12_TEXTURE_COPY_LOCATION dstl = {0}, srcl = {0};
        dstl.pResource = s_subvp[i].res;
        dstl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dstl.SubresourceIndex = 0;
        srcl.pResource = s_d3d.render_targets[fi];
        srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; srcl.SubresourceIndex = 0;
        D3D12_BOX box; box.left = s_subvp[i].x; box.top = s_subvp[i].y;
        box.front = 0; box.right = s_subvp[i].x + s_subvp[i].w;
        box.bottom = s_subvp[i].y + s_subvp[i].h; box.back = 1;
        if (box.right > s_d3d.width)  box.right  = s_d3d.width;
        if (box.bottom > s_d3d.height) box.bottom = s_d3d.height;
        s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dstl, 0, 0, 0, &srcl, &box);

        bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        bb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        bb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, bb);
        { static int _n = 0; if (getenv("SUBVP_DBG") && _n++ < 8)
            fprintf(stderr, "[SUBVP] captured %ux%u from (%u,%u)%c",
                    s_subvp[i].w, s_subvp[i].h, s_subvp[i].x, s_subvp[i].y, 10); }
    }
}
static void screen_copy_capture(u32 fi);   /* fwd */
static int  s_sc_dump_pending = 0;
static u32 s_duck_raw = 0;   /* as BOUND (what draw records carry) */
static u32 s_duck_off = 0;   /* as RESOLVED (what the uploader sees)   */
/* PERF=1: where a frame's CPU time actually goes. Guessing at this is how you
 * spend an afternoon optimising the wrong loop. */
static double s_perf_tex = 0.0, s_perf_frame = 0.0, s_perf_vtx = 0.0, s_perf_rf = 0.0;
static double s_perf_gpu = 0.0, s_perf_pre = 0.0, s_perf_srv = 0.0, s_perf_pso = 0.0;
static int s_perf_pso_calls = 0, s_perf_pso_miss = 0, s_perf_pso_hashbytes = 0;
static u64    s_perf_nverts = 0;
static u64    s_perf_texbytes = 0;
static int    s_perf_ntex = 0;
static double perf_now(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
static int perf_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("PERF"); v = e ? atoi(e) : 0; }
    return v;
}
/* DBG_LOCK: running clip-space centroid of the tracked mesh (DUCK_VTX's
 * texture), so the debug camera can follow it. The ducks are driven by the
 * physics sim and drift every frame, so a fixed DBG_CENTER loses them as soon
 * as the magnification is high enough to make one recognisable. */
static float s_lock_x = 0.0f, s_lock_y = 0.0f;
static int   s_lock_valid = 0;
/* Accumulated over one frame, published at the frame boundary. Averaging as the
 * frame is recorded makes the aim wander while it is being used, which at high
 * magnification walks the target off-screen entirely. */
static double s_lock_sx = 0.0, s_lock_sy = 0.0;
static u32    s_lock_n  = 0;

/* ---------------------------------------------------------------------------
 * Win32 window
 * -----------------------------------------------------------------------*/

static LRESULT CALLBACK d3d12_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLOSE:
        s_d3d.window_closed = 1;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            s_d3d.window_closed = 1;
            DestroyWindow(hwnd);
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static HWND create_window(u32 width, u32 height, const char* title)
{
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = d3d12_wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "ps3recomp_d3d12";
    RegisterClassExA(&wc);

    RECT wr = {0, 0, (LONG)width, (LONG)height};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    return CreateWindowExA(
        0, "ps3recomp_d3d12",
        title ? title : "ps3recomp (D3D12)",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        NULL, NULL, GetModuleHandle(NULL), NULL);
}

/* ---------------------------------------------------------------------------
 * D3D12 initialization
 * -----------------------------------------------------------------------*/

static int init_d3d12(u32 width, u32 height)
{
    HRESULT hr;

    /* Enable debug layer in debug builds */
    /* Debug layer: on in debug builds, or in any build when D3D12_DBG is set
     * (so a Release run can capture exact PSO/validation errors). */
    if (
#ifdef NDEBUG
        getenv("D3D12_DBG")
#else
        1
#endif
    ) {
        ID3D12Debug* debug_controller = NULL;
        hr = D3D12GetDebugInterface(&IID_ID3D12Debug, (void**)&debug_controller);
        if (SUCCEEDED(hr) && debug_controller) {
            debug_controller->lpVtbl->EnableDebugLayer(debug_controller);
            debug_controller->lpVtbl->Release(debug_controller);
            printf("[D3D12] Debug layer enabled\n");
        } else {
            printf("[D3D12] Debug layer requested but unavailable (0x%08lX) -- "
                   "install 'Graphics Tools' optional feature\n", hr);
        }
    }

    /* Create DXGI factory */
    IDXGIFactory4* factory = NULL;
    hr = CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&factory);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: CreateDXGIFactory1 failed (0x%08lX)\n", hr);
        return -1;
    }

    /* Create D3D12 device. On a dual-GPU laptop a NULL adapter usually lands on
     * the integrated GPU; explicitly pick the high-performance (discrete)
     * adapter via IDXGIFactory6. Set CELLMARK_IGPU to force the low-power one
     * (for A/B testing a suspected iGPU-driver stall). */
    {
        IDXGIFactory6* factory6 = NULL;
        if (SUCCEEDED(factory->lpVtbl->QueryInterface(
                factory, &IID_IDXGIFactory6, (void**)&factory6))) {
            DXGI_GPU_PREFERENCE pref = getenv("CELLMARK_IGPU")
                ? DXGI_GPU_PREFERENCE_MINIMUM_POWER
                : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
            IDXGIAdapter1* adapter = NULL;
            for (UINT ai = 0; factory6->lpVtbl->EnumAdapterByGpuPreference(
                     factory6, ai, pref, &IID_IDXGIAdapter1, (void**)&adapter)
                     != DXGI_ERROR_NOT_FOUND; ai++) {
                DXGI_ADAPTER_DESC1 ad;
                adapter->lpVtbl->GetDesc1(adapter, &ad);
                if (!(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                    SUCCEEDED(D3D12CreateDevice((IUnknown*)adapter,
                        D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device,
                        (void**)&s_d3d.device))) {
                    printf("[D3D12] adapter: %ls (VRAM %llu MB)\n", ad.Description,
                           (unsigned long long)(ad.DedicatedVideoMemory >> 20));
                    adapter->lpVtbl->Release(adapter);
                    break;
                }
                adapter->lpVtbl->Release(adapter);
                adapter = NULL;
            }
            factory6->lpVtbl->Release(factory6);
        }
    }
    if (!s_d3d.device) {
        hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0,
                               &IID_ID3D12Device, (void**)&s_d3d.device);
        if (FAILED(hr)) {
            printf("[D3D12] ERROR: D3D12CreateDevice failed (0x%08lX)\n", hr);
            factory->lpVtbl->Release(factory);
            return -1;
        }
        printf("[D3D12] Device created on default adapter (feature level 11.0)\n");
    }

    /* Create command queue */
    D3D12_COMMAND_QUEUE_DESC queue_desc = {0};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = s_d3d.device->lpVtbl->CreateCommandQueue(
        s_d3d.device, &queue_desc, &IID_ID3D12CommandQueue, (void**)&s_d3d.cmd_queue);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: CreateCommandQueue failed\n");
        factory->lpVtbl->Release(factory);
        return -1;
    }

    /* Create swap chain */
    DXGI_SWAP_CHAIN_DESC1 sc_desc = {0};
    sc_desc.Width = width;
    sc_desc.Height = height;
    sc_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = FRAME_COUNT;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain1* swap_chain1 = NULL;
    hr = factory->lpVtbl->CreateSwapChainForHwnd(
        factory, (IUnknown*)s_d3d.cmd_queue,
        s_d3d.hwnd, &sc_desc, NULL, NULL, &swap_chain1);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: CreateSwapChainForHwnd failed (0x%08lX)\n", hr);
        factory->lpVtbl->Release(factory);
        return -1;
    }

    /* Disable Alt+Enter fullscreen toggle */
    factory->lpVtbl->MakeWindowAssociation(factory, s_d3d.hwnd, DXGI_MWA_NO_ALT_ENTER);
    factory->lpVtbl->Release(factory);

    /* Query SwapChain3 interface */
    hr = swap_chain1->lpVtbl->QueryInterface(
        swap_chain1, &IID_IDXGISwapChain3, (void**)&s_d3d.swap_chain);
    swap_chain1->lpVtbl->Release(swap_chain1);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: QueryInterface for SwapChain3 failed\n");
        return -1;
    }

    s_d3d.frame_index = s_d3d.swap_chain->lpVtbl->GetCurrentBackBufferIndex(s_d3d.swap_chain);

    /* Create RTV descriptor heap */
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {0};
    rtv_heap_desc.NumDescriptors = FRAME_COUNT;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
        s_d3d.device, &rtv_heap_desc, &IID_ID3D12DescriptorHeap, (void**)&s_d3d.rtv_heap);
    if (FAILED(hr)) return -1;

    s_d3d.rtv_descriptor_size = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    /* Create RTVs for each frame */
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
    s_d3d.rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rtv_heap, &rtv_handle);

    for (u32 i = 0; i < FRAME_COUNT; i++) {
        hr = s_d3d.swap_chain->lpVtbl->GetBuffer(
            s_d3d.swap_chain, i, &IID_ID3D12Resource, (void**)&s_d3d.render_targets[i]);
        if (FAILED(hr)) return -1;

        s_d3d.device->lpVtbl->CreateRenderTargetView(
            s_d3d.device, s_d3d.render_targets[i], NULL, rtv_handle);
        rtv_handle.ptr += s_d3d.rtv_descriptor_size;
    }

    /* ---------------------------------------------------------------
     * Depth/stencil buffer
     * 24-bit depth + 8-bit stencil (DXGI_FORMAT_D24_UNORM_S8_UINT).
     * One shared depth texture across both frames — RSX games on PS3
     * typically use a single zeta surface.
     * ---------------------------------------------------------------*/
    {
        D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc = {0};
        dsv_heap_desc.NumDescriptors = 1;
        dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
            s_d3d.device, &dsv_heap_desc, &IID_ID3D12DescriptorHeap,
            (void**)&s_d3d.dsv_heap);
        if (FAILED(hr)) {
            printf("[D3D12] DSV heap creation failed\n");
            return -1;
        }

        D3D12_HEAP_PROPERTIES heap_props = {0};
        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC depth_desc = {0};
        depth_desc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        /* Sized to cover the LARGEST render target, not just the window:
         * D3D12 clips rasterization to the smallest bound attachment, and
         * this DSV is shared by every pass -- with a window-sized depth
         * buffer, draws into larger offscreen RTs (wave's 1920x1080 input
         * image) were silently cropped to the window rect. */
        depth_desc.Width              = 2048;
        depth_desc.Height             = 2048;
        depth_desc.DepthOrArraySize   = 1;
        depth_desc.MipLevels          = 1;
        depth_desc.Format             = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_desc.SampleDesc.Count   = 1;
        depth_desc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        depth_desc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE depth_clear = {0};
        depth_clear.Format                       = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_clear.DepthStencil.Depth           = 1.0f;
        depth_clear.DepthStencil.Stencil         = 0;

        hr = s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &heap_props, D3D12_HEAP_FLAG_NONE,
            &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear,
            &IID_ID3D12Resource, (void**)&s_d3d.depth_buffer);
        if (FAILED(hr)) {
            printf("[D3D12] Depth buffer creation failed (0x%08lX)\n", hr);
            return -1;
        }

        D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = {0};
        dsv_desc.Format         = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dsv_desc.ViewDimension  = D3D12_DSV_DIMENSION_TEXTURE2D;

        D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
        s_d3d.dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.dsv_heap, &dsv_handle);
        s_d3d.device->lpVtbl->CreateDepthStencilView(
            s_d3d.device, s_d3d.depth_buffer, &dsv_desc, dsv_handle);

        printf("[D3D12] Depth buffer created (%ux%u D24S8)\n", width, height);
    }

    /* Create command allocators and command list */
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        hr = s_d3d.device->lpVtbl->CreateCommandAllocator(
            s_d3d.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void**)&s_d3d.cmd_allocators[i]);
        if (FAILED(hr)) return -1;
    }

    hr = s_d3d.device->lpVtbl->CreateCommandList(
        s_d3d.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        s_d3d.cmd_allocators[0], NULL,
        &IID_ID3D12GraphicsCommandList, (void**)&s_d3d.cmd_list);
    if (FAILED(hr)) return -1;

    /* Close the command list (it starts in recording state) */
    s_d3d.cmd_list->lpVtbl->Close(s_d3d.cmd_list);

    /* Create fence */
    hr = s_d3d.device->lpVtbl->CreateFence(
        s_d3d.device, 0, D3D12_FENCE_FLAG_NONE,
        &IID_ID3D12Fence, (void**)&s_d3d.fence);
    if (FAILED(hr)) return -1;

    s_d3d.fence_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    memset(s_d3d.fence_values, 0, sizeof(s_d3d.fence_values));

    /* ---------------------------------------------------------------
     * Create root signature with 16 root constants (one mat4 MVP at b0).
     * Visible only to the vertex shader — pixel shader doesn't need it.
     * ---------------------------------------------------------------*/
    {
        /* param 0: mat4 MVP as 16 root constants at b0 (vertex shader).
         * param 1: one SRV (t0) descriptor table for the atlas texture (pixel).
         * static sampler s0: linear clamp. The plain (untextured) PSO simply
         * never references t0/s0, so it ignores them. */
        D3D12_DESCRIPTOR_RANGE srv_range = {0};
        srv_range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors     = 1;
        srv_range.BaseShaderRegister = 0;   /* t0 */
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER root_params[2] = {0};
        root_params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        root_params[0].Constants.ShaderRegister = 0;   /* b0 */
        root_params[0].Constants.RegisterSpace  = 0;
        root_params[0].Constants.Num32BitValues = 16;  /* mat4 */
        root_params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_VERTEX;
        root_params[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        root_params[1].DescriptorTable.NumDescriptorRanges = 1;
        root_params[1].DescriptorTable.pDescriptorRanges   = &srv_range;
        root_params[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp = {0};
        samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ShaderRegister   = 0;   /* s0 */
        samp.MaxLOD           = D3D12_FLOAT32_MAX;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rs_desc = {0};
        rs_desc.NumParameters     = 2;
        rs_desc.pParameters       = root_params;
        rs_desc.NumStaticSamplers = 1;
        rs_desc.pStaticSamplers   = &samp;
        rs_desc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                              | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                              | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                              | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        ID3DBlob* signature_blob = NULL;
        ID3DBlob* error_blob = NULL;
        hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                          &signature_blob, &error_blob);
        if (FAILED(hr)) {
            printf("[D3D12] Root signature serialization failed: %s\n",
                   error_blob ? (const char*)error_blob->lpVtbl->GetBufferPointer(error_blob) : "?");
            if (error_blob) error_blob->lpVtbl->Release(error_blob);
            return -1;
        }

        hr = s_d3d.device->lpVtbl->CreateRootSignature(
            s_d3d.device, 0,
            signature_blob->lpVtbl->GetBufferPointer(signature_blob),
            signature_blob->lpVtbl->GetBufferSize(signature_blob),
            &IID_ID3D12RootSignature, (void**)&s_d3d.root_signature);
        signature_blob->lpVtbl->Release(signature_blob);
        if (FAILED(hr)) {
            printf("[D3D12] Root signature creation failed\n");
            return -1;
        }
    }

    /* ---------------------------------------------------------------
     * Compile shaders and create PSO
     * ---------------------------------------------------------------*/
    {
        /* Basic vertex-colored shader.
         * The MVP matrix arrives via root constants as 4 vec4 columns
         * (PS3/OpenGL column-major convention). We multiply explicitly so
         * we don't depend on HLSL's matrix packing — matches PS3 semantics
         * `gl_Position = MVP * vec4(pos, 1.0)`. */
        static const char vs_hlsl[] =
            "cbuffer cb0 : register(b0) {\n"
            "    float4 mvp_col0;\n"
            "    float4 mvp_col1;\n"
            "    float4 mvp_col2;\n"
            "    float4 mvp_col3;\n"
            "};\n"
            "struct VSInput  { float3 pos : POSITION; float4 col : COLOR; };\n"
            "struct VSOutput { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
            "VSOutput main(VSInput i) {\n"
            "    VSOutput o;\n"
            "    float4 p = float4(i.pos, 1.0);\n"
            "    o.pos = mvp_col0 * p.x + mvp_col1 * p.y + mvp_col2 * p.z + mvp_col3 * p.w;\n"
            "    o.col = i.col;\n"
            "    return o;\n"
            "}\n";
        static const char ps_hlsl[] =
            "struct PSInput { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
            "float4 main(PSInput i) : SV_TARGET { return i.col; }\n";

        ID3DBlob* vs_blob = NULL;
        ID3DBlob* ps_blob = NULL;
        ID3DBlob* err = NULL;

        hr = D3DCompile(vs_hlsl, sizeof(vs_hlsl) - 1, "vs_basic", NULL, NULL,
                        "main", "vs_5_0", 0, 0, &vs_blob, &err);
        if (FAILED(hr)) {
            printf("[D3D12] VS compile failed: %s\n",
                   err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "unknown");
            if (err) err->lpVtbl->Release(err);
        }

        hr = D3DCompile(ps_hlsl, sizeof(ps_hlsl) - 1, "ps_basic", NULL, NULL,
                        "main", "ps_5_0", 0, 0, &ps_blob, &err);
        if (FAILED(hr)) {
            printf("[D3D12] PS compile failed: %s\n",
                   err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "unknown");
            if (err) err->lpVtbl->Release(err);
        }

        if (vs_blob && ps_blob) {
            D3D12_INPUT_ELEMENT_DESC input_layout[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            };

            D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {0};
            pso_desc.pRootSignature = s_d3d.root_signature;
            pso_desc.VS.pShaderBytecode = vs_blob->lpVtbl->GetBufferPointer(vs_blob);
            pso_desc.VS.BytecodeLength = vs_blob->lpVtbl->GetBufferSize(vs_blob);
            pso_desc.PS.pShaderBytecode = ps_blob->lpVtbl->GetBufferPointer(ps_blob);
            pso_desc.PS.BytecodeLength = ps_blob->lpVtbl->GetBufferSize(ps_blob);
            pso_desc.InputLayout.pInputElementDescs = input_layout;
            pso_desc.InputLayout.NumElements = 3;
            pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
                D3D12_COLOR_WRITE_ENABLE_ALL;
            pso_desc.SampleMask = UINT_MAX;
            pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pso_desc.NumRenderTargets = 1;
            pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            pso_desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
            /* Depth test enabled, write enabled, LESS func.
             * Games with depth_test_enable=0 in RSX state can still render —
             * LESS just means new-z must be less than existing — but future
             * work should mirror RSX depth state into a PSO cache. */
            pso_desc.DepthStencilState.DepthEnable    = TRUE;
            pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            pso_desc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            pso_desc.DepthStencilState.StencilEnable  = FALSE;
            pso_desc.SampleDesc.Count = 1;

            hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pso_desc,
                &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state);
            if (SUCCEEDED(hr)) {
                s_d3d.pipeline_ready = 1;
                printf("[D3D12] Pipeline state created (triangle class)\n");
            } else {
                printf("[D3D12] PSO TRIANGLE creation failed (0x%08lX)\n", hr);
            }

            /* Line-class PSO — same shader, LINE topology type. */
            pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pso_desc,
                &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_lines);
            if (SUCCEEDED(hr)) printf("[D3D12] Pipeline state created (line class)\n");
            else printf("[D3D12] PSO LINE creation failed (0x%08lX)\n", hr);

            /* Point-class PSO. */
            pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
            hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pso_desc,
                &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_points);
            if (SUCCEEDED(hr)) printf("[D3D12] Pipeline state created (point class)\n");
            else printf("[D3D12] PSO POINT creation failed (0x%08lX)\n", hr);

            vs_blob->lpVtbl->Release(vs_blob);
            ps_blob->lpVtbl->Release(ps_blob);
        }

        /* -----------------------------------------------------------------
         * Textured triangle PSO (dbgfont / 2D atlas quads). Same MVP VS but
         * carrying UV; the PS samples the single-channel atlas as coverage
         * and modulates the vertex color, with straight alpha blending so
         * glyph edges composite over the framebuffer. Depth off (2D overlay).
         * ----------------------------------------------------------------*/
        {
            static const char vs_tex[] =
                "cbuffer cb0 : register(b0){float4 c0;float4 c1;float4 c2;float4 c3;};\n"
                "struct VSIn { float3 pos:POSITION; float4 col:COLOR; float2 uv:TEXCOORD; };\n"
                "struct VSOut{ float4 pos:SV_POSITION; float4 col:COLOR; float2 uv:TEXCOORD; };\n"
                "VSOut main(VSIn i){ VSOut o; float4 p=float4(i.pos,1.0);\n"
                "  o.pos=c0*p.x+c1*p.y+c2*p.z+c3*p.w; o.col=i.col; o.uv=i.uv; return o; }\n";
            static const char ps_tex[] =
                "Texture2D tex : register(t0);\n"
                "SamplerState smp : register(s0);\n"
                "struct PSIn{ float4 pos:SV_POSITION; float4 col:COLOR; float2 uv:TEXCOORD; };\n"
                "float4 main(PSIn i):SV_TARGET{\n"
                /* dbgfont texcoords are compressed ~10x (U) / ~8x (V) vs. the
                 * atlas; scale to recover glyph cells. Exact per-glyph offset
                 * still being calibrated. */
                "  float2 uv2 = float2(i.uv.x*10.0, i.uv.y*8.0 + 0.59);\n"
                "  float cov = tex.Sample(smp, uv2).r;\n"
                "  return float4(i.col.rgb, i.col.a * cov); }\n";

            ID3DBlob *vtb = NULL, *ptb = NULL, *e2 = NULL;
            hr = D3DCompile(vs_tex, sizeof(vs_tex) - 1, "vs_tex", NULL, NULL, "main", "vs_5_0", 0, 0, &vtb, &e2);
            if (FAILED(hr)) printf("[D3D12] VS(tex) compile failed: %s\n", e2 ? (const char*)e2->lpVtbl->GetBufferPointer(e2) : "?");
            hr = D3DCompile(ps_tex, sizeof(ps_tex) - 1, "ps_tex", NULL, NULL, "main", "ps_5_0", 0, 0, &ptb, &e2);
            if (FAILED(hr)) printf("[D3D12] PS(tex) compile failed: %s\n", e2 ? (const char*)e2->lpVtbl->GetBufferPointer(e2) : "?");

            if (vtb && ptb) {
                D3D12_INPUT_ELEMENT_DESC il[] = {
                    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                    {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                };
                D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
                pd.pRootSignature = s_d3d.root_signature;
                pd.VS.pShaderBytecode = vtb->lpVtbl->GetBufferPointer(vtb);
                pd.VS.BytecodeLength  = vtb->lpVtbl->GetBufferSize(vtb);
                pd.PS.pShaderBytecode = ptb->lpVtbl->GetBufferPointer(ptb);
                pd.PS.BytecodeLength  = ptb->lpVtbl->GetBufferSize(ptb);
                pd.InputLayout.pInputElementDescs = il;
                pd.InputLayout.NumElements = 3;
                pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                pd.BlendState.RenderTarget[0].BlendEnable    = TRUE;
                pd.BlendState.RenderTarget[0].SrcBlend       = D3D12_BLEND_SRC_ALPHA;
                pd.BlendState.RenderTarget[0].DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
                pd.BlendState.RenderTarget[0].BlendOp        = D3D12_BLEND_OP_ADD;
                pd.BlendState.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
                pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
                pd.BlendState.RenderTarget[0].BlendOpAlpha   = D3D12_BLEND_OP_ADD;
                pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                pd.SampleMask = UINT_MAX;
                pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                pd.NumRenderTargets = 1;
                pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
                pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
                pd.DepthStencilState.DepthEnable = FALSE;
                pd.DepthStencilState.StencilEnable = FALSE;
                pd.SampleDesc.Count = 1;
                hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                    s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_tex);
                if (SUCCEEDED(hr)) printf("[D3D12] Pipeline state created (textured class)\n");
                else printf("[D3D12] PSO TEX creation failed (0x%08lX)\n", hr);
            }
            if (vtb) vtb->lpVtbl->Release(vtb);
            if (ptb) ptb->lpVtbl->Release(ptb);
        }

        /* SRV descriptor heap (shader-visible). Layout: slot 0 = legacy atlas
         * (dbgfont / textured 2D path), slots 1-4 = per-draw VP textures,
         * 5-20 = offscreen render targets (render-to-texture), rest spare.
         * All slots start as null SRVs so any 4-wide table window (the
         * root signature binds t0-t3) is valid on tier-1 hardware. */
        {
            D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
            hd.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            hd.NumDescriptors = SRV_HEAP_SIZE;
            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
                s_d3d.device, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_d3d.srv_heap);
            if (FAILED(hr)) printf("[D3D12] SRV heap creation failed (0x%08lX)\n", hr);
            else {
                s_d3d.srv_inc = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
                    s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                D3D12_SHADER_RESOURCE_VIEW_DESC nv = {0};
                nv.Format = DXGI_FORMAT_R8_UNORM;
                nv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                nv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                nv.Texture2D.MipLevels = 1;
                D3D12_CPU_DESCRIPTOR_HANDLE hh;
                s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &hh);
                for (int _i = 0; _i < SRV_HEAP_SIZE; _i++) {
                    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, NULL, &nv, hh);
                    hh.ptr += s_d3d.srv_inc;
                }
            }
        }

        /* RTV heap for offscreen render targets (CPU-visible only). */
        {
            D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
            hd.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            hd.NumDescriptors = MAX_OFF_RTS;
            hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
                s_d3d.device, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_d3d.rt_rtv_heap);
            if (FAILED(hr)) printf("[D3D12] offscreen RTV heap creation failed (0x%08lX)\n", hr);
        }
    }

    /* ---------------------------------------------------------------
     * Create dynamic vertex buffer (upload heap, 4MB)
     * ---------------------------------------------------------------*/
    {
        D3D12_HEAP_PROPERTIES heap_props = {0};
        heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC buf_desc = {0};
        buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf_desc.Width = MAX_VERTICES * VERTEX_STRIDE;
        buf_desc.Height = 1;
        buf_desc.DepthOrArraySize = 1;
        buf_desc.MipLevels = 1;
        buf_desc.SampleDesc.Count = 1;
        buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        hr = s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &heap_props, D3D12_HEAP_FLAG_NONE,
            &buf_desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.vertex_buffer);
        if (SUCCEEDED(hr)) {
            D3D12_RANGE read_range = {0, 0};
            s_d3d.vertex_buffer->lpVtbl->Map(
                s_d3d.vertex_buffer, 0, &read_range, &s_d3d.vb_mapped);
            s_d3d.vb_view.BufferLocation =
                s_d3d.vertex_buffer->lpVtbl->GetGPUVirtualAddress(s_d3d.vertex_buffer);
            s_d3d.vb_view.StrideInBytes = VERTEX_STRIDE;
            s_d3d.vb_view.SizeInBytes = MAX_VERTICES * VERTEX_STRIDE;
            printf("[D3D12] Vertex buffer created (%u KB)\n",
                   (MAX_VERTICES * VERTEX_STRIDE) / 1024);
        }
    }

    /* ---------------------------------------------------------------
     * Real vertex-program path resources: root signature (CBV b0 for the
     * vp_c[] bank + SRV t0 + static sampler s0), a raw-float4 vertex buffer,
     * and the constant-bank buffer. The PSO itself is built lazily once the
     * game uploads its VP microcode (render_frame).
     * ---------------------------------------------------------------*/
    {
        /* 4-descriptor SRV table (t0-t3) so decompiled fragment programs can
         * sample up to 4 texture units; the hardcoded atlas/colour PSs use only
         * t0 and are unaffected. Matching 4 static samplers s0-s3. */
        D3D12_DESCRIPTOR_RANGE srv_range = {0};
        srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors = 4;
        srv_range.BaseShaderRegister = 0;
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER rp[3] = {0};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;   /* b0 = vp_c bank */
        rp[0].Descriptor.ShaderRegister = 0;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable.NumDescriptorRanges = 1;
        rp[1].DescriptorTable.pDescriptorRanges = &srv_range;
        rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;   /* b1 = FP texscale */
        rp[2].Descriptor.ShaderRegister = 1;
        rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp[4] = {0};
        for (int _s = 0; _s < 4; _s++) {
            samp[_s].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samp[_s].AddressU = samp[_s].AddressV = samp[_s].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            samp[_s].ShaderRegister = (UINT)_s;
            samp[_s].MaxLOD = D3D12_FLOAT32_MAX;
            samp[_s].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }
        /* s0 keeps point/clamp: the dbgfont atlas PS samples glyph cells and
         * linear filtering bleeds neighbouring glyphs. */
        samp[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        samp[0].AddressU = samp[0].AddressV = samp[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

        D3D12_ROOT_SIGNATURE_DESC rd = {0};
        rd.NumParameters = 3; rd.pParameters = rp;
        rd.NumStaticSamplers = 4; rd.pStaticSamplers = samp;
        rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ID3DBlob* sig = NULL; ID3DBlob* err = NULL;
        hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (SUCCEEDED(hr)) {
            s_d3d.device->lpVtbl->CreateRootSignature(s_d3d.device, 0,
                sig->lpVtbl->GetBufferPointer(sig), sig->lpVtbl->GetBufferSize(sig),
                &IID_ID3D12RootSignature, (void**)&s_d3d.vp_root_sig);
            sig->lpVtbl->Release(sig);
        } else if (err) { printf("[D3D12] VP root sig: %s\n", (const char*)err->lpVtbl->GetBufferPointer(err)); err->lpVtbl->Release(err); }

        /* raw float4 vertex buffer + constant bank (both UPLOAD, persistently mapped) */
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {0};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_RANGE nr = {0,0};

        bd.Width = (u64)MAX_VERTICES * 256 * 2;  /* generic VP vertex: 16 float4 slots,
                                          * DOUBLE-buffered by frame parity so a
                                          * new frame's upload never overwrites
                                          * vertices the in-flight GPU frame is
                                          * still reading (was: torn/missing
                                          * triangles mixing stale+new verts) */
        /* Failure here leaves vp_vb_mapped NULL and every VP draw silently
         * declines to upload -- the scene just does not render, with no error
         * anywhere. Report the size too: this buffer is
         * MAX_VERTICES * 256 * 2 bytes and grows fast. */
        fprintf(stderr, "[VPVB] per-frame vertex buffer: %llu MB (%u verts, x2 parity)%c",
                (unsigned long long)(bd.Width >> 20), (unsigned)MAX_VERTICES, 10);
        if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp,
                D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.vp_vb)))
            s_d3d.vp_vb->lpVtbl->Map(s_d3d.vp_vb, 0, &nr, &s_d3d.vp_vb_mapped);
        else
            fprintf(stderr, "[VPVB] ALLOCATION FAILED -- no VP geometry will render%c", 10);

        bd.Width = (u64)VP_CB_STRIDE * MAX_DRAWS * 2;   /* per-draw constant slots, x2 parity */
        /* A failure here is silent and catastrophic: vp_record_cb returns early,
         * every draw loses its constants, and the whole scene transforms by
         * zeros -- indistinguishable from a broken vertex program. Say so. */
        fprintf(stderr, "[VPCB] per-draw constant buffer: %llu MB (%d draws)%c",
                (unsigned long long)(bd.Width >> 20), MAX_DRAWS, 10);
        if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp,
                D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.vp_cb)))
            s_d3d.vp_cb->lpVtbl->Map(s_d3d.vp_cb, 0, &nr, &s_d3d.vp_cb_mapped);

        bd.Width = (u64)VP_FPCB_STRIDE * MAX_DRAWS * 2;  /* per-draw FP slots (b1), x2 parity */
        if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp,
                D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.vp_fpcb)))
            s_d3d.vp_fpcb->lpVtbl->Map(s_d3d.vp_fpcb, 0, &nr, &s_d3d.vp_fpcb_mapped);

        printf("[D3D12] VP path resources: rootsig=%p vb=%p cb=%p\n",
               (void*)s_d3d.vp_root_sig, (void*)s_d3d.vp_vb, (void*)s_d3d.vp_cb);
    }

    printf("[D3D12] Initialization complete (%ux%u, %u buffers, pipeline=%s)\n",
           width, height, FRAME_COUNT,
           s_d3d.pipeline_ready ? "ready" : "NOT ready");
    return 0;
}

/* ---------------------------------------------------------------------------
 * Frame sync helpers
 * -----------------------------------------------------------------------*/

static void wait_for_gpu(void)
{
    u32 fi = s_d3d.frame_index;
    s_d3d.fence_values[fi]++;
    s_d3d.cmd_queue->lpVtbl->Signal(s_d3d.cmd_queue, s_d3d.fence, s_d3d.fence_values[fi]);

    if (s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence) < s_d3d.fence_values[fi]) {
        s_d3d.fence->lpVtbl->SetEventOnCompletion(
            s_d3d.fence, s_d3d.fence_values[fi], s_d3d.fence_event);
        /* NEVER wait unbounded here: this runs on the vblank ticker, which also
         * drives the guest's vblank handlers, the FIFO drain, and the fence
         * publication. A device removal (TDR) leaves the D3D12 fence unsignaled
         * forever and an INFINITE wait silently froze the ENTIRE emulation --
         * observed on LBP as a boot that died the moment the 2048x2048
         * offscreen RT was created (last log line), every guest thread parked.
         * Degrade loudly instead: bounded waits + the removal reason, then
         * carry on (subsequent D3D calls fail visibly but the game keeps
         * running headless). */
        for (int tries = 0; tries < 3; tries++) {
            if (WaitForSingleObject(s_d3d.fence_event, 2000) != WAIT_TIMEOUT)
                return;
            HRESULT rr = s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device);
            printf("[D3D12] wait_for_gpu STUCK %ds: want %llu got %llu removed=0x%08lX\n",
                   2 * (tries + 1),
                   (unsigned long long)s_d3d.fence_values[fi],
                   (unsigned long long)s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence),
                   (long)rr);
            fflush(stdout);
            if (rr != 0 /* S_OK: device still alive, keep waiting a bit */)
                break;
        }
    }
}

static void move_to_next_frame(void)
{
    u64 current_fence = s_d3d.fence_values[s_d3d.frame_index];
    s_d3d.cmd_queue->lpVtbl->Signal(s_d3d.cmd_queue, s_d3d.fence, current_fence);

    s_d3d.frame_index = s_d3d.swap_chain->lpVtbl->GetCurrentBackBufferIndex(s_d3d.swap_chain);

    if (s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence) < s_d3d.fence_values[s_d3d.frame_index]) {
        s_d3d.fence->lpVtbl->SetEventOnCompletion(
            s_d3d.fence, s_d3d.fence_values[s_d3d.frame_index], s_d3d.fence_event);
        if (WaitForSingleObject(s_d3d.fence_event, 2000) == WAIT_TIMEOUT) {
            HRESULT rr = s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device);
            printf("[D3D12] FENCE STUCK 2s: want %llu got %llu removed=0x%08lX\n",
                   (unsigned long long)s_d3d.fence_values[s_d3d.frame_index],
                   (unsigned long long)s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence),
                   (long)rr);
        }
    }

    s_d3d.fence_values[s_d3d.frame_index] = current_fence + 1;
}

/* ---------------------------------------------------------------------------
 * Render a frame (clear + present)
 * -----------------------------------------------------------------------*/

/* Write the mapped readback buffer (R8G8B8A8, row pitch = readback_pitch) out
 * as a 24-bit bottom-up BMP. Debug-only. */
/* Name prefix for the next readback dump: lets the same writer emit the
 * backbuffer and the screen-copy texture under different filenames. */
static const char* s_dump_name = NULL;

static void dump_backbuffer_bmp(void)
{
    if (!s_d3d.readback_buf) return;
    void* mapped = NULL;
    D3D12_RANGE rr = {0, (SIZE_T)s_d3d.readback_pitch * s_d3d.height};
    if (FAILED(s_d3d.readback_buf->lpVtbl->Map(s_d3d.readback_buf, 0, &rr, &mapped)) || !mapped)
        return;

    static int idx = 0;
    char path[512];
    const char* dir = getenv("CELLMARK_DUMP_DIR");   /* default: current dir */
    snprintf(path, sizeof(path), "%s%s%s_%03d.bmp",
             dir ? dir : "", dir ? "/" : "", s_dump_name ? s_dump_name : "frame", idx++);
    FILE* f = fopen(path, "wb");
    if (f) {
        u32 w = s_d3d.width, h = s_d3d.height;
        u32 padded = (w * 3 + 3) & ~3u;
        u32 imgsz  = padded * h;
        u32 filesz = 54 + imgsz;
        unsigned char hdr[54] = {0};
        hdr[0] = 'B'; hdr[1] = 'M';
        hdr[2]=filesz&0xFF; hdr[3]=(filesz>>8)&0xFF; hdr[4]=(filesz>>16)&0xFF; hdr[5]=(filesz>>24)&0xFF;
        hdr[10]=54; hdr[14]=40;
        hdr[18]=w&0xFF; hdr[19]=(w>>8)&0xFF; hdr[20]=(w>>16)&0xFF; hdr[21]=(w>>24)&0xFF;
        hdr[22]=h&0xFF; hdr[23]=(h>>8)&0xFF; hdr[24]=(h>>16)&0xFF; hdr[25]=(h>>24)&0xFF;
        hdr[26]=1; hdr[28]=24;
        hdr[34]=imgsz&0xFF; hdr[35]=(imgsz>>8)&0xFF; hdr[36]=(imgsz>>16)&0xFF; hdr[37]=(imgsz>>24)&0xFF;
        fwrite(hdr, 1, 54, f);
        unsigned char* row = (unsigned char*)malloc(padded);
        if (row) {
            memset(row, 0, padded);
            for (int y = (int)h - 1; y >= 0; y--) {   /* BMP is bottom-up */
                unsigned char* src = (unsigned char*)mapped + (u32)y * s_d3d.readback_pitch;
                for (u32 x = 0; x < w; x++) {
                    row[x*3+0] = src[x*4+2]; /* B */
                    row[x*3+1] = src[x*4+1]; /* G */
                    row[x*3+2] = src[x*4+0]; /* R */
                }
                fwrite(row, 1, padded, f);
            }
            free(row);
        }
        fclose(f);
        printf("[D3D12] dumped %s (%ux%u)\n", path, s_d3d.width, s_d3d.height);
    }
    D3D12_RANGE wr = {0, 0};
    s_d3d.readback_buf->lpVtbl->Unmap(s_d3d.readback_buf, 0, &wr);
}

/* ---------------------------------------------------------------------------
 * RSX frame capture ("rsxcap") -- a purpose-built GPU debugger. RSX_CAP=start
 * [:count[:stride]] snapshots whole frames to <RSX_CAP_DIR>/frame_NN/: a text
 * manifest of every op (in submission order, with full surface/shader/texture
 * state) plus a BMP of the backbuffer and every offscreen colour RT used that
 * frame. One frame, captured atomically -- no cross-frame guessing.
 * -----------------------------------------------------------------------*/

/* Shared 24-bit BMP writer for readbacks. R8G8B8A8_UNORM straight; the half/
 * float RT formats are |v|-tonemapped so signed/HDR data is visible. */
static void cap_write_bmp(const char* path, const void* mapped, u32 w, u32 h,
                          u32 pitch, u32 dxgi)
{
    FILE* f = fopen(path, "wb");
    if (!f) return;
    u32 rowb = (w * 3 + 3) & ~3u, datasz = rowb * h;
    u8 hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    *(u32*)(hdr+2) = 54 + datasz; *(u32*)(hdr+10) = 54; *(u32*)(hdr+14) = 40;
    *(int*)(hdr+18) = (int)w; *(int*)(hdr+22) = (int)h;
    *(u16*)(hdr+26) = 1; *(u16*)(hdr+28) = 24; *(u32*)(hdr+34) = datasz;
    fwrite(hdr, 1, 54, f);
    u8* line = (u8*)malloc(rowb);
    if (line) for (int y = (int)h - 1; y >= 0; y--) {   /* BMP is bottom-up */
        const u8* srow = (const u8*)mapped + (u64)y * pitch;
        memset(line, 0, rowb);
        for (u32 x = 0; x < w; x++) {
            float rv, gv, bv;
            if (dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                const u16* hp = (const u16*)(srow + (u64)x * 8);
                float v[3];
                for (int c = 0; c < 3; c++) {
                    u16 hv = hp[c]; u32 s = (hv>>15)&1, e = (hv>>10)&0x1F, m = hv&0x3FF; float fv;
                    if (e == 0) fv = (float)m / 16777216.0f;
                    else { u32 fb = (s<<31)|((e-15+127)<<23)|(m<<13); memcpy(&fv,&fb,4); }
                    v[c] = fv;
                }
                rv = v[0]; gv = v[1]; bv = v[2];
            } else if (dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) {
                const float* fp = (const float*)(srow + (u64)x * 16);
                rv = fp[0]; gv = fp[1]; bv = fp[2];
            } else {
                const u8* p = srow + (u64)x * 4;
                rv = p[0]/255.0f; gv = p[1]/255.0f; bv = p[2]/255.0f;
            }
            float ar = rv<0?-rv:rv, ag = gv<0?-gv:gv, ab = bv<0?-bv:bv;
            if (ar>1) ar=1; if (ag>1) ag=1; if (ab>1) ab=1;
            line[x*3+0] = (u8)(ab*255.0f);
            line[x*3+1] = (u8)(ag*255.0f);
            line[x*3+2] = (u8)(ar*255.0f);
        }
        fwrite(line, 1, rowb, f);
    }
    free(line);
    fclose(f);
}

/* Copy one GPU resource into a fresh readback buffer and write it as a BMP.
 * Self-contained: waits, resets the frame command list, copies, executes,
 * waits, maps. `prior` is the resource's current tracked state (restored after
 * the copy so the caller's state tracking stays valid). */
static void cap_readback_write(ID3D12Resource* res, u32 w, u32 h, u32 dxgi,
                               D3D12_RESOURCE_STATES prior, u32 fi, const char* path)
{
    if (!res || !w || !h) return;
    u32 bpp = (dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8 :
              (dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) ? 16 : 4;
    u32 pitch = (w * bpp + 255) & ~255u;

    ID3D12Resource* rb = NULL;
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = (u64)pitch * h; rd.Height = 1; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&rb)) || !rb)
        return;

    wait_for_gpu();
    s_d3d.cmd_allocators[fi]->lpVtbl->Reset(s_d3d.cmd_allocators[fi]);
    s_d3d.cmd_list->lpVtbl->Reset(s_d3d.cmd_list, s_d3d.cmd_allocators[fi], NULL);

    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (prior != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        b.Transition.StateBefore = prior;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }

    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = (DXGI_FORMAT)dxgi;
    dst.PlacedFootprint.Footprint.Width = w; dst.PlacedFootprint.Footprint.Height = h;
    dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = pitch;
    src.pResource = res; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);

    if (prior != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter  = prior;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }

    s_d3d.cmd_list->lpVtbl->Close(s_d3d.cmd_list);
    ID3D12CommandList* cl[] = { (ID3D12CommandList*)s_d3d.cmd_list };
    s_d3d.cmd_queue->lpVtbl->ExecuteCommandLists(s_d3d.cmd_queue, 1, cl);
    wait_for_gpu();

    void* mp = NULL; D3D12_RANGE rr = {0, (SIZE_T)pitch * h};
    if (SUCCEEDED(rb->lpVtbl->Map(rb, 0, &rr, &mp)) && mp) {
        cap_write_bmp(path, mp, w, h, pitch, dxgi);
        D3D12_RANGE wr = {0, 0};
        rb->lpVtbl->Unmap(rb, 0, &wr);
    }
    rb->lpVtbl->Release(rb);
}

static void rsx_capture_frame(u32 fi, u32 ndraws, u32 capidx)
{
    char dir[300]; const char* base = getenv("RSX_CAP_DIR");
    if (!base || !*base) base = "rsxcap";
    CreateDirectoryA(base, NULL);
    snprintf(dir, sizeof dir, "%s/frame_%02u", base, capidx);
    CreateDirectoryA(dir, NULL);
    char path[512];

    /* Manifest: every op in submission order with full state. */
    snprintf(path, sizeof path, "%s/manifest.txt", dir);
    FILE* mf = fopen(path, "w");
    if (mf) {
        fprintf(mf, "frame_count=%llu draws=%u backbuffer=%ux%u\n",
                (unsigned long long)s_d3d.frame_count, ndraws, s_d3d.width, s_d3d.height);
        for (int i = 0; i < MAX_OFF_RTS; i++)
            if (s_d3d.off_rt[i].res && s_d3d.off_rt[i].used)
                fprintf(mf, "offrt[%d] off=0x%08X %ux%u dxgi=%u\n", i, s_d3d.off_rt[i].off,
                        s_d3d.off_rt[i].w, s_d3d.off_rt[i].h, s_d3d.off_rt[i].dxgi);
        fprintf(mf, "--- ops ---\n");
        for (u32 d = 0; d < ndraws && d < MAX_DRAWS; d++) {
            const D3D12DrawRecord* r = &s_d3d.draws[d];
            if (r->is_clear)
                fprintf(mf, "op%03u CLEAR rt=0x%08X mrt=0x%X/0x%X/0x%X cc=(%.3f,%.3f,%.3f,%.3f)\n",
                        d, r->rt_off, r->rt_mrt[0], r->rt_mrt[1], r->rt_mrt[2],
                        r->cc[0], r->cc[1], r->cc[2], r->cc[3]);
            else {
                fprintf(mf, "op%03u DRAW  rt=0x%08X mrt=0x%X/0x%X/0x%X vp=%u,%u,%ux%u fp=0x%08X vs=%d "
                            "n=%u cmask=%X blend=%d bk=0x%X t0=0x%08X t1=0x%08X t2=0x%08X t3=0x%08X\n",
                        d, r->rt_off, r->rt_mrt[0], r->rt_mrt[1], r->rt_mrt[2],
                        r->vp_x, r->vp_y, r->vp_w, r->vp_h,
                        r->fp_addr, r->vs_idx, r->vertex_count, r->cmask, r->blend, r->blend_key,
                        r->tex[0].raw, r->tex[1].raw, r->tex[2].raw, r->tex[3].raw);
                for (int u = 0; u < 4; u++)
                    if (r->tex[u].set)
                        fprintf(mf, "      t%d off=0x%08X fmt=0x%02X (%s%s) %ux%u remap=0x%04X\n", u,
                                r->tex[u].off, r->tex[u].fmt,
                                (r->tex[u].fmt & 0x20) ? "LN" : "SZ",
                                (r->tex[u].fmt & 0x40) ? ",NR" : "",
                                r->tex[u].w, r->tex[u].h, r->tex[u].ctrl1 & 0xFFFF);
            }
        }
        /* Per-draw VP constant banks for the first few offscreen-RT geometry
         * draws: c[0..3] is the projection row-major for most CG VPs; also scan
         * for the largest non-zero slot so a mis-placed MVP is obvious. Records
         * live at slot==draw-index; parity is post-^1 here (capture is after the
         * frame's vp_parity flip), so read the other half. */
        if (s_d3d.vp_cb_mapped) {
            u32 par = (u32)(s_d3d.vp_parity ^ 1);
            fprintf(mf, "--- vp constants (first offscreen geometry draws) ---\n");
            int shown = 0;
            for (u32 d = 0; d < ndraws && d < MAX_DRAWS && shown < 4; d++) {
                const D3D12DrawRecord* r = &s_d3d.draws[d];
                if (r->is_clear || !r->rt_off) continue;
                const float* c = (const float*)((const char*)s_d3d.vp_cb_mapped
                    + ((u64)par * MAX_DRAWS + d) * VP_CB_STRIDE);
                int last_nz = -1; float maxabs = 0.0f;
                for (int i = 0; i < RSX_MAX_VERTEX_CONSTANTS * 4; i++) {
                    float a = c[i] < 0 ? -c[i] : c[i];
                    if (a > 1e-9f) last_nz = i;
                    if (a > maxabs) maxabs = a;
                }
                fprintf(mf, "op%03u vs=%d lastNZslot=%d maxabs=%.3f\n",
                        d, r->vs_idx, last_nz / 4, maxabs);
                if (shown == 0) {   /* full non-zero slot dump for the 1st draw */
                    for (int s = 0; s <= last_nz / 4; s++) {
                        const float* v = c + s * 4;
                        if ((v[0]||v[1]||v[2]||v[3]))
                            fprintf(mf, "   c[%3d] = %9.4f %9.4f %9.4f %9.4f\n",
                                    s, v[0], v[1], v[2], v[3]);
                    }
                }
                shown++;
            }
        }
        fclose(mf);
    }

    /* Backbuffer (in PRESENT state at capture time) + every used offscreen RT. */
    snprintf(path, sizeof path, "%s/backbuffer.bmp", dir);
    cap_readback_write(s_d3d.render_targets[fi], s_d3d.width, s_d3d.height,
                       DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_PRESENT, fi, path);
    for (int i = 0; i < MAX_OFF_RTS; i++) {
        OffRT* r = &s_d3d.off_rt[i];
        if (!r->res || !r->used) continue;
        snprintf(path, sizeof path, "%s/rt_%08X.bmp", dir, r->off);
        cap_readback_write(r->res, r->w, r->h, r->dxgi, r->st, fi, path);
    }
    printf("[RSXCAP] frame %llu -> %s (%u ops)\n",
           (unsigned long long)s_d3d.frame_count, dir, ndraws);
}

/* Decompile the captured RSX vertex program to HLSL and build the VP PSO
 * (decompiled VS + atlas alpha-test PS). One-shot per program. */
static void compile_vp(void)
{
    extern int rsx_vp_decompile(const uint8_t*, u32, char*, u32);
    const rsx_state* st = s_d3d.current_rsx_state;
    if (!st || st->vp_ucode_bytes < 16 || !s_d3d.vp_root_sig) return;

    static char hlsl[64 * 1024];
    int ni = rsx_vp_decompile(st->vp_ucode, st->vp_ucode_bytes, hlsl, sizeof hlsl);
    /* Does this VP read vp_c[0..3]? Gates the garbage-projection fallback:
     * programs keeping their MVP elsewhere (gcm/cube: c[256..259]) must not
     * have c[0..3] stomped nor the viewport z-lane overridden. */
    s_d3d.vp_uses_c03 = (strstr(hlsl, "vp_c[0]") || strstr(hlsl, "vp_c[1]") ||
                         strstr(hlsl, "vp_c[2]") || strstr(hlsl, "vp_c[3]")) ? 1 : 0;
    if (ni <= 0) { printf("[VP] decompile failed (%d)\n", ni); s_d3d.vp_compiled_bytes = st->vp_ucode_bytes; return; }
    if (getenv("VP_DUMP")) {
        FILE* vf = fopen("vp_dump.hlsl", "w");
        if (vf) { fwrite(hlsl, 1, strlen(hlsl), vf); fclose(vf);
                  printf("[VP] dumped vp_dump.hlsl (%d instrs)\n", ni); }
        printf("[VPRAW] first 3 instrs (%u ucode bytes):\n", st->vp_ucode_bytes);
        for (u32 _q = 0; _q < 48 && _q < st->vp_ucode_bytes; _q += 16)
            printf("[VPRAW]  d0=%02X%02X%02X%02X d1=%02X%02X%02X%02X d2=%02X%02X%02X%02X d3=%02X%02X%02X%02X\n",
                st->vp_ucode[_q+0],st->vp_ucode[_q+1],st->vp_ucode[_q+2],st->vp_ucode[_q+3],
                st->vp_ucode[_q+4],st->vp_ucode[_q+5],st->vp_ucode[_q+6],st->vp_ucode[_q+7],
                st->vp_ucode[_q+8],st->vp_ucode[_q+9],st->vp_ucode[_q+10],st->vp_ucode[_q+11],
                st->vp_ucode[_q+12],st->vp_ucode[_q+13],st->vp_ucode[_q+14],st->vp_ucode[_q+15]);
    }

    /* Pixel shader mirrors dbgfont's FP: sample the atlas coverage at TEXCOORD0
     * (VP output o[7]), alpha-test at 0.5, output the vertex color (o[1]). */
    static const char ps[] =
        "Texture2D tex : register(t0); SamplerState smp : register(s0);\n"
        "struct PSIn{ float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1; float4 fog:FOG;\n"
        "  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
        "  float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7; };\n"
        "float4 main(PSIn i):SV_TARGET{ float cov = tex.Sample(smp, i.t0.xy).r;\n"
        "  if (cov <= 0.5) discard; return float4(i.col0.rgb, 1); }\n";

    ID3DBlob *vb=NULL,*pb=NULL,*e=NULL;
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), "vp", NULL, NULL, "main", "vs_5_0", 0, 0, &vb, &e);
    if (FAILED(hr)) { printf("[VP] VS compile FAIL: %s\n", e?(const char*)e->lpVtbl->GetBufferPointer(e):"?"); if(e)e->lpVtbl->Release(e); s_d3d.vp_compiled_bytes=st->vp_ucode_bytes; return; }
    hr = D3DCompile(ps, sizeof(ps)-1, "vpps", NULL, NULL, "main", "ps_5_0", 0, 0, &pb, &e);
    if (FAILED(hr)) { printf("[VP] PS compile FAIL: %s\n", e?(const char*)e->lpVtbl->GetBufferPointer(e):"?"); if(e)e->lpVtbl->Release(e); if(vb)vb->lpVtbl->Release(vb); s_d3d.vp_compiled_bytes=st->vp_ucode_bytes; return; }

    /* The decompiled VS declares inputs a0:ATTR0 .. a15:ATTR15 and the HLSL
     * compiler keeps whichever the program body reads. D3D12 requires EVERY VS
     * input to have a matching input-layout element (by semantic name+index),
     * so declare all 16 ATTR slots. (The old single "POSITION" element matched
     * NO VS input once the decompiler switched to ATTR semantics -> PSO failed
     * with E_INVALIDARG for every VP, blanking cellmark's text and vkcube.)
     * The VP path uploads only attrib0 to vp_vb (one float4/vertex), so every
     * slot reads that same float4 at offset 0; attributes other than position
     * therefore alias attrib0 for now (colours/uv wrong until the VP path
     * uploads multiple attributes), but geometry is correct and the PSO is
     * valid. */
    D3D12_INPUT_ELEMENT_DESC il[16];
    for (int _e = 0; _e < 16; _e++) {
        il[_e].SemanticName = "ATTR";
        il[_e].SemanticIndex = _e;
        il[_e].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        il[_e].InputSlot = 0;
        /* 256-byte generic VP vertex: every ATTRi is a float4 slot at i*16
         * (read_vp_vertex converts each enabled RSX attrib by type; disabled
         * slots hold (0,0,0,1)). Covers tiny3d (a0/a3/a8), SDK gcm samples
         * (a0/a1/a2), dbgfont -- no aliasing. */
        il[_e].AlignedByteOffset = (UINT)(_e * 16);
        il[_e].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        il[_e].InstanceDataStepRate = 0;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = s_d3d.vp_root_sig;
    pd.VS.pShaderBytecode = vb->lpVtbl->GetBufferPointer(vb); pd.VS.BytecodeLength = vb->lpVtbl->GetBufferSize(vb);
    pd.PS.pShaderBytecode = pb->lpVtbl->GetBufferPointer(pb); pd.PS.BytecodeLength = pb->lpVtbl->GetBufferSize(pb);
    pd.InputLayout.pInputElementDescs = il; pd.InputLayout.NumElements = 16;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    /* Depth test ON (LESS_EQUAL, matching the guest's rsxtiny_DepthTestFunc 515 /
     * CELL_GCM_LEQUAL). Without it the cube's faces drew in submission order and
     * back faces overwrote the front -> you saw through the near face to the
     * darker interior. The depth buffer is bound + cleared to 1.0 each frame. */
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.SampleDesc.Count = 1;
    hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_vp);

    /* Second PSO: same VS but a COLOUR-only PS (no texture sample, no alpha
     * discard) for untextured 3D geometry (vkcube's cube). The atlas PS above
     * samples a texture and discards where coverage<=0.5, which blanks any draw
     * with no atlas bound. */
    {
        static const char ps_col[] =
            "struct PSIn{ float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1; float4 fog:FOG;\n"
            "  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
            "  float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7; };\n"
            "float4 main(PSIn i):SV_TARGET{ return float4(i.col0.rgb, 1); }\n";
        ID3DBlob *pcb=NULL,*ec=NULL;
        if (SUCCEEDED(D3DCompile(ps_col, sizeof(ps_col)-1, "vppsc", NULL, NULL,
                                 "main", "ps_5_0", 0, 0, &pcb, &ec)) && pcb) {
            pd.PS.pShaderBytecode = pcb->lpVtbl->GetBufferPointer(pcb);
            pd.PS.BytecodeLength  = pcb->lpVtbl->GetBufferSize(pcb);
            HRESULT hr2 = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pd, &IID_ID3D12PipelineState,
                (void**)&s_d3d.pipeline_state_vp_color);
            printf(SUCCEEDED(hr2) ? "[VP] colour pipeline ready\n"
                                  : "[VP] colour PSO FAIL (0x%08lX)\n", hr2);
            pcb->lpVtbl->Release(pcb);
        }
        if (ec) ec->lpVtbl->Release(ec);
    }
    /* Keep the VS bytecode for guest-FP PSO builds (vp_get_fp_pso); bump the
     * generation so cached FP PSOs built against the old VS are rebuilt. */
    if (s_d3d.vp_vs_blob) s_d3d.vp_vs_blob->lpVtbl->Release(s_d3d.vp_vs_blob);
    s_d3d.vp_vs_blob = vb;
    s_d3d.vp_gen++;
    pb->lpVtbl->Release(pb);
    s_d3d.vp_compiled_bytes = st->vp_ucode_bytes;
    if (SUCCEEDED(hr)) { s_d3d.vp_ready = 1; printf("[VP] pipeline ready (%d instrs)\n", ni); }
    else {
        printf("[VP] PSO creation FAIL (0x%08lX)\n", hr);
        /* Drain the debug layer's message queue to get the EXACT validation
         * reason (E_INVALIDARG is otherwise opaque). One-shot. */
        ID3D12InfoQueue* iq = NULL;
        if (SUCCEEDED(s_d3d.device->lpVtbl->QueryInterface(
                s_d3d.device, &IID_ID3D12InfoQueue, (void**)&iq)) && iq) {
            UINT64 n = iq->lpVtbl->GetNumStoredMessages(iq);
            for (UINT64 mi = 0; mi < n; mi++) {
                SIZE_T len = 0;
                iq->lpVtbl->GetMessage(iq, mi, NULL, &len);
                D3D12_MESSAGE* m = (D3D12_MESSAGE*)malloc(len);
                if (m && SUCCEEDED(iq->lpVtbl->GetMessage(iq, mi, m, &len)))
                    printf("[VP][DBG] %s\n", m->pDescription);
                free(m);
            }
            iq->lpVtbl->Release(iq);
        }
    }
}

/* Build (or fetch) a PSO pairing the current decompiled VS with the guest's
 * FRAGMENT program at fp_addr: read the FP ucode from guest memory, decompile
 * to HLSL (rsx_fp_decompiler), compile, and cache. This replaces the two
 * hardcoded pixel shaders for draws whose FP we can translate -- e.g.
 * gcm/cube's plasma FP `c = tex2D(t0, uv).x; out = (c, 0, c, 1)`. */
#include "rsx_fp_decompiler.h"

/* Hash + compile the CURRENT rsx_state vertex program into the VS cache;
 * returns the cache slot (or -1). Called at draw-record time so each draw
 * carries the VP that was loaded when it was submitted. */
static u32 vp_hash_ucode(const u8* p, u32 n)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h ? h : 1u;
}

static int vp_get_vs(const rsx_state* st)
{
    extern int rsx_vp_decompile(const uint8_t*, u32, char*, u32);
    if (!st || st->vp_ucode_bytes < 16) return -1;
    /* Start at the instruction SET_TRANSFORM_PROGRAM_START selects, not at 0.
     * The microcode store holds every resident program -- this title keeps its
     * scene, fluid, caustics and droplet programs in it at once -- so
     * decompiling from 0 ran the scene's transform for every draw. That is why
     * the fluid's 114300 vertices rasterized nothing: they were being pushed
     * through the wrong program.
     * VP_START_OFF=1 restores the old always-from-zero behaviour. */
    u32 vstart = 0;
    { static int off = -1; if (off < 0) off = getenv("VP_START_OFF") ? 1 : 0;
      if (!off) vstart = st->transform_program_start * 16u; }
    if (vstart >= st->vp_ucode_bytes) vstart = 0;
    const u8* vuc = st->vp_ucode + vstart;
    u32 vlen = st->vp_ucode_bytes - vstart;
    u32 hash = vp_hash_ucode(vuc, vlen);
    for (int i = 0; i < s_d3d.vp_vs_n; i++)
        if (s_d3d.vp_vs[i].hash == hash) return i;

    static char hlsl[262144];
    int ni = rsx_vp_decompile(vuc, vlen, hlsl, sizeof hlsl);
    if (ni <= 0) return -1;
    if (getenv("VP_DUMP")) { static int _d=0; if (_d++ < 4) {
        FILE* f = fopen("vp2_dump.hlsl", _d==1 ? "w" : "a");
        if (f) { fprintf(f, "/* per-draw VS hash pending, %d instrs */%s%s", ni, hlsl, "\n"); fclose(f); } } }
    /* DUCK_VP=<hex tex0 offset>: write the vertex program belonging to the draws
     * that bind that texture. vp2_dump.hlsl holds whichever four programs were
     * compiled first, which need not include the one under investigation -- and
     * reading the wrong program sends you chasing the wrong constants. */
    { static const char* dp = (const char*)1; static u32 wantp = 0; static int done = 0;
      if (dp == (const char*)1) { dp = getenv("DUCK_VP");
                                  wantp = dp ? (u32)strtoul(dp, NULL, 16) : 0; }
      if (wantp && !done && s_d3d.cur_texs[0].raw == wantp) {
          done = 1;
          FILE* f = fopen("duck_vp.hlsl", "w");
          if (f) { fprintf(f, "/* duck VP, %d instrs, tex0=0x%X */%s", ni, wantp, hlsl);
                   fclose(f); }
          fprintf(stderr, "[DUCKVP] wrote duck_vp.hlsl (%d instrs)%c", ni, 10);
      } }
    /* VP_BYPASS=1: replace the guest transform with a direct map of attribute 0
     * into clip space. If geometry appears with this on, everything from the
     * input layout through raster/depth/present is sound and the fault is the
     * transform (constants or the decompiled program); if it stays blank, the
     * fault is upstream of the shader -- the attribute binding itself. Patched
     * in place, space-padded to the original length so offsets stay valid. */
    if (getenv("VP_BYPASS")) {
        static const char anchor[] =
            "Out.pos = float4(_p.xyz * vp_posscale.xyz + _p.w * vp_posoffset.xyz, _p.w);";
        static const char repl[] = "Out.pos = float4(v[0].xy * 0.5, 0.5, 1.0);";
        char* at = hlsl;
        while ((at = strstr(at, anchor)) != NULL) {
            size_t n = sizeof(anchor) - 1, m = sizeof(repl) - 1;
            memcpy(at, repl, m);
            memset(at + m, ' ', n - m);
            at += n;
        }
    }
    ID3DBlob* vb = NULL; ID3DBlob* e = NULL;
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), "guest_vp2", NULL, NULL,
                            "main", "vs_5_0", 0, 0, &vb, &e);
    if (e) e->lpVtbl->Release(e);
    if (FAILED(hr) || !vb) {
        static int _e=0; if (_e++<4) printf("[VP] per-draw VS compile FAIL (hash=0x%08X)\n", hash);
        return -1;
    }
    int slot;
    if (s_d3d.vp_vs_n < VP_VS_CACHE) slot = s_d3d.vp_vs_n++;
    else {  /* evict slot 0 */
        if (s_d3d.vp_vs[0].vs) s_d3d.vp_vs[0].vs->lpVtbl->Release(s_d3d.vp_vs[0].vs);
        memmove(&s_d3d.vp_vs[0], &s_d3d.vp_vs[1], sizeof(VPVSEntry)*(VP_VS_CACHE-1));
        slot = VP_VS_CACHE - 1;
    }
    s_d3d.vp_vs[slot].hash = hash;
    s_d3d.vp_vs[slot].vs   = vb;
    s_d3d.vp_vs[slot].uses_c03 =
        (strstr(hlsl, "vp_c[0]") || strstr(hlsl, "vp_c[1]") ||
         strstr(hlsl, "vp_c[2]") || strstr(hlsl, "vp_c[3]")) ? 1 : 0;
    { static int _n=0; if (_n++<6) printf("[VP] per-draw VS cached (hash=0x%08X, %d instrs, slot %d)\n", hash, ni, slot); }
    /* Build the base VP pipeline here if it does not exist yet. render_frame's
     * trigger reads s_d3d.current_rsx_state, which by frame end no longer has
     * the microcode -- so for a title that only ever compiles per-draw VS the
     * base pipeline was never built, s_d3d.vp_ready stayed 0, and the VP draw
     * pass it gates dropped every recorded is_vp draw. Here we are at RECORD
     * time with a live state that definitely has microcode. */
    if (!s_d3d.vp_ready && st && st->vp_ucode_bytes >= 16) {
        const rsx_state* prev = s_d3d.current_rsx_state;
        s_d3d.current_rsx_state = st;
        compile_vp();
        s_d3d.current_rsx_state = prev;
    }
    return slot;
}

/* --- Guest blend state -> packed PSO key ----------------------------------
 * RSX blend factors/equation carry GL enums; each SFACTOR/DFACTOR/EQUATION
 * word packs (alpha<<16)|colour. The key packs D3D12 enums so the PSO cache
 * distinguishes real blend modes: bit0 enable, [5:1] srcC, [10:6] dstC,
 * [15:11] srcA, [20:16] dstA, [23:21] opC, [26:24] opA. DeferredShading's
 * light accumulation is additive (ONE,ONE) -- the old hardcoded straight-
 * alpha PSO turned each light pass into a screen-blanking flash. */
static u8 gl_blend_factor_d3d(u32 f, int is_alpha)
{
    switch (f & 0xFFFF) {
    case 0x0000: return D3D12_BLEND_ZERO;
    case 0x0001: return D3D12_BLEND_ONE;
    case 0x0300: return is_alpha ? D3D12_BLEND_SRC_ALPHA     : D3D12_BLEND_SRC_COLOR;
    case 0x0301: return is_alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D12_BLEND_SRC_ALPHA;
    case 0x0303: return D3D12_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D12_BLEND_DEST_ALPHA;
    case 0x0305: return D3D12_BLEND_INV_DEST_ALPHA;
    case 0x0306: return is_alpha ? D3D12_BLEND_DEST_ALPHA     : D3D12_BLEND_DEST_COLOR;
    case 0x0307: return is_alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D12_BLEND_SRC_ALPHA_SAT;
    case 0x8001: return D3D12_BLEND_BLEND_FACTOR;       /* CONSTANT_COLOR   */
    case 0x8002: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 0x8003: return D3D12_BLEND_BLEND_FACTOR;       /* CONSTANT_ALPHA ~ */
    case 0x8004: return D3D12_BLEND_INV_BLEND_FACTOR;
    default:     return D3D12_BLEND_ONE;
    }
}
static u8 gl_blend_op_d3d(u32 e)
{
    switch (e & 0xFFFF) {
    case 0x8007: return D3D12_BLEND_OP_MIN;
    case 0x8008: return D3D12_BLEND_OP_MAX;
    case 0x800A: return D3D12_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D12_BLEND_OP_REV_SUBTRACT;
    default:     return D3D12_BLEND_OP_ADD;   /* 0x8006 FUNC_ADD / unset */
    }
}
/* Pack the guest's face-culling state for the PSO key.
 * bit0 = cull enabled, bit1 = cull FRONT (else BACK), bit2 = front face is CCW.
 * RSX/GL enums: CULL_FACE FRONT=0x0404 BACK=0x0405 FRONT_AND_BACK=0x0408;
 * FRONT_FACE CW=0x0900 CCW=0x0901. rsx_commands seeds cull_face/front_face with
 * plain 1/0 before any register arrives, so treat those defaults as BACK/CW. */
static u32 rsx_cull_key(const rsx_state* st)
{
    if (!st || !st->cull_face_enable) return 0;
    u32 k = 1u;
    if (st->cull_face == 0x0404u || st->cull_face == 0x0408u) k |= 2u;  /* FRONT */
    if (st->front_face == 0x0901u) k |= 4u;                             /* CCW */
    return k;
}

static u32 rsx_blend_key(const rsx_state* st, int enable)
{
    { static int _bd = -1; if (_bd < 0) _bd = getenv("BLENDDBG") ? 1 : 0;
      if (_bd) { static u32 seen[32]; static int ns=0;
        u32 sk = (enable?0x80000000u:0u) | (st ? (st->blend_sfactor & 0xFFFFu) : 0u);
        int f=0; for (int k2=0;k2<ns;k2++) if (seen[k2]==sk) f=1;
        if (!f && ns<32) { seen[ns++]=sk;
            fprintf(stderr, "[BLEND] enable=%d sfactor=0x%X dfactor=0x%X%c", enable,
                    st?st->blend_sfactor:0, st?st->blend_dfactor:0, 10); } } }
    if (!enable) return 0;
    /* Factors never programmed: keep the legacy straight-alpha behaviour
     * (dbgfont-style text enables blending without setting factors). */
    if (!st || (st->blend_sfactor == 0 && st->blend_dfactor == 0))
        return 1u
             | ((u32)D3D12_BLEND_SRC_ALPHA     << 1)
             | ((u32)D3D12_BLEND_INV_SRC_ALPHA << 6)
             | ((u32)D3D12_BLEND_ONE           << 11)
             | ((u32)D3D12_BLEND_INV_SRC_ALPHA << 16)
             | ((u32)D3D12_BLEND_OP_ADD << 21) | ((u32)D3D12_BLEND_OP_ADD << 24);
    return 1u
         | ((u32)gl_blend_factor_d3d(st->blend_sfactor,       0) << 1)
         | ((u32)gl_blend_factor_d3d(st->blend_dfactor,       0) << 6)
         | ((u32)gl_blend_factor_d3d(st->blend_sfactor >> 16, 1) << 11)
         | ((u32)gl_blend_factor_d3d(st->blend_dfactor >> 16, 1) << 16)
         | ((u32)gl_blend_op_d3d(st->blend_equation)       << 21)
         | ((u32)gl_blend_op_d3d(st->blend_equation >> 16) << 24);
}

/* Bound colour target count for a draw record: 1 (target A) + the contiguous
 * prefix of MRT B/C/D offsets. */
static int dr_num_rts(const D3D12DrawRecord* dr)
{
    int n = 1;
    while (n < 4 && dr->rt_mrt[n - 1]) n++;
    return n;
}

/* Replace every occurrence of `find` with `repl` inside a NUL-terminated buffer,
 * shifting the tail. Used to retarget generated HLSL (2D sampler -> cube). */
static void hlsl_replace_all(char* buf, size_t cap, const char* find, const char* repl)
{
    size_t fl = strlen(find), rl = strlen(repl);
    if (!fl) return;
    char* at = buf;
    while ((at = strstr(at, find)) != NULL) {
        size_t used = strlen(buf) + 1;
        if (used + rl - fl > cap) return;
        memmove(at + rl, at + fl, used - (size_t)(at - buf) - fl);
        memcpy(at, repl, rl);
        at += rl;
    }
}

/* Which of a draw's texture units are cube textures, as a 4-bit mask. */
static u32 dr_cube_mask(const D3D12DrawRecord* dr)
{
    /* ON by default (CUBE_TEX=0 to disable). This was off while the cube path
     * cost 2262 PSO misses per 20 frames and 0.38 fps -- both caused by inline
     * fragment-program constants baking into the shader source. With those
     * constants hoisted into b1 the cube path costs 0 PSO misses, and with the
     * per-face stride fixed (each face is a whole mip pyramid, not one mip-0
     * image) all six faces decode as clean environment art. */
    static int en = -1;
    if (en < 0) { const char* e = getenv("CUBE_TEX"); en = e ? atoi(e) : 1; }
    if (!en) return 0;
    u32 m = 0;
    for (int u = 0; u < 4; u++) if (dr->tex[u].set && dr->tex[u].cube) m |= 1u << u;
    return m;
}

static ID3D12PipelineState* vp_get_fp_pso(int vs_idx, u32 fp_addr, u32 blend, int nrt,
                                          DXGI_FORMAT rtfmt, int exp32, u32 cmask, u32 cull,
                                          u32 cube_mask)
{
    if (nrt < 1) nrt = 1; if (nrt > 4) nrt = 4;
    if (rtfmt == 0) rtfmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveOffset(u32);
    if (!fp_addr || !vm_base) return NULL;
    { static int _off=-1; if(_off<0)_off=getenv("FP_OFF")?1:0; if(_off) return NULL; }
    /* Resolve the VS: the draw's cached per-VP blob, else the primary. */
    ID3DBlob* vsb = NULL; u32 vs_hash = 0;
    if (vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n) {
        vsb = s_d3d.vp_vs[vs_idx].vs; vs_hash = s_d3d.vp_vs[vs_idx].hash;
    }
    if (!vsb) { vsb = s_d3d.vp_vs_blob; vs_idx = -1; }
    if (!vsb) return NULL;

    /* SET_SHADER_PROGRAM low bits = location+1 (same as textures): 1 = LOCAL
     * (VRAM), 2 = MAIN (gcm/cube emits 0x00B90001 for its VRAM-resident FP). */
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    u32 off = cellGcmResolveLocated((fp_addr & 0x3u) == 1, fp_addr & ~0x3u);
    if (off == 0xFFFFFFFFu) return NULL;

    /* Content hash: inline constants are patched in place per frame (wave's
     * stamp), so identity is the BYTES, not the address. */
    /* Hash the CODE, not the constants: with fp_k[] hoisted into the per-draw
     * constant buffer the compiled shader is invariant under constant changes,
     * so hashing them made the pipeline key move every draw. */
    u32 uhash;
    {
        u32 usz = rsx_fp_program_size(vm_base + off, 4096);
        if (usz == 0) usz = 64;
        s_perf_pso_hashbytes += (int)usz;
        if (getenv("FP_CONSTBUF") && getenv("FP_CONSTBUF")[0] == '0') {
            uhash = 2166136261u;
            const u8* up = vm_base + off;
            for (u32 i = 0; i < usz; i++) { uhash ^= up[i]; uhash *= 16777619u; }
        } else {
            uhash = rsx_fp_code_hash(vm_base + off, 4096);
        }
    }

    s_perf_pso_calls++;
    for (int i = 0; i < s_d3d.vp_fp_n; i++)
        if (s_d3d.vp_fp[i].fp_addr != fp_addr) continue;   /* cheap reject first */
        else if (s_d3d.vp_fp[i].vs_idx == vs_idx &&
            s_d3d.vp_fp[i].vs_hash == vs_hash && s_d3d.vp_fp[i].gen == s_d3d.vp_gen &&
            s_d3d.vp_fp[i].blend == blend && s_d3d.vp_fp[i].nrt == nrt &&
            s_d3d.vp_fp[i].rtfmt == (u32)rtfmt && s_d3d.vp_fp[i].exp32 == exp32 &&
            s_d3d.vp_fp[i].ucode_hash == uhash && s_d3d.vp_fp[i].cmask == cmask &&
            s_d3d.vp_fp[i].cull == cull &&
            s_d3d.vp_fp[i].cube_mask == cube_mask)
            return s_d3d.vp_fp[i].pso;
    s_perf_pso_miss++;      /* falls through to a full decompile + D3DCompile */
    /* PSOMISSDBG=1: on a miss, name the key field that differs from an existing
     * entry for the same program. "the cache misses" is not actionable; "it
     * misses on ucode_hash" is. */
    /* HASHDBG=1: how many DISTINCT ucode hashes each program produces. Inline
     * constants are patched into the shader bytes, so a program whose constants
     * animate yields a new hash -- and a new pipeline -- every time. */
    { static int hd = -1;
      if (hd < 0) { const char* e = getenv("HASHDBG"); hd = e ? atoi(e) : 0; }
      if (hd) { static u32 fps[32]; static u32 cnt[32]; static int nf = 0;
        int idx = -1;
        for (int i = 0; i < nf; i++) if (fps[i] == fp_addr) idx = i;
        if (idx < 0 && nf < 32) { idx = nf++; fps[idx] = fp_addr; cnt[idx] = 0; }
        if (idx >= 0) { cnt[idx]++;
          if ((cnt[idx] % 200) == 0)
            fprintf(stderr, "[HASHDBG] fp=0x%X has produced %u distinct compiles%c",
                    fp_addr, cnt[idx], 10); } } }
    { static int md = -1;
      if (md < 0) { const char* e = getenv("PSOMISSDBG"); md = e ? atoi(e) : 0; }
      if (md) { static int n = 0;
        for (int i = 0; i < s_d3d.vp_fp_n && n < 12; i++) {
            VPFPEntry* q = &s_d3d.vp_fp[i];
            if (q->fp_addr != fp_addr) continue;
            n++;
            fprintf(stderr, "[PSOMISS] fp=0x%X differs:%s%s%s%s%s%s%s%s%c", fp_addr,
                    q->vs_idx != vs_idx        ? " vs_idx"     : "",
                    q->vs_hash != vs_hash      ? " vs_hash"    : "",
                    q->gen != s_d3d.vp_gen     ? " gen"        : "",
                    q->blend != blend          ? " blend"      : "",
                    q->nrt != nrt              ? " nrt"        : "",
                    q->rtfmt != (u32)rtfmt     ? " rtfmt"      : "",
                    q->ucode_hash != uhash     ? " ucode_hash" : "",
                    q->cube_mask != cube_mask  ? " cube_mask"  : "", 10);
            break;
        } } }
    /* CUBEKEYDBG=1: which (program, cube mask) pairs are being compiled. If one
     * program shows several masks, the mask is unstable and is the reason the
     * pipeline cache thrashes with cube support on. */
    { static int ck = -1;
      if (ck < 0) { const char* e = getenv("CUBEKEYDBG"); ck = e ? atoi(e) : 0; }
      if (ck) { static u32 seen[64][2]; static int ns = 0; int known = 0;
        for (int i = 0; i < ns; i++)
            if (seen[i][0] == fp_addr && seen[i][1] == cube_mask) known = 1;
        if (!known && ns < 64) { seen[ns][0] = fp_addr; seen[ns][1] = cube_mask; ns++;
            fprintf(stderr, "[CUBEKEY] fp=0x%X cube_mask=0x%X (distinct pairs=%d)%c",
                    fp_addr, cube_mask, ns, 10); } } }
    static char hlsl[32768];
    int n = rsx_fp_decompile(vm_base + off, 4096, hlsl, sizeof(hlsl), exp32);
    if (n <= 0) { static int _e=0; if(_e++<16) printf("[FP] decompile fail (fp=0x%08X)\n", fp_addr); return NULL; }
    if (getenv("FP_DUMP")) { static int _d=0; if (_d++ < 4) {
        FILE* f = fopen("fp_dump.hlsl", _d==1 ? "w" : "a");
        if (f) {
            const u8* uc = vm_base + off;
            fprintf(f, "/* fp_addr=0x%08X vmoff=0x%08X raw:", fp_addr, off);
            for (int _b = 0; _b < 32; _b++) fprintf(f, " %02X", uc[_b]);
            fprintf(f, " */\n%s\n", hlsl); fclose(f);
        } } }

    /* FP_PICK=<hex fp_addr>: write just that program, whatever order it compiles
     * in. fp_dump.hlsl keeps only the first four, same trap as vp2_dump.hlsl. */
    { static const char* fpk = (const char*)1; static u32 wantf = 0; static int done = 0;
      if (fpk == (const char*)1) { fpk = getenv("FP_PICK");
                                   wantf = fpk ? (u32)strtoul(fpk, NULL, 16) : 0; }
      if (wantf && !done && fp_addr == wantf) { done = 1;
          FILE* f = fopen("duck_fp.hlsl", "w");
          if (f) { fprintf(f, "/* fp_addr=0x%08X, %d instrs */%s", fp_addr, n, hlsl);
                   fclose(f); }
          fprintf(stderr, "[DUCKFP] wrote duck_fp.hlsl (fp=0x%08X, %d instrs)%c",
                  fp_addr, n, 10);
      } }

    /* Debug FP_ONE=<hex fp_addr>: force that program's colour output to
     * all-ones (e.g. wave's colour-detect -> full mask -> island borders
     * everywhere -> the water sim must visibly radiate if it works). */
    { const char* f1 = getenv("FP_ONE");
      if (f1 && (u32)strtoul(f1, NULL, 16) == fp_addr) {
          /* Match the assignment, not one exact spelling of it: the final
           * register is r[N] for some programs and h[N] for others, and the old
           * fixed "= r[0];" anchor silently did nothing for every h[] shader --
           * so this switch appeared to have no effect and was useless for
           * identifying which program draws what. Rewrite the expression with a
           * proper length-changing splice. */
          char* rp = strstr(hlsl, "_po.c0 = ");
          if (rp) {
              char* semi = strchr(rp, ';');
              if (semi) {
                  static const char rep[] = "_po.c0 = (1.0).xxxx";
                  size_t oldlen = (size_t)(semi - rp), newlen = sizeof(rep) - 1;
                  size_t tail = strlen(semi) + 1;
                  if ((rp - hlsl) + newlen + tail < sizeof(hlsl)) {
                      memmove(rp + newlen, semi, tail);
                      memcpy(rp, rep, newlen);
                  }
                  (void)oldlen;
              }
          }
      } }
    /* FP_TEX=1: make every program output its unit-0 texture sample verbatim.
     * Separates "the texture is wrong" from "the shading tints it" in one run,
     * without having to identify which program draws which surface first. */
    /* FP_SHOW=<hlsl expr>: replace a program's colour output with an arbitrary
     * expression (e.g. FP_SHOW=h[1]) so an intermediate register can be looked
     * at directly. Restrict with FP_TEX_FP=<hex>. Reading a 69-instruction
     * shader to guess which term carries a bad value is slower and less certain
     * than displaying the terms. */
    /* FP_KILL=<hex fp_addr>: make that program discard every fragment, so what
     * it covers becomes visible. Distinguishes "this surface shades black" from
     * "this surface is correct and something else is missing behind it". */
    { const char* kf = getenv("FP_KILL");
      if (kf && (u32)strtoul(kf, NULL, 16) == fp_addr) {
          char* rp = strstr(hlsl, "PSOut _po;");
          if (rp) {
              const char* ins = "discard; ";
              size_t nl = strlen(ins), tail = strlen(rp) + 1;
              if ((size_t)(rp - hlsl) + nl + tail < sizeof(hlsl)) {
                  memmove(rp + nl, rp, tail);
                  memcpy(rp, ins, nl);
                  fprintf(stderr, "[FPKILL] fp=0x%X discards%c", fp_addr, 10);
              }
          }
      } }
    { const char* sh = getenv("FP_SHOW");
      const char* only = getenv("FP_TEX_FP");
      if (sh && (!only || (u32)strtoul(only, NULL, 16) == fp_addr)) {
          char* rp = strstr(hlsl, "_po.c0 = ");
          if (rp) { char* semi = strchr(rp, ';');
            if (semi) {
                char rep[256];
                snprintf(rep, sizeof rep, "_po.c0 = float4((%s).xyz, 1.0)", sh);
                size_t newlen = strlen(rep), tail = strlen(semi) + 1;
                if ((size_t)(rp - hlsl) + newlen + tail < sizeof(hlsl)) {
                    memmove(rp + newlen, semi, tail);
                    memcpy(rp, rep, newlen);
                    fprintf(stderr, "[FPSHOW] fp=0x%X output replaced with %s%c",
                            fp_addr, sh, 10);
                }
            } }
      } }
    /* Retarget the samplers the guest bound as CUBE textures. The decompiler
     * always emits 2D samplers; a cube is sampled with a 3-component direction
     * and no texel-scale, and the emitted coordinate always ends in the fixed
     * tail ".xy * rsx_texscale[N].xy", so both edits are exact replacements. */
    if (cube_mask) {
        for (int _u = 0; _u < 4; _u++) {
            if (!(cube_mask & (1u << _u))) continue;
            char f1[64], r1[64], f2[64], r2[64];
            snprintf(f1, sizeof f1, "Texture2D    rsx_tex%d", _u);
            snprintf(r1, sizeof r1, "TextureCube  rsx_tex%d", _u);
            hlsl_replace_all(hlsl, sizeof hlsl, f1, r1);
            snprintf(f1, sizeof f1, "Texture2D rsx_tex%d", _u);
            snprintf(r1, sizeof r1, "TextureCube rsx_tex%d", _u);
            hlsl_replace_all(hlsl, sizeof hlsl, f1, r1);
            snprintf(f2, sizeof f2, ".xy * rsx_texscale[%d].xy", _u);
            snprintf(r2, sizeof r2, ".xyz");
            hlsl_replace_all(hlsl, sizeof hlsl, f2, r2);
        }
        { static int _n = 0; if (_n++ < 4)
            fprintf(stderr, "[CUBE] fp=0x%X compiled with cube units mask 0x%X%c",
                    fp_addr, cube_mask, 10); }
    }
    /* FP_IDCOLOR=1: give every fragment program a distinct flat colour derived
     * from its address. One frame then shows which program paints which surface,
     * instead of one run per candidate to test them by elimination. */
    if (getenv("FP_IDCOLOR")) {
        char* rp = strstr(hlsl, "_po.c0 = ");
        if (rp) {
            char* semi = strchr(rp, ';');
            if (semi) {
                /* Mix properly: every fp address in a title tends to share low
                 * bits (they all end 0x01/0x81 here), so a single multiply left
                 * one channel constant across all programs and made distinct
                 * shaders look like the same colour. */
                u32 hsh = fp_addr * 2654435761u;
                hsh ^= hsh >> 13; hsh *= 0x5BD1E995u; hsh ^= hsh >> 15;
                /* Force the top bit on in each channel: a hash that happens to
                 * land near black is indistinguishable from "this surface was
                 * never painted", which is the exact question the mode exists
                 * to answer. */
                float cr = (float)(((hsh >> 16) & 0x7F) | 0x80) / 255.0f;
                float cg = (float)(((hsh >>  8) & 0x7F) | 0x80) / 255.0f;
                float cb = (float)(((hsh      ) & 0x7F) | 0x80) / 255.0f;
                char rep[128];
                snprintf(rep, sizeof rep, "_po.c0 = float4(%.3f,%.3f,%.3f,1.0)", cr, cg, cb);
                size_t newlen = strlen(rep), tail = strlen(semi) + 1;
                if ((size_t)(rp - hlsl) + newlen + tail < sizeof(hlsl)) {
                    memmove(rp + newlen, semi, tail);
                    memcpy(rp, rep, newlen);
                    fprintf(stderr, "[FPID] fp=0x%X -> rgb(%.0f,%.0f,%.0f)%c",
                            fp_addr, cr*255, cg*255, cb*255, 10);
                }
            }
        }
    }
    /* FP_TEX=<unit>: output that unit's sample verbatim. FP_TEX_FP=<hex> limits
     * it to one program, so one surface can be inspected without repainting the
     * whole scene. FP_TEX_UV=1 samples with the same texcoord the program uses
     * for that unit rather than tc0. */
    if (getenv("FP_TEX") && (!getenv("FP_TEX_FP") ||
        (u32)strtoul(getenv("FP_TEX_FP"), NULL, 16) == fp_addr)) {
        int _un = atoi(getenv("FP_TEX"));
        if (_un < 0 || _un > 3) _un = 0;
        char* rp = strstr(hlsl, "_po.c0 = ");
        if (rp) {
            char* semi = strchr(rp, ';');
            if (semi) {
                char rep[160];
                snprintf(rep, sizeof rep,
                         "_po.c0 = rsx_tex%d.Sample(rsx_samp%d, input.tc0.xy * rsx_texscale[%d].xy)",
                         _un, _un, _un);
                size_t newlen = strlen(rep), tail = strlen(semi) + 1;
                fprintf(stderr, "[FP_TEX] program 0x%X rewritten to sample unit %d%c",
                        fp_addr, _un, 10);
                if ((size_t)(rp - hlsl) + newlen + tail < sizeof(hlsl)) {
                    memmove(rp + newlen, semi, tail);
                    memcpy(rp, rep, newlen);
                }
            }
        }
    }
    /* FP_FORCE=1: replace the translated body with solid magenta -- isolates
     * geometry/transform problems from texture/blend problems. */
    if (getenv("FP_FORCE")) {
        char* rp = strstr(hlsl, "return r[0];");
        if (rp) memcpy(rp, "return float4(1,0,1,1)", 23);
    }
    ID3DBlob* pb = NULL; ID3DBlob* e = NULL;
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), "guest_fp", NULL, NULL,
                            "main", "ps_5_0", 0, 0, &pb, &e);
    if (FAILED(hr) || !pb) {
        static int _e2=0; if (_e2++<16)
            printf("[FP] PS compile FAIL (fp=0x%08X): %s\n", fp_addr,
                   e ? (const char*)e->lpVtbl->GetBufferPointer(e) : "?");
        if (e) e->lpVtbl->Release(e);
        return NULL;
    }
    if (e) e->lpVtbl->Release(e);

    D3D12_INPUT_ELEMENT_DESC il[16];
    for (int _i = 0; _i < 16; _i++) {
        il[_i].SemanticName = "ATTR"; il[_i].SemanticIndex = _i;
        il[_i].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; il[_i].InputSlot = 0;
        il[_i].AlignedByteOffset = (UINT)(_i * 16);
        il[_i].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        il[_i].InstanceDataStepRate = 0;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = s_d3d.vp_root_sig;
    pd.VS.pShaderBytecode = vsb->lpVtbl->GetBufferPointer(vsb);
    pd.VS.BytecodeLength  = vsb->lpVtbl->GetBufferSize(vsb);
    pd.PS.pShaderBytecode = pb->lpVtbl->GetBufferPointer(pb);
    pd.PS.BytecodeLength  = pb->lpVtbl->GetBufferSize(pb);
    pd.InputLayout.pInputElementDescs = il; pd.InputLayout.NumElements = 16;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    /* Guest face culling. CULL_OFF=1 forces double-sided, the old
     * unconditional behaviour -- geometry that vanishes under culling is a
     * winding/front-face problem, not a missing draw. */
    { static int _co = -1; if (_co < 0) _co = getenv("CULL_OFF") ? 1 : 0;
      pd.RasterizerState.CullMode = (!_co && (cull & 1u))
          ? ((cull & 2u) ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK)
          : D3D12_CULL_MODE_NONE;
      /* CULL_FLIP=1: invert the front-face convention. Our VP path may hand
       * D3D the opposite winding from the guest's, in which case honouring
       * CULL_FACE culls exactly the faces it should keep. */
      static int _cfl = -1; if (_cfl < 0) _cfl = getenv("CULL_FLIP") ? 1 : 0;
      pd.RasterizerState.FrontCounterClockwise =
          (((cull & 4u) ? 1 : 0) ^ _cfl) ? TRUE : FALSE; }
    /* Blend per the guest's packed key (see rsx_blend_key): dbgfont text needs
     * straight alpha; demosaic's effect passes blend OFF; DeferredShading's
     * light accumulation is additive ONE,ONE. */
    pd.BlendState.RenderTarget[0].BlendEnable    = (blend & 1) ? TRUE : FALSE;
    pd.BlendState.RenderTarget[0].SrcBlend       = (blend & 1) ? (D3D12_BLEND)((blend >> 1)  & 0x1F) : D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].DestBlend      = (blend & 1) ? (D3D12_BLEND)((blend >> 6)  & 0x1F) : D3D12_BLEND_ZERO;
    pd.BlendState.RenderTarget[0].BlendOp        = (blend & 1) ? (D3D12_BLEND_OP)((blend >> 21) & 0x7) : D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].SrcBlendAlpha  = (blend & 1) ? (D3D12_BLEND)((blend >> 11) & 0x1F) : D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].DestBlendAlpha = (blend & 1) ? (D3D12_BLEND)((blend >> 16) & 0x1F) : D3D12_BLEND_ZERO;
    pd.BlendState.RenderTarget[0].BlendOpAlpha   = (blend & 1) ? (D3D12_BLEND_OP)((blend >> 24) & 0x7) : D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = (UINT)nrt;
    for (int _r = 0; _r < nrt; _r++) {
        pd.RTVFormats[_r] = rtfmt;
        /* Zero-init leaves RenderTargetWriteMask 0 on secondary targets --
         * every MRT-B write would be masked off. Mirror RT0's blend state. */
        pd.BlendState.RenderTarget[_r] = pd.BlendState.RenderTarget[0];
    }
    /* Guest colour write mask (RGBA nibble, already D3D-ordered). cmask is 0xF
     * by default (unset colour_mask register decodes to all-on); a literal 0
     * therefore means the guest EXPLICITLY masked every channel -- a depth-only
     * pass (e.g. DeferredShading's shadow-map generation). Honour it: forcing 0
     * back to 0xF splatters the depth pass's fragment colour onto the target. */
    /* CMASK_FORCE=1: ignore the guest colour mask (write RGBA on every draw).
     * A mis-decoded mask makes geometry rasterize correctly and write nothing,
     * which is indistinguishable from "the draw never happened". */
    { static int _cf = -1; if (_cf < 0) _cf = getenv("CMASK_FORCE") ? 1 : 0;
      if (_cf) cmask = 0xF; }
    for (int _r = 0; _r < nrt; _r++)
        pd.BlendState.RenderTarget[_r].RenderTargetWriteMask = (UINT8)(cmask & 0xF);
    pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pd.DepthStencilState.StencilEnable = FALSE;
    /* DEPTH_OFF=1: drop the depth test for guest-FP draws, so submission order
     * alone decides what is on top. Paired with DRAW_LAST_TEX this puts one
     * object in front of everything and answers "is it merely occluded?" --
     * which reordering alone cannot, because a relocated draw still fails the
     * depth test against whatever is already in the buffer. Diagnostic. */
    { static int doff = -1;
      if (doff < 0) { const char* e = getenv("DEPTH_OFF"); doff = e ? atoi(e) : 0; }
      if (doff) { pd.DepthStencilState.DepthEnable = FALSE;
                  pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; } }
    pd.SampleDesc.Count = 1;

    ID3D12PipelineState* pso = NULL;
    hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
        s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&pso);
    pb->lpVtbl->Release(pb);
    if (FAILED(hr)) {
        static int _e3=0; if (_e3++<16) printf("[FP] PSO FAIL (fp=0x%08X, 0x%08lX)\n", fp_addr, hr);
        return NULL;
    }
    { static int _ok=0; if (_ok++<32) printf("[FP] guest FP pipeline ready (fp=0x%08X)\n", fp_addr); }

    /* insert (evict oldest when full) */
    if (s_d3d.vp_fp_n >= VP_FP_CACHE) {
        if (s_d3d.vp_fp[0].pso) s_d3d.vp_fp[0].pso->lpVtbl->Release(s_d3d.vp_fp[0].pso);
        memmove(&s_d3d.vp_fp[0], &s_d3d.vp_fp[1], sizeof(VPFPEntry) * (VP_FP_CACHE - 1));
        s_d3d.vp_fp_n = VP_FP_CACHE - 1;
    }
    s_d3d.vp_fp[s_d3d.vp_fp_n].fp_addr = fp_addr;
    s_d3d.vp_fp[s_d3d.vp_fp_n].vs_idx  = vs_idx;
    s_d3d.vp_fp[s_d3d.vp_fp_n].vs_hash = vs_hash;
    s_d3d.vp_fp[s_d3d.vp_fp_n].gen     = s_d3d.vp_gen;
    s_d3d.vp_fp[s_d3d.vp_fp_n].blend   = blend;
    s_d3d.vp_fp[s_d3d.vp_fp_n].nrt     = nrt;
    s_d3d.vp_fp[s_d3d.vp_fp_n].rtfmt   = (u32)rtfmt;
    s_d3d.vp_fp[s_d3d.vp_fp_n].exp32   = exp32;
    s_d3d.vp_fp[s_d3d.vp_fp_n].ucode_hash = uhash;
    s_d3d.vp_fp[s_d3d.vp_fp_n].cmask   = cmask;
    s_d3d.vp_fp[s_d3d.vp_fp_n].cull    = cull;
    s_d3d.vp_fp[s_d3d.vp_fp_n].cube_mask = cube_mask;
    s_d3d.vp_fp[s_d3d.vp_fp_n].pso     = pso;
    s_d3d.vp_fp_n++;
    return pso;
}

/* Upload the guest texture for a VP draw into a per-frame texture slot
 * (re-uploaded every frame: gcm/cube's plasma animates in guest memory).
 * Returns the slot index (SRV at heap 1+slot) or -1. Must run while the
 * command list is open, before the draw passes. */
/* NV4097 TEXTURE_CONTROL1 component remap -> D3D12 Shader4ComponentMapping.
 * The crossbar decode itself is RSX semantics and lives in
 * rsx_texture_layout.c; all that is left here is packing the four selectors
 * into D3D12's word. Its force-zero/force-one encodings are 4 and 5, which is
 * what RSX_REMAP_ZERO/ONE already are, so the selectors pass straight through.
 *
 * out[] arrives in the crossbar's field order A,R,G,B; D3D12 wants
 * destR | destG<<3 | destB<<6 | destA<<9 | valid<<12. */
static u32 rsx_remap_to_d3d(u32 c1, u32 basef)
{
    u8 out[4];
    rsx_texture_component_remap(c1, basef, out);
    /* TEX_REMAP_ID=<n>: override the derived crossbar with a fixed mapping, to
     * test channel order directly. The resource holds the guest's A8R8G8B8
     * bytes straight through, so its components are (R=A, G=R, B=G, A=B) and
     * the shader needs destR=1, destG=2, destB=3, destA=0 to see real RGBA.
     *   1 = identity   2 = rotate (the ARGB fix)   3 = BGRA swap */
    { static int fixed = -1;
      if (fixed < 0) { const char* e = getenv("TEX_REMAP_ID"); fixed = e ? atoi(e) : 0; }
      if (fixed == 1) return 0u | (1u<<3) | (2u<<6) | (3u<<9) | (1u<<12);
      if (fixed == 2) return 1u | (2u<<3) | (3u<<6) | (0u<<9) | (1u<<12);
      if (fixed == 3) return 2u | (1u<<3) | (0u<<6) | (3u<<9) | (1u<<12); }
    /* D3D12 mapping: destR | destG<<3 | destB<<6 | destA<<9 | valid bit */
    { static int _rd = -1; if (_rd < 0) _rd = getenv("REMAPDBG") ? 1 : 0;
      if (_rd) { static u32 seen[32]; static int ns=0;
        u32 k = (basef << 16) | (c1 & 0xFFFFu); int f=0;
        for (int i2=0;i2<ns;i2++) if (seen[i2]==k) f=1;
        if (!f && ns<32) { seen[ns++]=k;
            fprintf(stderr, "[REMAP] basef=0x%02X c1=0x%04X -> destR=%u destG=%u destB=%u destA=%u%c",
                    basef, c1 & 0xFFFFu, out[1], out[2], out[3], out[0], 10); } } }
    return out[1] | (out[2] << 3) | (out[3] << 6) | (out[0] << 9) | (1u << 12);
}

/* Morton/Z-order texel offset for RSX swizzled textures (LN bit clear).
 * Interleaves x (even bit positions) and y (odd) until the smaller dimension's
 * bits run out, then the larger dimension's remaining bits ride above -- the
 * NV40 layout (matches RPCS3 convert_linear_swizzle). POT dims only, which is
 * what the hardware requires for swizzled textures. */

/* Sparse FNV-1a over a texture's source bytes -- enough to notice an animated
 * surface changing, cheap enough to run on every bind. Kept a function rather
 * than a statement-expression macro so the file still compiles under MSVC;
 * ps3recomp builds with clang-cl, but titles like Tokyo Jungle drive the build
 * through the Visual Studio generator. */
static u32 tex_csum(const u8* base, u32 nbytes)
{
    u32 h = 2166136261u;
    u32 step = nbytes > 4096u ? nbytes / 1024u : 4u;
    for (u32 i = 0; i + 3 < nbytes; i += step) {
        u32 w32; memcpy(&w32, base + i, sizeof w32);
        h ^= w32; h *= 16777619u;
    }
    return h;
}

static int vp_upload_tex_slot(u32 off, u32 w, u32 h, u32 fmt, int cube, u32 mips)
{
    extern uint8_t* vm_base;

    if (!off || !w || !h || !vm_base || !s_d3d.srv_heap) return -1;
    /* Format classes (base = fmt & 0x9F). The LBP loading screen uses:
     * 0x85 A8R8G8B8 (swizzled UI art), 0x8B G8B8 (the 1024x2048 linear FONT
     * atlas -- without it no text renders at all), 0x86/87/88 DXT1/23/45
     * (512x512 detail/LUT layers bound at t1/t3 on every draw). */
    /* Format class, row sizes and swizzling are RSX semantics: rsx_texture_
     * layout.c owns them so the Metal backend can read a guest texture without
     * re-deriving any of it. Only the DXGI mapping below is ours. */
    rsx_tex_layout tl;
    rsx_texture_layout(fmt, w, h, &tl);
    u32 basef = fmt & 0x9F;              /* still needed for the SRV remap */
    int argb = (tl.fmt == RSX_TEXFMT_R8G8B8A8);
    int dxt  = tl.compressed;
    /* Compressed rows are counted in blocks, so keep bpp at 1 for the byte
     * arithmetic the diagnostics below do. */
    u32 bpp = tl.compressed ? 1u : tl.bytes_per_texel;
    DXGI_FORMAT dxfmt = (tl.fmt == RSX_TEXFMT_R8G8B8A8) ? DXGI_FORMAT_R8G8B8A8_UNORM
                      : (tl.fmt == RSX_TEXFMT_R8G8)     ? DXGI_FORMAT_R8G8_UNORM
                      : (tl.fmt == RSX_TEXFMT_BC1)      ? DXGI_FORMAT_BC1_UNORM
                      : (tl.fmt == RSX_TEXFMT_BC2)      ? DXGI_FORMAT_BC2_UNORM
                      : (tl.fmt == RSX_TEXFMT_BC3)      ? DXGI_FORMAT_BC3_UNORM
                      : DXGI_FORMAT_R8_UNORM;
    /* DXT data is stored as linear 4x4 block rows (compressed formats are
     * never Morton-swizzled on RSX). Row of blocks = (w/4)*blocksize. */
    u32 blkrow  = dxt ? tl.row_bytes : 0;
    u32 blkrows = dxt ? tl.rows      : 0;
    const u32 key_off = off;         /* lookup key: the offset as bound */
    /* Sparse checksum of a texture's source bytes -- enough to notice an
     * animated surface changing, cheap enough to run on every bind. */
    /* (portable helper tex_csum() -- see above; a statement-expression macro
     * here was a GCC/Clang extension MSVC rejects.) */
    #define TEX_CSUM(base, nbytes) tex_csum((base), (nbytes))
    int slot = -1, freeslot = -1;
    for (int i = 0; i < VP_TEX_SLOTS; i++) {
        VPTexSlot* c = &s_d3d.vp_tex[i];
        if (c->res && c->key == key_off && c->w == w && c->h == h && c->fmt == fmt
            && c->cube == cube) {
            if (c->used) return i;                /* already bound this frame */
            { static int nocache = -1;            /* TEX_NOCACHE=1: always re-upload */
              if (nocache < 0) { const char* e = getenv("TEX_NOCACHE");
                                 nocache = e ? atoi(e) : 0; }
              if (nocache) { slot = i; break; } }
            /* Held from an earlier frame: re-upload only if the guest bytes
             * changed. Most of this scene's textures are static, and converting
             * every one of them every frame was ~70% of the frame's CPU time. */
            u32 nb = dxt ? (blkrow * blkrows) : (w * h * bpp);
            u32 cs = TEX_CSUM(vm_base + c->off, nb);
            if (cs == c->csum) { c->used = 1; return i; }
            slot = i; break;                      /* stale: fall through and redo */
        }
        if (!c->used && freeslot < 0) freeslot = i;
    }
    /* TEX_OFF_BIAS=1: sample the image one whole level-0 BELOW the bound offset.
     * Measured on the Rubber Ducky demo: at the bind offset every texture reads
     * as all-zero, while [off - w*h*bpp, off) holds the image (3800-4160 of 4298
     * sampled bytes non-zero, for four different textures at three sizes). So
     * the offset the guest programs points PAST level 0 rather than at it. */
    /* TEX_OFF_BIAS: 1 = one level-0 below the bound offset, 2 = one full MIP
     * CHAIN above it. The measured upload-to-bind deltas on the Rubber Ducky
     * demo are 0x2AAB00 for a 2MB level 0 and 0x555580 for a 4MB one -- both
     * exactly the mipmap pyramid total (L0 * 4/3), which is what the guest
     * reserves per texture. */
    { static int bias = -1;
      if (bias < 0) { const char* e = getenv("TEX_OFF_BIAS"); bias = e ? atoi(e) : 0; }
      if (bias == 1) { u32 sz = w * h * bpp; if (off > sz) off -= sz; }
      else if (bias >= 2) {
          u32 chain = 0;
          for (u32 mw = w, mh = h; mw && mh; mw >>= 1, mh >>= 1) {
              chain += mw * mh * bpp;
              if (mw == 1 && mh == 1) break;
          }
          /* bias 3: only shift when the bound offset really is empty, so a
           * texture that IS bound correctly keeps its own data. Applying the
           * chain unconditionally fixes the mis-bound majority but breaks any
           * correctly-bound minority, which shows up as objects going black. */
          int shift = 1;
          if (bias >= 3) {
              u32 nz = 0, sz = w * h * bpp;
              for (u32 i = 0; i < sz && i < 0x20000u; i += 97) if (vm_base[off + i]) nz++;
              shift = (nz == 0);
          }
          if (shift) off += chain;
      } }
    /* TEX_REMAP=1: if the bound offset holds nothing, look the image up by size
     * in the VRAM upload registry (cellGcmSys). For a title whose upload address
     * and SET_TEXTURE_OFFSET disagree this puts the real bytes under the sampler
     * without guessing a delta. */
    { static int remap = -1;
      if (remap < 0) { const char* e = getenv("TEX_REMAP"); remap = e ? atoi(e) : 0; }
      if (remap) {
        u32 nz = 0, sz = w * h * bpp;
        for (u32 i = 0; i < sz && i < 0x20000u; i += 97) if (vm_base[off + i]) nz++;
        if (!nz) {
            extern u32 rsx_find_vram_upload(u32);
            u32 alt = rsx_find_vram_upload(sz);
            if (alt) {
                static int _n = 0;
                if (_n++ < 8) fprintf(stderr, "[TEXREMAP] 0x%08X (%ux%u, %u bytes) -> 0x%08X%c",
                                      off, w, h, sz, alt, 10);
                off = alt;
            }
        } } }
    /* A stale cache entry (slot >= 0) re-uses its own slot; otherwise take a
     * free one. Without this the stale path fell through to freeslot and either
     * duplicated the texture into a second slot or bailed when none was free. */
    if (slot < 0 && freeslot < 0) return -1;      /* out of slots this frame */
    /* TEX_SRCDBG=<N>: is the guest memory this slot uploads FROM actually
     * populated? An empty source and a broken sampler both render flat. */
    { static int cap = -1, n = 0;
      if (cap < 0) { const char* e = getenv("TEX_SRCDBG"); cap = e ? atoi(e) : 0; }
      if (cap && n < cap) { n++;
        u32 nz = 0, tot = 0;
        for (u32 i = 0; i < w * h * bpp && i < 0x40000u; i += 61) { tot++; if (vm_base[off + i]) nz++; }
        /* Several of this title's textures are bound at EXACTLY the end address
         * of an upload, so also sample the block immediately BELOW the bind
         * offset: if that holds the image, the bind is one whole texture high. */
        u32 nzb = 0, totb = 0;
        { u32 sz = w * h * bpp; u32 base = (off > sz) ? off - sz : 0;
          for (u32 i = 0; i < sz && i < 0x40000u; i += 61) { totb++; if (vm_base[base + i]) nzb++; } }
        long nearest = 0; u32 found = 0;
        if (!nz) {   /* empty: where IS the data? scan out from the bind offset */
            for (long d = 0x1000; d <= 0x1000000 && !found; d += 0x1000) {
                for (int s = 0; s < 2 && !found; s++) {
                    long a = (long)off + (s ? -d : d);
                    if (a < 0x1000) continue;
                    u32 c = 0;
                    for (u32 i = 0; i < 0x1000u; i += 61) if (vm_base[a + i]) c++;
                    if (c > 8) { nearest = (s ? -d : d); found = c; }
                }
            }
        }
        fprintf(stderr, "[TEXSRC] off=0x%08X %ux%u fmt=0x%02X bpp=%u nonzero=%u/%u"
                        "%s%+ld (page nonzero=%u)\n",
                off, w, h, fmt, bpp, nz, tot,
                nz ? "" : "  nearest data at ", nz ? 0L : nearest, found);
        fprintf(stderr, "[TEXSRC]    block BELOW bind (off-%u): nonzero=%u/%u%c",
                w * h * bpp, nzb, totb, 10);
        /* Walk DOWN in 4KB steps to the first populated page whose predecessor
         * is empty: that is where this texture's data actually begins, and the
         * delta from the bound offset is the rule we are missing. */
        { u32 step = 0x1000, prev_pop = 0; long start_delta = 0;
          for (long d = 0; d >= -(long)(w * h * bpp) * 2; d -= step) {
              u32 a = (u32)((long)off + d); u32 pop = 0;
              if (a < 0x1000) break;
              for (u32 i = 0; i < step; i += 53) if (vm_base[a + i]) pop++;
              if (!pop && prev_pop) { start_delta = d + step; break; }
              prev_pop = pop;
          }
          fprintf(stderr, "[TEXSRC]    data starts at off%+ld (level0=%u bytes)%c",
                  start_delta, w * h * bpp, 10); } } }
    if (slot < 0) slot = freeslot;
    VPTexSlot* t = &s_d3d.vp_tex[slot];
    u32 pitch = ((dxt ? blkrow : w * bpp) + 255) & ~255u;
    int fresh = 0;

    /* Also recreate when the CUBE-ness changes: a slot holding a 2D texture of
     * the same dimensions would otherwise be reused with 1 array slice while the
     * cube path copies 6 subresources and writes a 6x upload buffer -- which
     * crashes. */
    if (t->res && (t->w != w || t->h != h || t->fmt != fmt || t->cube != cube)) {
        t->res->lpVtbl->Release(t->res); t->res = NULL;
        if (t->up) { t->up->lpVtbl->Release(t->up); t->up = NULL; }
    }
    if (!t->res) {
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {0};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h;
        td.DepthOrArraySize = cube ? 6 : 1;   /* cube = 6 array slices */
        td.MipLevels = 1;
        td.Format = dxfmt; td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
                s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                &IID_ID3D12Resource, (void**)&t->res)))
            return -1;
        D3D12_HEAP_PROPERTIES hu = {0}; hu.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {0};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = (u64)pitch * (dxt ? blkrows : h) * (cube ? 6 : 1);
        bd.Height = 1; bd.DepthOrArraySize = 1;
        bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
                s_d3d.device, &hu, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&t->up))) {
            t->res->lpVtbl->Release(t->res); t->res = NULL; return -1;
        }
        fresh = 1;
    }

    void* mapped = NULL; D3D12_RANGE nr = {0,0};
    if (FAILED(t->up->lpVtbl->Map(t->up, 0, &nr, &mapped)) || !mapped) return -1;
    { static int _tp=0; if (getenv("RTT_DUMP") && _tp++ < 6) {
        const u8* sp = vm_base + off;
        fprintf(stderr, "[TEXUP] off=0x%X fmt=0x%X row0:", off, fmt);
        for (int _b=0;_b<8;_b++) fprintf(stderr, " %02X", sp[_b]);
        fprintf(stderr, "  row240:");
        for (int _b=0;_b<8;_b++) fprintf(stderr, " %02X", sp[240*w*bpp+_b]);
        fprintf(stderr, "  mid:");
        for (int _b=0;_b<8;_b++) fprintf(stderr, " %02X", sp[240*w*bpp+320*bpp+_b]);
        fprintf(stderr, "%s", "\n");
    } }
    /* Swizzled (LN bit 0x20 clear): texels live in Morton/Z-order, not rows.
     * Uploading them as linear rows produced the diagonal-stripe garbage on
     * LBP's loading screen (every texture there is 0x85 swizzled). POT dims
     * only -- the hardware requires that for swizzled textures anyway. */
    int swz = tl.swizzled;
    /* Cube textures store their 6 faces consecutively. Convert each into its own
     * slice of the upload buffer; the conversion below is unchanged and simply
     * runs once per face with off/mapped rebased. */
    const u32 _face_rows  = dxt ? blkrows : h;
    /* The face stride is NOT one mip-0 image: RSX stores each cube face as its
     * own complete mip pyramid, so face f starts a whole pyramid (128-byte
     * aligned) in. Assuming mip-0-sized strides made face 1 land inside face 0's
     * mip chain -- which is exactly what the dumps showed, every face after the
     * first a progressively smaller copy of the first. */
    u32 _face_bytes = dxt ? (blkrow * blkrows) : (w * h * bpp);
    if (cube) {
        u32 total = 0;
        for (u32 lw = w, lh = h, l = 0; l < (mips ? mips : 1u); l++) {
            total += dxt ? (((lw + 3) / 4) * (bpp == 8 ? 16u : 8u) * ((lh + 3) / 4))
                         : (lw * lh * bpp);
            if (lw > 1) lw >>= 1;
            if (lh > 1) lh >>= 1;
        }
        _face_bytes = (total + 127u) & ~127u;
    }
    const u32 _nfaces     = cube ? 6u : 1u;
    const u32 _off0 = off; u8* const _map0 = (u8*)mapped;
    for (u32 _f = 0; _f < _nfaces; _f++) {
    off    = _off0 + _f * _face_bytes;
    if (cube && getenv("CUBEDBG")) { u32 _s=0; const u8* _p = vm_base + off;
        for (u32 _i = 0; _i < _face_bytes; _i += 97) _s = _s*131 + _p[_i];
        fprintf(stderr, "[CUBEFACE] f=%u off=0x%X bytes=%u bpp=%u swz=%d csum=%08X%c",
                _f, off, _face_bytes, bpp, swz, _s, 10); }
    mapped = _map0 + (u64)_f * pitch * _face_rows;
    /* Pixel conversion -- deswizzle, channel order, block copy -- is RSX
     * semantics and lives in rsx_texture_layout.c, so a second backend gets
     * it by calling this rather than porting the loops again. */
    rsx_texture_decode(mapped, pitch, vm_base + off, w, h, &tl,
                       rsx_texture_argb_is_rgba());

    /* The diagnostics below only apply to the linear 8-bit case (Bink video
     * planes); they used to sit inside that branch of the conversion. */
    if (!tl.compressed && tl.bytes_per_texel == 1 && !tl.swizzled) {
        /* MOVIE_FIND=1: when a 640x360 movie plane uploads ZERO, scan a wide
         * guest window for the REAL frame (a 640-wide region with content) so
         * we learn where the video decode actually wrote vs where the texture
         * points -- ends the frame-buffer-address guessing. */
        if (getenv("MOVIE_FIND") && w == 640 && h == 360) {
            extern uint8_t* vm_base;
            const u8* here = vm_base + off;
            u32 hz = 0; for (u32 i=0;i<w*h;i+=137) if (here[i]) { hz=1; break; }
            static int _mf = 0;
            if (!hz && _mf++ < 3) {
                fprintf(stderr, "[movie-find] bound plane ea=0x%X is ZERO; full scan...\n", off);
                /* Scan FULL VRAM + main heap in 0x8000 steps for a 640x360
                 * content block -- NO early cap (the previous found<6 stopped in
                 * the low-VRAM display buffers before ever reaching the movie
                 * region ~0x40E80000). Report the near-plane hits explicitly so
                 * ring-desync (frame present at a DIFFERENT slot) is separable
                 * from decode-never-ran (region entirely zero). */
                struct { u32 lo, hi; const char* tag; } rng[] = {
                    {0x40000000u, 0x41000000u, "VRAM"}, {0x00100000u, 0x10000000u, "MAIN"},
                    {0x48000000u, 0x4A000000u, "BINK"} };  /* Bink working set (handle ~0x4849E760, SPU I/O 0x4847/0x4945) */
                u32 plane_lo = (off > 0x400000u) ? off - 0x400000u : 0;
                u32 plane_hi = off + 0x400000u;
                int found = 0, nearc = 0;
                for (int r = 0; r < (int)(sizeof(rng)/sizeof(rng[0])); r++) {
                    u32 best_a = 0, best_nz = 0; u32 best_span = 0;
                    for (u32 a = rng[r].lo; a + w*h < rng[r].hi; a += 0x8000) {
                        const u8* p = vm_base + a;
                        u32 nz = 0; u8 mn2 = 255, mx2 = 0;
                        for (u32 i = 0; i < w*h; i += 257) { u8 v = p[i]; if (v) nz++; if (v<mn2) mn2=v; if (v>mx2) mx2=v; }
                        u32 span = mx2 - mn2;
                        if (nz > best_nz) { best_nz = nz; best_a = a; best_span = span; }
                        if (nz > 400 && span > 60) {  /* looks like image content */
                            int isnear = (a >= plane_lo && a <= plane_hi);
                            if (isnear) nearc++;
                            if (found < 40)
                                fprintf(stderr, "[movie-find]   CONTENT @0x%08X (%s) nz=%u span=%u%s\n",
                                        a, rng[r].tag, nz, span, isnear ? " <== NEAR bound plane" : "");
                            found++;
                        }
                    }
                    /* Always report the densest block in this range even below the
                     * content threshold -- the MM intro fades in from black, so the
                     * real first frames are low-span and would otherwise be invisible. */
                    fprintf(stderr, "[movie-find]   %s best @0x%08X nz=%u span=%u%s\n",
                            rng[r].tag, best_a, best_nz, best_span,
                            (best_a >= plane_lo && best_a <= plane_hi) ? " <== NEAR bound plane" : "");
                    /* Dump the densest block as a 640x360 grayscale BMP so the
                     * "is this the decoded movie frame" question is answered by
                     * eye. One dump per range per process. */
                    if (best_nz > 400 && best_span > 40) {
                        static int _bd[4] = {0,0,0,0};
                        if (r < 4 && !_bd[r]) { _bd[r] = 1;
                            const u8* sp = vm_base + best_a;
                            char pn[128];
                            snprintf(pn, sizeof(pn), "binkscan_%s_%08X.bmp", rng[r].tag, best_a);
                            FILE* f = fopen(pn, "wb");
                            if (f) {
                                u32 rowb = w*3, rowp = (rowb+3)&~3u, fsz = 54 + rowp*h;
                                u8 hd[54] = {'B','M'};
                                hd[2]=(u8)fsz; hd[3]=(u8)(fsz>>8); hd[4]=(u8)(fsz>>16); hd[5]=(u8)(fsz>>24);
                                hd[10]=54; hd[14]=40;
                                hd[18]=(u8)w; hd[19]=(u8)(w>>8); hd[22]=(u8)h; hd[23]=(u8)(h>>8);
                                hd[26]=1; hd[28]=24;
                                fwrite(hd,1,54,f);
                                for (int y=(int)h-1; y>=0; y--) {
                                    const u8* srow = sp + (u64)y*w;
                                    for (u32 x=0;x<w;x++){ u8 px[3]; px[0]=px[1]=px[2]=srow[x]; fwrite(px,1,3,f); }
                                    { u8 z[3]={0,0,0}; fwrite(z,1,rowp-rowb,f); }
                                }
                                fclose(f);
                                fprintf(stderr, "[movie-find]   dumped %s\n", pn);
                            }
                        }
                    }
                }
                fprintf(stderr, "[movie-find]   total=%d near-plane=%d (window 0x%X..0x%X)\n",
                        found, nearc, plane_lo, plane_hi);
                if (!found) fprintf(stderr, "[movie-find]   no 640x360 content block (>thresh) found anywhere\n");
            }
        }
        /* TEX_SAVE=1: also dump wide B8 uploads (Bink video planes are B8 --
         * the Y plane IS the movie frame in grayscale) + a content stat, so
         * "is the decoder producing pixels" is answerable by looking at a BMP. */
        if (getenv("TEX_SAVE") && w >= 256) {
            static int _bs = 0;
            const u8* sp = vm_base + off;
            u32 nz = 0; u8 mn = 255, mx = 0;
            for (u32 i = 0; i < w * h; i += 17) {
                u8 v = sp[i]; if (v) nz++; if (v < mn) mn = v; if (v > mx) mx = v;
            }
            fprintf(stderr, "[TEXB8] off=0x%X %ux%u nz=%u/%u min=%u max=%u\n",
                    off, w, h, nz, (w * h) / 17, mn, mx);
            if (_bs < 8 && mx > mn) { _bs++;
                char pn[128];
                snprintf(pn, sizeof(pn), "texb8_%08X_%ux%u_%d.bmp", off, w, h, _bs);
                FILE* f = fopen(pn, "wb");
                if (f) {
                    u32 rowb = w * 3, rowp = (rowb + 3) & ~3u;
                    u32 fsz = 54 + rowp * h;
                    u8 hd[54] = {'B','M'};
                    hd[2]=(u8)fsz; hd[3]=(u8)(fsz>>8); hd[4]=(u8)(fsz>>16); hd[5]=(u8)(fsz>>24);
                    hd[10]=54; hd[14]=40;
                    hd[18]=(u8)w; hd[19]=(u8)(w>>8); hd[22]=(u8)h; hd[23]=(u8)(h>>8);
                    hd[26]=1; hd[28]=24;
                    fwrite(hd, 1, 54, f);
                    for (int y = (int)h - 1; y >= 0; y--) {
                        const u8* srow = sp + (u64)y * w;
                        for (u32 x = 0; x < w; x++) {
                            u8 px[3]; px[0]=px[1]=px[2]=srow[x];
                            fwrite(px, 1, 3, f);
                        }
                        { u8 z[3] = {0,0,0}; fwrite(z, 1, rowp - rowb, f); }
                    }
                    fclose(f);
                    fprintf(stderr, "[TEX_SAVE] %s\n", pn);
                }
            }
        }
    }
    /* TEX_SAVE=1: dump the first few converted ARGB uploads as BMPs (rgb +
     * alpha channel separately) -- ground-truth for "is the guest texture
     * wrong or is the sampling wrong" questions (wave's hue palette). */
    if (argb && _f + 1 == _nfaces && getenv("TEX_SAVE")) { static int _ts = 0; if (_ts < 8) { _ts++;
        /* For a cube, write every face: the point of the dump is to check the
         * assumed 6-face layout against the game's cubemap art. */
        for (u32 _face = 0; _face < _nfaces; _face++)
        for (int pass = 0; pass < 2; pass++) {
            char pn[128];
            if (_nfaces > 1)
                snprintf(pn, sizeof(pn), "cube_%08X_%ux%u_f%u_%s.bmp", _off0, w, h,
                         _face, pass ? "a" : "rgb");
            else
                snprintf(pn, sizeof(pn), "tex_%08X_%ux%u_%s.bmp", off, w, h,
                     pass ? "a" : "rgb");
            FILE* f = fopen(pn, "wb");
            if (f) {
                u32 rowb = w * 3, rowp = (rowb + 3) & ~3u;
                u32 fsz = 54 + rowp * h;
                u8 hd[54] = {'B','M'};
                hd[2]=(u8)fsz; hd[3]=(u8)(fsz>>8); hd[4]=(u8)(fsz>>16); hd[5]=(u8)(fsz>>24);
                hd[10]=54; hd[14]=40;
                hd[18]=(u8)w; hd[19]=(u8)(w>>8); hd[22]=(u8)h; hd[23]=(u8)(h>>8);
                hd[26]=1; hd[28]=24;
                fwrite(hd, 1, 54, f);
                for (int y = (int)h - 1; y >= 0; y--) {
                    const u8* srow = _map0 + (u64)_face * pitch * _face_rows
                                     + (u64)y * pitch;
                    for (u32 x = 0; x < w; x++) {
                        u8 px[3];
                        if (pass) { px[0]=px[1]=px[2]=srow[x*4+3]; }
                        else { px[0]=srow[x*4+2]; px[1]=srow[x*4+1]; px[2]=srow[x*4+0]; }
                        fwrite(px, 1, 3, f);
                    }
                    { u8 z[3] = {0,0,0}; fwrite(z, 1, rowp - rowb, f); }
                }
                fclose(f);
                fprintf(stderr, "[TEX_SAVE] %s\n", pn);
            }
        }
    } }
    /* TEX_AVGDBG=1: one line per distinct bound texture offset with its average
     * source colour. duck.tga is overwhelmingly yellow (avg ~243,191,23), so this
     * says whether the duck's image is bound at all -- and if it is, the duck is
     * being drawn somewhere and the question is where, not whether. */
    { static int avgd = -1;
      if (avgd < 0) { const char* e = getenv("TEX_AVGDBG"); avgd = e ? atoi(e) : 0; }
      if (avgd && !dxt && bpp == 4) {
        static u32 seen[64]; static int nseen = 0;
        int known = 0;
        for (int i2 = 0; i2 < nseen; i2++) if (seen[i2] == off) { known = 1; break; }
        if (!known && nseen < 64) {
            seen[nseen++] = off;
            /* Report the source as R,G,B,A -- the layout TEX_RGBA uploads (PSGL
             * writes GL_RGBA/UNSIGNED_INT_8_8_8_8, so bytes land R,G,B,A). Alpha
             * matters as much as colour here: a blended surface whose alpha is a
             * flat 255 renders opaque no matter how correct the blend state is. */
            u64 sr = 0, sg = 0, sb = 0, sa = 0; u32 cnt = 0, amin = 255, amax = 0;
            for (u32 i2 = 0; i2 + 3 < w * h * 4u; i2 += 4 * 37) {
                const u8* q = vm_base + off + i2;
                sr += q[0]; sg += q[1]; sb += q[2]; sa += q[3]; cnt++;
                if (q[3] < amin) amin = q[3];
                if (q[3] > amax) amax = q[3];
            }
            if (cnt) fprintf(stderr, "[TEXAVG] raw=0x%08X %4ux%-4u fmt=0x%02X"
                                     " avgRGBA=(%3u,%3u,%3u,%3u) alpha[%u..%u]%c",
                             key_off, w, h, fmt, (u32)(sr/cnt), (u32)(sg/cnt),
                             (u32)(sb/cnt), (u32)(sa/cnt), amin, amax, 10);
        }
      } }
    /* Content-identify the duck's texture so filters can say "duck". */
    if (!dxt && bpp == 4 && w == 512 && h == 512 && s_duck_raw != key_off) {
        u64 ar = 0, ag = 0, ab = 0; u32 n = 0;
        for (u32 i2 = 0; i2 + 3 < w * h * 4u; i2 += 4 * 211) {
            const u8* q = vm_base + off + i2;
            ar += q[0]; ag += q[1]; ab += q[2]; n++;
        }
        if (n && ar/n > 200 && ag/n > 140 && ab/n < 90) {
            s_duck_off = key_off;
            { static u32 last = 0; if (last != key_off) { last = key_off;
                fprintf(stderr, "[DUCKTEX] resolved=0x%08X identified by content%c", key_off, 10); } }
        }
    }
    /* TEX_HILITE=1: paint the duck's texture pure red. duck.tga is overwhelmingly
     * yellow (avg 243,191,23); with the R,G,B,A source order that is bytes
     * s[0] high, s[1] mid, s[2] low. Flooding it with an unmistakable colour
     * answers "where on screen is the duck drawn" directly, which neither the
     * vertex data nor a pixel count can. Diagnostic only. */
    { static int hil = -1;
      if (hil < 0) { const char* e = getenv("TEX_HILITE"); hil = e ? atoi(e) : 0; }
      if (hil && !dxt && bpp == 4 && w == 512 && h == 512) {
        u64 sr = 0, sg = 0, sb = 0; u32 cnt = 0;
        for (u32 i2 = 0; i2 + 3 < w * h * 4u; i2 += 4 * 37) {
            const u8* q = vm_base + off + i2;
            sr += q[0]; sg += q[1]; sb += q[2]; cnt++;
        }
        if (cnt) {
            u32 ar = (u32)(sr/cnt), ag = (u32)(sg/cnt), ab = (u32)(sb/cnt);
            if (ar > 200 && ag > 140 && ab < 90) {
                for (u32 y = 0; y < h; y++) {
                    u8* drow = (u8*)mapped + (u64)y * pitch;
                    for (u32 x = 0; x < w; x++) {
                        drow[x*4+0] = 0xFF; drow[x*4+1] = 0x00;
                        drow[x*4+2] = 0x00; drow[x*4+3] = 0xFF;
                    }
                }
                { static int _n = 0; if (_n++ < 4)
                    fprintf(stderr, "[TEXHILITE] duck texture off=0x%08X avg=(%u,%u,%u) -> red%c",
                            off, ar, ag, ab, 10); }
            }
        }
      } }
    /* TEX_MARKEMPTY=1: paint any texture whose guest source is entirely zero a
     * flat magenta instead of uploading the empty bytes. An unresolved texture
     * and a legitimately black surface are indistinguishable on screen, so this
     * says WHICH geometry is missing its image -- diagnostic only. */
    { static int mark = -1;
      if (mark < 0) { const char* e = getenv("TEX_MARKEMPTY"); mark = e ? atoi(e) : 0; }
      if (mark && !dxt) {
        u32 nz = 0, sz = w * h * bpp;
        for (u32 i2 = 0; i2 < sz && i2 < 0x40000u; i2 += 61) if (vm_base[off + i2]) nz++;
        if (!nz) {
            for (u32 y = 0; y < h; y++) {
                u8* drow = (u8*)mapped + (u64)y * pitch;
                for (u32 x = 0; x < w; x++) {
                    /* Pure RED. Magenta collided with this title's wall tint,
                     * so the marker was indistinguishable from real geometry. */
                    drow[x*4+0] = 0xFF; drow[x*4+1] = 0x00;
                    drow[x*4+2] = 0x00; drow[x*4+3] = 0xFF;
                }
            }
            { static int _n = 0; if (_n++ < 8)
                fprintf(stderr, "[TEXEMPTY] off=0x%08X %ux%u fmt=0x%02X -> magenta%c",
                        off, w, h, fmt, 10); }
        }
      } }
    }   /* end per-face conversion */
    off = _off0; mapped = _map0;
    t->up->lpVtbl->Unmap(t->up, 0, NULL);

    if (!fresh) {   /* reused resource: PSR -> COPY_DEST first */
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = t->res;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }
    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    dst.pResource = t->res; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = t->up;  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format   = dxfmt;
    src.PlacedFootprint.Footprint.Width    = w;
    src.PlacedFootprint.Footprint.Height   = h;
    src.PlacedFootprint.Footprint.Depth    = 1;
    src.PlacedFootprint.Footprint.RowPitch = pitch;
    for (u32 _f = 0; _f < _nfaces; _f++) {
        dst.SubresourceIndex        = _f;      /* cube face = array slice */
        src.PlacedFootprint.Offset  = (u64)_f * pitch * _face_rows;
        s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);
    }
    {
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = t->res;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }
    /* SRV at heap slot 1+slot */
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = dxfmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    /* RSX B8 replicates the byte into all four channels (dbgfont's FP reads
     * coverage from .w); DXGI R8 defaults to (r,0,0,1), so swizzle (R,R,R,R)
     * = encoded 0x1000 (component 0 in all lanes + always-set bit). ARGB8
     * keeps the identity mapping. */
    sv.Shader4ComponentMapping = (basef == 0x81) ? 0x1000 : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE sh;
    s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &sh);
    sh.ptr += (u64)(1 + slot) * s_d3d.srv_inc;
    if (cube) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube.MipLevels = 1;
        sv.TextureCube.MostDetailedMip = 0;
        sv.TextureCube.ResourceMinLODClamp = 0.0f;
    }
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, t->res, &sv, sh);

    t->off = off; t->key = key_off; t->w = w; t->h = h; t->fmt = fmt;
    t->cube = cube; t->used = 1;
    { u32 nb = dxt ? (blkrow * blkrows) : (w * h * bpp);
      t->csum = TEX_CSUM(vm_base + off, nb); }
    #undef TEX_CSUM
    return slot;
}

/* ---------------------------------------------------------------------------
 * Render-to-texture: offscreen RT pool.
 *
 * Draws/clears targeting a non-display surface (SET_SURFACE_COLOR_xOFFSET
 * not registered via cellGcmSetDisplayBuffer) render into a pooled RGBA8
 * texture keyed by the surface's resolved vm offset; a later draw binding a
 * texture at that offset samples the RT directly instead of guest memory.
 * RGBA8 for every RT (even half-float guest surfaces) keeps the existing
 * PSO/RTV formats -- values clamp to [0,1], which demosaic's RGB data fits.
 * -----------------------------------------------------------------------*/

/* Snapshot the VP constant bank + viewport epilogue for one draw into its
 * vp_cb slot. Runs at record time so every draw keeps the constants that
 * were live when the guest issued it. */
static void vp_record_cb(u32 slot, int vs_idx, const D3D12DrawRecord* dr)
{
    const rsx_state* st = s_d3d.current_rsx_state;
    if (!s_d3d.vp_cb_mapped || !st || slot >= MAX_DRAWS) return;
    /* FP texcoord scale (b1): 1/size for UNnormalized textures (fmt bit
     * 0x40 -- wave samples everything in texel space), 1.0 otherwise. */
    if (s_d3d.vp_fpcb_mapped) {
        float* ts = (float*)((char*)s_d3d.vp_fpcb_mapped
            + ((u64)s_d3d.vp_parity * MAX_DRAWS + slot) * VP_FPCB_STRIDE);
        for (int _u = 0; _u < 4; _u++) {
            float sx = 1.0f, sy = 1.0f;
            if (dr && dr->tex[_u].set && (dr->tex[_u].fmt & 0x40) &&
                dr->tex[_u].w && dr->tex[_u].h) {
                sx = 1.0f / (float)dr->tex[_u].w;
                sy = 1.0f / (float)dr->tex[_u].h;
            }
            ts[_u*4+0] = sx; ts[_u*4+1] = sy; ts[_u*4+2] = 0.0f; ts[_u*4+3] = 0.0f;
        }
        /* rsx_alphatest (b1[4]): x=enable, y=ref/255, z=func-0x200. D3D12
         * has no fixed alpha test; guest FPs discard on it dynamically
         * (wave's colour wheel is a disc via alpha test -- without it the
         * palette drew as an opaque square). */
        if (dr && !getenv("NO_ALPHATEST")) {
            { static int _at = 0;
              if (((dr->alpha_ctl >> 16) & 1u) && _at++ < 4)
                  printf("[ALPHATEST] enable func=%u ref=%u\n",
                         (dr->alpha_ctl >> 8) & 0xFFu, dr->alpha_ctl & 0xFFu); }
            ts[16] = (float)((dr->alpha_ctl >> 16) & 1u);
            ts[17] = (float)(dr->alpha_ctl & 0xFFu) / 255.0f;
            ts[18] = (float)((dr->alpha_ctl >> 8) & 0x07u);
            ts[19] = 0.0f;
        } else {
            ts[16] = 0.0f; ts[17] = 0.0f; ts[18] = 7.0f; ts[19] = 0.0f;
        }

        /* fp_k[]: the program's inline fragment constants, re-read from the
         * guest ucode every draw. The decompiler emits fp_k[i] lookups instead
         * of baking these in as literals, so the compiled shader no longer
         * changes when the title re-patches them -- which is what forced the
         * pipeline cache to key on a hash of the ucode bytes. */
        if (dr && dr->fp_addr) {
            extern uint8_t* vm_base;
            extern u32 cellGcmResolveLocated(int, u32);
            u32 foff = cellGcmResolveLocated((dr->fp_addr & 0x3u) == 1,
                                             dr->fp_addr & ~0x3u);
            if (vm_base && foff != 0xFFFFFFFFu)
                rsx_fp_extract_consts(vm_base + foff, 4096, &ts[20], FP_MAX_CONSTS);
        }
    }
    char* dst = (char*)s_d3d.vp_cb_mapped
        + ((u64)s_d3d.vp_parity * MAX_DRAWS + slot) * VP_CB_STRIDE;
    memcpy(dst, st->vertex_constants, RSX_MAX_VERTEX_CONSTANTS * 16);
    /* VP_MVP=<N>: the constant bank as the shader will see it, for the first N
     * draws. A snapshot taken from a stale rsx_state looks exactly like a broken
     * vertex program from the outside -- both give zero fragments. */
    { static int cap = -1, seen = 0;
      if (cap < 0) { const char* e = getenv("VP_MVP"); cap = e ? atoi(e) : 0; }
      if (cap && seen < cap) { seen++;
        const float* c = (const float*)dst;
        int last_nz = -1;
        for (int i = 0; i < RSX_MAX_VERTEX_CONSTANTS * 4; i++)
            if (c[i] > 1e-9f || c[i] < -1e-9f) last_nz = i / 4;
        fprintf(stderr, "[VPMVP] slot=%u lastNZ=c%d  c260=(%g %g %g %g) c261=(%g %g %g %g)\n",
                slot, last_nz,
                c[260*4+0], c[260*4+1], c[260*4+2], c[260*4+3],
                c[261*4+0], c[261*4+1], c[261*4+2], c[261*4+3]); } }
    /* DUCK_CB=<hex tex0 offset>: the constants as they land in the per-draw
     * constant buffer for that texture's draws -- i.e. exactly what the shader
     * samples, not what the CPU-side rsx_state holds. Those two agreeing is an
     * assumption worth checking directly when a draw transforms to nothing. */
    const float* vpx_dbg = (const float*)(dst + RSX_MAX_VERTEX_CONSTANTS * 16);
    { static const char* dc = (const char*)1; static u32 wantc = 0; static int n = 0;
      if (dc == (const char*)1) { dc = getenv("DUCK_CB");
                                  wantc = dc ? (u32)strtoul(dc, NULL, 16) : 0; }
      if (wantc && dr && dr->tex[0].raw == wantc && n < 4) { n++;
          const float* c = (const float*)dst;
          fprintf(stderr, "[DUCKCB] slot=%u vs_idx=%d posscale=(%g %g %g) posoffset=(%g %g %g)%c",
                  slot, vs_idx, vpx_dbg[0], vpx_dbg[1], vpx_dbg[2],
                  vpx_dbg[4], vpx_dbg[5], vpx_dbg[6], 10);
          for (int r = 256; r <= 259; r++)
              fprintf(stderr, "[DUCKCB]   c%d=(%g %g %g %g)%c", r,
                      c[r*4+0], c[r*4+1], c[r*4+2], c[r*4+3], 10);
      } }

    /* Viewport epilogue (see the render_frame notes this logic came from):
     * x/y identity, z lane remaps GL clip z when the guest programs one. */
    float* vpx = (float*)(dst + RSX_MAX_VERTEX_CONSTANTS * 16);
    const float* vs_ = st->viewport_scale;
    const float* vo_ = st->viewport_offset;
    vpx[0] = vpx[1] = vpx[3] = 1.0f;
    vpx[4] = vpx[5] = vpx[7] = 0.0f;
    if (vs_[2] != 0.0f) { vpx[2] = vs_[2]; vpx[6] = vo_[2]; }
    else                { vpx[2] = 1.0f;   vpx[6] = 0.0f;   }
    /* DBG_ZOOM=<k> with DBG_CENTER="cx,cy": a debug camera applied in CLIP space,
     * not a framebuffer crop -- the geometry is re-rasterized at full resolution.
     * The shader epilogue already computes
     *     pos.xyz = _p.xyz * vp_posscale + _p.w * vp_posoffset
     * so recentring NDC point c and magnifying by k is exactly
     *     pos.xy = _p.xy * k + _p.w * (-c * k).
     * Rubber Ducky frames its ducks near the bottom edge of the viewport and its
     * camera only moves from the analog sticks, so without this there is no way
     * to look at the subject of the demo. It changes the view, nothing else:
     * same draws, same transform, same textures. */
    { static int z = -1; static float k = 1.0f, cx = 0.0f, cy = 0.0f;
      if (z < 0) { const char* e = getenv("DBG_ZOOM");
                   k = e ? (float)atof(e) : 1.0f;
                   z = (e && k > 0.0f) ? 1 : 0;
                   const char* ce = getenv("DBG_CENTER");
                   if (ce) { double a = 0, b = 0; sscanf(ce, "%lf,%lf", &a, &b);
                             cx = (float)a; cy = (float)b; } }
      if (z) { float ux = cx, uy = cy;
               static int lock = -1;
               if (lock < 0) { const char* le = getenv("DBG_LOCK"); lock = le ? atoi(le) : 0; }
               if (lock && s_lock_valid) { ux = s_lock_x; uy = s_lock_y; }
               vpx[0] = k; vpx[1] = k; vpx[4] = -ux * k; vpx[5] = -uy * k; } }

    /* Garbage-projection fallback (vkcube; see the original comment). */
    int uses_c03 = (vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n)
                       ? s_d3d.vp_vs[vs_idx].uses_c03 : s_d3d.vp_uses_c03;
    float* c = (float*)dst;
    float c00 = c[0];
    int garbage = !(c00 > 0.0f && c00 < 8.0f);
    if (getenv("VP_NOFIXPROJ")) garbage = 0;
    if ((garbage && uses_c03) || getenv("VP_FIXPROJ")) {
        { static int _fb = 0; if (_fb++ < 6)
            printf("[VPFB] fallback proj on draw slot %u (c00=%g vs_idx=%d)\n",
                   slot, c00, vs_idx); }
        for (int _i = 0; _i < 16; _i++) c[_i] = 0.0f;
        c[0]=1.358f; c[5]=2.414f; c[11]=1.0f;
        c[10]=1.0f/99.0f; c[14]=-1.0f/99.0f;
        vpx[2] = 1.0f; vpx[6] = 0.0f;
    }
}

/* Which offscreen surface (if any) does the current RSX state render to?
 * Returns 0 for a display buffer (backbuffer), else the surface's RAW RSX
 * offset. RTs are keyed by raw offset -- a texture bound at the same raw
 * offset is the same buffer (surface and texture registers share the offset
 * space; location bits differ but one title doesn't alias local vs main at
 * one offset), which sidesteps guessing the surface's context DMA location. */
static u32 current_rt_off(u32* out_w, u32* out_h, u32 out_mrt[3])
{
    extern int cellGcmOffsetIsDisplay(u32 offset);
    const rsx_state* st = s_d3d.current_rsx_state;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (out_mrt) out_mrt[0] = out_mrt[1] = out_mrt[2] = 0;
    if (!st) return 0;
    /* SET_SURFACE_COLOR_TARGET: 1 = A, 2 = B, 0x13 = MRT1 (A+B),
     * 0x17 = MRT2 (A+B+C), 0x1F = MRT3 (A+B+C+D). */
    int sel = (st->color_target == 2) ? 1 : 0;
    u32 raw = st->surface_color_offset[sel];
    if (out_mrt) {
        if (st->color_target >= 0x13) out_mrt[0] = st->surface_color_offset[1];
        if (st->color_target >= 0x17) out_mrt[1] = st->surface_color_offset[2];
        if (st->color_target >= 0x1F) out_mrt[2] = st->surface_color_offset[3];
    }
    { static int _rs = -1; if (_rs < 0) _rs = getenv("RT_SEQDBG") ? 1 : 0;
      static u32 _last = 0; static int _n2 = 0;
      if (_rs && st->surface_color_offset[0] != _last && _n2 < 400000) {
          _last = st->surface_color_offset[0]; _n2++;
          fprintf(stderr, "[RTSEQ] #%d -> 0x%X%c", _n2, _last, 10); } }
    { static int _ro = -1; if (_ro < 0) _ro = getenv("RT_OFFDBG") ? 1 : 0;
      static u32 _seen[32]; static int _ns = 0;
      if (_ro) { u32 _k = st->surface_color_offset[0] ^ (st->color_target << 24)
                        ^ (st->surface_clip_w << 8) ^ st->surface_format;
        int _f = 0; for (int _i = 0; _i < _ns; _i++) if (_seen[_i] == _k) _f = 1;
        if (!_f && _ns < 32) { _seen[_ns++] = _k;
        fprintf(stderr, "[RTOFF] fmt=0x%X tgt=0x%X off[0]=0x%X off[1]=0x%X zeta=0x%X clip=%ux%u disp=%d -> 0x%X\n",
                st->surface_format, st->color_target, st->surface_color_offset[0], st->surface_color_offset[1],
                st->surface_zeta_offset, st->surface_clip_w, st->surface_clip_h,
                cellGcmOffsetIsDisplay(raw), cellGcmOffsetIsDisplay(raw) ? 0 : raw); } } }
    if (cellGcmOffsetIsDisplay(raw)) return 0;
    /* RT_DISPLAY_BY_SIZE=1: also treat a single-target surface whose clip
     * exactly matches the display resolution as the backbuffer.
     *
     * cellGcmSetDisplayBuffer only registers the buffers the FLIP may point at;
     * a guest is free to render into a different surface of the same size and
     * flip to it later (PSGL does). Matching offsets alone then classifies every
     * draw as offscreen, has_display stays 0, and render_frame() -- which is
     * what draws ALL recorded geometry -- is never called. The backbuffer shows
     * only the clear, which looks exactly like "nothing rasterizes". */
    { static int _bs = -1; if (_bs < 0) _bs = getenv("RT_DISPLAY_BY_SIZE") ? 1 : 0;
      if (_bs && st->color_target < 0x13 &&
          st->surface_clip_w == s_d3d.width && st->surface_clip_h == s_d3d.height)
          return 0; }
    /* Surface clip dims when sane; else the window size. Any size works --
     * passes draw normalized full-surface quads -- this only picks resolution. */
    u32 w = st->surface_clip_w, h = st->surface_clip_h;
    if (w < 16 || w > 2048 || h < 16 || h > 2048) { w = 0; h = 0; }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return raw;
}

/* RSX surface colour format (SET_SURFACE_FORMAT bits [4:0]) -> DXGI. Float
 * targets matter: wave's water height maps store SIGNED values (F_W16..),
 * demosaic's differential planes likewise -- RGBA8 clamps them to zero. */
static DXGI_FORMAT rsx_surface_dxgi(u32 fmt)
{
    switch (fmt & 0x1F) {
    case 0x0B: return DXGI_FORMAT_R16G16B16A16_FLOAT; /* F_W16Z16Y16X16 */
    case 0x0C: return DXGI_FORMAT_R32G32B32A32_FLOAT; /* F_W32Z32Y32X32 */
    case 0x0D: return DXGI_FORMAT_R32_FLOAT;          /* F_X32          */
    default:   return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

static int off_rt_find(u32 off)
{
    if (!off) return -1;
    for (int i = 0; i < MAX_OFF_RTS; i++)
        if (s_d3d.off_rt[i].res && s_d3d.off_rt[i].off == off) return i;
    return -1;
}

/* Ensure an RT resource exists for this surface; (re)creates the RTV at
 * rt_rtv_heap[i] and the SRV at srv_heap[RT_SRV_BASE+i]. */
static int off_rt_get(u32 off, u32 w, u32 h, u32 rsx_fmt)
{
    if (!off || !s_d3d.rt_rtv_heap || !s_d3d.srv_heap) return -1;
    if (!w) w = s_d3d.width;
    if (!h) h = s_d3d.height;
    DXGI_FORMAT want_fmt = rsx_surface_dxgi(rsx_fmt);
    int slot = off_rt_find(off);
    if (slot >= 0) {
        OffRT* r = &s_d3d.off_rt[slot];
        if (r->w == w && r->h == h && r->dxgi == (u32)want_fmt) { r->used = 1; return slot; }
        r->res->lpVtbl->Release(r->res); r->res = NULL;   /* dims/format changed */
        if (r->up) { r->up->lpVtbl->Release(r->up); r->up = NULL; }
    } else {
        for (int i = 0; i < MAX_OFF_RTS; i++)
            if (!s_d3d.off_rt[i].res) { slot = i; break; }
        if (slot < 0) {   /* evict an entry not used this frame */
            for (int i = 0; i < MAX_OFF_RTS; i++)
                if (!s_d3d.off_rt[i].used) { slot = i; break; }
            if (slot < 0) return -1;
            s_d3d.off_rt[slot].res->lpVtbl->Release(s_d3d.off_rt[slot].res);
            s_d3d.off_rt[slot].res = NULL;
            if (s_d3d.off_rt[slot].up) {
                s_d3d.off_rt[slot].up->lpVtbl->Release(s_d3d.off_rt[slot].up);
                s_d3d.off_rt[slot].up = NULL;
            }
        }
    }
    OffRT* r = &s_d3d.off_rt[slot];
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {0};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = want_fmt; td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = {0};
    cv.Format = td.Format;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COPY_DEST, &cv,
            &IID_ID3D12Resource, (void**)&r->res)))
        return -1;
    r->off = off; r->w = w; r->h = h; r->dxgi = (u32)want_fmt;
    r->st = D3D12_RESOURCE_STATE_COPY_DEST;
    r->used = 1;

    /* Populate the new RT from guest memory: titles CPU-initialise their
     * render-target buffers (wave fills the height fields with texels of
     * (0,0,0, 1.0f) -- the .w is 'inverse of mass'; from an all-zero GPU
     * resource the water simulation can never boot). Guest data is
     * big-endian and tightly packed. */
    {
        extern uint8_t* vm_base;
        extern u32 cellGcmResolveOffset(u32);
        u32 bpp = (want_fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8u :
                  (want_fmt == DXGI_FORMAT_R32G32B32A32_FLOAT) ? 16u :
                  (want_fmt == DXGI_FORMAT_R32_FLOAT) ? 4u : 4u;
        u32 pitch = (w * bpp + 255) & ~255u;
        u32 src = cellGcmResolveOffset(off);
        if (vm_base && src != 0xFFFFFFFFu) {
            D3D12_HEAP_PROPERTIES hu = {0}; hu.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bd = {0};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = (u64)pitch * h; bd.Height = 1; bd.DepthOrArraySize = 1;
            bd.MipLevels = 1; bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(
                    s_d3d.device, &hu, D3D12_HEAP_FLAG_NONE, &bd,
                    D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                    &IID_ID3D12Resource, (void**)&r->up))) {
                void* mp = NULL; D3D12_RANGE nr = {0,0};
                if (SUCCEEDED(r->up->lpVtbl->Map(r->up, 0, &nr, &mp)) && mp) {
                    const u8* sp = vm_base + src;
                    for (u32 y = 0; y < h; y++) {
                        const u8* srow = sp + (u64)y * w * bpp;
                        u8* drow = (u8*)mp + (u64)y * pitch;
                        if (want_fmt == DXGI_FORMAT_R8G8B8A8_UNORM) {
                            /* guest A8R8G8B8 (bytes A,R,G,B) -> R,G,B,A */
                            for (u32 x = 0; x < w; x++) {
                                drow[x*4+0] = srow[x*4+1];
                                drow[x*4+1] = srow[x*4+2];
                                drow[x*4+2] = srow[x*4+3];
                                drow[x*4+3] = srow[x*4+0];
                            }
                        } else if (want_fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                            for (u32 x = 0; x < w * 4; x++) {   /* u16 halves, BE */
                                drow[x*2+0] = srow[x*2+1];
                                drow[x*2+1] = srow[x*2+0];
                            }
                        } else {                                 /* u32 floats, BE */
                            u32 nw = (w * bpp) / 4;
                            for (u32 x = 0; x < nw; x++) {
                                drow[x*4+0] = srow[x*4+3];
                                drow[x*4+1] = srow[x*4+2];
                                drow[x*4+2] = srow[x*4+1];
                                drow[x*4+3] = srow[x*4+0];
                            }
                        }
                    }
                    r->up->lpVtbl->Unmap(r->up, 0, NULL);
                    D3D12_TEXTURE_COPY_LOCATION cdst = {0}, csrc = {0};
                    cdst.pResource = r->res;
                    cdst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    csrc.pResource = r->up;
                    csrc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    csrc.PlacedFootprint.Footprint.Format   = want_fmt;
                    csrc.PlacedFootprint.Footprint.Width    = w;
                    csrc.PlacedFootprint.Footprint.Height   = h;
                    csrc.PlacedFootprint.Footprint.Depth    = 1;
                    csrc.PlacedFootprint.Footprint.RowPitch = pitch;
                    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &cdst, 0, 0, 0, &csrc, NULL);
                }
            }
        }
        {
            D3D12_RESOURCE_BARRIER b = {0};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource   = r->res;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
            r->st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rh;
    s_d3d.rt_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rt_rtv_heap, &rh);
    rh.ptr += (u64)slot * s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device, r->res, NULL, rh);

    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = td.Format;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE sh;
    s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &sh);
    sh.ptr += (u64)(RT_SRV_BASE + slot) * s_d3d.srv_inc;
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, r->res, &sv, sh);

    static int _log = 0;
    if (_log++ < 8)
        printf("[D3D12] offscreen RT %d: off=0x%X %ux%u (render-to-texture)\n",
               slot, off, w, h);
    return slot;
}

static D3D12_CPU_DESCRIPTOR_HANDLE off_rt_rtv(int slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE rh;
    s_d3d.rt_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rt_rtv_heap, &rh);
    rh.ptr += (u64)slot * s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return rh;
}

/* Write a texture SRV (or a null SRV when res == NULL) at an absolute SRV
 * heap slot. Used to fill per-draw t0-t3 descriptor windows. */
static void srv_write_ex(u32 heap_slot, ID3D12Resource* res, DXGI_FORMAT fmt,
                         UINT mapping, int cube)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = fmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = mapping;
    sv.Texture2D.MipLevels = 1;
    if (cube) {   /* must match the TextureCube declaration in the HLSL */
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube.MipLevels = 1;
        sv.TextureCube.MostDetailedMip = 0;
        sv.TextureCube.ResourceMinLODClamp = 0.0f;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &h);
    h.ptr += (u64)heap_slot * s_d3d.srv_inc;
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, res, &sv, h);
}

static void srv_write(u32 heap_slot, ID3D12Resource* res, DXGI_FORMAT fmt, UINT mapping)
{
    srv_write_ex(heap_slot, res, fmt, mapping, 0);
}

static void off_rt_transition(int slot, D3D12_RESOURCE_STATES to)
{
    OffRT* r = &s_d3d.off_rt[slot];
    if (r->st == to) return;
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = r->res;
    b.Transition.StateBefore = r->st;
    b.Transition.StateAfter  = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    r->st = to;
}

/* Set by the present entry point: 1 = this batch ends with a swapchain
 * Present, 0 = execute the recorded draws only. An OFFSCREEN-only batch (a
 * render-to-texture pass) must still run -- it is what fills the texture a
 * later batch samples -- but presenting it would show a half-built frame. */
static int s_present_this_frame = 1;

/* Resolve a DRAW_*_TEX value: a hex raw offset, or the literal "duck" for the
 * content-identified duck texture. */
static u32 draw_filter_tex(const char* e)
{
    if (!e) return 0;
    if (e[0] == 'd' && e[1] == 'u') return s_duck_raw;
    return (u32)strtoul(e, NULL, 16);
}

/* Copy the current backbuffer into s_screen_copy. Called mid-frame the moment
 * the reduced-viewport passes finish -- this title renders its reflection into
 * the TOP-LEFT REGION of the render target and the main scene then overwrites
 * it, so a snapshot taken at end of frame contains the scene, not the
 * reflection. Also called at end of frame as a fallback for frames with no
 * reduced-viewport pass. */
static void screen_copy_capture(u32 fi)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("SCREEN_AS_TEX"); en = e ? atoi(e) : 1; }
    if (!en || !s_d3d.device) return;
            if (!s_screen_copy) {
                D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC td = {0};
                td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                td.Width = s_d3d.width; td.Height = s_d3d.height;
                td.DepthOrArraySize = 1; td.MipLevels = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
                td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
                        s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL,
                        &IID_ID3D12Resource, (void**)&s_screen_copy)))
                    s_screen_copy = NULL;
            }
            if (s_screen_copy) {
                D3D12_RESOURCE_BARRIER bb[2] = {0};
                bb[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bb[0].Transition.pResource   = s_d3d.render_targets[fi];
                bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                bb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
                bb[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                bb[1] = bb[0];
                bb[1].Transition.pResource   = s_screen_copy;
                bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                bb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
                s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, bb);

                s_d3d.cmd_list->lpVtbl->CopyResource(s_d3d.cmd_list,
                    s_screen_copy, s_d3d.render_targets[fi]);

                bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                bb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
                bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                bb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, bb);

                /* SCREENCOPY_DUMP=1: stage the captured image into the readback
                 * buffer so it can be written out and LOOKED AT. Claiming a
                 * capture happened is not the same as showing what is in it. */
                { static int sd = -1;
                  if (sd < 0) { const char* e = getenv("SCREENCOPY_DUMP"); sd = e ? atoi(e) : 0; }
                  if (sd && s_d3d.readback_buf) {
                      D3D12_RESOURCE_BARRIER t = {0};
                      t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                      t.Transition.pResource   = s_screen_copy;
                      t.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                      t.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
                      t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                      s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &t);

                      D3D12_TEXTURE_COPY_LOCATION cd = {0}, cs = {0};
                      cd.pResource = s_d3d.readback_buf;
                      cd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                      cd.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
                      cd.PlacedFootprint.Footprint.Width    = s_d3d.width;
                      cd.PlacedFootprint.Footprint.Height   = s_d3d.height;
                      cd.PlacedFootprint.Footprint.Depth    = 1;
                      cd.PlacedFootprint.Footprint.RowPitch = s_d3d.readback_pitch;
                      cs.pResource = s_screen_copy;
                      cs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                      s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &cd, 0, 0, 0, &cs, NULL);

                      t.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                      t.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                      s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &t);
                      s_sc_dump_pending = 1;
                  } }
            }

}

static void render_frame(void)
{
    double _rf0 = perf_on() ? perf_now() : 0.0;
    u32 fi = s_d3d.frame_index;

    /* Drain the GPU before touching shared upload resources (vp_vb vertices,
     * vp_cb constants, per-frame texture staging): the previous frame's draws
     * may still be reading them, and overwriting mid-flight tears geometry
     * (gcm/cube: triangles mixing stale and new vertices -> missing/sliver
     * polygons). These workloads are a few draws/frame, so full serialisation
     * costs little. */
    wait_for_gpu();

    /* Compile the real vertex program once its microcode is captured, and keep
     * the constant bank uploaded for the VS. */
    /* The base VP pipeline gates the ENTIRE vertex-program draw pass below
     * (`if (s_d3d.vp_ready ...)`). The old trigger also required the microcode
     * size to differ from the last compile, so a title whose first captured
     * program has the same byte count as vp_compiled_bytes never built the base
     * pipeline -- and then every is_vp draw record was silently dropped: draws
     * recorded, constants uploaded, per-draw VS compiled and cached, and not one
     * of them submitted. While !vp_ready there is nothing to be stale against,
     * so only the "have microcode" test belongs here. */
    if (s_d3d.current_rsx_state && !s_d3d.vp_ready &&
        s_d3d.current_rsx_state->vp_ucode_bytes >= 16)
        compile_vp();
    /* Per-draw VP constants are snapshotted at record time (vp_record_cb). */

    /* Lazily create the readback buffer the first time a dump is requested. */
    if (s_d3d.dump_frames_left > 0 && !s_d3d.readback_buf) {
        s_d3d.readback_pitch = (s_d3d.width * 4 + 255) & ~255u;
        D3D12_HEAP_PROPERTIES hp = {0};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (u64)s_d3d.readback_pitch * s_d3d.height;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.readback_buf);
    }

    /* Reset command allocator and list */
    s_d3d.cmd_allocators[fi]->lpVtbl->Reset(s_d3d.cmd_allocators[fi]);
    s_d3d.cmd_list->lpVtbl->Reset(s_d3d.cmd_list, s_d3d.cmd_allocators[fi], NULL);

    /* Upload the bound font atlas into an R8_UNORM texture and create its SRV.
     * The atlas is a linear 8-bit coverage map, so a straight row copy (no
     * deswizzle) suffices. It is a DYNAMIC glyph cache -- the game rasterizes
     * new glyphs into it over time -- so re-upload whenever it is dirty (set on
     * every bind), not just once. Uploading only on first bind left every glyph
     * added after frame 1 sampling stale texels -> torn text. The backend is
     * synchronous (wait_for_gpu each frame), so recreating the resource per
     * dirty frame is safe. */
    if (s_d3d.tex_bound && (!s_d3d.tex_ready || s_d3d.tex_dirty) && s_d3d.srv_heap && s_d3d.pipeline_state_tex) {
        extern uint8_t* vm_base;
        u32 w = s_d3d.tex_w, h = s_d3d.tex_h;
        u32 pitch = (w + 255) & ~255u;   /* D3D12 requires 256-byte row pitch */

        if (s_d3d.tex_resource) { s_d3d.tex_resource->lpVtbl->Release(s_d3d.tex_resource); s_d3d.tex_resource = NULL; }
        if (s_d3d.tex_upload)   { s_d3d.tex_upload->lpVtbl->Release(s_d3d.tex_upload);     s_d3d.tex_upload = NULL; }

        D3D12_HEAP_PROPERTIES hp_def = {0}; hp_def.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {0};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8_UNORM; td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        HRESULT thr = s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &hp_def, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.tex_resource);

        D3D12_HEAP_PROPERTIES hp_up = {0}; hp_up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC ud = {0};
        ud.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        ud.Width = (u64)pitch * h; ud.Height = 1; ud.DepthOrArraySize = 1; ud.MipLevels = 1;
        ud.SampleDesc.Count = 1; ud.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (SUCCEEDED(thr))
            thr = s_d3d.device->lpVtbl->CreateCommittedResource(
                s_d3d.device, &hp_up, D3D12_HEAP_FLAG_NONE, &ud,
                D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.tex_upload);

        if (SUCCEEDED(thr) && vm_base) {
            void* mapped = NULL;
            D3D12_RANGE nr = {0, 0};
            if (SUCCEEDED(s_d3d.tex_upload->lpVtbl->Map(s_d3d.tex_upload, 0, &nr, &mapped)) && mapped) {
                const u8* srcbase = vm_base + s_d3d.tex_src_offset;
                for (u32 y = 0; y < h; y++)
                    memcpy((u8*)mapped + (u64)y * pitch, srcbase + (u64)y * w, w);
                s_d3d.tex_upload->lpVtbl->Unmap(s_d3d.tex_upload, 0, NULL);

                D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
                dst.pResource = s_d3d.tex_resource;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = 0;
                src.pResource = s_d3d.tex_upload;
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint.Offset = 0;
                src.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8_UNORM;
                src.PlacedFootprint.Footprint.Width    = w;
                src.PlacedFootprint.Footprint.Height   = h;
                src.PlacedFootprint.Footprint.Depth    = 1;
                src.PlacedFootprint.Footprint.RowPitch = pitch;
                s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);

                D3D12_RESOURCE_BARRIER tb = {0};
                tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                tb.Transition.pResource   = s_d3d.tex_resource;
                tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                tb.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &tb);

                D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
                sv.Format = DXGI_FORMAT_R8_UNORM;
                sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sv.Texture2D.MipLevels = 1;
                D3D12_CPU_DESCRIPTOR_HANDLE sh;
                s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &sh);
                s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, s_d3d.tex_resource, &sv, sh);

                s_d3d.tex_ready = 1;
                s_d3d.tex_dirty = 0;   /* content now in sync with guest atlas */
                { static int _au = 0; if (_au++ < 3)
                    printf("[D3D12] atlas uploaded (%ux%u R8) -> textured\n", w, h); }
            }
        }
    }

    /* Frames with no ops (init/boot presents) don't count against the cap --
     * PNG-decode-heavy titles burn hundreds of empty presents before the first
     * real draw. */
    if (getenv("RTT_DUMP") && s_d3d.draw_count > 0) { static int _f=0; int _cap = atoi(getenv("RTT_DUMP")); if (_cap < 2) _cap = 14; if (_f++ < _cap) {
        fprintf(stderr, "[RTT] frame %d: %u ops\n", _f, s_d3d.draw_count);
        for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
            D3D12DrawRecord* r = &s_d3d.draws[_d];
            fprintf(stderr, "[RTT]  op%02u %s rt=0x%X mrt=0x%X/0x%X/0x%X t0=0x%X fp=0x%X n=%u cmask=%X vp=%u,%u %ux%u blend=%d\n",
                _d, r->is_clear?"CLR ":(r->is_vp?"draw":"leg "), r->rt_off,
                r->rt_mrt[0], r->rt_mrt[1], r->rt_mrt[2],
                r->tex[0].raw, r->fp_addr, r->vertex_count,
                r->cmask, r->vp_x, r->vp_y, r->vp_w, r->vp_h, r->blend);
        }
    } }

    /* Debug: RTT_PASS=N shows pass N's output directly on screen (drops later
     * ops, retargets pass N to the backbuffer). */
    { const char* rp = getenv("RTT_PASS");
      if (rp) {
        int keep = atoi(rp), seen = 0; u32 cut = s_d3d.draw_count;
        int viewrt = getenv("RTT_VIEWRT") != NULL;
        for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
            D3D12DrawRecord* r = &s_d3d.draws[_d];
            if (r->is_clear || !r->is_vp) continue;
            if (seen == keep) {
                /* Alone: retarget op N to the backbuffer. With RTT_VIEWRT:
                 * keep op N intact and pull the display composite forward so
                 * the chosen RT is shown as of this point in the chain. */
                if (!viewrt) { r->rt_off = 0; r->rt_mrt[0] = r->rt_mrt[1] = r->rt_mrt[2] = 0; }
                cut = _d + 1;
                break;
            }
            seen++;
        }
        if (viewrt) {
            for (u32 _j = cut; _j < s_d3d.draw_count && _j < MAX_DRAWS; _j++) {
                D3D12DrawRecord* r = &s_d3d.draws[_j];
                if (r->is_vp && !r->is_clear && r->rt_off == 0) {
                    if (cut < MAX_DRAWS) s_d3d.draws[cut++] = *r;
                    break;
                }
            }
        }
        s_d3d.draw_count = (cut < s_d3d.draw_count) ? cut : s_d3d.draw_count;
      } }

    /* DRAW_LIMIT=<N>: keep only the first N recorded ops this frame. The duck's
     * draws are ops 00..~03 of every frame, so a small limit shows the ducks
     * alone -- separating "the ducks never rasterize" from "later geometry is
     * painted over them". Diagnostic only. */
    { static int lim = -1;
      if (lim < 0) { const char* e = getenv("DRAW_LIMIT"); lim = e ? atoi(e) : 0; }
      if (lim > 0 && s_d3d.draw_count > (u32)lim) s_d3d.draw_count = (u32)lim; }

    /* DRAW_SKIP_TEX=<hex raw offset>: drop every op binding that texture on
     * unit 0. The ducks sit behind the water's slab geometry, so removing the
     * occluder is the way to see whether the duck itself renders. Diagnostic. */
    { static const char* sk = (const char*)1; static u32 want = 0;
      if (sk == (const char*)1) sk = getenv("DRAW_SKIP_TEX");
      want = draw_filter_tex(sk);
      if (want) {
        u32 keep = 0;
        for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
            if (!s_d3d.draws[_d].is_clear && s_d3d.draws[_d].tex[0].raw == want) continue;
            if (keep != _d) s_d3d.draws[keep] = s_d3d.draws[_d];
            keep++;
        }
        s_d3d.draw_count = keep;
      } }

    /* DRAW_KEEP_TEX=<hex raw offset>: the inverse -- keep only the ops binding
     * that texture (plus clears). Isolates one object from everything drawn over
     * it, which is what finally shows the duck on its own. Diagnostic. */
    { static const char* kp = (const char*)1; static u32 want = 0;
      if (kp == (const char*)1) kp = getenv("DRAW_KEEP_TEX");
      want = draw_filter_tex(kp);
      /* DRAW_KEEP_NOCLEAR=1: also drop the clears. Keeping them preserves their
       * ORIGINAL position in the batch, so a clear that originally ran after the
       * kept draws still runs after them -- and wipes the very geometry being
       * isolated. That makes an object that renders fine look like it renders
       * nothing. */
      static int noclr = -1;
      if (noclr < 0) { const char* e = getenv("DRAW_KEEP_NOCLEAR"); noclr = e ? atoi(e) : 0; }
      /* DRAW_KEEP_BLEND=0|1: further restrict DRAW_KEEP_TEX to draws with that
       * blend flag. A mesh drawn twice -- once opaque, once blended -- is two
       * different passes (body vs reflection shell) that a texture filter alone
       * cannot separate. */
      static int kblend = -2;
      if (kblend == -2) { const char* e = getenv("DRAW_KEEP_BLEND");
                          kblend = e ? atoi(e) : -1; }
      if (want) {
        /* Emit the CLEARS FIRST, then the kept draws. Preserving the original
         * order means a clear that ran after them still runs after them and
         * wipes the isolated geometry; dropping the clears entirely (noclr)
         * leaves stale DEPTH that rejects most of its fragments. Neither shows
         * the object properly -- clear, then draw, does. */
        static D3D12DrawRecord kept[MAX_DRAWS];
        u32 nk = 0;
        if (!noclr)
            for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++)
                if (s_d3d.draws[_d].is_clear && nk < MAX_DRAWS) kept[nk++] = s_d3d.draws[_d];
        for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++)
            if (!s_d3d.draws[_d].is_clear && s_d3d.draws[_d].tex[0].raw == want
                && (kblend < 0 || s_d3d.draws[_d].blend == kblend)
                && nk < MAX_DRAWS)
                kept[nk++] = s_d3d.draws[_d];
        for (u32 _k = 0; _k < nk; _k++) s_d3d.draws[_k] = kept[_k];
        s_d3d.draw_count = nk;
      } }

    /* DRAW_LAST_TEX=<hex raw offset>: move the ops binding that texture to the
     * END of the batch so nothing is drawn over them. The duck's draws are ops
     * 00..03 of every frame and the tub wall is drawn afterwards across the same
     * screen area, so anything that defeats the depth sort hides the duck
     * completely. Reordering separates "occluded" from "not rendered". */
    { static const char* lt = (const char*)1; static u32 want = 0;
      if (lt == (const char*)1) lt = getenv("DRAW_LAST_TEX");
      want = draw_filter_tex(lt);
      if (want && s_d3d.draw_count > 1) {
        static D3D12DrawRecord moved[MAX_DRAWS];
        u32 nm = 0, keep = 0;
        for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
            if (!s_d3d.draws[_d].is_clear && s_d3d.draws[_d].tex[0].raw == want) {
                if (nm < MAX_DRAWS) moved[nm++] = s_d3d.draws[_d];
                continue;
            }
            if (keep != _d) s_d3d.draws[keep] = s_d3d.draws[_d];
            keep++;
        }
        for (u32 _m = 0; _m < nm && keep < MAX_DRAWS; _m++)
            s_d3d.draws[keep++] = moved[_m];
        s_d3d.draw_count = keep;
      } }

    /* Render-to-texture pre-pass: make sure an offscreen RT resource exists for
     * every non-display surface targeted this frame (so draws binding it as a
     * texture can resolve to it below, whatever the op order). */
    for (int _i = 0; _i < MAX_OFF_RTS; _i++) s_d3d.off_rt[_i].used = 0;
    for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
        D3D12DrawRecord* dr = &s_d3d.draws[_d];
        if (dr->is_vp && dr->rt_off)
            off_rt_get(dr->rt_off, dr->rt_w, dr->rt_h, dr->rt_fmt);
        for (int _m = 0; _m < 3; _m++)
            if (dr->is_vp && dr->rt_mrt[_m])
                off_rt_get(dr->rt_mrt[_m], dr->rt_w, dr->rt_h, dr->rt_fmt);
    }

    /* Per-frame VP textures + guest-FP pipelines: for each VP draw, upload the
     * texture it had bound at submit time into a slot (SRV heap 1+slot; plasma
     * animates so contents re-upload every frame) and pre-build its FP PSO.
     * A texture whose offset matches an offscreen RT samples the RT directly
     * (tex_slot 1000+idx) -- no guest-memory upload. */
    double _pre0 = perf_on() ? perf_now() : 0.0;
    for (int _i = 0; _i < VP_TEX_SLOTS; _i++) s_d3d.vp_tex[_i].used = 0;
    for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
        D3D12DrawRecord* dr = &s_d3d.draws[_d];
        if (!dr->is_vp || dr->is_clear) continue;
        /* Debug: RTT_VIEWRT=<hex raw offset> makes display draws sample that
         * offscreen RT at t0 (the composite blit then shows it fullscreen). */
        { static const char* vr = (const char*)1;     /* hoisted: this runs per DRAW */
          if (vr == (const char*)1) vr = getenv("RTT_VIEWRT");
          if (vr && dr->rt_off == 0) {
              dr->tex[0].raw = (u32)strtoul(vr, NULL, 16);
              dr->tex[0].off = 0;
              dr->tex[0].set = 1;
          } }
        /* SCREENTEX=1: report draws that sample a DISPLAY-SIZED texture. On RSX
         * a title renders a reflection/refraction into a surface and then
         * samples it; our backend renders into a D3D backbuffer, so the guest
         * memory that sampler reads is never written and the texture comes back
         * empty. Finding which geometry does it is the first step to feeding it
         * the rendered image instead. */
        /* EMPTYTEX=1: which geometry binds a texture whose source is all zero.
         * An unresolved texture renders untextured, so this names the surfaces
         * affected instead of leaving it to guesswork. */
        { static int ed = -1;
          if (ed < 0) { const char* e = getenv("EMPTYTEX"); ed = e ? atoi(e) : 0; }
          extern uint8_t* vm_base;
          if (ed && vm_base) for (int _u = 0; _u < 4; _u++) {
              if (!dr->tex[_u].set || !dr->tex[_u].off) continue;
              u32 nz = 0, sz = dr->tex[_u].w * dr->tex[_u].h * 4u;
              for (u32 i2 = 0; i2 < sz && i2 < 0x20000u; i2 += 997)
                  if (vm_base[dr->tex[_u].off + i2]) { nz = 1; break; }
              if (!nz) {
                  static u32 seen[24]; static int ns = 0; int known = 0;
                  for (int k = 0; k < ns; k++) if (seen[k] == dr->fp_addr) known = 1;
                  if (!known && ns < 24) { seen[ns++] = dr->fp_addr;
                      /* Scan a window around the resolved address: if the pass
                       * that fills this surface ran but landed elsewhere, the
                       * data is nearby; if the whole window is zero it was never
                       * produced. */
                      /* Same offset through BOTH context DMAs: a texture the
                       * guest put in main memory but that we resolve as LOCAL
                       * reads as untouched VRAM, i.e. all zeros. */
                      { extern u32 cellGcmResolveLocated(int, u32);
                        u32 la = cellGcmResolveLocated(1, dr->tex[_u].raw);
                        u32 ma = cellGcmResolveLocated(0, dr->tex[_u].raw);
                        u32 ln = 0, mn = 0;
                        for (u32 i4 = 0; i4 < 0x20000u; i4 += 997) {
                            if (vm_base[la + i4]) ln++;
                            if (vm_base[ma + i4]) mn++;
                        }
                        fprintf(stderr, "[TEXLOC] raw=0x%08X LOCAL@0x%08X nz=%u  MAIN@0x%08X nz=%u%c",
                                dr->tex[_u].raw, la, ln, ma, mn, 10); }
                      { u32 base = dr->tex[_u].off & ~0xFFFFFu;
                        for (u32 w = 0; w < 0x400000u; w += 0x100000u) {
                            u32 nzc = 0;
                            for (u32 i3 = 0; i3 < 0x100000u; i3 += 1021)
                                if (vm_base[base + w + i3]) nzc++;
                            fprintf(stderr, "[EMPTYSCAN] 0x%08X..+1MB nonzero %u/1027%c",
                                    base + w, nzc, 10);
                        } }
                      fprintf(stderr, "[EMPTYTEX] fp=0x%X unit=%d raw=0x%08X res=0x%08X %ux%u"
                                      " fmt=0x%02X verts=%u vp=%u,%u %ux%u%c",
                              dr->fp_addr, _u, dr->tex[_u].raw, dr->tex[_u].off, dr->tex[_u].w,
                              dr->tex[_u].h, dr->tex[_u].fmt, dr->vertex_count,
                              dr->vp_x, dr->vp_y, dr->vp_w, dr->vp_h, 10); }
              }
          } }
        { static int sd = -1;
          if (sd < 0) { const char* e = getenv("SCREENTEX"); sd = e ? atoi(e) : 0; }
          if (sd) for (int _u = 0; _u < 4; _u++)
              if (dr->tex[_u].set && dr->tex[_u].w == s_d3d.width &&
                  dr->tex[_u].h == s_d3d.height) {
                  static u32 seen[16]; static int ns = 0; int known = 0;
                  for (int k = 0; k < ns; k++) if (seen[k] == dr->fp_addr) known = 1;
                  if (!known && ns < 16) { seen[ns++] = dr->fp_addr;
                      fprintf(stderr, "[SCREENTEX] unit=%d raw=0x%08X %ux%u fmt=0x%02X"
                                      " fp=0x%X verts=%u vp=%u,%u %ux%u%c",
                              _u, dr->tex[_u].raw, dr->tex[_u].w, dr->tex[_u].h,
                              dr->tex[_u].fmt, dr->fp_addr, dr->vertex_count,
                              dr->vp_x, dr->vp_y, dr->vp_w, dr->vp_h, 10); }
              } }
        /* Fill this draw's t0-t3 SRV window (DRAW_SRV_BASE + d*4): each unit
         * resolves to an offscreen RT (sampled directly), an uploaded guest
         * texture, or a null SRV. */
        double _sv0 = perf_on() ? perf_now() : 0.0;
        for (int _u = 0; _u < 4; _u++) {
            u32 wslot = DRAW_SRV_BASE + _d * 4 + (u32)_u;
            dr->tex_rt[_u] = -1;
            /* Display-sized sampler source -> the rendered frame. */
            if (dr->tex[_u].set && s_screen_copy &&
                dr->tex[_u].w == s_d3d.width && dr->tex[_u].h == s_d3d.height) {
                static int en = -1;
                if (en < 0) { const char* e = getenv("SCREEN_AS_TEX"); en = e ? atoi(e) : 1; }
                if (en) {
                    { static int _n = 0; if (_n++ < 3)
                        fprintf(stderr, "[SCREENTEX] bound frame copy at unit %d for"
                                        " fp=0x%X (%ux%u)%c", _u, dr->fp_addr,
                                dr->tex[_u].w, dr->tex[_u].h, 10); }
                    srv_write(wslot, s_screen_copy, DXGI_FORMAT_R8G8B8A8_UNORM,
                              D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                    continue;
                }
            }
            /* A sampler whose guest buffer was never written, at the size of a
             * reduced-viewport pass we captured: that pass IS its producer. */
            if (dr->tex[_u].set && dr->tex[_u].off && s_subvp_n) {
                int hit = -1;
                for (int i = 0; i < s_subvp_n; i++)
                    if (s_subvp[i].res && s_subvp[i].w == dr->tex[_u].w &&
                        s_subvp[i].h == dr->tex[_u].h) { hit = i; break; }
                if (hit >= 0) {
                    extern uint8_t* vm_base;
                    u32 nz = 0, sz = dr->tex[_u].w * dr->tex[_u].h * 4u;
                    for (u32 i5 = 0; i5 < sz && i5 < 0x20000u; i5 += 997)
                        if (vm_base[dr->tex[_u].off + i5]) { nz = 1; break; }
                    if (!nz) {
                        { static int _n = 0; if (getenv("SUBVP_DBG") && _n++ < 8)
                            fprintf(stderr, "[SUBVP] unit %d fp=0x%X <- captured %ux%u%c",
                                    _u, dr->fp_addr, dr->tex[_u].w, dr->tex[_u].h, 10); }
                        srv_write(wslot, s_subvp[hit].res, DXGI_FORMAT_R8G8B8A8_UNORM,
                                  D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                        continue;
                    }
                }
            }
            if (dr->tex[_u].set) {
                int rt = off_rt_find(dr->tex[_u].raw);
                if (rt >= 0) {
                    dr->tex_rt[_u] = rt;
                    srv_write(wslot, s_d3d.off_rt[rt].res, (DXGI_FORMAT)s_d3d.off_rt[rt].dxgi,
                              D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                    continue;
                }
                u32 _bf = dr->tex[_u].fmt & 0x9F;
                if (dr->tex[_u].off &&
                    (_bf == 0x81 || _bf == 0x85 || _bf == 0x8B ||
                     (_bf >= 0x86 && _bf <= 0x88))) {
                    double _tt = perf_on() ? perf_now() : 0.0;
                    int _cube = dr->tex[_u].cube && (dr_cube_mask(dr) & (1u << _u));
                    int ts = vp_upload_tex_slot(dr->tex[_u].off, dr->tex[_u].w,
                                                dr->tex[_u].h, dr->tex[_u].fmt,
                                                _cube, dr->tex[_u].mips);
                    if (perf_on()) { s_perf_tex += perf_now() - _tt; s_perf_ntex++;
                        s_perf_texbytes += (u64)dr->tex[_u].w * dr->tex[_u].h * 4u; }
                    /* Both forms of the offset are in hand here; the filters
                     * compare the BOUND one. */
                    if (s_duck_off && dr->tex[_u].off == s_duck_off)
                        s_duck_raw = dr->tex[_u].raw;
                    if (ts >= 0) {
                        DXGI_FORMAT sf =
                            (_bf == 0x85) ? DXGI_FORMAT_R8G8B8A8_UNORM :
                            (_bf == 0x8B) ? DXGI_FORMAT_R8G8_UNORM :
                            (_bf == 0x86) ? DXGI_FORMAT_BC1_UNORM :
                            (_bf == 0x87) ? DXGI_FORMAT_BC2_UNORM :
                            (_bf == 0x88) ? DXGI_FORMAT_BC3_UNORM :
                                            DXGI_FORMAT_R8_UNORM;
                        srv_write_ex(wslot, s_d3d.vp_tex[ts].res, sf,
                                  (_bf == 0x81) ? 0x1000
                                                : rsx_remap_to_d3d(dr->tex[_u].ctrl1, _bf),
                                  _cube);
                        continue;
                    }
                }
            }
            /* TEX_BINDDBG=<N>: a unit that ends up with a NULL SRV. The draw is
             * still recorded, lit and transformed -- it just samples nothing,
             * which renders as a solid black object. */
            { static int cap = -1, n = 0;
              if (cap < 0) { const char* e = getenv("TEX_BINDDBG"); cap = e ? atoi(e) : 0; }
              if (cap && n < cap && dr->tex[_u].set) { n++;
                fprintf(stderr, "[TEXBIND] null SRV unit=%d raw=0x%X off=0x%X %ux%u fmt=0x%02X%c",
                        _u, dr->tex[_u].raw, dr->tex[_u].off, dr->tex[_u].w,
                        dr->tex[_u].h, dr->tex[_u].fmt, 10); } }
            srv_write(wslot, NULL, DXGI_FORMAT_R8G8B8A8_UNORM,
                      D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
        }
        if (perf_on()) s_perf_srv += perf_now() - _sv0;
        double _ps0 = perf_on() ? perf_now() : 0.0;
        if (dr->fp_addr) vp_get_fp_pso(dr->vs_idx, dr->fp_addr, dr->blend_key,
                                       dr_num_rts(dr),
                                       dr->rt_off ? rsx_surface_dxgi(dr->rt_fmt)
                                                  : DXGI_FORMAT_R8G8B8A8_UNORM,
                                       dr->fp_exp32, dr->cmask, dr->cull, dr_cube_mask(dr));
        if (perf_on()) s_perf_pso += perf_now() - _ps0;
    }

    /* Transition render target to RENDER_TARGET state */
    D3D12_RESOURCE_BARRIER barrier = {0};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = s_d3d.render_targets[fi];
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);

    /* Get RTV handle for current frame */
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
    s_d3d.rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rtv_heap, &rtv_handle);
    rtv_handle.ptr += fi * s_d3d.rtv_descriptor_size;

    /* Get DSV handle (single depth buffer shared across frames) */
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
    s_d3d.dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.dsv_heap, &dsv_handle);

    if (perf_on()) s_perf_pre += perf_now() - _pre0;
    /* Set render target + depth */
    s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(s_d3d.cmd_list, 1, &rtv_handle, FALSE, &dsv_handle);

    /* Clear color and depth */
    s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(
        s_d3d.cmd_list, rtv_handle, s_d3d.clear_color, 0, NULL);
    s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(
        s_d3d.cmd_list, dsv_handle,
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
        1.0f, 0, 0, NULL);

    /* Set viewport and scissor */
    D3D12_VIEWPORT viewport = {0, 0, (float)s_d3d.width, (float)s_d3d.height, 0.0f, 1.0f};
    D3D12_RECT scissor = {0, 0, (LONG)s_d3d.width, (LONG)s_d3d.height};
    s_d3d.cmd_list->lpVtbl->RSSetViewports(s_d3d.cmd_list, 1, &viewport);
    s_d3d.cmd_list->lpVtbl->RSSetScissorRects(s_d3d.cmd_list, 1, &scissor);

    /* Bind pipeline state and push MVP if anything to draw */
    if (s_d3d.pipeline_ready && s_d3d.draw_count > 0) {
        s_d3d.cmd_list->lpVtbl->SetGraphicsRootSignature(s_d3d.cmd_list, s_d3d.root_signature);
        s_d3d.cmd_list->lpVtbl->IASetVertexBuffers(s_d3d.cmd_list, 0, 1, &s_d3d.vb_view);

        /* Make the atlas SRV heap current and bind its table (t0) for textured
         * draws. Safe to set even if no draw is textured. */
        if (s_d3d.tex_ready && s_d3d.srv_heap) {
            ID3D12DescriptorHeap* heaps[] = { s_d3d.srv_heap };
            s_d3d.cmd_list->lpVtbl->SetDescriptorHeaps(s_d3d.cmd_list, 1, heaps);
            D3D12_GPU_DESCRIPTOR_HANDLE gh;
            s_d3d.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &gh);
            s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(s_d3d.cmd_list, 1, gh);
        }

        /* Push the MVP matrix from RSX vertex constants slots 0..3.
         * If the game hasn't written any constants (e.g. placeholder data
         * already in clip space), fall back to identity. */
        float mvp[16];
        const rsx_state* st = s_d3d.current_rsx_state;
        int have_mvp = 0;
        if (st) {
            for (u32 r = 0; r < 4; r++) {
                for (u32 c = 0; c < 4; c++) {
                    float v = st->vertex_constants[r][c];
                    mvp[r * 4 + c] = v;
                    if (v != 0.0f) have_mvp = 1;
                }
            }
        }
        if (!have_mvp) {
            memset(mvp, 0, sizeof(mvp));
            mvp[0] = mvp[5] = mvp[10] = mvp[15] = 1.0f; /* identity */
        }
        s_d3d.cmd_list->lpVtbl->SetGraphicsRoot32BitConstants(
            s_d3d.cmd_list, 0 /*root param 0*/, 16, mvp, 0);

        /* D3D12_IQ=1: dump exactly what the GPU is about to see -- the MVP root
         * constants and the first uploaded host vertices. The legacy path draws
         * with no validation errors yet produces zero pixels, so the geometry
         * must be degenerate/off-screen post-VS. */
        { static int _vd = -1;
          if (_vd < 0) { const char* e = getenv("D3D12_IQ"); _vd = e ? 1 : 0; }
          static int _n = 0;
          if (_vd && _n < 3) {
            _n++;
            fprintf(stderr, "[D3D12-DBG] have_mvp=%d draw_count=%u vb_offset=%u\n",
                    have_mvp, s_d3d.draw_count, s_d3d.vb_offset);
            fprintf(stderr, "[D3D12-DBG] mvp rows: [%.2f %.2f %.2f %.2f][%.2f %.2f %.2f %.2f]"
                            "[%.2f %.2f %.2f %.2f][%.2f %.2f %.2f %.2f]\n",
                    mvp[0],mvp[1],mvp[2],mvp[3], mvp[4],mvp[5],mvp[6],mvp[7],
                    mvp[8],mvp[9],mvp[10],mvp[11], mvp[12],mvp[13],mvp[14],mvp[15]);
            if (s_d3d.vb_mapped) {
                /* host vertex = 9 floats (pos3 + col4 + uv2), VERTEX_STRIDE=36 */
                const float* bv = (const float*)s_d3d.vb_mapped;
                for (int k = 0; k < 3; k++) {
                    const float* v = bv + k * 9;
                    fprintf(stderr, "[D3D12-DBG]  hostvert[%d] pos=(%.3f,%.3f,%.3f) col=(%.2f,%.2f,%.2f,%.2f)\n",
                            k, v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
                }
            }
            for (u32 d = 0; d < s_d3d.draw_count && d < 3; d++) {
                const D3D12DrawRecord* dr = &s_d3d.draws[d];
                fprintf(stderr, "[D3D12-DBG]  draw[%u] is_vp=%d is_clear=%d topo=%u cnt=%u vbofs=%u startv=%u rt=%u\n",
                        d, dr->is_vp, dr->is_clear, dr->topology, dr->vertex_count,
                        dr->vb_byte_offset, dr->vb_byte_offset / VERTEX_STRIDE, dr->rt_off);
            }
            fflush(stderr);
          } }

        /* Replay each recorded draw with its own primitive topology and
         * the matching PSO class (triangle / line / point). The PSO class
         * must match the topology or D3D12 rejects the draw. */
        u32 last_topo = 0xFFFFFFFFu;
        ID3D12PipelineState* last_pso = NULL;
        u32 draws = s_d3d.draw_count;
        if (draws > MAX_DRAWS) draws = MAX_DRAWS;
        for (u32 d = 0; d < draws; d++) {
            const D3D12DrawRecord* dr = &s_d3d.draws[d];
            if (dr->is_vp) continue; /* drawn by the VP pass below */

            /* Select PSO: textured triangles (dbgfont) use the atlas PSO;
             * otherwise pick by topology class. */
            ID3D12PipelineState* target_pso = s_d3d.pipeline_state; /* default triangle */
            if (dr->textured && s_d3d.tex_ready && s_d3d.pipeline_state_tex) {
                target_pso = s_d3d.pipeline_state_tex;
            } else if (dr->topology == D3D_TOPOLOGY_POINTLIST) {
                target_pso = s_d3d.pipeline_state_points
                             ? s_d3d.pipeline_state_points : s_d3d.pipeline_state;
            } else if (dr->topology == D3D_TOPOLOGY_LINELIST ||
                       dr->topology == D3D_TOPOLOGY_LINESTRIP) {
                target_pso = s_d3d.pipeline_state_lines
                             ? s_d3d.pipeline_state_lines : s_d3d.pipeline_state;
            }
            if (target_pso != last_pso) {
                s_d3d.cmd_list->lpVtbl->SetPipelineState(s_d3d.cmd_list, target_pso);
                last_pso = target_pso;
            }
            if (dr->topology != last_topo) {
                s_d3d.cmd_list->lpVtbl->IASetPrimitiveTopology(s_d3d.cmd_list, dr->topology);
                last_topo = dr->topology;
            }
            u32 start_vert = dr->vb_byte_offset / VERTEX_STRIDE;
            s_d3d.cmd_list->lpVtbl->DrawInstanced(
                s_d3d.cmd_list, dr->vertex_count, 1, start_vert, 0);
        }
    }

    /* VP pass: real decompiled vertex program + atlas alpha-test PS. Feeds raw
     * float4 attrib0 from vp_vb and the vp_c[] constant bank. */
    if (s_d3d.vp_ready && s_d3d.draw_count > 0) {
        int any = 0;
        for (u32 d = 0; d < s_d3d.draw_count && d < MAX_DRAWS; d++)
            if (s_d3d.draws[d].is_vp) { any = 1; break; }
        /* Textured geometry (dbgfont atlas) uses the sampling PS; untextured 3D
         * (vkcube) uses the colour-only PS. Fall back to whichever exists. */
        ID3D12PipelineState* vpso =
            (s_d3d.tex_ready && s_d3d.pipeline_state_vp) ? s_d3d.pipeline_state_vp
                                                         : s_d3d.pipeline_state_vp_color;
        if (!vpso) vpso = s_d3d.pipeline_state_vp;
        { static int cap = -1, n = 0;
          if (cap < 0) { const char* e = getenv("VP_SUBMIT"); cap = e ? atoi(e) : 0; }
          if (cap && n < cap) { n++;
            u32 nvp = 0, nclr = 0;
            for (u32 d2 = 0; d2 < s_d3d.draw_count && d2 < MAX_DRAWS; d2++) {
                if (s_d3d.draws[d2].is_vp)    nvp++;
                if (s_d3d.draws[d2].is_clear) nclr++;
            }
            fprintf(stderr, "[VPPASS] records=%u is_vp=%u clears=%u any=%d vpso=%p rootsig=%p\n",
                    s_d3d.draw_count, nvp, nclr, any,
                    (void*)vpso, (void*)s_d3d.vp_root_sig); } }
        if (any && vpso) {
            s_d3d.cmd_list->lpVtbl->SetGraphicsRootSignature(s_d3d.cmd_list, s_d3d.vp_root_sig);
            s_d3d.cmd_list->lpVtbl->SetPipelineState(s_d3d.cmd_list, vpso);
            s_d3d.cmd_list->lpVtbl->IASetPrimitiveTopology(s_d3d.cmd_list, D3D_TOPOLOGY_TRIANGLELIST);
            s_d3d.cmd_list->lpVtbl->SetGraphicsRootConstantBufferView(s_d3d.cmd_list, 0,
                s_d3d.vp_cb->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_cb));
            ID3D12DescriptorHeap* heaps[] = { s_d3d.srv_heap };
            s_d3d.cmd_list->lpVtbl->SetDescriptorHeaps(s_d3d.cmd_list, 1, heaps);
            D3D12_GPU_DESCRIPTOR_HANDLE gh;
            s_d3d.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &gh);
            s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(s_d3d.cmd_list, 1, gh);
            D3D12_VERTEX_BUFFER_VIEW vbv;
            vbv.BufferLocation = s_d3d.vp_vb->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_vb)
                               + (u64)s_d3d.vp_parity * MAX_VERTICES * 256;
            vbv.SizeInBytes    = MAX_VERTICES * 256;
            vbv.StrideInBytes  = 256;   /* 16 float4 attrib slots */
            s_d3d.cmd_list->lpVtbl->IASetVertexBuffers(s_d3d.cmd_list, 0, 1, &vbv);
            D3D12_GPU_DESCRIPTOR_HANDLE gh_base;
            s_d3d.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &gh_base);
            int cur_rt = -1;                       /* target A: -1 = backbuffer */
            int cur_m[3] = {-1, -1, -1};           /* MRT B/C/D: -1 = unbound   */
            double _rec0 = perf_on() ? perf_now() : 0.0;
            int seen_small_vp = 0, captured = 0;
            for (u32 d = 0; d < s_d3d.draw_count && d < MAX_DRAWS; d++) {
                const D3D12DrawRecord* dr = &s_d3d.draws[d];
                if (!dr->is_vp) continue;
                /* The reduced-viewport passes (this title's reflection, drawn
                 * into a region of the same target) finish here -- snapshot
                 * before the full-screen scene overwrites them. */
                if (!dr->is_clear) {
                    if (dr->vp_w && dr->vp_w < s_d3d.width) {
                        seen_small_vp = 1;
                        subvp_note(dr->vp_x, dr->vp_y, dr->vp_w, dr->vp_h);
                    }
                    else if (seen_small_vp && !captured &&
                             dr->vp_w == s_d3d.width && dr->rt_off == 0) {
                        captured = 1;
                        screen_copy_capture(fi);
                        subvp_capture(fi);
                        s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(
                            s_d3d.cmd_list, 1, &rtv_handle, FALSE, &dsv_handle);
                    }
                }
                /* Render-to-texture: retarget when this op's surfaces differ.
                 * Depth is a single shared buffer, so clear it per switch. */
                int want  = dr->rt_off  ? off_rt_find(dr->rt_off)  : -1;
                int wantm[3];
                for (int _m = 0; _m < 3; _m++)
                    wantm[_m] = dr->rt_mrt[_m] ? off_rt_find(dr->rt_mrt[_m]) : -1;
                if (want != cur_rt || wantm[0] != cur_m[0] ||
                    wantm[1] != cur_m[1] || wantm[2] != cur_m[2]) {
                    D3D12_CPU_DESCRIPTOR_HANDLE rh[4];
                    UINT nrt = 1;
                    rh[0] = rtv_handle;
                    D3D12_VIEWPORT vp = {0, 0, (float)s_d3d.width, (float)s_d3d.height, 0.0f, 1.0f};
                    if (want >= 0) {
                        off_rt_transition(want, D3D12_RESOURCE_STATE_RENDER_TARGET);
                        rh[0] = off_rt_rtv(want);
                        vp.Width  = (float)s_d3d.off_rt[want].w;
                        vp.Height = (float)s_d3d.off_rt[want].h;
                    }
                    for (int _m = 0; _m < 3 && wantm[_m] >= 0; _m++) {
                        off_rt_transition(wantm[_m], D3D12_RESOURCE_STATE_RENDER_TARGET);
                        rh[nrt++] = off_rt_rtv(wantm[_m]);
                    }
                    D3D12_RECT sc = {0, 0, (LONG)vp.Width, (LONG)vp.Height};
                    s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(s_d3d.cmd_list, nrt, rh, FALSE, &dsv_handle);
                    s_d3d.cmd_list->lpVtbl->RSSetViewports(s_d3d.cmd_list, 1, &vp);
                    s_d3d.cmd_list->lpVtbl->RSSetScissorRects(s_d3d.cmd_list, 1, &sc);
                    s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(s_d3d.cmd_list, dsv_handle,
                        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
                    cur_rt = want;
                    for (int _m = 0; _m < 3; _m++) cur_m[_m] = wantm[_m];
                }
                if (dr->is_clear) {
                    D3D12_CPU_DESCRIPTOR_HANDLE rh =
                        (cur_rt >= 0) ? off_rt_rtv(cur_rt) : rtv_handle;
                    s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(s_d3d.cmd_list, rh, dr->cc, 0, NULL);
                    for (int _m = 0; _m < 3; _m++)
                        if (cur_m[_m] >= 0) {
                            D3D12_CPU_DESCRIPTOR_HANDLE rhm = off_rt_rtv(cur_m[_m]);
                            s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(s_d3d.cmd_list, rhm, dr->cc, 0, NULL);
                        }
                    continue;
                }
                /* Per-draw pipeline: prefer the guest's own compiled FP; fall
                 * back to the hardcoded atlas/colour PS pair. */
                ID3D12PipelineState* dpso =
                    dr->fp_addr ? vp_get_fp_pso(dr->vs_idx, dr->fp_addr, dr->blend_key,
                                                dr_num_rts(dr),
                                                dr->rt_off ? rsx_surface_dxgi(dr->rt_fmt)
                                                           : DXGI_FORMAT_R8G8B8A8_UNORM,
                                                dr->fp_exp32, dr->cmask, dr->cull,
                                                dr_cube_mask(dr)) : NULL;
                s_d3d.cmd_list->lpVtbl->SetPipelineState(s_d3d.cmd_list,
                                                         dpso ? dpso : vpso);
                /* Per-draw viewport: the guest rect when sane, else the
                 * full target. Scissor tracks the same rect. */
                {
                    float tw = (cur_rt >= 0) ? (float)s_d3d.off_rt[cur_rt].w : (float)s_d3d.width;
                    float th = (cur_rt >= 0) ? (float)s_d3d.off_rt[cur_rt].h : (float)s_d3d.height;
                    D3D12_VIEWPORT dvp = {0, 0, tw, th, 0.0f, 1.0f};
                    if (dr->vp_w >= 2 && dr->vp_h >= 2 &&
                        (float)(dr->vp_x + dr->vp_w) <= tw + 0.5f &&
                        (float)(dr->vp_y + dr->vp_h) <= th + 0.5f) {
                        dvp.TopLeftX = (float)dr->vp_x;
                        dvp.TopLeftY = (float)dr->vp_y;
                        dvp.Width    = (float)dr->vp_w;
                        dvp.Height   = (float)dr->vp_h;
                    }
                    D3D12_RECT dsc = {(LONG)dvp.TopLeftX, (LONG)dvp.TopLeftY,
                                      (LONG)(dvp.TopLeftX + dvp.Width),
                                      (LONG)(dvp.TopLeftY + dvp.Height)};
                    s_d3d.cmd_list->lpVtbl->RSSetViewports(s_d3d.cmd_list, 1, &dvp);
                    s_d3d.cmd_list->lpVtbl->RSSetScissorRects(s_d3d.cmd_list, 1, &dsc);
                }
                /* Per-draw textures: bind this draw's t0-t3 SRV window.
                 * Any sampled offscreen RT transitions to PSR first (never
                 * one of the currently-bound colour targets). */
                for (int _u = 0; _u < 4; _u++) {
                    int rt = dr->tex_rt[_u];
                    if (rt >= 0 && rt != cur_rt &&
                        rt != cur_m[0] && rt != cur_m[1] && rt != cur_m[2])
                        off_rt_transition(rt, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                }
                D3D12_GPU_DESCRIPTOR_HANDLE gh = gh_base;
                gh.ptr += (u64)(DRAW_SRV_BASE + d * 4) * s_d3d.srv_inc;
                s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(s_d3d.cmd_list, 1, gh);
                /* Per-draw constants: this draw's vp_cb + FP texscale slots. */
                s_d3d.cmd_list->lpVtbl->SetGraphicsRootConstantBufferView(s_d3d.cmd_list, 0,
                    s_d3d.vp_cb->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_cb)
                    + ((u64)s_d3d.vp_parity * MAX_DRAWS + dr->cb_slot) * VP_CB_STRIDE);
                if (s_d3d.vp_fpcb)
                    s_d3d.cmd_list->lpVtbl->SetGraphicsRootConstantBufferView(s_d3d.cmd_list, 2,
                        s_d3d.vp_fpcb->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_fpcb)
                        + ((u64)s_d3d.vp_parity * MAX_DRAWS + dr->cb_slot) * VP_FPCB_STRIDE);
                s_d3d.cmd_list->lpVtbl->DrawInstanced(s_d3d.cmd_list,
                    dr->vertex_count, 1, dr->vb_byte_offset / 256, 0);
                /* VP_SUBMIT=<N>: prove the VP pass actually reaches the GPU.
                 * "records exist" and "draws were submitted" are different
                 * claims, and every blank-output investigation conflates them. */
                { static int cap = -1, n = 0;
                  if (cap < 0) { const char* e = getenv("VP_SUBMIT"); cap = e ? atoi(e) : 0; }
                  if (cap && n < cap) { n++;
                    fprintf(stderr, "[VPSUBMIT] draw[%u] verts=%u pso=%s vp=%ux%u tex0=0x%X(set=%d) tex1=0x%X slot=%d fp=0x%X%c",
                            d, dr->vertex_count, dpso ? "guest-fp" : "fallback",
                            dr->vp_w, dr->vp_h, dr->tex[0].raw, dr->tex[0].set,
                            dr->tex[1].raw, dr->tex_slot, dr->fp_addr, 10);
                    fprintf(stderr, "            vs_idx=%d cb_slot=%u vbofs=%u cull=%u blend=%d%c",
                            dr->vs_idx, dr->cb_slot, dr->vb_byte_offset / 256,
                            dr->cull, dr->blend, 10); } }
            }
            if (perf_on()) s_perf_gpu += perf_now() - _rec0;   /* reuse: record time */
            /* Leave the backbuffer bound for the dump/present epilogue. */
            if (cur_rt >= 0) {
                s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(s_d3d.cmd_list, 1, &rtv_handle, FALSE, &dsv_handle);
                D3D12_VIEWPORT vp = {0, 0, (float)s_d3d.width, (float)s_d3d.height, 0.0f, 1.0f};
                D3D12_RECT sc = {0, 0, (LONG)s_d3d.width, (LONG)s_d3d.height};
                s_d3d.cmd_list->lpVtbl->RSSetViewports(s_d3d.cmd_list, 1, &vp);
                s_d3d.cmd_list->lpVtbl->RSSetScissorRects(s_d3d.cmd_list, 1, &sc);
            }
        }
    }

    if (perf_on()) s_perf_rf += perf_now() - _rf0;
    /* MEMPEEK=<hex addr>:<n>: n big-endian floats at a guest address each frame,
     * with a min/max so a buffer of constants is distinguishable from real data. */
    { const char* mp = getenv("MEMPEEK");
      if (mp) { extern uint8_t* vm_base; static int _n = 0;
        u32 addr = 0; int cnt = 8; sscanf(mp, "%x:%d", &addr, &cnt);
        if (vm_base && addr && _n++ < 6) {
            float lo = 1e30f, hi = -1e30f; u32 nz = 0;
            for (int i = 0; i < cnt; i++) {
                const u8* q = vm_base + addr + i * 4;
                u32 bits = ((u32)q[0]<<24)|((u32)q[1]<<16)|((u32)q[2]<<8)|q[3];
                float f; memcpy(&f, &bits, 4);
                if (f == f && f < 1e30f && f > -1e30f) { if (f < lo) lo = f; if (f > hi) hi = f; }
                if (bits) nz++;
            }
            fprintf(stderr, "[MEMPEEK] 0x%08X n=%d nonzero=%u range[%g..%g]%c",
                    addr, cnt, nz, lo, hi, 10);
        } } }
    /* VRAMSCAN=1: which 64 KB blocks of guest memory change between frames.
     * "nothing writes the buffer we read" is only half an answer -- this says
     * where the producer IS writing, so the two can be matched up. */
    { static int vs = -1; if (vs < 0) vs = getenv("VRAMSCAN") ? 1 : 0;
      if (vs) { extern uint8_t* vm_base;
        static u32 prev[512]; static int have = 0; static int reported = 0;
        u32 base = 0xC6000000u; u32 nblk = 512;          /* 32 MB window */
        const char* bs = getenv("VRAMSCAN_BASE");
        if (bs) base = (u32)strtoul(bs, NULL, 0);
        if (vm_base && reported < 6) {
            u32 changed = 0; char list[512]; int ln = 0;
            for (u32 b = 0; b < nblk; b++) {
                const u8* p = vm_base + base + b * 0x10000u;
                u32 h = 2166136261u;
                for (u32 i = 0; i < 0x10000u; i += 61) h = (h ^ p[i]) * 16777619u;
                if (have && h != prev[b]) { changed++;
                    if (ln < 400) ln += snprintf(list + ln, sizeof(list) - ln,
                                                 " +0x%X", b * 0x10000u); }
                prev[b] = h;
            }
            if (have && changed) { reported++;
                fprintf(stderr, "[VRAMSCAN] base=0x%08X %u/512 blocks changed:%s%c",
                        base, changed, list, 10); }
            have = 1;
        } } }
    /* FRAME_BUDGET=1: geometry a frame asked for vs what the batch can hold.
     * Reported from render_frame so it works under RSX_ACCUM_FRAME too -- it
     * used to live in the clear-boundary present path, which accum bypasses, so
     * it silently produced nothing in exactly the configuration being used. */
    { static int fb = -1;
      if (fb < 0) { const char* e = getenv("FRAME_BUDGET"); fb = e ? atoi(e) : 0; }
      if (fb && s_req_draws) { static int n = 0; if (n++ < 40)
        fprintf(stderr, "[BUDGET] frame: requested %llu verts / %llu draws, "
                        "buffer holds %u verts, %llu draws truncated%c",
                (unsigned long long)s_req_verts, (unsigned long long)s_req_draws,
                MAX_VERTICES, (unsigned long long)s_drop_draws, 10); }
      s_req_verts = 0; s_req_draws = 0; s_drop_draws = 0; }
    if (perf_on()) {
        static double s_t_prev = 0.0; static int s_n = 0;
        double now = perf_now();
        if (s_t_prev > 0.0) s_perf_frame += now - s_t_prev;
        s_t_prev = now;
        if (++s_n % 20 == 0) {
            double fr = s_perf_frame > 0 ? s_perf_frame : 1;
            fprintf(stderr, "[PERF] %.2f fps | tex %.2fs (%.0f%%, %d calls) | vtx %.2fs"
                            " (%.0f%%, %.0fk verts) | render_frame %.2fs (%.0f%%)"
                            " | prepass %.2fs (%.0f%%) [srv %.2fs %.0f%% | pso %.2fs %.0f%% %d calls %d MISS %dKB hashed] | guest %.2fs (%.0f%%)%c",
                    20.0 / fr,
                    s_perf_tex, 100.0 * s_perf_tex / fr, s_perf_ntex,
                    s_perf_vtx, 100.0 * s_perf_vtx / fr,
                    (double)s_perf_nverts / 1000.0,
                    s_perf_rf, 100.0 * s_perf_rf / fr,
                    s_perf_pre, 100.0 * s_perf_pre / fr,
                    s_perf_srv, 100.0 * s_perf_srv / fr,
                    s_perf_pso, 100.0 * s_perf_pso / fr,
                    s_perf_pso_calls, s_perf_pso_miss, s_perf_pso_hashbytes / 1024,
                    fr - s_perf_rf, 100.0 * (fr - s_perf_rf) / fr, 10);
            s_perf_frame = 0.0; s_perf_tex = 0.0; s_perf_vtx = 0.0; s_perf_rf = 0.0;
            s_perf_gpu = 0.0; s_perf_pre = 0.0; s_perf_srv = 0.0; s_perf_pso = 0.0;
            s_perf_pso_calls = 0; s_perf_pso_miss = 0; s_perf_pso_hashbytes = 0;
            s_perf_ntex = 0; s_perf_texbytes = 0; s_perf_nverts = 0;
        }
    }
    { static int dt = -1;
      if (dt < 0) { const char* e = getenv("DUCKTRACK"); dt = e ? atoi(e) : 0; }
      if (dt) { static int fr = 0; fr++;
        if (s_lock_n)
            fprintf(stderr, "[DUCKTRACK] frame %d: %u duck draws, ndc=(%.3f,%.3f)%c",
                    fr, s_lock_n, (float)(s_lock_sx/s_lock_n), (float)(s_lock_sy/s_lock_n), 10);
        else if (fr % 20 == 0)
            fprintf(stderr, "[DUCKTRACK] frame %d: NO duck draws%c", fr, 10);
      } }
    if (s_lock_n) {                       /* publish this frame's centroid */
        s_lock_x = (float)(s_lock_sx / s_lock_n);
        s_lock_y = (float)(s_lock_sy / s_lock_n);
        s_lock_valid = 1;
        s_lock_sx = s_lock_sy = 0.0; s_lock_n = 0;
    }
    s_dbg_last_draws = s_d3d.draw_count;
    s_d3d.vb_offset  = 0; /* reset for next frame */
    s_d3d.vp_vb_offset = 0;
    s_d3d.draw_count = 0;
    s_d3d.vp_parity ^= 1;  /* next frame's records go to the other half */

    /* Debug: RTT_SAVERT=<hex raw offset>[:frame] copies that offscreen RT
     * into a readback buffer this frame and writes rt_save.bmp (half-float
     * RTs are tonemapped |v| -> byte). */
    static ID3D12Resource* s_rtsave_buf = NULL;
    static u32 s_rtsave_state = 0;   /* 1 = copy queued this frame */
    static u32 s_rtsave_w, s_rtsave_h, s_rtsave_pitch, s_rtsave_dxgi;
    { const char* sv = getenv("RTT_SAVERT");
      static int _done = 0;
      static u32 _skip = 0;
      if (sv && !_done && s_d3d.frame_count > 60 + _skip) {
        int rt = off_rt_find((u32)strtoul(sv, NULL, 16));
        if (rt >= 0 && s_d3d.off_rt[rt].res) {
            OffRT* r = &s_d3d.off_rt[rt];
            u32 bpp = (r->dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8 :
                      (r->dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) ? 16 : 4;
            u32 pitch = (r->w * bpp + 255) & ~255u;
            if (!s_rtsave_buf) {
                D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_READBACK;
                D3D12_RESOURCE_DESC rd = {0};
                rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                rd.Width = (u64)pitch * r->h; rd.Height = 1; rd.DepthOrArraySize = 1;
                rd.MipLevels = 1; rd.SampleDesc.Count = 1;
                rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                s_d3d.device->lpVtbl->CreateCommittedResource(
                    s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                    D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                    &IID_ID3D12Resource, (void**)&s_rtsave_buf);
            }
            if (s_rtsave_buf) {
                off_rt_transition(rt, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION cdst = {0}, csrc = {0};
                cdst.pResource = s_rtsave_buf;
                cdst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                cdst.PlacedFootprint.Footprint.Format   = (DXGI_FORMAT)r->dxgi;
                cdst.PlacedFootprint.Footprint.Width    = r->w;
                cdst.PlacedFootprint.Footprint.Height   = r->h;
                cdst.PlacedFootprint.Footprint.Depth    = 1;
                cdst.PlacedFootprint.Footprint.RowPitch = pitch;
                csrc.pResource = r->res;
                csrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &cdst, 0, 0, 0, &csrc, NULL);
                off_rt_transition(rt, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                s_rtsave_state = 1;
                s_rtsave_w = r->w; s_rtsave_h = r->h;
                s_rtsave_pitch = pitch; s_rtsave_dxgi = r->dxgi;
                _done = 1;
            }
        }
      } }

    /* CELLMARK_DUMP_MINDRAWS=<N>: only consider frames carrying at least N draw
     * records. Most render_frame calls come from the guest flip with a batch the
     * clear boundary already drained, so they present an empty backbuffer -- and
     * sampling those wastes every dump on a black image while the frames that do
     * hold the scene go uncaptured. */
    { static int mind = -1;
      if (mind < 0) { const char* e = getenv("CELLMARK_DUMP_MINDRAWS");
                      mind = e ? atoi(e) : 0; }
      /* draw_count is already reset by here; s_dbg_last_draws holds the
       * count this frame actually rendered. */
      if (mind > 0 && s_dbg_last_draws < (u32)mind) goto skip_dump_consider; }
    if (s_d3d.dump_skip_left > 0 && s_d3d.dump_frames_left > 0) s_d3d.dump_skip_left--;
skip_dump_consider: ;
    int dumping = (s_d3d.dump_frames_left > 0 && s_d3d.readback_buf
                   && s_d3d.dump_skip_left == 0);
    { static int mind2 = -1;
      if (mind2 < 0) { const char* e = getenv("CELLMARK_DUMP_MINDRAWS");
                       mind2 = e ? atoi(e) : 0; }
      if (mind2 > 0 && s_dbg_last_draws < (u32)mind2) dumping = 0; }
    /* CELLMARK_DUMP_EVERY=N: after each dump, skip N-1 frames -- samples the whole
     * run instead of one contiguous window, so a short burst of real content
     * can't fall between the dumped frames. */
    if (dumping) {
        static int s_every = -1;
        if (s_every < 0) { const char* e = getenv("CELLMARK_DUMP_EVERY");
                           s_every = e ? atoi(e) : 0; }
        if (s_every > 1) s_d3d.dump_skip_left = s_every - 1;
    }
    if (dumping) {
        /* RT -> COPY_SOURCE, copy into the readback buffer, then -> PRESENT. */
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);

        D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
        dst.pResource = s_d3d.readback_buf;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width    = s_d3d.width;
        dst.PlacedFootprint.Footprint.Height   = s_d3d.height;
        dst.PlacedFootprint.Footprint.Depth    = 1;
        dst.PlacedFootprint.Footprint.RowPitch = s_d3d.readback_pitch;
        src.pResource = s_d3d.render_targets[fi];
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);
    } else {
        /* End-of-frame fallback snapshot: used when the frame never contained a
         * reduced-viewport pass to capture mid-frame. */
        screen_copy_capture(fi);
        /* Transition render target to PRESENT state */
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);
    }

    /* Close and execute */
    s_d3d.cmd_list->lpVtbl->Close(s_d3d.cmd_list);
    ID3D12CommandList* cmd_lists[] = {(ID3D12CommandList*)s_d3d.cmd_list};
    s_d3d.cmd_queue->lpVtbl->ExecuteCommandLists(s_d3d.cmd_queue, 1, cmd_lists);

    /* D3D12_IQ=1: drain the debug layer's message queue after submitting the
     * frame. The legacy (is_vp=0) DrawInstanced path silently produced ZERO
     * pixels in flOw's render injection while ClearRenderTargetView worked, and
     * every input (verts, offsets, layout, MVP, PSO, root sig, depth, cull) was
     * verified correct -- so the only remaining explanation is a validation
     * error the debug layer is swallowing. Off unless the env var is set. */
    { static int _iq = -1;
      if (_iq < 0) { const char* e = getenv("D3D12_IQ"); _iq = e ? 1 : 0; }
      if (_iq && s_d3d.device) {
        ID3D12InfoQueue* iq = NULL;
        if (SUCCEEDED(s_d3d.device->lpVtbl->QueryInterface(
                s_d3d.device, &IID_ID3D12InfoQueue, (void**)&iq)) && iq) {
            UINT64 n = iq->lpVtbl->GetNumStoredMessages(iq);
            static int _printed = 0;
            for (UINT64 mi = 0; mi < n && _printed < 80; mi++) {
                SIZE_T len = 0;
                iq->lpVtbl->GetMessage(iq, mi, NULL, &len);
                D3D12_MESSAGE* m = (D3D12_MESSAGE*)malloc(len);
                if (m && SUCCEEDED(iq->lpVtbl->GetMessage(iq, mi, m, &len))) {
                    fprintf(stderr, "[D3D12-IQ][sev=%d id=%d] %s\n",
                            (int)m->Severity, (int)m->ID, m->pDescription);
                    _printed++;
                }
                free(m);
            }
            if (n) iq->lpVtbl->ClearStoredMessages(iq);
            iq->lpVtbl->Release(iq);
            fflush(stderr);
        }
      } }

    if (s_sc_dump_pending) {
        s_sc_dump_pending = 0;
        wait_for_gpu();
        s_dump_name = "screencopy";
        dump_backbuffer_bmp();
        s_dump_name = NULL;
    }
    if (dumping) {
        wait_for_gpu();            /* ensure the copy finished before mapping */
        dump_backbuffer_bmp();
        s_d3d.dump_frames_left--;
    }

    if (s_rtsave_state) {
        if (!dumping) { double _g = perf_on() ? perf_now() : 0.0;
                        wait_for_gpu();
                        if (perf_on()) s_perf_gpu += perf_now() - _g; }
        void* mp = NULL; D3D12_RANGE rr = {0, (SIZE_T)s_rtsave_pitch * s_rtsave_h};
        if (SUCCEEDED(s_rtsave_buf->lpVtbl->Map(s_rtsave_buf, 0, &rr, &mp)) && mp) {
            FILE* f = fopen("rt_save.bmp", "wb");
            if (f) {
                u32 w = s_rtsave_w, h = s_rtsave_h;
                u32 rowb = (w * 3 + 3) & ~3u;
                u32 datasz = rowb * h;
                u8 hdr[54] = {0};
                hdr[0]='B'; hdr[1]='M';
                *(u32*)(hdr+2) = 54 + datasz; *(u32*)(hdr+10) = 54;
                *(u32*)(hdr+14) = 40; *(int*)(hdr+18) = (int)w; *(int*)(hdr+22) = (int)h;
                *(u16*)(hdr+26) = 1; *(u16*)(hdr+28) = 24; *(u32*)(hdr+34) = datasz;
                fwrite(hdr, 1, 54, f);
                u8* line = (u8*)malloc(rowb);
                for (int y = (int)h - 1; y >= 0; y--) {
                    const u8* srow = (const u8*)mp + (u64)y * s_rtsave_pitch;
                    memset(line, 0, rowb);
                    for (u32 x = 0; x < w; x++) {
                        float rv, gv, bv;
                        if (s_rtsave_dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                            const u16* hp16 = (const u16*)(srow + (u64)x * 8);
                            /* crude half->float: sign|exp|mant */
                            float v[3];
                            for (int c2 = 0; c2 < 3; c2++) {
                                u16 hv = hp16[c2];
                                u32 sign = (hv >> 15) & 1, exp = (hv >> 10) & 0x1F, man = hv & 0x3FF;
                                float fv;
                                if (exp == 0) fv = (float)man / 16777216.0f;
                                else { u32 fb = (sign << 31) | ((exp - 15 + 127) << 23) | (man << 13);
                                       memcpy(&fv, &fb, 4); }
                                v[c2] = fv;
                            }
                            rv = v[0]; gv = v[1]; bv = v[2];
                        } else if (s_rtsave_dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) {
                            const float* fp32 = (const float*)(srow + (u64)x * 16);
                            rv = fp32[0]; gv = fp32[1]; bv = fp32[2];
                        } else {
                            const u8* p8 = srow + (u64)x * 4;
                            rv = p8[0] / 255.0f; gv = p8[1] / 255.0f; bv = p8[2] / 255.0f;
                        }
                        /* RTT_SAVEA=1: show the alpha lane in RED (gates
                         * like wave's mask.w live there). */
                        if (getenv("RTT_SAVEA")) {
                            float av;
                            if (s_rtsave_dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                                const u16* hp16 = (const u16*)(srow + (u64)x * 8);
                                u16 hv = hp16[3];
                                u32 sign = (hv >> 15) & 1, exp = (hv >> 10) & 0x1F, man = hv & 0x3FF;
                                if (exp == 0) av = (float)man / 16777216.0f;
                                else { u32 fb = (sign << 31) | ((exp - 15 + 127) << 23) | (man << 13);
                                       memcpy(&av, &fb, 4); }
                            } else if (s_rtsave_dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) {
                                av = ((const float*)(srow + (u64)x * 16))[3];
                            } else {
                                av = (srow + (u64)x * 4)[3] / 255.0f;
                            }
                            rv = av;
                        }
                        /* |v| tonemap so signed heights are visible */
                        float ar = rv < 0 ? -rv : rv, ag = gv < 0 ? -gv : gv, ab = bv < 0 ? -bv : bv;
                        if (ar > 1) ar = 1; if (ag > 1) ag = 1; if (ab > 1) ab = 1;
                        line[x*3+0] = (u8)(ab * 255.0f);
                        line[x*3+1] = (u8)(ag * 255.0f);
                        line[x*3+2] = (u8)(ar * 255.0f);
                    }
                    fwrite(line, 1, rowb, f);
                }
                free(line);
                fclose(f);
                printf("[D3D12] wrote rt_save.bmp (%ux%u dxgi=%u)\n", w, h, s_rtsave_dxgi);
            }
            s_rtsave_buf->lpVtbl->Unmap(s_rtsave_buf, 0, NULL);
        }
        s_rtsave_state = 0;
    }

    /* rsxcap: snapshot whole frames (backbuffer + every offscreen RT + a draw
     * manifest) at RSX_CAP=start[:count[:stride]] frame_counts. Runs before the
     * present transition would matter -- the backbuffer is in PRESENT here and
     * is restored to PRESENT after each readback. */
    { const char* cap = getenv("RSX_CAP");
      if (cap) {
        static int s_cap_parsed = 0;
        static u32 s_cap_start = 0, s_cap_count = 1, s_cap_stride = 1, s_cap_done = 0;
        if (!s_cap_parsed) {
            s_cap_parsed = 1;
            u32 a = 0, b = 1, c = 1;
            int n = sscanf(cap, "%u:%u:%u", &a, &b, &c);
            s_cap_start  = (n >= 1 && a) ? a : 200;
            s_cap_count  = (n >= 2 && b) ? b : 1;
            s_cap_stride = (n >= 3 && c) ? c : 1;
        }
        if (s_cap_done < s_cap_count) {
            u32 target = s_cap_start + s_cap_done * s_cap_stride;
            if (s_d3d.frame_count >= target && s_dbg_last_draws > 0) {
                rsx_capture_frame(fi, s_dbg_last_draws, s_cap_done);
                s_cap_done++;
            }
        }
      } }

    /* Present */
    if (s_present_this_frame)
        s_d3d.swap_chain->lpVtbl->Present(s_d3d.swap_chain, 1, 0); /* vsync */   /* skipped for an offscreen-only batch */

    move_to_next_frame();

    s_d3d.frame_count++;

    /* RPCS3-style titlebar stats, refreshed once a second: presented FPS,
     * draw count of the last frame, and the backbuffer size. */
    {
        static ULONGLONG s_tt0 = 0;
        static u64 s_tframes = 0;
        s_tframes++;
        ULONGLONG tnow = GetTickCount64();
        if (s_tt0 == 0) s_tt0 = tnow;
        if (tnow - s_tt0 >= 1000 && s_d3d.hwnd) {
            extern char g_rsx_title_base[128];
            char tb[256];
            snprintf(tb, sizeof(tb), "%s | FPS: %.2f | draws: %u | %ux%u",
                     g_rsx_title_base,
                     s_tframes * 1000.0 / (double)(tnow - s_tt0),
                     s_dbg_last_draws, s_d3d.width, s_d3d.height);
            SetWindowTextA(s_d3d.hwnd, tb);
            s_tframes = 0;
            s_tt0 = tnow;
        }
    }
}

/* ---------------------------------------------------------------------------
 * RSX backend callbacks
 * -----------------------------------------------------------------------*/

static int d3d12_init(void* ud, u32 width, u32 height)
{
    (void)ud;
    printf("[D3D12] Backend init(%ux%u)\n", width, height);
    return 0;
}

static void d3d12_shutdown(void* ud)
{
    (void)ud;
    printf("[D3D12] Backend shutdown\n");
}

static void d3d12_begin_frame(void* ud)
{
    (void)ud;
}

static void d3d12_end_frame(void* ud)
{
    (void)ud;
}

static u32 s_dbg_clears_since_present = 0;   /* CELLMARK_BLINKDBG */
static u32 s_clear_presents = 0;   /* presents issued at clear (frame boundary) */

/* FRAME_BUDGET=1: how much geometry a guest frame actually asks for, versus
 * what the per-frame vertex buffer can hold. A frame that overflows is silently
 * truncated -- the tail of the scene never renders -- which reads as "the object
 * is missing" rather than "the batch is too small". */

static int blink_dbg(void)
{
    static int v = -1;
    if (v < 0) v = getenv("CELLMARK_BLINKDBG") ? 1 : 0;
    return v;
}

static void d3d12_present(void* ud, u32 buffer_id)
{
    (void)ud;
    (void)buffer_id;

    if (blink_dbg())
        printf("[PRESENT] draws=%u clears_since_last=%u\n",
               s_d3d.draw_count, s_dbg_clears_since_present);
    s_dbg_clears_since_present = 0;

    /* An accumulated batch with draws but NONE targeting a display buffer is
     * offscreen pass work only (demosaic flips once per effect pass): showing
     * it would strobe the bare backbuffer clear. Keep accumulating -- the
     * composite draw that targets the display presents the whole chain, in
     * order, in one command list. Empty batches still present (boot/idle). */
    int flip_has_display = (s_d3d.draw_count == 0);
    for (u32 _i = 0; _i < s_d3d.draw_count && _i < MAX_DRAWS; _i++)
        if (!s_d3d.draws[_i].is_clear && s_d3d.draws[_i].rt_off == 0) {
            flip_has_display = 1;
            break;
        }

    if (s_d3d.initialized && flip_has_display)
        render_frame();

    /* FPS tracking */
    ULONGLONG now = GetTickCount64();
    if (now - s_d3d.last_fps_time >= 1000) {
        s_d3d.fps = (u32)s_d3d.frame_count; /* rough estimate */
        s_d3d.last_fps_time = now;
        s_d3d.frame_count = 0;
    }
}

static void d3d12_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud;
    (void)flags;
    (void)depth;
    (void)stencil;

    /* Convert RSX ARGB u32 to float[4] RGBA */
    float cc[4];
    cc[0] = ((color >> 16) & 0xFF) / 255.0f; /* R */
    cc[1] = ((color >> 8) & 0xFF) / 255.0f;  /* G */
    cc[2] = (color & 0xFF) / 255.0f;          /* B */
    cc[3] = ((color >> 24) & 0xFF) / 255.0f;  /* A */
    /* CLEAR_RGB=r,g,b: override the guest clear colour. Black holes (geometry
     * that never rasterized) and black pixels a shader really wrote look
     * identical against a black clear; this separates them in one run. */
    { static int _cinit = 0; static float _co[3]; static int _con = 0;
      if (!_cinit) { _cinit = 1; const char* e = getenv("CLEAR_RGB");
          if (e && sscanf(e, "%f,%f,%f", &_co[0], &_co[1], &_co[2]) == 3) _con = 1; }
      if (_con) { cc[0] = _co[0]; cc[1] = _co[1]; cc[2] = _co[2]; } }

    u32 rt_w = 0, rt_h = 0, mrt[3] = {0, 0, 0};
    u32 rt = current_rt_off(&rt_w, &rt_h, mrt);

    /* An OFFSCREEN clear is just an ordered op in the current frame's pass
     * chain (demosaic clears each effect pass's surface) -- record it, don't
     * touch the frame boundary. */
    if (rt != 0) {
        if (s_d3d.draw_count < MAX_DRAWS) {
            D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count++];
            memset(dr, 0, sizeof(*dr));
            dr->is_vp = 1; dr->is_clear = 1; dr->tex_slot = -1;
            dr->rt_off = rt; memcpy(dr->rt_mrt, mrt, sizeof(mrt));
            dr->rt_w = rt_w; dr->rt_h = rt_h;
            dr->rt_fmt = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_format : 0;
            memcpy(dr->cc, cc, sizeof(cc));
        }
        return;
    }

    memcpy(s_d3d.clear_color, cc, sizeof(cc));

    /* A DISPLAY clear marks the start of a new visible frame. If a completed
     * frame is still accumulated (the drain gulped across a frame boundary --
     * guaranteed at the FIFO ring wrap, where rest-of-frame-N + clear-N+1
     * arrive in one batch), PRESENT it now instead of discarding it. Only a
     * batch that actually contains DISPLAY draws is a completed frame: a
     * batch of offscreen pass work (render-to-texture) must keep accumulating
     * until its composite draw arrives, or the screen strobes intermediates. */
    int have_display_draws = 0;
    for (u32 i = 0; i < s_d3d.draw_count && i < MAX_DRAWS; i++)
        if (!s_d3d.draws[i].is_clear && s_d3d.draws[i].rt_off == 0) {
            have_display_draws = 1;
            break;
        }
    if (!have_display_draws)
        return;   /* keep accumulating the in-progress frame */

    /* A title that DOUBLE-BUFFERS the display (DeferredShading clears both
     * 0x0 and 0x440000 per frame, plus a HUD pass) issues several display
     * clears per real frame -- treating each as a boundary presented the
     * 1-2-draw intermediates, strobing black between the full 146-draw
     * frames. Anchor the boundary to the FLIP instead: only present once a
     * cellGcmSetFlip has landed since the last present. Titles that flip once
     * per frame (wave/cellmark/gcmcube) are unaffected -- their single
     * display clear still follows their single flip. Fall back to the old
     * clear-only heuristic for titles that never flip (fc stays 0). */
    static u32 s_frame_draws_max = 0;   /* running max frame size (typical full frame) */
    if (s_d3d.draw_count > s_frame_draws_max) s_frame_draws_max = s_d3d.draw_count;
    {
        extern unsigned cellGcm_flip_request_count(void);
        static unsigned s_last_present_flip = 0;
        unsigned fc = cellGcm_flip_request_count();
        if (fc != 0) {
            if (fc == s_last_present_flip)
                return;   /* no flip since last present -> not a real boundary */
            /* A double-buffered title issues several display clears per flip
             * (DeferredShading: clear back-buffer, HUD pass, ...). They can
             * arrive in either order, so a small batch here may be an
             * intermediate that precedes the full frame's clear. Don't present
             * (or consume the flip) until the batch is a substantial fraction
             * of a full frame -- keep accumulating so the complete frame lands
             * in one present. Gauged against the running MAX frame size (not
             * the last, which a leaked tiny batch would poison), so it
             * self-scales per title with no fixed threshold. */
            if (s_frame_draws_max > 16 && s_d3d.draw_count < s_frame_draws_max / 4)
                return;   /* intermediate: keep accumulating, flip stays pending */
            s_last_present_flip = fc;
        }
    }

    /* RSX_ACCUM_FRAME=1: never present at a clear boundary, so every draw in a
     * guest frame accumulates into one image. A title that clears several times
     * per frame (render-to-texture passes) otherwise gets presented mid-scene,
     * and each captured frame holds only a slice of the geometry -- the floor in
     * one, a wall in the next -- which makes a single model impossible to see. */
    { static int accum = -1;
      if (accum < 0) { const char* e = getenv("RSX_ACCUM_FRAME"); accum = e ? atoi(e) : 0; }
      if (accum) return; }
    if (s_d3d.initialized) {
        if (blink_dbg())
            printf("[CLEAR] presenting %u accumulated draws at frame boundary\n",
                   s_d3d.draw_count);
        render_frame();
        s_clear_presents++;
    }
    s_dbg_clears_since_present++;
    s_d3d.draw_count   = 0;
    s_d3d.vb_offset    = 0;
    s_d3d.vp_vb_offset = 0;
}

static void d3d12_set_render_target(void* ud, const rsx_state* state)
{
    (void)ud;
    s_d3d.current_rsx_state = state;
    /* Log only the first few; set_render_target is called every frame and
     * floods the log otherwise. */
    static int s_count = 0;
    if (s_count < 5) {
        printf("[D3D12] set_render_target(%ux%u)\n",
               state->surface_clip_w, state->surface_clip_h);
        s_count++;
    }
}

static void d3d12_set_viewport(void* ud, const rsx_state* state)
{
    (void)ud;
    /* TODO: update D3D12 viewport from RSX state */
    (void)state;
}

/* Our host vertex layout for the fallback path: position (xyz) + color (rgba)
 * + texcoord (uv), 36 bytes. (The real VP path feeds raw float4 attrib0.) */
typedef struct { float x, y, z; float r, g, b, a; float u, v; } BasicVertex;


/* Read one RSX vertex (by absolute vertex index) from guest memory into our
 * host layout. Position is attrib 0 (float3+), color is attrib 3 (ubyte4 or
 * float4); missing attribs default to opaque white. RSX stores each 32-bit
 * component big-endian, so every lane is byte-swapped. */
static void read_rsx_vertex(const rsx_state* state, u32 vindex, BasicVertex* out)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveOffset(u32);

    out->x = out->y = out->z = 0.0f;
    out->r = out->g = out->b = out->a = 1.0f;
    out->u = out->v = 0.0f;
    if (!state || !vm_base) return;

    const rsx_vertex_attrib* pos = &state->vertex_attribs[0];
    if (pos->enabled && pos->type == 2 /* float */ && pos->size >= 2) {
        u8* src = vm_base + cellGcmResolveOffset(pos->offset + vindex * pos->stride);
        out->x = rsx_rd_bef(src);
        out->y = rsx_rd_bef(src + 4);
        if (pos->size >= 3) out->z = rsx_rd_bef(src + 8);

        /* dbgfont (and similar 2D overlays) store screen positions biased far
         * outside clip space; their vertex program folds them back to clip
         * space using transform constants we don't execute. When a position is
         * well outside NDC, recover the fractional part as a normalized [0,1]
         * screen coord and map it to NDC (screen Y-down -> clip Y-up). In this
         * layout the attribute is [posX, posY, U, V] (size 4), so the last two
         * components are the atlas texcoords, not depth.
         * TODO: execute the real vertex-program / RSX viewport transform so
         * this is not needed. */
        if (out->x > 2.0f || out->x < -2.0f || out->y > 2.0f || out->y < -2.0f) {
            float sx = out->x - floorf(out->x);
            float sy = out->y - floorf(out->y);
            out->x = sx * 2.0f - 1.0f;
            out->y = 1.0f - sy * 2.0f;
            out->z = 0.0f;
            if (pos->size >= 4) {
                out->u = rsx_rd_bef(src + 8);   /* U */
                out->v = rsx_rd_bef(src + 12);  /* V */
            }
        }
    }

    const rsx_vertex_attrib* col = &state->vertex_attribs[3];
    if (col->enabled && col->size >= 3) {
        u8* src = vm_base + cellGcmResolveOffset(col->offset + vindex * col->stride);
        if (col->type == 4 /* ubyte */) {
            out->r = src[0] / 255.0f;
            out->g = src[1] / 255.0f;
            out->b = src[2] / 255.0f;
            out->a = (col->size >= 4) ? src[3] / 255.0f : 1.0f;
        } else if (col->type == 2 /* float */) {
            u32 fr, fg, fb, fa;
            memcpy(&fr, src,     4); fr = ((fr>>24)&0xFF)|((fr>>8)&0xFF00)|((fr<<8)&0xFF0000)|((fr<<24)&0xFF000000);
            memcpy(&fg, src + 4, 4); fg = ((fg>>24)&0xFF)|((fg>>8)&0xFF00)|((fg<<8)&0xFF0000)|((fg<<24)&0xFF000000);
            memcpy(&fb, src + 8, 4); fb = ((fb>>24)&0xFF)|((fb>>8)&0xFF00)|((fb<<8)&0xFF0000)|((fb<<24)&0xFF000000);
            memcpy(&out->r, &fr, 4); memcpy(&out->g, &fg, 4); memcpy(&out->b, &fb, 4);
            if (col->size >= 4) {
                memcpy(&fa, src + 12, 4); fa = ((fa>>24)&0xFF)|((fa>>8)&0xFF00)|((fa<<8)&0xFF0000)|((fa<<24)&0xFF000000);
                memcpy(&out->a, &fa, 4);
            }
        }
    }
}

/* Upload `count` sequential vertices [first, first+count). Returns the count
 * actually written (clamped to the remaining per-frame buffer). */
static u32 upload_vertices_from_rsx(u32 first, u32 count)
{
    BasicVertex* verts = (BasicVertex*)((u8*)s_d3d.vb_mapped + s_d3d.vb_offset);
    u32 max_verts = (MAX_VERTICES * VERTEX_STRIDE - s_d3d.vb_offset) / sizeof(BasicVertex);
    if (count > max_verts) count = max_verts;
    const rsx_state* state = s_d3d.current_rsx_state;
    for (u32 i = 0; i < count; i++)
        read_rsx_vertex(state, first + i, &verts[i]);
    s_d3d.vb_offset += count * sizeof(BasicVertex);
    return count;
}

/* Upload RSX QUADS (prim 8) as a triangle list: each 4-vertex quad v0..v3
 * (perimeter winding) splits into triangles (v0,v1,v2) and (v0,v2,v3).
 * D3D12 has no quad topology, so this expansion is how quads render at all.
 * Returns the number of triangle-list vertices emitted (6 per quad). */
static u32 upload_quads_from_rsx(u32 first, u32 count)
{
    const rsx_state* state = s_d3d.current_rsx_state;
    u32 quads = count / 4;
    u32 max_verts = (MAX_VERTICES * VERTEX_STRIDE - s_d3d.vb_offset) / sizeof(BasicVertex);
    if (quads * 6 > max_verts) quads = max_verts / 6;
    BasicVertex* verts = (BasicVertex*)((u8*)s_d3d.vb_mapped + s_d3d.vb_offset);
    u32 o = 0;
    for (u32 q = 0; q < quads; q++) {
        BasicVertex c[4];
        for (u32 k = 0; k < 4; k++)
            read_rsx_vertex(state, first + q * 4 + k, &c[k]);
        verts[o++] = c[0]; verts[o++] = c[1]; verts[o++] = c[2];
        verts[o++] = c[0]; verts[o++] = c[2]; verts[o++] = c[3];
    }
    s_d3d.vb_offset += o * sizeof(BasicVertex);
    return o;
}

/* Upload RSX QUADS as raw float4 attrib0 (byte-swapped) into vp_vb, expanded
 * to a triangle list (6 verts/quad). The decompiled vertex shader does the
 * transform. Returns emitted vertex count. */
/* Generic VP vertex: all 16 RSX vertex attributes, each converted to a float4
 * slot -- 256 bytes/vertex, input layout ATTRi @ i*16. Apps place attributes at
 * arbitrary indices (tiny3d: pos=a0 colour=a3 tex=a8; SDK gcm samples: pos=a0
 * colour=a1 tex=a2; dbgfont: a0..a2), so a hardcoded pos+colour pair can't
 * cover them: every enabled attrib is fetched from guest memory and converted
 * by its RSX type; disabled slots read (0,0,0,1). */
typedef struct { float v[4]; } VPSlot;
#define VP_VERT_STRIDE (16 * sizeof(VPSlot))   /* 256 */


void rsx_vtx_pos_dbg(const rsx_state* state, const float* v, u32 n);

/* Fill all 16 attribute slots for one vertex. The per-attribute work lives in
 * rsx_vertex_fetch.c so the Metal and null backends read guest vertices the
 * same way this one does, rather than each porting the logic again. */
static void read_vp_vertex(const rsx_state* state, u32 vi, VPSlot* out16)
{
    for (int i = 0; i < 16; i++) {
        rsx_fetch_attrib(state, i, vi, out16[i].v);
        if (i == 0) {
            const rsx_vertex_attrib* a = &state->vertex_attribs[0];
            u32 n = a->size ? a->size : 4; if (n > 4) n = 4;
            rsx_vtx_pos_dbg(state, out16[0].v, n);
        }
    }
}

/* VTX_POS=<N>: print the first fetched position of the first N draws. A guest
 * vertex array that resolves to the wrong memory reads as garbage/denormals, and
 * that is indistinguishable from "the shader is wrong" without seeing the input. */
void rsx_vtx_pos_dbg(const rsx_state* state, const float* v, u32 n)
{
    static int cap = -1, seen = 0;
    if (cap < 0) { const char* e = getenv("VTX_POS"); cap = e ? atoi(e) : 0; }
    if (!cap || seen >= cap) return;
    seen++;
    const rsx_vertex_attrib* a = &state->vertex_attribs[0];
    fprintf(stderr, "[VTXPOS] a0 off=0x%X stride=%u size=%u type=%u -> (%g, %g, %g, %g)\n",
            a->offset, a->stride, a->size, a->type,
            n > 0 ? v[0] : 0.f, n > 1 ? v[1] : 0.f, n > 2 ? v[2] : 0.f, n > 3 ? v[3] : 0.f);
}

static void vp_attrs_dbg(const rsx_state* state)
{
    if (!getenv("VP_ATTRS")) return;
    /* VP_ATTRS_FP=<hex shader_program>: dump only the draws that use one
     * fragment program. Without it the first six draws are whatever the frame
     * happens to start with, which is never the mesh being investigated. */
    { static const char* e = (const char*)1; static u32 want = 0;
      if (e == (const char*)1) { e = getenv("VP_ATTRS_FP");
          want = e ? (u32)strtoul(e, NULL, 0) : 0; }
      if (want && (!state || state->shader_program != want)) return; }
    /* Dedupe by the set of enabled attributes rather than printing the first
     * few draws: one fragment program can be used by meshes with different
     * vertex layouts, and a mesh missing an attribute takes the constant
     * register instead -- which is exactly the case worth seeing. */
    { u32 mask = 0;
      for (int i = 0; i < 16; i++) if (state->vertex_attribs[i].enabled) mask |= 1u << i;
      static u32 seen[16]; static int ns = 0;
      for (int i = 0; i < ns; i++) if (seen[i] == mask) return;
      if (ns >= 16) return;
      seen[ns++] = mask; }
    fprintf(stderr, "[VPATTR] --- fp=0x%X ---%c", state->shader_program, 10);
    fprintf(stderr, "[VPATTR] divider_op=0x%08X\n", state->frequency_divider_op);
    for (int i = 0; i < 16; i++) {
        const rsx_vertex_attrib* a = &state->vertex_attribs[i];
        if (a->enabled) fprintf(stderr, "[VPATTR] a%d off=0x%X stride=%u size=%u type=%u freq=%u fmt=0x%08X\n",
                                i, a->offset, a->stride, a->size, a->type, a->frequency, a->format);
    }
    /* VP_ATTRS_DUMP=<attr>:<n>: read n entries of that attribute straight out of
     * guest memory. Answers "did the guest write these values, or did our fetch
     * mangle them" without inferring it from the rendered image. */
    { const char* e = getenv("VP_ATTRS_DUMP"); if (!e) return;
      int ai = 0, cnt = 8; sscanf(e, "%d:%d", &ai, &cnt);
      const rsx_vertex_attrib* a = &state->vertex_attribs[ai];
      if (!a->enabled || !a->stride) return;
      extern uint8_t* vm_base;
      extern u32 cellGcmResolveLocated(int, u32);
      u32 nbad = 0, nzero = 0;
      for (int v = 0; v < cnt; v++) {
          u32 aoff = (a->offset & 0x7FFFFFFFu) + (u32)v * a->stride;
          const u8* q = vm_base + ((a->offset & 0x80000000u)
                        ? cellGcmResolveLocated(0, aoff) : cellGcmResolveLocated(1, aoff));
          /* Also read the same offset through the OTHER context-DMA. An array
           * the guest put in main memory but whose offset we resolve as LOCAL
           * reads as untouched VRAM, i.e. all zeros -- indistinguishable from
           * "the producer never ran". */
          if (v == 0) {
              u32 la = cellGcmResolveLocated(1, aoff), ma = cellGcmResolveLocated(0, aoff);
              const u8* lp = vm_base + la; const u8* mp = vm_base + ma;
              const u8* rp = vm_base + (a->offset & 0x7FFFFFFFu);   /* raw, unresolved */
              fprintf(stderr, "[VPDUMP] a%d off=0x%X  LOCAL@0x%X=(%g,%g,%g)  MAIN@0x%X=(%g,%g,%g)"
                              "  RAW@0x%X=(%g,%g,%g)%c",
                      ai, aoff, la, rsx_rd_bef(lp), rsx_rd_bef(lp+4), rsx_rd_bef(lp+8),
                      ma, rsx_rd_bef(mp), rsx_rd_bef(mp+4), rsx_rd_bef(mp+8),
                      (a->offset & 0x7FFFFFFFu), rsx_rd_bef(rp), rsx_rd_bef(rp+4), rsx_rd_bef(rp+8), 10);
          }
          u32 nc = a->size ? a->size : 3; if (nc > 3) nc = 3;
          float f[3] = {0,0,0};
          for (u32 k = 0; k < nc; k++) f[k] = rsx_rd_bef(q + k * 4);
          if (v < 12)
              fprintf(stderr, "[VPDUMP] a%d[%d] = (%g, %g, %g)%c", ai, v, f[0], f[1], f[2], 10);
          int allz = 1; for (u32 k = 0; k < nc; k++) if (f[k] != 0.f) allz = 0;
          if (allz) nzero++;
          else if (f[2] < -0.9f && f[0] > -0.1f && f[0] < 0.1f) nbad++;
      }
      fprintf(stderr, "[VPDUMP] a%d over %d verts: %u zero, %u near (0,0,-1)%c",
              ai, cnt, nzero, nbad, 10);
      /* How many entries differ from the first? An all-identical position array
       * is a degenerate mesh: it rasterizes nothing, which looks the same as
       * "the draw never happened". */
      { float f0[3]; u32 o0 = (a->offset & 0x7FFFFFFFu);
        const u8* q0 = vm_base + ((a->offset & 0x80000000u)
                       ? cellGcmResolveLocated(0, o0) : cellGcmResolveLocated(1, o0));
        for (int k = 0; k < 3; k++) f0[k] = rsx_rd_bef(q0 + k * 4);
        u32 diff = 0;
        for (int v = 1; v < cnt; v++) {
            u32 ao = o0 + (u32)v * a->stride;
            const u8* q = vm_base + ((a->offset & 0x80000000u)
                          ? cellGcmResolveLocated(0, ao) : cellGcmResolveLocated(1, ao));
            for (int k = 0; k < 3; k++)
                if (rsx_rd_bef(q + k * 4) != f0[k]) { diff++; break; }
        }
        fprintf(stderr, "[VPDUMP] a%d: %u/%d entries differ from entry 0 (%g,%g,%g)%c",
                ai, diff, cnt, f0[0], f0[1], f0[2], 10);
        /* Extent: an isosurface with little fluid is a speck, and a speck that
         * rasterizes nothing is correct, not a bug. */
        { float lo[3] = {1e30f,1e30f,1e30f}, hi[3] = {-1e30f,-1e30f,-1e30f};
          for (int v = 0; v < cnt; v++) {
              u32 ao = o0 + (u32)v * a->stride;
              const u8* q = vm_base + ((a->offset & 0x80000000u)
                            ? cellGcmResolveLocated(0, ao) : cellGcmResolveLocated(1, ao));
              for (int k = 0; k < 3; k++) { float f = rsx_rd_bef(q + k * 4);
                  if (f < lo[k]) lo[k] = f; if (f > hi[k]) hi[k] = f; } }
          u32 nnan = 0, nfin = 0;
          for (int v = 0; v < cnt; v++) {
              u32 ao = o0 + (u32)v * a->stride;
              const u8* q = vm_base + ((a->offset & 0x80000000u)
                            ? cellGcmResolveLocated(0, ao) : cellGcmResolveLocated(1, ao));
              int bad = 0;
              for (int k = 0; k < 3; k++) { float f = rsx_rd_bef(q + k * 4);
                  if (!(f == f) || f > 1e30f || f < -1e30f) bad = 1; }
              if (bad) nnan++; else nfin++;
          }
          fprintf(stderr, "[VPDUMP] a%d extent x[%g..%g] y[%g..%g] z[%g..%g]"
                          "  finite=%u nan/inf=%u%c",
                  ai, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], nfin, nnan, 10); } } }
}

static u32 upload_quads_vp(const rsx_state* state, u32 first, u32 count)
{
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    vp_attrs_dbg(state);
    u32 quads = count / 4;
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (quads * 6 > maxv) quads = maxv / 6;
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    u32 o = 0;
    for (u32 q = 0; q < quads; q++) {
        VPSlot c[4][16];
        for (u32 k = 0; k < 4; k++)
            read_vp_vertex(state, first + q*4 + k, c[k]);
        /* quad -> two triangles (perimeter winding) */
        static const int idx[6] = {0,1,2, 0,2,3};
        for (int t = 0; t < 6; t++) { memcpy(&out[o*16], c[idx[t]], sizeof(c[0])); o++; }
        if (getenv("VTX_DUMP")) { static int _n=0; if (_n++ < 4) {
            FILE* f = fopen("vtx_dump.txt", _n==1 ? "w" : "a");
            if (f) { for (u32 k = 0; k < 4; k++)
                fprintf(f, "q%02d v%u a0=(%.3f,%.3f,%.3f,%.3f) a8=(%.3f,%.3f)\n", _n, k,
                    c[k][0].v[0],c[k][0].v[1],c[k][0].v[2],c[k][0].v[3],
                    c[k][8].v[0],c[k][8].v[1]);
              fclose(f); } } }
    }
    s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
    return o;
}

/* Straight triangle-list upload through the VP path (gcm/cube's cube draws
 * TRIANGLES, prim 5 -- no expansion needed). */
static u32 upload_tris_vp(const rsx_state* state, u32 first, u32 count)
{
    extern uint8_t* vm_base;
    s_req_verts += count; s_req_draws++;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    vp_attrs_dbg(state);
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (count > maxv) { s_drop_draws++;
        { static int _n = 0; if (getenv("VBFULL") && _n++ < 12)
            fprintf(stderr, "[VBFULL] fp=0x%X wanted %u verts, room for %u%c",
                    state->shader_program, count, maxv, 10); }
        count = maxv - (maxv % 3); }
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    { double _tv = perf_on() ? perf_now() : 0.0;
      for (u32 k = 0; k < count; k++)
          read_vp_vertex(state, first + k, &out[k*16]);
      if (perf_on()) { s_perf_vtx += perf_now() - _tv; s_perf_nverts += count; } }
    /* IDXDBG=<hex shader_program>: vertex range for that program's draws. */
    { static const char* ie = (const char*)1; static u32 iw = 0;
      if (ie == (const char*)1) { ie = getenv("IDXDBG");
          iw = ie ? (u32)strtoul(ie, NULL, 16) : 0; }
      if (iw && state->shader_program == iw) { static int _n = 0; if (_n++ < 12)
          fprintf(stderr, "[IDXDBG] tris first=%u count=%u a0off=0x%X stride=%u%c",
                  first, count, state->vertex_attribs[0].offset,
                  state->vertex_attribs[0].stride, 10); } }
    /* DUCK_VTX=<hex tex0 offset>: dump attribute-0 positions for the draws that
     * bind that texture. The duck's texture resolves and its 9960 draws all
     * target the backbuffer, yet none of its texels reach the screen -- so the
     * question is whether its vertices are where they should be. */
    { static const char* dv = (const char*)1; static u32 want = 0; static int n = 0;
      if (dv == (const char*)1) { dv = getenv("DUCK_VTX");
                                  want = dv ? (u32)strtoul(dv, NULL, 16) : 0; }
      if (want && s_d3d.cur_texs[0].raw == want && n < 6) { n++;
        fprintf(stderr, "[DUCKVTX] first=%u count=%u", first, count);
        for (u32 k = 0; k < count && k < 4; k++)
            fprintf(stderr, "  v%u=(%.2f,%.2f,%.2f)", k,
                    out[k*16].v[0], out[k*16].v[1], out[k*16].v[2]);
        { int lo = -1, hi = -1, nz = 0;
          for (int r = 0; r < RSX_MAX_VERTEX_CONSTANTS; r++) {
              const float* m = state->vertex_constants[r];
              if (m[0] || m[1] || m[2] || m[3]) { if (lo < 0) lo = r; hi = r; nz++; }
          }
          fprintf(stderr, "  | constants: %d non-zero, slots %d..%d", nz, lo, hi); }
        fprintf(stderr, "%c", 10);
      } }
    if (getenv("VTX_DUMP") && state->surface_color_offset[0] == 0xCC0000) {
        static int _n=0; if (_n++ < 1) {
        FILE* f = fopen("vtx_dump.txt", "w");
        if (f) { fprintf(f, "G-buffer draw surf0=0x%X count=%u first=%u\n",
                         state->surface_color_offset[0], count, first);
          for (u32 k = 0; k < count && k < 30; k++)
            fprintf(f, "v%02u a0=(%.3f,%.3f,%.3f) a2=(%.3f,%.3f,%.3f) a9=(%.4f,%.4f,%.4f,%.4f)\n", k,
                out[k*16].v[0],out[k*16].v[1],out[k*16].v[2],
                out[k*16+2].v[0],out[k*16+2].v[1],out[k*16+2].v[2],
                out[k*16+9].v[0],out[k*16+9].v[1],out[k*16+9].v[2],out[k*16+9].v[3]);
          fclose(f); } } }
    s_d3d.vp_vb_offset += count * VP_VERT_STRIDE;
    return count;
}

/* Non-indexed TRIANGLE_STRIP (fan=0) / TRIANGLE_FAN (fan=1) through the VP
 * path, expanded to a triangle list. LBP's Bink movie draws its YUV quad as a
 * non-indexed 4-vertex strip (prim 6); without this it fell to the fixed-
 * function fallback with is_vp=0 (no vertex-program transform -> the quad
 * collapsed to a line) and textured=0 (the Y/U/V planes never sampled). */
static u32 upload_strip_vp(const rsx_state* state, u32 first, u32 count, int fan)
{
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    if (count < 3) return 0;
    u32 tris = count - 2;
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (tris * 3 > maxv) tris = maxv / 3;
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    u32 o = 0;
    for (u32 t = 0; t < tris; t++) {
        u32 i0 = fan ? 0 : t;
        /* strip winding alternates per triangle to keep facing consistent */
        u32 i1 = t + 1, i2 = t + 2;
        if (!fan && (t & 1)) { u32 tmp = i1; i1 = i2; i2 = tmp; }
        read_vp_vertex(state, first + i0, &out[o*16]); o++;
        read_vp_vertex(state, first + i1, &out[o*16]); o++;
        read_vp_vertex(state, first + i2, &out[o*16]); o++;
    }
    s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
    return o;
}

/* Fetch index k from the guest index array (SET_INDEX_ARRAY_ADDRESS/_DMA:
 * dma [3:0] = location (0 local, 1 main), [7:4] = type (0 u32, 1 u16)).
 * Indices are big-endian in guest memory. */
static u32 read_guest_index(const rsx_state* st, u32 k)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    int local = ((st->index_array_dma & 0xF) == 0);
    u32 base = cellGcmResolveLocated(local, st->index_array_offset);
    const u8* p = vm_base + base;
    if (((st->index_array_dma >> 4) & 0xF) == 1) {
        p += (u64)k * 2;
        return ((u32)p[0] << 8) | p[1];
    }
    p += (u64)k * 4;
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* Indexed variants of the VP-path uploads. */
static u32 upload_quads_vp_indexed(const rsx_state* state, u32 first, u32 count)
{
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    u32 quads = count / 4;
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (quads * 6 > maxv) quads = maxv / 6;
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    u32 o = 0;
    for (u32 q = 0; q < quads; q++) {
        VPSlot c[4][16];
        for (u32 k = 0; k < 4; k++)
            read_vp_vertex(state, read_guest_index(state, first + q*4 + k), c[k]);
        static const int idx[6] = {0,1,2, 0,2,3};
        for (int t = 0; t < 6; t++) { memcpy(&out[o*16], c[idx[t]], sizeof(c[0])); o++; }
        if (getenv("VTX_DUMP")) { static int _n=0; if (_n++ < 6) {
            FILE* f = fopen("vtx_dump.txt", _n==1 ? "w" : "a");
            if (f) {
                const float* vs_ = state->viewport_scale;
                const float* vo_ = state->viewport_offset;
                fprintf(f, "iq%02d vp_scale=(%.1f,%.1f,%.3f) vp_off=(%.1f,%.1f,%.3f)\n", _n,
                        vs_[0], vs_[1], vs_[2], vo_[0], vo_[1], vo_[2]);
                for (int _c = 464; _c <= 467; _c++)
                    fprintf(f, "  c[%d]=(%.3f,%.3f,%.3f,%.3f)\n", _c,
                            state->vertex_constants[_c][0], state->vertex_constants[_c][1],
                            state->vertex_constants[_c][2], state->vertex_constants[_c][3]);
                for (u32 k = 0; k < 4; k++)
                    fprintf(f, "  v%u i%u a0=(%.3f,%.3f,%.3f,%.3f) a8=(%.3f,%.3f)\n", k,
                        read_guest_index(state, first + q*4 + k),
                        c[k][0].v[0],c[k][0].v[1],c[k][0].v[2],c[k][0].v[3],
                        c[k][8].v[0],c[k][8].v[1]);
                fclose(f);
            } } }
    }
    s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
    return o;
}

static u32 upload_tris_vp_indexed(const rsx_state* state, u32 first, u32 count)
{
    extern uint8_t* vm_base;
    s_req_verts += count; s_req_draws++;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    vp_attrs_dbg(state);
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (count > maxv) { s_drop_draws++;
        { static int _n = 0; if (getenv("VBFULL") && _n++ < 12)
            fprintf(stderr, "[VBFULL] fp=0x%X wanted %u verts, room for %u%c",
                    state->shader_program, count, maxv, 10); }
        count = maxv - (maxv % 3); }
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    { double _tv = perf_on() ? perf_now() : 0.0;
      for (u32 k = 0; k < count; k++)
          read_vp_vertex(state, read_guest_index(state, first + k), &out[k*16]);
      if (perf_on()) { s_perf_vtx += perf_now() - _tv; s_perf_nverts += count; } }
    /* IDXDBG=<hex shader_program>: index range for that program's draws. An
     * index past the vertex array reads unmapped guest memory as zero, which
     * shows up as a spike triangle with a zero texcoord, not as a missing draw. */
    { static const char* ie = (const char*)1; static u32 iw = 0;
      if (ie == (const char*)1) { ie = getenv("IDXDBG");
          iw = ie ? (u32)strtoul(ie, NULL, 16) : 0; }
      if (iw && state->shader_program == iw) { static int _n = 0; if (_n++ < 12) {
          u32 lo = 0xFFFFFFFFu, hi = 0;
          for (u32 k = 0; k < count; k++) { u32 ix = read_guest_index(state, first + k);
              if (ix < lo) lo = ix; if (ix > hi) hi = ix; }
          fprintf(stderr, "[IDXDBG] first=%u count=%u idx=[%u..%u] dma=0x%X a0off=0x%X stride=%u%c",
                  first, count, lo, hi, state->index_array_dma,
                  state->vertex_attribs[0].offset, state->vertex_attribs[0].stride, 10); } } }
    /* DBG_LOCK tracking: projected centroid of this mesh, updated every draw. */
    { static const char* dl = (const char*)1; static u32 wantl = 0; static int mvpb = 256;
      if (dl == (const char*)1) { dl = getenv("DUCK_VTX");
                                  wantl = dl ? (u32)strtoul(dl, NULL, 16) : 0;
                                  const char* mb = getenv("MVP_BASE");
                                  if (mb) mvpb = atoi(mb); }
      /* DUCKTRACK=1 tracks the content-identified duck without needing its
       * offset passed in (VRAM offsets move between runs). Re-read every call:
       * s_duck_raw is filled by the texture upload, which happens AFTER the
       * first draws, so caching it once left the tracker watching offset 0. */
      { static int dtrk = -1;
        if (dtrk < 0) { const char* e = getenv("DUCKTRACK"); dtrk = e ? atoi(e) : 0; }
        if (dtrk && s_duck_raw) wantl = s_duck_raw; }
      /* DUCK_PICK=<first>: track ONLY the draw with that starting index. The
       * mesh is one big vertex buffer sliced into 256-index chunks, so averaging
       * across all of them converges on the centre of the whole field -- which is
       * where there is nothing in particular. A single slice is a stable target
       * and can be magnified without drifting off it. */
      static int pick = -1;
      if (pick < 0) { const char* e = getenv("DUCK_PICK"); pick = e ? atoi(e) : -2; }
      if (wantl && s_d3d.cur_texs[0].raw == wantl && count
          && (pick == -2 || (int)first == pick)) {
          double sx = 0, sy = 0, sz = 0;
          for (u32 k = 0; k < count; k++) {
              sx += out[k*16].v[0]; sy += out[k*16].v[1]; sz += out[k*16].v[2];
          }
          float v[4] = { (float)(sx/count), (float)(sy/count), (float)(sz/count), 1.0f };
          float clip[4];
          for (int r = 0; r < 4; r++) {
              const float* m = state->vertex_constants[mvpb + r];
              clip[r] = m[0]*v[0] + m[1]*v[1] + m[2]*v[2] + m[3]*v[3];
          }
          if (clip[3] > 1e-6f) {
              float nx = clip[0]/clip[3], ny = clip[1]/clip[3];
              if (nx > -2.0f && nx < 2.0f && ny > -2.0f && ny < 2.0f) {
                  s_lock_sx += nx; s_lock_sy += ny; s_lock_n++;
                  if (!s_lock_valid) {        /* seed so frame 1 is usable */
                      s_lock_x = nx; s_lock_y = ny; s_lock_valid = 1;
                  }
              }
          }
      } }
    /* DUCK_SCALE=<f>: multiply attribute-0 positions for the draws binding
     * DUCK_VTX's texture. The duck's object-space bbox is ~0.2 units and its
     * draws rasterize only a few dozen pixels per frame -- sub-pixel. Growing it
     * about its own origin tests the rest of the chain (index buffer, texture,
     * per-draw transform): if a duck-shaped, duck-coloured object appears, only
     * the scale is wrong. Diagnostic. */
    { static const char* ds = (const char*)1; static float sc = 0.0f;
      if (ds == (const char*)1) { ds = getenv("DUCK_SCALE");
                                  sc = ds ? (float)atof(ds) : 0.0f; }
      static const char* dv2 = (const char*)1; static u32 want2 = 0;
      if (dv2 == (const char*)1) { dv2 = getenv("DUCK_VTX");
                                   want2 = dv2 ? (u32)strtoul(dv2, NULL, 16) : 0; }
      if (sc > 0.0f && want2 && s_d3d.cur_texs[0].raw == want2 && count) {
        /* Magnify about a FIXED point captured from the first tracked draw, not
         * about each chunk's own centre -- recentring per chunk would stack every
         * chunk of the mesh on top of the others. Combined with VP_BYPASS (which
         * maps attribute 0 straight to clip space) this renders the mesh's real
         * geometry and texture at a readable size. */
        /* DUCK_RECENTER=1: recentre EVERY chunk on its own centroid, overlaying
         * them. If each chunk is one instance of the same mesh they align into a
         * single silhouette; if they are arbitrary slices of one big mesh they
         * smear. Either way the answer is visible at a glance. */
        static int recen = -1;
        if (recen < 0) { const char* e = getenv("DUCK_RECENTER"); recen = e ? atoi(e) : 0; }
        static int have_c = 0; static float ox = 0, oy = 0, oz = 0;
        if (!have_c || recen) {
            double sx = 0, sy = 0, sz = 0;
            for (u32 k = 0; k < count; k++) {
                sx += out[k*16].v[0]; sy += out[k*16].v[1]; sz += out[k*16].v[2];
            }
            ox = (float)(sx/count); oy = (float)(sy/count); oz = (float)(sz/count);
            have_c = 1;
        }
        for (u32 k = 0; k < count; k++) {
            /* DUCK_SHIFT="dx,dy": nudge after scaling. VP_BYPASS applies the
             * guest's posoffset, so a recentred mesh does not land at screen
             * centre -- without this the magnified geometry sits clipped against
             * the top edge. */
            static int shf = -1; static float dx = 0.0f, dy = 0.0f;
            if (shf < 0) { const char* e = getenv("DUCK_SHIFT");
                           if (e) { double a=0,b=0; sscanf(e, "%lf,%lf", &a, &b);
                                    dx = (float)a; dy = (float)b; }
                           shf = 1; }
            out[k*16].v[0] = (out[k*16].v[0] - ox) * sc + dx;
            out[k*16].v[1] = (out[k*16].v[1] - oy) * sc + dy;
            /* Leave z alone: under VP_BYPASS the object z maps straight to clip
             * z, so scaling it walks the geometry out of the depth range and
             * everything clips away. */
            (void)oz;
        }
      } }
    /* DUCK_VTX=<hex tex0 offset>: attribute-0 positions for the draws binding
     * that texture (see upload_tris_vp). The duck's meshes are indexed, so this
     * is the copy that actually fires for it. */
    { static const char* dv = (const char*)1; static u32 want = 0; static int n = 0;
      if (dv == (const char*)1) { dv = getenv("DUCK_VTX");
                                  want = dv ? (u32)strtoul(dv, NULL, 16) : 0; }
      if (want && s_d3d.cur_texs[0].raw == want && n < 8) { n++;
        float mnx=1e30f,mny=1e30f,mnz=1e30f,mxx=-1e30f,mxy=-1e30f,mxz=-1e30f;
        for (u32 k = 0; k < count; k++) {
            float x=out[k*16].v[0], y=out[k*16].v[1], z=out[k*16].v[2];
            if (x<mnx)mnx=x; if (y<mny)mny=y; if (z<mnz)mnz=z;
            if (x>mxx)mxx=x; if (y>mxy)mxy=y; if (z>mxz)mxz=z;
        }
        const rsx_vertex_attrib* _a3 = &state->vertex_attribs[3];
        fprintf(stderr, "[DUCKVTX] a3(en=%d type=%u size=%u stride=%u off=0x%08X freq=%u)"
                        " -> col0 v0=(%.3f %.3f %.3f %.3f)%c",
                _a3->enabled, _a3->type, _a3->size, _a3->stride, _a3->offset,
                _a3->frequency,
                out[3].v[0], out[3].v[1], out[3].v[2], out[3].v[3], 10);
        const rsx_vertex_attrib* _a0 = &state->vertex_attribs[0];
        fprintf(stderr, "[DUCKVTX] first=%u count=%u a0(type=%u size=%u stride=%u off=0x%08X)"
                        " obj-bbox x[%.3f,%.3f] y[%.3f,%.3f] z[%.3f,%.3f]%c",
                first, count, _a0->type, _a0->size, _a0->stride, _a0->offset,
                mnx,mxx, mny,mxy, mnz,mxz, 10);
        /* Project the object-space bbox corners through the first four transform
         * constants (the conventional MVP rows) and report NDC. Sub-pixel output
         * with correct vertices means the placement transform is the problem, and
         * the NDC extent says by how much. */
        { float cx=(mnx+mxx)*0.5f, cy=(mny+mxy)*0.5f, cz=(mnz+mxz)*0.5f;
          /* This title's Cg programs keep their MVP at c[256..259], not c[0..3]
           * (see the vp_uses_c03 note); projecting through c0 reported zeros. */
          static int MVP = -1;
          if (MVP < 0) { const char* e = getenv("MVP_BASE"); MVP = e ? atoi(e) : 256; }
          float v[4] = { cx, cy, cz, 1.0f };
          float clip[4];
          for (int r = 0; r < 4; r++) {
              const float* m = state->vertex_constants[MVP + r];
              clip[r] = m[0]*v[0] + m[1]*v[1] + m[2]*v[2] + m[3]*v[3];
          }
          float ex[4];
          { float e[4] = { mxx-cx, mxy-cy, mxz-cz, 0.0f };
            for (int r = 0; r < 4; r++) {
                const float* m = state->vertex_constants[MVP + r];
                ex[r] = m[0]*e[0] + m[1]*e[1] + m[2]*e[2];
            } }
          { int lo = -1, hi = -1, nz = 0;
            for (int r = 0; r < RSX_MAX_VERTEX_CONSTANTS; r++) {
                const float* m = state->vertex_constants[r];
                if (m[0] || m[1] || m[2] || m[3]) { if (lo < 0) lo = r; hi = r; nz++; }
            }
            fprintf(stderr, "[DUCKVTX]   constants: %d non-zero, slots %d..%d;", nz, lo, hi);
            for (int r = (lo < 0 ? 0 : lo); r < (lo < 0 ? 0 : lo) + 4 && r < RSX_MAX_VERTEX_CONSTANTS; r++)
                fprintf(stderr, " c%d=(%.3f %.3f %.3f %.3f)", r,
                        state->vertex_constants[r][0], state->vertex_constants[r][1],
                        state->vertex_constants[r][2], state->vertex_constants[r][3]);
            fprintf(stderr, "%c", 10); }
          fprintf(stderr, "[DUCKVTX]   mvp0=(%.3f %.3f %.3f %.3f) clip=(%.3f %.3f %.3f w=%.3f)",
                  state->vertex_constants[MVP][0], state->vertex_constants[MVP][1],
                  state->vertex_constants[MVP][2], state->vertex_constants[MVP][3],
                  clip[0], clip[1], clip[2], clip[3]);
          if (clip[3] > 1e-6f)
              fprintf(stderr, " ndc=(%.4f %.4f) half-extent-px=(%.2f %.2f)",
                      clip[0]/clip[3], clip[1]/clip[3],
                      ex[0]/clip[3]*640.0f, ex[1]/clip[3]*360.0f);
          fprintf(stderr, "%c", 10); }
      } }
    /* IDX_DBG=<N>: the first indices and the position they resolve to. An index
     * buffer read with the wrong element size or base yields huge indices and
     * degenerate triangles -- an INDEXED mesh vanishes while non-indexed
     * geometry in the same scene renders fine. */
    { static int cap = -1, n = 0;
      if (cap < 0) { const char* e = getenv("IDX_DBG"); cap = e ? atoi(e) : 0; }
      if (cap && n < cap && count >= 3) { n++;
        extern u32 cellGcmResolveOffset(u32);
        const rsx_vertex_attrib* a0 = &state->vertex_attribs[0];
        u32 vbase = cellGcmResolveOffset(a0->offset & 0x7FFFFFFFu);
        u32 nz = 0; for (u32 q = 0; q < 0x1000u; q += 29) if (vm_base[vbase + q]) nz++;
        fprintf(stderr, "[IDX] idxoff=0x%X idx: %u %u %u | a0 off=0x%08X(->0x%08X nz=%u/142)"
                        " stride=%u size=%u type=%u  pos0=(%g %g %g)%c",
                state->index_array_offset,
                read_guest_index(state, first), read_guest_index(state, first+1),
                read_guest_index(state, first+2),
                a0->offset, vbase, nz, a0->stride, a0->size, a0->type,
                out[0].v[0], out[0].v[1], out[0].v[2], 10); } }
    s_d3d.vp_vb_offset += count * VP_VERT_STRIDE;
    return count;
}

/* Indexed TRIANGLE_STRIP / TRIANGLE_FAN, expanded CPU-side into a triangle
 * list (the whole VP path draws lists; CullMode is NONE so strip parity
 * winding doesn't matter). DeferredShading's spline tube meshes -- the
 * octopus tentacles -- are indexed strips. */
static u32 upload_strip_vp_indexed(const rsx_state* state, u32 first, u32 count, int fan)
{
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    if (count < 3) return 0;
    u32 tris = count - 2;
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (tris * 3 > maxv) tris = maxv / 3;
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    u32 o = 0;
    for (u32 t = 0; t < tris; t++) {
        u32 i0 = fan ? 0 : t;
        read_vp_vertex(state, read_guest_index(state, first + i0),    &out[o*16]); o++;
        read_vp_vertex(state, read_guest_index(state, first + t + 1), &out[o*16]); o++;
        read_vp_vertex(state, read_guest_index(state, first + t + 2), &out[o*16]); o++;
    }
    s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
    return o;
}

static void d3d12_draw_arrays(void* ud, u32 primitive, u32 first, u32 count)
{
    (void)ud;
    /* Log the first 20 calls in detail, then every 1000th to show liveness
     * without flooding. */
    static u64 s_total = 0;
    if (s_total < 20 || (s_total % 1000) == 0) {
        printf("[D3D12] draw_arrays #%llu prim=%u first=%u count=%u\n",
               (unsigned long long)s_total, primitive, first, count);
    }
    s_total++;

    if (!s_d3d.pipeline_ready || !s_d3d.vb_mapped) return;
    if (count == 0 || count > MAX_VERTICES) return;

    /* QUADS (prim 8) have no D3D12 topology; expand to a triangle list.
     * dbgfont draws all text as quads. Prefer the real vertex-program path
     * (exact position + texcoord): upload raw float4 attrib0 into vp_vb and
     * mark the draw is_vp; render_frame compiles the VP and draws it. Fall
     * back to the frac-approximation textured path if VP resources are absent. */
    if (primitive == 8 /* CELL_GCM_PRIMITIVE_QUADS */) {
        /* Prefer the real vertex-program path whenever VP resources exist --
         * NOT only when a texture is bound. Requiring tex_bound routed vkcube's
         * UNtextured cube to the fixed-function fallback, which never applies the
         * VP's MVP transform, so the cube was drawn in object space and clipped
         * off-screen. Untextured VP draws now render via the colour PSO. */
        if (s_d3d.vp_vb_mapped && s_d3d.vp_root_sig) {
            u32 rec = s_d3d.vp_vb_offset;
            u32 emitted = upload_quads_vp(s_d3d.current_rsx_state, first, count);
            if (emitted && s_d3d.draw_count < MAX_DRAWS) {
                D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count];
                dr->vb_byte_offset = rec;
                dr->vertex_count   = emitted;
                dr->topology       = D3D_TOPOLOGY_TRIANGLELIST;
                dr->textured       = s_d3d.tex_bound;
                dr->is_vp          = 1;
                dr->fp_addr = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->shader_program : 0;
                dr->fp_exp32 = s_d3d.current_rsx_state ?
                    ((s_d3d.current_rsx_state->shader_control & 0x40) != 0) : 1;
                dr->cull = rsx_cull_key(s_d3d.current_rsx_state);
        dr->begin_epoch = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
            dr->begin_epoch = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
                dr->cmask = 0xF;
                if (s_d3d.current_rsx_state) {
                    u32 _cm = s_d3d.current_rsx_state->color_mask;
                    dr->cmask = ((_cm & 0x00010000) ? 1u : 0u)   /* R */
                              | ((_cm & 0x00000100) ? 2u : 0u)   /* G */
                              | ((_cm & 0x00000001) ? 4u : 0u)   /* B */
                              | ((_cm & 0x01000000) ? 8u : 0u);  /* A */
                }
                dr->alpha_ctl = 0;
                if (s_d3d.current_rsx_state)
                    dr->alpha_ctl = ((s_d3d.current_rsx_state->alpha_test_enable ? 1u : 0u) << 16)
                                  | ((s_d3d.current_rsx_state->alpha_func & 0xFFu) << 8)
                                  | (s_d3d.current_rsx_state->alpha_ref & 0xFFu);
                for (int _u = 0; _u < 4; _u++) {
                    dr->tex[_u].off = s_d3d.cur_texs[_u].off;
                    dr->tex[_u].raw = s_d3d.cur_texs[_u].raw;
                    dr->tex[_u].w   = s_d3d.cur_texs[_u].w;
                    dr->tex[_u].h   = s_d3d.cur_texs[_u].h;
                    dr->tex[_u].fmt = s_d3d.cur_texs[_u].fmt;
                    dr->tex[_u].ctrl1 = s_d3d.cur_texs[_u].ctrl1;
                    dr->tex[_u].cube  = s_d3d.cur_texs[_u].cube;
                    dr->tex[_u].mips  = s_d3d.cur_texs[_u].mips;
                    dr->tex[_u].set = s_d3d.cur_texs[_u].set;
                    dr->tex_rt[_u]  = -1;
                }
                dr->tex_slot = -1;
                dr->vs_idx = vp_get_vs(s_d3d.current_rsx_state);
                dr->is_clear = 0;
                dr->blend = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_enable : 1;
                dr->blend_key = rsx_blend_key(s_d3d.current_rsx_state, dr->blend);
                if (getenv("BLENDDBG")) { static u32 seen[32]; static int ns=0;
                    u32 k = (dr->blend ? 0x80000000u : 0u) | dr->blend_key;
                    int f=0; for (int i=0;i<ns;i++) if (seen[i]==k) f=1;
                    if (!f && ns<32) { seen[ns++]=k;
                        fprintf(stderr, "[BLEND] enable=%d key=0x%X%c",
                                dr->blend, dr->blend_key, 10); } }
                dr->rt_off = current_rt_off(&dr->rt_w, &dr->rt_h, dr->rt_mrt);
                dr->rt_fmt = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_format : 0;
                if (s_d3d.current_rsx_state) {
                    dr->vp_x = s_d3d.current_rsx_state->viewport_x;
                    dr->vp_y = s_d3d.current_rsx_state->viewport_y;
                    dr->vp_w = s_d3d.current_rsx_state->viewport_w;
                    dr->vp_h = s_d3d.current_rsx_state->viewport_h;
                } else { dr->vp_x = dr->vp_y = dr->vp_w = dr->vp_h = 0; }
                dr->cb_slot = s_d3d.draw_count;
                vp_record_cb(s_d3d.draw_count, dr->vs_idx, dr);
                s_d3d.draw_count++;
            }
            return;
        }
        u32 record_offset = s_d3d.vb_offset;
        u32 emitted = upload_quads_from_rsx(first, count);
        if (emitted == 0) return;
        if (s_d3d.draw_count < MAX_DRAWS) {
            s_d3d.draws[s_d3d.draw_count].vb_byte_offset = record_offset;
            s_d3d.draws[s_d3d.draw_count].vertex_count   = emitted;
            s_d3d.draws[s_d3d.draw_count].topology       = D3D_TOPOLOGY_TRIANGLELIST;
            s_d3d.draws[s_d3d.draw_count].textured       = s_d3d.tex_bound;
            s_d3d.draws[s_d3d.draw_count].is_vp          = 0;
            s_d3d.draws[s_d3d.draw_count].is_clear       = 0;
            s_d3d.draws[s_d3d.draw_count].rt_off         = 0;
            s_d3d.draw_count++;
        }
        return;
    }

    /* TRIANGLES (prim 5): same VP-path preference as quads -- the guest's
     * vertex program does the MVP transform (gcm/cube draws its cube this way);
     * the fixed-function fallback below applies no transform, so 3D geometry
     * ends up in object space (invisible/garbage). */
    /* Route TRIANGLES/STRIP/FAN through the VP path (sagemono: LBP draws
     * non-indexed strips/fans), but ONLY when the guest ACTUALLY HAS a vertex
     * program (flOw). The VP path transforms via the guest's VP microcode; with
     * no microcode loaded it transforms nothing and the draw silently produces
     * zero pixels (it also uploads to vp_vb, leaving the fixed-function vb empty).
     * Geometry already in clip space with no VP -- flOw's injected scene -- must
     * go down the fixed-function passthrough below instead. */
    if ((primitive == 5 /* TRIANGLES */ || primitive == 6 /* TRIANGLE_STRIP */
         || primitive == 7 /* TRIANGLE_FAN */) &&
        s_d3d.vp_vb_mapped && s_d3d.vp_root_sig &&
        s_d3d.current_rsx_state && s_d3d.current_rsx_state->vp_ucode_bytes >= 16) {
        u32 rec = s_d3d.vp_vb_offset;
        u32 emitted = (primitive == 5)
            ? upload_tris_vp(s_d3d.current_rsx_state, first, count)
            : upload_strip_vp(s_d3d.current_rsx_state, first, count, primitive == 7);
        /* RSX splits one primitive stream across several DRAW_ARRAYS entries
         * inside a single BEGIN/END (each carries at most 256 vertices), and the
         * hardware concatenates them before assembling primitives. Recording a
         * draw per batch instead regroups the triangles: this title's tub mesh
         * arrives as 128 then 256,256,..., neither a multiple of 3, so every
         * batch after the first was assembled one vertex out of phase -- which
         * rendered as spike triangles with a zero texcoord along the tub rim.
         * The batches land contiguously in the vertex buffer, so extending the
         * previous record is all that is needed to restore the stream. */
        if (emitted && s_d3d.merge_prev_draw && s_d3d.draw_count > 0 &&
            s_d3d.merge_first_end == first) {
            D3D12DrawRecord* pv = &s_d3d.draws[s_d3d.draw_count - 1];
            u32 fpnow = s_d3d.current_rsx_state
                        ? s_d3d.current_rsx_state->shader_program : 0;
            if (pv->is_vp && pv->topology == D3D_TOPOLOGY_TRIANGLELIST &&
                pv->fp_addr == fpnow && primitive == 5 &&
                pv->begin_epoch == (s_d3d.current_rsx_state
                                    ? s_d3d.current_rsx_state->begin_epoch : 0) &&
                pv->vb_byte_offset + pv->vertex_count * VP_VERT_STRIDE == rec) {
                pv->vertex_count += emitted;
                s_d3d.merge_first_end = first + count;
                return;
            }
        }
        if (emitted && s_d3d.draw_count < MAX_DRAWS) {
            D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count];
            dr->vb_byte_offset = rec;
            dr->vertex_count   = emitted;
            dr->topology       = D3D_TOPOLOGY_TRIANGLELIST;
            dr->textured       = s_d3d.tex_bound;
            dr->is_vp          = 1;
            dr->fp_addr = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->shader_program : 0;
            dr->fp_exp32 = s_d3d.current_rsx_state ?
                ((s_d3d.current_rsx_state->shader_control & 0x40) != 0) : 1;
            dr->cull = rsx_cull_key(s_d3d.current_rsx_state);
        dr->begin_epoch = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
            dr->begin_epoch = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
            dr->cmask = 0xF;
            if (s_d3d.current_rsx_state) {
                u32 _cm = s_d3d.current_rsx_state->color_mask;
                dr->cmask = ((_cm & 0x00010000) ? 1u : 0u)   /* R */
                          | ((_cm & 0x00000100) ? 2u : 0u)   /* G */
                          | ((_cm & 0x00000001) ? 4u : 0u)   /* B */
                          | ((_cm & 0x01000000) ? 8u : 0u);  /* A */
            }
            dr->alpha_ctl = 0;
            if (s_d3d.current_rsx_state)
                dr->alpha_ctl = ((s_d3d.current_rsx_state->alpha_test_enable ? 1u : 0u) << 16)
                              | ((s_d3d.current_rsx_state->alpha_func & 0xFFu) << 8)
                              | (s_d3d.current_rsx_state->alpha_ref & 0xFFu);
            for (int _u = 0; _u < 4; _u++) {
                dr->tex[_u].off = s_d3d.cur_texs[_u].off;
                dr->tex[_u].raw = s_d3d.cur_texs[_u].raw;
                dr->tex[_u].w   = s_d3d.cur_texs[_u].w;
                dr->tex[_u].h   = s_d3d.cur_texs[_u].h;
                dr->tex[_u].fmt = s_d3d.cur_texs[_u].fmt;
                    dr->tex[_u].ctrl1 = s_d3d.cur_texs[_u].ctrl1;
                    dr->tex[_u].cube  = s_d3d.cur_texs[_u].cube;
                    dr->tex[_u].mips  = s_d3d.cur_texs[_u].mips;
                dr->tex[_u].set = s_d3d.cur_texs[_u].set;
                dr->tex_rt[_u]  = -1;
            }
            dr->tex_slot = -1;
            dr->vs_idx = vp_get_vs(s_d3d.current_rsx_state);
            dr->is_clear = 0;
            dr->blend = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_enable : 1;
            dr->blend_key = rsx_blend_key(s_d3d.current_rsx_state, dr->blend);
            dr->rt_off = current_rt_off(&dr->rt_w, &dr->rt_h, dr->rt_mrt);
            dr->rt_fmt = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_format : 0;
            if (s_d3d.current_rsx_state) {
                dr->vp_x = s_d3d.current_rsx_state->viewport_x;
                dr->vp_y = s_d3d.current_rsx_state->viewport_y;
                dr->vp_w = s_d3d.current_rsx_state->viewport_w;
                dr->vp_h = s_d3d.current_rsx_state->viewport_h;
            } else { dr->vp_x = dr->vp_y = dr->vp_w = dr->vp_h = 0; }
            dr->cb_slot = s_d3d.draw_count;
                vp_record_cb(s_d3d.draw_count, dr->vs_idx, dr);
            s_d3d.draw_count++;
            /* Anchor for merging the next DRAW_ARRAYS batch of this stream. */
            s_d3d.merge_prev_draw  = (primitive == 5);
            s_d3d.merge_first_end  = first + count;
        }
        return;
    }
    s_d3d.merge_prev_draw = 0;   /* any other primitive breaks the stream */

    u32 topo = rsx_to_d3d12_topology(primitive);
    if (topo == D3D_TOPOLOGY_UNDEFINED) {
        /* Other primitives still needing index-buffer conversion
         * (line loops, triangle fans) — skip rather than draw wrong. */
        static int s_skipped_nontri = 0;
        if (s_skipped_nontri < 3) {
            printf("[D3D12] draw_arrays: skipping prim=%u (needs index conversion)\n",
                   primitive);
            s_skipped_nontri++;
        }
        return;
    }

    u32 record_offset = s_d3d.vb_offset;
    u32 actual_count  = upload_vertices_from_rsx(first, count);
    if (actual_count == 0) return;

    if (s_d3d.draw_count < MAX_DRAWS) {
        s_d3d.draws[s_d3d.draw_count].vb_byte_offset = record_offset;
        s_d3d.draws[s_d3d.draw_count].vertex_count   = actual_count;
        s_d3d.draws[s_d3d.draw_count].topology       = topo;
        s_d3d.draws[s_d3d.draw_count].textured       = 0;
        s_d3d.draws[s_d3d.draw_count].is_vp          = 0;
        s_d3d.draws[s_d3d.draw_count].is_clear       = 0;
        s_d3d.draws[s_d3d.draw_count].rt_off         = 0;
        s_d3d.draw_count++;
    }
}

static void d3d12_draw_indexed(void* ud, u32 primitive, u32 first, u32 count)
{
    (void)ud;
    static int log_count = 0;
    if (log_count < 8) {
        printf("[D3D12] draw_indexed(prim=%u, first=%u, count=%u)\n",
               primitive, first, count);
        log_count++;
    }
    if (!s_d3d.pipeline_ready) return;
    if (count == 0 || count > MAX_VERTICES) return;
    if (!s_d3d.vp_vb_mapped || !s_d3d.vp_root_sig) return;

    /* Expand through the VP path (indices resolved CPU-side): QUADS -> two
     * triangles per quad, TRIANGLES straight through, STRIP/FAN -> triangle
     * list. Other primitives are skipped rather than drawn wrong. */
    u32 emitted = 0;
    u32 rec = s_d3d.vp_vb_offset;
    if (primitive == 8)      emitted = upload_quads_vp_indexed(s_d3d.current_rsx_state, first, count);
    else if (primitive == 5) emitted = upload_tris_vp_indexed(s_d3d.current_rsx_state, first, count);
    else if (primitive == 6) emitted = upload_strip_vp_indexed(s_d3d.current_rsx_state, first, count, 0);
    else if (primitive == 7) emitted = upload_strip_vp_indexed(s_d3d.current_rsx_state, first, count, 1);
    else {
        static int _skip = 0;
        if (_skip++ < 3)
            printf("[D3D12] draw_indexed: skipping prim=%u (not wired)\n", primitive);
        return;
    }
    /* Same BEGIN/END batch concatenation as the non-indexed path: this title's
     * fluid arrives as ~3000 DRAW_INDEX_ARRAY batches of 256, and 256 is not a
     * multiple of 3, so every batch after the first assembled out of phase. */
    if (emitted && s_d3d.merge_prev_draw && s_d3d.draw_count > 0 &&
        s_d3d.merge_first_end == first && primitive == 5) {
        D3D12DrawRecord* pv = &s_d3d.draws[s_d3d.draw_count - 1];
        u32 fpnow = s_d3d.current_rsx_state
                    ? s_d3d.current_rsx_state->shader_program : 0;
        if (pv->is_vp && pv->topology == D3D_TOPOLOGY_TRIANGLELIST &&
            pv->fp_addr == fpnow &&
            pv->begin_epoch == (s_d3d.current_rsx_state
                                ? s_d3d.current_rsx_state->begin_epoch : 0) &&
            pv->vb_byte_offset + pv->vertex_count * VP_VERT_STRIDE == rec) {
            pv->vertex_count += emitted;
            s_d3d.merge_first_end = first + count;
            return;
        }
    }
    if (emitted && s_d3d.draw_count < MAX_DRAWS) {
        D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count];
        dr->vb_byte_offset = rec;
        dr->vertex_count   = emitted;
        dr->topology       = D3D_TOPOLOGY_TRIANGLELIST;
        dr->textured       = s_d3d.tex_bound;
        dr->is_vp          = 1;
        dr->fp_addr = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->shader_program : 0;
        dr->fp_exp32 = s_d3d.current_rsx_state ?
            ((s_d3d.current_rsx_state->shader_control & 0x40) != 0) : 1;
        dr->cull = rsx_cull_key(s_d3d.current_rsx_state);
        dr->begin_epoch = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
        dr->cmask = 0xF;
        if (s_d3d.current_rsx_state) {
            u32 _cm = s_d3d.current_rsx_state->color_mask;
            dr->cmask = ((_cm & 0x00010000) ? 1u : 0u)   /* R */
                      | ((_cm & 0x00000100) ? 2u : 0u)   /* G */
                      | ((_cm & 0x00000001) ? 4u : 0u)   /* B */
                      | ((_cm & 0x01000000) ? 8u : 0u);  /* A */
        }
        dr->alpha_ctl = 0;
        if (s_d3d.current_rsx_state)
            dr->alpha_ctl = ((s_d3d.current_rsx_state->alpha_test_enable ? 1u : 0u) << 16)
                          | ((s_d3d.current_rsx_state->alpha_func & 0xFFu) << 8)
                          | (s_d3d.current_rsx_state->alpha_ref & 0xFFu);
        for (int _u = 0; _u < 4; _u++) {
            dr->tex[_u].off = s_d3d.cur_texs[_u].off;
            dr->tex[_u].raw = s_d3d.cur_texs[_u].raw;
            dr->tex[_u].w   = s_d3d.cur_texs[_u].w;
            dr->tex[_u].h   = s_d3d.cur_texs[_u].h;
            dr->tex[_u].fmt = s_d3d.cur_texs[_u].fmt;
                    dr->tex[_u].ctrl1 = s_d3d.cur_texs[_u].ctrl1;
                    dr->tex[_u].cube  = s_d3d.cur_texs[_u].cube;
                    dr->tex[_u].mips  = s_d3d.cur_texs[_u].mips;
            dr->tex[_u].set = s_d3d.cur_texs[_u].set;
            dr->tex_rt[_u]  = -1;
        }
        dr->tex_slot = -1;
        dr->vs_idx = vp_get_vs(s_d3d.current_rsx_state);
        dr->is_clear = 0;
        dr->blend = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_enable : 1;
        dr->blend_key = rsx_blend_key(s_d3d.current_rsx_state, dr->blend);
        dr->rt_off = current_rt_off(&dr->rt_w, &dr->rt_h, dr->rt_mrt);
        dr->rt_fmt = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_format : 0;
        if (s_d3d.current_rsx_state) {
            dr->vp_x = s_d3d.current_rsx_state->viewport_x;
            dr->vp_y = s_d3d.current_rsx_state->viewport_y;
            dr->vp_w = s_d3d.current_rsx_state->viewport_w;
            dr->vp_h = s_d3d.current_rsx_state->viewport_h;
        } else { dr->vp_x = dr->vp_y = dr->vp_w = dr->vp_h = 0; }
        dr->cb_slot = s_d3d.draw_count;
                vp_record_cb(s_d3d.draw_count, dr->vs_idx, dr);
        s_d3d.draw_count++;
        /* Anchor for merging the next batch of this indexed stream. */
        s_d3d.merge_prev_draw = (primitive == 5);
        s_d3d.merge_first_end = first + count;
    }
}

static void d3d12_bind_texture(void* ud, u32 unit, const rsx_texture_state* tex)
{
    (void)ud;
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveOffset(u32);

    u32 width  = (tex->image_rect >> 16) & 0xFFFF;
    u32 height = tex->image_rect & 0xFFFF;
    u32 format = (tex->format >> 8) & 0xFF;
    u32 offset = tex->offset;
    /* CUBEDBG=1: NV4097_SET_TEXTURE_FORMAT bit 2 is the cubemap flag, and the
     * line above throws it away with the rest of the low byte. Nothing in this
     * backend handles cube textures, so a cubemap bound for an environment
     * reflection is sampled as a plain 2D image -- which is what the chrome
     * faucet's black patches and banded escutcheons look like. */
    { static int cd = -1;
      if (cd < 0) { const char* e = getenv("CUBEDBG"); cd = e ? atoi(e) : 0; }
      if (cd && (tex->format & 4)) {
          static u32 seen[8]; static int ns = 0; int known = 0;
          for (int k = 0; k < ns; k++) if (seen[k] == offset) known = 1;
          if (!known && ns < 8) { seen[ns++] = offset;
              fprintf(stderr, "[CUBE] unit=%u offset=0x%08X %ux%u fmt=0x%02X"
                              " raw_format=0x%08X (cubemap bit SET)%c",
                      unit, offset, width, height, format, tex->format, 10); }
      } }

    static int log_count = 0;
    if (log_count < 10) {
        printf("[D3D12] bind_texture(unit=%u, offset=0x%X, fmt=0x%02X, %ux%u)\n",
               unit, offset, format, width, height);
        log_count++;
    }
    /* MOVIE_BIND=1: trace movie-plane binds (640x360 Y / 320x180 U/V) with the
     * resolved EA + a content probe -- used to diagnose the Bink frame-buffer
     * ring mismatch (the draw binds a cleared buffer 0x4D80 before the one the
     * decoder actually fills). */
    if (getenv("MOVIE_BIND") &&
        ((width == 640 && height == 360) || (width == 320 && height == 180))) {
        extern u32 cellGcmResolveLocated(int local, u32 offset);
        static int _mv = 0; if (_mv++ < 24) {
            /* Resolve the SAME raw offset both ways -- as LOCAL (VRAM) and as MAIN
             * (IO-mapped) -- and count nonzero bytes over the whole plane for each.
             * If the plane the game declares (loc bits fmt&3) is zero but the OTHER
             * pool has content, the decoder wrote to a different memory space than
             * our texture resolve picked (the local-vs-main mismatch). */
            u32 loc = tex->format & 3;               /* 1=LOCAL, 2=MAIN */
            u32 ea_loc  = cellGcmResolveLocated(1, offset);
            u32 ea_main = cellGcmResolveLocated(0, offset);
            u32 plane = width * height;
            u32 nz_loc = 0, nz_main = 0;
            if (vm_base) for (u32 i = 0; i < plane; i += 137) {
                if (vm_base[ea_loc  + i]) nz_loc++;
                if (vm_base[ea_main + i]) nz_main++;
            }
            fprintf(stderr, "[mv-bind] unit=%u fmt=0x%02X loc=%u %ux%u off=0x%X | LOCAL ea=0x%08X nz=%u | MAIN ea=0x%08X nz=%u\n",
                    unit, format, loc, width, height, offset,
                    ea_loc, nz_loc, ea_main, nz_main);
        }
    }

    if (!vm_base || width == 0 || height == 0) return;

    /* Record the currently-bound atlas so subsequent quad draws sample it. The
     * actual GPU upload happens in render_frame (we have no open command list
     * here). Only the 8-bit single-channel font atlas (B8, RSX fmt base 0x81 /
     * as-seen 0xA1 with the LN flag) is wired up so far; other formats fall
     * back to untextured. */
    u32 base_fmt = format & 0x9F;   /* strip LN(0x20)/UN(0x40) flags */
    /* VP path: record the latest bound texture (any supported format) so draws
     * can carry it per-draw. Location bits (format[1:0]): 1 = LOCAL, 2 = MAIN. */
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    if (unit < 4 &&
        (base_fmt == 0x81 /* B8 */ || base_fmt == 0x85 /* A8R8G8B8 */ ||
         base_fmt == 0x8B /* G8B8: LBP's font atlas */ ||
         (base_fmt >= 0x86 && base_fmt <= 0x88) /* DXT1/23/45 */ ||
         base_fmt == 0x9A /* W16Z16Y16X16 half-float: RTT intermediates */)) {
        /* TEX_RESOLVE_AUTO=1: resolve through the page tables (local-page map
         * then IO table) instead of trusting the format's location bits. A
         * texture the guest built in main memory but tagged local resolves to
         * untouched VRAM and samples as all-zero -- geometry renders, flat. */
        { static int _ra = -1; if (_ra < 0) _ra = getenv("TEX_RESOLVE_AUTO") ? 1 : 0;
          extern u32 cellGcmResolveIO(u32);
          u32 _r = 0;
          if (_ra) _r = cellGcmResolveIO(offset);          /* IO table first */
          if (!_r) _r = cellGcmResolveLocated((tex->format & 3) == 1, offset);
          s_d3d.cur_texs[unit].off = _r; }
        s_d3d.cur_texs[unit].raw = offset;
        s_d3d.cur_texs[unit].w = width; s_d3d.cur_texs[unit].h = height;
        s_d3d.cur_texs[unit].fmt = format;   /* full byte: LN(0x20)/UN(0x40) kept */
        s_d3d.cur_texs[unit].ctrl1 = tex->control1;
        s_d3d.cur_texs[unit].cube  = (tex->format & 4) ? 1 : 0;
        s_d3d.cur_texs[unit].mips  = (tex->format >> 16) & 0xFFFFu;
        if (getenv("TEXFMTDBG")) { static u32 seen[64]; static int ns=0;
            u32 k = (unit<<24) | (((tex->format>>8) & 0xFFu)<<16) | (width & 0xFFFFu);
            int f=0; for (int i=0;i<ns;i++) if (seen[i]==k) f=1;
            if (!f && ns < 64) { seen[ns++]=k;
                fprintf(stderr, "[TEXFMT] unit=%u fmt=0x%02X %ux%u mips=%u cube=%d off=0x%X%c",
                        unit, (tex->format >> 8) & 0xFFu, width, height,
                        (tex->format>>16)&0xFFFFu, (tex->format&4)?1:0, offset, 10); } }
        s_d3d.cur_texs[unit].set = 1;
    }
    if (base_fmt == 0x81 /* B8 */) {
        s_d3d.tex_src_offset = cellGcmResolveLocated((tex->format & 3) == 1, offset);
        if (s_d3d.tex_w != width || s_d3d.tex_h != height) {
            /* dims changed -> resource must be (re)created in render_frame */
            s_d3d.tex_ready = 0;
        }
        s_d3d.tex_w     = width;
        s_d3d.tex_h     = height;
        s_d3d.tex_bound = 1;
        s_d3d.tex_dirty = 1;
    } else {
        s_d3d.tex_bound = 0;
    }
}

static void d3d12_set_vertex_attribs(void* ud, const rsx_state* state)
{
    (void)ud;
    s_d3d.current_rsx_state = state;

    /* Log enabled vertex attributes for debugging. RSX_VTXDBG=<N> raises the
     * cap: the fixed 5 were all consumed by boot-time setup, so the layouts the
     * real draws use were never printed. */
    static int log_count = 0, log_cap = -1;
    if (log_cap < 0) { const char* e = getenv("RSX_VTXDBG"); log_cap = e ? atoi(e) : 5; }
    if (log_count < log_cap) {
        printf("[D3D12] set_vertex_attribs:\n");
        for (int i = 0; i < 16; i++) {
            const rsx_vertex_attrib* a = &state->vertex_attribs[i];
            if (a->enabled) {
                const char* type_name = "?";
                switch (a->type) {
                case 1: type_name = "snorm16"; break;
                case 2: type_name = "float32"; break;
                case 3: type_name = "float16"; break;
                case 4: type_name = "ubyte"; break;
                case 5: type_name = "s16"; break;
                case 7: type_name = "ubyte256"; break;
                }
                printf("  attrib[%d]: %s x%u, stride=%u, offset=0x%X\n",
                       i, type_name, a->size, a->stride, a->offset);
            }
        }
        log_count++;
    }
}

static void d3d12_set_shader(void* ud, const rsx_state* state)
{
    (void)ud;
    s_d3d.current_rsx_state = state;

    static int log_count = 0;
    if (log_count < 5) {
        printf("[D3D12] set_shader: fp_addr=0x%08X, vp_load=%u, output_mask=0x%08X\n",
               state->fragment_program_addr, state->transform_program_load,
               state->vertex_attrib_output_mask);
        log_count++;
    }

    /* TODO: Look up or compile a PSO matching this shader combination.
     * For now we use the basic vertex-colored PSO for everything. */
}

static void d3d12_set_blend(void* ud, const rsx_state* state)
{
    (void)ud;
    /* TODO: modify PSO blend state or use dynamic state.
     * D3D12 requires PSO recreation for blend state changes,
     * so we'd need a PSO cache keyed by blend configuration. */
    static int log_count = 0;
    if (log_count < 5) {
        printf("[D3D12] set_blend(enable=%d, sfactor=0x%X, dfactor=0x%X)\n",
               state->blend_enable, state->blend_sfactor, state->blend_dfactor);
        log_count++;
    }
}

static void d3d12_set_depth_stencil(void* ud, const rsx_state* state)
{
    (void)ud;
    static int log_count = 0;
    if (log_count < 5) {
        printf("[D3D12] set_depth_stencil(depth=%d, stencil=%d, func=0x%X)\n",
               state->depth_test_enable, state->stencil_test_enable,
               state->depth_func);
        log_count++;
    }
}

/* ---------------------------------------------------------------------------
 * Backend registration
 * -----------------------------------------------------------------------*/

static rsx_backend s_d3d12_backend = {0};

/* ---------------------------------------------------------------------------
 * Public API
 * -----------------------------------------------------------------------*/

int rsx_d3d12_backend_init(u32 width, u32 height, const char* title)
{
    memset(&s_d3d, 0, sizeof(s_d3d));
    s_d3d.width = width;
    s_d3d.height = height;
    s_d3d.clear_color[0] = 0.0f;
    s_d3d.clear_color[1] = 0.0f;
    s_d3d.clear_color[2] = 0.1f;
    s_d3d.clear_color[3] = 1.0f;

    /* Debug: dump the first N presented frames to BMP if CELLMARK_DUMP is set
     * (its numeric value when > 1, else 24). */
    {
        const char* dv = getenv("CELLMARK_DUMP");
        int n = dv ? atoi(dv) : 0;
        s_d3d.dump_frames_left = dv ? (n > 1 ? n : 24) : 0;
        /* CELLMARK_DUMP_SKIP=N: ignore the first N presents before dumping.
         * The opening frames are the loading screen; the interesting content
         * only appears once the sim SPUs have advanced the scene. */
        { const char* sv = getenv("CELLMARK_DUMP_SKIP");
          s_d3d.dump_skip_left = sv ? atoi(sv) : 0; }
    }

    /* Create window */
    {
        extern char g_rsx_title_base[128];
        snprintf(g_rsx_title_base, sizeof(g_rsx_title_base), "%s",
                 title ? title : "ps3recomp");
    }
    s_d3d.hwnd = create_window(width, height, title);
    if (!s_d3d.hwnd) {
        printf("[D3D12] ERROR: Window creation failed\n");
        return -1;
    }

    /* Initialize D3D12 */
    if (init_d3d12(width, height) != 0) {
        printf("[D3D12] ERROR: D3D12 initialization failed\n");
        return -1;
    }

    /* Set up backend callbacks */
    s_d3d12_backend.userdata          = &s_d3d;
    s_d3d12_backend.init              = d3d12_init;
    s_d3d12_backend.shutdown          = d3d12_shutdown;
    s_d3d12_backend.begin_frame       = d3d12_begin_frame;
    s_d3d12_backend.end_frame         = d3d12_end_frame;
    s_d3d12_backend.present           = d3d12_present;
    s_d3d12_backend.clear             = d3d12_clear;
    s_d3d12_backend.set_render_target = d3d12_set_render_target;
    s_d3d12_backend.set_viewport      = d3d12_set_viewport;
    s_d3d12_backend.set_blend         = d3d12_set_blend;
    s_d3d12_backend.set_depth_stencil = d3d12_set_depth_stencil;
    s_d3d12_backend.set_shader        = d3d12_set_shader;
    s_d3d12_backend.set_vertex_attribs = d3d12_set_vertex_attribs;
    s_d3d12_backend.draw_arrays       = d3d12_draw_arrays;
    s_d3d12_backend.draw_indexed      = d3d12_draw_indexed;
    s_d3d12_backend.bind_texture      = d3d12_bind_texture;

    rsx_set_backend(&s_d3d12_backend);

    s_d3d.initialized = 1;
    s_d3d.last_fps_time = GetTickCount64();

    printf("[D3D12] Backend ready: %ux%u\n", width, height);
    return 0;
}

void rsx_d3d12_backend_shutdown(void)
{
    if (!s_d3d.initialized) return;

    wait_for_gpu();

    /* Release D3D12 resources */
    if (s_d3d.vertex_buffer) {
        s_d3d.vertex_buffer->lpVtbl->Unmap(s_d3d.vertex_buffer, 0, NULL);
        s_d3d.vertex_buffer->lpVtbl->Release(s_d3d.vertex_buffer);
    }
    if (s_d3d.pipeline_state)        s_d3d.pipeline_state->lpVtbl->Release(s_d3d.pipeline_state);
    if (s_d3d.pipeline_state_lines)  s_d3d.pipeline_state_lines->lpVtbl->Release(s_d3d.pipeline_state_lines);
    if (s_d3d.pipeline_state_points) s_d3d.pipeline_state_points->lpVtbl->Release(s_d3d.pipeline_state_points);
    if (s_d3d.depth_buffer) s_d3d.depth_buffer->lpVtbl->Release(s_d3d.depth_buffer);
    if (s_d3d.dsv_heap)     s_d3d.dsv_heap->lpVtbl->Release(s_d3d.dsv_heap);
    if (s_d3d.root_signature) s_d3d.root_signature->lpVtbl->Release(s_d3d.root_signature);
    if (s_d3d.fence) s_d3d.fence->lpVtbl->Release(s_d3d.fence);
    if (s_d3d.fence_event) CloseHandle(s_d3d.fence_event);
    if (s_d3d.cmd_list) s_d3d.cmd_list->lpVtbl->Release(s_d3d.cmd_list);
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        if (s_d3d.cmd_allocators[i]) s_d3d.cmd_allocators[i]->lpVtbl->Release(s_d3d.cmd_allocators[i]);
        if (s_d3d.render_targets[i]) s_d3d.render_targets[i]->lpVtbl->Release(s_d3d.render_targets[i]);
    }
    if (s_d3d.rtv_heap) s_d3d.rtv_heap->lpVtbl->Release(s_d3d.rtv_heap);
    if (s_d3d.swap_chain) s_d3d.swap_chain->lpVtbl->Release(s_d3d.swap_chain);
    if (s_d3d.cmd_queue) s_d3d.cmd_queue->lpVtbl->Release(s_d3d.cmd_queue);
    if (s_d3d.device) s_d3d.device->lpVtbl->Release(s_d3d.device);

    if (s_d3d.hwnd) DestroyWindow(s_d3d.hwnd);

    rsx_set_backend(NULL);
    s_d3d.initialized = 0;

    printf("[D3D12] Backend shut down\n");
}

int rsx_d3d12_backend_pump_messages(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return -1;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return s_d3d.window_closed ? -1 : 0;
}

void rsx_d3d12_backend_present(void)
{
    if (blink_dbg())
        printf("[PRESENT] draws=%u clears_since_last=%u clear_presents=%u\n",
               s_d3d.draw_count, s_dbg_clears_since_present, s_clear_presents);
    s_dbg_clears_since_present = 0;

    /* Once frame-boundary presents are active (d3d12_clear presents each
     * completed frame as the drain crosses into the next one), the ticker
     * present would only ever show the partially-accumulated NEXT frame --
     * that partial present right after the FIFO ring recycle was the visible
     * blink. Keep the ticker present solely as the boot-time fallback (before
     * the first framed clear arrives). */
    if (s_clear_presents > 0)
        return;

    /* Same display gate as d3d12_present: a batch of offscreen pass work only
     * (render-to-texture) keeps accumulating until its composite arrives.
     * Empty batches present only until the first real frame -- after that an
     * empty present is a flip/drain race and wipes the screen for a frame
     * (wave: black flashes and layout flicker between frames). */
    static int s_seen_content = 0;
    int has_display = (s_d3d.draw_count == 0 && !s_seen_content);
    for (u32 _i = 0; _i < s_d3d.draw_count && _i < MAX_DRAWS; _i++)
        if (!s_d3d.draws[_i].is_clear && s_d3d.draws[_i].rt_off == 0) {
            has_display = 1;
            break;
        }
    if (has_display && s_d3d.draw_count > 0) s_seen_content = 1;

    /* VP_SUBMIT=<N>: has_display gates render_frame() entirely, so a batch whose
     * records all target an OFFSCREEN rt (rt_off != 0) presents without ever
     * running the draw pass -- the backbuffer then shows only the clear, which
     * reads as "nothing rasterizes" from every downstream check. */
    { static int cap = -1, n = 0;
      if (cap < 0) { const char* e = getenv("VP_SUBMIT"); cap = e ? atoi(e) : 0; }
      if (cap && n < cap && s_d3d.draw_count) { n++;
        u32 onscreen = 0, offscreen = 0, clears = 0;
        for (u32 _i = 0; _i < s_d3d.draw_count && _i < MAX_DRAWS; _i++) {
            if (s_d3d.draws[_i].is_clear) { clears++; continue; }
            if (s_d3d.draws[_i].rt_off) offscreen++; else onscreen++;
        }
        fprintf(stderr, "[PRESENTGATE] records=%u onscreen=%u offscreen=%u clears=%u"
                        " has_display=%d seen_content=%d -> render_frame=%s\n",
                s_d3d.draw_count, onscreen, offscreen, clears, has_display,
                s_seen_content, (s_d3d.initialized && has_display) ? "YES" : "SKIPPED"); } }

    /* Execute the batch whenever it has draws. Gating the whole call on
     * has_display meant a render-to-texture pass -- every draw targeting an
     * offscreen surface -- was DISCARDED rather than deferred, so the texture
     * it produces was never written and whatever sampled it later read an
     * empty resource. Only the Present needs onscreen content. */
    if (s_d3d.initialized && (has_display || s_d3d.draw_count > 0)) {
        { extern void rsx_reset_upload_claims(void);
          static int remap = -1;
          if (remap < 0) { const char* e = getenv("TEX_REMAP"); remap = e ? atoi(e) : 0; }
          if (remap) rsx_reset_upload_claims(); }
        s_present_this_frame = has_display;
        render_frame();
        s_present_this_frame = 1;
    }
}

#else /* !_WIN32 */

#include <ps3emu/ps3types.h>   /* u32 (header includes above are inside the _WIN32 guard) */
#include <stdio.h>

/* Stub for non-Windows — D3D12 is Windows-only */
int rsx_d3d12_backend_init(u32 w, u32 h, const char* t)
{
    (void)w; (void)h; (void)t;
    printf("[D3D12] Not available on this platform (use Vulkan backend)\n");
    return -1;
}
void rsx_d3d12_backend_shutdown(void) {}
int rsx_d3d12_backend_pump_messages(void) { return 0; }
void rsx_d3d12_backend_present(void) {}

#endif /* _WIN32 */
