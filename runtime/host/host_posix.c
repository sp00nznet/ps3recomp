/*
 * ps3recomp - minimal POSIX host
 *
 * Drives the cellGcm -> RSX -> backend bridge with no lifted game: it builds a
 * real big-endian NV4097 command buffer by hand, hands it to the FIFO, and runs
 * the flip loop. That exercises the whole graphics path a real title uses to
 * clear and flip, which is exactly the state the D3D12 backend reaches today.
 *
 * Its value is as a bring-up and regression harness for backends. A backend
 * that renders the right colour here has its clear, surface and present paths
 * correct, and can be developed with no game binary and no recompiler run.
 *
 * Exit status: 0 on success, non-zero if the guest command stream did not reach
 * the backend intact -- so it works as a CI check.
 */
#include "cellGcmSys.h"
#include "../memory/vm.h"           /* VM_HLE_INJECT_BASE */
#include "rsx_commands.h"

/* Backend selection. Metal where there is one, otherwise the null backend's
 * headless software path -- which is what lets this harness run on Linux,
 * where no real backend exists yet. Both expose the same four entry points
 * plus the same two test hooks, so the body below is backend-agnostic. */
#if defined(__APPLE__)
#  include "rsx_metal_backend.h"
#  define HOST_BACKEND_NAME     "Metal"
#  define host_backend_init     rsx_metal_backend_init
#  define host_backend_shutdown rsx_metal_backend_shutdown
#  define host_backend_pump     rsx_metal_backend_pump_messages
#  define host_backend_present  rsx_metal_backend_present
#  define host_backend_color    rsx_metal_backend_debug_color
#  define host_backend_center   rsx_metal_backend_readback_center
#else
#  include "rsx_null_backend.h"
#  define HOST_BACKEND_NAME     "null (headless software)"
#  define host_backend_init     rsx_null_backend_init
#  define host_backend_shutdown rsx_null_backend_shutdown
#  define host_backend_pump     rsx_null_backend_pump_messages
#  define host_backend_present  rsx_null_backend_present
#  define host_backend_color    rsx_null_backend_debug_color
#  define host_backend_center   rsx_null_backend_readback_center
#endif

#include <ps3emu/guest_call.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The guest address space this host hands to the HLE layer. */
uint8_t* vm_base = NULL;

/* Must reach VM_HLE_INJECT_BASE: cellGcmSetupContext plants the GCM label
 * window, the control register and the IO<->EA offset tables there, so a VM
 * that stops short of it segfaults the moment the context is built. calloc is
 * lazy, so the extra range costs address space rather than memory. */
#define VM_SIZE     (VM_HLE_INJECT_BASE + 0x10000u)
#define IO_ADDR     0x00100000u          /* RSX IO window base                */
#define IO_SIZE     0x00200000u
#define CMD_SIZE    0x00010000u
/* DMA control: put @+0, get @+4. Derived, never spelled out -- a hardcoded
 * copy of this address is exactly what broke the FIFO callback when the block
 * moved (see the sentinel in ppu_sysprx.cpp). */
#define CTRL_ADDR   (VM_HLE_INJECT_BASE + 0x2000u)
#define HEAP_BASE   0x00400000u

#define CLEAR_ARGB  0xFF101830u          /* what we expect to come out again  */
#define VTX_OFFSET  0x00010000u          /* RSX offset of our vertex buffer   */
#define TEX_OFFSET  0x00020000u          /* RSX offset of our test texture    */
#define TEX_W       4u
#define TEX_H       4u
#define TEX_ARGB    0xFF20C040u          /* what a textured pixel must read   */

/* Functions the RSX side exports but does not declare in a public header. */
extern void cellGcm_rsx_process_fifo(void);
extern int  cellGcm_take_flip_pending_synced(void);

/* --- guest memory helpers (all guest writes are big-endian) --------------- */

static void guest_w32(uint32_t addr, uint32_t v)
{
    uint8_t* p = vm_base + addr;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8); p[3] = (uint8_t)v;
}

static uint32_t g_heap = HEAP_BASE;

static u32 host_alloc(u32 size, u32 align)
{
    if (align == 0) align = 4;
    g_heap = (g_heap + align - 1u) & ~(align - 1u);
    u32 r = g_heap;
    g_heap += size;
    return r;
}

static void host_w32(u32 addr, u32 v) { guest_w32(addr, v); }

/* --- NV4097 command buffer ------------------------------------------------ */

static uint32_t g_fifo_len = 0;

/* One non-incrementing method write: [count<<18 | method] followed by data. */
static void emit(uint32_t method, uint32_t data)
{
    guest_w32(IO_ADDR + g_fifo_len, (1u << 18) | (((method >> 2) & 0x7FFu) << 2));
    g_fifo_len += 4;
    guest_w32(IO_ADDR + g_fifo_len, data);
    g_fifo_len += 4;
}

/* Write a big-endian float into the guest's IO window. */
static void guest_f32(uint32_t addr, float f)
{
    uint32_t w; memcpy(&w, &f, 4); guest_w32(addr, w);
}

/* A full-viewport triangle in clip space, vertex-coloured. With no vertex
 * program loaded the backend falls back to an identity transform, so these
 * coordinates land directly in NDC.
 *
 * Layout per vertex: float4 position at +0, float4 colour at +16, stride 32.
 * Position is attribute 0 and diffuse colour attribute 3, matching the slots
 * the backend's fallback path reads. */
static void upload_triangle(void)
{
    static const float v[3][8] = {
        { -1.0f, -1.0f, 0.0f, 1.0f,   1.0f, 0.0f, 0.0f, 1.0f },
        {  3.0f, -1.0f, 0.0f, 1.0f,   1.0f, 0.0f, 0.0f, 1.0f },
        { -1.0f,  3.0f, 0.0f, 1.0f,   1.0f, 0.0f, 0.0f, 1.0f },
    };
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 8; k++)
            guest_f32(IO_ADDR + VTX_OFFSET + (uint32_t)(i * 32 + k * 4), v[i][k]);
}

/* A 4x4 texture, every texel the same colour, written as the guest would:
 * A8R8G8B8 means the bytes land A,R,G,B in that order on a big-endian PPU.
 *
 * Uniform on purpose: this asserts the pixel VALUE survives the whole path --
 * bind, layout, decode, channel reorder, sample -- and a single centre-pixel
 * readback is what the harness can observe.
 *
 * It therefore does NOT detect swizzling, and cannot: reordering identical
 * texels changes nothing. Nor can a non-uniform texture fix that here. The
 * centre lands somewhere in the four texels around (2,2) of a 4x4, and Morton
 * order maps those to offsets {3,6,9,12} against linear's {5,6,9,10} -- the
 * two agree at 6 and 9, so a centre sample can land on a texel where swizzled
 * and linear read the same byte. Swizzling is covered instead by
 * libs/video/tests/test_texture_layout.c, which asserts Morton addressing is a
 * bijection and that a swizzled read differs from a linear one over the same
 * bytes. Verified: breaking the swizzle decision fails those and not this. */
static void upload_texture(void)
{
    for (uint32_t i = 0; i < TEX_W * TEX_H; i++)
        guest_w32(IO_ADDR + TEX_OFFSET + i * 4u, TEX_ARGB);
}

/* The textured draw uses its own vertex block: two triangles covering the
 * screen, with texcoord0 (attribute 8) alongside position and colour.
 * Layout per vertex: float4 pos, float4 colour, float4 uv -- stride 48. */
#define TVTX_OFFSET (VTX_OFFSET + 0x1000u)
static void upload_textured_quad(void)
{
    static const float v[6][12] = {
        { -1,-1,0,1,  0,1,0,1,  0,0,0,0 },
        {  1,-1,0,1,  0,1,0,1,  1,0,0,0 },
        {  1, 1,0,1,  0,1,0,1,  1,1,0,0 },
        { -1,-1,0,1,  0,1,0,1,  0,0,0,0 },
        {  1, 1,0,1,  0,1,0,1,  1,1,0,0 },
        { -1, 1,0,1,  0,1,0,1,  0,1,0,0 },
    };
    for (int i = 0; i < 6; i++)
        for (int k = 0; k < 12; k++)
            guest_f32(IO_ADDR + TVTX_OFFSET + (uint32_t)(i * 48 + k * 4), v[i][k]);
}

/* type[3:0]=2 (float32), size[7:4], stride[15:8] */
#define VFMT(size, stride) (2u | ((u32)(size) << 4) | ((u32)(stride) << 8))

/* Bit 31 of a vertex-array offset selects the context DMA: 0 = LOCAL (VRAM),
 * 1 = MAIN (IO-mapped system memory). This harness writes its vertices into
 * the IO window, so they are MAIN and must say so. Leaving the bit clear used
 * to work by accident: one backend resolved LOCAL through the IO offset table,
 * which happens to be right for these vertices and wrong for a real title's. */
#define VTX_MAIN(off)  (0x80000000u | (u32)(off))

static void emit_triangle_draw(void)
{
    emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 0 * 4, VTX_MAIN(VTX_OFFSET +  0));  /* position */
    emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 0 * 4, VFMT(4, 32));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 3 * 4, VTX_MAIN(VTX_OFFSET + 16));  /* colour   */
    emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 3 * 4, VFMT(4, 32));

    emit(NV4097_SET_BEGIN_END, 5u);                       /* TRIANGLES        */
    emit(NV4097_DRAW_ARRAYS,   0u | ((3u - 1u) << 24));   /* first=0 count=3  */
    emit(NV4097_SET_BEGIN_END, 0u);                       /* end              */
}

/* Bind the test texture on unit 0 and draw the quad. The vertex colour is
 * deliberately GREEN while the texture is a different colour, so a pass that
 * ignored the texture would still produce a plausible-looking frame -- and
 * the assertion would catch it. */
static void emit_textured_draw(void)
{
    /* NV4097_SET_TEXTURE_* are 0x20 apart per unit; unit 0 is the base. */
    emit(NV4097_SET_TEXTURE_OFFSET     + 0, VTX_MAIN(TEX_OFFSET));
    /* format: A8R8G8B8 (0x85) | LN (0x20) -- linear, not swizzled. */
    emit(NV4097_SET_TEXTURE_FORMAT     + 0, 0x85u | 0x20u);
    emit(NV4097_SET_TEXTURE_CONTROL0   + 0, 0x80000000u);   /* unit enable */
    emit(NV4097_SET_TEXTURE_CONTROL1   + 0, 0xAAE4u);       /* identity crossbar */
    emit(NV4097_SET_TEXTURE_IMAGE_RECT + 0, (TEX_W << 16) | TEX_H);

    emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 0 * 4, VTX_MAIN(TVTX_OFFSET +  0));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 0 * 4, VFMT(4, 48));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 3 * 4, VTX_MAIN(TVTX_OFFSET + 16));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 3 * 4, VFMT(4, 48));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 8 * 4, VTX_MAIN(TVTX_OFFSET + 32));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 8 * 4, VFMT(4, 48));

    emit(NV4097_SET_BEGIN_END, 5u);                       /* TRIANGLES       */
    emit(NV4097_DRAW_ARRAYS,   0u | ((6u - 1u) << 24));   /* first=0 count=6 */
    emit(NV4097_SET_BEGIN_END, 0u);
}

/* Append this frame's commands to the ring and advance `put`, exactly as a
 * title does. The RSX's `get` pointer lives inside cellGcmSys and chases `put`;
 * it is never rewound, so each frame must occupy fresh ring space rather than
 * overwrite the last one. */
static void submit_frame(int with_draw)
{
    emit(NV4097_SET_COLOR_CLEAR_VALUE, CLEAR_ARGB);
    emit(NV4097_CLEAR_SURFACE,         0xF0u);
    if (with_draw == 2)      emit_textured_draw();
    else if (with_draw == 1) emit_triangle_draw();

    guest_w32(CTRL_ADDR + 0, g_fifo_len);   /* put */
    cellGcm_rsx_process_fifo();
}

int main(int argc, char** argv)
{
    /* frames = 0 runs until the window is closed, which is what the .app
     * bundle uses; a fixed count keeps the CI runs bounded. */
    int frames = 3, do_draw = 0;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--frames=", 9) == 0) frames = atoi(argv[i] + 9);
        else if (strcmp(argv[i], "--draw") == 0)   do_draw = 1;
        /* --tex: bind a texture and sample it, which exercises the shared
         * rsx_texture_layout/decode path end to end rather than in a unit
         * test. The vertex colour differs from the texture colour, so a
         * backend that ignored the texture fails the assertion below. */
        else if (strcmp(argv[i], "--tex") == 0)    do_draw = 2;
    }

    vm_base = (uint8_t*)calloc(1, VM_SIZE);
    if (!vm_base) { fprintf(stderr, "[host] guest VM alloc failed\n"); return 1; }

    printf("[host] backend: %s\n", HOST_BACKEND_NAME);
    if (host_backend_init(1280, 720, "ps3recomp") != 0) {
        fprintf(stderr, "[host] backend init failed\n");
        free(vm_base);
        return 1;
    }

    /* Bring up the GCM context exactly as a guest's cellGcmInit would. */
    u32 ctx_out = host_alloc(4, 4);
    u32 cdata   = cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR,
                                      host_alloc, host_w32);
    printf("[host] gcm context data @ 0x%08X\n", cdata);
    cellGcmSetDisplayBuffer(0, 0, 1280 * 4, 1280, 720);

    guest_w32(CTRL_ADDR + 4, 0);            /* get: start of ring */
    if (do_draw == 1) upload_triangle();
    if (do_draw == 2) { upload_texture(); upload_textured_quad(); }
    submit_frame(do_draw);

    u32 got = host_backend_color();
    printf("[host] clear colour through the FIFO: 0x%08X (expected 0x%08X) %s\n",
           got, CLEAR_ARGB, got == CLEAR_ARGB ? "OK" : "MISMATCH");

    /* Flip loop, as cellGcmSetFlipCommand + the vblank ticker drive it. */
    cellGcmSetFlipCommand(0);
    int presented = 0;
    for (int i = 0; frames == 0 || i < frames; i++) {
        if (host_backend_pump() < 0) { printf("[host] window closed\n"); break; }
        /* Re-submit every frame, as a title does: the backend records draws
         * per frame and clears the record when it presents. */
        if (i > 0) submit_frame(do_draw);
        cellGcmTickVBlank();
        cellGcmTickFlip();
        if (cellGcm_take_flip_pending_synced()) presented++;
        host_backend_present();
    }
    printf("[host] presented %d frame(s)\n", presented);

    u32 back = host_backend_center();
    int rc = (got == CLEAR_ARGB) ? 0 : 2;
    if (back) {
        /* Headless: the drawable is ours, so verify what actually landed.
         * RSX ARGB -> Metal BGRA8Unorm keeps R, G and B in the same bytes. */
        /* Without a draw the centre pixel is the clear colour; with the test
         * triangle it is the triangle's red, which proves the vertex fetch,
         * primitive path, pipeline state and draw all worked. With --tex it is
         * the TEXTURE's colour, not the quad's green -- so it also proves the
         * texture reached the sampler through layout, decode and the crossbar.
         */
        u32 want = (do_draw == 2) ? TEX_ARGB
                 : (do_draw == 1) ? 0xFFFF0000u
                                  : CLEAR_ARGB;
        printf("[host] presented pixel: 0x%08X (expected 0x%08X) %s\n",
               back, want, (back & 0x00FFFFFFu) == (want & 0x00FFFFFFu) ? "OK" : "MISMATCH");
        if ((back & 0x00FFFFFFu) != (want & 0x00FFFFFFu)) rc = 3;
    }

    host_backend_shutdown();
    free(vm_base);
    return rc;
}
