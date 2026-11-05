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

u32 rsx_swizzle_offset_3d(u32 x, u32 y, u32 z, u32 log2w, u32 log2h, u32 log2d)
{
    u32 off = 0, shift = 0;
    while (log2w || log2h || log2d) {
        if (log2w) { off |= (x & 1u) << shift; x >>= 1; shift++; log2w--; }
        if (log2h) { off |= (y & 1u) << shift; y >>= 1; shift++; log2h--; }
        if (log2d) { off |= (z & 1u) << shift; z >>= 1; shift++; log2d--; }
    }
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

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    /* Linear A8R8G8B8 / D8R8G8B8 is a byte rotation per texel (guest A,R,G,B
     * to host R,G,B,A), which the compiler vectorizes; through decode_texel
     * it was most of the cost of re-uploading a full-screen render target
     * read back as a texture. Same bytes as decode_texel. */
    if (!tl->swizzled && (tl->base_format == 0x85u || tl->base_format == 0x9Eu) &&
        sbpp == 4u && dbpp == 4u) {
        const u32 alpha = tl->base_format == 0x9Eu ? 0xFF000000u : 0u;
        for (u32 y = 0; y < h; y++) {
            const u8* srow = src + (size_t)y * tl->row_bytes;
            u8* drow = d + (size_t)y * dst_pitch;
            for (u32 x = 0; x < w; x++) {
                u32 v;
                memcpy(&v, srow + (size_t)x * 4u, 4);
                if (!argb_as_rgba) v = (v >> 8) | (v << 24);
                v |= alpha;
                memcpy(drow + (size_t)x * 4u, &v, 4);
            }
        }
        return;
    }
#endif

    /* Morton order interleaves the low bits of x and y, so a swizzled image
     * has to be walked texel by texel; so does anything the host cannot take
     * as stored. x's and y's bits land on separate positions of the swizzled
     * offset, so it is the OR of a y part, computed once a row, and an x part
     * stepped with the dilated increment (d - mask) & mask. Interleaving every
     * texel's bits afresh was most of the decode in God of War HD's texture
     * bursts. */
    const u32 xmask = tl->swizzled ? rsx_swizzle_offset((1u << l2w) - 1u, 0, l2w, l2h) : 0;
    for (u32 y = 0; y < h; y++) {
        u8* drow = d + (size_t)y * dst_pitch;
        if (tl->swizzled) {
            const u32 yoff = rsx_swizzle_offset(0, y, l2w, l2h);
            u32 xoff = 0;
            for (u32 x = 0; x < w; x++) {
                decode_texel(tl, src + (size_t)(xoff | yoff) * sbpp,
                             drow + (size_t)x * dbpp, argb_as_rgba);
                xoff = (xoff - xmask) & xmask;
            }
        } else {
            const u8* srow = src + (size_t)y * tl->row_bytes;
            for (u32 x = 0; x < w; x++)
                decode_texel(tl, srow + (size_t)x * sbpp, drow + (size_t)x * dbpp, argb_as_rgba);
        }
    }
}

void rsx_texture_component_remap(u32 control1, u32 rsx_fmt, u8 out[4])
{
    if (!out) return;

    /* Source codes index the presented vector {A,R,G,B}. The uploaded resource
     * holds R,G,B,A at components 0..3, so A is component 3 and R,G,B are
     * 0,1,2 -- that is what lanes_argb says. G8B8 only has two real channels,
     * and the sampler presents them as {G,R,G,R}. B8 decodes to a one-channel
     * resource whose byte is presented as R, G and B with an opaque A, which
     * is what the D3D12 live engine's CPU decode builds (decode_texel); read
     * through lanes_argb its B source would be component 2, always zero.
     * X16 is one 16-bit channel decoded to R16 and is presented the same way:
     * God of War: Ascension samples its X16 font atlas through crossbar 0xAAEE
     * (every output from G or B), which read zeros -- all menu text invisible.
     * The depth formats decode to one channel and present it on every source
     * (RPCS3 VKFormats.cpp get_component_mapping: R,R,R,R): a retail title
     * reads its DEPTH16 shadow maps through crossbar 0xA900, R, G and B from A,
     * which through lanes_argb was the resource's opaque alpha -- every
     * shadow compare saw depth 1.0. */
    static const u8 lanes_argb[4]  = {3, 0, 1, 2};
    static const u8 lanes_g8b8[4]  = {1, 0, 1, 0};
    static const u8 lanes_b8[4]    = {RSX_REMAP_ONE, 0, 0, 0};
    static const u8 lanes_depth[4] = {0, 0, 0, 0};
    const u32 base = rsx_fmt & 0x9Fu;
    const u8* src2res = (base == 0x8Bu) ? lanes_g8b8
                      : (base == 0x81u || base == 0x94u) ? lanes_b8
                      : (base >= 0x90u && base <= 0x93u) ? lanes_depth : lanes_argb;

    if (!(control1 & 0xFFFFu)) control1 = 0xAAE4u;   /* unset -> identity */

    /* Y16_X16 (and HILO8): two 16-bit channels whose crossbar is encoded in
     * 16-bit blocks -- 0xE4 or 0x4E only -- and presented interleaved
     * R-G-R-G, or R-R-R-G when CONTROL1's high word (the remap override,
     * bit 16) is set (RPCS3 fragment_texture::decoded_remap, verified on
     * hardware with Virtua Fighter 5 / Mass Effect 3). Through the plain
     * ARGB table a .w read got the resource's empty alpha, 1.0, instead of
     * Y: inFAMOUS 2's water reads its 64x64 wave map through .yw that way. */
    if (base == 0x95u || base == 0x98u || base == 0x99u) {
        const int ovr = (control1 >> 16) & 0xFFFFu;
        u32 lo = control1 & 0xFFu;
        switch (lo) {
        case 0xE4: lo = ovr ? 0x56u : 0x66u; break;
        case 0x4E: lo = ovr ? 0xA9u : 0x99u; break;
        case 0xEE: lo = 0xAAu; break;
        case 0x44: lo = 0x55u; break;
        default: break;
        }
        control1 = (control1 & ~0xFFu) | lo;
    }

    /* i runs A, R, G, B -- the crossbar's field order, LSB first. */
    for (int i = 0; i < 4; i++) {
        u32 s  = (control1 >> (i * 2)) & 3u;
        u32 op = (control1 >> (8 + i * 2)) & 3u;
        out[i] = (op == 0) ? (u8)RSX_REMAP_ZERO
               : (op == 1) ? (u8)RSX_REMAP_ONE
                           : src2res[s];
    }
}

/* ADDRESS gamma flags name final RGBA lanes, after remapping. Signed input
 * interpretation wins; its FILTER ARGB flags follow the source crossbar. */
u8 rsx_texture_gamma_mask(u32 format, u32 control1, u32 filter, u32 address)
{
    switch (format & 0x9Fu) {
    case 0x81: case 0x82: case 0x83: case 0x84: case 0x85:
    case 0x86: case 0x87: case 0x88: case 0x8B: case 0x8D: case 0x8E: case 0x8F:
    case 0x97: case 0x9D: case 0x9E: break;
    default: return 0;
    }
    u8 gamma = (u8)((address >> 20) & 15u);
    if (!gamma) return 0;
    if (!(control1 & 0xFFFFu)) control1 = 0xAAE4u;
    u8 remap[4]; rsx_texture_component_remap(control1, format, remap);
    static const u8 rgba_lane[4] = {3, 0, 1, 2};
    for (u32 i = 0; i < 4; ++i) {
        const u32 source = (control1 >> (2u * i)) & 3u;
        if (remap[i] < RSX_REMAP_ZERO && ((filter >> (28u + source)) & 1u))
            gamma &= (u8)~(1u << rgba_lane[i]);
    }
    return gamma;
}

u8 rsx_texture_expand_mask(u32 format, u32 control1, u32 filter, u32 address)
{
    /* RPCS3 rsx::get_format_features, RSX_FORMAT_FEATURE_BIASED_NORMALIZATION:
     * the 8-bit colour formats, the depth formats, X16, Y16_X16 and HILO8.
     * Floats and the 32-bit depth-as-colour reads have nothing to expand. */
    switch (format & 0x9Fu) {
    case 0x81: case 0x82: case 0x83: case 0x84: case 0x85:
    case 0x86: case 0x87: case 0x88: case 0x8B: case 0x8D: case 0x8E: case 0x8F:
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x98:
    case 0x97: case 0x9D: case 0x9E: break;
    default: return 0;
    }
    if (!((address >> 12) & 1u)) return 0;          /* UNSIGNED_REMAP_NORMAL */
    if (!(control1 & 0xFFFFu)) control1 = 0xAAE4u;
    u8 remap[4]; rsx_texture_component_remap(control1, format, remap);
    static const u8 rgba_lane[4] = {3, 0, 1, 2};
    u8 mask = 0;
    for (u32 i = 0; i < 4; ++i) {
        const u32 source = (control1 >> (2u * i)) & 3u;
        if (remap[i] < RSX_REMAP_ZERO && !((filter >> (28u + source)) & 1u))
            mask |= (u8)(1u << rgba_lane[i]);
    }
    /* Bit 4: 16-bit channels (X16, Y16_X16) re-centre on 32768/32767. */
    if (mask && ((format & 0x9Fu) == 0x94u || (format & 0x9Fu) == 0x95u)) mask |= 0x10u;
    return mask;
}
