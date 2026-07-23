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
#define MAX_VERTICES     65536  /* per-frame vertex buffer (dbgfont submits
                                 * ~7.5k verts/frame; leave generous headroom) */
#define MAX_DRAWS         1024  /* per-frame draw records */
/* Per-draw VP constant-buffer slot: vp_c[512] + posscale + posoffset
 * (514 vec4 = 8224 B) rounded up to D3D12's 256-byte CBV alignment.
 * Constants are snapshotted at RECORD time -- wave's passes each set their
 * own texScale/offset uniforms, so one per-frame snapshot ran every pass
 * with the LAST pass's constants. */
#define VP_CB_STRIDE      8448
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
    int used;           /* referenced this frame */
} VPTexSlot;
#define VP_TEX_SLOTS 4

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
    ID3D12PipelineState* pso;
} VPFPEntry;
#define VP_FP_CACHE 16

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
    struct { u32 off, raw, w, h, fmt, ctrl1; int set; } cur_texs[4];

    /* Render-to-texture: offscreen RT pool + their RTV heap. */
    OffRT                 off_rt[MAX_OFF_RTS];
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
        if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp,
                D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.vp_vb)))
            s_d3d.vp_vb->lpVtbl->Map(s_d3d.vp_vb, 0, &nr, &s_d3d.vp_vb_mapped);

        bd.Width = (u64)VP_CB_STRIDE * MAX_DRAWS * 2;   /* per-draw constant slots, x2 parity */
        if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp,
                D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.vp_cb)))
            s_d3d.vp_cb->lpVtbl->Map(s_d3d.vp_cb, 0, &nr, &s_d3d.vp_cb_mapped);

        bd.Width = (u64)256 * MAX_DRAWS * 2;   /* per-draw FP texscale slots (b1), x2 parity */
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
    snprintf(path, sizeof(path), "%s%sframe_%03d.bmp",
             dir ? dir : "", dir ? "/" : "", idx++);
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
    u32 hash = vp_hash_ucode(st->vp_ucode, st->vp_ucode_bytes);
    for (int i = 0; i < s_d3d.vp_vs_n; i++)
        if (s_d3d.vp_vs[i].hash == hash) return i;

    static char hlsl[262144];
    int ni = rsx_vp_decompile(st->vp_ucode, st->vp_ucode_bytes, hlsl, sizeof hlsl);
    if (ni <= 0) return -1;
    if (getenv("VP_DUMP")) { static int _d=0; if (_d++ < 4) {
        FILE* f = fopen("vp2_dump.hlsl", _d==1 ? "w" : "a");
        if (f) { fprintf(f, "/* per-draw VS hash pending, %d instrs */%s%s", ni, hlsl, "\n"); fclose(f); } } }
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
static u32 rsx_blend_key(const rsx_state* st, int enable)
{
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

static ID3D12PipelineState* vp_get_fp_pso(int vs_idx, u32 fp_addr, u32 blend, int nrt,
                                          DXGI_FORMAT rtfmt, int exp32, u32 cmask)
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
    u32 uhash = 2166136261u;
    {
        u32 usz = rsx_fp_program_size(vm_base + off, 4096);
        if (usz == 0) usz = 64;
        const u8* up = vm_base + off;
        for (u32 i = 0; i < usz; i++) { uhash ^= up[i]; uhash *= 16777619u; }
    }

    for (int i = 0; i < s_d3d.vp_fp_n; i++)
        if (s_d3d.vp_fp[i].fp_addr == fp_addr && s_d3d.vp_fp[i].vs_idx == vs_idx &&
            s_d3d.vp_fp[i].vs_hash == vs_hash && s_d3d.vp_fp[i].gen == s_d3d.vp_gen &&
            s_d3d.vp_fp[i].blend == blend && s_d3d.vp_fp[i].nrt == nrt &&
            s_d3d.vp_fp[i].rtfmt == (u32)rtfmt && s_d3d.vp_fp[i].exp32 == exp32 &&
            s_d3d.vp_fp[i].ucode_hash == uhash && s_d3d.vp_fp[i].cmask == cmask)
            return s_d3d.vp_fp[i].pso;
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

    /* Debug FP_ONE=<hex fp_addr>: force that program's colour output to
     * all-ones (e.g. wave's colour-detect -> full mask -> island borders
     * everywhere -> the water sim must visibly radiate if it works). */
    { const char* f1 = getenv("FP_ONE");
      if (f1 && (u32)strtoul(f1, NULL, 16) == fp_addr) {
          char* rp = strstr(hlsl, "PSOut _po; _po.c0 = r[0];");
          if (rp) memcpy(rp, "PSOut _po; _po.c0=(1).xxxx;", 27);
      } }
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
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
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
    for (int _r = 0; _r < nrt; _r++)
        pd.BlendState.RenderTarget[_r].RenderTargetWriteMask = (UINT8)(cmask & 0xF);
    pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pd.DepthStencilState.StencilEnable = FALSE;
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
    s_d3d.vp_fp[s_d3d.vp_fp_n].pso     = pso;
    s_d3d.vp_fp_n++;
    return pso;
}

/* Upload the guest texture for a VP draw into a per-frame texture slot
 * (re-uploaded every frame: gcm/cube's plasma animates in guest memory).
 * Returns the slot index (SRV at heap 1+slot) or -1. Must run while the
 * command list is open, before the draw passes. */
/* NV4097 TEXTURE_CONTROL1 component remap -> D3D12 Shader4ComponentMapping.
 * Low byte = crossbar: bits[1:0] select the SOURCE (0=A,1=R,2=G,3=B) for
 * output A, [3:2] for R, [5:4] for G, [7:6] for B. Next byte = per-output op:
 * 0 = force ZERO, 1 = force ONE, 2 = use crossbar (identity word = 0xAAE4).
 * Our uploaded resource holds guest R,G,B,A at comps 0,1,2,3. */
static u32 rsx_remap_to_d3d(u32 c1)
{
    /* Field order LSB->MSB is B,G,R,A (measured: LBP's identity word is
     * 0xAA1B = B<-B, G<-G, R<-R, A<-A with all ops = 2/remap), source codes
     * 0=A,1=R,2=G,3=B. NOT the 0xE4-identity ordering. */
    if (!(c1 & 0xFFFF)) c1 = 0xAA1B;               /* unset -> identity */
    static const u8 src2res[4] = {3, 0, 1, 2};     /* src code A,R,G,B -> resource comp */
    u32 out[4];                                    /* outputs in field order B,G,R,A */
    for (int i = 0; i < 4; i++) {
        u32 s = (c1 >> (i * 2)) & 3, op = (c1 >> (8 + i * 2)) & 3;
        out[i] = (op == 0) ? 4u : (op == 1) ? 5u : (u32)src2res[s];
    }
    /* D3D12 mapping: destR | destG<<3 | destB<<6 | destA<<9 | valid bit */
    return out[2] | (out[1] << 3) | (out[0] << 6) | (out[3] << 9) | (1u << 12);
}

/* Morton/Z-order texel offset for RSX swizzled textures (LN bit clear).
 * Interleaves x (even bit positions) and y (odd) until the smaller dimension's
 * bits run out, then the larger dimension's remaining bits ride above -- the
 * NV40 layout (matches RPCS3 convert_linear_swizzle). POT dims only, which is
 * what the hardware requires for swizzled textures. */
static inline u32 rsx_swz_off(u32 x, u32 y, u32 log2w, u32 log2h)
{
    u32 off = 0, shift = 0;
    while (log2w && log2h) {
        off |= (x & 1u) << shift; x >>= 1; shift++;
        off |= (y & 1u) << shift; y >>= 1; shift++;
        log2w--; log2h--;
    }
    off |= (x | y) << shift;     /* only one of x/y still has bits */
    return off;
}
static inline u32 rsx_log2u(u32 v) { u32 l = 0; while ((1u << l) < v) l++; return l; }

static int vp_upload_tex_slot(u32 off, u32 w, u32 h, u32 fmt)
{
    extern uint8_t* vm_base;
    if (!off || !w || !h || !vm_base || !s_d3d.srv_heap) return -1;
    int argb = ((fmt & 0x9F) == 0x85);            /* A8R8G8B8 vs B8 */
    u32 bpp = argb ? 4u : 1u;
    DXGI_FORMAT dxfmt = argb ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8_UNORM;
    int slot = -1, freeslot = -1;
    for (int i = 0; i < VP_TEX_SLOTS; i++) {
        if (s_d3d.vp_tex[i].used && s_d3d.vp_tex[i].off == off &&
            s_d3d.vp_tex[i].w == w && s_d3d.vp_tex[i].h == h)
            return i;                             /* already uploaded this frame */
        if (!s_d3d.vp_tex[i].used && freeslot < 0) freeslot = i;
    }
    if (freeslot < 0) return -1;                  /* out of slots this frame */
    slot = freeslot;
    VPTexSlot* t = &s_d3d.vp_tex[slot];
    u32 pitch = (w * bpp + 255) & ~255u;
    int fresh = 0;

    if (t->res && (t->w != w || t->h != h || t->fmt != fmt)) {
        t->res->lpVtbl->Release(t->res); t->res = NULL;
        if (t->up) { t->up->lpVtbl->Release(t->up); t->up = NULL; }
    }
    if (!t->res) {
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {0};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
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
        bd.Width = (u64)pitch * h; bd.Height = 1; bd.DepthOrArraySize = 1;
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
    int swz = !(fmt & 0x20) && (w & (w - 1)) == 0 && (h & (h - 1)) == 0;
    u32 l2w = rsx_log2u(w), l2h = rsx_log2u(h);
    if (argb) {
        /* guest big-endian A8R8G8B8 (bytes A,R,G,B) -> DXGI R8G8B8A8 (R,G,B,A) */
        const u8* sbase = vm_base + off;
        for (u32 y = 0; y < h; y++) {
            u8* drow = (u8*)mapped + (u64)y * pitch;
            for (u32 x = 0; x < w; x++) {
                const u8* s = sbase + (u64)(swz ? rsx_swz_off(x, y, l2w, l2h)
                                                : y * w + x) * 4;
                drow[x*4+0] = s[1]; drow[x*4+1] = s[2];
                drow[x*4+2] = s[3]; drow[x*4+3] = s[0];
            }
        }
    } else if (swz) {
        const u8* sbase = vm_base + off;
        for (u32 y = 0; y < h; y++) {
            u8* drow = (u8*)mapped + (u64)y * pitch;
            for (u32 x = 0; x < w; x++)
                drow[x] = sbase[rsx_swz_off(x, y, l2w, l2h)];
        }
    } else {
        for (u32 y = 0; y < h; y++)
            memcpy((u8*)mapped + (u64)y * pitch, vm_base + off + (u64)y * w, w);
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
    if (argb && getenv("TEX_SAVE")) { static int _ts = 0; if (_ts < 8) { _ts++;
        for (int pass = 0; pass < 2; pass++) {
            char pn[128];
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
                    const u8* srow = (const u8*)mapped + (u64)y * pitch;
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
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);
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
    sv.Shader4ComponentMapping = argb ? D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING : 0x1000;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE sh;
    s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &sh);
    sh.ptr += (u64)(1 + slot) * s_d3d.srv_inc;
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, t->res, &sv, sh);

    t->off = off; t->w = w; t->h = h; t->fmt = fmt; t->used = 1;
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
            + ((u64)s_d3d.vp_parity * MAX_DRAWS + slot) * 256);
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
    }
    char* dst = (char*)s_d3d.vp_cb_mapped
        + ((u64)s_d3d.vp_parity * MAX_DRAWS + slot) * VP_CB_STRIDE;
    memcpy(dst, st->vertex_constants, RSX_MAX_VERTEX_CONSTANTS * 16);
    /* Viewport epilogue (see the render_frame notes this logic came from):
     * x/y identity, z lane remaps GL clip z when the guest programs one. */
    float* vpx = (float*)(dst + RSX_MAX_VERTEX_CONSTANTS * 16);
    const float* vs_ = st->viewport_scale;
    const float* vo_ = st->viewport_offset;
    vpx[0] = vpx[1] = vpx[3] = 1.0f;
    vpx[4] = vpx[5] = vpx[7] = 0.0f;
    if (vs_[2] != 0.0f) { vpx[2] = vs_[2]; vpx[6] = vo_[2]; }
    else                { vpx[2] = 1.0f;   vpx[6] = 0.0f;   }
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
    if (getenv("RT_OFFDBG")) { static int _n=0; if (_n++ < 240)
        fprintf(stderr, "[RTOFF] tgt=0x%X off[0]=0x%X off[1]=0x%X zeta=0x%X clip=%ux%u disp=%d -> 0x%X\n",
                st->color_target, st->surface_color_offset[0], st->surface_color_offset[1],
                st->surface_zeta_offset, st->surface_clip_w, st->surface_clip_h,
                cellGcmOffsetIsDisplay(raw), cellGcmOffsetIsDisplay(raw) ? 0 : raw); }
    if (cellGcmOffsetIsDisplay(raw)) return 0;
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
static void srv_write(u32 heap_slot, ID3D12Resource* res, DXGI_FORMAT fmt, UINT mapping)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = fmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = mapping;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &h);
    h.ptr += (u64)heap_slot * s_d3d.srv_inc;
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, res, &sv, h);
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

static void render_frame(void)
{
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
    if (s_d3d.current_rsx_state && !s_d3d.vp_ready &&
        s_d3d.current_rsx_state->vp_ucode_bytes >= 16 &&
        s_d3d.vp_compiled_bytes != s_d3d.current_rsx_state->vp_ucode_bytes)
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

    /* Upload the bound font atlas once (or after a dims change) into an
     * R8_UNORM texture and create its SRV. The atlas is a linear 8-bit
     * coverage map, so a straight row copy (no deswizzle) suffices. */
    if (s_d3d.tex_bound && !s_d3d.tex_ready && s_d3d.srv_heap && s_d3d.pipeline_state_tex) {
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
                printf("[D3D12] atlas uploaded (%ux%u R8) -> textured\n", w, h);
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
    for (int _i = 0; _i < VP_TEX_SLOTS; _i++) s_d3d.vp_tex[_i].used = 0;
    for (u32 _d = 0; _d < s_d3d.draw_count && _d < MAX_DRAWS; _d++) {
        D3D12DrawRecord* dr = &s_d3d.draws[_d];
        if (!dr->is_vp || dr->is_clear) continue;
        /* Debug: RTT_VIEWRT=<hex raw offset> makes display draws sample that
         * offscreen RT at t0 (the composite blit then shows it fullscreen). */
        { const char* vr = getenv("RTT_VIEWRT");
          if (vr && dr->rt_off == 0) {
              dr->tex[0].raw = (u32)strtoul(vr, NULL, 16);
              dr->tex[0].off = 0;
              dr->tex[0].set = 1;
          } }
        /* Fill this draw's t0-t3 SRV window (DRAW_SRV_BASE + d*4): each unit
         * resolves to an offscreen RT (sampled directly), an uploaded guest
         * texture, or a null SRV. */
        for (int _u = 0; _u < 4; _u++) {
            u32 wslot = DRAW_SRV_BASE + _d * 4 + (u32)_u;
            dr->tex_rt[_u] = -1;
            if (dr->tex[_u].set) {
                int rt = off_rt_find(dr->tex[_u].raw);
                if (rt >= 0) {
                    dr->tex_rt[_u] = rt;
                    srv_write(wslot, s_d3d.off_rt[rt].res, (DXGI_FORMAT)s_d3d.off_rt[rt].dxgi,
                              D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                    continue;
                }
                if (dr->tex[_u].off &&
                    ((dr->tex[_u].fmt & 0x9F) == 0x81 || (dr->tex[_u].fmt & 0x9F) == 0x85)) {
                    int ts = vp_upload_tex_slot(dr->tex[_u].off, dr->tex[_u].w,
                                                dr->tex[_u].h, dr->tex[_u].fmt);
                    if (ts >= 0) {
                        int argb = ((s_d3d.vp_tex[ts].fmt & 0x9F) == 0x85);
                        srv_write(wslot, s_d3d.vp_tex[ts].res,
                                  argb ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8_UNORM,
                                  argb ? rsx_remap_to_d3d(dr->tex[_u].ctrl1) : 0x1000);
                        continue;
                    }
                }
            }
            srv_write(wslot, NULL, DXGI_FORMAT_R8G8B8A8_UNORM,
                      D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
        }
        if (dr->fp_addr) vp_get_fp_pso(dr->vs_idx, dr->fp_addr, dr->blend_key,
                                       dr_num_rts(dr),
                                       dr->rt_off ? rsx_surface_dxgi(dr->rt_fmt)
                                                  : DXGI_FORMAT_R8G8B8A8_UNORM,
                                       dr->fp_exp32, dr->cmask);
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
            for (u32 d = 0; d < s_d3d.draw_count && d < MAX_DRAWS; d++) {
                const D3D12DrawRecord* dr = &s_d3d.draws[d];
                if (!dr->is_vp) continue;
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
                                                dr->fp_exp32, dr->cmask) : NULL;
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
                    + ((u64)s_d3d.vp_parity * MAX_DRAWS + d) * VP_CB_STRIDE);
                if (s_d3d.vp_fpcb)
                    s_d3d.cmd_list->lpVtbl->SetGraphicsRootConstantBufferView(s_d3d.cmd_list, 2,
                        s_d3d.vp_fpcb->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_fpcb)
                        + ((u64)s_d3d.vp_parity * MAX_DRAWS + d) * 256);
                s_d3d.cmd_list->lpVtbl->DrawInstanced(s_d3d.cmd_list,
                    dr->vertex_count, 1, dr->vb_byte_offset / 256, 0);
            }
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

    int dumping = (s_d3d.dump_frames_left > 0 && s_d3d.readback_buf);
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
        /* Transition render target to PRESENT state */
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);
    }

    /* Close and execute */
    s_d3d.cmd_list->lpVtbl->Close(s_d3d.cmd_list);
    ID3D12CommandList* cmd_lists[] = {(ID3D12CommandList*)s_d3d.cmd_list};
    s_d3d.cmd_queue->lpVtbl->ExecuteCommandLists(s_d3d.cmd_queue, 1, cmd_lists);

    if (dumping) {
        wait_for_gpu();            /* ensure the copy finished before mapping */
        dump_backbuffer_bmp();
        s_d3d.dump_frames_left--;
    }

    if (s_rtsave_state) {
        if (!dumping) wait_for_gpu();
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
    s_d3d.swap_chain->lpVtbl->Present(s_d3d.swap_chain, 1, 0); /* vsync */

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

/* Read a big-endian 32-bit float from guest memory. */
static float rd_bef(const u8* src)
{
    u32 w;
    memcpy(&w, src, 4);
    w = ((w>>24)&0xFF)|((w>>8)&0xFF00)|((w<<8)&0xFF0000)|((w<<24)&0xFF000000);
    float f; memcpy(&f, &w, 4); return f;
}

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
        out->x = rd_bef(src);
        out->y = rd_bef(src + 4);
        if (pos->size >= 3) out->z = rd_bef(src + 8);

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
                out->u = rd_bef(src + 8);   /* U */
                out->v = rd_bef(src + 12);  /* V */
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

static float rd_half_be(const u8* p)
{
    u16 h = (u16)((p[0] << 8) | p[1]);
    u32 sgn = (h >> 15) & 1, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
    u32 f;
    if (exp == 0)       f = (sgn << 31);                                    /* +-0 / denorm->0 */
    else if (exp == 31) f = (sgn << 31) | 0x7F800000u | (man << 13);        /* inf/nan */
    else                f = (sgn << 31) | ((exp - 15 + 127) << 23) | (man << 13);
    float out; memcpy(&out, &f, 4); return out;
}

static void read_vp_vertex(const rsx_state* state, u32 vi, VPSlot* out16)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveOffset(u32);
    extern u32 cellGcmResolveLocated(int, u32);
    for (int i = 0; i < 16; i++) {
        VPSlot* o = &out16[i];
        o->v[0] = o->v[1] = o->v[2] = 0.0f; o->v[3] = 1.0f;
        const rsx_vertex_attrib* a = &state->vertex_attribs[i];
        if (!a->enabled || a->stride == 0) continue;
        /* Vertex frequency divisor (instancing). freq 0/1 = per-vertex. For
         * freq > 1 the element index is either vertex/freq (DIVIDE: per-instance
         * data advances once per freq verts) or vertex%freq (MODULO: a mesh
         * repeats every freq verts). NV4097_SET_FREQUENCY_DIVIDER_OPERATION's
         * per-attribute bit selects MODULO. DeferredShading's cube rings need
         * this: the shared cube mesh is MODULO, the per-instance transform in
         * attrib9 is DIVIDE -- without it attrib9 advanced every vertex and the
         * instanced cubes drew with garbage transforms (only the baseplate,
         * which isn't instanced, survived). */
        u32 ei = vi;
        if (a->frequency > 1 && !getenv("VP_NOFREQ")) {   /* VP_NOFREQ: instancing kill-switch */
            ei = (state->frequency_divider_op & (1u << i))
                     ? (vi % a->frequency)      /* MODULO: repeat mesh */
                     : (vi / a->frequency);     /* DIVIDE: per-instance */
        }
        /* The vertex-array OFFSET register (NV4097_SET_VERTEX_DATA_ARRAY_OFFSET)
         * carries the context-DMA location in bit 31: 0 = LOCAL (VRAM), 1 = MAIN
         * (IO-mapped system memory). DeferredShading is the first sample to put
         * its meshes in the main heap -- passing the raw 0x80xxxxxx offset to
         * cellGcmResolveOffset made page = 0x80x and resolved to garbage VRAM
         * (vertices read as -7.992 => degenerate => empty G-buffer). Strip the
         * bit and resolve MAIN explicitly; local offsets keep the old path. */
        u32 off = (a->offset & 0x7FFFFFFFu) + ei * a->stride;
        const u8* p = vm_base + ((a->offset & 0x80000000u)
            ? cellGcmResolveLocated(0, off)   /* MAIN: IO offset table */
            : cellGcmResolveOffset(off));     /* LOCAL/legacy path */
        u32 n = a->size ? a->size : 4; if (n > 4) n = 4;
        switch (a->type) {
        case 2: /* CELL_GCM_VERTEX_F: float32 BE */
            for (u32 k = 0; k < n; k++) o->v[k] = rd_bef(p + k * 4);
            break;
        case 3: /* SF: half float BE */
            for (u32 k = 0; k < n; k++) o->v[k] = rd_half_be(p + k * 2);
            break;
        case 4: /* UB: u8 normalized [0,1] */
            for (u32 k = 0; k < n; k++) o->v[k] = p[k] / 255.0f;
            break;
        case 1: /* S1: s16 normalized [-1,1] */
            for (u32 k = 0; k < n; k++) {
                s16 s = (s16)((p[k*2] << 8) | p[k*2+1]);
                o->v[k] = (float)s / 32767.0f;
            }
            break;
        case 5: /* S32K: s16 integer */
            for (u32 k = 0; k < n; k++)
                o->v[k] = (float)(s16)((p[k*2] << 8) | p[k*2+1]);
            break;
        case 7: /* UB256: u8 unnormalized */
            for (u32 k = 0; k < n; k++) o->v[k] = (float)p[k];
            break;
        default:
            for (u32 k = 0; k < n; k++) o->v[k] = rd_bef(p + k * 4);
            break;
        }
    }
}

static void vp_attrs_dbg(const rsx_state* state)
{
    if (!getenv("VP_ATTRS")) return;
    static int _a = 0; if (_a++ >= 6) return;
    fprintf(stderr, "[VPATTR] divider_op=0x%08X\n", state->frequency_divider_op);
    for (int i = 0; i < 16; i++) {
        const rsx_vertex_attrib* a = &state->vertex_attribs[i];
        if (a->enabled) fprintf(stderr, "[VPATTR] a%d off=0x%X stride=%u size=%u type=%u freq=%u fmt=0x%08X\n",
                                i, a->offset, a->stride, a->size, a->type, a->frequency, a->format);
    }
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
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    vp_attrs_dbg(state);
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (count > maxv) count = maxv - (maxv % 3);
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    for (u32 k = 0; k < count; k++)
        read_vp_vertex(state, first + k, &out[k*16]);
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
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled) return 0;
    u32 maxv = (MAX_VERTICES * VP_VERT_STRIDE - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (count > maxv) count = maxv - (maxv % 3);
    VPSlot* out = (VPSlot*)((u8*)s_d3d.vp_vb_mapped
        + (u64)s_d3d.vp_parity * MAX_VERTICES * VP_VERT_STRIDE + s_d3d.vp_vb_offset);
    for (u32 k = 0; k < count; k++)
        read_vp_vertex(state, read_guest_index(state, first + k), &out[k*16]);
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
    if ((primitive == 5 /* TRIANGLES */ || primitive == 6 /* TRIANGLE_STRIP */
         || primitive == 7 /* TRIANGLE_FAN */) &&
        s_d3d.vp_vb_mapped && s_d3d.vp_root_sig) {
        u32 rec = s_d3d.vp_vb_offset;
        u32 emitted = (primitive == 5)
            ? upload_tris_vp(s_d3d.current_rsx_state, first, count)
            : upload_strip_vp(s_d3d.current_rsx_state, first, count, primitive == 7);
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
            vp_record_cb(s_d3d.draw_count, dr->vs_idx, dr);
            s_d3d.draw_count++;
        }
        return;
    }

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
        vp_record_cb(s_d3d.draw_count, dr->vs_idx, dr);
        s_d3d.draw_count++;
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
         base_fmt == 0x9A /* W16Z16Y16X16 half-float: RTT intermediates */)) {
        s_d3d.cur_texs[unit].off = cellGcmResolveLocated((tex->format & 3) == 1, offset);
        s_d3d.cur_texs[unit].raw = offset;
        s_d3d.cur_texs[unit].w = width; s_d3d.cur_texs[unit].h = height;
        s_d3d.cur_texs[unit].fmt = format;   /* full byte: LN(0x20)/UN(0x40) kept */
        s_d3d.cur_texs[unit].ctrl1 = tex->control1;
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

    /* Log enabled vertex attributes for debugging */
    static int log_count = 0;
    if (log_count < 5) {
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

    if (s_d3d.initialized && has_display)
        render_frame();
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
