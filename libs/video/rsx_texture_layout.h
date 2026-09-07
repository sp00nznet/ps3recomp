/*
 * ps3recomp - RSX texture layout: format class, sizes, and swizzle addressing
 *
 * Working out what shape a guest texture is -- how many bytes a row takes, how
 * many rows there are, whether the texels are Morton-ordered, where texel
 * (x, y) actually lives -- is RSX semantics. It has nothing to do with the host
 * graphics API, which only needs the answers.
 *
 * It lived inside rsx_d3d12_backend.c's uploader, which is why the Metal
 * backend has no textures at all: a second backend cannot sample a guest
 * texture without first re-deriving all of this. Pulling it out is what makes
 * that possible without a third copy (see rsx_vertex_fetch.h for how the
 * second copy of the vertex fetch worked out).
 *
 * Scope is the formats the live draw engine handles, plus the ones a backend
 * can take natively. Everything else falls back to one byte per texel, exactly
 * as it did before, so an unrecognised format degrades rather than reading out
 * of bounds.
 */
#ifndef PS3RECOMP_RSX_TEXTURE_LAYOUT_H
#define PS3RECOMP_RSX_TEXTURE_LAYOUT_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Host-neutral format class: what the DECODED image is, which is not always
 * what the guest stored. A backend maps these to its own enum -- DXGI,
 * MTLPixelFormat, VkFormat -- rather than each deriving them from the RSX
 * format byte.
 *
 * Two rules decide whether a guest format keeps its bits or is converted.
 * Anything the host has a matching format for keeps them, byte-swapped out of
 * the guest's big-endian order. The packed 16-bit colour formats do not: RSX
 * numbers their fields from the high bit of a big-endian half-word, which is
 * neither of the orders D3D12 and Metal offer, so they are unpacked to RGBA8
 * on the CPU rather than fed to a host 5:6:5 or 5:5:5:1 format that would read
 * their channels rotated. The live draw engine unpacks them the same way. */
typedef enum {
    RSX_TEXFMT_R8 = 0,   /* B8, and the fallback for anything unrecognised */
    RSX_TEXFMT_R8G8,     /* G8B8, COMPRESSED_HILO8, HILO_S8                */
    RSX_TEXFMT_R8G8B8A8, /* A8R8G8B8, D8R8G8B8, and the packed 16-bit ones */
    RSX_TEXFMT_BC1,      /* DXT1                                           */
    RSX_TEXFMT_BC2,      /* DXT23                                          */
    RSX_TEXFMT_BC3,      /* DXT45                                          */
    RSX_TEXFMT_R16,      /* X16 and DEPTH16: one 16-bit unorm channel      */
    RSX_TEXFMT_R16G16,   /* Y16_X16: two 16-bit unorm channels             */
    RSX_TEXFMT_R16G16F,  /* Y16_X16_FLOAT                                  */
    RSX_TEXFMT_R16G16B16A16F, /* W16_Z16_Y16_X16_FLOAT                     */
    RSX_TEXFMT_R32F,     /* X32_FLOAT, and DEPTH24_D8 converted to it      */
    RSX_TEXFMT_R32G32B32A32F  /* W32_Z32_Y32_X32_FLOAT                     */
} rsx_texfmt;

typedef struct {
    rsx_texfmt fmt;
    u32  base_format;     /* SET_TEXTURE_FORMAT byte with LN/UN masked off   */
    int  compressed;      /* block-compressed: rows are rows of 4x4 blocks   */
    int  swizzled;        /* texels are Morton/Z-ordered, not row-major      */
    u32  bytes_per_texel; /* SOURCE bytes per texel; 0 when compressed       */
    u32  block_bytes;     /* 0 when not compressed (8 for BC1, else 16)      */
    u32  row_bytes;       /* SOURCE row stride: one texel row, or block row  */
    u32  rows;            /* texel rows, or block rows                       */
    u32  face_bytes;      /* row_bytes * rows: one face, one mip level       */
    u32  dst_bytes_per_texel; /* DECODED bytes per texel; 0 when compressed  */
    u32  dst_row_bytes;   /* DECODED row, tightly packed                     */
} rsx_tex_layout;

/* Classify `rsx_fmt` (the NV4097 SET_TEXTURE_FORMAT byte) at `w` x `h`.
 *
 * `swizzled` is set when the LN bit (0x20) is clear, the format is not
 * compressed, and both dimensions are powers of two -- the hardware only
 * swizzles under those conditions, and compressed data is never swizzled.
 * Treating a swizzled texture as linear is what produced diagonal-stripe
 * garbage on LBP's loading screen, so the flag is load-bearing.
 *
 * The source and the decoded image are described separately because they are
 * not always the same shape: a packed 16-bit texel is two bytes in guest
 * memory and four on the host, so `row_bytes` sizes the read and
 * `dst_row_bytes` the write. They are equal for every format that keeps its
 * bits. */
void rsx_texture_layout(u32 rsx_fmt, u32 w, u32 h, rsx_tex_layout* out);

/* As rsx_texture_layout(), with SET_TEXTURE_CONTROL3's row pitch.
 *
 * A LINEAR texture whose pitch register is set stores its rows that many bytes
 * apart rather than w * bytes_per_texel; swizzled and compressed images have no
 * pitch and ignore it, as the live draw engine does. A pitch narrower than one
 * texel row is ignored too: it cannot describe the image, and honouring it
 * would make face_bytes too small to cover the bytes the decode then reads. */
void rsx_texture_layout_pitched(u32 rsx_fmt, u32 w, u32 h, u32 pitch,
                                rsx_tex_layout* out);

/* --- mip chains and cube faces -------------------------------------------
 *
 * A guest texture is not one image. SET_TEXTURE_FORMAT's level count says how
 * many mip levels follow level 0, packed one after another, and bit 2 says the
 * whole thing is repeated for six cube faces. Where each of those starts is
 * arithmetic on the format and the dimensions, so it lives here with the rest
 * of the layout rather than in each backend.
 */

/* 1x1 is 13 halvings below 4096, the largest texture the hardware takes; the
 * live draw engine caps its own level arrays at 14 for the same reason. */
#define RSX_MAX_TEXTURE_LEVELS 14

typedef struct {
    u32 w, h;            /* texel dimensions of this level              */
    u32 offset;          /* byte offset from the start of the face      */
    rsx_tex_layout tl;   /* this level's own layout                     */
} rsx_tex_level;

/* Describe the mip chain of a `w` x `h` `rsx_fmt` texture into `out`, which
 * holds RSX_MAX_TEXTURE_LEVELS entries. Returns how many levels were written:
 * `levels` (SET_TEXTURE_FORMAT's count, 0 read as 1) clamped to the levels the
 * dimensions can actually produce, since a texture cannot have more levels
 * than halvings down to 1x1.
 *
 * A swizzled texture's levels are each Morton-ordered within themselves, which
 * falls out of classifying every level separately: halving a power of two
 * stays a power of two, so a swizzled level 0 has swizzled levels below it.
 * `pitch` is SET_TEXTURE_CONTROL3's row pitch and applies to level 0 only --
 * the levels below it are packed at their own width. */
u32 rsx_texture_mip_chain(u32 rsx_fmt, u32 w, u32 h, u32 levels, u32 pitch,
                          rsx_tex_level* out);

/* Stride from one cube face to the next.
 *
 * The face stride is NOT one level-0 image: RSX stores each face as its own
 * complete mip pyramid, aligned to 128 bytes. Assuming mip-0-sized strides
 * makes face 1 land inside face 0's mip chain, which is what the D3D12
 * backend's dumps showed before it was fixed -- every face after the first a
 * progressively smaller copy of the first. */
u32 rsx_texture_cube_face_stride(u32 rsx_fmt, u32 w, u32 h, u32 levels,
                                 u32 pitch);

/* Byte offset of texel (x, y) within a swizzled (Morton-ordered) image whose
 * dimensions are 2^log2w by 2^log2h, in texels -- multiply by bytes_per_texel.
 * Interleaves the low bits of x and y, then appends whichever axis still has
 * bits left when the other is exhausted (non-square images). */
u32 rsx_swizzle_offset(u32 x, u32 y, u32 log2w, u32 log2h);

/* ceil(log2(v)); 0 for v <= 1. */
u32 rsx_log2_ceil(u32 v);

/* Convert one face/level of a guest texture into host-ready rows.
 *
 *   dst       destination, at least dst_pitch * tl->rows bytes
 *   dst_pitch destination row stride (the host API's alignment,
 *             >= dst_row_bytes)
 *   src       guest bytes for this face, already resolved to a host pointer
 *   w, h      texel dimensions
 *   tl        layout from rsx_texture_layout() for the same w/h/format
 *   argb_as_rgba  see rsx_texture_argb_is_rgba()
 *
 * Undoes Morton ordering where the layout says the source is swizzled, and
 * puts every channel where the host expects it: A8R8G8B8's guest byte order
 * (A,R,G,B) becomes R,G,B,A, the packed 16-bit formats are unpacked to RGBA8,
 * DEPTH24_D8's integer depth becomes a float, and anything wider than a byte
 * that the host takes natively is swapped out of big-endian. Compressed
 * formats are copied block-row by block-row without touching the payload,
 * because BC1/2/3 are bit-identical to DXT1/23/45.
 *
 * Output is always tightly packed within each row, so a backend only supplies
 * the destination and its pitch -- it needs to know nothing about swizzling,
 * channel order, endianness or block sizes.
 */
void rsx_texture_decode(void* dst, u32 dst_pitch,
                        const u8* src, u32 w, u32 h,
                        const rsx_tex_layout* tl, int argb_as_rgba);

/* --- component remap (NV4097 TEXTURE_CONTROL1 crossbar) ------------------
 *
 * Each of the texture's four outputs is either a channel of the sampled
 * resource or a forced constant. These are the selector values
 * rsx_texture_component_remap() writes.
 *
 * 0..3 index the UPLOADED RESOURCE's components, which rsx_texture_decode()
 * always lays out as R,G,B,A -- BC1/2/3 decode to RGBA the same way, so both
 * paths agree. (D3D12 happens to encode force-zero and force-one as 4 and 5
 * too, so its backend can pass these through; that is a convenience, not the
 * reason for the values.) */
#define RSX_REMAP_ZERO 4u   /* force 0.0 */
#define RSX_REMAP_ONE  5u   /* force 1.0 */

/* Decode TEXTURE_CONTROL1's crossbar for `rsx_fmt`.
 *
 * `out` receives the selector for each output IN THE CROSSBAR'S OWN FIELD
 * ORDER: out[0] = A, out[1] = R, out[2] = G, out[3] = B. A backend reorders
 * them into whatever its API wants.
 *
 * Layout of control1: the low byte is the crossbar, two bits per output,
 * selecting the source from the presented vector {A,R,G,B}; the next byte is
 * the per-output operation, 0 = force zero, 1 = force one, 2 = use the
 * crossbar. A control word of 0 means "unset" and is treated as the hardware
 * identity, 0xAAE4.
 *
 * The field order is load-bearing and has been got wrong before. Running the
 * fields backwards (B,G,R,A) makes 0xAAE4 -- the documented identity -- decode
 * to a channel rotation, and makes 0xAA1B look like the identity instead. That
 * bug was then papered over by bending the DXT lane table to cancel it, so DXT
 * sampled correctly while every A8R8G8B8 texture came back permuted: Rubber
 * Ducky's bump maps returned (R,A,A) and drove the scene's magenta and green
 * casts. test_texture_layout.c asserts 0xAAE4 is the identity and 0xAA1B is
 * not, which is precisely that regression. */
void rsx_texture_component_remap(u32 control1, u32 rsx_fmt, u8 out[4]);

/* TEX_RGBA: treat A8R8G8B8 source bytes as already R,G,B,A and copy them
 * straight through.
 *
 * PSGL uploads its converted textures as GL_RGBA/GL_UNSIGNED_INT_8_8_8_8,
 * which on the big-endian PPU lays the bytes down R,G,B,A even though the GCM
 * format field says A8R8G8B8 -- and the format field alone cannot tell the two
 * apart. Off by default: what looked like a channel rotation turned out to be
 * the TEXTURE_CONTROL1 crossbar being read backwards, and with the crossbar
 * decoded correctly this applies the rotation a second time.
 *
 * Read once and cached. Lives here rather than in each backend so they cannot
 * disagree about it. */
int rsx_texture_argb_is_rgba(void);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_TEXTURE_LAYOUT_H */
