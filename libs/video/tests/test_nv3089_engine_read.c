/* NV3089 blit: the source origin (IMAGE_IN_POINT, 12.4 fixed point per axis) and the engine write-back before the copy.
 *
 * With the draw engine on, the scene a title scales with NV3089 exists only on the GPU, so nv3089_blit asks the
 * engine to write the source rectangle back to guest memory (nv3089_engine_read) before it copies from there. The
 * steps (DS_DX, DT_DY) have 20 fractional bits, so the origin enters the copy as origin << 16; it used to be << 8, which
 * counts it 256 times too small: a one texel origin (in_uv 0x00100010, a title's 2:1 resolve tiles carry it) read
 * column 0 and row 0 instead of 1 and the write-back, which took the origin as u0 >> 4, did not cover them (stale guest
 * memory: the first column and row of every tile came out black).
 *
 * The test plays the engine: guest_read stores into the rectangle it is asked for a pixel that names its own source
 * position, and nothing else. Destination pixel (x, y) must then be the source pixel at (origin + x * scale,
 * origin + y * scale).
 *
 * A destination in a tile bound over main memory (cellGcmSetTile, location MAIN) is written through the tiling
 * (rsx_tiling.h): A retail title downsamples its scene into one for an SPU depth-of-field job, which reads the tiled
 * layout; written linearly, the job's output came back as bands. The tiled case checks every destination pixel at
 * its tiled address. A source there is read through it, by NV3089 and by NV309E's swizzled copy, and an NV308A inline
 * word into it lands at its tiled address.
 *
 * clang -std=gnu17 -I include -I libs/video libs/video/tests/test_nv3089_engine_read.c -Wl,-dead_strip -pthread
 *   -o /tmp/test_nv3089_engine_read && /tmp/test_nv3089_engine_read
 *   (gcc: -ffunction-sections -fdata-sections -Wl,--gc-sections)
 */
#include "../cellGcmSys.c"
#include "../rsx_tiling.c"
#include <stdio.h>
#include <stdlib.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
ps3_guest_caller_fn g_ps3_guest_caller;
/* The rest of the walker's world: no graphics backend, no lifted program. */
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
u32 g_rsx_last_reference;
void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
void rsx_raise_user_cmd(uint32_t arg) { (void)arg; }
void ppu_dump_bctrl_ring(uint32_t n, const char* why) { (void)n; (void)why; }
void ppu_dump_guest_stack(ppu_context* c, const char* why) { (void)c; (void)why; }
void rsx_state_init(rsx_state* state) { (void)state; }
int  rsx_process_method(rsx_state* state, u32 method, u32 data) { (void)state; (void)method; (void)data; return 0; }
int  rsx_draw_engine_enabled(void) { return 1; }
void rsx_draw_engine_method(u32 method, u32 arg) { (void)method; (void)arg; }
void rsx_draw_engine_note_flip(u32 buffer_id) { (void)buffer_id; }
/* guest_read is defined below: it plays the engine's part. */
void rsx_draw_engine_guest_wrote(u32 l, u32 o, u32 p, u32 w, u32 h) { (void)l; (void)o; (void)p; (void)w; (void)h; }
int  rsx_live_draw_enabled(void) { return 0; }
void rsx_live_draw_method(u32 method, u32 arg) { (void)method; (void)arg; }
int  rsx_live_draw_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 w, u32 h)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)w; (void)h; return 0; }
void rsx_live_draw_note_resolve(u32 a, u32 b, u32 c, u32 d) { (void)a; (void)b; (void)c; (void)d; }
int  rsx_live_draw_resolve_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 g, u32 h,
                                u32 i, u32 j, u32 k, u32 l)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; (void)j; (void)k; (void)l; return 0; }


#define LOCAL_BASE 0x00200000u      /* where "local memory" sits in the test's guest VM */
#define SRC_OFF    0x00000000u
#define DST_OFF    0x00100000u

/* Main memory for the tiled cases: IO page 0 at EA 1 MB. Tile 4 as a retail title sets it, scaled down. */
#define MAIN_EA   0x00100000u
#define TILE_IO   0x00030000u
static const rsx_tile s_tile = { TILE_IO, 0x80000u, 2048u, 2u };

/* The EA the RSX keeps main-memory offset `io` at while the tile is bound. */
static u32 main_ea(u32 io)
{
    return MAIN_EA + (io - s_tile.offset < s_tile.size ? rsx_tile_address(&s_tile, io) : io);
}

/* A non-zero pixel that names the source position it stands for. */
static u32 src_pixel(u32 col, u32 row) { return 0xFF000000u | (row << 12) | col; }

static int s_reads;
static u32 s_src_off = SRC_OFF;     /* the blit source's first pixel */

/* What the engine does for a transfer: the pixels of the surface in this rectangle, as 32-bit guest words, as the
 * RSX stores them (a main-memory source at its tiled addresses). */
void rsx_draw_engine_guest_read(u32 loc, u32 off, u32 pitch, u32 w, u32 h)
{
    s_reads++;
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            const u32 at = off + y * pitch + x * 4u;
            vm_write32(loc == 0u /* RSX_LOCATION_LOCAL */ ? cellGcmResolveLocated(1, at) : main_ea(at),
                       src_pixel((off - s_src_off) % pitch / 4u + x, (off - s_src_off) / pitch + y));
        }
}

static int fails;

/* One blit of `out` x `out` pixels from a source `scale` times larger, its origin at (uv >> 4) texels. */
static void blit_case(const char* what, u32 out, u32 scale, u32 uv, u32 interp)
{
    const u32 src_w = out * scale + (uv >> 4) + 8u, src_pitch = src_w * 4u, dst_pitch = out * 4u;
    memset(vm_base + LOCAL_BASE + SRC_OFF, 0, (size_t)src_pitch * src_w);      /* stale guest memory */
    memset(vm_base + LOCAL_BASE + DST_OFF, 0, (size_t)dst_pitch * out);
    s_reads = 0;

    memset(&s_nv3089, 0, sizeof s_nv3089);
    memset(&s_gcm2d, 0, sizeof s_gcm2d);
    s_nv3089.fmt     = 3u;                                  /* A8R8G8B8 in the scale-format enum */
    s_nv3089.out_sz  = (out << 16) | out;
    s_nv3089.in_sz   = ((out * scale) << 16) | (out * scale);
    s_nv3089.in_fmt  = (interp << 24) | (2u << 16) | src_pitch;   /* corner origin, interpolator, pitch */
    s_nv3089.in_off  = SRC_OFF;
    s_nv3089.in_uv   = (uv << 16) | uv;
    s_nv3089.ds_dx   = s_nv3089.dt_dy = scale << 20;         /* 12.20 */
    s_gcm2d.pitch      = dst_pitch << 16;
    s_gcm2d.dst_offset = DST_OFF;
    nv3089_blit();

    const u32 origin = uv >> 4;                                 /* texels */
    u32 wrong = 0, zero = 0;
    for (u32 y = 0; y < out; y++)
        for (u32 x = 0; x < out; x++) {
            const u32 got = vm_read32(LOCAL_BASE + DST_OFF + y * dst_pitch + x * 4u);
            if (got == 0) zero++;
            if (got != src_pixel(origin + x * scale, origin + y * scale)) wrong++;
        }
    const int ok = s_reads > 0 && wrong == 0;
    printf("[%s] %s: %u of %u destination pixels are not the expected source pixel (%u of them never got engine data)\n",
           ok ? "PASS" : "FAIL", what, wrong, out * out, zero);
    fails += !ok;
}

static void blit_tiled_case(const char* what, u32 out, u32 scale)
{
    const u32 src_w = out * scale + 8u, src_pitch = src_w * 4u;
    const u32 dst_off = TILE_IO + 64u;                          /* 16 pixels into the row */
    memset(vm_base + LOCAL_BASE + SRC_OFF, 0, (size_t)src_pitch * src_w);
    memset(vm_base + MAIN_EA, 0, 0x100000u);
    s_reads = 0;

    memset(&s_nv3089, 0, sizeof s_nv3089);
    memset(&s_gcm2d, 0, sizeof s_gcm2d);
    s_nv3089.fmt     = 3u;
    s_nv3089.out_sz  = (out << 16) | out;
    s_nv3089.in_sz   = ((out * scale) << 16) | (out * scale);
    s_nv3089.in_fmt  = (1u << 24) | (2u << 16) | src_pitch;
    s_nv3089.in_off  = SRC_OFF;
    s_nv3089.ds_dx   = s_nv3089.dt_dy = scale << 20;
    s_gcm2d.pitch      = s_tile.pitch << 16;
    s_gcm2d.dst_offset = dst_off;
    s_gcm2d.dst_dma    = 0xFEED0001u;                            /* main memory */
    nv3089_blit();

    u32 wrong = 0, moved = 0;
    for (u32 y = 0; y < out; y++)
        for (u32 x = 0; x < out; x++) {
            const u32 lin = dst_off + y * s_tile.pitch + x * 4u;
            const u32 a = rsx_tile_address(&s_tile, lin);
            moved += a != lin;
            if (vm_read32(MAIN_EA + a) != src_pixel(x * scale, y * scale)) wrong++;
        }
    const int ok = s_reads > 0 && wrong == 0 && moved > 0;
    printf("[%s] %s: %u of %u destination pixels are not at their tiled address (%u moved by the tiling)\n",
           ok ? "PASS" : "FAIL", what, wrong, out * out, moved);
    fails += !ok;
}

/* A source in the tile, `scale` times the `out` x `out` destination, read through the tiling into a linear
 * local destination (NV3089) or a swizzled one (NV309E). */
static void blit_tiled_source_case(const char* what, u32 out, u32 scale, int swizzled)
{
    const u32 src_off = TILE_IO + 4u * s_tile.pitch + 128u, dst_pitch = out * 4u;
    memset(vm_base + MAIN_EA, 0, 0x100000u);
    memset(vm_base + LOCAL_BASE + DST_OFF, 0, (size_t)dst_pitch * out);
    s_reads = 0;
    s_src_off = src_off;

    memset(&s_nv3089, 0, sizeof s_nv3089);
    memset(&s_gcm2d, 0, sizeof s_gcm2d);
    s_nv3089.src_dma = 0xFEED0001u;                            /* main memory */
    s_nv3089.fmt     = 3u;
    s_nv3089.out_sz  = (out << 16) | out;
    s_nv3089.in_sz   = ((out * scale) << 16) | (out * scale);
    s_nv3089.in_fmt  = (1u << 24) | (2u << 16) | s_tile.pitch;
    s_nv3089.in_off  = src_off;
    s_nv3089.ds_dx   = s_nv3089.dt_dy = scale << 20;
    s_gcm2d.pitch      = dst_pitch << 16;
    s_gcm2d.dst_offset = DST_OFF;
    u32 l2 = 0;
    while ((1u << l2) < out) l2++;
    if (swizzled) {
        s_nv309e.active = 1;
        s_nv309e.fmt    = (l2 << 24) | (l2 << 16) | 3u;
        s_nv309e.offset = DST_OFF;
    }
    nv3089_blit();
    memset(&s_nv309e, 0, sizeof s_nv309e);
    s_src_off = SRC_OFF;

    u32 wrong = 0;
    for (u32 y = 0; y < out; y++)
        for (u32 x = 0; x < out; x++) {
            const u32 at = swizzled ? nv309e_swz(x, y, l2, l2) * 4u : y * dst_pitch + x * 4u;
            if (vm_read32(LOCAL_BASE + DST_OFF + at) != src_pixel(x * scale, y * scale)) wrong++;
        }
    const int ok = s_reads > 0 && wrong == 0;
    printf("[%s] %s: %u of %u destination pixels are not the expected source pixel\n",
           ok ? "PASS" : "FAIL", what, wrong, out * out);
    fails += !ok;
}

/* An NV308A inline word into the tile lands at its tiled address. */
static void inline_tiled_case(void)
{
    memset(vm_base + MAIN_EA, 0, 0x100000u);
    memset(&s_gcm2d, 0, sizeof s_gcm2d);
    memset(&s_nv309e, 0, sizeof s_nv309e);
    s_gcm2d.dst_dma    = 0xFEED0001u;
    s_gcm2d.dst_offset = TILE_IO;
    s_gcm2d.pitch      = s_tile.pitch << 16;
    s_gcm2d.point      = (5u << 16) | 3u;                       /* row 5, pixel 3 */
    gcm_2d_method(4, 0x0400 + 2u * 4u, 0x11223344u);            /* COLOR[2]: pixel 5 */
    const u32 at = TILE_IO + 5u * s_tile.pitch + 5u * 4u;
    const int ok = vm_read32(main_ea(at)) == 0x11223344u && main_ea(at) != MAIN_EA + at;
    printf("[%s] an inline word into a tile in main memory lands at its tiled address\n", ok ? "PASS" : "FAIL");
    fails += !ok;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const uint32_t vm_size = LOCAL_BASE + 0x00400000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { puts("FAIL: guest VM alloc"); return 1; }
    ppu_vm_size = vm_size;
    ppu_hle_inject_base = vm_size;
    s_config.localAddress = LOCAL_BASE;

    blit_case("2:1 resolve, no source offset", 16, 2, 0x00, 1);
    blit_case("2:1 resolve, one texel source offset (in_uv 0x10)", 16, 2, 0x10, 1);
    blit_case("2:1 resolve, 3 texel source offset", 16, 2, 0x30, 1);
    blit_case("1:1 copy, 20 texel source offset", 16, 1, 0x140, 0);
    blit_case("1:1 copy, one texel source offset", 16, 1, 0x10, 0);
    blit_case("4:1 resolve, one texel source offset", 16, 4, 0x10, 1);
    s_ea_address_table[TILE_IO >> 20] = (u16)(MAIN_EA >> 20);
    cellGcmSetTile(4, 1 /* main */, s_tile.offset, s_tile.size, s_tile.pitch, 0, 3, (u8)s_tile.bank);
    blit_tiled_case("2:1 downsample into a tile in main memory", 16, 2);
    blit_tiled_case("1:1 copy into a tile in main memory", 16, 1);
    blit_tiled_source_case("2:1 downsample out of a tile in main memory", 16, 2, 0);
    blit_tiled_source_case("1:1 copy out of a tile in main memory", 16, 1, 0);
    blit_tiled_source_case("NV309E swizzled 2:1 copy out of a tile in main memory", 16, 2, 1);
    inline_tiled_case();
    printf("[nv3089_engine_read] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}
