/* rsx_tiling.h / rsx_tiling.c: where the RSX keeps a byte of a tiled main-memory region, and the range copies
 * every RSX client goes through.
 *
 * The vectors come from envytools' tile_translate_addr (nvhw/tile.c) itself, built unmodified with the RSX's memory
 * configuration, with the four in-block bits rsx_tiling.h names set the RSX's way; one per pitch class the function
 * distinguishes: a power of two (factor 32, 8 and 2) and a prime multiple (5120 = 5 x 4 x 256). The first is
 * a retail title's tile 4 (main 0x30000, 0x580000 bytes, pitch 8192, bank 2). Then, for that tile: every 32-byte block
 * lands on a distinct block inside the tile. Then the range copies: a row into the tiling and back, a range that
 * starts before a tile and runs on through a second one, IO pages scattered in host memory, unreachable pages,
 * a local tile, and the footprint a change detector hashes.
 *
 * python3 tools/test_rsx_tiling.py, or
 * clang -std=gnu17 -I include -I libs/video libs/video/tests/test_rsx_tiling.c libs/video/rsx_tiling.c -o /tmp/t
 */
#include "rsx_tiling.h"
#include "rsx_dispatch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

#define CHECK(cond, ...) do { int ok_ = !!(cond); printf("[%s] ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); \
                              printf("\n"); fails += !ok_; } while (0)

static void vectors(const char* what, rsx_tile t, const u32 (*v)[2], u32 n)
{
    u32 wrong = 0;
    for (u32 i = 0; i < n; i++)
        if (rsx_tile_address(&t, v[i][0]) != v[i][1]) {
            printf("  0x%X -> 0x%X, expected 0x%X\n", v[i][0], rsx_tile_address(&t, v[i][0]), v[i][1]);
            wrong++;
        }
    CHECK(!wrong, "%s: %u of %u addresses differ", what, wrong, n);
}

/* Guest main memory for the range copies: GUEST bytes, IO page p at host page p ^ g_scatter, and g_hole (when not
 * ~0) an IO page nothing backs. */
#define GUEST (8u << 20)
#define PAGE  0x100000u
static u8* g_mem;
static u32 g_scatter, g_hole = ~0u;

static u8* guest_ptr(void* user, u32 location, u32 offset, u32 len)
{
    (void)user;
    if (location != RSX_LOCATION_MAIN || (u64)offset + len > GUEST) return NULL;
    if ((offset & ~(PAGE - 1u)) != ((offset + len - 1u) & ~(PAGE - 1u))) return NULL;   /* one page at a time */
    if (offset / PAGE == g_hole) return NULL;
    return g_mem + ((offset / PAGE) ^ g_scatter) * PAGE + offset % PAGE;
}

static const rsx_tiled_mem g_view = { guest_ptr, NULL, NULL };

/* The host byte the RSX keeps location offset `lin` at. */
static u8* rsx_byte(u32 lin)
{
    rsx_tile t;
    u32 a = lin;
    if (rsx_tiling_find(RSX_LOCATION_MAIN, lin, 1, &t)) a = rsx_tile_address(&t, lin);
    return g_mem + ((a / PAGE) ^ g_scatter) * PAGE + a % PAGE;
}

static void ranges(rsx_tile tile4)
{
    g_mem = calloc(GUEST, 1);
    u8* buf = malloc(0x70000u);
    u8* back = malloc(0x70000u);
    rsx_tiling_set(4, RSX_LOCATION_MAIN, tile4.offset, tile4.size, tile4.pitch, tile4.bank);

    /* a 480-pixel row, not block aligned, into the tiling and back */
    for (u32 i = 0; i < 1920u; i++) buf[i] = (u8)(i * 7u + 3u);
    const u32 at = tile4.offset + 17u * 8192u + 2048u + 4u;
    int all = rsx_tiled_write(&g_view, RSX_LOCATION_MAIN, at, buf, 1920u);
    all &= rsx_tiled_read(&g_view, RSX_LOCATION_MAIN, at, back, 1920u);
    CHECK(all && !memcmp(buf, back, 1920u) && g_mem[rsx_tile_address(&tile4, at)] == buf[0] &&
          rsx_tile_address(&tile4, at) != at, "a row written through the tiling reads back the same, at its tiled address");

    /* a range from before a tile through it and a second tile right after it, to past both; each tile whole
     * 64-row bands (a partial band's tiled addresses run past the tile, and those bytes are skipped) */
    const rsx_tile a = { 0x600000u, 0x10000u, 512u, 3u }, b = { 0x610000u, 0x50000u, 5120u, 1u };
    rsx_tiling_set(0, RSX_LOCATION_MAIN, a.offset, a.size, a.pitch, a.bank);
    rsx_tiling_set(1, RSX_LOCATION_MAIN, b.offset, b.size, b.pitch, b.bank);
    const u32 from = a.offset - 100u, len = a.size + b.size + 200u;
    for (u32 i = 0; i < len; i++) buf[i] = (u8)(i * 13u + (i >> 8));
    all = rsx_tiled_write(&g_view, RSX_LOCATION_MAIN, from, buf, len);
    u32 misplaced = 0;
    for (u32 i = 0; i < len; i++) misplaced += *rsx_byte(from + i) != buf[i];
    memset(back, 0, len);
    all &= rsx_tiled_read(&g_view, RSX_LOCATION_MAIN, from, back, len);
    CHECK(all && !misplaced && !memcmp(buf, back, len) && g_mem[from] == buf[0] && g_mem[from + len - 1u] == buf[len - 1u],
          "a range across two tiles goes through each tiling, linear outside them (%u bytes misplaced)", misplaced);

    /* IO pages scattered in host memory: each tiled byte still lands on its own page */
    g_scatter = 1u;
    const u32 wide = 0x1F0000u;                       /* rows spanning IO pages 1 and 2 */
    for (u32 i = 0; i < 0x30000u; i++) buf[i] = (u8)(i * 5u + 1u);
    all = rsx_tiled_write(&g_view, RSX_LOCATION_MAIN, wide, buf, 0x30000u);
    misplaced = 0;
    for (u32 i = 0; i < 0x30000u; i++) misplaced += *rsx_byte(wide + i) != buf[i];
    CHECK(all && !misplaced, "IO pages scattered in host memory are reached page by page (%u bytes misplaced)", misplaced);
    g_scatter = 0;

    /* an IO page nothing backs: the read zero-fills its bytes and says so, the rest arrive */
    g_hole = 2u;
    memset(back, 0xEE, 0x30000u);
    all = rsx_tiled_read(&g_view, RSX_LOCATION_MAIN, wide, back, 0x30000u);
    u32 zeros = 0, kept = 0;
    for (u32 i = 0; i < 0x30000u; i++) {
        rsx_tile t;
        rsx_tiling_find(RSX_LOCATION_MAIN, wide + i, 1, &t);
        if (rsx_tile_address(&t, wide + i) / PAGE == g_hole) zeros += back[i] == 0;
        else kept += back[i] == *rsx_byte(wide + i);
    }
    CHECK(!all && zeros + kept == 0x30000u && zeros, "an unbacked page reads as zero and fails the read (%u zero)", zeros);
    g_hole = ~0u;

    /* a local tile is transparent; unbound, main memory is linear again */
    rsx_tile t;
    rsx_tiling_set(2, RSX_LOCATION_LOCAL, 0x700000u, 0x10000u, 1024u, 0u);
    CHECK(!rsx_tiling_find(RSX_LOCATION_MAIN, 0x700000u, 0x10000u, &t) &&
          !rsx_tiling_find(RSX_LOCATION_LOCAL, 0x700000u, 0x10000u, &t), "a local tile is not kept");
    rsx_tiling_set(0, 0, 0, 0, 0, 0);
    rsx_tiling_set(1, 0, 0, 0, 0, 0);
    rsx_tiling_set(4, 0, 0, 0, 0, 0);
    CHECK(!rsx_tiling_active() && !rsx_tiling_find(RSX_LOCATION_MAIN, 0, GUEST, &t), "unbound, no tile is found");
    rsx_tiled_write(&g_view, RSX_LOCATION_MAIN, at, buf, 64u);
    CHECK(!memcmp(g_mem + at, buf, 64u), "with no tile bound a write is linear");
    free(back);
    free(buf);
    free(g_mem);
}

/* Every 32-byte block of a range is stored inside its footprint. */
static void footprint_case(const rsx_tile* t, u32 offset, u32 len)
{
    u32 lo, span, outside = 0;
    const int any = rsx_tiling_footprint(RSX_LOCATION_MAIN, offset, len, &lo, &span);
    for (u32 p = offset & ~31u; p < offset + len; p += 32u) {
        u32 a = p;
        if (p - t->offset < t->size) a = rsx_tile_address(t, p);
        if (a < lo || a - lo >= span || (a + 31u) - lo >= span) outside++;
    }
    CHECK(any && !outside && lo <= offset && lo + span >= offset + len,
          "pitch %u: the footprint of 0x%X+0x%X (0x%X+0x%X) holds all of its bytes (%u blocks outside)",
          t->pitch, offset, len, lo, span, outside);
}

static void footprints(rsx_tile tile4)
{
    rsx_tiling_set(4, RSX_LOCATION_MAIN, tile4.offset, tile4.size, tile4.pitch, tile4.bank);
    footprint_case(&tile4, tile4.offset, 8192u * 351u + 1920u);          /* a retail title's 480x352 texture */
    footprint_case(&tile4, tile4.offset + 8192u * 100u + 64u, 8192u * 7u);
    footprint_case(&tile4, tile4.offset - 4096u, 8192u * 70u);           /* from before the tile */
    footprint_case(&tile4, tile4.offset + tile4.size - 8192u * 3u, 8192u * 5u);  /* past its end */
    rsx_tiling_set(4, 0, 0, 0, 0, 0);
    const rsx_tile p5120 = { 0x100000u, 0x280000u, 5120u, 1u };
    rsx_tiling_set(3, RSX_LOCATION_MAIN, p5120.offset, p5120.size, p5120.pitch, p5120.bank);
    footprint_case(&p5120, p5120.offset + 5120u * 64u * 3u + 640u, 5120u * 90u);
    footprint_case(&p5120, p5120.offset + 0x12345u, 0x777u);
    u32 lo, span;
    CHECK(!rsx_tiling_footprint(RSX_LOCATION_MAIN, 0x10000u, 0x1000u, &lo, &span) && lo == 0x10000u && span == 0x1000u,
          "a range off every tile is its own footprint");
    rsx_tiling_set(3, 0, 0, 0, 0, 0);
}

int main(void)
{
    static const u32 title_tile4[][2] = {
        { 0x123f52, 0x12beb2 }, { 0x2be4de, 0x2cc67e }, { 0x43526f, 0x47c3cf },
        { 0x448368, 0x434d08 }, { 0x55d763, 0x58d343 }, { 0x102334, 0xbaf34 } };
    static const u32 pitch2048[][2] = {
        { 0x172525, 0x176385 }, { 0x1d75ad, 0x1d2bcd }, { 0x1fb57d, 0x1f735d },
        { 0x1e1d73, 0x1f41f3 }, { 0x17ad8a, 0x1774aa }, { 0x1014c3, 0x1106c3 } };
    static const u32 pitch5120[][2] = {
        { 0x14aa4e, 0x168e8e }, { 0x1c593a, 0x1d2f5a }, { 0x498893, 0x47e8b3 },
        { 0x1919e9, 0x1a05a9 }, { 0x731c94, 0x750434 }, { 0x2f0ee, 0x3672e } };
    static const u32 pitch512[][2] = {
        { 0x4ec1b, 0x4b2db }, { 0x3838c, 0x3862c }, { 0x2ae8a, 0x296ea },
        { 0x42d80, 0x4d4c0 }, { 0x30319, 0x34239 }, { 0x3c800, 0x32080 } };
    const rsx_tile tile4 = { 0x30000u, 0x580000u, 8192u, 2u };
    vectors("pitch 8192, bank 2 (a retail title's tile 4)", tile4, title_tile4, 6);
    vectors("pitch 2048, bank 0", (rsx_tile){ 0x100000u, 0x100000u, 2048u, 0u }, pitch2048, 6);
    vectors("pitch 5120, bank 1", (rsx_tile){ 0x0u, 0x880000u, 5120u, 1u }, pitch5120, 6);
    vectors("pitch 512, bank 3", (rsx_tile){ 0x10000u, 0x40000u, 512u, 3u }, pitch512, 6);

    const u32 blocks = tile4.size / 32u;
    u8* seen = calloc(blocks, 1);
    u32 outside = 0, twice = 0, split = 0;
    for (u32 b = 0; b < blocks; b++) {
        const u32 lin = tile4.offset + b * 32u;
        const u32 a = rsx_tile_address(&tile4, lin);
        if (a == 0xFFFFFFFFu || a - tile4.offset >= tile4.size) { outside++; continue; }
        if (rsx_tile_address(&tile4, lin + 31u) != a + 31u) split++;
        if (seen[(a - tile4.offset) / 32u]++) twice++;
    }
    free(seen);
    CHECK(!outside && !twice && !split, "the tiling permutes the tile's 32-byte blocks (%u outside, %u twice, %u split)",
          outside, twice, split);

    CHECK(rsx_tile_address(&tile4, tile4.offset - 1u) == RSX_TILE_NO_ADDRESS, "a byte before the tile has no tiled address");

    ranges(tile4);
    footprints(tile4);
    printf("[rsx_tiling] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
