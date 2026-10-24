/*
 * RSX tiled main memory.
 *
 * A tile bound over main memory (cellGcmSetTile with location MAIN) lays the
 * RSX's view of that memory out in 256-byte x 64-row tiles, with banks and
 * partitions interleaved. Every RSX client sees memory through that mapping:
 * render targets, textures, the 2D engines (NV3089, NV309E, NV308A), NV0039
 * buffer copies, vertex, index and program fetches. The PPU and the SPUs see
 * the bytes where they lie. Local memory tiles are transparent to every
 * client and need nothing. A retail title downsamples its scene into tiled main
 * memory with NV3089 for an SPU depth-of-field job while aiming; the job reads
 * and writes the tiled layout, and sampled linearly its output showed as
 * bands (rows 0-15 of every 64).
 *
 * The address math is a port of RPCS3's rsx::tiled_dma_copy
 * (https://github.com/RPCS3/rpcs3/blob/06ae1af55161e8a9e8ad77395a97455d493b5cb3/rpcs3/Emu/RSX/Common/tiled_dma_copy.hpp),
 * itself from envytools' NV40 VRAM notes
 * (https://envytools.readthedocs.io/en/latest/hw/memory/vram.html).
 * RPCS3 takes the tile's effective address as the base; the location offset
 * used here differs from it only above bit 19 (IO pages are 1 MB), and those
 * bits cancel in the result. RPCS3 goes through the tiling for NV3089
 * destinations and texture reads only; the tile table and the range copies
 * below (rsx_tiling.c) let every client do it.
 */
#ifndef RSX_TILING_H
#define RSX_TILING_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_TILE_COUNT 15u
#define RSX_TILE_NO_ADDRESS 0xFFFFFFFFu

typedef struct rsx_tile {
    u32 offset;   /* location offset of the tile's first byte */
    u32 size;
    u32 pitch;
    u32 bank;
} rsx_tile;

/* Where the RSX keeps the byte at linear location offset `lin` of tile `t`:
 * a location offset, or RSX_TILE_NO_ADDRESS when it falls outside the tile.
 * Bits 0-4 carry over, so 32 aligned linear bytes stay together. */
static inline u32 rsx_tile_address(const rsx_tile* t, u32 lin)
{
    static const u32 bank_lut[16] = { 0, 1, 2, 3, 2, 3, 0, 1, 1, 2, 3, 0, 3, 0, 1, 2 };
    const u32 base = t->offset;
    /* tiles per row: pitch / 256 = prime * factor, prime the odd part */
    u32 factor = t->pitch >> 8;
    if (t->pitch & (t->pitch - 1u)) {
        static const u32 primes[5] = { 3, 5, 7, 11, 13 };
        u32 k = 0;
        while (k < 5u && factor % primes[k]) k++;
        if (k == 5u) return RSX_TILE_NO_ADDRESS;
        factor /= primes[k];
    }
    const u32 per_row = t->pitch >> 8;
    if (!per_row || lin < base) return RSX_TILE_NO_ADDRESS;

    const u32 texel = (lin - base) / 256u;
    const u32 tile_x = texel % per_row;
    const u32 tile_y = (texel / per_row) / 64u;
    const u32 tile_id = tile_y * per_row + tile_x;
    const u32 sel = (tile_id + (base >> 14)) & 0x3ffffu;
    const u32 row_address = (sel >> 2) & 0xffffu;

    u32 bank = 0;
    if (factor == 1u)      bank = sel & 3u;
    else if (factor == 2u) bank = bank_lut[((sel + ((tile_y & 1u) << 1)) & 3u) * 4u + (tile_y & 3u)];
    else if (factor >= 4u) bank = bank_lut[(sel & 3u) * 4u + (tile_y & 3u)];
    bank = (bank + t->bank) & 3u;

    const u32 line = (texel / per_row) % 64u;
    u32 column = 0;
    column |= ((line >> 3) & 7u) << 7;
    column |= ((lin >> 5) & 7u) << 4;
    column |= (line & 3u) << 2;
    const u32 partition = (((line >> 2) & 1u) + ((lin >> 6) & 1u)) & 1u;

    u32 a = (row_address << 16) | (bank << 14) | (((column >> 4) & 0x3fu) << 8) |
            (partition << 7) | (((column >> 2) & 3u) << 5) | (lin & 0x1fu);
    a ^= (((a >> 12) ^ ((bank ^ sel) & 1u) ^ (a >> 14)) & 1u) << 9;
    a ^= ((a >> 11) & 1u) << 10;
    return (a - base < t->size) ? a : RSX_TILE_NO_ADDRESS;
}

/* ---- the bound tiles (rsx_tiling.c) ----------------------------------------
 * cellGcmSetTile / BindTile / UnbindTile publish here; any thread may look
 * up. Only main-memory tiles are kept. */

/* Tile `index` now covers [offset, offset + size) at `location`
 * (RSX_LOCATION_*); a zero size or pitch, or a local location, unbinds it. */
void rsx_tiling_set(u32 index, u32 location, u32 offset, u32 size, u32 pitch, u32 bank);

/* Nonzero while any main-memory tile is bound: the one check a client pays
 * when none is. */
int rsx_tiling_active(void);

/* The bound tile that overlaps [offset, offset + len) at `location` and
 * starts lowest, copied to *out. 0 when none does. */
int rsx_tiling_find(u32 location, u32 offset, u32 len, rsx_tile* out);

/* The guest bytes [*lo, *lo + *span) the RSX's view of [offset, offset +
 * len) is made of: the range itself, widened over each tile it touches to
 * whole 64-row bands rounded out to the 64 KB bank groups the band's tiles
 * are interleaved over. A change detector that hashes those bytes sees every
 * write that changes what the RSX reads. Returns 0 (and the range itself)
 * when no tile is touched. */
int rsx_tiling_footprint(u32 location, u32 offset, u32 len, u32* lo, u32* span);

/* How a client reaches guest memory. `ptr` returns the host bytes of
 * [offset, offset + len) at `location`, or NULL; the range copies below ask
 * for no more than one 1 MB IO page at a time, so a caller whose IO pages
 * map scattered in host memory is still read right. `store`, when not NULL,
 * replaces memcpy for every write (a client that must tell the SPUs their
 * reservations were hit). */
typedef struct rsx_tiled_mem {
    u8* (*ptr)(void* user, u32 location, u32 offset, u32 len);
    void (*store)(void* user, u8* dst, const u8* src, u32 len);
    void* user;
} rsx_tiled_mem;

/* Copy [offset, offset + len) at `location` as the RSX sees it into `dst`,
 * or `src` into it: through the tiling where a bound tile covers it, as the
 * bytes lie elsewhere. The range may start before a tile, cross several and
 * end past them. A read zero-fills what `ptr` cannot reach, a write skips
 * it; both return 1 when every byte was reached. */
int rsx_tiled_read(const rsx_tiled_mem* mem, u32 location, u32 offset, void* dst, u32 len);
int rsx_tiled_write(const rsx_tiled_mem* mem, u32 location, u32 offset, const void* src, u32 len);

#ifdef __cplusplus
}
#endif

#endif /* RSX_TILING_H */
