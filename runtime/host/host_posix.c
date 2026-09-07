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
/* --threads reads the host stack's size back through pthread_getattr_np,
 * which glibc declares only under _GNU_SOURCE, and that has to be set before
 * the first system header of the translation unit. Darwin has its own
 * pthread_get_stacksize_np and needs nothing. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif
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
#  define HOST_BACKEND_GUEST_SHADERS 1
#  define host_backend_init     rsx_metal_backend_init
#  define host_backend_shutdown rsx_metal_backend_shutdown
#  define host_backend_pump     rsx_metal_backend_pump_messages
#  define host_backend_present  rsx_metal_backend_present
#  define host_backend_color    rsx_metal_backend_debug_color
#  define host_backend_center   rsx_metal_backend_readback_center
#  define host_backend_guest_draws rsx_metal_backend_guest_draws
#else
#  include "rsx_null_backend.h"
#  define HOST_BACKEND_NAME     "null (headless software)"
#  define HOST_BACKEND_GUEST_SHADERS 0
#  define host_backend_init     rsx_null_backend_init
#  define host_backend_shutdown rsx_null_backend_shutdown
#  define host_backend_pump     rsx_null_backend_pump_messages
#  define host_backend_present  rsx_null_backend_present
#  define host_backend_color    rsx_null_backend_debug_color
#  define host_backend_center   rsx_null_backend_readback_center
#  define host_backend_guest_draws rsx_null_backend_guest_draws
#endif
#include "rsx_test_programs.h"

#include "../syscalls/sys_ppu_thread.h"   /* --threads: the lv2 thread path */
#include "../../libs/audio/cellAudio.h"   /* --audio-pad: the SDL2 backends */
#include "../../libs/input/cellPad.h"

#include <ps3emu/guest_call.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The guest address space this host hands to the HLE layer. */
uint8_t* vm_base = NULL;
/* ...and how much of it is really there. runtime/memory/vm.h treats 0 as "the
 * whole 32-bit space is backed, no check needed", which is true of a runner
 * that reserves 4 GB and false of this harness: it callocs a few hundred MB,
 * so an RSX offset that resolves into VRAM (localAddress is 0xC0000000) is off
 * the end of the allocation. A title's registers make those resolves happen
 * whether or not the harness ever wrote there. */
extern uint32_t ppu_vm_size;

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
#define FP_OFFSET   0x00030000u          /* RSX offset of our fragment programs */
#define TFP_OFFSET  (FP_OFFSET + 0x1000u) /* ... the texturing one            */
#define MFP_OFFSET  (FP_OFFSET + 0x2000u) /* ... and the two-target one       */
#define TEX_W       4u
#define TEX_H       4u
#define TEX_ARGB    0xFF20C040u          /* what a textured pixel must read   */
#define MIP_OFFSET  0x00040000u          /* RSX offset of the mipped texture  */
#define MIP_W       64u
#define MIP_H       64u
#define MIP_L0_ARGB 0xFF3060A0u          /* level 0: must NOT be sampled      */
#define MIP_L1_ARGB 0xFFC0F080u          /* level 1: what --mip must read     */
/* --rtt: an RSX offset in the IO window that cellGcmSetDisplayBuffer never
 * registered, so a colour surface there is offscreen, and the size the guest
 * clips it to. The backing colour is what guest memory at that offset holds,
 * and it is deliberately not the colour the surface gets rendered. */
#define RTT_OFFSET  0x00080000u
#define RTT_DIM     256u
#define RTT_BACKING 0xFF00FF00u          /* green, like the quad's vertices   */
/* --depthtex: an RSX offset for a depth buffer of the guest's own, the size
 * the texture unit declares when it comes back to sample it, the NDC z the
 * first pass writes there, and the pixel that depth has to produce.
 *
 * 0.25 is chosen so the sampled depth lands on an exact byte: 0.25 * 255 is
 * 63.75, which is nearer 64 than 63 and so cannot round either way. */
#define DTEX_OFFSET  0x00100000u
#define DTEX_DIM     256u
#define DTEX_Z       0.25f
#define DTEX_ARGB    0xFF400000u
/* What the guest bytes behind that offset hold: Z24S8 for depth 1.0 and
 * stencil 0, which is what a cleared depth buffer really contains. A
 * renderer that uploads those bytes instead of sampling the live zeta reads
 * 1.0 and presents saturated red, and so does one that samples a zeta
 * nothing wrote -- one recognisable wrong answer rather than two. */
#define DTEX_BACKING 0xFFFFFF00u
/* --mrt: two offscreen colour surfaces for an MRT1 set (A and B), the size
 * the guest clips them to, and the colour each one is drawn. One fragment
 * program writes both -- r0 is target A and r2 is target B under 32-bit
 * exports -- so the two surfaces come out different colours from a single
 * draw, and the display pass samples one of them.
 *
 * Each of the three ways to get this wrong has its own colour. A renderer
 * that uploads the guest bytes behind a surface instead of binding the one it
 * rendered into presents the backing green, as in --rtt; one that attaches
 * and clears the set but does not feed its second target presents the set's
 * clear colour; and one that sampled the wrong member presents the other
 * target's. */
#define MRT_A_OFFSET 0x00140000u
#define MRT_B_OFFSET 0x00180000u
#define MRT_DIM      256u
#define MRT_BACKING  0xFF00FF00u         /* green: neither target's colour  */
#define MRT_A_ARGB   0xFFFF0000u         /* r0 <- COL0       (red)          */
#define MRT_B_ARGB   0xFF0000FFu         /* r2 <- COL0.zyxw  (blue)         */
/* The set's own clear colour, distinct from the frame's and from all three
 * above. It is what an attached but unwritten target holds, and it has to be
 * something rather than nothing: an unwritten surface on the vtable path
 * reads as transparent black, and a zero pixel is what the harness cannot
 * tell from a backend with no headless readback at all -- so it would pass. */
#define MRT_CLEAR    0xFF804020u

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

/* One guest VM, with its size published for the OOB guard above. */
static uint8_t* host_vm_alloc(void)
{
    uint8_t* p = (uint8_t*)calloc(1, VM_SIZE);
    if (p) ppu_vm_size = VM_SIZE;
    return p;
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

/* Guest memory behind the --rtt surface, filled with a colour that is not the
 * one the surface gets rendered: titles CPU-initialise their render-target
 * buffers, so those bytes are really there, and the D3D12 backend seeds a new
 * offscreen target from them for exactly that reason.
 *
 * It is also what gives the check teeth. A backend that misses the
 * render-to-texture match falls back to uploading this region, and the frame
 * comes out green instead of red. Left as zeros the failure would present a
 * transparent black pixel, which is indistinguishable from a backend that has
 * no headless readback at all -- and would pass. */
static void upload_rtt_backing(void)
{
    for (uint32_t i = 0; i < RTT_DIM * RTT_DIM; i++)
        guest_w32(IO_ADDR + RTT_OFFSET + i * 4u, RTT_BACKING);
}

/* ...and the same for --depthtex's zeta: the bytes a CPU-cleared depth buffer
 * would hold at the offset the second pass binds a texture unit to. */
static void upload_depthtex_backing(void)
{
    for (uint32_t i = 0; i < DTEX_DIM * DTEX_DIM; i++)
        guest_w32(IO_ADDR + DTEX_OFFSET + i * 4u, DTEX_BACKING);
}

/* ...and for both members of --mrt's set, so neither surface can pass by
 * holding what the CPU left there. */
static void upload_mrt_backing(void)
{
    for (uint32_t i = 0; i < MRT_DIM * MRT_DIM; i++) {
        guest_w32(IO_ADDR + MRT_A_OFFSET + i * 4u, MRT_BACKING);
        guest_w32(IO_ADDR + MRT_B_OFFSET + i * 4u, MRT_BACKING);
    }
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

/* A 64x64 A8R8G8B8 texture with TWO levels, each a flat colour: level 0 at the
 * bound offset and level 1 immediately behind it, which is where the hardware
 * expects the next level of a linear image.
 *
 * The two colours differ so the presented pixel says which level was sampled.
 * That is the whole point of --mip: a backend that uploads level 0 and stops
 * has a one-level texture, its sampler clamps to that level however wide the
 * LOD range says, and the centre pixel comes out level 0's colour. */
static void upload_mip_texture(void)
{
    for (uint32_t i = 0; i < MIP_W * MIP_H; i++)
        guest_w32(IO_ADDR + MIP_OFFSET + i * 4u, MIP_L0_ARGB);
    const uint32_t l1 = MIP_OFFSET + MIP_W * MIP_H * 4u;
    for (uint32_t i = 0; i < (MIP_W / 2u) * (MIP_H / 2u); i++)
        guest_w32(IO_ADDR + l1 + i * 4u, MIP_L1_ARGB);
}

/* The mip draw's quad: a few pixels around the centre of the 1280x720 target,
 * with texcoord0 spanning the whole texture. Squeezing 64 texels into ~26x14
 * pixels is what drives the LOD past level 0; the sampler's max LOD then pins
 * it at level 1. Layout as the textured quad: float4 pos, colour, uv. */
#define MVTX_OFFSET (VTX_OFFSET + 0x3000u)
static void upload_mip_quad(void)
{
    static const float v[6][12] = {
        { -0.02f,-0.02f,0,1,  0,1,0,1,  0,0,0,0 },
        {  0.02f,-0.02f,0,1,  0,1,0,1,  1,0,0,0 },
        {  0.02f, 0.02f,0,1,  0,1,0,1,  1,1,0,0 },
        { -0.02f,-0.02f,0,1,  0,1,0,1,  0,0,0,0 },
        {  0.02f, 0.02f,0,1,  0,1,0,1,  1,1,0,0 },
        { -0.02f, 0.02f,0,1,  0,1,0,1,  0,1,0,0 },
    };
    for (int i = 0; i < 6; i++)
        for (int k = 0; k < 12; k++)
            guest_f32(IO_ADDR + MVTX_OFFSET + (uint32_t)(i * 48 + k * 4), v[i][k]);
}

/* The guest-shader draw's vertex block: the same full-viewport triangle, but
 * BLUE. Its vertex program passes position and colour through unchanged and
 * its fragment program swaps red and blue, so the pixel that must come out is
 * red -- a draw that quietly fell back to the built-in shader would produce
 * blue and fail. Layout as upload_triangle: float4 pos, float4 colour. */
#define SVTX_OFFSET (VTX_OFFSET + 0x2000u)
static void upload_shader_triangle(void)
{
    static const float v[3][8] = {
        { -1.0f, -1.0f, 0.0f, 1.0f,   0.0f, 0.0f, 1.0f, 1.0f },
        {  3.0f, -1.0f, 0.0f, 1.0f,   0.0f, 0.0f, 1.0f, 1.0f },
        { -1.0f,  3.0f, 0.0f, 1.0f,   0.0f, 0.0f, 1.0f, 1.0f },
    };
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 8; k++)
            guest_f32(IO_ADDR + SVTX_OFFSET + (uint32_t)(i * 32 + k * 4), v[i][k]);
}

/* Two full-viewport triangles the depth test has to order, drawn NEAR LAST:
 * red at NDC z = 0.25 first, then green at z = 0.75. With SET_DEPTH_FUNC LESS
 * and a depth buffer cleared to 1.0 the red one wins the centre pixel, and a
 * backend with no working depth test draws the green one over it -- which is
 * the whole point of drawing them in that order.
 *
 * The vertex block is laid out as upload_triangle's: float4 pos, float4
 * colour, stride 32. No vertex program, so these coordinates land in NDC
 * through the backend's identity fallback and z arrives at the depth test
 * unchanged. */
#define ZVTX_OFFSET (VTX_OFFSET + 0x3000u)
static void upload_depth_triangles(void)
{
    static const float v[6][8] = {
        { -1.0f, -1.0f, 0.25f, 1.0f,   1.0f, 0.0f, 0.0f, 1.0f },   /* near, red   */
        {  3.0f, -1.0f, 0.25f, 1.0f,   1.0f, 0.0f, 0.0f, 1.0f },
        { -1.0f,  3.0f, 0.25f, 1.0f,   1.0f, 0.0f, 0.0f, 1.0f },
        { -1.0f, -1.0f, 0.75f, 1.0f,   0.0f, 1.0f, 0.0f, 1.0f },   /* far, green  */
        {  3.0f, -1.0f, 0.75f, 1.0f,   0.0f, 1.0f, 0.0f, 1.0f },
        { -1.0f,  3.0f, 0.75f, 1.0f,   0.0f, 1.0f, 0.0f, 1.0f },
    };
    for (int i = 0; i < 6; i++)
        for (int k = 0; k < 8; k++)
            guest_f32(IO_ADDR + ZVTX_OFFSET + (uint32_t)(i * 32 + k * 4), v[i][k]);
}

/* --depthtex's first pass: one full-viewport triangle at DTEX_Z, drawn with
 * the depth test and depth writes on, so the zeta it is aimed at ends the
 * pass holding that one value everywhere.
 *
 * Flat on purpose. What the mode asserts is the depth VALUE that comes back
 * through a sampler, and a centre-pixel readback is all the harness can
 * observe; a depth that varied across the surface would make the assertion
 * depend on where in the image the resolve landed rather than on what it
 * read. Layout as upload_triangle's: float4 position, float4 colour. */
#define DVTX_OFFSET (VTX_OFFSET + 0x5000u)
static void upload_depthtex_triangle(void)
{
    static const float v[3][8] = {
        { -1.0f, -1.0f, DTEX_Z, 1.0f,   1.0f, 1.0f, 1.0f, 1.0f },
        {  3.0f, -1.0f, DTEX_Z, 1.0f,   1.0f, 1.0f, 1.0f, 1.0f },
        { -1.0f,  3.0f, DTEX_Z, 1.0f,   1.0f, 1.0f, 1.0f, 1.0f },
    };
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 8; k++)
            guest_f32(IO_ADDR + DVTX_OFFSET + (uint32_t)(i * 32 + k * 4), v[i][k]);
}

/* --quads: the two primitives that have no host equivalent and are not simply
 * a list of quads -- a POLYGON and a QUAD_STRIP -- one drawn over the other.
 *
 * The polygon is a convex pentagon large enough to contain the whole viewport
 * and it is GREEN; the quad strip covers the viewport in RED and is drawn
 * after it, so the centre pixel is red only if the strip was expanded and
 * rasterised. Its seam sits left of centre on purpose: the centre belongs to
 * the SECOND quad, so an expansion that emitted the first one and stopped
 * leaves the pentagon's green there rather than passing by accident.
 *
 * The pixel proves the strip. The polygon is what a renderer that drops it
 * shows nothing of, and its own expansion -- vertex by vertex, restart cuts
 * included -- is asserted in libs/video/tests/test_rsx_draw_engine.c.
 *
 * Layout as upload_triangle's: float4 position, float4 colour, stride 32, and
 * no vertex program, so the coordinates land in NDC unchanged. */
#define QVTX_OFFSET  (VTX_OFFSET + 0x4000u)
#define QSTRIP_OFFSET (QVTX_OFFSET + 0x100u)
static void upload_quad_geometry(void)
{
    static const float poly[5][8] = {
        {  0.0f,  3.0f, 0.0f, 1.0f,   0.0f, 1.0f, 0.0f, 1.0f },
        { -3.0f,  1.0f, 0.0f, 1.0f,   0.0f, 1.0f, 0.0f, 1.0f },
        { -2.0f, -3.0f, 0.0f, 1.0f,   0.0f, 1.0f, 0.0f, 1.0f },
        {  2.0f, -3.0f, 0.0f, 1.0f,   0.0f, 1.0f, 0.0f, 1.0f },
        {  3.0f,  1.0f, 0.0f, 1.0f,   0.0f, 1.0f, 0.0f, 1.0f },
    };
    /* Three pairs of vertices, so two quads: x in [-1,-0.5] and [-0.5,1]. */
    static const float strip[6][8] = {
        { -1.0f, -1.0f, 0.0f, 1.0f,   1.0f, 0.0f, 0.0f, 1.0f },
        { -1.0f,  1.0f, 0.0f, 1.0f,   1.0f, 0.0f, 0.0f, 1.0f },
        { -0.5f, -1.0f, 0.0f, 1.0f,   1.0f, 0.0f, 0.0f, 1.0f },
        { -0.5f,  1.0f, 0.0f, 1.0f,   1.0f, 0.0f, 0.0f, 1.0f },
        {  1.0f, -1.0f, 0.0f, 1.0f,   1.0f, 0.0f, 0.0f, 1.0f },
        {  1.0f,  1.0f, 0.0f, 1.0f,   1.0f, 0.0f, 0.0f, 1.0f },
    };
    for (int i = 0; i < 5; i++)
        for (int k = 0; k < 8; k++)
            guest_f32(IO_ADDR + QVTX_OFFSET + (uint32_t)(i * 32 + k * 4), poly[i][k]);
    for (int i = 0; i < 6; i++)
        for (int k = 0; k < 8; k++)
            guest_f32(IO_ADDR + QSTRIP_OFFSET + (uint32_t)(i * 32 + k * 4), strip[i][k]);
}

/* The fragment programs live in guest memory, where SET_SHADER_PROGRAM points
 * the RSX at them, stored the way rsx_fp_read_word expects (big-endian,
 * half-words swapped):
 *   FP_OFFSET   MOV r0, COL0.zyxw ; END    (--shader: swaps red and blue)
 *   TFP_OFFSET  TEX r0, TC0 ; END          (--tex: samples unit 0 at texcoord0)
 *   MFP_OFFSET  MOV r0, COL0 ; MOV r2, COL0.zyxw ; END   (--mrt: two targets)
 * The unit test test_shader_msl decompiles exactly these words and checks the
 * HLSL reads (input.col0).zyxw, rsx_tex[0].Sample(rsx_samp[0], ...), and that
 * the two-target one comes out as SV_TARGET0 and SV_TARGET1. */
static void upload_fragment_programs(void)
{
    rsx_test_fp_mov_r0_col0(vm_base + IO_ADDR + FP_OFFSET, RSX_TEST_FP_SWZ(2, 1, 0, 3));
    rsx_test_fp_tex_r0_tc0(vm_base + IO_ADDR + TFP_OFFSET, 0);
    rsx_test_fp_mrt_col0(vm_base + IO_ADDR + MFP_OFFSET,
                         RSX_TEST_FP_SWZ_IDENT, RSX_TEST_FP_SWZ(2, 1, 0, 3));
}

/* Load a vertex program and select a fragment program the way a title does.
 *
 * The vertex program goes into the RSX's instruction store as data words:
 * SET_TRANSFORM_PROGRAM_LOAD sets the write cursor (in instructions), and
 * each write to SET_TRANSFORM_PROGRAM + 4k stores one word and advances.
 * SET_TRANSFORM_PROGRAM_START then says which instruction the program begins
 * at. The fragment program is addressed by SET_SHADER_PROGRAM, whose low two
 * bits are the location: 1 = local (VRAM), 2 = main memory -- our bytes sit
 * in the IO window, so main. SET_SHADER_CONTROL's 0x40 says the program's
 * colour comes out of r0 (32-bit exports), which is the register both of
 * ours write.
 *
 * Both programs are re-sent every frame, as a title's command list would;
 * the backend's caches are what keep that from recompiling anything. */
static void emit_programs(const u8* vp, u32 vp_bytes, u32 fp_offset)
{
    emit(NV4097_SET_TRANSFORM_PROGRAM_LOAD, 0u);
    for (u32 k = 0; k < vp_bytes / 4u; k++) {
        const u8* w = vp + k * 4;
        emit(NV4097_SET_TRANSFORM_PROGRAM + k * 4,
             (u32)w[0] | ((u32)w[1] << 8) | ((u32)w[2] << 16) | ((u32)w[3] << 24));
    }
    emit(NV4097_SET_TRANSFORM_PROGRAM_START, 0u);
    emit(NV4097_SET_SHADER_PROGRAM, fp_offset | 2u);
    emit(NV4097_SET_SHADER_CONTROL, CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS);
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

/* Bind a texture on unit 0 and draw the quad through a guest vertex program
 * that passes position, colour and texcoord0 through (MOV o0,v0 ; MOV o1,v3 ;
 * MOV o7,v8) and the TEX r0, TC0 fragment program, which is how a title
 * samples a texture. A backend with no guest-program path ignores the
 * programs and samples in its own way. The vertex colour is deliberately
 * GREEN while the texture is a different colour, so a pass that ignored the
 * texture would still produce a plausible-looking frame -- and the assertion
 * would catch it.
 *
 * Parameterised on where the texture is and how big it is, because --rtt
 * points the same draw at a render target instead of an uploaded image. */
static void emit_textured_draw_fmt(u32 tex_offset, u32 w, u32 h, u32 fmt_byte)
{
    u8 vp[48];
    rsx_test_vp_mov_out(vp +  0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);   /* MOV o0, v0      */
    rsx_test_vp_mov_out(vp + 16, 3, RSX_TEST_VP_SWZ_IDENT, 1, 0);   /* MOV o1, v3      */
    rsx_test_vp_mov_out(vp + 32, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);   /* MOV o7, v8, END */
    emit_programs(vp, sizeof vp, TFP_OFFSET);

    /* NV4097_SET_TEXTURE_* are 0x20 apart per unit; unit 0 is the base. */
    emit(NV4097_SET_TEXTURE_OFFSET     + 0, tex_offset);
    /* SET_TEXTURE_FORMAT as cellGcmSetTexture packs it: location in [1:0]
     * (2 = main memory, where the IO window is), 2D in [7:4], the format
     * byte in [15:8] -- A8R8G8B8 (0x85) with LN (0x20), linear rather than
     * swizzled, or DEPTH24_D8 (0x90) for --depthtex -- and one mip level in
     * [19:16]. */
    emit(NV4097_SET_TEXTURE_FORMAT     + 0, 2u | (2u << 4) | (fmt_byte << 8) | (1u << 16));
    emit(NV4097_SET_TEXTURE_CONTROL0   + 0, 0x80000000u);   /* unit enable */
    emit(NV4097_SET_TEXTURE_CONTROL1   + 0, 0xAAE4u);       /* identity crossbar */
    emit(NV4097_SET_TEXTURE_IMAGE_RECT + 0, (w << 16) | h);

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

/* A8R8G8B8 with the LN bit, which is every mode but --depthtex. */
static void emit_textured_draw_at(u32 tex_offset, u32 w, u32 h)
{
    emit_textured_draw_fmt(tex_offset, w, h, 0x85u | 0x20u);
}

static void emit_textured_draw(void)
{
    emit_textured_draw_at(TEX_OFFSET, TEX_W, TEX_H);
}

/* Render into a surface of the guest's own, then sample it -- the two halves
 * of render-to-texture, in the order a title does them.
 *
 * First, colour target A moves to an offset no display buffer was registered
 * at, with a 256x256 clip, and the red full-surface triangle is drawn there.
 * SET_SHADER_PROGRAM 0 leaves the backend no fragment program to run, so that
 * draw takes the fixed-function path -- which is what a title looks like
 * before it has loaded one, and what keeps this draw fixed-function on every
 * frame rather than only the first, since the RSX keeps the previous frame's
 * programs resident.
 *
 * Then target A goes back to display buffer 0 and the full-screen quad is
 * drawn through the passthrough vertex program and TEX r0, TC0, with texture
 * unit 0 bound at the SURFACE's offset. Red exists only in the surface the
 * backend rendered into: the quad's own vertices are green and so are the
 * guest bytes behind that offset, so every way of getting this wrong -- an
 * ignored surface, a guest-memory upload, a dropped texture -- shows green. */
static void emit_rtt_draws(void)
{
    /* Which memory the surface's offset is an offset into. This harness's
     * surface really does live in the IO window, and saying so is what lets a
     * renderer that keys surfaces by (context, offset) match the texture unit
     * bound at the same offset in the same context. Leaving it at the reset
     * value declared the surface in VRAM while the texture said main memory,
     * so the two never met and the quad sampled the guest bytes. */
    emit(NV4097_SET_CONTEXT_DMA_COLOR_A,     CELL_GCM_CONTEXT_DMA_MEMORY_HOST_BUFFER);
    emit(NV4097_SET_SURFACE_COLOR_AOFFSET,   RTT_OFFSET);
    emit(NV4097_SET_SURFACE_COLOR_TARGET,    CELL_GCM_SURFACE_TARGET_0);
    emit(NV4097_SET_SURFACE_CLIP_HORIZONTAL, RTT_DIM << 16);
    emit(NV4097_SET_SURFACE_CLIP_VERTICAL,   RTT_DIM << 16);
    emit(NV4097_SET_SHADER_PROGRAM,          0u);
    emit_triangle_draw();

    emit(NV4097_SET_CONTEXT_DMA_COLOR_A,     CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER);
    emit(NV4097_SET_SURFACE_COLOR_AOFFSET,   0u);
    emit(NV4097_SET_SURFACE_CLIP_HORIZONTAL, 1280u << 16);
    emit(NV4097_SET_SURFACE_CLIP_VERTICAL,   720u << 16);
    emit_textured_draw_at(RTT_OFFSET, RTT_DIM, RTT_DIM);
}

/* Declare an MRT1 set, write both of its members from ONE draw, then sample
 * one of them -- which is what a deferred renderer's G-buffer pass is.
 *
 * Colour targets A and B move to two offsets no display buffer was registered
 * at, both in main memory (each member of a set has its own context DMA, and
 * the draw engine keys surfaces by location as well as offset, so B's has to
 * be said as plainly as A's). SET_SURFACE_COLOR_TARGET then names MRT1, so
 * the pass has two attachments.
 *
 * The draw runs the passthrough vertex program and MOV r0, COL0 ; MOV r2,
 * COL0.zyxw. With 32-bit exports r0 is target A and r2 is target B, so one
 * red triangle fills A with red and B with blue. Nothing else in the mode
 * produces blue: the vertices are red, the quad's are green, and so are the
 * guest bytes behind both surfaces.
 *
 * Then target A goes back to display buffer 0, the set goes back to one
 * target, and the full-screen quad samples ONE of the two through TEX r0,
 * TC0. Which one is the caller's choice, because the harness reads a single
 * centre pixel: --mrt samples B, which is the export that had nowhere to go
 * until the decompiler grew a second SV_TARGET, and --mrt-a samples A, which
 * is what says growing the struct did not disturb the first target. Sampling
 * both in one frame would only assert whichever was drawn last. */
static void emit_mrt_draws(int sample_b)
{
    emit(NV4097_SET_CONTEXT_DMA_COLOR_A,     CELL_GCM_CONTEXT_DMA_MEMORY_HOST_BUFFER);
    emit(NV4097_SET_CONTEXT_DMA_COLOR_B,     CELL_GCM_CONTEXT_DMA_MEMORY_HOST_BUFFER);
    emit(NV4097_SET_SURFACE_COLOR_AOFFSET,   MRT_A_OFFSET);
    emit(NV4097_SET_SURFACE_COLOR_BOFFSET,   MRT_B_OFFSET);
    emit(NV4097_SET_SURFACE_COLOR_TARGET,    CELL_GCM_SURFACE_TARGET_MRT1);
    emit(NV4097_SET_SURFACE_CLIP_HORIZONTAL, MRT_DIM << 16);
    emit(NV4097_SET_SURFACE_CLIP_VERTICAL,   MRT_DIM << 16);

    /* Clear the set, which is one clear per target on both paths, before
     * anything draws into it. That is what a G-buffer pass does, and it is
     * also what gives an unwritten target a colour of its own to be caught
     * holding. */
    emit(NV4097_SET_COLOR_CLEAR_VALUE, MRT_CLEAR);
    emit(NV4097_CLEAR_SURFACE,         0xF0u);

    u8 vp[32];
    rsx_test_vp_mov_out(vp +  0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);   /* MOV o0, v0      */
    rsx_test_vp_mov_out(vp + 16, 3, RSX_TEST_VP_SWZ_IDENT, 1, 1);   /* MOV o1, v3, END */
    emit_programs(vp, sizeof vp, MFP_OFFSET);
    emit_triangle_draw();

    emit(NV4097_SET_CONTEXT_DMA_COLOR_A,     CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER);
    emit(NV4097_SET_SURFACE_COLOR_AOFFSET,   0u);
    emit(NV4097_SET_SURFACE_COLOR_TARGET,    CELL_GCM_SURFACE_TARGET_0);
    emit(NV4097_SET_SURFACE_CLIP_HORIZONTAL, 1280u << 16);
    emit(NV4097_SET_SURFACE_CLIP_VERTICAL,   720u << 16);
    /* The frame's own clear, which every other mode emits before its draws.
     * It has to come after the offscreen pass here rather than before it: the
     * draw engine folds a RUN of clears into one pass's load actions, and a
     * run naming surfaces of different sizes is one it cannot open, so a
     * display clear sitting immediately in front of the set's would take the
     * set's down with it. A title clears each target set as it binds it,
     * which is what this is. */
    emit(NV4097_SET_COLOR_CLEAR_VALUE, CLEAR_ARGB);
    emit(NV4097_CLEAR_SURFACE,         0xF0u);
    emit_textured_draw_at(sample_b ? MRT_B_OFFSET : MRT_A_OFFSET, MRT_DIM, MRT_DIM);
}

/* Write a depth buffer of the guest's own, then sample it -- the two halves
 * of depth-as-texture, in the order a title's shadow or depth-of-field pass
 * does them.
 *
 * First the zeta moves to an address of its own in main memory, is cleared to
 * 1.0, and the flat triangle at DTEX_Z is drawn into it with the depth test
 * and depth writes on. That the write happened is what a renderer has to
 * observe before it may publish the buffer as a texture: a write-enable bit
 * alone does not prove the pass produced a depth map, and a clear-only zeta
 * has nothing worth sampling.
 *
 * Then the zeta moves elsewhere -- a pass may not sample the depth buffer it
 * is rendering into -- the depth test comes off, and texture unit 0 is bound
 * at the FIRST zeta's address as DEPTH24_D8, with the full-screen quad drawn
 * through the passthrough vertex program and TEX r0, TC0. The sampled depth
 * arrives in the red channel, so the centre pixel is DTEX_Z as a byte and
 * nothing else: 1.0 for a zeta nothing wrote or for the guest bytes behind
 * that offset, 0 for a unit left on the zero texture.
 *
 * The depth clear is emitted here rather than with the frame's colour clear
 * because it has to land while the FIRST zeta is the bound one -- it is what
 * makes each frame resolve the depth again instead of reusing frame 1's. */
static void emit_depthtex_draws(void)
{
    emit(NV4097_SET_CONTEXT_DMA_ZETA,      CELL_GCM_CONTEXT_DMA_MEMORY_HOST_BUFFER);
    emit(NV4097_SET_SURFACE_ZETA_OFFSET,   DTEX_OFFSET);
    emit(NV4097_SET_ZSTENCIL_CLEAR_VALUE,  0xFFFFFF00u);   /* depth 1.0, stencil 0 */
    emit(NV4097_CLEAR_SURFACE,             0x3u);          /* depth + stencil */

    emit(NV4097_SET_DEPTH_TEST_ENABLE, 1u);
    emit(NV4097_SET_DEPTH_FUNC,        0x0201u);   /* GL_LESS */
    emit(NV4097_SET_DEPTH_MASK,        1u);
    emit(NV4097_SET_SHADER_PROGRAM,    0u);        /* fixed-function pass */

    emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 0 * 4, VTX_MAIN(DVTX_OFFSET +  0));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 0 * 4, VFMT(4, 32));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 3 * 4, VTX_MAIN(DVTX_OFFSET + 16));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 3 * 4, VFMT(4, 32));
    emit(NV4097_SET_BEGIN_END, 5u);                       /* TRIANGLES       */
    emit(NV4097_DRAW_ARRAYS,   0u | ((3u - 1u) << 24));   /* first=0 count=3 */
    emit(NV4097_SET_BEGIN_END, 0u);

    /* Another zeta for the sampling pass, and no depth test: the quad must
     * reach the target on every frame, not only the first. */
    emit(NV4097_SET_SURFACE_ZETA_OFFSET, 0u);
    emit(NV4097_SET_DEPTH_TEST_ENABLE,   0u);
    emit(NV4097_SET_DEPTH_MASK,          0u);
    emit_textured_draw_fmt(DTEX_OFFSET, DTEX_DIM, DTEX_DIM, 0x90u | 0x20u);
}

/* Bind the two-level texture on unit 0 and draw the small quad through the
 * same passthrough vertex program and TEX r0, TC0 fragment program --tex uses,
 * so the only thing that differs is the texture's mip state.
 *
 * SET_TEXTURE_FILTER's min field is 4, LINEAR_MIPMAP_NEAREST: linear within a
 * level and NEAREST between levels, so the sampled pixel is exactly one
 * level's colour rather than a blend of two. SET_TEXTURE_CONTROL0 carries the
 * LOD range in 4.8 fixed point, max at [18:7] and min at [30:19]: max 1.0
 * (256) with min 0 lets the sampler reach level 1 and no further, and the
 * quad's derivatives put the computed LOD above 1 so it lands there. */
static void emit_mip_draw(void)
{
    u8 vp[48];
    rsx_test_vp_mov_out(vp +  0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);   /* MOV o0, v0      */
    rsx_test_vp_mov_out(vp + 16, 3, RSX_TEST_VP_SWZ_IDENT, 1, 0);   /* MOV o1, v3      */
    rsx_test_vp_mov_out(vp + 32, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);   /* MOV o7, v8, END */
    emit_programs(vp, sizeof vp, TFP_OFFSET);

    emit(NV4097_SET_TEXTURE_OFFSET     + 0, MIP_OFFSET);
    /* ... and two mip levels in [19:16] rather than one. */
    emit(NV4097_SET_TEXTURE_FORMAT     + 0, 2u | (2u << 4) | ((0x85u | 0x20u) << 8) | (2u << 16));
    emit(NV4097_SET_TEXTURE_CONTROL0   + 0, 0x80000000u | (256u << 7));
    emit(NV4097_SET_TEXTURE_CONTROL1   + 0, 0xAAE4u);
    /* CONTROL3's low 20 bits are the row pitch. The natural pitch is what a
     * title writes for a texture with no padding, and it is load-bearing here:
     * a backend that read the wrong bits would size level 0 wrong and read
     * level 1 from the wrong place. */
    emit(NV4097_SET_TEXTURE_CONTROL3   + 0, MIP_W * 4u);
    emit(NV4097_SET_TEXTURE_FILTER     + 0, (4u << 16) | (2u << 24));
    emit(NV4097_SET_TEXTURE_IMAGE_RECT + 0, (MIP_W << 16) | MIP_H);

    emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 0 * 4, VTX_MAIN(MVTX_OFFSET +  0));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 0 * 4, VFMT(4, 48));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 3 * 4, VTX_MAIN(MVTX_OFFSET + 16));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 3 * 4, VFMT(4, 48));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 8 * 4, VTX_MAIN(MVTX_OFFSET + 32));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 8 * 4, VFMT(4, 48));

    emit(NV4097_SET_BEGIN_END, 5u);                       /* TRIANGLES       */
    emit(NV4097_DRAW_ARRAYS,   0u | ((6u - 1u) << 24));   /* first=0 count=6 */
    emit(NV4097_SET_BEGIN_END, 0u);
}

/* Draw the blue triangle through MOV o0, v0 ; MOV o1, v3 (END) -- position
 * and diffuse colour straight through -- and the red/blue-swapping fragment
 * program, so it must come out red. */
static void emit_shader_draw(void)
{
    u8 vp[32];
    rsx_test_vp_mov_out(vp +  0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);   /* MOV o0, v0      */
    rsx_test_vp_mov_out(vp + 16, 3, RSX_TEST_VP_SWZ_IDENT, 1, 1);   /* MOV o1, v3, END */
    emit_programs(vp, sizeof vp, FP_OFFSET);

    emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 0 * 4, VTX_MAIN(SVTX_OFFSET +  0));  /* position */
    emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 0 * 4, VFMT(4, 32));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 3 * 4, VTX_MAIN(SVTX_OFFSET + 16));  /* colour   */
    emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 3 * 4, VFMT(4, 32));

    emit(NV4097_SET_BEGIN_END, 5u);                       /* TRIANGLES        */
    emit(NV4097_DRAW_ARRAYS,   0u | ((3u - 1u) << 24));   /* first=0 count=3  */
    emit(NV4097_SET_BEGIN_END, 0u);                       /* end              */
}

/* Depth test on, LESS, writes on -- the state a title sets for opaque
 * geometry -- and the two triangles as separate BEGIN/END pairs, which is how
 * a title issues two draws rather than one split primitive stream. */
static void emit_depth_draw(void)
{
    emit(NV4097_SET_DEPTH_TEST_ENABLE, 1u);
    emit(NV4097_SET_DEPTH_FUNC,        0x0201u);   /* GL_LESS */
    emit(NV4097_SET_DEPTH_MASK,        1u);

    emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 0 * 4, VTX_MAIN(ZVTX_OFFSET +  0));  /* position */
    emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 0 * 4, VFMT(4, 32));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 3 * 4, VTX_MAIN(ZVTX_OFFSET + 16));  /* colour   */
    emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 3 * 4, VFMT(4, 32));

    emit(NV4097_SET_BEGIN_END, 5u);                       /* TRIANGLES        */
    emit(NV4097_DRAW_ARRAYS,   0u | ((3u - 1u) << 24));   /* first=0 count=3  */
    emit(NV4097_SET_BEGIN_END, 0u);
    emit(NV4097_SET_BEGIN_END, 5u);
    emit(NV4097_DRAW_ARRAYS,   3u | ((3u - 1u) << 24));   /* first=3 count=3  */
    emit(NV4097_SET_BEGIN_END, 0u);
}

/* The pentagon, then the quad strip over it. Both are fixed-function, so
 * every backend can run this mode. */
static void emit_quad_draws(void)
{
    emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 0 * 4, VTX_MAIN(QVTX_OFFSET +  0));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 0 * 4, VFMT(4, 32));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 3 * 4, VTX_MAIN(QVTX_OFFSET + 16));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 3 * 4, VFMT(4, 32));
    emit(NV4097_SET_BEGIN_END, RSX_PRIMITIVE_POLYGON);
    emit(NV4097_DRAW_ARRAYS,   0u | ((5u - 1u) << 24));   /* first=0 count=5 */
    emit(NV4097_SET_BEGIN_END, 0u);

    emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 0 * 4, VTX_MAIN(QSTRIP_OFFSET +  0));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 0 * 4, VFMT(4, 32));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 3 * 4, VTX_MAIN(QSTRIP_OFFSET + 16));
    emit(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 3 * 4, VFMT(4, 32));
    emit(NV4097_SET_BEGIN_END, RSX_PRIMITIVE_QUAD_STRIP);
    emit(NV4097_DRAW_ARRAYS,   0u | ((6u - 1u) << 24));   /* first=0 count=6 */
    emit(NV4097_SET_BEGIN_END, 0u);
}

/* Append this frame's commands to the ring and advance `put`, exactly as a
 * title does. The RSX's `get` pointer lives inside cellGcmSys and chases `put`;
 * it is never rewound, so each frame must occupy fresh ring space rather than
 * overwrite the last one. */
static void submit_frame(int with_draw)
{
    /* The MRT modes clear twice, once per target set, and emit both clears
     * themselves so the display's lands after the offscreen pass rather than
     * in front of it. */
    if (with_draw != 9 && with_draw != 10) {
        emit(NV4097_SET_COLOR_CLEAR_VALUE, CLEAR_ARGB);
        if (with_draw == 4) {
            /* CLEAR_SURFACE bits: 0x01 depth, 0x02 stencil, 0xF0 the four
             * colour channels. ZSTENCIL_CLEAR_VALUE is Z24S8, so 0xFFFFFF00
             * is depth 1.0 and stencil 0 -- the nv40 reset values, and what a
             * title writes before drawing an opaque scene. */
            emit(NV4097_SET_ZSTENCIL_CLEAR_VALUE, 0xFFFFFF00u);
            emit(NV4097_CLEAR_SURFACE,            0xF3u);
        } else {
            emit(NV4097_CLEAR_SURFACE,            0xF0u);
        }
    }
    if (with_draw == 10)     emit_mrt_draws(0);
    else if (with_draw == 9) emit_mrt_draws(1);
    else if (with_draw == 8) emit_depthtex_draws();
    else if (with_draw == 7) emit_quad_draws();
    else if (with_draw == 6) emit_rtt_draws();
    else if (with_draw == 5) emit_mip_draw();
    else if (with_draw == 4) emit_depth_draw();
    else if (with_draw == 3) emit_shader_draw();
    else if (with_draw == 2) emit_textured_draw();
    else if (with_draw == 1) emit_triangle_draw();

    guest_w32(CTRL_ADDR + 0, g_fifo_len);   /* put */
    cellGcm_rsx_process_fifo();
}

/* ---------------------------------------------------------------------------
 * --threads: the host stack a guest PPU thread actually gets
 *
 * sys_ppu_thread_create reserves a big host stack because every recompiled
 * guest call is a real host call and a guest call chain nests just as deeply
 * on the host. The Win32 branch has always done that; the POSIX branch used
 * to take the default, which is 512 KB on Darwin. Nothing about that shows up
 * until a title recurses a few hundred frames deep and the process dies with
 * no diagnosis, so check the number here instead.
 * -----------------------------------------------------------------------*/
static size_t g_reported_stack = 0;
static int    g_thread_ran     = 0;

static size_t host_self_stack_bytes(void)
{
#if defined(__APPLE__)
    return pthread_get_stacksize_np(pthread_self());
#elif defined(__GLIBC__) || defined(__linux__)
    pthread_attr_t a;
    size_t sz = 0;
    if (pthread_getattr_np(pthread_self(), &a) == 0) {
        pthread_attr_getstacksize(&a, &sz);
        pthread_attr_destroy(&a);
    }
    return sz;
#else
    return 0;
#endif
}

static void thread_probe_entry(ppu_context* ctx)
{
    (void)ctx;
    g_reported_stack = host_self_stack_bytes();
    g_thread_ran = 1;
}

/* A guest thread is worth starting only if its host stack can hold a deep
 * recompiled call chain. 64 MB is well under the 256 MB reserved and well
 * over any default, so it separates "the reservation was applied" from "the
 * host handed out whatever it felt like" without pinning the exact number. */
#define HOST_STACK_FLOOR  (64u * 1024u * 1024u)

static int run_thread_check(void)
{
    printf("[host] --threads: guest PPU thread host stack\n");
    vm_stack_alloc_init(&g_vm_stack_alloc);
    g_ppu_thread_entry_trampoline = thread_probe_entry;
    ppu_thread_register_main();

    ppu_context ctx;
    ppu_context_init(&ctx);
    ctx.thread_id = 1;

    u32 tid_out = host_alloc(8, 8);
    ctx.gpr[3] = tid_out;      /* u64* thread id out */
    ctx.gpr[4] = 0x10000;      /* entry (unused: the trampoline is ours) */
    ctx.gpr[5] = 0;            /* arg */
    ctx.gpr[6] = 1000;         /* priority */
    ctx.gpr[7] = 0x10000;      /* guest stack size */
    ctx.gpr[8] = 0;            /* flags */
    ctx.gpr[9] = 0;            /* name */
    int64_t crc = sys_ppu_thread_create(&ctx);
    if (crc != 0) {
        fprintf(stderr, "[host] sys_ppu_thread_create failed: 0x%llX\n",
                (unsigned long long)crc);
        return 1;
    }

    /* The id is written big-endian into guest memory. */
    const uint8_t* p = vm_base + tid_out;
    uint64_t tid = 0;
    for (int i = 0; i < 8; i++) tid = (tid << 8) | p[i];

    ctx.gpr[3] = tid;
    ctx.gpr[4] = 0;
    int64_t jrc = sys_ppu_thread_join(&ctx);
    if (jrc != 0) {
        fprintf(stderr, "[host] sys_ppu_thread_join(tid=%llu) failed: 0x%llX\n",
                (unsigned long long)tid, (unsigned long long)jrc);
        return 1;
    }
    if (!g_thread_ran) {
        fprintf(stderr, "[host] the guest thread never ran\n");
        return 1;
    }
    if (g_reported_stack == 0) {
        printf("[host] host stack size not queryable here -- thread ran, size unchecked\n");
        return 0;
    }
    printf("[host] guest thread host stack: %zu bytes (%.1f MB), floor %.0f MB %s\n",
           g_reported_stack, g_reported_stack / 1048576.0,
           HOST_STACK_FLOOR / 1048576.0,
           g_reported_stack >= HOST_STACK_FLOOR ? "OK" : "TOO SMALL");
    return g_reported_stack >= HOST_STACK_FLOOR ? 0 : 5;
}

/* ---------------------------------------------------------------------------
 * --audio-pad: bring the SDL2 audio and pad backends up and back down
 *
 * Neither had ever been initialised off Windows -- the Windows build takes
 * WASAPI and XInput -- so nothing said whether they even run. This is not a
 * sound or input test: it is the assertion that init, a few polls and
 * shutdown complete, twice, and that a shutdown with no device open is
 * survivable. With SDL_AUDIODRIVER=dummy and no controller plugged in it
 * needs no hardware, which is what lets CI run it.
 * -----------------------------------------------------------------------*/
static int run_audio_pad_check(void)
{
    int rc = 0;
    printf("[host] --audio-pad: SDL2 audio and pad backends\n");

    /* Quit before init: the guest is entitled to do this and lv2 answers
     * NOT_INIT / NOT_OPENED rather than tearing down state that was never
     * built. It is also where an unguarded join or close would fall over. */
    s32 r = cellAudioQuit();
    printf("[host] cellAudioQuit before init -> 0x%08X %s\n", (unsigned)r,
           r == (s32)CELL_AUDIO_ERROR_NOT_INIT ? "OK" : "UNEXPECTED");
    if (r != (s32)CELL_AUDIO_ERROR_NOT_INIT) rc = 6;

    r = cellPadEnd();
    printf("[host] cellPadEnd before init -> 0x%08X %s\n", (unsigned)r,
           r == (s32)CELL_PAD_ERROR_NOT_OPENED ? "OK" : "UNEXPECTED");
    if (r != (s32)CELL_PAD_ERROR_NOT_OPENED) rc = 6;

    /* Twice through, because the second cycle is where a shutdown that left
     * a stale handle behind shows up. */
    for (int pass = 0; pass < 2; pass++) {
        printf("[host] pass %d\n", pass);

        r = cellAudioInit();
        if (r != CELL_OK) {
            fprintf(stderr, "[host] cellAudioInit -> 0x%08X\n", (unsigned)r);
            return 6;
        }
        r = cellPadInit(7);
        if (r != CELL_OK) {
            fprintf(stderr, "[host] cellPadInit -> 0x%08X\n", (unsigned)r);
            return 6;
        }

        /* A double init is the guest's mistake and gets an error, not a
         * second backend. */
        r = cellAudioInit();
        if (r != (s32)CELL_AUDIO_ERROR_ALREADY_INIT) {
            fprintf(stderr, "[host] second cellAudioInit -> 0x%08X, want ALREADY_INIT\n",
                    (unsigned)r);
            rc = 6;
        }
        r = cellPadInit(7);
        if (r != (s32)CELL_PAD_ERROR_ALREADY_OPENED) {
            fprintf(stderr, "[host] second cellPadInit -> 0x%08X, want ALREADY_OPENED\n",
                    (unsigned)r);
            rc = 6;
        }

        /* Let the mixing thread run a few blocks and poll the pad the way a
         * frame loop would. No controller is expected to be attached. */
        for (int i = 0; i < 30; i++) {
            cellPad_poll();
            struct timespec ts = { 0, 2 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }

        r = cellPadEnd();
        if (r != CELL_OK) { fprintf(stderr, "[host] cellPadEnd -> 0x%08X\n", (unsigned)r); rc = 6; }
        r = cellAudioQuit();
        if (r != CELL_OK) { fprintf(stderr, "[host] cellAudioQuit -> 0x%08X\n", (unsigned)r); rc = 6; }

        /* And again, on the state the shutdown just left behind. */
        r = cellAudioQuit();
        if (r != (s32)CELL_AUDIO_ERROR_NOT_INIT) {
            fprintf(stderr, "[host] repeat cellAudioQuit -> 0x%08X, want NOT_INIT\n",
                    (unsigned)r);
            rc = 6;
        }
        r = cellPadEnd();
        if (r != (s32)CELL_PAD_ERROR_NOT_OPENED) {
            fprintf(stderr, "[host] repeat cellPadEnd -> 0x%08X, want NOT_OPENED\n",
                    (unsigned)r);
            rc = 6;
        }
    }

    printf("[host] audio and pad backends came up and down twice %s\n",
           rc == 0 ? "OK" : "with failures");
    return rc;
}

/* Which modes need a backend that translates the guest's own programs. --tex
 * is not one of them: the null backend samples the texture in its own
 * software path, which is why Linux runs that mode and not these. */
static int mode_needs_translator(int mode)
{
    return mode == 3 || mode == 5 || mode == 6 || mode == 8 ||
           mode == 9 || mode == 10;
}

/* ...and which ones must, on a backend that has a translator, have run the
 * guest's programs rather than the built-in pair. */
static int mode_runs_guest_programs(int mode)
{
    return mode == 2 || mode_needs_translator(mode);
}

static const char* mode_flag_name(int mode)
{
    switch (mode) {
    case 3:  return "--shader";
    case 5:  return "--mip";
    case 6:  return "--rtt";
    case 8:  return "--depthtex";
    case 9:  return "--mrt";
    case 10: return "--mrt-a";
    default: return "(mode)";
    }
}

int main(int argc, char** argv)
{
    /* frames = 0 runs until the window is closed, which is what the .app
     * bundle uses; a fixed count keeps the CI runs bounded. */
    int frames = 3, do_draw = 0;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--frames=", 9) == 0) frames = atoi(argv[i] + 9);
        else if (strcmp(argv[i], "--draw") == 0)   do_draw = 1;
        /* --threads: no graphics at all, just the lv2 thread path. */
        else if (strcmp(argv[i], "--threads") == 0) {
            vm_base = host_vm_alloc();
            if (!vm_base) { fprintf(stderr, "[host] guest VM alloc failed\n"); return 1; }
            int trc = run_thread_check();
            free(vm_base);
            return trc;
        }
        /* --audio-pad: no graphics either, just the SDL2 backends. */
        else if (strcmp(argv[i], "--audio-pad") == 0) {
            vm_base = host_vm_alloc();
            if (!vm_base) { fprintf(stderr, "[host] guest VM alloc failed\n"); return 1; }
            int arc = run_audio_pad_check();
            free(vm_base);
            return arc;
        }
        /* --tex: bind a texture and sample it, which exercises the shared
         * rsx_texture_layout/decode path end to end rather than in a unit
         * test. The vertex colour differs from the texture colour, so a
         * backend that ignored the texture fails the assertion below. */
        else if (strcmp(argv[i], "--tex") == 0)    do_draw = 2;
        /* --shader: load a guest vertex program and a guest fragment program
         * through the FIFO and draw with them, which exercises the whole
         * decompile -> translate -> compile -> bind path a real title's
         * draws take. Only a backend with that path can pass it. */
        else if (strcmp(argv[i], "--shader") == 0) do_draw = 3;
        /* --depth: two full-viewport triangles, the near one drawn FIRST, so
         * the centre pixel is red only if the depth test rejected the later
         * far one. A backend with no depth buffer draws them in submission
         * order and fails. Fixed-function, so every backend can run it. */
        else if (strcmp(argv[i], "--depth") == 0)  do_draw = 4;
        /* --mip: bind a texture with TWO levels and draw a quad small enough
         * that the sampler reaches level 1. The levels are different colours,
         * so a backend that uploads only level 0 presents the wrong one. */
        else if (strcmp(argv[i], "--mip") == 0)    do_draw = 5;
        /* --rtt: render into a colour surface of the guest's own and then
         * sample it, which is what a title does for every reflection, shadow
         * map and post-processing pass. Needs the guest-program path. */
        else if (strcmp(argv[i], "--rtt") == 0)    do_draw = 6;
        /* --quads: a POLYGON and a QUAD_STRIP, the two primitives a host API
         * has no equivalent of that are not simply a list of quads. Both have
         * to be expanded into triangles before they can be drawn at all.
         * Fixed-function, so every backend can run it. */
        else if (strcmp(argv[i], "--quads") == 0)  do_draw = 7;
        /* --depthtex: write a depth buffer of the guest's own and then sample
         * it as a DEPTH24_D8 texture, which is what a shadow map, a soft
         * particle and every depth-of-field pass does. Needs a renderer that
         * tracks a zeta per address and can publish one as a texture; the
         * vtable path does not, so this mode is the draw engine's. */
        else if (strcmp(argv[i], "--depthtex") == 0) do_draw = 8;
        /* --mrt: declare an MRT1 set and fill both of its members from one
         * draw, with a fragment program that writes r0 and r2. That is a
         * deferred renderer's G-buffer pass, and the second target is the one
         * nothing wrote until the fragment decompiler grew a second
         * SV_TARGET. --mrt samples target B on the display; --mrt-a samples
         * target A, since a centre-pixel readback can only speak for one. */
        else if (strcmp(argv[i], "--mrt") == 0)   do_draw = 9;
        else if (strcmp(argv[i], "--mrt-a") == 0) do_draw = 10;
    }
    /* The guest-program modes, which a backend without a translator cannot
     * run. --depth and --quads are not among them: both are fixed-function. */
    if (mode_needs_translator(do_draw) && !HOST_BACKEND_GUEST_SHADERS) {
        printf("[host] %s: the %s backend runs no guest programs; nothing to check\n",
               mode_flag_name(do_draw), HOST_BACKEND_NAME);
        return 0;
    }

    vm_base = host_vm_alloc();
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
    if (do_draw == 3) upload_shader_triangle();
    if (do_draw == 4) upload_depth_triangles();
    if (do_draw == 5) { upload_mip_texture(); upload_mip_quad(); }
    /* --rtt draws both: the triangle into the surface, the quad out of it. */
    if (do_draw == 6) { upload_triangle(); upload_textured_quad(); upload_rtt_backing(); }
    if (do_draw == 7) upload_quad_geometry();
    /* --depthtex draws both too: the flat triangle into the zeta, the quad
     * out of it. */
    if (do_draw == 8) {
        upload_depthtex_triangle();
        upload_textured_quad();
        upload_depthtex_backing();
    }
    /* --mrt draws both halves as well: the triangle into the set, the quad
     * out of one of its members. */
    if (do_draw == 9 || do_draw == 10) {
        upload_triangle();
        upload_textured_quad();
        upload_mrt_backing();
    }
    if (mode_runs_guest_programs(do_draw)) upload_fragment_programs();
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
         * With --depth it is the NEAR triangle's red, drawn before the far
         * green one: green here means the depth test did nothing. With --mip
         * it is level 1's colour, not level 0's, which no backend that
         * uploads a single level can produce. With --rtt it is red again, and
         * red exists nowhere but in the offscreen surface the first draw
         * rendered into: the quad's own vertices are green, and so are the
         * guest bytes behind the surface, so anything that skips the
         * render-to-texture path shows green. With --quads it is the quad
         * strip's red over the pentagon's green, and the centre belongs to
         * the strip's second quad, so a dropped or half-expanded strip is
         * green. With --depthtex it is the DEPTH the first pass wrote, come
         * back through a sampler into the red channel: a zeta nothing wrote,
         * or the guest bytes behind its address, both read 1.0 and present
         * saturated red instead. With --mrt it is target B's blue and with
         * --mrt-a target A's red, both written by one draw through an MRT1
         * set: MRT_CLEAR means the target was attached and cleared but never
         * fed, which is what a dropped second colour export looks like from
         * here, and MRT_BACKING means the guest bytes behind the surface were
         * sampled instead of the surface itself.
         */
        u32 want = (do_draw == 10) ? MRT_A_ARGB
                 : (do_draw == 9) ? MRT_B_ARGB
                 : (do_draw == 8) ? DTEX_ARGB     /* the sampled depth as a byte */
                 : (do_draw == 7) ? 0xFFFF0000u   /* the quad strip over the polygon */
                 : (do_draw == 6) ? 0xFFFF0000u   /* only the surface holds red  */
                 : (do_draw == 5) ? MIP_L1_ARGB
                 : (do_draw == 4) ? 0xFFFF0000u   /* near red beats far green    */
                 : (do_draw == 3) ? 0xFFFF0000u   /* blue vertices, FP swaps r/b */
                 : (do_draw == 2) ? TEX_ARGB
                 : (do_draw == 1) ? 0xFFFF0000u
                                  : CLEAR_ARGB;
        printf("[host] presented pixel: 0x%08X (expected 0x%08X) %s\n",
               back, want, (back & 0x00FFFFFFu) == (want & 0x00FFFFFFu) ? "OK" : "MISMATCH");
        if ((back & 0x00FFFFFFu) != (want & 0x00FFFFFFu)) rc = 3;
    }
    if (mode_runs_guest_programs(do_draw) && HOST_BACKEND_GUEST_SHADERS) {
        /* The pixel alone cannot prove the programs ran: a backend that
         * ignored them would draw the triangle blue, which the check above
         * catches, but one that ran the vertex program and dropped the
         * fragment program to a passthrough would also be blue -- so ask
         * the backend which path the draw took as well. */
        u32 gd = host_backend_guest_draws();
        printf("[host] draws through guest programs: %u %s\n", gd, gd ? "OK" : "NONE");
        if (!gd) rc = 4;
    }

    host_backend_shutdown();
    free(vm_base);
    return rc;
}
