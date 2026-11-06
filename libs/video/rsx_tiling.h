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
 * The address math is envytools' model of the NV40-family tiling,
 * tile_translate_addr() in nvhw/tile.c
 * (https://github.com/envytools/envytools/blob/f102b82381f3f11cee113d16374c87091db039d9/nvhw/tile.c),
 * PFB_NV41 path, with the RSX's memory configuration: G7x, VRAM mode,
 * mcbits 3, partbits 1, colbits_lo 10 (blocks of 256 bytes x 64 rows, 16 KB),
 * partshift 7, the tile's bank as bankoff. Four in-block bits differ from that
 * model on the RSX: bit 4 carries over, and bits 7, 9 and 10 are the hashes
 * below instead of the NV41 partition bit and its G7x undo. Those bits and
 * the block grid anchored at the tile's first 16 KB block are the RSX's
 * layout as a retail title's depth-of-field job reads and writes it. The pitches
 * are envytools' (tile_pitch_valid), the list cellGcmSetTile takes. The tile
 * table and the range copies below (rsx_tiling.c) let every client go through
 * it.
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
 * Bits 0-4 carry over, so 32 aligned linear bytes stay together.
 *
 * Derived from envytools' tile_translate_addr (see the top of this file):
 *
 * Copyright (C) 2012 Marcelina Kościelnicka <mwk@0x04.net>
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE. */
static inline u32 rsx_tile_address(const rsx_tile* t, u32 lin)
{
    const u32 pitch = t->pitch;
    const u32 cols = pitch >> 8;               /* 256-byte columns per row */
    /* the pitches cellGcmSetTile takes: 512 to 64 KB, factor << (8 + shift) */
    if ((pitch & 0xffu) || cols < 2u || cols > 0x100u || lin < t->offset) return RSX_TILE_NO_ADDRESS;
    u32 shift = 0;
    while (!(cols >> shift & 1u)) shift++;
    switch (cols >> shift) {
    case 1: case 3: case 5: case 7: case 13: break;
    default: return RSX_TILE_NO_ADDRESS;
    }

    const u32 a = lin - t->offset;
    const u32 x = a % pitch, y = a / pitch;
    const u32 ix = lin & 0xffu, iy = y & 0x3fu; /* byte and row inside the 16 KB block */
    const u32 bx = x >> 8, by = y >> 6;         /* block column and block row */
    const u32 base = t->offset >> 14;           /* 16 KB block of the tile's start */
    const u32 block = base + by * cols + bx;

    /* The bank (block bits 0-1) is swizzled by column unless the pitch is an
     * odd multiple of 256, then offset by the tile's bank. */
    const u32 col = base + bx;
    u32 bank = shift ? ((col >> 1 & 1u) | (col << 1 & 2u)) + by : block;
    bank += t->bank;

    const u32 in = (ix & 0x1fu)
        | (iy & 3u) << 5
        | ((iy >> 2 ^ ix >> 6) & 1u) << 7
        | (ix >> 5 & 1u) << 8
        | ((ix >> 6 ^ iy >> 4 ^ block) & 1u) << 9
        | ((ix >> 7 ^ iy >> 3) & 1u) << 10
        | (iy >> 3) << 11;
    const u32 r = in | ((block & ~3u) | (bank & 3u)) << 14;
    return (r - t->offset < t->size) ? r : RSX_TILE_NO_ADDRESS;
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
