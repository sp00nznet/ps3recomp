/* D3D12 register-file engine backend on a real adapter, headless, with the
 * debug layer when it is installed (any error message fails the run):
 *  - clear, then a decompiler-shaped pipeline (ATTRn inputs, VPConst at b0,
 *    PSConstants at b1) draws a full-target triangle whose colour comes from
 *    both constant blocks; the readback is exact;
 *  - an indexed draw, a scissor and a viewport restrict where it lands;
 *  - MRT: two targets of one size and format both take the draw;
 *  - depth: a nearer draw wins, a farther one is rejected, then a cleared
 *    depth accepts it again;
 *  - texture sampling with the TEXTURE_CONTROL1 crossbar (a channel rotation)
 *    and a surface view of a colour target;
 *  - color_target_write lands rows at an offset;
 *  - released objects are retired on the fence and their handles reused;
 *  - every refusal is counted, not silently skipped.
 * No window, no swapchain, no guest memory and no game data.
 * python3 tools/test_d3d12_engine_backend.py */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../rsx_d3d12_backend.c"
#include <assert.h>
#include <math.h>

/* The fixture has no guest: none of these may be reached on its paths. */
u8* vm_base; u32 ppu_vm_size, ppu_hle_inject_base;
static void no_guest(const char* what) { fprintf(stderr, "FAIL fixture reached guest-side %s\n", what); exit(1); }
unsigned cellGcm_flip_request_count(void) { no_guest("flip count"); return 0; }
int cellGcm_current_scanout(u32* off, u32* pitch, u32* w, u32* h) { (void)off; (void)pitch; (void)w; (void)h; no_guest("scanout"); return 0; }
u32 cellGcmResolveOffset(u32 offset) { (void)offset; no_guest("offset"); return 0; }
u32 cellGcmResolveLocated(int location, u32 offset) { (void)location; (void)offset; no_guest("located offset"); return 0; }
u32 cellGcmResolveIO(u32 offset) { (void)offset; no_guest("IO offset"); return 0; }
int cellGcmOffsetIsDisplay(u32 offset) { (void)offset; no_guest("display offset"); return 0; }
u32 rsx_find_vram_upload(u32 size) { (void)size; no_guest("vram upload"); return 0; }
void rsx_reset_upload_claims(void) { no_guest("upload claims"); }
void cellGcm_replay_display_buffers_to_engine(void) {}
u32 gcm_io_offset_to_ea(u32 offset) { (void)offset; no_guest("IO map"); return 0; }
void vm_write32(uint32_t a, uint32_t v) { (void)a; (void)v; no_guest("memory write"); }
void ps3_ms(const char* key) { (void)key; }
unsigned long long ps3_ms_now(void) { return 0; }
struct spu_context;
uint32_t spurs_policy_service_identity(const struct spu_context* c) { (void)c; return 0; }

static int failures, checks;
#define CHECK(v, name) do { checks++; if (!(v)) { failures++; fprintf(stderr, "FAIL %s\n", name); } \
                            else fprintf(stderr, "ok   %s\n", name); } while (0)

static const char VS[] =
    "struct VSInput { float4 a0:ATTR0; float4 a8:ATTR8; };\n"
    "struct VSOutput { float4 pos:SV_Position; float4 col0:COLOR0; float4 tc0:TEXCOORD0; };\n"
    "cbuffer VPConst : register(b0) { float4 vp_c[512]; float4 vp_posscale; float4 vp_posoffset; };\n"
    "VSOutput main(VSInput input) { VSOutput o;\n"
    "  o.pos = float4(input.a0.xyz * vp_posscale.xyz + input.a0.w * vp_posoffset.xyz, input.a0.w);\n"
    "  o.col0 = vp_c[1];\n"
    "  o.tc0 = input.a8; return o; }\n";
static const char PS[] =
    "struct PSInput { float4 pos:SV_Position; float4 col0:COLOR0; float4 tc0:TEXCOORD0; };\n"
    "cbuffer PSConstants : register(b1) { float4 fp_c[2]; };\n"
    "float4 main(PSInput input) : SV_TARGET { return input.col0 * fp_c[0]; }\n";
static const char PS_TEX[] =
    "struct PSInput { float4 pos:SV_Position; float4 col0:COLOR0; float4 tc0:TEXCOORD0; };\n"
    "Texture2D    rsx_tex[16] : register(t0);\n"
    "SamplerState rsx_samp[16] : register(s0);\n"
    "float4 main(PSInput input) : SV_TARGET { return rsx_tex[0].Sample(rsx_samp[0], input.tc0.xy); }\n";
static const char PS_MRT[] =
    "struct PSInput { float4 pos:SV_Position; float4 col0:COLOR0; float4 tc0:TEXCOORD0; };\n"
    "struct PSOutput { float4 c0:SV_Target0; float4 c1:SV_Target1; };\n"
    "PSOutput main(PSInput input) { PSOutput o; o.c0 = input.col0; o.c1 = input.col0.bgra; return o; }\n";

/* Two attribute slots (ATTR0 position, ATTR8 texcoord), one float4 each. */
typedef struct { float pos[4]; float tc[4]; } vtx;
static const vtx FULL[3] = { { { -1, -1, 0.5f, 1 }, { 0, 1, 0, 0 } }, { { -1, 3, 0.5f, 1 }, { 0, -1, 0, 0 } },
                             { { 3, -1, 0.5f, 1 }, { 2, 1, 0, 0 } } };

static rsx_be_render_state plain_state(void)
{
    rsx_be_render_state rs; memset(&rs, 0, sizeof rs);
    rs.color_mask = 0x01010101u; rs.depth_func = 0x0203u; /* LEQUAL */
    rs.s_func = 0x0207u; rs.s_func_mask = 0xFF; rs.s_write_mask = 0xFF;
    rs.s_fail = rs.s_zfail = rs.s_zpass = 0x1E00u;
    return rs;
}
static rsx_vertex_layout_plan layout2(void)
{
    rsx_vertex_layout_plan l; memset(&l, 0, sizeof l);
    l.count = 2; l.attrs[0] = 0; l.attrs[1] = 8; l.stride = sizeof(vtx);
    return l;
}
static u32 px(const rsx_draw_backend* b, u32 rt, u32 x, u32 y)
{
    u32 v = 0; b->readback(b->user, rt, x, y, 1, 1, &v, 4); return v;
}
static void draw_full(const rsx_draw_backend* b, u32 pipe)
{
    b->bind_pipeline(b->user, pipe);
    b->draw(b->user, RSX_TOPOLOGY_TRIANGLES, FULL, 3, sizeof(vtx), NULL, 0);
}

static int open_device(void)
{
    const char* v = getenv("D3D12_ENGINE_TEST_VALIDATION");
    if (v && *v == '1') {
        ID3D12Debug* dbg = NULL;
        if (FAILED(D3D12GetDebugInterface(&IID_ID3D12Debug, (void**)&dbg))) { fprintf(stderr, "SKIP: no debug layer\n"); return 2; }
        dbg->lpVtbl->EnableDebugLayer(dbg); dbg->lpVtbl->Release(dbg);
    }
    if (FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&s_d3d.device))) {
        fprintf(stderr, "SKIP: no D3D12 device\n"); return 2;
    }
    D3D12_COMMAND_QUEUE_DESC qd; memset(&qd, 0, sizeof qd); qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommandQueue(s_d3d.device, &qd, &IID_ID3D12CommandQueue, (void**)&s_d3d.cmd_queue)))
        return 2;
    return 0;
}
static int validation_errors(void)
{
    ID3D12InfoQueue* iq = NULL; int errors = 0;
    if (FAILED(s_d3d.device->lpVtbl->QueryInterface(s_d3d.device, &IID_ID3D12InfoQueue, (void**)&iq)) || !iq) return 0;
    const UINT64 n = iq->lpVtbl->GetNumStoredMessages(iq);
    for (UINT64 i = 0; i < n; i++) {
        SIZE_T len = 0; iq->lpVtbl->GetMessage(iq, i, NULL, &len);
        D3D12_MESSAGE* m = (D3D12_MESSAGE*)malloc(len);
        if (m && SUCCEEDED(iq->lpVtbl->GetMessage(iq, i, m, &len)) && m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
            fprintf(stderr, "[validation error] %s\n", m->pDescription); errors++;
        }
        free(m);
    }
    iq->lpVtbl->Release(iq);
    return errors;
}


/* A hidden window and flip-model swap chain on the test queue, so the
 * engine's present path runs for real. Returns 0 when none can be made (for
 * example a non-interactive session); the present checks then skip. */
static LRESULT CALLBACK test_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }
static int open_swap_chain(u32 w, u32 h)
{
    WNDCLASSW wc; memset(&wc, 0, sizeof wc); wc.lpfnWndProc = test_wndproc; wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"e12_present_fixture"; RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"e12", WS_OVERLAPPEDWINDOW, 0, 0, (int)w, (int)h, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) return 0;
    IDXGIFactory4* f = NULL;
    if (FAILED(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void**)&f))) return 0;
    DXGI_SWAP_CHAIN_DESC1 d; memset(&d, 0, sizeof d);
    d.Width = w; d.Height = h; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = FRAME_COUNT; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc1 = NULL;
    const HRESULT hr = f->lpVtbl->CreateSwapChainForHwnd(f, (IUnknown*)s_d3d.cmd_queue, hwnd, &d, NULL, NULL, &sc1);
    f->lpVtbl->Release(f);
    if (FAILED(hr) || !sc1) { fprintf(stderr, "     no swap chain here (0x%08lX)\n", (long)hr); return 0; }
    sc1->lpVtbl->QueryInterface(sc1, &IID_IDXGISwapChain3, (void**)&s_d3d.swap_chain); sc1->lpVtbl->Release(sc1);
    D3D12_DESCRIPTOR_HEAP_DESC hd; memset(&hd, 0, sizeof hd); hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = FRAME_COUNT;
    if (FAILED(s_d3d.device->lpVtbl->CreateDescriptorHeap(s_d3d.device, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_d3d.rtv_heap))) return 0;
    s_d3d.rtv_descriptor_size = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE c; s_d3d.rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rtv_heap, &c);
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        if (FAILED(s_d3d.swap_chain->lpVtbl->GetBuffer(s_d3d.swap_chain, i, &IID_ID3D12Resource, (void**)&s_d3d.render_targets[i]))) return 0;
        D3D12_CPU_DESCRIPTOR_HANDLE ci = c; ci.ptr += (SIZE_T)i * s_d3d.rtv_descriptor_size;
        s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device, s_d3d.render_targets[i], NULL, ci);
    }
    s_d3d.width = w; s_d3d.height = h;
    return 1;
}
int main(void)
{
    const int open = open_device();
    if (open) return open;
    InitializeCriticalSection(&s_e12.lock);
    const rsx_draw_backend* b = &s_d3d12_engine_backend;
    CHECK(b->init(b->user, 64, 64) == 0, "init");
    if (!s_e12.ready) return 1;

    const u32 rt = b->color_target_create(b->user, RSX_BE_FMT_R8G8B8A8, 64, 64, NULL, 0);
    CHECK(rt != 0, "colour target");
    const float red[4] = { 1, 0, 0, 1 };
    b->clear_color(b->user, rt, red);
    CHECK(px(b, rt, 5, 5) == 0xFF0000FFu, "clear reads back R,G,B,A");

    rsx_be_render_state rs = plain_state();
    rsx_vertex_layout_plan lay = layout2();
    const u32 pipe = b->pipeline_create(b->user, VS, PS, &rs, &lay, sizeof(vtx), RSX_BE_FMT_R8G8B8A8, 1);
    CHECK(pipe != 0, "pipeline from decompiler-shaped HLSL (VPConst with the viewport epilogue)");
    /* The engine's block: 512 transform constants, then the viewport epilogue
     * (scale, offset) the decompiled program's tail applies. */
    static float vc[514 * 4]; float fc[8] = { 0 };
    vc[512 * 4 + 0] = vc[512 * 4 + 1] = vc[512 * 4 + 2] = 1.0f;   /* vp_posscale; vp_posoffset stays 0 */
    vc[4] = 0.0f; vc[5] = 1.0f; vc[6] = 0.5f; vc[7] = 1.0f;      /* vp_c[1] */
    fc[0] = 1.0f; fc[1] = 1.0f; fc[2] = 1.0f; fc[3] = 1.0f;      /* fp_c[0] */
    b->bind_vs_constants(b->user, vc, sizeof vc);
    b->bind_ps_constants(b->user, fc, sizeof fc);
    b->bind_textures(b->user, NULL, NULL, 0);
    b->bind_vertex_textures(b->user, NULL, NULL, 0);
    b->bind_targets(b->user, &rt, 1, 0);
    draw_full(b, pipe);
    const u32 green = px(b, rt, 32, 32);
    CHECK((green & 0xFF) == 0 && ((green >> 8) & 0xFF) == 0xFF && ((green >> 16) & 0xFF) >= 0x7F &&
          ((green >> 16) & 0xFF) <= 0x80 && (green >> 24) == 0xFF, "draw colour from both constant blocks");

    /* Scissor: only the left half. */
    b->clear_color(b->user, rt, red);
    b->set_scissor(b->user, 0, 0, 32, 64);
    fc[0] = 0.0f;                                                  /* zero red channel */
    b->bind_ps_constants(b->user, fc, sizeof fc);
    draw_full(b, pipe);
    CHECK(px(b, rt, 10, 10) != 0xFF0000FFu && px(b, rt, 50, 10) == 0xFF0000FFu, "scissor bounds the draw");
    b->set_scissor(b->user, 0, 0, 64, 64);

    /* Indexed draw. */
    static const u32 idx[3] = { 2, 1, 0 };
    b->clear_color(b->user, rt, red);
    b->bind_pipeline(b->user, pipe);
    b->draw(b->user, RSX_TOPOLOGY_TRIANGLES, FULL, 3, sizeof(vtx), idx, 3);
    CHECK(px(b, rt, 32, 32) != 0xFF0000FFu, "indexed draw");

    /* Depth: draws at z 0.5; a target cleared to 0.25 rejects them under LEQUAL. */
    rsx_be_render_state rsd = plain_state(); rsd.depth_test = 1; rsd.depth_write = 1;
    const u32 pdepth = b->pipeline_create(b->user, VS, PS, &rsd, &lay, sizeof(vtx), RSX_BE_FMT_R8G8B8A8, 1);
    const u32 z = b->depth_target_create(b->user, 64, 64);
    CHECK(pdepth && z, "depth pipeline and target");
    b->clear_color(b->user, rt, red);
    b->clear_depth_stencil(b->user, z, RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL, 0.25f, 0);
    b->bind_targets(b->user, &rt, 1, z);
    draw_full(b, pdepth);
    CHECK(px(b, rt, 32, 32) == 0xFF0000FFu, "farther draw rejected by depth");
    b->clear_depth_stencil(b->user, z, RSX_BE_CLEAR_DEPTH, 1.0f, 0);
    draw_full(b, pdepth);
    CHECK(px(b, rt, 32, 32) != 0xFF0000FFu, "cleared depth accepts it");
    /* Depth readback: the cleared 1.0 outside the draw, the draw's 0.5 inside. */
    CHECK(b->depth_readback != NULL, "depth readback present");
    {
        float dv[4 * 3]; memset(dv, 0, sizeof dv);
        CHECK(b->depth_readback(b->user, z, 30, 31, 4, 3, dv, 4 * sizeof(float)) == 0, "depth readback succeeds");
        int all_half = 1; for (int i = 0; i < 12; i++) if (dv[i] < 0.499f || dv[i] > 0.501f) all_half = 0;
        CHECK(all_half, "depth readback reads the drawn depth 0.5 in a sub-rectangle");
        b->clear_depth_stencil(b->user, z, RSX_BE_CLEAR_DEPTH, 0.75f, 0);
        float corner = 0; CHECK(b->depth_readback(b->user, z, 63, 63, 1, 1, &corner, 4) == 0 && corner == 0.75f,
                                "depth readback sees a later clear (0.75)");
        CHECK(b->depth_readback(b->user, z, 60, 60, 8, 8, dv, 32) != 0, "out-of-bounds depth readback refused");
        draw_full(b, pdepth);   /* the target is still usable for depth after a readback */
        CHECK(b->depth_readback(b->user, z, 32, 32, 1, 1, &corner, 4) == 0 && corner > 0.499f && corner < 0.501f,
              "depth test and write still work after readback");
    }
    const u32 snap = b->depth_snapshot(b->user, z, 64, 64);
    CHECK(snap != 0, "depth snapshot");
    b->texture_release(b->user, snap);

    /* MRT. */
    const u32 rt2 = b->color_target_create(b->user, RSX_BE_FMT_R8G8B8A8, 64, 64, NULL, 0);
    const u32 pmrt = b->pipeline_create(b->user, VS, PS_MRT, &rs, &lay, sizeof(vtx), RSX_BE_FMT_R8G8B8A8, 2);
    const u32 pair[2] = { rt, rt2 };
    vc[4] = 1.0f; vc[5] = 0.0f; vc[6] = 0.0f; vc[7] = 1.0f;       /* red in c0, blue in c1 */
    b->bind_vs_constants(b->user, vc, sizeof vc);
    b->bind_targets(b->user, pair, 2, 0);
    draw_full(b, pmrt);
    CHECK(px(b, rt, 8, 8) == 0xFF0000FFu && px(b, rt2, 8, 8) == 0xFFFF0000u, "MRT writes both targets");

    /* Texture with a crossbar, sampled point-wise. Texel bytes R,G,B,A = 10,20,30,40. */
    u8 texel[4 * 4 * 4];
    for (int i = 0; i < 16; i++) { texel[4 * i] = 10; texel[4 * i + 1] = 20; texel[4 * i + 2] = 30; texel[4 * i + 3] = 40; }
    const u32 identity = 0xE4u;   /* TEXTURE_CONTROL1 for an identity A8R8G8B8 crossbar (see rsx_texture_layout.h) */
    const u32 tex = b->texture_create(b->user, RSX_BE_FMT_R8G8B8A8, 4, 4, 1, 1, identity, 0x85u);
    CHECK(tex != 0, "texture");
    b->texture_upload(b->user, tex, 0, 0, 4, 4, texel, 16, 4);
    const u32 ptex = b->pipeline_create(b->user, VS, PS_TEX, &rs, &lay, sizeof(vtx), RSX_BE_FMT_R8G8B8A8, 1);
    rsx_be_sampler_desc sd[RSX_BE_MAX_TEXTURES]; memset(sd, 0, sizeof sd);
    for (int u = 0; u < RSX_BE_MAX_TEXTURES; u++) { sd[u].wrap_s = sd[u].wrap_t = sd[u].wrap_r = 1; sd[u].max_lod = 0; }
    u32 units[RSX_BE_MAX_TEXTURES] = { tex };
    b->bind_targets(b->user, &rt, 1, 0);
    b->bind_textures(b->user, units, sd, 1u);
    draw_full(b, ptex);
    {
        u8 expect[4]; u8 sel[4];
        rsx_texture_component_remap(identity, 0x85u & 0x9Fu, sel);   /* A,R,G,B order */
        const u8 src[6] = { 10, 20, 30, 40, 0, 255 };
        expect[0] = src[sel[1]]; expect[1] = src[sel[2]]; expect[2] = src[sel[3]]; expect[3] = src[sel[0]];
        const u32 got = px(b, rt, 32, 32);
        const u32 want = expect[0] | (expect[1] << 8) | (expect[2] << 16) | ((u32)expect[3] << 24);
        if (got != want) fprintf(stderr, "     texel %08X want %08X\n", got, want);
        CHECK(got == want, "texture sampled through the crossbar mapping");
    }
    /* A surface view samples the colour target rt2 (blue). */
    const u32 view = b->surface_view(b->user, rt2, identity, 0x85u);
    CHECK(view != 0 && b->surface_view(b->user, rt2, identity, 0x85u) == view, "surface view, cached");
    units[0] = view;
    b->bind_textures(b->user, units, sd, 1u);
    draw_full(b, ptex);
    CHECK(px(b, rt, 32, 32) != 0xFF0000FFu, "surface view sampled");
    /* 0xAAE4: every output channel takes its remapped input (the low byte alone
     * selects constants), so these checks see the sampled colour itself. */
    const u32 pass_through = 0xAAE4u;
    const u32 live = b->surface_view(b->user, rt2, pass_through, 0x85u);
    units[0] = live;
    b->bind_textures(b->user, units, sd, 1u);
    draw_full(b, ptex);
    const u32 blue_sample = px(b, rt, 32, 32);
    CHECK(blue_sample != 0 && blue_sample != 0xFF0000FFu, "pass-through view samples the target's colour");

    /* color_snapshot: an immutable copy taken now, unaffected by later writes,
     * sampled through a view, and sampled while rendering into its source. */
    CHECK(b->color_snapshot != NULL, "colour snapshot present");
    const u32 snapc = b->color_snapshot(b->user, rt2);
    CHECK(snapc != 0, "colour snapshot");
    const float lime[4] = { 0, 1, 0, 1 };
    b->clear_color(b->user, rt2, lime);
    const u32 sview = b->surface_view(b->user, snapc, pass_through, 0x85u);
    CHECK(sview != 0, "view of a colour snapshot");
    units[0] = sview;
    b->bind_targets(b->user, &rt, 1, 0);
    b->bind_textures(b->user, units, sd, 1u);
    draw_full(b, ptex);
    CHECK(px(b, rt, 32, 32) == blue_sample, "snapshot keeps the contents at the time of the copy");
    units[0] = live;   /* the live view of rt2 sees the later clear */
    b->bind_textures(b->user, units, sd, 1u);
    draw_full(b, ptex);
    const u32 lime_sample = px(b, rt, 32, 32);
    CHECK(lime_sample != blue_sample, "live view sees the later write");
    {   /* feedback: render into rt2 while sampling a snapshot of rt2 */
        const u32 fb = b->color_snapshot(b->user, rt2);
        const u32 fbv = fb ? b->surface_view(b->user, fb, pass_through, 0x85u) : 0;
        CHECK(fb && fbv, "snapshot of a target about to be rendered");
        units[0] = fbv;
        b->bind_targets(b->user, &rt2, 1, 0);
        b->bind_textures(b->user, units, sd, 1u);
        draw_full(b, ptex);
        CHECK(px(b, rt2, 32, 32) == lime_sample, "draw into the target while sampling its snapshot");
        b->texture_release(b->user, fb);
        b->bind_targets(b->user, &rt, 1, 0);
    }
    b->texture_release(b->user, snapc);
    CHECK(s_e12o[sview].kind == E12_FREE, "releasing a snapshot releases its views");

    /* Depth bounds: left half of the depth at 0.5, right half at 0.75; with
     * bounds [0.4, 0.6] a full-screen draw only lands on the left half. */
    {
        CHECK(b->set_depth_bounds != NULL, "depth bounds entry point present");
        const u32 zb = b->depth_target_create(b->user, 64, 64);
        b->clear_depth_stencil(b->user, zb, RSX_BE_CLEAR_DEPTH, 0.75f, 0);
        b->bind_targets(b->user, &rt, 1, zb);
        b->set_scissor(b->user, 0, 0, 32, 64);
        draw_full(b, pdepth);
        b->set_scissor(b->user, 0, 0, 64, 64);
        rsx_be_render_state rsb = plain_state(); rsb.depth_bounds_test = 1;
        const u32 pbounds = b->pipeline_create(b->user, VS, PS, &rsb, &lay, sizeof(vtx), RSX_BE_FMT_R8G8B8A8, 1);
        CHECK(pbounds != 0, "depth-bounds pipeline");
        b->clear_color(b->user, rt, red);
        b->set_depth_bounds(b->user, 0.4f, 0.6f);
        draw_full(b, pbounds);
        const u32 l = px(b, rt, 8, 8), r = px(b, rt, 56, 8);
        CHECK(l != 0xFF0000FFu && r == 0xFF0000FFu, "bounds [0.4, 0.6] pass 0.5 and reject 0.75");
        b->clear_color(b->user, rt, red);
        b->set_depth_bounds(b->user, 0.0f, 1.0f);
        draw_full(b, pbounds);
        CHECK(px(b, rt, 8, 8) != 0xFF0000FFu && px(b, rt, 56, 8) != 0xFF0000FFu, "bounds [0, 1] pass both");
        b->clear_color(b->user, rt, red);
        b->set_depth_bounds(b->user, NAN, 0.6f);   /* NaN opens its end: [0, 0.6] */
        draw_full(b, pbounds);
        CHECK(px(b, rt, 8, 8) != 0xFF0000FFu && px(b, rt, 56, 8) == 0xFF0000FFu, "NaN minimum opens the range");
        b->clear_color(b->user, rt, red);
        b->set_depth_bounds(b->user, 0.4f, NAN);   /* [0.4, 1] */
        draw_full(b, pbounds);
        CHECK(px(b, rt, 8, 8) != 0xFF0000FFu && px(b, rt, 56, 8) != 0xFF0000FFu, "NaN maximum opens the range");
        b->clear_color(b->user, rt, red);
        b->clear_depth_stencil(b->user, zb, RSX_BE_CLEAR_DEPTH, 1.0f, 0);   /* depth exactly 1 everywhere */
        const u64 skipped = s_e12.bounds_empty_draws;
        b->set_depth_bounds(b->user, 0.7f, 0.3f);  /* inverted: nothing passes, not even depth 1 */
        draw_full(b, pbounds);
        CHECK(px(b, rt, 8, 8) == 0xFF0000FFu && px(b, rt, 56, 8) == 0xFF0000FFu &&
              s_e12.bounds_empty_draws == skipped + 1, "inverted range draws nothing");
        b->set_depth_bounds(b->user, 0.0f, 1.0f);
        CHECK(s_e12.count[E12_X_DEPTH_BOUNDS_UNSUPPORTED] == 0, "no depth-bounds fallback counted");
        b->pipeline_release(b->user, pbounds);
        b->bind_targets(b->user, &rt, 1, 0);
        b->depth_target_release(b->user, zb);
    }

    /* color_target_write at an offset. */
    u8 rows[2 * 2 * 4]; for (int i = 0; i < 4; i++) { rows[4 * i] = 1; rows[4 * i + 1] = 2; rows[4 * i + 2] = 3; rows[4 * i + 3] = 4; }
    b->color_target_write(b->user, rt, 10, 20, 2, 2, rows, 8);
    CHECK(px(b, rt, 11, 21) == 0x04030201u, "colour target write at an offset");

    /* Refusals are counted. */
    const u64 before = s_e12.count[E12_X_DRAW_TARGETS];
    b->bind_targets(b->user, pair, 2, 0);
    draw_full(b, pipe);                      /* one-target pipeline, two targets */
    CHECK(s_e12.count[E12_X_DRAW_TARGETS] == before + 1, "mismatched target count refused and counted");

    /* Release and reuse: handles come back; resources retire on the fence. */
    b->texture_release(b->user, tex);
    b->color_target_release(b->user, rt2);
    const u32 again = b->color_target_create(b->user, RSX_BE_FMT_R8G8B8A8, 8, 8, NULL, 0);
    CHECK(again != 0, "handle reuse after release");
    b->submit_and_wait(b->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    CHECK(s_e12.retire_n == 0, "retired resources released after the fence");
    b->submit_and_wait(b->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    CHECK(!s_e12.failed, "flush leaves the backend healthy");

    /* The shader-visible heap holds a whole frame of descriptor tables: 4000
     * draws (20 entries each, more than the old 65536-entry heap) need no wait. */
    CHECK(s_e12.srv_gpu_cap == ENG12_SRV_GPU_HEAP_MAX, "large shader-visible heap");
    {
        const u64 before = s_e12.waits[E12_W_SRV_TABLES];
        b->bind_targets(b->user, &rt, 1, 0);
        for (int i = 0; i < 4000; i++) draw_full(b, pipe);
        b->submit_and_wait(b->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        CHECK(s_e12.waits[E12_W_SRV_TABLES] == before, "4000 draws without a descriptor-ring wait");
    }
    /* Fenced rings: shrink both rings so many batches submitted without a
     * wait (e12_execute) must wrap them. Space returns as the GPU passes each
     * batch's fence; no full wait is needed, and the drawing stays correct. */
    {
        b->submit_and_wait(b->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        const u64 keep_srv = s_e12.srv_ring.size, keep_up = s_e12.upload_ring.size;
        s_e12.srv_ring.size = 2000;            /* 100 draws of 20 descriptors */
        s_e12.upload_ring.size = 4u << 20;     /* a few batches of constants and vertices */
        const u64 flushes = s_e12.flushes;
        b->bind_targets(b->user, &rt, 1, 0);
        int ok = 1;
        for (int batch = 0; batch < 40; batch++) {
            for (int i = 0; i < 60; i++) draw_full(b, pipe);
            EnterCriticalSection(&s_e12.lock);
            if (!e12_execute()) ok = 0;
            LeaveCriticalSection(&s_e12.lock);
        }
        CHECK(ok, "40 unwaited batches over small rings");
        if (s_e12.flushes != flushes) fprintf(stderr, "     full waits %llu, up head %llu tail %llu segs %u\n",
            (unsigned long long)(s_e12.flushes - flushes), (unsigned long long)s_e12.upload_ring.head,
            (unsigned long long)s_e12.upload_ring.tail, s_e12.upload_ring.nseg);
        CHECK(s_e12.flushes == flushes, "rings wrapped without a full GPU wait");
        CHECK(s_e12.srv_ring.first + s_e12.srv_ring.nseg > 1 || s_e12.waits[E12_W_SRV_TABLES] > 0,
              "the descriptor ring did wrap");
        CHECK(s_e12.srv_ring.head <= s_e12.srv_ring.size && s_e12.upload_ring.head <= s_e12.upload_ring.size,
              "ring heads stay inside the rings");
        b->submit_and_wait(b->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        CHECK(px(b, rt, 32, 32) != 0xFF0000FFu, "drawing through wrapped rings is correct");
        s_e12.srv_ring.size = keep_srv; s_e12.upload_ring.size = keep_up;
    }
    /* Rings barely larger than one draw: every draw must make room first,
     * and that wait or flush must happen before any of its allocations. */
    {
        b->submit_and_wait(b->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        const u64 keep_srv = s_e12.srv_ring.size, keep_up = s_e12.upload_ring.size;
        s_e12.srv_ring.size = 30; s_e12.upload_ring.size = 24u << 10;
        const u64 rw = s_e12.waits[E12_W_UPLOAD_RING] + s_e12.waits[E12_W_SRV_TABLES];
        b->clear_color(b->user, rt, red);
        b->bind_targets(b->user, &rt, 1, 0);
        for (int i = 0; i < 20; i++) draw_full(b, pipe);
        b->submit_and_wait(b->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        CHECK(s_e12.waits[E12_W_UPLOAD_RING] + s_e12.waits[E12_W_SRV_TABLES] > rw, "tiny rings make draws wait");
        CHECK(px(b, rt, 32, 32) != 0xFF0000FFu && !s_e12.failed, "draws that waited for ring space are intact");
        s_e12.srv_ring.size = keep_srv; s_e12.upload_ring.size = keep_up;
    }
    /* Present: every present submits and waits for its frame. */
    if (open_swap_chain(64, 64)) {
        b->bind_targets(b->user, &rt, 1, 0);
        const u64 w0 = s_e12.waits[E12_W_PRESENT], p0 = s_e12.presents;
        for (int i = 0; i < 6; i++) { draw_full(b, pipe); b->present(b->user, rt); }
        CHECK(s_e12.presents == p0 + 6 && s_e12.waits[E12_W_PRESENT] == w0 + 6, "default present waits every frame");
        b->submit_and_wait(b->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        CHECK(px(b, rt, 32, 32) != 0xFF0000FFu && !s_e12.failed, "frames after presents are intact");
    } else fprintf(stderr, "SKIP present checks: no swap chain in this session\n");
    CHECK(s_e12.waits[E12_W_ENGINE_FLUSH] >= 2 && s_e12.waits[E12_W_COLOR_READBACK] > 0 &&
          s_e12.waits[E12_W_DEPTH_READBACK] > 0, "waits are counted by site");

    b->shutdown(b->user);
    const int verrors = validation_errors();
    CHECK(verrors == 0, "no debug-layer errors");
    fprintf(stderr, "%s (%d/%d)\n", failures ? "FAILED" : "PASS", checks - failures, checks);
    return failures ? 1 : 0;
}
