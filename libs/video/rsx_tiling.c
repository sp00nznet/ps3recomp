/*
 * RSX tiled main memory: the bound tiles and the range copies every RSX
 * client goes through (rsx_tiling.h).
 */
#include "rsx_tiling.h"
#include "rsx_dispatch.h"                     /* RSX_LOCATION_MAIN */
#include "../../runtime/platform/win32_compat.h"
#include "ps3emu/log_first.h"
#include <stdio.h>
#include <string.h>

/* The ranges below ask `ptr` for at most one IO page at a time: main memory
 * is IO-mapped in 1 MB pages, and each page is contiguous in guest memory. */
#define RSX_TILING_PAGE 0x100000u

/* Written from the PPU thread (cellGcmSetTile), read by the FIFO walker and
 * the draw engine. s_rsx_tiles_bound has a bit per bound tile, so a client checks it
 * without the lock while nothing is bound. */
static rsx_tile s_rsx_tiles[RSX_TILE_COUNT];
static SRWLOCK s_rsx_tiles_lock = SRWLOCK_INIT;
static volatile LONG s_rsx_tiles_bound;

void rsx_tiling_set(u32 index, u32 location, u32 offset, u32 size, u32 pitch, u32 bank)
{
    if (index >= RSX_TILE_COUNT) return;
    rsx_tile t = { 0, 0, 0, 0 };
    if (location == RSX_LOCATION_MAIN && size && pitch) {
        t.offset = offset; t.size = size; t.pitch = pitch; t.bank = bank & 3u;
    }
    AcquireSRWLockExclusive(&s_rsx_tiles_lock);
    s_rsx_tiles[index] = t;
    const LONG bit = (LONG)(1u << index);
    InterlockedExchange(&s_rsx_tiles_bound, t.size ? (s_rsx_tiles_bound | bit) : (s_rsx_tiles_bound & ~bit));
    ReleaseSRWLockExclusive(&s_rsx_tiles_lock);
}

int rsx_tiling_active(void)
{
    return s_rsx_tiles_bound != 0;
}

int rsx_tiling_find(u32 location, u32 offset, u32 len, rsx_tile* out)
{
    if (location != RSX_LOCATION_MAIN || !len || !s_rsx_tiles_bound) return 0;
    const u64 end = (u64)offset + len;
    int found = 0;
    AcquireSRWLockShared(&s_rsx_tiles_lock);
    for (u32 i = 0; i < RSX_TILE_COUNT; i++) {
        const rsx_tile* t = &s_rsx_tiles[i];
        if (!t->size || t->offset >= end || (u64)t->offset + t->size <= offset) continue;
        if (!found || t->offset < out->offset) { *out = *t; found = 1; }
    }
    ReleaseSRWLockShared(&s_rsx_tiles_lock);
    return found;
}

int rsx_tiling_footprint(u32 location, u32 offset, u32 len, u32* lo, u32* span)
{
    const u64 end = (u64)offset + len;
    u64 l = offset, h = end, pos = offset;
    int any = 0;
    rsx_tile t;
    while (pos < end && rsx_tiling_find(location, (u32)pos, (u32)(end - pos), &t)) {
        const u64 t_end = (u64)t.offset + t.size;
        const u64 band = 64ull * t.pitch;
        const u64 a = pos > t.offset ? pos : t.offset;
        const u64 b = end < t_end ? end : t_end;
        /* a 64-row band of tiles is stored over the 16 KB tiles' own span,
         * each tile in one of the four banks of its 64 KB group */
        u64 fl = t.offset + (a - t.offset) / band * band;
        u64 fh = t.offset + (b - t.offset + band - 1u) / band * band;
        fl &= ~0xFFFFull;
        fh = (fh + 0xFFFFu) & ~0xFFFFull;
        if (fl < t.offset) fl = t.offset;
        if (fh > t_end) fh = t_end;
        if (fl < l) l = fl;
        if (fh > h) h = fh;
        any = 1;
        pos = t_end;
    }
    *lo = (u32)l;
    *span = h - l > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)(h - l);
    return any;
}

static void rsx_tiling_put(const rsx_tiled_mem* mem, u8* dst, const u8* src, u32 n)
{
    if (mem->store) mem->store(mem->user, dst, src, n);
    else            memcpy(dst, src, n);
}

/* [pos, stop) as the bytes lie, an IO page at a time. */
static int rsx_tiling_linear(const rsx_tiled_mem* mem, u32 location, u64 pos, u64 stop,
                             u8* linear, int write)
{
    int all = 1;
    while (pos < stop) {
        const u64 page_end = (pos & ~(u64)(RSX_TILING_PAGE - 1u)) + RSX_TILING_PAGE;
        const u32 n = (u32)((stop < page_end ? stop : page_end) - pos);
        u8* p = mem->ptr(mem->user, location, (u32)pos, n);
        if (!p)         { if (!write) memset(linear, 0, n); all = 0; }
        else if (write) rsx_tiling_put(mem, p, linear, n);
        else            memcpy(linear, p, n);
        pos += n; linear += n;
    }
    return all;
}

/* [pos, stop) inside tile t, 32 bytes at a time through its tiling. The host
 * bytes of the IO page the last run landed in are kept for the next. */
static int rsx_tiling_tiled(const rsx_tiled_mem* mem, u32 location, const rsx_tile* t,
                            u64 pos, u64 stop, u8* linear, int write)
{
    if (rsx_tile_address(t, t->offset) == RSX_TILE_NO_ADDRESS) {
        static u32 logs;
        if (PS3_LOG_FIRST(logs, 4))
            fprintf(stderr, "[rsx tiling] tile 0x%X+0x%X pitch %u has no tiling; read as linear\n",
                    t->offset, t->size, t->pitch);
        return rsx_tiling_linear(mem, location, pos, stop, linear, write);
    }
    const u64 t_end = (u64)t->offset + t->size;
    u64 page = ~0ull, page_lo = 0;
    u8* host = NULL;
    int all = 1;
    while (pos < stop) {
        const u32 run = (u32)(32u - (pos & 31u) < stop - pos ? 32u - (pos & 31u) : stop - pos);
        const u32 a = rsx_tile_address(t, (u32)pos);
        if (a != RSX_TILE_NO_ADDRESS && (a & ~(RSX_TILING_PAGE - 1u)) != page) {
            page = a & ~(RSX_TILING_PAGE - 1u);
            page_lo = page > t->offset ? page : t->offset;
            const u64 page_hi = page + RSX_TILING_PAGE < t_end ? page + RSX_TILING_PAGE : t_end;
            host = mem->ptr(mem->user, location, (u32)page_lo, (u32)(page_hi - page_lo));
        }
        u8* p = (a != RSX_TILE_NO_ADDRESS && host) ? host + (a - page_lo) : NULL;
        if (!p)         { if (!write) memset(linear, 0, run); all = 0; }
        else if (write) rsx_tiling_put(mem, p, linear, run);
        else            memcpy(linear, p, run);
        pos += run; linear += run;
    }
    return all;
}

static int rsx_tiling_copy(const rsx_tiled_mem* mem, u32 location, u32 offset, u8* linear, u32 len,
                           int write)
{
    const u64 end = (u64)offset + len;
    u64 pos = offset;
    int all = 1;
    while (pos < end) {
        rsx_tile t;
        if (!rsx_tiling_find(location, (u32)pos, (u32)(end - pos), &t)) {
            all &= rsx_tiling_linear(mem, location, pos, end, linear, write);
            break;
        }
        if (t.offset > pos) {
            all &= rsx_tiling_linear(mem, location, pos, t.offset, linear, write);
            linear += t.offset - pos;
            pos = t.offset;
        }
        const u64 t_end = (u64)t.offset + t.size;
        const u64 stop = end < t_end ? end : t_end;
        all &= rsx_tiling_tiled(mem, location, &t, pos, stop, linear, write);
        linear += stop - pos;
        pos = stop;
    }
    return all;
}

int rsx_tiled_read(const rsx_tiled_mem* mem, u32 location, u32 offset, void* dst, u32 len)
{
    return rsx_tiling_copy(mem, location, offset, (u8*)dst, len, 0);
}

int rsx_tiled_write(const rsx_tiled_mem* mem, u32 location, u32 offset, const void* src, u32 len)
{
    return rsx_tiling_copy(mem, location, offset, (u8*)src, len, 1);
}
