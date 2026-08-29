/*
 * ps3recomp - RSX texture layout (see rsx_texture_layout.h)
 */
#include "rsx_texture_layout.h"

#include <stdlib.h>   /* getenv, atoi */
#include <string.h>   /* memcpy       */

u32 rsx_log2_ceil(u32 v)
{
    u32 l = 0;
    while ((1u << l) < v) l++;
    return l;
}

u32 rsx_swizzle_offset(u32 x, u32 y, u32 log2w, u32 log2h)
{
    u32 off = 0, shift = 0;
    while (log2w && log2h) {
        off |= (x & 1u) << shift; x >>= 1; shift++;
        off |= (y & 1u) << shift; y >>= 1; shift++;
        log2w--; log2h--;
    }
    off |= (x | y) << shift;     /* only one of x/y still has bits */
    return off;
}

void rsx_texture_layout(u32 rsx_fmt, u32 w, u32 h, rsx_tex_layout* out)
{
    if (!out) return;

    /* Format classes (base = fmt & 0x9F, masking off the LN/UN flag bits).
     * The LBP loading screen exercises all of them: 0x85 A8R8G8B8 (swizzled UI
     * art), 0x8B G8B8 (the 1024x2048 linear font atlas -- without it no text
     * renders at all), 0x86/87/88 DXT1/23/45 (512x512 detail and LUT layers
     * bound on every draw). */
    u32 basef = rsx_fmt & 0x9Fu;

    out->compressed     = 0;
    out->block_bytes    = 0;
    out->bytes_per_texel = 1;
    out->fmt            = RSX_TEXFMT_R8;

    switch (basef) {
    case 0x85: out->fmt = RSX_TEXFMT_R8G8B8A8; out->bytes_per_texel = 4; break;
    case 0x8B: out->fmt = RSX_TEXFMT_R8G8;     out->bytes_per_texel = 2; break;
    case 0x86: out->fmt = RSX_TEXFMT_BC1; out->compressed = 1; out->block_bytes = 8;  break;
    case 0x87: out->fmt = RSX_TEXFMT_BC2; out->compressed = 1; out->block_bytes = 16; break;
    case 0x88: out->fmt = RSX_TEXFMT_BC3; out->compressed = 1; out->block_bytes = 16; break;
    default:   break;   /* R8, one byte per texel -- the pre-existing fallback */
    }

    if (out->compressed) {
        /* Compressed data is stored as linear rows of 4x4 blocks, and is never
         * Morton-swizzled on RSX. */
        out->bytes_per_texel = 0;
        out->swizzled = 0;
        out->row_bytes = ((w + 3u) / 4u) * out->block_bytes;
        out->rows      = (h + 3u) / 4u;
    } else {
        /* Swizzled unless the LN bit is set. The hardware requires power-of-two
         * dimensions to swizzle, so a NPOT image is linear regardless. */
        out->swizzled  = !(rsx_fmt & 0x20u) &&
                         w && h &&
                         (w & (w - 1u)) == 0u && (h & (h - 1u)) == 0u;
        out->row_bytes = w * out->bytes_per_texel;
        out->rows      = h;
    }

    out->face_bytes = out->row_bytes * out->rows;
}

int rsx_texture_argb_is_rgba(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char* e = getenv("TEX_RGBA");
        cached = e ? atoi(e) : 0;
    }
    return cached;
}

void rsx_texture_decode(void* dst, u32 dst_pitch,
                        const u8* src, u32 w, u32 h,
                        const rsx_tex_layout* tl, int argb_as_rgba)
{
    if (!dst || !src || !tl || !w || !h) return;

    u8* d = (u8*)dst;

    if (tl->compressed) {
        /* BC1/2/3 are bit-identical to DXT1/23/45, so the payload is copied
         * rather than converted -- only the row stride changes. */
        for (u32 y = 0; y < tl->rows; y++)
            memcpy(d + (size_t)y * dst_pitch,
                   src + (size_t)y * tl->row_bytes, tl->row_bytes);
        return;
    }

    /* Morton order interleaves the low bits of x and y, so a swizzled image has
     * to be walked texel by texel. A linear one is a straight row copy, which is
     * worth keeping as a separate path: it is the common case and by far the
     * faster one. */
    const u32 l2w = rsx_log2_ceil(w), l2h = rsx_log2_ceil(h);
    const u32 bpp = tl->bytes_per_texel;

    if (tl->fmt == RSX_TEXFMT_R8G8B8A8) {
        for (u32 y = 0; y < h; y++) {
            u8* drow = d + (size_t)y * dst_pitch;
            for (u32 x = 0; x < w; x++) {
                const u8* s = src + (size_t)(tl->swizzled
                                  ? rsx_swizzle_offset(x, y, l2w, l2h)
                                  : (size_t)y * w + x) * 4;
                if (argb_as_rgba) {
                    drow[x*4+0] = s[0]; drow[x*4+1] = s[1];
                    drow[x*4+2] = s[2]; drow[x*4+3] = s[3];
                } else {
                    /* guest A,R,G,B -> host R,G,B,A */
                    drow[x*4+0] = s[1]; drow[x*4+1] = s[2];
                    drow[x*4+2] = s[3]; drow[x*4+3] = s[0];
                }
            }
        }
        return;
    }

    /* R8G8 and R8: no channel reordering. G8B8's placement is done by the
     * sampler's component remap, not here. */
    for (u32 y = 0; y < h; y++) {
        u8* drow = d + (size_t)y * dst_pitch;
        if (tl->swizzled) {
            for (u32 x = 0; x < w; x++) {
                const u8* s = src + (size_t)rsx_swizzle_offset(x, y, l2w, l2h) * bpp;
                for (u32 b = 0; b < bpp; b++) drow[x * bpp + b] = s[b];
            }
        } else {
            memcpy(drow, src + (size_t)y * w * bpp, (size_t)w * bpp);
        }
    }
}

void rsx_texture_component_remap(u32 control1, u32 rsx_fmt, u8 out[4])
{
    if (!out) return;

    /* Source codes index the presented vector {A,R,G,B}. The uploaded resource
     * holds R,G,B,A at components 0..3, so A is component 3 and R,G,B are
     * 0,1,2 -- that is what lanes_argb says. G8B8 only has two real channels,
     * and the sampler presents them as {G,R,G,R}. */
    static const u8 lanes_argb[4] = {3, 0, 1, 2};
    static const u8 lanes_g8b8[4] = {1, 0, 1, 0};
    const u8* src2res = ((rsx_fmt & 0x9Fu) == 0x8Bu) ? lanes_g8b8 : lanes_argb;

    if (!(control1 & 0xFFFFu)) control1 = 0xAAE4u;   /* unset -> identity */

    /* i runs A, R, G, B -- the crossbar's field order, LSB first. */
    for (int i = 0; i < 4; i++) {
        u32 s  = (control1 >> (i * 2)) & 3u;
        u32 op = (control1 >> (8 + i * 2)) & 3u;
        out[i] = (op == 0) ? (u8)RSX_REMAP_ZERO
               : (op == 1) ? (u8)RSX_REMAP_ONE
                           : src2res[s];
    }
}
