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
    rsx_texture_layout_pitched(rsx_fmt, w, h, 0, out);
}

void rsx_texture_layout_pitched(u32 rsx_fmt, u32 w, u32 h, u32 pitch,
                                rsx_tex_layout* out)
{
    if (!out) return;

    /* Format classes (base = fmt & 0x9F, masking off the LN/UN flag bits).
     * The LBP loading screen exercises the first group: 0x85 A8R8G8B8
     * (swizzled UI art), 0x8B G8B8 (the 1024x2048 linear font atlas -- without
     * it no text renders at all), 0x86/87/88 DXT1/23/45 (512x512 detail and
     * LUT layers bound on every draw). The rest are what the live draw engine
     * reached for once a real title's frame was being drawn. */
    u32 basef = rsx_fmt & 0x9Fu;
    u32 src_bpp = 1, dst_bpp = 1;

    out->base_format    = basef;
    out->compressed     = 0;
    out->block_bytes    = 0;
    out->fmt            = RSX_TEXFMT_R8;

    switch (basef) {
    /* Four bytes, A,R,G,B. D8R8G8B8 is the same image with no alpha channel:
     * the byte is there but means nothing, so the decode forces it to one
     * rather than letting whatever the guest left behind modulate a draw. */
    case 0x85: case 0x9E:
        out->fmt = RSX_TEXFMT_R8G8B8A8; src_bpp = dst_bpp = 4; break;
    /* Two 8-bit channels. HILO is a normal-map pair; HILO_S8's channels are
     * signed, which is not modelled -- neither reference backend samples it,
     * so there is nothing to mirror and unorm is the conservative reading. */
    case 0x8B: case 0x8C: case 0x8D:
        out->fmt = RSX_TEXFMT_R8G8;     src_bpp = dst_bpp = 2; break;
    /* Packed 16-bit colour: two source bytes unpacked to four host ones. */
    case 0x82: case 0x83: case 0x84: case 0x97: case 0x9D:
        out->fmt = RSX_TEXFMT_R8G8B8A8; src_bpp = 2; dst_bpp = 4; break;
    case 0x86: out->fmt = RSX_TEXFMT_BC1; out->compressed = 1; out->block_bytes = 8;  break;
    case 0x87: out->fmt = RSX_TEXFMT_BC2; out->compressed = 1; out->block_bytes = 16; break;
    case 0x88: out->fmt = RSX_TEXFMT_BC3; out->compressed = 1; out->block_bytes = 16; break;
    /* Depth read as colour. Both sample as a single red channel: a shader
     * reading one of these wants the depth value, and spreading it across RGB
     * would only make the crossbar's job ambiguous. DEPTH24_D8's 24-bit
     * integer has no host format, so it is normalised into a float, which is
     * exact -- 24 bits fit a float32 mantissa. */
    case 0x90: out->fmt = RSX_TEXFMT_R32F;  src_bpp = dst_bpp = 4; break;
    case 0x92: case 0x94:
        out->fmt = RSX_TEXFMT_R16;      src_bpp = dst_bpp = 2; break;
    case 0x95: out->fmt = RSX_TEXFMT_R16G16; src_bpp = dst_bpp = 4; break;
    /* Float render targets sampled back as textures. Host formats take these
     * bit for bit; only the guest's big-endian component order changes. */
    case 0x9A: out->fmt = RSX_TEXFMT_R16G16B16A16F; src_bpp = dst_bpp = 8;  break;
    case 0x9B: out->fmt = RSX_TEXFMT_R32G32B32A32F; src_bpp = dst_bpp = 16; break;
    case 0x9C: out->fmt = RSX_TEXFMT_R32F;   src_bpp = dst_bpp = 4; break;
    case 0x9F: out->fmt = RSX_TEXFMT_R16G16F; src_bpp = dst_bpp = 4; break;
    default:   break;   /* R8, one byte per texel -- the pre-existing fallback */
    }

    if (out->compressed) {
        /* Compressed data is stored as linear rows of 4x4 blocks, and is never
         * Morton-swizzled on RSX. */
        out->bytes_per_texel = 0;
        out->swizzled = 0;
        out->row_bytes = ((w + 3u) / 4u) * out->block_bytes;
        out->rows      = (h + 3u) / 4u;
        out->dst_bytes_per_texel = 0;
        out->dst_row_bytes       = out->row_bytes;
    } else {
        /* Swizzled unless the LN bit is set. The hardware requires power-of-two
         * dimensions to swizzle, so a NPOT image is linear regardless. */
        out->swizzled  = !(rsx_fmt & 0x20u) &&
                         w && h &&
                         (w & (w - 1u)) == 0u && (h & (h - 1u)) == 0u;
        out->bytes_per_texel = src_bpp;
        out->row_bytes = w * src_bpp;
        /* A swizzled image has no rows to space out, so the pitch register
         * only applies to a linear one. */
        if (pitch > out->row_bytes && (rsx_fmt & 0x20u))
            out->row_bytes = pitch;
        out->rows      = h;
        out->dst_bytes_per_texel = dst_bpp;
        out->dst_row_bytes       = w * dst_bpp;
    }

    out->face_bytes = out->row_bytes * out->rows;
}

u32 rsx_texture_mip_chain(u32 rsx_fmt, u32 w, u32 h, u32 levels, u32 pitch,
                          rsx_tex_level* out)
{
    if (!out || !w || !h) return 0;
    if (levels == 0) levels = 1;
    if (levels > RSX_MAX_TEXTURE_LEVELS) levels = RSX_MAX_TEXTURE_LEVELS;

    u32 mw = w, mh = h, off = 0, n = 0;
    while (n < levels) {
        rsx_tex_level* lv = &out[n];
        lv->w = mw; lv->h = mh; lv->offset = off;
        /* Only level 0 has a pitch register; the rest are packed at their own
         * width, as the live draw engine's texture_source_span walks them. */
        rsx_texture_layout_pitched(rsx_fmt, mw, mh, n == 0 ? pitch : 0, &lv->tl);
        off += lv->tl.face_bytes;
        n++;
        if (mw == 1 && mh == 1) break;   /* no level below 1x1 */
        if (mw > 1) mw >>= 1;
        if (mh > 1) mh >>= 1;
    }
    return n;
}

u32 rsx_texture_cube_face_stride(u32 rsx_fmt, u32 w, u32 h, u32 levels,
                                 u32 pitch)
{
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 n = rsx_texture_mip_chain(rsx_fmt, w, h, levels, pitch, lv);
    if (!n) return 0;
    const u32 total = lv[n - 1].offset + lv[n - 1].tl.face_bytes;
    return (total + 127u) & ~127u;
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

/* A 5- or 6-bit field widened to 8 bits, and a 4-bit one, exactly as the live
 * draw engine's decode_texel widens them. */
static u8 expand5(u32 v) { return (u8)(v * 255u / 31u); }
static u8 expand6(u32 v) { return (u8)(v * 255u / 63u); }
static u8 expand4(u32 v) { return (u8)(v * 17u); }

/* Guest half-words and words are big-endian. */
static u32 be16(const u8* p) { return ((u32)p[0] << 8) | p[1]; }
static u32 be32(const u8* p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* `n` components of `width` bytes, big-endian in guest memory and
 * little-endian on every host this runs on. */
static void swap_components(u8* d, const u8* s, u32 n, u32 width)
{
    for (u32 c = 0; c < n; c++)
        for (u32 b = 0; b < width; b++)
            d[c * width + b] = s[c * width + (width - 1u - b)];
}

/* One source texel at `s` to one host texel at `d`, which receives
 * tl->dst_bytes_per_texel bytes. */
static void decode_texel(const rsx_tex_layout* tl, const u8* s, u8* d,
                         int argb_as_rgba)
{
    switch (tl->base_format) {
    case 0x85:            /* A8R8G8B8 */
    case 0x9E:            /* D8R8G8B8: the alpha byte is not a channel */
        if (argb_as_rgba) {
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        } else {
            /* guest A,R,G,B -> host R,G,B,A */
            d[0] = s[1]; d[1] = s[2]; d[2] = s[3]; d[3] = s[0];
        }
        if (tl->base_format == 0x9E) d[3] = 255;
        return;

    /* Packed 16-bit colour. RSX numbers these fields from the high bit of a
     * big-endian half-word, so the half-word is read that way and each field
     * widened to a byte. */
    case 0x82: case 0x9D: {   /* A1R5G5B5, and D1R5G5B5 with no alpha */
        const u32 v = be16(s);
        d[0] = expand5((v >> 10) & 0x1Fu);
        d[1] = expand5((v >>  5) & 0x1Fu);
        d[2] = expand5( v        & 0x1Fu);
        d[3] = (tl->base_format == 0x9D) ? 255u
             : (u8)((v & 0x8000u) ? 255u : 0u);
        return;
    }
    case 0x83: {              /* A4R4G4B4 */
        const u32 v = be16(s);
        d[0] = expand4((v >> 8) & 0xFu);
        d[1] = expand4((v >> 4) & 0xFu);
        d[2] = expand4( v       & 0xFu);
        d[3] = expand4((v >> 12) & 0xFu);
        return;
    }
    case 0x84: {              /* R5G6B5 */
        const u32 v = be16(s);
        d[0] = expand5((v >> 11) & 0x1Fu);
        d[1] = expand6((v >>  5) & 0x3Fu);
        d[2] = expand5( v        & 0x1Fu);
        d[3] = 255;
        return;
    }
    case 0x97: {              /* R5G5B5A1: alpha is the LOW bit, not the high */
        const u32 v = be16(s);
        d[0] = expand5((v >> 11) & 0x1Fu);
        d[1] = expand5((v >>  6) & 0x1Fu);
        d[2] = expand5((v >>  1) & 0x1Fu);
        d[3] = (u8)((v & 1u) ? 255u : 0u);
        return;
    }

    case 0x90: {              /* DEPTH24_D8: depth in the top 24 bits */
        const float f = (float)(be32(s) >> 8) / 16777215.0f;
        memcpy(d, &f, sizeof f);
        return;
    }

    default:
        break;
    }

    /* Everything left keeps its bits. One byte per component needs nothing
     * done to it; wider components are swapped out of big-endian. */
    switch (tl->fmt) {
    case RSX_TEXFMT_R16:             swap_components(d, s, 1, 2); return;
    case RSX_TEXFMT_R16G16:
    case RSX_TEXFMT_R16G16F:         swap_components(d, s, 2, 2); return;
    case RSX_TEXFMT_R16G16B16A16F:   swap_components(d, s, 4, 2); return;
    case RSX_TEXFMT_R32F:            swap_components(d, s, 1, 4); return;
    case RSX_TEXFMT_R32G32B32A32F:   swap_components(d, s, 4, 4); return;
    default:
        for (u32 b = 0; b < tl->dst_bytes_per_texel; b++) d[b] = s[b];
        return;
    }
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

    const u32 l2w = rsx_log2_ceil(w), l2h = rsx_log2_ceil(h);
    const u32 sbpp = tl->bytes_per_texel, dbpp = tl->dst_bytes_per_texel;

    /* R8 and R8G8 reach the host byte for byte. A linear one is then a
     * straight row copy, which is worth keeping as a separate path: it is the
     * common case (Bink video planes, the G8B8 font atlas) and by far the
     * faster one. G8B8's channel placement is the sampler's component remap,
     * not this. */
    if ((tl->fmt == RSX_TEXFMT_R8 || tl->fmt == RSX_TEXFMT_R8G8) && !tl->swizzled) {
        for (u32 y = 0; y < h; y++)
            memcpy(d + (size_t)y * dst_pitch,
                   src + (size_t)y * tl->row_bytes, (size_t)w * sbpp);
        return;
    }

    /* Morton order interleaves the low bits of x and y, so a swizzled image
     * has to be walked texel by texel; so does anything the host cannot take
     * as stored. */
    for (u32 y = 0; y < h; y++) {
        u8* drow = d + (size_t)y * dst_pitch;
        for (u32 x = 0; x < w; x++) {
            const u8* s = tl->swizzled
                ? src + (size_t)rsx_swizzle_offset(x, y, l2w, l2h) * sbpp
                : src + (size_t)y * tl->row_bytes + (size_t)x * sbpp;
            decode_texel(tl, s, drow + (size_t)x * dbpp, argb_as_rgba);
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
