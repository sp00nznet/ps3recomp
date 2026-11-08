/*
 * ps3recomp - D3D12 backend for the register-file draw engine (rsx_draw_engine.h)
 *
 * Included by rsx_d3d12_backend.c, which owns the device, the direct queue and
 * the swapchain. Opt-in: PS3RECOMP_D3D12_DRAW_ENGINE=1. Without it the legacy
 * vtable path stays the only one registered.
 *
 * Contract, in this file's own words (from rsx_draw_engine.h and the observable
 * behaviour the engine expects of any backend):
 *
 *  - Handles are small integers into one object table; 0 is "none". A handle
 *    names a texture, a colour target, a depth target, a pipeline or a view.
 *    Released objects keep their GPU resources until the GPU has finished every
 *    submission that could use them: they are retired on the fence, never by
 *    waiting from the release itself.
 *  - Work is recorded into one command list in call order. submit_and_wait
 *    executes it and waits; nothing the engine hands over (vertex arrays,
 *    constants, texel rows) is read after the call that hands it over returns,
 *    because it is copied into an upload ring right away.
 *  - A colour target is an image the engine draws into and may later sample.
 *    MRT sets are all target A's size and format. A depth target carries depth
 *    and stencil; a draw without a usable one gets a scratch depth buffer of
 *    the draw's size, so every pipeline can declare the same depth format.
 *  - A pipeline is the pair of decompiled HLSL programs plus the render state,
 *    the vertex layout (one float4 per attribute slot, ATTRn semantics), the
 *    colour format and the attachment count. The host topology type is chosen
 *    per draw, so one engine pipeline owns up to three PSOs.
 *  - Textures and surface views wear the unit's TEXTURE_CONTROL1 crossbar as a
 *    D3D12 component mapping, computed from rsx_texture_component_remap.
 *  - present(surface) shows that colour target: the engine has already chosen
 *    it from the flipped display buffer, so the backend never guesses.
 *  - Whatever this backend cannot do is refused by name and counted.
 *    PS3RECOMP_D3D12_ENGINE_STATS_EVERY=<n> prints the counts every n
 *    presents (off by default).
 */

#define ENG12_MAX_OBJ        16384
#define ENG12_RTV_HEAP       4096
#define ENG12_DSV_HEAP       1024
#define ENG12_SRV_CPU_HEAP   16384
#define ENG12_SRV_GPU_HEAP_MAX 1000000u  /* D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1/2 */
#define ENG12_SRV_GPU_HEAP_MIN 65536u    /* the size used before; the fallback */
#define ENG12_SAMP_GPU_HEAP  2048
#define ENG12_UPLOAD_RING    (128u << 20)
#define ENG12_ALLOCATORS     8    /* command allocators in flight for async submissions */
#define ENG12_DEPTH_FORMAT   DXGI_FORMAT_D32_FLOAT_S8X24_UINT
#define ENG12_DEPTH_TYPELESS DXGI_FORMAT_R32G8X24_TYPELESS
#define ENG12_DEPTH_SRV      DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS
#define ENG12_VTEX           RSX_BE_MAX_VERTEX_TEXTURES

enum { E12_FREE = 0, E12_TEXTURE, E12_COLOR, E12_DEPTH, E12_PIPELINE, E12_VIEW };

/* Where the engine waits for the GPU; counted per site and printed with the stats. */
enum { E12_W_PRESENT, E12_W_ENGINE_FLUSH, E12_W_COLOR_READBACK, E12_W_DEPTH_READBACK,
       E12_W_UPLOAD_RING, E12_W_SRV_TABLES, E12_W_SAMPLER_TABLES, E12_W_SHUTDOWN, E12_W_SITES };
static const char* const s_e12_wait_site[E12_W_SITES] = {
    "present", "engine_flush", "color_readback", "depth_readback",
    "upload_ring", "srv_tables", "sampler_tables", "shutdown" };
/* Reasons something was refused or fell back; every one is counted. */
enum {
    E12_X_FORMAT, E12_X_TABLE_FULL, E12_X_CREATE_FAILED, E12_X_COMPILE_VS, E12_X_COMPILE_PS,
    E12_X_PSO, E12_X_DRAW_NO_PIPELINE, E12_X_DRAW_TARGETS, E12_X_DRAW_TARGET_FORMAT,
    E12_X_DRAW_STRIDE, E12_X_DRAW_TOPOLOGY, E12_X_UPLOAD_TOO_LARGE, E12_X_WRITE_FORMAT,
    E12_X_READBACK, E12_X_DEPTH_BOUNDS_UNSUPPORTED, E12_X_SAMPLER_TABLES, E12_X_PRESENT_NONE,
    E12_X_DEVICE, E12_X_ALLOCATOR_WAIT, E12_X_REASONS
};
static const char* const s_e12_reason[E12_X_REASONS] = {
    "format", "table_full", "create_failed", "compile_vs", "compile_ps", "pso",
    "draw_no_pipeline", "draw_targets", "draw_target_format", "draw_stride", "draw_topology",
    "upload_too_large", "write_format", "readback", "depth_bounds_unsupported",
    "sampler_tables_reset", "present_none", "device", "allocator_wait" };

typedef struct {
    u8  kind, cube, rt_count, depth_bounds, snapshot;
    DXGI_FORMAT fmt;                 /* resource/SRV format (pipeline: colour format) */
    u32 w, h, mips, faces, bpp, block;
    ID3D12Resource* res;
    D3D12_RESOURCE_STATES state;
    u32 srv;                         /* CPU SRV slot + 1, 0 none */
    u32 rtv;                         /* RTV slot + 1 */
    u32 dsv;                         /* DSV slot + 1 */
    u32 parent;                      /* view: the colour target it looks at */
    u32 remap, rsx_fmt;
    /* pipeline */
    ID3DBlob* vs; ID3DBlob* ps;
    ID3D12PipelineState* pso[3];     /* point, line, triangle */
    rsx_be_render_state rs;
    u32 nslots, stride, cube_mask;
    u8  attrs[RSX_DSP_NUM_VERTEX_ATTR];
} e12_obj;

typedef struct { IUnknown* res; u64 fence; } e12_retiree;
/* A fenced ring (upload bytes or shader-visible descriptors). Each submission
 * closes a segment ending at the head; a segment's space comes back once the
 * GPU passes its fence. Allocation waits only for the oldest segment when the
 * ring is full, never for the whole queue. */
#define E12_RING_SEGS 1024
typedef struct { u64 fence, end; } e12_ring_seg;
typedef struct {
    u64 size, head, tail, rec_start;
    e12_ring_seg seg[E12_RING_SEGS]; u32 first, nseg;
} e12_ring;

static struct {
    int active, ready, recording, failed;
    CRITICAL_SECTION lock;
    /* One allocator per submission still on the GPU: a list is re-recorded
     * into an allocator only after the fence passed its last submission. */
    ID3D12CommandAllocator* alloc[ENG12_ALLOCATORS]; u64 alloc_fence[ENG12_ALLOCATORS]; u32 cur_alloc;
    ID3D12GraphicsCommandList* cl;
    ID3D12GraphicsCommandList1* cl1;   /* OMSetDepthBounds, when supported */
    ID3D12Fence* fence; HANDLE event; u64 fence_value;
    ID3D12RootSignature* root;
    ID3D12DescriptorHeap *rtv_heap, *dsv_heap, *srv_cpu, *srv_gpu, *samp_gpu;
    u32 rtv_inc, dsv_inc, srv_inc, samp_inc;
    u8  rtv_used[ENG12_RTV_HEAP], dsv_used[ENG12_DSV_HEAP], srv_used[ENG12_SRV_CPU_HEAP];
    u32 samp_gpu_next, srv_gpu_cap;
    ID3D12Resource* upload; u8* upload_ptr;
    e12_ring upload_ring, srv_ring;
    e12_retiree* retire; u32 retire_n, retire_cap;
    u32 null_srv2d, null_srvcube;    /* CPU SRV slots + 1 */
    /* present blit */
    ID3D12RootSignature* blit_root; ID3D12PipelineState* blit_pso;
    /* bound state */
    u32 rt[RSX_BE_MAX_COLOR_TARGETS], nrt, depth, pipeline;
    u32 scratch_depth;
    /* VPConst: 512 transform constants plus the viewport epilogue (two more
     * vec4s); PSConstants: the buffered fragment constants plus fp_alpha. Both
     * up to the 64 KiB a constant buffer view can address. */
    u8  vsc[65536], psc[65536]; u32 vs_bytes, ps_bytes;
    u32 tex[RSX_BE_MAX_TEXTURES], vtex[ENG12_VTEX]; u32 tex_mask, vtex_mask;
    rsx_be_sampler_desc samp[RSX_BE_MAX_TEXTURES], vsamp[ENG12_VTEX];
    float vp[4]; int have_vp; u32 sc[4]; int have_sc; u32 stencil_ref; float dbounds[2]; int dbounds_empty;
    u64 bounds_empty_draws;   /* draws an inverted depth-bounds range rejected entirely */
    int depth_bounds_ok;
    /* sampler table cache: identical tables share one range of the GPU heap */
    struct { u64 key[RSX_BE_MAX_TEXTURES]; u32 n, base; } stab[ENG12_SAMP_GPU_HEAP / 4];
    u32 stab_n;
    u64 count[E12_X_REASONS];
    u64 draws, presents, flushes, submits;
    u64 waits[E12_W_SITES];
} s_e12;
static e12_obj s_e12o[ENG12_MAX_OBJ];

static void e12_count(int reason) { if (reason >= 0 && reason < E12_X_REASONS) s_e12.count[reason]++; }

static D3D12_CPU_DESCRIPTOR_HANDLE e12_cpu(ID3D12DescriptorHeap* h, u32 inc, u32 slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE c; h->lpVtbl->GetCPUDescriptorHandleForHeapStart(h, &c);
    c.ptr += (SIZE_T)slot * inc; return c;
}
static D3D12_GPU_DESCRIPTOR_HANDLE e12_gpu(ID3D12DescriptorHeap* h, u32 inc, u32 slot)
{
    D3D12_GPU_DESCRIPTOR_HANDLE g; h->lpVtbl->GetGPUDescriptorHandleForHeapStart(h, &g);
    g.ptr += (UINT64)slot * inc; return g;
}
static u32 e12_slot_alloc(u8* used, u32 n)
{
    for (u32 i = 0; i < n; i++) if (!used[i]) { used[i] = 1; return i + 1; }
    return 0;
}

/* ---- submission ----------------------------------------------------------- */
static int e12_wait_fence(u64 v)
{
    if (s_e12.fence->lpVtbl->GetCompletedValue(s_e12.fence) >= v) return 0;
    s_e12.fence->lpVtbl->SetEventOnCompletion(s_e12.fence, v, s_e12.event);
    if (WaitForSingleObject(s_e12.event, 10000) == WAIT_OBJECT_0) return 0;
    s_e12.failed = 1; e12_count(E12_X_DEVICE);
    printf("[D3D12 engine] fence wait timed out (device removed 0x%08lX)\n",
           (long)s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device));
    return -1;
}
/* An allocator whose submissions are complete, next in turn; when every one
 * is still in flight, wait for the oldest (counted: the GPU is that far behind). */
static int e12_pick_allocator(void)
{
    const u64 done = s_e12.fence->lpVtbl->GetCompletedValue(s_e12.fence);
    u32 oldest = 0;
    for (u32 k = 1; k <= ENG12_ALLOCATORS; k++) {
        const u32 i = (s_e12.cur_alloc + k) % ENG12_ALLOCATORS;
        if (s_e12.alloc_fence[i] <= done) { s_e12.cur_alloc = i; return 0; }
        if (s_e12.alloc_fence[i] < s_e12.alloc_fence[oldest]) oldest = i;
    }
    e12_count(E12_X_ALLOCATOR_WAIT);
    if (e12_wait_fence(s_e12.alloc_fence[oldest])) return -1;
    s_e12.cur_alloc = oldest;
    return 0;
}
static int e12_begin(void)
{
    if (s_e12.recording) return 0;
    if (s_e12.failed || e12_pick_allocator()) return -1;
    ID3D12CommandAllocator* a = s_e12.alloc[s_e12.cur_alloc];
    if (FAILED(a->lpVtbl->Reset(a)) ||
        FAILED(s_e12.cl->lpVtbl->Reset(s_e12.cl, a, NULL))) {
        s_e12.failed = 1; e12_count(E12_X_DEVICE); return -1;
    }
    ID3D12DescriptorHeap* heaps[2] = { s_e12.srv_gpu, s_e12.samp_gpu };
    s_e12.cl->lpVtbl->SetDescriptorHeaps(s_e12.cl, 2, heaps);
    s_e12.recording = 1;
    return 0;
}
static void e12_retire_now(u64 completed)
{
    u32 k = 0;
    for (u32 i = 0; i < s_e12.retire_n; i++) {
        if (s_e12.retire[i].fence <= completed) s_e12.retire[i].res->lpVtbl->Release(s_e12.retire[i].res);
        else s_e12.retire[k++] = s_e12.retire[i];
    }
    s_e12.retire_n = k;
}
/* Retire on the fence: released now, destroyed once the GPU has passed every
 * submission recorded so far. Never waits. */
static void e12_retire(IUnknown* r)
{
    if (!r) return;
    if (s_e12.retire_n == s_e12.retire_cap) {
        u32 cap = s_e12.retire_cap ? s_e12.retire_cap * 2 : 256;
        e12_retiree* n = (e12_retiree*)realloc(s_e12.retire, cap * sizeof *n);
        if (!n) { e12_count(E12_X_DEVICE); return; }   /* leak rather than free in use */
        s_e12.retire = n; s_e12.retire_cap = cap;
    }
    s_e12.retire[s_e12.retire_n].res = r;
    s_e12.retire[s_e12.retire_n++].fence = s_e12.fence_value + 1;   /* the next signal */
}
/* D3D12_DBG=1 (debug layer) + D3D12_IQ=1: print what the debug layer stored,
 * after each submission; capped. Off by default. */
static void e12_drain_info_queue(void)
{
    static int on = -1, printed = 0;
    if (on < 0) on = getenv("D3D12_IQ") ? 1 : 0;
    if (!on || printed > 200) return;
    ID3D12InfoQueue* iq = NULL;
    if (FAILED(s_d3d.device->lpVtbl->QueryInterface(s_d3d.device, &IID_ID3D12InfoQueue, (void**)&iq)) || !iq) return;
    const UINT64 n = iq->lpVtbl->GetNumStoredMessages(iq);
    for (UINT64 i = 0; i < n && printed <= 200; i++) {
        SIZE_T len = 0;
        iq->lpVtbl->GetMessage(iq, i, NULL, &len);
        D3D12_MESSAGE* m = (D3D12_MESSAGE*)malloc(len);
        if (m && SUCCEEDED(iq->lpVtbl->GetMessage(iq, i, m, &len)) && m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
            printf("[D3D12 engine iq] sev=%d id=%d %s\n", (int)m->Severity, (int)m->ID, m->pDescription);
            printed++;
        }
        free(m);
    }
    iq->lpVtbl->ClearStoredMessages(iq);
    iq->lpVtbl->Release(iq);
}
/* Execute what is recorded and wait for it: the engine's synchronous contract.
 * After the wait every ring restarts and every retirement is due. */
static void e12_ring_reset(e12_ring* r) { r->head = r->tail = r->rec_start = 0; r->first = r->nseg = 0; }
static void e12_ring_reclaim(e12_ring* r, u64 done)
{
    while (r->nseg && r->seg[r->first].fence <= done) {
        r->tail = r->seg[r->first].end;
        r->first = (r->first + 1) % E12_RING_SEGS; r->nseg--;
    }
    if (!r->nseg && r->head == r->rec_start && r->tail == r->head) e12_ring_reset(r);
}
/* Close the current recording's part of the ring under fence `v`. */
static void e12_ring_close(e12_ring* r, u64 v)
{
    if (r->head == r->rec_start) return;
    if (r->nseg == E12_RING_SEGS) {   /* fold into the newest segment: same fence order */
        r->seg[(r->first + r->nseg - 1) % E12_RING_SEGS] = (e12_ring_seg){ v, r->head };
    } else {
        r->seg[(r->first + r->nseg) % E12_RING_SEGS] = (e12_ring_seg){ v, r->head };
        r->nseg++;
    }
    r->rec_start = r->head;
}
static int e12_submit_wait(int site);
static int e12_begin(void);
static int e12_wait_fence(u64 v);
/* Space for n units at `align`; -1 if it can never fit. Waits for the oldest
 * segment when full (counted at `site`); a full wait only when the current,
 * unsubmitted recording alone fills the ring. */
static int e12_ring_alloc(e12_ring* r, u64 n, u64 align, int site, u64* at_out)
{
    if (n + align >= r->size) return -1;
    for (;;) {
        e12_ring_reclaim(r, s_e12.fence->lpVtbl->GetCompletedValue(s_e12.fence));
        /* In use: [tail, head) wrapping at size; head == tail means empty. The
         * head never catches the tail from behind, so that stays unambiguous. */
        const u64 at = (r->head + align - 1) & ~(align - 1);
        u64 pos = ~0ull;
        if (r->head >= r->tail) {
            if (at + n <= r->size) pos = at;
            else if (n < r->tail) pos = 0;        /* wrap; the skipped end frees with its segment */
        } else if (at + n < r->tail) pos = at;
        if (pos != ~0ull) { r->head = pos + n; *at_out = pos; return 0; }
        if (site >= 0 && site < E12_W_SITES) s_e12.waits[site]++;
        if (r->nseg) {
            if (e12_wait_fence(r->seg[r->first].fence)) return -1;
        } else if (e12_submit_wait(-1) || e12_begin()) return -1;
    }
}

/* Make sure n units (alignment slack included by the caller) can be
 * allocated from the ring without waiting, doing any wait now. A draw does
 * this for its whole need before its first allocation, so a wait or a full
 * flush can never fall between two of its allocations (a full flush resets
 * the rings and would let later allocations overwrite earlier ones). */
static int e12_ring_reserve(e12_ring* r, u64 n, int site)
{
    if (n >= r->size) return -1;
    for (;;) {
        e12_ring_reclaim(r, s_e12.fence->lpVtbl->GetCompletedValue(s_e12.fence));
        if (r->head >= r->tail ? (r->head + n <= r->size || n < r->tail) : (r->head + n < r->tail)) return 0;
        if (site >= 0 && site < E12_W_SITES) s_e12.waits[site]++;
        if (r->nseg) {
            if (e12_wait_fence(r->seg[r->first].fence)) return -1;
        } else if (e12_submit_wait(-1) || e12_begin()) return -1;
    }
}
/* Execute what is recorded and signal the fence after it; returns the value
 * that completes it, 0 on failure. The allocator stays busy until then. */
static u64 e12_execute(void)
{
    if (s_e12.recording) {
        s_e12.recording = 0;
        if (FAILED(s_e12.cl->lpVtbl->Close(s_e12.cl))) { s_e12.failed = 1; e12_count(E12_X_DEVICE); return 0; }
        ID3D12CommandList* lists[1] = { (ID3D12CommandList*)s_e12.cl };
        s_d3d.cmd_queue->lpVtbl->ExecuteCommandLists(s_d3d.cmd_queue, 1, lists);
        s_e12.submits++;
    }
    const u64 v = ++s_e12.fence_value;
    if (FAILED(s_d3d.cmd_queue->lpVtbl->Signal(s_d3d.cmd_queue, s_e12.fence, v))) { s_e12.failed = 1; return 0; }
    s_e12.alloc_fence[s_e12.cur_alloc] = v;
    e12_ring_close(&s_e12.upload_ring, v);
    e12_ring_close(&s_e12.srv_ring, v);
    return v;
}
static int e12_submit_wait(int site)
{
    if (site >= 0 && site < E12_W_SITES) s_e12.waits[site]++;
    const int rc = s_e12.failed ? -1 : 0;
    const u64 v = e12_execute();
    if (!v || e12_wait_fence(v)) return -1;
    e12_retire_now(v);
    e12_drain_info_queue();
    e12_ring_reset(&s_e12.upload_ring);
    e12_ring_reset(&s_e12.srv_ring);
    s_e12.flushes++;
    return rc;
}
/* Upload-ring space; a full ring flushes (submit + wait) and starts over. */
static void* e12_upload(u64 bytes, u64 align, D3D12_GPU_VIRTUAL_ADDRESS* gpu, u64* offset)
{
    u64 at = 0;
    if (bytes >= ENG12_UPLOAD_RING / 2 || e12_ring_alloc(&s_e12.upload_ring, bytes, align, E12_W_UPLOAD_RING, &at)) {
        e12_count(E12_X_UPLOAD_TOO_LARGE); return NULL;
    }
    if (gpu) *gpu = s_e12.upload->lpVtbl->GetGPUVirtualAddress(s_e12.upload) + at;
    if (offset) *offset = at;
    return s_e12.upload_ptr + at;
}
static void e12_transition(e12_obj* o, D3D12_RESOURCE_STATES to)
{
    if (!o->res || o->state == to) return;
    D3D12_RESOURCE_BARRIER b; memset(&b, 0, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = o->res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = o->state; b.Transition.StateAfter = to;
    s_e12.cl->lpVtbl->ResourceBarrier(s_e12.cl, 1, &b);
    o->state = to;
}

/* ---- formats and mappings -------------------------------------------------- */
static DXGI_FORMAT e12_format(rsx_be_format f, u32* bpp, u32* block)
{
    *bpp = 0; *block = 0;
    switch (f) {
    case RSX_BE_FMT_R8:            *bpp = 1;  return DXGI_FORMAT_R8_UNORM;
    case RSX_BE_FMT_R8G8:          *bpp = 2;  return DXGI_FORMAT_R8G8_UNORM;
    case RSX_BE_FMT_R8G8B8A8:      *bpp = 4;  return DXGI_FORMAT_R8G8B8A8_UNORM;
    case RSX_BE_FMT_R16:           *bpp = 2;  return DXGI_FORMAT_R16_UNORM;
    case RSX_BE_FMT_R16G16:        *bpp = 4;  return DXGI_FORMAT_R16G16_UNORM;
    case RSX_BE_FMT_R16G16F:       *bpp = 4;  return DXGI_FORMAT_R16G16_FLOAT;
    case RSX_BE_FMT_R16G16B16A16F: *bpp = 8;  return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case RSX_BE_FMT_R32F:          *bpp = 4;  return DXGI_FORMAT_R32_FLOAT;
    case RSX_BE_FMT_R32G32B32A32F: *bpp = 16; return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case RSX_BE_FMT_BC1:           *block = 8;  return DXGI_FORMAT_BC1_UNORM;
    case RSX_BE_FMT_BC2:           *block = 16; return DXGI_FORMAT_BC2_UNORM;
    case RSX_BE_FMT_BC3:           *block = 16; return DXGI_FORMAT_BC3_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
/* The crossbar as Shader4ComponentMapping: rsx_texture_component_remap gives,
 * in A,R,G,B order, the source channel (0..3 = R,G,B,A) or a constant (4 zero,
 * 5 one) each output reads; D3D12's selectors use the same codes. */
static UINT e12_mapping(u32 remap, u32 rsx_fmt)
{
    u8 sel[4];
    rsx_texture_component_remap(remap, rsx_fmt & 0x9Fu, sel);
    for (int i = 0; i < 4; i++) if (sel[i] > 5) sel[i] = 4;
    return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(sel[1], sel[2], sel[3], sel[0]);
}
static D3D12_COMPARISON_FUNC e12_compare(u32 f)
{
    return (f >= 0x0200u && f <= 0x0207u) ? (D3D12_COMPARISON_FUNC)(f - 0x0200u + 1u)
                                          : D3D12_COMPARISON_FUNC_ALWAYS;
}
static D3D12_STENCIL_OP e12_stencil_op(u32 op)
{
    switch (op) {
    case 0x0000: return D3D12_STENCIL_OP_ZERO;
    case 0x1E01: return D3D12_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D12_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D12_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D12_STENCIL_OP_INVERT;
    case 0x8507: return D3D12_STENCIL_OP_INCR;
    case 0x8508: return D3D12_STENCIL_OP_DECR;
    default:     return D3D12_STENCIL_OP_KEEP;
    }
}
static D3D12_TEXTURE_ADDRESS_MODE e12_wrap(u32 w, u8 border)
{
    switch (w & 0xFu) {
    case 1: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case 2: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case 4: return border == 3 ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    default: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    }
}
static void e12_sampler_desc(const rsx_be_sampler_desc* d, D3D12_SAMPLER_DESC* s)
{
    memset(s, 0, sizeof *s);
    const int minl = d->min_linear, magl = d->mag_linear, mipl = d->mip_present && d->mip_linear;
    s->Filter = (D3D12_FILTER)((minl ? 0x10 : 0) | (magl ? 0x4 : 0) | (mipl ? 0x1 : 0));
    s->AddressU = e12_wrap(d->wrap_s, d->border);
    s->AddressV = e12_wrap(d->wrap_t, d->border);
    s->AddressW = e12_wrap(d->wrap_r, d->border);
    s->MaxAnisotropy = 1;
    s->ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    const float a = (d->border == 1 || d->border == 2) ? 1.0f : 0.0f, c = d->border == 2 ? 1.0f : 0.0f;
    s->BorderColor[0] = s->BorderColor[1] = s->BorderColor[2] = c; s->BorderColor[3] = a;
    s->MinLOD = d->min_lod;
    s->MaxLOD = d->mip_present ? (d->max_lod < d->min_lod ? d->min_lod : d->max_lod) : d->min_lod;
}
static u64 e12_sampler_key(const rsx_be_sampler_desc* d)
{
    u32 lo, hi; memcpy(&lo, &d->min_lod, 4); memcpy(&hi, &d->max_lod, 4);
    return (u64)d->min_linear | ((u64)d->mag_linear << 1) | ((u64)d->mip_linear << 2) |
           ((u64)d->mip_present << 3) | ((u64)(d->wrap_s & 0xF) << 4) | ((u64)(d->wrap_t & 0xF) << 8) |
           ((u64)(d->wrap_r & 0xF) << 12) | ((u64)(d->border & 3) << 14) |
           ((u64)(lo >> 16) << 16) | ((u64)(hi >> 16) << 32) | (1ull << 63);
}

/* ---- objects ---------------------------------------------------------------- */
static u32 e12_alloc(void)
{
    for (u32 i = 1; i < ENG12_MAX_OBJ; i++)
        if (s_e12o[i].kind == E12_FREE) { memset(&s_e12o[i], 0, sizeof s_e12o[i]); return i; }
    e12_count(E12_X_TABLE_FULL);
    return 0;
}
static e12_obj* e12_get(u32 h, int kind)
{
    if (!h || h >= ENG12_MAX_OBJ || s_e12o[h].kind != kind) return NULL;
    return &s_e12o[h];
}
static ID3D12Resource* e12_committed(D3D12_HEAP_TYPE heap, const D3D12_RESOURCE_DESC* d,
                                     D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv)
{
    D3D12_HEAP_PROPERTIES hp; memset(&hp, 0, sizeof hp); hp.Type = heap;
    ID3D12Resource* r = NULL;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, d, st, cv,
                                                             &IID_ID3D12Resource, (void**)&r)))
        return NULL;
    return r;
}
static D3D12_RESOURCE_DESC e12_tex_desc(DXGI_FORMAT f, u32 w, u32 h, u32 mips, u32 layers, D3D12_RESOURCE_FLAGS fl)
{
    D3D12_RESOURCE_DESC d; memset(&d, 0, sizeof d);
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h;
    d.DepthOrArraySize = (UINT16)layers; d.MipLevels = (UINT16)mips; d.Format = f;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; d.Flags = fl;
    return d;
}
static u32 e12_make_srv(ID3D12Resource* r, DXGI_FORMAT f, u32 mips, int cube, UINT mapping)
{
    const u32 slot = e12_slot_alloc(s_e12.srv_used, ENG12_SRV_CPU_HEAP);
    if (!slot) { e12_count(E12_X_TABLE_FULL); return 0; }
    D3D12_SHADER_RESOURCE_VIEW_DESC v; memset(&v, 0, sizeof v);
    v.Format = f; v.Shader4ComponentMapping = mapping;
    if (cube) { v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE; v.TextureCube.MipLevels = mips; }
    else { v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; v.Texture2D.MipLevels = mips; }
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, r, &v,
        e12_cpu(s_e12.srv_cpu, s_e12.srv_inc, slot - 1));
    return slot;
}
static void e12_free_slots(e12_obj* o)
{
    if (o->srv) s_e12.srv_used[o->srv - 1] = 0;
    if (o->rtv) s_e12.rtv_used[o->rtv - 1] = 0;
    if (o->dsv) s_e12.dsv_used[o->dsv - 1] = 0;
    o->srv = o->rtv = o->dsv = 0;
}
static void e12_release_obj(u32 h)
{
    if (!h || h >= ENG12_MAX_OBJ || s_e12o[h].kind == E12_FREE) return;
    e12_obj* o = &s_e12o[h];
    if (o->kind == E12_COLOR || (o->kind == E12_TEXTURE && o->snapshot))   /* views go with their parent */
        for (u32 i = 1; i < ENG12_MAX_OBJ; i++)
            if (s_e12o[i].kind == E12_VIEW && s_e12o[i].parent == h) e12_release_obj(i);
    e12_free_slots(o);
    if (o->kind == E12_PIPELINE) {
        for (int t = 0; t < 3; t++) if (o->pso[t]) e12_retire((IUnknown*)o->pso[t]);
        if (o->vs) o->vs->lpVtbl->Release(o->vs);
        if (o->ps) o->ps->lpVtbl->Release(o->ps);
    } else if (o->kind != E12_VIEW) {
        e12_retire((IUnknown*)o->res);
    }
    for (u32 i = 0; i < RSX_BE_MAX_COLOR_TARGETS; i++) if (s_e12.rt[i] == h) s_e12.rt[i] = 0;
    if (s_e12.depth == h) s_e12.depth = 0;
    if (s_e12.pipeline == h) s_e12.pipeline = 0;
    memset(o, 0, sizeof *o);
}
/* Rows into a subresource through the upload ring. */
static void e12_copy_rows(e12_obj* o, u32 sub, u32 x, u32 y, u32 w, u32 h, const void* src,
                          u32 row_bytes, u32 rows)
{
    const u32 tight = o->block ? ((w + 3u) / 4u) * o->block : w * o->bpp;
    if (!tight || !rows) return;
    const u32 pitch = (tight + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    u64 off = 0;
    u8* dst = (u8*)e12_upload((u64)pitch * rows, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, NULL, &off);
    if (!dst) return;
    for (u32 r = 0; r < rows; r++)
        memcpy(dst + (size_t)r * pitch, (const u8*)src + (size_t)r * row_bytes, row_bytes < tight ? row_bytes : tight);
    if (e12_begin()) return;
    e12_transition(o, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION d, s; memset(&d, 0, sizeof d); memset(&s, 0, sizeof s);
    d.pResource = o->res; d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; d.SubresourceIndex = sub;
    s.pResource = s_e12.upload; s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    s.PlacedFootprint.Offset = off;
    s.PlacedFootprint.Footprint.Format = o->fmt;
    s.PlacedFootprint.Footprint.Width = o->block ? ((w + 3u) & ~3u) : w;
    s.PlacedFootprint.Footprint.Height = o->block ? rows * 4u : rows;
    s.PlacedFootprint.Footprint.Depth = 1;
    s.PlacedFootprint.Footprint.RowPitch = pitch;
    s_e12.cl->lpVtbl->CopyTextureRegion(s_e12.cl, &d, x, y, 0, &s, NULL);
    (void)h;
}

/* ---- backend: lifecycle ------------------------------------------------------ */
static int e12_heap(D3D12_DESCRIPTOR_HEAP_TYPE t, u32 n, int visible, ID3D12DescriptorHeap** out)
{
    D3D12_DESCRIPTOR_HEAP_DESC d; memset(&d, 0, sizeof d);
    d.Type = t; d.NumDescriptors = n;
    d.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    return FAILED(s_d3d.device->lpVtbl->CreateDescriptorHeap(s_d3d.device, &d, &IID_ID3D12DescriptorHeap, (void**)out)) ? -1 : 0;
}
/* The shader-visible CBV/SRV/UAV heap that per-draw descriptor tables are
 * carved from. Each draw takes 20 entries and the ring restarts only after a
 * full GPU wait, so a small heap forces a wait every few thousand draws. Use
 * the largest size every resource binding tier allows (1,000,000 entries),
 * falling back to the earlier 65536 when the device refuses it. */
static int e12_srv_gpu_heap(void)
{
    static const u32 sizes[2] = { ENG12_SRV_GPU_HEAP_MAX, ENG12_SRV_GPU_HEAP_MIN };
    for (int i = 0; i < 2; i++)
        if (e12_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, sizes[i], 1, &s_e12.srv_gpu) == 0) {
            s_e12.srv_gpu_cap = sizes[i];
            s_e12.srv_ring.size = sizes[i];
            if (i) printf("[D3D12 engine] shader-visible descriptor heap: %u entries (larger heap refused)\n", sizes[i]);
            return 0;
        }
    return -1;
}
static int e12_root_signature(void)
{
    D3D12_DESCRIPTOR_RANGE r[4]; memset(r, 0, sizeof r);
    r[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;     r[0].NumDescriptors = RSX_BE_MAX_TEXTURES;
    r[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER; r[1].NumDescriptors = RSX_BE_MAX_TEXTURES;
    /* The vertex program declares its texture units at t16..t19 (rsx_vp_decompiler.c),
     * so the fragment units' t0..t15 and the vertex units never alias in one layout. */
    r[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;     r[2].NumDescriptors = ENG12_VTEX;
    r[2].BaseShaderRegister = RSX_BE_MAX_TEXTURES;
    r[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER; r[3].NumDescriptors = ENG12_VTEX;
    D3D12_ROOT_PARAMETER p[6]; memset(p, 0, sizeof p);
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[0].Descriptor.ShaderRegister = 0;
    p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[1].Descriptor.ShaderRegister = 1;
    p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (int i = 0; i < 4; i++) {
        p[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[2 + i].DescriptorTable.NumDescriptorRanges = 1;
        p[2 + i].DescriptorTable.pDescriptorRanges = &r[i];
        p[2 + i].ShaderVisibility = i < 2 ? D3D12_SHADER_VISIBILITY_PIXEL : D3D12_SHADER_VISIBILITY_VERTEX;
    }
    D3D12_ROOT_SIGNATURE_DESC rd; memset(&rd, 0, sizeof rd);
    rd.NumParameters = 6; rd.pParameters = p;
    rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob *blob = NULL, *err = NULL;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
        if (err) { printf("[D3D12 engine] root signature: %s\n", (const char*)err->lpVtbl->GetBufferPointer(err)); err->lpVtbl->Release(err); }
        return -1;
    }
    HRESULT hr = s_d3d.device->lpVtbl->CreateRootSignature(s_d3d.device, 0, blob->lpVtbl->GetBufferPointer(blob),
        blob->lpVtbl->GetBufferSize(blob), &IID_ID3D12RootSignature, (void**)&s_e12.root);
    blob->lpVtbl->Release(blob);
    return FAILED(hr) ? -1 : 0;
}
static ID3DBlob* e12_compile(const char* src, const char* target, int stage_reason)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), target, NULL, NULL, "main", target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        e12_count(stage_reason);
        static int logged = 0;
        if (PS3_LOG_FIRST(logged, 6))
            printf("[D3D12 engine] %s compile failed: %s\n", target,
                   err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "?");
        if (code) code->lpVtbl->Release(code);
        code = NULL;
    }
    if (err) err->lpVtbl->Release(err);
    return code;
}
static int e12_blit_pipeline(void)
{
    static const char vs[] =
        "struct O { float4 p : SV_Position; float2 t : TEXCOORD0; };\n"
        "O main(uint id : SV_VertexID) { O o; o.t = float2((id << 1) & 2, id & 2);\n"
        "  o.p = float4(o.t * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n";
    static const char ps[] =
        "Texture2D t : register(t0); SamplerState s : register(s0);\n"
        "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return float4(t.Sample(s, uv).rgb, 1); }\n";
    D3D12_DESCRIPTOR_RANGE r; memset(&r, 0, sizeof r);
    r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER p; memset(&p, 0, sizeof p);
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p.DescriptorTable.NumDescriptorRanges = 1; p.DescriptorTable.pDescriptorRanges = &r;
    p.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC ss; memset(&ss, 0, sizeof ss);
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.MaxLOD = D3D12_FLOAT32_MAX; ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd; memset(&rd, 0, sizeof rd);
    rd.NumParameters = 1; rd.pParameters = &p; rd.NumStaticSamplers = 1; rd.pStaticSamplers = &ss;
    ID3DBlob *blob = NULL, *err = NULL;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) { if (err) err->lpVtbl->Release(err); return -1; }
    HRESULT hr = s_d3d.device->lpVtbl->CreateRootSignature(s_d3d.device, 0, blob->lpVtbl->GetBufferPointer(blob),
        blob->lpVtbl->GetBufferSize(blob), &IID_ID3D12RootSignature, (void**)&s_e12.blit_root);
    blob->lpVtbl->Release(blob);
    if (FAILED(hr)) return -1;
    ID3DBlob* v = e12_compile(vs, "vs_5_0", E12_X_COMPILE_VS);
    ID3DBlob* f = e12_compile(ps, "ps_5_0", E12_X_COMPILE_PS);
    if (!v || !f) { if (v) v->lpVtbl->Release(v); if (f) f->lpVtbl->Release(f); return -1; }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d; memset(&d, 0, sizeof d);
    d.pRootSignature = s_e12.blit_root;
    d.VS.pShaderBytecode = v->lpVtbl->GetBufferPointer(v); d.VS.BytecodeLength = v->lpVtbl->GetBufferSize(v);
    d.PS.pShaderBytecode = f->lpVtbl->GetBufferPointer(f); d.PS.BytecodeLength = f->lpVtbl->GetBufferSize(f);
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = 0xFFFFFFFFu;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1; d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(s_d3d.device, &d, &IID_ID3D12PipelineState, (void**)&s_e12.blit_pso);
    v->lpVtbl->Release(v); f->lpVtbl->Release(f);
    return FAILED(hr) ? -1 : 0;
}
static void e12_shutdown(void* user);
static int e12_init(void* user, u32 width, u32 height)
{
    (void)user; (void)width; (void)height;
    if (!s_d3d.device || !s_d3d.cmd_queue) return -1;
    ID3D12Device* dv = s_d3d.device;
    for (u32 i = 0; i < ENG12_ALLOCATORS; i++)
        if (FAILED(dv->lpVtbl->CreateCommandAllocator(dv, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void**)&s_e12.alloc[i])))
            goto fail;
    if (FAILED(dv->lpVtbl->CreateCommandList(dv, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_e12.alloc[0], NULL,
                                             &IID_ID3D12GraphicsCommandList, (void**)&s_e12.cl)) ||
        FAILED(s_e12.cl->lpVtbl->Close(s_e12.cl)) ||
        FAILED(dv->lpVtbl->CreateFence(dv, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&s_e12.fence)))
        goto fail;
    s_e12.event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!s_e12.event) goto fail;
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS2 o2; memset(&o2, 0, sizeof o2);
        if (SUCCEEDED(dv->lpVtbl->CheckFeatureSupport(dv, D3D12_FEATURE_D3D12_OPTIONS2, &o2, sizeof o2)) &&
            o2.DepthBoundsTestSupported &&
            SUCCEEDED(s_e12.cl->lpVtbl->QueryInterface(s_e12.cl, &IID_ID3D12GraphicsCommandList1, (void**)&s_e12.cl1)))
            s_e12.depth_bounds_ok = 1;
    }
    if (e12_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, ENG12_RTV_HEAP, 0, &s_e12.rtv_heap) ||
        e12_heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, ENG12_DSV_HEAP, 0, &s_e12.dsv_heap) ||
        e12_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, ENG12_SRV_CPU_HEAP, 0, &s_e12.srv_cpu) ||
        e12_srv_gpu_heap() ||
        e12_heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, ENG12_SAMP_GPU_HEAP, 1, &s_e12.samp_gpu))
        goto fail;
    s_e12.rtv_inc = dv->lpVtbl->GetDescriptorHandleIncrementSize(dv, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    s_e12.dsv_inc = dv->lpVtbl->GetDescriptorHandleIncrementSize(dv, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    s_e12.srv_inc = dv->lpVtbl->GetDescriptorHandleIncrementSize(dv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    s_e12.samp_inc = dv->lpVtbl->GetDescriptorHandleIncrementSize(dv, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    {
        D3D12_RESOURCE_DESC b; memset(&b, 0, sizeof b);
        b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; b.Width = ENG12_UPLOAD_RING; b.Height = 1;
        b.DepthOrArraySize = 1; b.MipLevels = 1; b.SampleDesc.Count = 1; b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        s_e12.upload = e12_committed(D3D12_HEAP_TYPE_UPLOAD, &b, D3D12_RESOURCE_STATE_GENERIC_READ, NULL);
        D3D12_RANGE none = { 0, 0 };
        if (!s_e12.upload || FAILED(s_e12.upload->lpVtbl->Map(s_e12.upload, 0, &none, (void**)&s_e12.upload_ptr)))
            goto fail;
    }
    /* Null SRVs for unbound units, one per dimension a unit can declare. */
    s_e12.null_srv2d = e12_make_srv(NULL, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 0, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
    s_e12.null_srvcube = e12_make_srv(NULL, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
    if (!s_e12.null_srv2d || !s_e12.null_srvcube || e12_root_signature() || e12_blit_pipeline()) goto fail;
    s_e12.dbounds[0] = 0.0f; s_e12.dbounds[1] = 1.0f;
    s_e12.upload_ring.size = ENG12_UPLOAD_RING;
    s_e12.ready = 1;
    printf("[D3D12 engine] backend ready (depth bounds %s)\n", s_e12.depth_bounds_ok ? "on" : "unsupported");
    return 0;
fail:
    printf("[D3D12 engine] init failed\n");
    e12_shutdown(NULL);
    return -1;
}
#define E12_REL(p) do { if (p) { (p)->lpVtbl->Release(p); (p) = NULL; } } while (0)
static void e12_shutdown(void* user)
{
    (void)user;
    if (s_e12.fence) e12_submit_wait(E12_W_SHUTDOWN);
    for (u32 i = 1; i < ENG12_MAX_OBJ; i++) e12_release_obj(i);
    if (s_e12.fence) e12_submit_wait(E12_W_SHUTDOWN);
    e12_retire_now(~0ull);
    free(s_e12.retire);
    if (s_e12.upload && s_e12.upload_ptr) s_e12.upload->lpVtbl->Unmap(s_e12.upload, 0, NULL);
    E12_REL(s_e12.upload); E12_REL(s_e12.blit_pso); E12_REL(s_e12.blit_root); E12_REL(s_e12.root);
    E12_REL(s_e12.rtv_heap); E12_REL(s_e12.dsv_heap); E12_REL(s_e12.srv_cpu); E12_REL(s_e12.srv_gpu);
    E12_REL(s_e12.samp_gpu); E12_REL(s_e12.cl1); E12_REL(s_e12.cl); E12_REL(s_e12.fence);
    for (u32 i = 0; i < ENG12_ALLOCATORS; i++) E12_REL(s_e12.alloc[i]);
    if (s_e12.event) CloseHandle(s_e12.event);
    CRITICAL_SECTION keep = s_e12.lock; int had = s_e12.active;
    memset(&s_e12, 0, sizeof s_e12);
    s_e12.lock = keep; s_e12.active = had;
}
static void e12_submit_and_wait(void* user, u32 reason)
{
    (void)user; (void)reason;
    EnterCriticalSection(&s_e12.lock);
    e12_submit_wait(E12_W_ENGINE_FLUSH);
    LeaveCriticalSection(&s_e12.lock);
}
/* ---- backend: textures and targets ------------------------------------------- */
static u32 e12_texture_create(void* user, rsx_be_format fmt, u32 w, u32 h, u32 mips, u32 faces,
                              u32 remap, u32 rsx_fmt)
{
    (void)user;
    u32 bpp, block;
    const DXGI_FORMAT f = e12_format(fmt, &bpp, &block);
    const int cube = faces == 6;
    if (f == DXGI_FORMAT_UNKNOWN || !w || !h || w > 16384 || h > 16384 || (faces != 1 && !cube)) { e12_count(E12_X_FORMAT); return 0; }
    if (!mips) mips = 1;
    EnterCriticalSection(&s_e12.lock);
    u32 hd = e12_alloc();
    if (hd) {
        e12_obj* o = &s_e12o[hd];
        D3D12_RESOURCE_DESC d = e12_tex_desc(f, block ? ((w + 3u) & ~3u) : w, block ? ((h + 3u) & ~3u) : h,
                                             mips, cube ? 6 : 1, D3D12_RESOURCE_FLAG_NONE);
        o->res = e12_committed(D3D12_HEAP_TYPE_DEFAULT, &d, D3D12_RESOURCE_STATE_COMMON, NULL);
        if (o->res) {
            o->kind = E12_TEXTURE; o->fmt = f; o->w = w; o->h = h; o->mips = mips; o->faces = cube ? 6 : 1;
            o->cube = (u8)cube; o->bpp = bpp; o->block = block; o->state = D3D12_RESOURCE_STATE_COMMON;
            o->srv = e12_make_srv(o->res, f, mips, cube, e12_mapping(remap, rsx_fmt));
        }
        if (!o->res || !o->srv) { e12_count(E12_X_CREATE_FAILED); e12_release_obj(hd); hd = 0; }
    }
    LeaveCriticalSection(&s_e12.lock);
    return hd;
}
static void e12_texture_upload(void* user, u32 texture, u32 face, u32 mip, u32 w, u32 h,
                               const void* src, u32 row_bytes, u32 rows)
{
    (void)user;
    EnterCriticalSection(&s_e12.lock);
    e12_obj* o = e12_get(texture, E12_TEXTURE);
    if (o && src && face < o->faces && mip < o->mips)
        e12_copy_rows(o, mip + face * o->mips, 0, 0, w, h, src, row_bytes, rows);
    LeaveCriticalSection(&s_e12.lock);
}
static void e12_texture_release(void* user, u32 texture)
{
    (void)user;
    EnterCriticalSection(&s_e12.lock);
    if (texture < ENG12_MAX_OBJ && (s_e12o[texture].kind == E12_TEXTURE || s_e12o[texture].kind == E12_VIEW))
        e12_release_obj(texture);
    LeaveCriticalSection(&s_e12.lock);
}
static u32 e12_color_target_create(void* user, rsx_be_format fmt, u32 w, u32 h, const void* seed, u32 seed_row_bytes)
{
    (void)user;
    u32 bpp, block;
    const DXGI_FORMAT f = e12_format(fmt, &bpp, &block);
    if (f == DXGI_FORMAT_UNKNOWN || block || !w || !h || w > 16384 || h > 16384) { e12_count(E12_X_FORMAT); return 0; }
    EnterCriticalSection(&s_e12.lock);
    u32 hd = e12_alloc();
    if (hd) {
        e12_obj* o = &s_e12o[hd];
        D3D12_RESOURCE_DESC d = e12_tex_desc(f, w, h, 1, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        o->res = e12_committed(D3D12_HEAP_TYPE_DEFAULT, &d, D3D12_RESOURCE_STATE_COMMON, NULL);
        o->rtv = e12_slot_alloc(s_e12.rtv_used, ENG12_RTV_HEAP);
        if (o->res && o->rtv) {
            o->kind = E12_COLOR; o->fmt = f; o->w = w; o->h = h; o->mips = 1; o->faces = 1; o->bpp = bpp;
            o->state = D3D12_RESOURCE_STATE_COMMON;
            s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device, o->res, NULL,
                e12_cpu(s_e12.rtv_heap, s_e12.rtv_inc, o->rtv - 1));
            o->srv = e12_make_srv(o->res, f, 1, 0, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
            if (seed && seed_row_bytes) e12_copy_rows(o, 0, 0, 0, w, h, seed, seed_row_bytes, h);
            else if (e12_begin() == 0) {
                const float zero[4] = { 0, 0, 0, 0 };
                e12_transition(o, D3D12_RESOURCE_STATE_RENDER_TARGET);
                s_e12.cl->lpVtbl->ClearRenderTargetView(s_e12.cl, e12_cpu(s_e12.rtv_heap, s_e12.rtv_inc, o->rtv - 1), zero, 0, NULL);
            }
        }
        if (!o->res || !o->rtv || !o->srv) { e12_count(E12_X_CREATE_FAILED); e12_release_obj(hd); hd = 0; }
    }
    LeaveCriticalSection(&s_e12.lock);
    return hd;
}
/* An immutable copy of a colour target, for a draw that samples the target
 * it renders to. The copy is recorded now, after every draw already recorded
 * and before the next one; the engine samples it through surface_view and
 * releases it with texture_release (it retires on the fence). 0 on failure. */
static u32 e12_color_snapshot(void* user, u32 surface)
{
    (void)user;
    EnterCriticalSection(&s_e12.lock);
    u32 hd = 0;
    e12_obj* t = e12_get(surface, E12_COLOR);
    if (t && e12_begin() == 0 && (hd = e12_alloc()) != 0) {
        e12_obj* o = &s_e12o[hd];
        D3D12_RESOURCE_DESC d = e12_tex_desc(t->fmt, t->w, t->h, 1, 1, D3D12_RESOURCE_FLAG_NONE);
        o->res = e12_committed(D3D12_HEAP_TYPE_DEFAULT, &d, D3D12_RESOURCE_STATE_COPY_DEST, NULL);
        if (!o->res) { e12_count(E12_X_CREATE_FAILED); memset(o, 0, sizeof *o); hd = 0; }
        else {
            o->kind = E12_TEXTURE; o->snapshot = 1; o->fmt = t->fmt; o->w = t->w; o->h = t->h;
            o->mips = 1; o->faces = 1; o->bpp = t->bpp; o->state = D3D12_RESOURCE_STATE_COPY_DEST;
            e12_transition(t, D3D12_RESOURCE_STATE_COPY_SOURCE);
            s_e12.cl->lpVtbl->CopyResource(s_e12.cl, o->res, t->res);
            o->srv = e12_make_srv(o->res, t->fmt, 1, 0, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
            if (!o->srv) { e12_count(E12_X_CREATE_FAILED); e12_release_obj(hd); hd = 0; }
        }
    } else if (!t) e12_count(E12_X_DRAW_TARGETS);
    LeaveCriticalSection(&s_e12.lock);
    return hd;
}
static void e12_color_target_release(void* user, u32 surface)
{
    (void)user;
    EnterCriticalSection(&s_e12.lock);
    if (e12_get(surface, E12_COLOR)) e12_release_obj(surface);
    LeaveCriticalSection(&s_e12.lock);
}
static void e12_color_target_write(void* user, u32 surface, u32 x, u32 y, u32 w, u32 h, const void* rows, u32 row_bytes)
{
    (void)user;
    EnterCriticalSection(&s_e12.lock);
    e12_obj* o = e12_get(surface, E12_COLOR);
    if (o && rows && w && h && x < o->w && y < o->h) {
        if (o->fmt != DXGI_FORMAT_R8G8B8A8_UNORM) e12_count(E12_X_WRITE_FORMAT);   /* rows are R,G,B,A bytes */
        else {
            if (x + w > o->w) w = o->w - x;
            if (y + h > o->h) h = o->h - y;
            e12_copy_rows(o, 0, x, y, w, h, rows, row_bytes, h);
        }
    }
    LeaveCriticalSection(&s_e12.lock);
}
static u32 e12_surface_view(void* user, u32 surface, u32 remap, u32 rsx_format)
{
    (void)user;
    EnterCriticalSection(&s_e12.lock);
    u32 hd = 0;
    e12_obj* t = e12_get(surface, E12_COLOR);
    if (!t) { t = e12_get(surface, E12_TEXTURE); if (t && !t->snapshot) t = NULL; }   /* colour snapshots too */
    if (t) {
        for (u32 i = 1; i < ENG12_MAX_OBJ && !hd; i++)
            if (s_e12o[i].kind == E12_VIEW && s_e12o[i].parent == surface && s_e12o[i].remap == remap &&
                s_e12o[i].rsx_fmt == rsx_format) hd = i;
        if (!hd && (hd = e12_alloc()) != 0) {
            e12_obj* o = &s_e12o[hd];
            o->srv = e12_make_srv(t->res, t->fmt, 1, 0, e12_mapping(remap, rsx_format));
            if (!o->srv) { memset(o, 0, sizeof *o); hd = 0; }
            else { o->kind = E12_VIEW; o->parent = surface; o->remap = remap; o->rsx_fmt = rsx_format;
                   o->fmt = t->fmt; o->w = t->w; o->h = t->h; }
        }
    }
    LeaveCriticalSection(&s_e12.lock);
    return hd;
}
static u32 e12_depth_create_locked(u32 w, u32 h)
{
    u32 hd = e12_alloc();
    if (!hd) return 0;
    e12_obj* o = &s_e12o[hd];
    D3D12_RESOURCE_DESC d = e12_tex_desc(ENG12_DEPTH_TYPELESS, w, h, 1, 1, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    D3D12_CLEAR_VALUE cv; memset(&cv, 0, sizeof cv); cv.Format = ENG12_DEPTH_FORMAT; cv.DepthStencil.Depth = 1.0f;
    o->res = e12_committed(D3D12_HEAP_TYPE_DEFAULT, &d, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv);
    o->dsv = e12_slot_alloc(s_e12.dsv_used, ENG12_DSV_HEAP);
    if (!o->res || !o->dsv) { e12_count(E12_X_CREATE_FAILED); e12_release_obj(hd); return 0; }
    o->kind = E12_DEPTH; o->fmt = ENG12_DEPTH_FORMAT; o->w = w; o->h = h; o->mips = 1; o->faces = 1;
    o->state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    D3D12_DEPTH_STENCIL_VIEW_DESC dv; memset(&dv, 0, sizeof dv);
    dv.Format = ENG12_DEPTH_FORMAT; dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    s_d3d.device->lpVtbl->CreateDepthStencilView(s_d3d.device, o->res, &dv, e12_cpu(s_e12.dsv_heap, s_e12.dsv_inc, o->dsv - 1));
    if (e12_begin() == 0)
        s_e12.cl->lpVtbl->ClearDepthStencilView(s_e12.cl, e12_cpu(s_e12.dsv_heap, s_e12.dsv_inc, o->dsv - 1),
            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
    return hd;
}
static u32 e12_depth_target_create(void* user, u32 w, u32 h)
{
    (void)user;
    if (!w || !h || w > 16384 || h > 16384) { e12_count(E12_X_FORMAT); return 0; }
    EnterCriticalSection(&s_e12.lock);
    const u32 hd = e12_depth_create_locked(w, h);
    LeaveCriticalSection(&s_e12.lock);
    return hd;
}
static void e12_depth_target_release(void* user, u32 depth)
{
    (void)user;
    EnterCriticalSection(&s_e12.lock);
    if (e12_get(depth, E12_DEPTH)) e12_release_obj(depth);
    LeaveCriticalSection(&s_e12.lock);
}
/* A sampleable copy of the depth plane: a new texture the engine owns. */
static u32 e12_depth_snapshot(void* user, u32 depth, u32 w, u32 h)
{
    (void)user; (void)w; (void)h;
    EnterCriticalSection(&s_e12.lock);
    u32 hd = 0;
    e12_obj* z = e12_get(depth, E12_DEPTH);
    if (z && e12_begin() == 0 && (hd = e12_alloc()) != 0) {
        e12_obj* o = &s_e12o[hd];
        D3D12_RESOURCE_DESC d = e12_tex_desc(ENG12_DEPTH_TYPELESS, z->w, z->h, 1, 1, D3D12_RESOURCE_FLAG_NONE);
        o->res = e12_committed(D3D12_HEAP_TYPE_DEFAULT, &d, D3D12_RESOURCE_STATE_COPY_DEST, NULL);
        if (!o->res) { e12_count(E12_X_CREATE_FAILED); memset(o, 0, sizeof *o); hd = 0; }
        else {
            o->kind = E12_TEXTURE; o->fmt = ENG12_DEPTH_SRV; o->w = z->w; o->h = z->h; o->mips = 1; o->faces = 1;
            o->state = D3D12_RESOURCE_STATE_COPY_DEST;
            e12_transition(z, D3D12_RESOURCE_STATE_COPY_SOURCE);
            s_e12.cl->lpVtbl->CopyResource(s_e12.cl, o->res, z->res);
            e12_transition(z, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            o->srv = e12_make_srv(o->res, ENG12_DEPTH_SRV, 1, 0, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
            if (!o->srv) { e12_release_obj(hd); hd = 0; }
        }
    }
    LeaveCriticalSection(&s_e12.lock);
    return hd;
}

/* ---- backend: pipelines ------------------------------------------------------ */
static u32 e12_cube_units(const char* ps_hlsl)
{
    u32 mask = 0; char decl[40];
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
        snprintf(decl, sizeof decl, "TextureCube rsx_tex%u ", u);
        if (strstr(ps_hlsl, decl)) mask |= 1u << u;
    }
    return mask;
}
static u32 e12_pipeline_create(void* user, const char* vs_hlsl, const char* ps_hlsl, const rsx_be_render_state* rs,
                               const rsx_vertex_layout_plan* layout, u32 vertex_stride, rsx_be_format rt_fmt, u32 rt_count)
{
    (void)user;
    if (!vs_hlsl || !ps_hlsl || !rs || !layout || !vertex_stride) return 0;
    u32 bpp, block;
    const DXGI_FORMAT cf = e12_format(rt_fmt, &bpp, &block);
    if (cf == DXGI_FORMAT_UNKNOWN || block) { e12_count(E12_X_FORMAT); return 0; }
    if (!rt_count) rt_count = 1;
    if (rt_count > RSX_BE_MAX_COLOR_TARGETS) rt_count = RSX_BE_MAX_COLOR_TARGETS;
    ID3DBlob* vs = e12_compile(vs_hlsl, "vs_5_0", E12_X_COMPILE_VS);
    if (!vs) return 0;
    ID3DBlob* ps = e12_compile(ps_hlsl, "ps_5_0", E12_X_COMPILE_PS);
    if (!ps) { vs->lpVtbl->Release(vs); return 0; }
    EnterCriticalSection(&s_e12.lock);
    const u32 hd = e12_alloc();
    if (hd) {
        e12_obj* o = &s_e12o[hd];
        o->kind = E12_PIPELINE; o->vs = vs; o->ps = ps; o->rs = *rs; o->fmt = cf; o->rt_count = (u8)rt_count;
        o->stride = vertex_stride; o->nslots = layout->count < RSX_DSP_NUM_VERTEX_ATTR ? layout->count : RSX_DSP_NUM_VERTEX_ATTR;
        memcpy(o->attrs, layout->attrs, sizeof o->attrs);
        o->cube_mask = e12_cube_units(ps_hlsl);
    } else { vs->lpVtbl->Release(vs); ps->lpVtbl->Release(ps); }
    LeaveCriticalSection(&s_e12.lock);
    return hd;
}
static void e12_pipeline_release(void* user, u32 pipeline)
{
    (void)user;
    EnterCriticalSection(&s_e12.lock);
    if (e12_get(pipeline, E12_PIPELINE)) e12_release_obj(pipeline);
    LeaveCriticalSection(&s_e12.lock);
}
/* The same pipeline as a full graphics description, built as a pipeline
 * state stream so it can carry DEPTH_STENCIL1 with the depth bounds test on.
 * Each subobject is a type tag followed by its value, pointer-aligned. */
#define E12_SUBOBJ(T) struct { _Alignas(void*) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type; T v; }
static int e12_pso_with_depth_bounds(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* g, ID3D12PipelineState** out)
{
    ID3D12Device2* dv2 = NULL;
    if (FAILED(s_d3d.device->lpVtbl->QueryInterface(s_d3d.device, &IID_ID3D12Device2, (void**)&dv2)) || !dv2) return -1;
    struct {
        E12_SUBOBJ(ID3D12RootSignature*) root;
        E12_SUBOBJ(D3D12_SHADER_BYTECODE) vs;
        E12_SUBOBJ(D3D12_SHADER_BYTECODE) ps;
        E12_SUBOBJ(D3D12_BLEND_DESC) blend;
        E12_SUBOBJ(UINT) sample_mask;
        E12_SUBOBJ(D3D12_RASTERIZER_DESC) raster;
        E12_SUBOBJ(D3D12_DEPTH_STENCIL_DESC1) ds;
        E12_SUBOBJ(D3D12_INPUT_LAYOUT_DESC) il;
        E12_SUBOBJ(D3D12_PRIMITIVE_TOPOLOGY_TYPE) topo;
        E12_SUBOBJ(struct D3D12_RT_FORMAT_ARRAY) rtv;
        E12_SUBOBJ(DXGI_FORMAT) dsv;
        E12_SUBOBJ(DXGI_SAMPLE_DESC) sample;
    } st;
    memset(&st, 0, sizeof st);
    st.root.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE; st.root.v = g->pRootSignature;
    st.vs.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS; st.vs.v = g->VS;
    st.ps.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS; st.ps.v = g->PS;
    st.blend.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND; st.blend.v = g->BlendState;
    st.sample_mask.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK; st.sample_mask.v = g->SampleMask;
    st.raster.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER; st.raster.v = g->RasterizerState;
    st.ds.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1;
    st.ds.v.DepthEnable = g->DepthStencilState.DepthEnable;
    st.ds.v.DepthWriteMask = g->DepthStencilState.DepthWriteMask;
    st.ds.v.DepthFunc = g->DepthStencilState.DepthFunc;
    st.ds.v.StencilEnable = g->DepthStencilState.StencilEnable;
    st.ds.v.StencilReadMask = g->DepthStencilState.StencilReadMask;
    st.ds.v.StencilWriteMask = g->DepthStencilState.StencilWriteMask;
    st.ds.v.FrontFace = g->DepthStencilState.FrontFace;
    st.ds.v.BackFace = g->DepthStencilState.BackFace;
    st.ds.v.DepthBoundsTestEnable = TRUE;
    st.il.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT; st.il.v = g->InputLayout;
    st.topo.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY; st.topo.v = g->PrimitiveTopologyType;
    st.rtv.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS;
    st.rtv.v.NumRenderTargets = g->NumRenderTargets;
    for (u32 r = 0; r < 8; r++) st.rtv.v.RTFormats[r] = g->RTVFormats[r];
    st.dsv.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT; st.dsv.v = g->DSVFormat;
    st.sample.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC; st.sample.v = g->SampleDesc;
    D3D12_PIPELINE_STATE_STREAM_DESC sd; sd.SizeInBytes = sizeof st; sd.pPipelineStateSubobjectStream = &st;
    const HRESULT hr = dv2->lpVtbl->CreatePipelineState(dv2, &sd, &IID_ID3D12PipelineState, (void**)out);
    dv2->lpVtbl->Release(dv2);
    if (FAILED(hr)) { *out = NULL; return -1; }
    return 0;
}
static ID3D12PipelineState* e12_pso(e12_obj* P, int type)
{
    if (P->pso[type]) return P->pso[type];
    const rsx_be_render_state* rs = &P->rs;
    D3D12_INPUT_ELEMENT_DESC ie[RSX_DSP_NUM_VERTEX_ATTR];
    for (u32 s = 0; s < P->nslots; s++) {
        memset(&ie[s], 0, sizeof ie[s]);
        ie[s].SemanticName = "ATTR"; ie[s].SemanticIndex = P->attrs[s];
        ie[s].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; ie[s].AlignedByteOffset = s * 16u;
        ie[s].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d; memset(&d, 0, sizeof d);
    d.pRootSignature = s_e12.root;
    d.VS.pShaderBytecode = P->vs->lpVtbl->GetBufferPointer(P->vs); d.VS.BytecodeLength = P->vs->lpVtbl->GetBufferSize(P->vs);
    d.PS.pShaderBytecode = P->ps->lpVtbl->GetBufferPointer(P->ps); d.PS.BytecodeLength = P->ps->lpVtbl->GetBufferSize(P->ps);
    d.InputLayout.pInputElementDescs = ie; d.InputLayout.NumElements = P->nslots;
    /* nv40 COLOR_MASK: B [0:7], G [8:15], R [16:23], A [24:31]; every attachment
     * takes target A's blend and mask (rsx_draw_engine.h). */
    UINT8 wm = 0;
    if ((rs->color_mask >> 16) & 0xFFu) wm |= D3D12_COLOR_WRITE_ENABLE_RED;
    if ((rs->color_mask >>  8) & 0xFFu) wm |= D3D12_COLOR_WRITE_ENABLE_GREEN;
    if ((rs->color_mask >>  0) & 0xFFu) wm |= D3D12_COLOR_WRITE_ENABLE_BLUE;
    if ((rs->color_mask >> 24) & 0xFFu) wm |= D3D12_COLOR_WRITE_ENABLE_ALPHA;
    d.BlendState.IndependentBlendEnable = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC* b = &d.BlendState.RenderTarget[0];
    b->BlendEnable = rs->blend_enable ? TRUE : FALSE;
    b->SrcBlend = (D3D12_BLEND)gl_blend_factor_d3d(rs->sf_rgb, 0);
    b->DestBlend = (D3D12_BLEND)gl_blend_factor_d3d(rs->df_rgb, 0);
    b->BlendOp = (D3D12_BLEND_OP)gl_blend_op_d3d(rs->eq_rgb);
    b->SrcBlendAlpha = (D3D12_BLEND)gl_blend_factor_d3d(rs->sf_a, 1);
    b->DestBlendAlpha = (D3D12_BLEND)gl_blend_factor_d3d(rs->df_a, 1);
    b->BlendOpAlpha = (D3D12_BLEND_OP)gl_blend_op_d3d(rs->eq_a);
    b->LogicOp = D3D12_LOGIC_OP_NOOP;
    b->RenderTargetWriteMask = wm;
    d.SampleMask = 0xFFFFFFFFu;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = !rs->cull_enable ? D3D12_CULL_MODE_NONE
                               : (rs->cull_face == 0x0404u || rs->cull_face == 0x0408u) ? D3D12_CULL_MODE_FRONT
                                                                                        : D3D12_CULL_MODE_BACK;
    d.RasterizerState.FrontCounterClockwise = rs->front_face == 0x0901u ? TRUE : FALSE;
    /* rsx_be_render_state.depth_clamp: no near/far clipping. D3D12 clamps the
     * depth to the viewport's range either way, so that is the clamp. */
    d.RasterizerState.DepthClipEnable = rs->depth_clamp ? FALSE : TRUE;
    d.DepthStencilState.DepthEnable = rs->depth_test ? TRUE : FALSE;
    d.DepthStencilState.DepthWriteMask = (rs->depth_test && rs->depth_write) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    d.DepthStencilState.DepthFunc = e12_compare(rs->depth_func);
    d.DepthStencilState.StencilEnable = rs->stencil_enable ? TRUE : FALSE;
    d.DepthStencilState.StencilReadMask = (UINT8)rs->s_func_mask;
    d.DepthStencilState.StencilWriteMask = (UINT8)rs->s_write_mask;
    D3D12_DEPTH_STENCILOP_DESC fr = { e12_stencil_op(rs->s_fail), e12_stencil_op(rs->s_zfail),
                                      e12_stencil_op(rs->s_zpass), e12_compare(rs->s_func) };
    d.DepthStencilState.FrontFace = fr;
    if (rs->stencil_two_sided) {
        D3D12_DEPTH_STENCILOP_DESC bk = { e12_stencil_op(rs->bs_fail), e12_stencil_op(rs->bs_zfail),
                                          e12_stencil_op(rs->bs_zpass), e12_compare(rs->bs_func) };
        d.DepthStencilState.BackFace = bk;
    } else d.DepthStencilState.BackFace = fr;
    d.DSVFormat = ENG12_DEPTH_FORMAT;
    static const D3D12_PRIMITIVE_TOPOLOGY_TYPE tt[3] = { D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT,
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE };
    d.PrimitiveTopologyType = tt[type];
    d.NumRenderTargets = P->rt_count;
    for (u32 r = 0; r < P->rt_count; r++) d.RTVFormats[r] = P->fmt;
    d.SampleDesc.Count = 1;
    /* The depth bounds test is only in the stream-built description
     * (DEPTH_STENCIL1). Without device support a request is counted and the
     * draw runs without the test. */
    if (rs->depth_bounds_test) {
        if (s_e12.depth_bounds_ok && e12_pso_with_depth_bounds(&d, &P->pso[type]) == 0) return P->pso[type];
        e12_count(E12_X_DEPTH_BOUNDS_UNSUPPORTED);
    }
    if (FAILED(s_d3d.device->lpVtbl->CreateGraphicsPipelineState(s_d3d.device, &d, &IID_ID3D12PipelineState, (void**)&P->pso[type]))) {
        e12_count(E12_X_PSO);
        P->pso[type] = NULL;
    }
    return P->pso[type];
}

/* ---- backend: per-draw binding -------------------------------------------------- */
static void e12_bind_targets(void* user, const u32* surfaces, u32 count, u32 depth)
{
    (void)user;
    EnterCriticalSection(&s_e12.lock);
    memset(s_e12.rt, 0, sizeof s_e12.rt);
    s_e12.nrt = count > RSX_BE_MAX_COLOR_TARGETS ? RSX_BE_MAX_COLOR_TARGETS : count;
    for (u32 i = 0; i < s_e12.nrt; i++) s_e12.rt[i] = surfaces ? surfaces[i] : 0;
    s_e12.depth = depth;
    LeaveCriticalSection(&s_e12.lock);
}
static void e12_bind_pipeline(void* user, u32 pipeline) { (void)user; s_e12.pipeline = pipeline; }
static void e12_bind_vs_constants(void* user, const void* data, u32 bytes)
{
    (void)user;
    if (bytes > sizeof s_e12.vsc) { e12_count(E12_X_UPLOAD_TOO_LARGE); bytes = sizeof s_e12.vsc; }
    if (data && bytes) memcpy(s_e12.vsc, data, bytes);
    s_e12.vs_bytes = data ? bytes : 0;
}
static void e12_bind_ps_constants(void* user, const void* data, u32 bytes)
{
    (void)user;
    if (bytes > sizeof s_e12.psc) { e12_count(E12_X_UPLOAD_TOO_LARGE); bytes = sizeof s_e12.psc; }
    if (data && bytes) memcpy(s_e12.psc, data, bytes);
    s_e12.ps_bytes = data ? bytes : 0;
}
static void e12_bind_textures(void* user, const u32* textures, const rsx_be_sampler_desc* samplers, u32 mask)
{
    (void)user;
    s_e12.tex_mask = mask;
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
        s_e12.tex[u] = (mask & (1u << u)) && textures ? textures[u] : 0;
        if (samplers) s_e12.samp[u] = samplers[u]; else memset(&s_e12.samp[u], 0, sizeof s_e12.samp[u]);
    }
}
static void e12_bind_vertex_textures(void* user, const u32* textures, const rsx_be_sampler_desc* samplers, u32 mask)
{
    (void)user;
    s_e12.vtex_mask = mask;
    for (u32 u = 0; u < ENG12_VTEX; u++) {
        s_e12.vtex[u] = (mask & (1u << u)) && textures ? textures[u] : 0;
        if (samplers) s_e12.vsamp[u] = samplers[u]; else memset(&s_e12.vsamp[u], 0, sizeof s_e12.vsamp[u]);
    }
}
static void e12_set_viewport(void* user, float x, float y, float w, float h)
{ (void)user; s_e12.vp[0] = x; s_e12.vp[1] = y; s_e12.vp[2] = w; s_e12.vp[3] = h; s_e12.have_vp = 1; }
static void e12_set_scissor(void* user, u32 x, u32 y, u32 w, u32 h)
{ (void)user; s_e12.sc[0] = x; s_e12.sc[1] = y; s_e12.sc[2] = w; s_e12.sc[3] = h; s_e12.have_sc = 1; }
static void e12_set_stencil_ref(void* user, u32 ref) { (void)user; s_e12.stencil_ref = ref; }
static void e12_set_depth_bounds(void* user, float mn, float mx)
{
    (void)user;
    /* Clamped to [0, 1]; NaN opens its end of the range, as the Vulkan
     * backend does. An inverted range passes nothing on the RSX, and D3D12
     * has no empty bounds (it needs min <= max), so such draws are skipped. */
    mn = (mn == mn) ? (mn > 0.0f ? (mn < 1.0f ? mn : 1.0f) : 0.0f) : 0.0f;
    mx = (mx == mx) ? (mx > 0.0f ? (mx < 1.0f ? mx : 1.0f) : 0.0f) : 1.0f;
    s_e12.dbounds_empty = mn > mx;
    if (s_e12.dbounds_empty) mn = mx;
    s_e12.dbounds[0] = mn; s_e12.dbounds[1] = mx;
}

/* The SRV a unit samples: a texture, a colour target, a view; else the null
 * SRV of the unit's dimension. A colour target sampled here leaves render-
 * target state for the draw. */
static u32 e12_unit_srv(u32 handle, int cube)
{
    if (handle && handle < ENG12_MAX_OBJ) {
        e12_obj* o = &s_e12o[handle];
        e12_obj* res = o;
        if (o->kind == E12_VIEW) {
            res = e12_get(o->parent, E12_COLOR);
            if (!res) res = e12_get(o->parent, E12_TEXTURE);
        }
        if ((o->kind == E12_TEXTURE || o->kind == E12_COLOR || o->kind == E12_VIEW) && res && o->srv &&
            (int)res->cube == cube) {
            e12_transition(res, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            return o->srv;
        }
    }
    return cube ? s_e12.null_srvcube : s_e12.null_srv2d;
}
/* Sampler tables live in the shader-visible sampler heap, deduplicated: most
 * draws repeat a handful of tables. A full heap resets after a wait. */
static int e12_sampler_table(const rsx_be_sampler_desc* d, u32 n, D3D12_GPU_DESCRIPTOR_HANDLE* out)
{
    u64 key[RSX_BE_MAX_TEXTURES];
    for (u32 i = 0; i < n; i++) key[i] = e12_sampler_key(&d[i]);
    for (u32 t = 0; t < s_e12.stab_n; t++)
        if (s_e12.stab[t].n == n && !memcmp(s_e12.stab[t].key, key, n * sizeof key[0])) {
            *out = e12_gpu(s_e12.samp_gpu, s_e12.samp_inc, s_e12.stab[t].base); return 0;
        }
    if (s_e12.samp_gpu_next + n > ENG12_SAMP_GPU_HEAP || s_e12.stab_n == sizeof s_e12.stab / sizeof s_e12.stab[0]) {
        e12_count(E12_X_SAMPLER_TABLES);
        if (e12_submit_wait(E12_W_SAMPLER_TABLES) || e12_begin()) return -1;
        s_e12.samp_gpu_next = 0; s_e12.stab_n = 0;
    }
    const u32 base = s_e12.samp_gpu_next;
    for (u32 i = 0; i < n; i++) {
        D3D12_SAMPLER_DESC sd; e12_sampler_desc(&d[i], &sd);
        s_d3d.device->lpVtbl->CreateSampler(s_d3d.device, &sd, e12_cpu(s_e12.samp_gpu, s_e12.samp_inc, base + i));
    }
    s_e12.samp_gpu_next += n;
    s_e12.stab[s_e12.stab_n].n = n; s_e12.stab[s_e12.stab_n].base = base;
    memcpy(s_e12.stab[s_e12.stab_n++].key, key, n * sizeof key[0]);
    *out = e12_gpu(s_e12.samp_gpu, s_e12.samp_inc, base);
    return 0;
}
static int e12_srv_table(const u32* srv_slots, u32 n, D3D12_GPU_DESCRIPTOR_HANDLE* out)
{
    u64 at = 0;
    if (e12_ring_alloc(&s_e12.srv_ring, n, 1, E12_W_SRV_TABLES, &at)) return -1;
    const u32 base = (u32)at;
    for (u32 i = 0; i < n; i++)
        s_d3d.device->lpVtbl->CopyDescriptorsSimple(s_d3d.device, 1, e12_cpu(s_e12.srv_gpu, s_e12.srv_inc, base + i),
            e12_cpu(s_e12.srv_cpu, s_e12.srv_inc, srv_slots[i] - 1), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    *out = e12_gpu(s_e12.srv_gpu, s_e12.srv_inc, base);
    return 0;
}
static D3D12_CPU_DESCRIPTOR_HANDLE e12_scratch_dsv(u32 w, u32 h, e12_obj** zo)
{
    e12_obj* z = e12_get(s_e12.scratch_depth, E12_DEPTH);
    if (!z || z->w < w || z->h < h) {
        if (z) e12_release_obj(s_e12.scratch_depth);
        s_e12.scratch_depth = e12_depth_create_locked(w, h);
        z = e12_get(s_e12.scratch_depth, E12_DEPTH);
    }
    *zo = z;
    D3D12_CPU_DESCRIPTOR_HANDLE none = { 0 };
    return z ? e12_cpu(s_e12.dsv_heap, s_e12.dsv_inc, z->dsv - 1) : none;
}
static void e12_draw(void* user, rsx_topology topology, const void* vertices, u32 vertex_count, u32 stride,
                     const u32* indices, u32 index_count)
{
    (void)user;
    if (!vertices || !vertex_count || !stride) return;
    EnterCriticalSection(&s_e12.lock);
    s_e12.draws++;
    e12_obj* P = e12_get(s_e12.pipeline, E12_PIPELINE);
    if (P && P->rs.depth_bounds_test && s_e12.dbounds_empty && s_e12.depth_bounds_ok) {
        s_e12.bounds_empty_draws++;   /* an inverted range: nothing passes, as on the RSX */
        LeaveCriticalSection(&s_e12.lock); return;
    }
    int type = -1;
    D3D12_PRIMITIVE_TOPOLOGY topo = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    switch (topology) {
    case RSX_TOPOLOGY_POINTS:         type = 0; topo = D3D_PRIMITIVE_TOPOLOGY_POINTLIST; break;
    case RSX_TOPOLOGY_LINES:          type = 1; topo = D3D_PRIMITIVE_TOPOLOGY_LINELIST; break;
    case RSX_TOPOLOGY_LINE_STRIP:     type = 1; topo = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP; break;
    case RSX_TOPOLOGY_TRIANGLES:      type = 2; topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
    case RSX_TOPOLOGY_TRIANGLE_STRIP: type = 2; topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
    default: break;
    }
    e12_obj* rt[RSX_BE_MAX_COLOR_TARGETS] = { 0 };
    int reason = -1;
    if (!P) reason = E12_X_DRAW_NO_PIPELINE;
    else if (type < 0) reason = E12_X_DRAW_TOPOLOGY;
    else if (stride != P->stride) reason = E12_X_DRAW_STRIDE;
    else if (s_e12.nrt < 1 || s_e12.nrt != P->rt_count) reason = E12_X_DRAW_TARGETS;
    else {
        for (u32 i = 0; i < s_e12.nrt && reason < 0; i++) {
            rt[i] = e12_get(s_e12.rt[i], E12_COLOR);
            if (!rt[i]) reason = E12_X_DRAW_TARGETS;
            else if (rt[i]->fmt != P->fmt) reason = E12_X_DRAW_TARGET_FORMAT;
        }
    }
    ID3D12PipelineState* pso = reason < 0 ? e12_pso(P, type) : NULL;
    if (reason < 0 && !pso) reason = E12_X_PSO;
    if (reason >= 0 || e12_begin()) { if (reason >= 0) e12_count(reason); LeaveCriticalSection(&s_e12.lock); return; }

    const u32 w = rt[0]->w, h = rt[0]->h;
    e12_obj* z = e12_get(s_e12.depth, E12_DEPTH);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv;
    if (z && z->w >= w && z->h >= h) dsv = e12_cpu(s_e12.dsv_heap, s_e12.dsv_inc, z->dsv - 1);
    else dsv = e12_scratch_dsv(w, h, &z);
    if (!z) { e12_count(E12_X_CREATE_FAILED); LeaveCriticalSection(&s_e12.lock); return; }

    /* Upload everything the GPU reads from host memory. Any wait or flush the
     * draw needs (sampler tables, upload ring, descriptor ring) happens before
     * its first allocation, never between two of them. */
    D3D12_GPU_VIRTUAL_ADDRESS vb_va = 0, ib_va = 0, vc_va = 0, pc_va = 0;
    const u64 vbytes = (u64)vertex_count * stride, ibytes = indices && index_count ? (u64)index_count * 4u : 0;
    {
        const u32 per_draw_samplers = RSX_BE_MAX_TEXTURES + ENG12_VTEX;
        if (s_e12.samp_gpu_next + per_draw_samplers > ENG12_SAMP_GPU_HEAP ||
            s_e12.stab_n + 2 > sizeof s_e12.stab / sizeof s_e12.stab[0]) {
            e12_count(E12_X_SAMPLER_TABLES);
            if (e12_submit_wait(E12_W_SAMPLER_TABLES) || e12_begin()) { LeaveCriticalSection(&s_e12.lock); return; }
            s_e12.samp_gpu_next = 0; s_e12.stab_n = 0;
        }
        const u64 vsb0 = (s_e12.vs_bytes + 255u) & ~255u, psb0 = (s_e12.ps_bytes + 255u) & ~255u;
        const u64 up_need = vbytes + 16 + ibytes + 16 + (vsb0 ? vsb0 : 256) + 256 + (psb0 ? psb0 : 256) + 256;
        if (e12_ring_reserve(&s_e12.upload_ring, up_need, E12_W_UPLOAD_RING) ||
            e12_ring_reserve(&s_e12.srv_ring, RSX_BE_MAX_TEXTURES + ENG12_VTEX, E12_W_SRV_TABLES) ||
            e12_ring_reserve(&s_e12.upload_ring, up_need, E12_W_UPLOAD_RING)) {   /* a descriptor flush may have reset it */
            e12_count(E12_X_UPLOAD_TOO_LARGE); LeaveCriticalSection(&s_e12.lock); return;
        }
        if (!s_e12.recording && e12_begin()) { LeaveCriticalSection(&s_e12.lock); return; }
    }
    void* vp = e12_upload(vbytes, 16, &vb_va, NULL);
    void* ip = ibytes ? e12_upload(ibytes, 16, &ib_va, NULL) : NULL;
    const u32 vsb = (s_e12.vs_bytes + 255u) & ~255u, psb = (s_e12.ps_bytes + 255u) & ~255u;
    void* vc = e12_upload(vsb ? vsb : 256, 256, &vc_va, NULL);
    void* pc = e12_upload(psb ? psb : 256, 256, &pc_va, NULL);
    if (!vp || (ibytes && !ip) || !vc || !pc) { LeaveCriticalSection(&s_e12.lock); return; }
    if (!s_e12.recording && e12_begin()) { LeaveCriticalSection(&s_e12.lock); return; }   /* a ring wrap flushed */
    memcpy(vp, vertices, (size_t)vbytes);
    if (ip) memcpy(ip, indices, (size_t)ibytes);
    memset(vc, 0, vsb ? vsb : 256); memcpy(vc, s_e12.vsc, s_e12.vs_bytes);
    memset(pc, 0, psb ? psb : 256); memcpy(pc, s_e12.psc, s_e12.ps_bytes);

    /* Descriptor tables. Sampling transitions come before the target transitions. */
    u32 srvs[RSX_BE_MAX_TEXTURES], vsrvs[ENG12_VTEX];
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) srvs[u] = e12_unit_srv(s_e12.tex[u], (P->cube_mask >> u) & 1u);
    for (u32 u = 0; u < ENG12_VTEX; u++) vsrvs[u] = e12_unit_srv(s_e12.vtex[u], 0);
    D3D12_GPU_DESCRIPTOR_HANDLE t_ps, s_ps, t_vs, s_vs;
    if (e12_srv_table(srvs, RSX_BE_MAX_TEXTURES, &t_ps) || e12_srv_table(vsrvs, ENG12_VTEX, &t_vs) ||
        e12_sampler_table(s_e12.samp, RSX_BE_MAX_TEXTURES, &s_ps) || e12_sampler_table(s_e12.vsamp, ENG12_VTEX, &s_vs)) {
        LeaveCriticalSection(&s_e12.lock); return;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[RSX_BE_MAX_COLOR_TARGETS];
    for (u32 i = 0; i < s_e12.nrt; i++) {
        e12_transition(rt[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
        rtvs[i] = e12_cpu(s_e12.rtv_heap, s_e12.rtv_inc, rt[i]->rtv - 1);
    }
    e12_transition(z, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    ID3D12GraphicsCommandList* cl = s_e12.cl;
    cl->lpVtbl->OMSetRenderTargets(cl, s_e12.nrt, rtvs, FALSE, &dsv);
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_e12.root);
    cl->lpVtbl->SetPipelineState(cl, pso);
    if (P->rs.depth_bounds_test && s_e12.depth_bounds_ok && s_e12.cl1)
        s_e12.cl1->lpVtbl->OMSetDepthBounds(s_e12.cl1, s_e12.dbounds[0], s_e12.dbounds[1]);
    cl->lpVtbl->SetGraphicsRootConstantBufferView(cl, 0, vc_va);
    cl->lpVtbl->SetGraphicsRootConstantBufferView(cl, 1, pc_va);
    cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 2, t_ps);
    cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 3, s_ps);
    cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 4, t_vs);
    cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 5, s_vs);
    D3D12_VIEWPORT vpt = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    if (s_e12.have_vp && s_e12.vp[2] > 0.0f && s_e12.vp[3] > 0.0f) {
        vpt.TopLeftX = s_e12.vp[0]; vpt.TopLeftY = s_e12.vp[1]; vpt.Width = s_e12.vp[2]; vpt.Height = s_e12.vp[3];
    }
    D3D12_RECT sc = { 0, 0, (LONG)w, (LONG)h };
    if (s_e12.have_sc) {
        u32 x0 = s_e12.sc[0] < w ? s_e12.sc[0] : w, y0 = s_e12.sc[1] < h ? s_e12.sc[1] : h;
        u32 x1 = s_e12.sc[0] + s_e12.sc[2], y1 = s_e12.sc[1] + s_e12.sc[3];
        if (x1 > w) x1 = w;
        if (y1 > h) y1 = h;
        sc.left = (LONG)x0; sc.top = (LONG)y0; sc.right = (LONG)(x1 > x0 ? x1 : x0); sc.bottom = (LONG)(y1 > y0 ? y1 : y0);
    }
    cl->lpVtbl->RSSetViewports(cl, 1, &vpt);
    cl->lpVtbl->RSSetScissorRects(cl, 1, &sc);
    cl->lpVtbl->OMSetStencilRef(cl, s_e12.stencil_ref & 0xFFu);
    cl->lpVtbl->IASetPrimitiveTopology(cl, topo);
    D3D12_VERTEX_BUFFER_VIEW vbv = { vb_va, (UINT)vbytes, stride };
    cl->lpVtbl->IASetVertexBuffers(cl, 0, 1, &vbv);
    if (ip) {
        D3D12_INDEX_BUFFER_VIEW ibv = { ib_va, (UINT)ibytes, DXGI_FORMAT_R32_UINT };
        cl->lpVtbl->IASetIndexBuffer(cl, &ibv);
        cl->lpVtbl->DrawIndexedInstanced(cl, index_count, 1, 0, 0, 0);
    } else {
        cl->lpVtbl->DrawInstanced(cl, vertex_count, 1, 0, 0);
    }
    LeaveCriticalSection(&s_e12.lock);
}
static void e12_clear_color(void* user, u32 surface, const float rgba[4])
{
    (void)user;
    EnterCriticalSection(&s_e12.lock);
    e12_obj* o = e12_get(surface, E12_COLOR);
    if (o && e12_begin() == 0) {
        e12_transition(o, D3D12_RESOURCE_STATE_RENDER_TARGET);
        s_e12.cl->lpVtbl->ClearRenderTargetView(s_e12.cl, e12_cpu(s_e12.rtv_heap, s_e12.rtv_inc, o->rtv - 1), rgba, 0, NULL);
    }
    LeaveCriticalSection(&s_e12.lock);
}
static void e12_clear_depth_stencil(void* user, u32 depth, u32 flags, float depth_value, u8 stencil)
{
    (void)user;
    EnterCriticalSection(&s_e12.lock);
    e12_obj* o = e12_get(depth, E12_DEPTH);
    D3D12_CLEAR_FLAGS f = (D3D12_CLEAR_FLAGS)(((flags & RSX_BE_CLEAR_DEPTH) ? D3D12_CLEAR_FLAG_DEPTH : 0) |
                                              ((flags & RSX_BE_CLEAR_STENCIL) ? D3D12_CLEAR_FLAG_STENCIL : 0));
    if (o && f && e12_begin() == 0) {
        e12_transition(o, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        s_e12.cl->lpVtbl->ClearDepthStencilView(s_e12.cl, e12_cpu(s_e12.dsv_heap, s_e12.dsv_inc, o->dsv - 1), f,
            depth_value, stencil, 0, NULL);
    }
    LeaveCriticalSection(&s_e12.lock);
}

/* ---- backend: present and readback --------------------------------------------- */
static void e12_print_stats(void)
{
    static long every = -1;
    if (every < 0) { const char* e = getenv("PS3RECOMP_D3D12_ENGINE_STATS_EVERY"); every = e ? atol(e) : 0; }
    if (every <= 0 || s_e12.presents % (u64)every) return;
    u64 any = 0;
    for (int i = 0; i < E12_X_REASONS; i++) any += s_e12.count[i];
    printf("[D3D12 engine] presents=%llu draws=%llu submits=%llu flushes=%llu",
           (unsigned long long)s_e12.presents, (unsigned long long)s_e12.draws,
           (unsigned long long)s_e12.submits, (unsigned long long)s_e12.flushes);
    if (any)
        for (int i = 0; i < E12_X_REASONS; i++)
            if (s_e12.count[i]) printf(" %s=%llu", s_e12_reason[i], (unsigned long long)s_e12.count[i]);
    for (int i = 0; i < E12_W_SITES; i++)
        if (s_e12.waits[i]) printf(" wait_%s=%llu", s_e12_wait_site[i], (unsigned long long)s_e12.waits[i]);
    printf("\n"); fflush(stdout);
}
static void e12_present(void* user, u32 surface)
{
    (void)user;
    EnterCriticalSection(&s_e12.lock);
    s_e12.presents++;
    e12_obj* o = e12_get(surface, E12_COLOR);
    if (!o) e12_count(E12_X_PRESENT_NONE);
    if (o && s_d3d.swap_chain && e12_begin() == 0) {
        const u32 bi = s_d3d.swap_chain->lpVtbl->GetCurrentBackBufferIndex(s_d3d.swap_chain);
        ID3D12Resource* bb = s_d3d.render_targets[bi];
        e12_obj back; memset(&back, 0, sizeof back); back.res = bb; back.state = D3D12_RESOURCE_STATE_PRESENT;
        e12_transition(o, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        e12_transition(&back, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_GPU_DESCRIPTOR_HANDLE t;
        u32 slot = o->srv;
        if (e12_srv_table(&slot, 1, &t) == 0) {
            D3D12_CPU_DESCRIPTOR_HANDLE rtv; s_d3d.rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rtv_heap, &rtv);
            rtv.ptr += (SIZE_T)bi * s_d3d.rtv_descriptor_size;
            ID3D12GraphicsCommandList* cl = s_e12.cl;
            cl->lpVtbl->OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
            cl->lpVtbl->SetGraphicsRootSignature(cl, s_e12.blit_root);
            cl->lpVtbl->SetPipelineState(cl, s_e12.blit_pso);
            cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 0, t);
            D3D12_VIEWPORT v = { 0, 0, (float)s_d3d.width, (float)s_d3d.height, 0, 1 };
            D3D12_RECT r = { 0, 0, (LONG)s_d3d.width, (LONG)s_d3d.height };
            cl->lpVtbl->RSSetViewports(cl, 1, &v); cl->lpVtbl->RSSetScissorRects(cl, 1, &r);
            cl->lpVtbl->IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cl->lpVtbl->DrawInstanced(cl, 3, 1, 0, 0);
        }
        e12_transition(&back, D3D12_RESOURCE_STATE_PRESENT);
        e12_submit_wait(E12_W_PRESENT);
        s_d3d.swap_chain->lpVtbl->Present(s_d3d.swap_chain, 0, 0);
    }
    e12_print_stats();
    LeaveCriticalSection(&s_e12.lock);
}
/* Depth back to the CPU as floats in [0, 1], for the engine's guest-memory
 * store of a depth buffer that a texture reads in a format of its own. A
 * depth-stencil copy must take the whole subresource, so the depth plane
 * (plane 0, 32-bit float) is copied in full and the rectangle cut on the CPU.
 * Waits for the GPU, as the engine's guest store requires. 0 on success. */
static int e12_depth_readback(void* user, u32 depth, u32 x, u32 y, u32 w, u32 h, float* out, u32 out_pitch)
{
    (void)user;
    int rc = -1;
    EnterCriticalSection(&s_e12.lock);
    e12_obj* z = e12_get(depth, E12_DEPTH);
    if (!z || !out || !w || !h || x + w > z->w || y + h > z->h || out_pitch < w * 4u || e12_begin()) {
        e12_count(E12_X_READBACK); LeaveCriticalSection(&s_e12.lock); return -1;
    }
    const u32 pitch = (z->w * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    D3D12_RESOURCE_DESC b; memset(&b, 0, sizeof b);
    b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; b.Width = (u64)pitch * z->h; b.Height = 1; b.DepthOrArraySize = 1;
    b.MipLevels = 1; b.SampleDesc.Count = 1; b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb = e12_committed(D3D12_HEAP_TYPE_READBACK, &b, D3D12_RESOURCE_STATE_COPY_DEST, NULL);
    if (!rb) { e12_count(E12_X_READBACK); LeaveCriticalSection(&s_e12.lock); return -1; }
    e12_transition(z, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION d, src; memset(&d, 0, sizeof d); memset(&src, 0, sizeof src);
    src.pResource = z->res; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;  /* depth plane */
    d.pResource = rb; d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    d.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_TYPELESS; d.PlacedFootprint.Footprint.Width = z->w;
    d.PlacedFootprint.Footprint.Height = z->h; d.PlacedFootprint.Footprint.Depth = 1; d.PlacedFootprint.Footprint.RowPitch = pitch;
    s_e12.cl->lpVtbl->CopyTextureRegion(s_e12.cl, &d, 0, 0, 0, &src, NULL);
    e12_transition(z, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if (e12_submit_wait(E12_W_DEPTH_READBACK) == 0) {
        void* p = NULL; D3D12_RANGE rr = { 0, (SIZE_T)pitch * z->h };
        if (SUCCEEDED(rb->lpVtbl->Map(rb, 0, &rr, &p))) {
            for (u32 r = 0; r < h; r++)
                memcpy((u8*)out + (size_t)r * out_pitch, (u8*)p + (size_t)(y + r) * pitch + (size_t)x * 4u, (size_t)w * 4u);
            D3D12_RANGE none = { 0, 0 }; rb->lpVtbl->Unmap(rb, 0, &none);
            rc = 0;
        } else e12_count(E12_X_READBACK);
    } else e12_count(E12_X_READBACK);
    rb->lpVtbl->Release(rb);
    LeaveCriticalSection(&s_e12.lock);
    return rc;
}
static void e12_readback(void* user, u32 surface, u32 x, u32 y, u32 w, u32 h, void* out, u32 out_pitch)
{
    (void)user;
    EnterCriticalSection(&s_e12.lock);
    e12_obj* o = e12_get(surface, E12_COLOR);
    if (!o || !out || !w || !h || x + w > o->w || y + h > o->h || e12_begin()) { e12_count(E12_X_READBACK); LeaveCriticalSection(&s_e12.lock); return; }
    const u32 tight = w * o->bpp;
    const u32 pitch = (tight + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    D3D12_RESOURCE_DESC b; memset(&b, 0, sizeof b);
    b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; b.Width = (u64)pitch * h; b.Height = 1; b.DepthOrArraySize = 1;
    b.MipLevels = 1; b.SampleDesc.Count = 1; b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb = e12_committed(D3D12_HEAP_TYPE_READBACK, &b, D3D12_RESOURCE_STATE_COPY_DEST, NULL);
    if (!rb) { e12_count(E12_X_READBACK); LeaveCriticalSection(&s_e12.lock); return; }
    e12_transition(o, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION d, s; memset(&d, 0, sizeof d); memset(&s, 0, sizeof s);
    s.pResource = o->res; s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    d.pResource = rb; d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    d.PlacedFootprint.Footprint.Format = o->fmt; d.PlacedFootprint.Footprint.Width = w;
    d.PlacedFootprint.Footprint.Height = h; d.PlacedFootprint.Footprint.Depth = 1; d.PlacedFootprint.Footprint.RowPitch = pitch;
    D3D12_BOX box = { x, y, 0, x + w, y + h, 1 };
    s_e12.cl->lpVtbl->CopyTextureRegion(s_e12.cl, &d, 0, 0, 0, &s, &box);
    if (e12_submit_wait(E12_W_COLOR_READBACK) == 0) {
        void* p = NULL; D3D12_RANGE rr = { 0, (SIZE_T)pitch * h };
        if (SUCCEEDED(rb->lpVtbl->Map(rb, 0, &rr, &p))) {
            for (u32 r = 0; r < h; r++) memcpy((u8*)out + (size_t)r * out_pitch, (u8*)p + (size_t)r * pitch, tight);
            D3D12_RANGE none = { 0, 0 }; rb->lpVtbl->Unmap(rb, 0, &none);
        } else e12_count(E12_X_READBACK);
    }
    rb->lpVtbl->Release(rb);
    LeaveCriticalSection(&s_e12.lock);
}

static const rsx_draw_backend s_d3d12_engine_backend = {
    .user = NULL,
    .init = e12_init, .shutdown = e12_shutdown, .submit_and_wait = e12_submit_and_wait,
    .texture_create = e12_texture_create, .texture_upload = e12_texture_upload, .texture_release = e12_texture_release,
    .color_target_create = e12_color_target_create, .color_target_release = e12_color_target_release,
    .color_target_write = e12_color_target_write, .surface_view = e12_surface_view,
    .depth_target_create = e12_depth_target_create, .depth_target_release = e12_depth_target_release,
    .depth_snapshot = e12_depth_snapshot,
    .pipeline_create = e12_pipeline_create, .pipeline_release = e12_pipeline_release,
    .bind_targets = e12_bind_targets, .bind_pipeline = e12_bind_pipeline,
    .bind_vs_constants = e12_bind_vs_constants, .bind_ps_constants = e12_bind_ps_constants,
    .bind_textures = e12_bind_textures, .bind_vertex_textures = e12_bind_vertex_textures,
    .set_viewport = e12_set_viewport, .set_scissor = e12_set_scissor, .set_stencil_ref = e12_set_stencil_ref,
    .set_depth_bounds = e12_set_depth_bounds,
    .draw = e12_draw, .clear_color = e12_clear_color, .clear_depth_stencil = e12_clear_depth_stencil,
    .present = e12_present, .readback = e12_readback, .depth_readback = e12_depth_readback,
    .color_snapshot = e12_color_snapshot,
};

/* Called from rsx_d3d12_backend_init once the device, queue and swapchain
 * exist. Returns 1 when the engine is the active path. */
static int e12_activate(u32 width, u32 height)
{
    const char* e = getenv("PS3RECOMP_D3D12_DRAW_ENGINE");
    if (!e || strcmp(e, "1") != 0) return 0;
    InitializeCriticalSection(&s_e12.lock);
    rsx_draw_engine_set_backend(&s_d3d12_engine_backend);
    rsx_draw_engine_set_default(1);
    if (rsx_draw_engine_enabled() && rsx_draw_engine_init(width, height) == 0) {
        s_e12.active = 1;
        printf("[D3D12] register-file draw engine path (PS3RECOMP_D3D12_DRAW_ENGINE=1)\n");
        return 1;
    }
    rsx_draw_engine_set_backend(NULL);
    printf("[D3D12] register-file draw engine unavailable; vtable path\n");
    return 0;
}
