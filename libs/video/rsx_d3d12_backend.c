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
#include "rsx_fp_decompiler.h"
#include "rsx_vp_decompiler.h"
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
#include <dxgi1_4.h>
#include <d3dcompiler.h>

/* We need these GUIDs — define them here to avoid uuid.lib dependency */
#include <initguid.h>

/* RSX→DXGI texture format mapping. Included AFTER the D3D12 headers on purpose:
 * it defines DXGI_FORMAT_* helper macros whose values match the real enum, so
 * here they harmlessly shadow the enumerators; including it first would clash
 * with dxgiformat.h's enum definition. */
#include "rsx_texture_formats.h"

/* Link libraries */
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

#define FRAME_COUNT         2   /* double buffering */
#define MAX_VERTICES      4096  /* per-frame vertex buffer */
#define MAX_DRAWS          256  /* per-frame draw records */
#define MAX_PSO_CACHE       64  /* distinct blend/depth/topology PSOs */
#define MAX_FP_CACHE        64  /* distinct fragment programs */
#define MAX_FP_BYTES     16384  /* upper bound on one fragment program */
#define FP_TEXTURE_SLOTS    16  /* SRV/sampler banks the decompiled PS expects */

/* A decompiled + compiled fragment program, keyed by guest address + content
 * hash. ps_blob == NULL records a known-bad program (decompile/compile failed)
 * so we don't retry it every frame; such draws fall back to the placeholder
 * pixel shader. */
typedef struct {
    u32       guest_addr;
    u32       hash;
    ID3DBlob* ps_blob;
} FpCacheEntry;

/* A decompiled + compiled vertex program, keyed by microcode content hash.
 * vs_blob == NULL records a known-bad program (falls back to placeholder VS). */
typedef struct {
    u32       hash;
    ID3DBlob* vs_blob;
} VpCacheEntry;

/* Host vertex fed to the placeholder VS. Carries the RSX vertex attributes
 * that map to fragment-program interpolants (passthrough VP model: we don't
 * translate the vertex program yet, so each attribute is forwarded straight
 * to its conventional interpolant). All but position are float4 for a uniform
 * layout. Field order must match s_input_layout and the VS input struct. */
typedef struct {
    float pos[3];     /* POSITION      (attrib 0)      */
    float col0[4];    /* COLOR0        (attrib 3)      */
    float col1[4];    /* COLOR1        (attrib 4)      */
    float fog[4];     /* FOG  (.x)     (attrib 5)      */
    float tc[8][4];   /* TEXCOORD0..7  (attribs 8..15) */
} RsxVertex;

/* Identifies a unique graphics PSO. D3D12 bakes blend + depth state into the
 * pipeline object, so any change to these forces a new PSO. We snapshot the
 * relevant RSX state at draw-record time into this key and look it up (or
 * create) in render_frame. Raw NV4097 enum values are stored directly; the
 * nv_to_d3d12_* mappers translate them at PSO-creation time. The struct is
 * memset to 0 before filling and compared with memcmp, so it must contain no
 * uninitialised padding -- keep the fields naturally aligned. */
typedef struct {
    u32 topology_type;      /* D3D12_PRIMITIVE_TOPOLOGY_TYPE_{POINT,LINE,TRIANGLE} */
    u32 blend_enable;       /* 0/1 */
    u32 blend_sfactor;      /* raw NV4097 */
    u32 blend_dfactor;      /* raw NV4097 */
    u32 blend_equation;     /* raw NV4097 */
    u32 depth_enable;       /* 0/1 */
    u32 depth_write;        /* 0/1 */
    u32 depth_func;         /* raw NV4097 */
    u32 fp_id;              /* fragment-program cache index, or 0xFFFFFFFF */
    u32 vp_id;              /* vertex-program cache index, or 0xFFFFFFFF */
} PsoKey;

typedef struct {
    PsoKey key;
    ID3D12PipelineState* pso;
} PsoCacheEntry;

typedef struct {
    u32 vb_byte_offset; /* offset into vb_mapped where this draw's verts live */
    u32 vertex_count;
    u32 topology;       /* D3D_PRIMITIVE_TOPOLOGY_* */
    PsoKey pso_key;     /* blend/depth/topology snapshot for PSO selection */
} D3D12DrawRecord;

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
    ID3D12PipelineState*  pipeline_state;         /* triangle class — default/fallback */
    ID3D12PipelineState*  pipeline_state_lines;   /* line class — fallback */
    ID3D12PipelineState*  pipeline_state_points;  /* point class — fallback */

    /* PSO cache keyed by blend/depth/topology state. Shader bytecode is kept
     * alive (vs_blob/ps_blob) so new PSOs can be built lazily at draw time. */
    ID3DBlob*             vs_blob;
    ID3DBlob*             ps_blob;
    PsoCacheEntry         pso_cache[MAX_PSO_CACHE];
    u32                   pso_cache_count;

    /* Fragment-program cache (RSX NV40 bytecode → compiled HLSL PS). */
    FpCacheEntry          fp_cache[MAX_FP_CACHE];
    u32                   fp_cache_count;
    u32                   current_fp_id;  /* selected by set_shader; 0xFFFFFFFF = placeholder */

    /* Vertex-program cache (RSX NV40 transform program → compiled HLSL VS),
     * plus a VS-visible CBV (b0) holding the vertex constant bank, and a
     * second root signature that binds that CBV instead of the placeholder
     * MVP root constants. */
    VpCacheEntry          vp_cache[MAX_FP_CACHE];
    u32                   vp_cache_count;
    u32                   current_vp_id;  /* 0xFFFFFFFF = placeholder MVP VS */
    ID3D12RootSignature*  root_signature_vp;
    ID3D12Resource*       vp_const_buffer;   /* upload heap, 1024 float4 */
    void*                 vp_const_mapped;

    /* Shader-visible SRV heap (FP_TEXTURE_SLOTS null/texture SRVs at t0..) so
     * decompiled pixel shaders that sample textures link against the root
     * signature. Slots are filled with real textures by bind_texture. */
    ID3D12DescriptorHeap* srv_heap;
    u32                   srv_descriptor_size;

    /* Texture upload (synchronous one-shot, isolated from the frame list). */
    ID3D12CommandAllocator*    upload_alloc;
    ID3D12GraphicsCommandList* upload_list;
    ID3D12Fence*               upload_fence;
    HANDLE                     upload_event;
    u64                        upload_fence_value;
    ID3D12Resource*            unit_textures[FP_TEXTURE_SLOTS];
    u32                        unit_tex_key[FP_TEXTURE_SLOTS];

    /* Depth/stencil */
    ID3D12DescriptorHeap* dsv_heap;
    ID3D12Resource*       depth_buffer;

    /* Dynamic vertex buffer (upload heap) */
    ID3D12Resource*       vertex_buffer;
    D3D12_VERTEX_BUFFER_VIEW vb_view;
    void*                 vb_mapped;      /* persistently mapped */
    u32                   vb_offset;      /* current write position */

    int                   pipeline_ready; /* 1 if root sig + PSO created */

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

/* Vertex input layout shared by every PSO. Mirrors RsxVertex / the VS input
 * struct: position + the interpolants a fragment program can read. */
#define IL_PV D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA
#define IL_F4 DXGI_FORMAT_R32G32B32A32_FLOAT
static const D3D12_INPUT_ELEMENT_DESC s_input_layout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,   0, IL_PV, 0},
    {"COLOR",    0, IL_F4,                       0,  12, IL_PV, 0},
    {"COLOR",    1, IL_F4,                       0,  28, IL_PV, 0},
    {"FOG",      0, IL_F4,                       0,  44, IL_PV, 0},
    {"TEXCOORD", 0, IL_F4,                       0,  60, IL_PV, 0},
    {"TEXCOORD", 1, IL_F4,                       0,  76, IL_PV, 0},
    {"TEXCOORD", 2, IL_F4,                       0,  92, IL_PV, 0},
    {"TEXCOORD", 3, IL_F4,                       0, 108, IL_PV, 0},
    {"TEXCOORD", 4, IL_F4,                       0, 124, IL_PV, 0},
    {"TEXCOORD", 5, IL_F4,                       0, 140, IL_PV, 0},
    {"TEXCOORD", 6, IL_F4,                       0, 156, IL_PV, 0},
    {"TEXCOORD", 7, IL_F4,                       0, 172, IL_PV, 0},
};
#define S_INPUT_LAYOUT_COUNT 12

/* ---------------------------------------------------------------------------
 * RSX (NV4097 / OpenGL-style enum) → D3D12 state mappers
 *
 * RSX blend factors and compare funcs use the OpenGL enum values that the
 * NV4097 class inherits. We store the raw value in rsx_state and translate
 * here, at PSO-creation time.
 * -----------------------------------------------------------------------*/

/* Blend factor for the RGB channels. */
static D3D12_BLEND nv_to_d3d12_blend_color(u32 f)
{
    switch (f) {
    case 0x0000: return D3D12_BLEND_ZERO;
    case 0x0001: return D3D12_BLEND_ONE;
    case 0x0300: return D3D12_BLEND_SRC_COLOR;
    case 0x0301: return D3D12_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D12_BLEND_SRC_ALPHA;
    case 0x0303: return D3D12_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D12_BLEND_DEST_ALPHA;
    case 0x0305: return D3D12_BLEND_INV_DEST_ALPHA;
    case 0x0306: return D3D12_BLEND_DEST_COLOR;
    case 0x0307: return D3D12_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D12_BLEND_SRC_ALPHA_SAT;
    case 0x8001: return D3D12_BLEND_BLEND_FACTOR;     /* CONSTANT_COLOR */
    case 0x8002: return D3D12_BLEND_INV_BLEND_FACTOR; /* ONE_MINUS_CONSTANT_COLOR */
    case 0x8003: return D3D12_BLEND_BLEND_FACTOR;     /* CONSTANT_ALPHA (approx) */
    case 0x8004: return D3D12_BLEND_INV_BLEND_FACTOR; /* ONE_MINUS_CONSTANT_ALPHA */
    default:     return D3D12_BLEND_ONE;
    }
}

/* Blend factor for the alpha channel. D3D12 rejects *_COLOR factors in the
 * alpha slots, so the four color-typed factors are folded to their alpha
 * equivalents; everything else is already alpha-safe. */
static D3D12_BLEND nv_to_d3d12_blend_alpha(u32 f)
{
    switch (f) {
    case 0x0300: return D3D12_BLEND_SRC_ALPHA;      /* SRC_COLOR  -> SRC_ALPHA  */
    case 0x0301: return D3D12_BLEND_INV_SRC_ALPHA;  /* 1-SRC_COLOR-> 1-SRC_ALPHA*/
    case 0x0306: return D3D12_BLEND_DEST_ALPHA;     /* DST_COLOR  -> DST_ALPHA  */
    case 0x0307: return D3D12_BLEND_INV_DEST_ALPHA; /* 1-DST_COLOR-> 1-DST_ALPHA*/
    default:     return nv_to_d3d12_blend_color(f);
    }
}

static D3D12_BLEND_OP nv_to_d3d12_blend_op(u32 e)
{
    switch (e) {
    case 0x8006: return D3D12_BLEND_OP_ADD;
    case 0x8007: return D3D12_BLEND_OP_MIN;
    case 0x8008: return D3D12_BLEND_OP_MAX;
    case 0x800A: return D3D12_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D12_BLEND_OP_REV_SUBTRACT;
    default:     return D3D12_BLEND_OP_ADD;
    }
}

static D3D12_COMPARISON_FUNC nv_to_d3d12_compare(u32 f)
{
    switch (f) {
    case 0x0200: return D3D12_COMPARISON_FUNC_NEVER;
    case 0x0201: return D3D12_COMPARISON_FUNC_LESS;
    case 0x0202: return D3D12_COMPARISON_FUNC_EQUAL;
    case 0x0203: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case 0x0204: return D3D12_COMPARISON_FUNC_GREATER;
    case 0x0205: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case 0x0206: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    case 0x0207: return D3D12_COMPARISON_FUNC_ALWAYS;
    default:     return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    }
}

/* Topology class (point/line/triangle) that a D3D primitive topology belongs
 * to. The PSO's PrimitiveTopologyType must match the topology set on the IA. */
static D3D12_PRIMITIVE_TOPOLOGY_TYPE d3d12_topo_class(u32 topo)
{
    if (topo == D3D_TOPOLOGY_POINTLIST)
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    if (topo == D3D_TOPOLOGY_LINELIST || topo == D3D_TOPOLOGY_LINESTRIP)
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
}

/* Build a PSO key from the current RSX state for a given draw topology.
 * fp_id/vp_id identify the resolved fragment/vertex programs (or 0xFFFFFFFF). */
static PsoKey pso_key_from_state(const rsx_state* st, u32 topo, u32 fp_id, u32 vp_id)
{
    PsoKey k;
    memset(&k, 0, sizeof(k));
    k.topology_type = (u32)d3d12_topo_class(topo);
    k.fp_id = fp_id;
    k.vp_id = vp_id;
    if (st) {
        k.blend_enable   = st->blend_enable ? 1u : 0u;
        k.blend_sfactor  = st->blend_sfactor;
        k.blend_dfactor  = st->blend_dfactor;
        k.blend_equation = st->blend_equation;
        k.depth_enable   = st->depth_test_enable ? 1u : 0u;
        k.depth_write    = st->depth_mask ? 1u : 0u;
        k.depth_func     = st->depth_func;
    } else {
        /* No state yet: opaque draw with default depth test. */
        k.depth_enable = 1;
        k.depth_write  = 1;
    }
    return k;
}

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
#ifndef NDEBUG
    {
        ID3D12Debug* debug_controller = NULL;
        hr = D3D12GetDebugInterface(&IID_ID3D12Debug, (void**)&debug_controller);
        if (SUCCEEDED(hr) && debug_controller) {
            debug_controller->lpVtbl->EnableDebugLayer(debug_controller);
            debug_controller->lpVtbl->Release(debug_controller);
            printf("[D3D12] Debug layer enabled\n");
        }
    }
#endif

    /* Create DXGI factory */
    IDXGIFactory4* factory = NULL;
    hr = CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&factory);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: CreateDXGIFactory1 failed (0x%08lX)\n", hr);
        return -1;
    }

    /* Create D3D12 device */
    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0,
                           &IID_ID3D12Device, (void**)&s_d3d.device);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: D3D12CreateDevice failed (0x%08lX)\n", hr);
        factory->lpVtbl->Release(factory);
        return -1;
    }
    printf("[D3D12] Device created (feature level 11.0)\n");

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
        depth_desc.Width              = width;
        depth_desc.Height             = height;
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

    /* ---------------------------------------------------------------
     * SRV heap for fragment-program textures (t0..t15).
     * Filled with null SRVs now (sampling returns 0); bind_texture will
     * replace slots with real texture views later. Shader-visible so the
     * root descriptor table can reference it.
     * ---------------------------------------------------------------*/
    {
        D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc = {0};
        srv_heap_desc.NumDescriptors = FP_TEXTURE_SLOTS;
        srv_heap_desc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
            s_d3d.device, &srv_heap_desc, &IID_ID3D12DescriptorHeap,
            (void**)&s_d3d.srv_heap);
        if (FAILED(hr)) {
            printf("[D3D12] SRV heap creation failed (0x%08lX)\n", hr);
            return -1;
        }
        s_d3d.srv_descriptor_size = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
            s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        D3D12_CPU_DESCRIPTOR_HANDLE h;
        s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &h);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {0};
        srv.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels     = 1;
        for (u32 i = 0; i < FP_TEXTURE_SLOTS; i++) {
            s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, NULL, &srv, h);
            h.ptr += s_d3d.srv_descriptor_size;
        }
        printf("[D3D12] SRV heap created (%d null slots)\n", FP_TEXTURE_SLOTS);
    }

    /* Dedicated command list + fence for synchronous texture uploads. */
    {
        hr = s_d3d.device->lpVtbl->CreateCommandAllocator(
            s_d3d.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void**)&s_d3d.upload_alloc);
        if (SUCCEEDED(hr))
            hr = s_d3d.device->lpVtbl->CreateCommandList(
                s_d3d.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                s_d3d.upload_alloc, NULL,
                &IID_ID3D12GraphicsCommandList, (void**)&s_d3d.upload_list);
        if (SUCCEEDED(hr)) {
            s_d3d.upload_list->lpVtbl->Close(s_d3d.upload_list);
            hr = s_d3d.device->lpVtbl->CreateFence(
                s_d3d.device, 0, D3D12_FENCE_FLAG_NONE,
                &IID_ID3D12Fence, (void**)&s_d3d.upload_fence);
        }
        if (FAILED(hr)) { printf("[D3D12] Upload command objects failed\n"); return -1; }
        s_d3d.upload_event = CreateEvent(NULL, FALSE, FALSE, NULL);
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
        /* Param 0: MVP root constants (b0, VS). Param 1: SRV table t0..t15
         * (PS) for decompiled fragment programs that sample textures. */
        D3D12_DESCRIPTOR_RANGE srv_range = {0};
        srv_range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors     = FP_TEXTURE_SLOTS;
        srv_range.BaseShaderRegister = 0;   /* t0 */
        srv_range.RegisterSpace      = 0;
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER root_params[2] = {0};
        root_params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        root_params[0].Constants.ShaderRegister = 0;   /* b0 */
        root_params[0].Constants.RegisterSpace  = 0;
        root_params[0].Constants.Num32BitValues = 16;  /* mat4 */
        root_params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_VERTEX;
        root_params[1].ParameterType   = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        root_params[1].DescriptorTable.NumDescriptorRanges = 1;
        root_params[1].DescriptorTable.pDescriptorRanges   = &srv_range;
        root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        /* 16 static samplers s0..s15 (linear wrap) so the PS sampler array is
         * satisfied without per-draw sampler descriptors. */
        D3D12_STATIC_SAMPLER_DESC samplers[FP_TEXTURE_SLOTS] = {0};
        for (u32 i = 0; i < FP_TEXTURE_SLOTS; i++) {
            samplers[i].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samplers[i].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            samplers[i].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            samplers[i].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            samplers[i].ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
            samplers[i].MaxLOD           = D3D12_FLOAT32_MAX;
            samplers[i].ShaderRegister   = i;   /* s0..s15 */
            samplers[i].RegisterSpace    = 0;
            samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }

        D3D12_ROOT_SIGNATURE_DESC rs_desc = {0};
        rs_desc.NumParameters     = 2;
        rs_desc.pParameters       = root_params;
        rs_desc.NumStaticSamplers = FP_TEXTURE_SLOTS;
        rs_desc.pStaticSamplers   = samplers;
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
     * VP root signature: identical to the default one but root param 0 is a
     * CBV at b0 (the vertex constant bank) instead of the MVP root constants.
     * Used by PSOs that run a decompiled vertex program.
     * ---------------------------------------------------------------*/
    {
        D3D12_DESCRIPTOR_RANGE srv_range = {0};
        srv_range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors     = FP_TEXTURE_SLOTS;
        srv_range.BaseShaderRegister = 0;
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER rp[2] = {0};
        rp[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rp[0].Descriptor.ShaderRegister = 0;   /* b0 = vp_c[] */
        rp[0].Descriptor.RegisterSpace  = 0;
        rp[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;
        rp[1].ParameterType   = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable.NumDescriptorRanges = 1;
        rp[1].DescriptorTable.pDescriptorRanges   = &srv_range;
        rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samplers[FP_TEXTURE_SLOTS] = {0};
        for (u32 i = 0; i < FP_TEXTURE_SLOTS; i++) {
            samplers[i].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samplers[i].AddressU = samplers[i].AddressV = samplers[i].AddressW =
                D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            samplers[i].ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
            samplers[i].MaxLOD           = D3D12_FLOAT32_MAX;
            samplers[i].ShaderRegister   = i;
            samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }

        D3D12_ROOT_SIGNATURE_DESC rd = {0};
        rd.NumParameters     = 2;
        rd.pParameters       = rp;
        rd.NumStaticSamplers = FP_TEXTURE_SLOTS;
        rd.pStaticSamplers   = samplers;
        rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                 | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                 | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                 | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        ID3DBlob* sb = NULL; ID3DBlob* eb = NULL;
        hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sb, &eb);
        if (SUCCEEDED(hr))
            hr = s_d3d.device->lpVtbl->CreateRootSignature(
                s_d3d.device, 0, sb->lpVtbl->GetBufferPointer(sb),
                sb->lpVtbl->GetBufferSize(sb),
                &IID_ID3D12RootSignature, (void**)&s_d3d.root_signature_vp);
        if (sb) sb->lpVtbl->Release(sb);
        if (eb) eb->lpVtbl->Release(eb);
        if (FAILED(hr)) { printf("[D3D12] VP root signature creation failed\n"); return -1; }

        /* Vertex constant bank CBV (upload heap, 1024 float4 = 16 KB),
         * persistently mapped; refreshed each frame from rsx_state. */
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC cbd = {0};
        cbd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        cbd.Width            = 1024 * 16;
        cbd.Height           = 1;
        cbd.DepthOrArraySize = 1;
        cbd.MipLevels        = 1;
        cbd.SampleDesc.Count = 1;
        cbd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr = s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &cbd,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.vp_const_buffer);
        if (FAILED(hr)) { printf("[D3D12] VP const buffer creation failed\n"); return -1; }
        D3D12_RANGE nr = {0,0};
        s_d3d.vp_const_buffer->lpVtbl->Map(s_d3d.vp_const_buffer, 0, &nr, &s_d3d.vp_const_mapped);
        printf("[D3D12] VP root signature + 16KB constant CBV ready\n");
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
        /* Passthrough VS: applies the MVP to position and forwards every RSX
         * interpolant (COL0/COL1/FOG/TEXCOORD0..7) so a decompiled fragment
         * program can read whichever it needs. VSIO is the superset the
         * decompiler's PSInput is always a subset of. */
        static const char vs_hlsl[] =
            "cbuffer cb0 : register(b0) {\n"
            "    float4 mvp_col0; float4 mvp_col1; float4 mvp_col2; float4 mvp_col3;\n"
            "};\n"
            "struct VSInput {\n"
            "    float3 pos:POSITION; float4 col0:COLOR0; float4 col1:COLOR1;\n"
            "    float4 fog:FOG;\n"
            "    float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2;\n"
            "    float4 tc3:TEXCOORD3; float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5;\n"
            "    float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;\n"
            "};\n"
            "struct VSIO {\n"
            "    float4 pos:SV_POSITION; float4 col0:COLOR0; float4 col1:COLOR1;\n"
            "    float4 fog:FOG;\n"
            "    float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2;\n"
            "    float4 tc3:TEXCOORD3; float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5;\n"
            "    float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;\n"
            "};\n"
            "VSIO main(VSInput i) {\n"
            "    VSIO o;\n"
            "    float4 p = float4(i.pos, 1.0);\n"
            "    o.pos = mvp_col0 * p.x + mvp_col1 * p.y + mvp_col2 * p.z + mvp_col3 * p.w;\n"
            "    o.col0=i.col0; o.col1=i.col1; o.fog=i.fog;\n"
            "    o.tc0=i.tc0; o.tc1=i.tc1; o.tc2=i.tc2; o.tc3=i.tc3;\n"
            "    o.tc4=i.tc4; o.tc5=i.tc5; o.tc6=i.tc6; o.tc7=i.tc7;\n"
            "    return o;\n"
            "}\n";
        static const char ps_hlsl[] =
            "struct PSInput { float4 pos : SV_POSITION; float4 col0 : COLOR0; };\n"
            "float4 main(PSInput i) : SV_TARGET { return i.col0; }\n";

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
            /* Keep the compiled bytecode alive: the PSO cache builds new
             * pipeline objects lazily from these blobs at draw time. */
            s_d3d.vs_blob = vs_blob;
            s_d3d.ps_blob = ps_blob;

            D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {0};
            pso_desc.pRootSignature = s_d3d.root_signature;
            pso_desc.VS.pShaderBytecode = vs_blob->lpVtbl->GetBufferPointer(vs_blob);
            pso_desc.VS.BytecodeLength = vs_blob->lpVtbl->GetBufferSize(vs_blob);
            pso_desc.PS.pShaderBytecode = ps_blob->lpVtbl->GetBufferPointer(ps_blob);
            pso_desc.PS.BytecodeLength = ps_blob->lpVtbl->GetBufferSize(ps_blob);
            pso_desc.InputLayout.pInputElementDescs = s_input_layout;
            pso_desc.InputLayout.NumElements = S_INPUT_LAYOUT_COUNT;
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

            /* vs_blob/ps_blob are intentionally NOT released here -- they are
             * retained in s_d3d for lazy PSO-cache creation and freed in
             * rsx_d3d12_backend_shutdown(). */
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
        buf_desc.Width = MAX_VERTICES * sizeof(RsxVertex);
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
            s_d3d.vb_view.StrideInBytes = sizeof(RsxVertex);
            s_d3d.vb_view.SizeInBytes = MAX_VERTICES * sizeof(RsxVertex);
            printf("[D3D12] Vertex buffer created (%u KB)\n",
                   (u32)(MAX_VERTICES * sizeof(RsxVertex)) / 1024);
        }
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
        WaitForSingleObject(s_d3d.fence_event, INFINITE);
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
        WaitForSingleObject(s_d3d.fence_event, INFINITE);
    }

    s_d3d.fence_values[s_d3d.frame_index] = current_fence + 1;
}

/* ---------------------------------------------------------------------------
 * Fragment-program cache (RSX NV40 bytecode → compiled HLSL pixel shader)
 * -----------------------------------------------------------------------*/

static u32 fnv1a(const u8* p, u32 n)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

/* Resolve the fragment program at guest address `addr` to an FP cache index.
 * Decompiles + compiles on first sight; reuses by (addr, content-hash).
 * Returns 0xFFFFFFFF when the program can't be used (out of range, no
 * terminator, decompile/compile failure, or cache full) -- the caller then
 * falls back to the placeholder pixel shader. */
static u32 resolve_fragment_program(u32 addr)
{
    extern uint8_t* vm_base;
    if (!vm_base || addr == 0 || addr >= 0x20000000) return 0xFFFFFFFFu;

    const u8* uc = vm_base + addr;
    u32 size = rsx_fp_program_size(uc, MAX_FP_BYTES);
    if (size == 0) return 0xFFFFFFFFu;          /* no PROGRAM_END within bound */
    u32 hash = fnv1a(uc, size);

    for (u32 i = 0; i < s_d3d.fp_cache_count; i++) {
        if (s_d3d.fp_cache[i].guest_addr == addr && s_d3d.fp_cache[i].hash == hash)
            return s_d3d.fp_cache[i].ps_blob ? i : 0xFFFFFFFFu;
    }
    if (s_d3d.fp_cache_count >= MAX_FP_CACHE) {
        static int warned = 0;
        if (!warned) { printf("[D3D12] FP cache full (%d)\n", MAX_FP_CACHE); warned = 1; }
        return 0xFFFFFFFFu;
    }

    /* Decompile to HLSL. */
    static char hlsl[64 * 1024];
    int n = rsx_fp_decompile(uc, size, hlsl, sizeof(hlsl));

    ID3DBlob* ps_blob = NULL;
    if (n > 0) {
        ID3DBlob* err = NULL;
        HRESULT hr = D3DCompile(hlsl, strlen(hlsl), "rsx_fp", NULL, NULL,
                                "main", "ps_5_0", 0, 0, &ps_blob, &err);
        if (FAILED(hr)) {
            printf("[D3D12] FP @0x%08X (%u instr) compile FAILED: %s\n", addr, n,
                   err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "?");
            if (err) err->lpVtbl->Release(err);
            ps_blob = NULL;  /* cache as known-bad */
        } else {
            if (err) err->lpVtbl->Release(err);
        }
    }

    u32 idx = s_d3d.fp_cache_count++;
    s_d3d.fp_cache[idx].guest_addr = addr;
    s_d3d.fp_cache[idx].hash       = hash;
    s_d3d.fp_cache[idx].ps_blob    = ps_blob;
    printf("[D3D12] FP @0x%08X %u bytes, %d instr -> %s (cache #%u)\n",
           addr, size, n, ps_blob ? "compiled" : "FALLBACK", idx);
    return ps_blob ? idx : 0xFFFFFFFFu;
}

/* ---------------------------------------------------------------------------
 * Vertex-program cache (RSX NV40 transform program → compiled HLSL VS)
 * -----------------------------------------------------------------------*/

static int vp_is_valid(u32 vp_id)
{
    return vp_id < s_d3d.vp_cache_count && s_d3d.vp_cache[vp_id].vs_blob != NULL;
}

/* Resolve the current transform program to a VP cache index. Decompiles +
 * compiles on first sight, reuses by content hash. 0xFFFFFFFF => fall back to
 * the placeholder MVP vertex shader. */
static u32 resolve_vertex_program(const rsx_state* st)
{
    if (!st || st->transform_program_words < 4) return 0xFFFFFFFFu;
    const u8* uc = (const u8*)st->transform_program;
    u32 bytes = st->transform_program_words * 4;
    u32 instrs = rsx_vp_program_size_instrs(uc, bytes);
    if (instrs == 0) return 0xFFFFFFFFu;
    u32 size = instrs * 16;
    u32 hash = fnv1a(uc, size);

    for (u32 i = 0; i < s_d3d.vp_cache_count; i++)
        if (s_d3d.vp_cache[i].hash == hash)
            return s_d3d.vp_cache[i].vs_blob ? i : 0xFFFFFFFFu;
    if (s_d3d.vp_cache_count >= MAX_FP_CACHE) return 0xFFFFFFFFu;

    static char hlsl[160 * 1024];
    int n = rsx_vp_decompile(uc, size, hlsl, sizeof(hlsl));
    ID3DBlob* vs_blob = NULL;
    if (n > 0) {
        ID3DBlob* err = NULL;
        HRESULT hr = D3DCompile(hlsl, strlen(hlsl), "rsx_vp", NULL, NULL,
                                "main", "vs_5_0", 0, 0, &vs_blob, &err);
        if (FAILED(hr)) {
            printf("[D3D12] VP (%u instr) compile FAILED: %s\n", instrs,
                   err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "?");
            vs_blob = NULL;
        }
        if (err) err->lpVtbl->Release(err);
    }
    u32 idx = s_d3d.vp_cache_count++;
    s_d3d.vp_cache[idx].hash    = hash;
    s_d3d.vp_cache[idx].vs_blob = vs_blob;
    printf("[D3D12] VP %u instr (%u bytes) -> %s (cache #%u)\n",
           instrs, size, vs_blob ? "compiled" : "FALLBACK", idx);
    return vs_blob ? idx : 0xFFFFFFFFu;
}

/* ---------------------------------------------------------------------------
 * PSO cache
 * -----------------------------------------------------------------------*/

/* Return a PSO matching `key`, creating and caching one on first use. Returns
 * NULL only if shaders are unavailable or both creation and cache are
 * exhausted (caller falls back to a topology-class PSO). */
static ID3D12PipelineState* get_or_create_pso(const PsoKey* key)
{
    /* Linear search -- the cache stays tiny (a handful of blend/depth combos
     * per title), so a hash map would be overkill. */
    for (u32 i = 0; i < s_d3d.pso_cache_count; i++) {
        if (memcmp(&s_d3d.pso_cache[i].key, key, sizeof(PsoKey)) == 0)
            return s_d3d.pso_cache[i].pso;
    }

    if (!s_d3d.vs_blob || !s_d3d.ps_blob) return NULL;
    if (s_d3d.pso_cache_count >= MAX_PSO_CACHE) {
        static int warned = 0;
        if (!warned) { printf("[D3D12] PSO cache full (%d) -- reusing fallback\n",
                              MAX_PSO_CACHE); warned = 1; }
        return NULL;
    }

    /* Pick the pixel shader: the decompiled fragment program if this key
     * names a valid one, otherwise the placeholder vertex-color PS. */
    ID3DBlob* ps = s_d3d.ps_blob;
    if (key->fp_id < s_d3d.fp_cache_count && s_d3d.fp_cache[key->fp_id].ps_blob)
        ps = s_d3d.fp_cache[key->fp_id].ps_blob;

    /* Pick the vertex shader + root signature: the decompiled vertex program
     * (with the vertex-constant CBV root sig) if valid, else the placeholder
     * MVP vertex shader (root-constants root sig). */
    ID3DBlob* vs = s_d3d.vs_blob;
    ID3D12RootSignature* rs = s_d3d.root_signature;
    if (vp_is_valid(key->vp_id)) {
        vs = s_d3d.vp_cache[key->vp_id].vs_blob;
        rs = s_d3d.root_signature_vp;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {0};
    pso_desc.pRootSignature = rs;
    pso_desc.VS.pShaderBytecode = vs->lpVtbl->GetBufferPointer(vs);
    pso_desc.VS.BytecodeLength  = vs->lpVtbl->GetBufferSize(vs);
    pso_desc.PS.pShaderBytecode = ps->lpVtbl->GetBufferPointer(ps);
    pso_desc.PS.BytecodeLength  = ps->lpVtbl->GetBufferSize(ps);
    pso_desc.InputLayout.pInputElementDescs = s_input_layout;
    pso_desc.InputLayout.NumElements = S_INPUT_LAYOUT_COUNT;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.SampleDesc.Count = 1;
    pso_desc.PrimitiveTopologyType = (D3D12_PRIMITIVE_TOPOLOGY_TYPE)key->topology_type;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    /* Blend state (single render target). */
    D3D12_RENDER_TARGET_BLEND_DESC* rt = &pso_desc.BlendState.RenderTarget[0];
    rt->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (key->blend_enable) {
        rt->BlendEnable    = TRUE;
        rt->SrcBlend       = nv_to_d3d12_blend_color(key->blend_sfactor);
        rt->DestBlend      = nv_to_d3d12_blend_color(key->blend_dfactor);
        rt->BlendOp        = nv_to_d3d12_blend_op(key->blend_equation);
        rt->SrcBlendAlpha  = nv_to_d3d12_blend_alpha(key->blend_sfactor);
        rt->DestBlendAlpha = nv_to_d3d12_blend_alpha(key->blend_dfactor);
        rt->BlendOpAlpha   = nv_to_d3d12_blend_op(key->blend_equation);
    }

    /* Depth/stencil state. */
    pso_desc.DepthStencilState.DepthEnable    = key->depth_enable ? TRUE : FALSE;
    pso_desc.DepthStencilState.DepthWriteMask = key->depth_write
        ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    pso_desc.DepthStencilState.DepthFunc      = nv_to_d3d12_compare(key->depth_func);
    pso_desc.DepthStencilState.StencilEnable  = FALSE;

    ID3D12PipelineState* pso = NULL;
    HRESULT hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
        s_d3d.device, &pso_desc, &IID_ID3D12PipelineState, (void**)&pso);
    if (FAILED(hr)) {
        printf("[D3D12] PSO cache: creation failed (0x%08lX) "
               "blend=%u depth=%u topo=%u\n",
               hr, key->blend_enable, key->depth_enable, key->topology_type);
        return NULL;
    }

    s_d3d.pso_cache[s_d3d.pso_cache_count].key = *key;
    s_d3d.pso_cache[s_d3d.pso_cache_count].pso = pso;
    s_d3d.pso_cache_count++;
    printf("[D3D12] PSO cached #%u (fp=%d | blend=%u s=0x%X d=0x%X eq=0x%X | "
           "depth=%u write=%u func=0x%X | topo=%u)\n",
           s_d3d.pso_cache_count - 1, (int)key->fp_id, key->blend_enable,
           key->blend_sfactor, key->blend_dfactor, key->blend_equation,
           key->depth_enable, key->depth_write, key->depth_func,
           key->topology_type);
    return pso;
}

/* ---------------------------------------------------------------------------
 * Render a frame (clear + present)
 * -----------------------------------------------------------------------*/

static void render_frame(void)
{
    u32 fi = s_d3d.frame_index;

    /* Reset command allocator and list */
    s_d3d.cmd_allocators[fi]->lpVtbl->Reset(s_d3d.cmd_allocators[fi]);
    s_d3d.cmd_list->lpVtbl->Reset(s_d3d.cmd_list, s_d3d.cmd_allocators[fi], NULL);

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

    /* Viewport / scissor: prefer the game's request (current_rsx_state),
     * fall back to the swap-chain extents. RSX viewport_w/h are 0 until the
     * game first writes NV4097_SET_VIEWPORT_HORIZONTAL/VERTICAL. */
    D3D12_VIEWPORT viewport = {0, 0, (float)s_d3d.width, (float)s_d3d.height, 0.0f, 1.0f};
    D3D12_RECT     scissor  = {0, 0, (LONG)s_d3d.width, (LONG)s_d3d.height};
    const rsx_state* rs = s_d3d.current_rsx_state;
    if (rs && rs->viewport_w && rs->viewport_h) {
        viewport.TopLeftX = (float)rs->viewport_x;
        viewport.TopLeftY = (float)rs->viewport_y;
        viewport.Width    = (float)rs->viewport_w;
        viewport.Height   = (float)rs->viewport_h;
        viewport.MinDepth = rs->clip_min;
        viewport.MaxDepth = rs->clip_max;
    }
    if (rs && rs->scissor_w && rs->scissor_h) {
        scissor.left   = (LONG)rs->scissor_x;
        scissor.top    = (LONG)rs->scissor_y;
        scissor.right  = (LONG)(rs->scissor_x + rs->scissor_w);
        scissor.bottom = (LONG)(rs->scissor_y + rs->scissor_h);
    }
    s_d3d.cmd_list->lpVtbl->RSSetViewports(s_d3d.cmd_list, 1, &viewport);
    s_d3d.cmd_list->lpVtbl->RSSetScissorRects(s_d3d.cmd_list, 1, &scissor);

    /* Bind pipeline state and push MVP if anything to draw */
    if (s_d3d.pipeline_ready && s_d3d.draw_count > 0) {
        /* SRV heap + vertex buffer are independent of the root signature. */
        D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = {0};
        if (s_d3d.srv_heap) {
            ID3D12DescriptorHeap* heaps[] = { s_d3d.srv_heap };
            s_d3d.cmd_list->lpVtbl->SetDescriptorHeaps(s_d3d.cmd_list, 1, heaps);
            s_d3d.srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &srv_gpu);
        }
        s_d3d.cmd_list->lpVtbl->IASetVertexBuffers(s_d3d.cmd_list, 0, 1, &s_d3d.vb_view);

        /* MVP (placeholder VS) from vertex constants slots 0..3, identity if
         * the game wrote none. */
        float mvp[16];
        const rsx_state* st = s_d3d.current_rsx_state;
        int have_mvp = 0;
        if (st) {
            for (u32 r = 0; r < 4; r++)
                for (u32 c = 0; c < 4; c++) {
                    float v = st->vertex_constants[r][c];
                    mvp[r*4+c] = v; if (v != 0.0f) have_mvp = 1;
                }
        }
        if (!have_mvp) { memset(mvp,0,sizeof mvp); mvp[0]=mvp[5]=mvp[10]=mvp[15]=1.0f; }

        /* Refresh the vertex-constant CBV (decompiled VS path) from the RSX
         * constant bank: 512 vec4 into the 1024-slot buffer. */
        if (s_d3d.vp_const_mapped && st)
            memcpy(s_d3d.vp_const_mapped, st->vertex_constants,
                   sizeof(st->vertex_constants));
        D3D12_GPU_VIRTUAL_ADDRESS cbv_addr = s_d3d.vp_const_buffer
            ? s_d3d.vp_const_buffer->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_const_buffer) : 0;

        /* Replay each recorded draw, switching root signature + its bindings
         * when a draw flips between the placeholder-VS and decompiled-VP paths. */
        u32 last_topo = 0xFFFFFFFFu;
        ID3D12PipelineState* last_pso = NULL;
        int last_uses_vp = -1;
        u32 draws = s_d3d.draw_count;
        if (draws > MAX_DRAWS) draws = MAX_DRAWS;
        for (u32 d = 0; d < draws; d++) {
            const D3D12DrawRecord* dr = &s_d3d.draws[d];

            int uses_vp = vp_is_valid(dr->pso_key.vp_id) ? 1 : 0;
            if (uses_vp != last_uses_vp) {
                s_d3d.cmd_list->lpVtbl->SetGraphicsRootSignature(
                    s_d3d.cmd_list, uses_vp ? s_d3d.root_signature_vp : s_d3d.root_signature);
                if (s_d3d.srv_heap)
                    s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(s_d3d.cmd_list, 1, srv_gpu);
                if (uses_vp)
                    s_d3d.cmd_list->lpVtbl->SetGraphicsRootConstantBufferView(s_d3d.cmd_list, 0, cbv_addr);
                else
                    s_d3d.cmd_list->lpVtbl->SetGraphicsRoot32BitConstants(s_d3d.cmd_list, 0, 16, mvp, 0);
                last_uses_vp = uses_vp;
                last_pso = NULL; /* force PSO rebind under the new root sig */
            }

            /* Select a PSO matching this draw's blend/depth/topology state.
             * Fall back to the fixed topology-class PSO if the cache could
             * not produce one (shaders missing, cache full, create failed). */
            ID3D12PipelineState* target_pso = get_or_create_pso(&dr->pso_key);
            if (!target_pso) {
                target_pso = s_d3d.pipeline_state; /* default triangle */
                if (dr->topology == D3D_TOPOLOGY_POINTLIST) {
                    target_pso = s_d3d.pipeline_state_points
                                 ? s_d3d.pipeline_state_points : s_d3d.pipeline_state;
                } else if (dr->topology == D3D_TOPOLOGY_LINELIST ||
                           dr->topology == D3D_TOPOLOGY_LINESTRIP) {
                    target_pso = s_d3d.pipeline_state_lines
                                 ? s_d3d.pipeline_state_lines : s_d3d.pipeline_state;
                }
            }
            if (target_pso != last_pso) {
                s_d3d.cmd_list->lpVtbl->SetPipelineState(s_d3d.cmd_list, target_pso);
                last_pso = target_pso;
            }
            if (dr->topology != last_topo) {
                s_d3d.cmd_list->lpVtbl->IASetPrimitiveTopology(s_d3d.cmd_list, dr->topology);
                last_topo = dr->topology;
            }
            u32 start_vert = dr->vb_byte_offset / (u32)sizeof(RsxVertex);
            s_d3d.cmd_list->lpVtbl->DrawInstanced(
                s_d3d.cmd_list, dr->vertex_count, 1, start_vert, 0);
        }
    }
    s_d3d.vb_offset  = 0; /* reset for next frame */
    s_d3d.draw_count = 0;

    /* Transition render target to PRESENT state */
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);

    /* Close and execute */
    s_d3d.cmd_list->lpVtbl->Close(s_d3d.cmd_list);
    ID3D12CommandList* cmd_lists[] = {(ID3D12CommandList*)s_d3d.cmd_list};
    s_d3d.cmd_queue->lpVtbl->ExecuteCommandLists(s_d3d.cmd_queue, 1, cmd_lists);

    /* Present */
    s_d3d.swap_chain->lpVtbl->Present(s_d3d.swap_chain, 1, 0); /* vsync */

    move_to_next_frame();

    s_d3d.frame_count++;
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

static void d3d12_present(void* ud, u32 buffer_id)
{
    (void)ud;
    (void)buffer_id;

    if (s_d3d.initialized)
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
    s_d3d.clear_color[0] = ((color >> 16) & 0xFF) / 255.0f; /* R */
    s_d3d.clear_color[1] = ((color >> 8) & 0xFF) / 255.0f;  /* G */
    s_d3d.clear_color[2] = (color & 0xFF) / 255.0f;          /* B */
    s_d3d.clear_color[3] = ((color >> 24) & 0xFF) / 255.0f;  /* A */
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
    /* The actual D3D12_VIEWPORT is set per-command-list in end_frame, where
     * we read from s_d3d.current_rsx_state directly (so this callback only
     * needs to ensure the state pointer is up to date -- which it is, since
     * rsx_commands.c calls us with the new state and the begin_frame path
     * captured the pointer). No work needed here. */
    (void)state;
}

static inline u32 be32(const void* p) {
    u32 v; memcpy(&v, p, 4);
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v <<  8) & 0xFF0000) | ((v << 24) & 0xFF000000);
}

/* Decode RSX vertex attribute `a` at vertex `vidx` into out[4], filling
 * missing/absent components from `def`. Handles float32 (type 2, big-endian)
 * and normalized ubyte (type 4); other formats fall back to `def`. */
static void read_attrib(const rsx_vertex_attrib* a, u32 vidx,
                        const float def[4], float out[4])
{
    extern uint8_t* vm_base;
    out[0] = def[0]; out[1] = def[1]; out[2] = def[2]; out[3] = def[3];
    if (!a->enabled || !vm_base) return;
    u32 addr = a->offset + vidx * a->stride;
    if (addr >= 0x20000000) return;
    const u8* src = vm_base + addr;
    u32 n = a->size; if (n > 4) n = 4;
    if (a->type == 2) {            /* float32 */
        for (u32 i = 0; i < n; i++) { u32 v = be32(src + i * 4); memcpy(&out[i], &v, 4); }
    } else if (a->type == 4) {     /* ubyte, normalized */
        for (u32 i = 0; i < n; i++) out[i] = src[i] / 255.0f;
    }
}

/* Read one logical RSX vertex at index `vidx` into `out`, following cellGcm's
 * conventional attribute→semantic mapping (passthrough VP model): attrib 0 =
 * position, 3 = COLOR0, 4 = COLOR1, 5 = FOG, 8..15 = TEXCOORD0..7.
 * Returns 0 on success; 1 if there is no usable state. */
static int read_one_vertex(u32 vidx, RsxVertex* out)
{
    static const float d_pos[4]  = {0, 0, 0, 1};
    static const float d_col0[4] = {1, 1, 1, 1};
    static const float d_zero[4] = {0, 0, 0, 0};

    const rsx_state* st = s_d3d.current_rsx_state;
    if (!st) {
        out->pos[0] = out->pos[1] = out->pos[2] = 0.0f;
        out->col0[0] = out->col0[1] = out->col0[2] = out->col0[3] = 1.0f;
        memset(out->col1, 0, sizeof(out->col1));
        memset(out->fog,  0, sizeof(out->fog));
        memset(out->tc,   0, sizeof(out->tc));
        return 1;
    }

    /* Position (attrib 0). */
    const rsx_vertex_attrib* pos = &st->vertex_attribs[0];
    if (pos->enabled && pos->type == 2 && pos->size >= 3) {
        float p[4]; read_attrib(pos, vidx, d_pos, p);
        out->pos[0] = p[0]; out->pos[1] = p[1]; out->pos[2] = p[2];
    } else {
        /* No real position -- fall back to a placeholder circle. */
        float t = (float)vidx / 100.0f;
        out->pos[0] = sinf(t * 6.28f) * 0.5f;
        out->pos[1] = cosf(t * 6.28f) * 0.5f;
        out->pos[2] = 0.0f;
    }

    read_attrib(&st->vertex_attribs[3], vidx, d_col0, out->col0); /* COLOR0 */
    read_attrib(&st->vertex_attribs[4], vidx, d_zero, out->col1); /* COLOR1 */
    read_attrib(&st->vertex_attribs[5], vidx, d_zero, out->fog);  /* FOG    */
    for (u32 t = 0; t < 8; t++)
        read_attrib(&st->vertex_attribs[8 + t], vidx, d_zero, out->tc[t]); /* TC0..7 */
    return 0;
}

/* Upload a sequential range of vertices [first .. first+count). Returns the
 * actual count written (may be capped by per-frame buffer space). */
static u32 upload_vertices_from_rsx(u32 first, u32 count)
{
    RsxVertex* verts = (RsxVertex*)((u8*)s_d3d.vb_mapped + s_d3d.vb_offset);
    u32 max_verts = (MAX_VERTICES * sizeof(RsxVertex) - s_d3d.vb_offset) / sizeof(RsxVertex);
    if (count > max_verts) count = max_verts;
    for (u32 i = 0; i < count; i++) read_one_vertex(first + i, &verts[i]);
    s_d3d.vb_offset += count * sizeof(RsxVertex);
    return count;
}

/* Read `count` indices from guest memory (per current rsx_state.index_array_*),
 * resolve each into an RsxVertex via read_one_vertex, and stage the resulting
 * flat vertex array. Returns the actual vertex count written. We expand on
 * the CPU (instead of using a real D3D12 index buffer) so the same draw
 * record path used by draw_arrays handles the result -- a future pass can
 * switch to a real GPU index buffer when the per-call vertex count justifies
 * it. */
static u32 upload_vertices_from_indices(u32 index_byte_offset, u32 count)
{
    extern uint8_t* vm_base;
    const rsx_state* state = s_d3d.current_rsx_state;
    if (!state || !vm_base) return 0;

    u32 stride = (state->index_array_format == RSX_INDEX_FORMAT_U16) ? 2 : 4;
    u32 base   = state->index_array_offset + index_byte_offset * stride;
    if (base >= 0x20000000) return 0;        /* sanity */

    RsxVertex* verts = (RsxVertex*)((u8*)s_d3d.vb_mapped + s_d3d.vb_offset);
    u32 max_verts = (MAX_VERTICES * sizeof(RsxVertex) - s_d3d.vb_offset) / sizeof(RsxVertex);
    if (count > max_verts) count = max_verts;

    for (u32 i = 0; i < count; i++) {
        u8* src = vm_base + base + i * stride;
        u32 idx;
        if (state->index_array_format == RSX_INDEX_FORMAT_U16) {
            idx = ((u32)src[0] << 8) | src[1];      /* BE u16 */
        } else {
            idx = be32(src);                          /* BE u32 */
        }
        read_one_vertex(idx, &verts[i]);
    }
    s_d3d.vb_offset += count * sizeof(RsxVertex);
    return count;
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

    /* One-shot: dump the MVP and first vertex on the very first draw so we
     * can see what coordinate space the game is sending positions in. */
    static int s_dumped = 0;
    if (!s_dumped && s_d3d.current_rsx_state) {
        extern uint8_t* vm_base;
        s_dumped = 1;
        const rsx_state* st = s_d3d.current_rsx_state;
        printf("[D3D12-DUMP] vertex_constants slots 0..7:\n");
        for (u32 i = 0; i < 8; i++) {
            printf("  [%u] = (% .4f, % .4f, % .4f, % .4f)\n", i,
                   st->vertex_constants[i][0], st->vertex_constants[i][1],
                   st->vertex_constants[i][2], st->vertex_constants[i][3]);
        }
        printf("[D3D12-DUMP] vc dirty=%d range=[%u..%u]\n",
               st->vertex_constants_dirty,
               st->vertex_constants_lo, st->vertex_constants_hi);
        printf("[D3D12-DUMP] viewport=%ux%u clip=%ux%u\n",
               st->viewport_w, st->viewport_h,
               st->surface_clip_w, st->surface_clip_h);
        const rsx_vertex_attrib* pos = &st->vertex_attribs[0];
        printf("[D3D12-DUMP] attrib0: enabled=%d type=%u size=%u stride=%u offset=0x%08X\n",
               pos->enabled, pos->type, pos->size, pos->stride, pos->offset);
        if (pos->enabled && pos->type == 2 && vm_base) {
            for (u32 v = 0; v < (count < 4 ? count : 4); v++) {
                u32 addr = pos->offset + (first + v) * pos->stride;
                if (addr >= 0x20000000) break;
                u8* src = vm_base + addr;
                u32 fx, fy, fz;
                memcpy(&fx, src,     4); fx = ((fx>>24)&0xFF)|((fx>>8)&0xFF00)|((fx<<8)&0xFF0000)|((fx<<24)&0xFF000000);
                memcpy(&fy, src + 4, 4); fy = ((fy>>24)&0xFF)|((fy>>8)&0xFF00)|((fy<<8)&0xFF0000)|((fy<<24)&0xFF000000);
                memcpy(&fz, src + 8, 4); fz = ((fz>>24)&0xFF)|((fz>>8)&0xFF00)|((fz<<8)&0xFF0000)|((fz<<24)&0xFF000000);
                float x, y, z;
                memcpy(&x, &fx, 4); memcpy(&y, &fy, 4); memcpy(&z, &fz, 4);
                printf("[D3D12-DUMP] v[%u] pos=(% .4f, % .4f, % .4f)\n", v, x, y, z);
            }
        }
    }

    u32 topo = rsx_to_d3d12_topology(primitive);
    if (topo == D3D_TOPOLOGY_UNDEFINED) {
        /* Skip primitives that still need index-buffer conversion
         * (quads, line loops, triangle fans) rather than silently
         * rendering them as the wrong shape. */
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
        s_d3d.draws[s_d3d.draw_count].pso_key        =
            pso_key_from_state(s_d3d.current_rsx_state, topo,
                               s_d3d.current_fp_id, s_d3d.current_vp_id);
        s_d3d.draw_count++;
    }
}

static void d3d12_draw_indexed(void* ud, u32 primitive, u32 offset, u32 count)
{
    (void)ud;
    static u64 s_total = 0;
    if (s_total < 20 || (s_total % 1000) == 0) {
        printf("[D3D12] draw_indexed #%llu prim=%u offset=%u count=%u\n",
               (unsigned long long)s_total, primitive, offset, count);
    }
    s_total++;

    if (!s_d3d.pipeline_ready || !s_d3d.vb_mapped) return;
    if (count == 0 || count > MAX_VERTICES) return;

    u32 topo = rsx_to_d3d12_topology(primitive);
    if (topo == D3D_TOPOLOGY_UNDEFINED) {
        static int s_skipped = 0;
        if (s_skipped < 3) {
            printf("[D3D12] draw_indexed: skipping prim=%u (needs index conversion)\n",
                   primitive);
            s_skipped++;
        }
        return;
    }

    /* Resolve indices on the CPU into a flat vertex stream. The same draw
     * record path that draw_arrays uses then dispatches a DrawInstanced
     * with the expanded count -- no D3D12 index buffer needed for now. */
    u32 record_offset = s_d3d.vb_offset;
    u32 actual_count  = upload_vertices_from_indices(offset, count);
    if (actual_count == 0) return;

    if (s_d3d.draw_count < MAX_DRAWS) {
        s_d3d.draws[s_d3d.draw_count].vb_byte_offset = record_offset;
        s_d3d.draws[s_d3d.draw_count].vertex_count   = actual_count;
        s_d3d.draws[s_d3d.draw_count].topology       = topo;
        s_d3d.draws[s_d3d.draw_count].pso_key        =
            pso_key_from_state(s_d3d.current_rsx_state, topo,
                               s_d3d.current_fp_id, s_d3d.current_vp_id);
        s_d3d.draw_count++;
    }
}

/* Wait for the dedicated upload queue work to finish. */
static void upload_flush(void)
{
    u64 v = ++s_d3d.upload_fence_value;
    s_d3d.cmd_queue->lpVtbl->Signal(s_d3d.cmd_queue, s_d3d.upload_fence, v);
    if (s_d3d.upload_fence->lpVtbl->GetCompletedValue(s_d3d.upload_fence) < v) {
        s_d3d.upload_fence->lpVtbl->SetEventOnCompletion(s_d3d.upload_fence, v, s_d3d.upload_event);
        WaitForSingleObject(s_d3d.upload_event, INFINITE);
    }
}

/* Upload a linear, uncompressed RSX texture into SRV slot `unit`. Returns 0 on
 * success. Swizzled / block-compressed / unsupported formats are rejected by
 * the caller (the slot keeps its null SRV). */
static int upload_texture(u32 unit, u32 guest_off, u32 dxgi, u32 bpp,
                          u32 width, u32 height)
{
    extern uint8_t* vm_base;
    u32 src_pitch = (width * bpp) / 8;
    if (src_pitch == 0) return -1;
    if ((u64)guest_off + (u64)src_pitch * height > 0x20000000ull) return -1; /* bounds */

    /* 1. Destination texture (DEFAULT heap, COPY_DEST). */
    D3D12_HEAP_PROPERTIES dheap = {0}; dheap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {0};
    td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width            = width;
    td.Height           = height;
    td.DepthOrArraySize = 1;
    td.MipLevels        = 1;
    td.Format           = (DXGI_FORMAT)dxgi;
    td.SampleDesc.Count = 1;
    td.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    ID3D12Resource* texres = NULL;
    HRESULT hr = s_d3d.device->lpVtbl->CreateCommittedResource(
        s_d3d.device, &dheap, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&texres);
    if (FAILED(hr)) return -1;

    /* 2. Copyable footprint + matching upload buffer. */
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT rows; UINT64 row_bytes, total;
    s_d3d.device->lpVtbl->GetCopyableFootprints(
        s_d3d.device, &td, 0, 1, 0, &fp, &rows, &row_bytes, &total);

    D3D12_HEAP_PROPERTIES uheap = {0}; uheap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width            = total;
    bd.Height           = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels        = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* upbuf = NULL;
    hr = s_d3d.device->lpVtbl->CreateCommittedResource(
        s_d3d.device, &uheap, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&upbuf);
    if (FAILED(hr)) { texres->lpVtbl->Release(texres); return -1; }

    /* 3. Copy rows from guest memory into the upload buffer (row-pitch aligned).
     * Linear layout assumed -- swizzled textures are rejected by the caller. */
    u8* dst = NULL;
    D3D12_RANGE no_read = {0, 0};
    upbuf->lpVtbl->Map(upbuf, 0, &no_read, (void**)&dst);
    const u8* src = vm_base + guest_off;
    u32 copy = (u32)row_bytes; if (copy > src_pitch) copy = src_pitch;
    for (UINT r = 0; r < rows; r++)
        memcpy(dst + fp.Offset + (u64)r * fp.Footprint.RowPitch,
               src + (u64)r * src_pitch, copy);
    upbuf->lpVtbl->Unmap(upbuf, 0, NULL);

    /* 4. Record copy + transition to shader-resource on the upload list. */
    s_d3d.upload_alloc->lpVtbl->Reset(s_d3d.upload_alloc);
    s_d3d.upload_list->lpVtbl->Reset(s_d3d.upload_list, s_d3d.upload_alloc, NULL);

    D3D12_TEXTURE_COPY_LOCATION dl; memset(&dl, 0, sizeof(dl));
    dl.pResource = texres; dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dl.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION sl; memset(&sl, 0, sizeof(sl));
    sl.pResource = upbuf;  sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; sl.PlacedFootprint = fp;
    s_d3d.upload_list->lpVtbl->CopyTextureRegion(s_d3d.upload_list, &dl, 0, 0, 0, &sl, NULL);

    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = texres;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.upload_list->lpVtbl->ResourceBarrier(s_d3d.upload_list, 1, &b);

    s_d3d.upload_list->lpVtbl->Close(s_d3d.upload_list);
    ID3D12CommandList* lists[] = { (ID3D12CommandList*)s_d3d.upload_list };
    s_d3d.cmd_queue->lpVtbl->ExecuteCommandLists(s_d3d.cmd_queue, 1, lists);
    upload_flush();
    upbuf->lpVtbl->Release(upbuf);

    /* 5. SRV into slot `unit`, replacing the null descriptor. */
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    s_d3d.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.srv_heap, &h);
    h.ptr += (size_t)unit * s_d3d.srv_descriptor_size;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {0};
    srv.Format                  = (DXGI_FORMAT)dxgi;
    srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels     = 1;
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, texres, &srv, h);

    if (s_d3d.unit_textures[unit]) s_d3d.unit_textures[unit]->lpVtbl->Release(s_d3d.unit_textures[unit]);
    s_d3d.unit_textures[unit] = texres;
    return 0;
}

static void d3d12_bind_texture(void* ud, u32 unit, const rsx_texture_state* tex)
{
    (void)ud;
    extern uint8_t* vm_base;

    u32 width  = (tex->image_rect >> 16) & 0xFFFF;
    u32 height = tex->image_rect & 0xFFFF;
    u32 fmtword = tex->format;
    u32 format = rsx_texture_get_format(fmtword);
    u32 offset = tex->offset;

    if (unit >= FP_TEXTURE_SLOTS || !vm_base) return;
    if (width == 0 || height == 0 || offset >= 0x20000000) return;

    /* Skip re-upload if this unit already holds an identical texture. */
    u32 key = offset ^ (fmtword * 2654435761u) ^ (width << 16) ^ height;
    if (s_d3d.unit_textures[unit] && s_d3d.unit_tex_key[unit] == key) return;

    u32 dxgi = rsx_to_dxgi_texture_format(format);
    int swizzled   = rsx_texture_is_swizzled(fmtword);
    int compressed = (format == RSX_TEXTURE_COMPRESSED_DXT1 ||
                      format == RSX_TEXTURE_COMPRESSED_DXT23 ||
                      format == RSX_TEXTURE_COMPRESSED_DXT45);

    static int log_count = 0;
    if (log_count < 10) {
        printf("[D3D12] bind_texture(unit=%u off=0x%X fmt=0x%02X %ux%u dxgi=%u%s%s)\n",
               unit, offset, format, width, height, dxgi,
               swizzled ? " SWZ" : "", compressed ? " DXT" : "");
        log_count++;
    }

    /* MVP scope: linear, uncompressed, single mip. Anything else keeps the
     * unit's null SRV (samples 0) -- deswizzle / block formats are future. */
    if (dxgi == 0 || swizzled || compressed) return;

    u32 bpp = rsx_texture_bpp(format);
    if (upload_texture(unit, offset, dxgi, bpp, width, height) == 0) {
        s_d3d.unit_tex_key[unit] = key;
        if (log_count <= 10)
            printf("[D3D12]   uploaded unit %u (%ux%u) -> SRV slot %u\n",
                   unit, width, height, unit);
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

    /* Resolve (decompile + compile + cache) the fragment program. The result
     * index feeds the PSO key so the PSO cache builds a pipeline per
     * (fragment program × blend × depth × topology). On any failure
     * current_fp_id stays 0xFFFFFFFF and draws use the placeholder PS. */
    s_d3d.current_fp_id = resolve_fragment_program(state->fragment_program_addr);
    /* Likewise resolve the vertex (transform) program; 0xFFFFFFFF keeps the
     * placeholder MVP vertex shader. */
    s_d3d.current_vp_id = resolve_vertex_program(state);

    static int log_count = 0;
    if (log_count < 5) {
        printf("[D3D12] set_shader: fp_addr=0x%08X -> fp_id=%d, vp_id=%d (%u words), output_mask=0x%08X\n",
               state->fragment_program_addr, (int)s_d3d.current_fp_id,
               (int)s_d3d.current_vp_id, state->transform_program_words,
               state->vertex_attrib_output_mask);
        log_count++;
    }
}

static void d3d12_set_blend(void* ud, const rsx_state* state)
{
    (void)ud;
    /* The blend configuration is baked into a PSO at draw time: each draw
     * record snapshots the current rsx_state via pso_key_from_state(), and
     * render_frame resolves it through the PSO cache (get_or_create_pso).
     * Nothing to do here beyond diagnostics. */
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
    /* Depth state is likewise baked into the per-draw PSO (see d3d12_set_blend).
     * This callback is diagnostics-only. */
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
    s_d3d.current_fp_id = 0xFFFFFFFFu; /* placeholder PS until a game sets one */
    s_d3d.current_vp_id = 0xFFFFFFFFu; /* placeholder MVP VS until a game sets one */
    s_d3d.clear_color[0] = 0.0f;
    s_d3d.clear_color[1] = 0.0f;
    s_d3d.clear_color[2] = 0.1f;
    s_d3d.clear_color[3] = 1.0f;

    /* Create window */
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
    for (u32 i = 0; i < s_d3d.pso_cache_count; i++)
        if (s_d3d.pso_cache[i].pso) s_d3d.pso_cache[i].pso->lpVtbl->Release(s_d3d.pso_cache[i].pso);
    for (u32 i = 0; i < s_d3d.fp_cache_count; i++)
        if (s_d3d.fp_cache[i].ps_blob) s_d3d.fp_cache[i].ps_blob->lpVtbl->Release(s_d3d.fp_cache[i].ps_blob);
    if (s_d3d.srv_heap) s_d3d.srv_heap->lpVtbl->Release(s_d3d.srv_heap);
    for (u32 i = 0; i < FP_TEXTURE_SLOTS; i++)
        if (s_d3d.unit_textures[i]) s_d3d.unit_textures[i]->lpVtbl->Release(s_d3d.unit_textures[i]);
    if (s_d3d.upload_list)  s_d3d.upload_list->lpVtbl->Release(s_d3d.upload_list);
    if (s_d3d.upload_alloc) s_d3d.upload_alloc->lpVtbl->Release(s_d3d.upload_alloc);
    if (s_d3d.upload_fence) s_d3d.upload_fence->lpVtbl->Release(s_d3d.upload_fence);
    if (s_d3d.upload_event) CloseHandle(s_d3d.upload_event);
    if (s_d3d.vs_blob) s_d3d.vs_blob->lpVtbl->Release(s_d3d.vs_blob);
    if (s_d3d.ps_blob) s_d3d.ps_blob->lpVtbl->Release(s_d3d.ps_blob);
    if (s_d3d.depth_buffer) s_d3d.depth_buffer->lpVtbl->Release(s_d3d.depth_buffer);
    if (s_d3d.dsv_heap)     s_d3d.dsv_heap->lpVtbl->Release(s_d3d.dsv_heap);
    for (u32 i = 0; i < s_d3d.vp_cache_count; i++)
        if (s_d3d.vp_cache[i].vs_blob) s_d3d.vp_cache[i].vs_blob->lpVtbl->Release(s_d3d.vp_cache[i].vs_blob);
    if (s_d3d.vp_const_buffer) {
        s_d3d.vp_const_buffer->lpVtbl->Unmap(s_d3d.vp_const_buffer, 0, NULL);
        s_d3d.vp_const_buffer->lpVtbl->Release(s_d3d.vp_const_buffer);
    }
    if (s_d3d.root_signature_vp) s_d3d.root_signature_vp->lpVtbl->Release(s_d3d.root_signature_vp);
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
    if (s_d3d.initialized)
        render_frame();
}

#else /* !_WIN32 */

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
