/*
 * ps3recomp - cellVdec's decoder back end (libavcodec, loaded at run time)
 *
 * cellVdec.c owns the guest-facing state (handles, callbacks, the picture
 * queue, pic items); this file turns one access unit into zero or more
 * decoded pictures, and stores a picture into guest memory in the format a
 * title asks for. Without PS3RECOMP_FFMPEG_RUNTIME, or when FFmpeg is not
 * found, vdec_ff_open returns NULL and cellVdec keeps its callback-only path.
 */

#ifndef PS3RECOMP_VDEC_FFMPEG_H
#define PS3RECOMP_VDEC_FFMPEG_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One decoded picture, I420 with no padding: w*h luma, then the
 * ((w+1)/2)*((h+1)/2) U plane, then V. */
typedef struct VdecFfPicture {
    u8* yuv;          /* malloc'd; the receiver owns it */
    u32 width;
    u32 height;
    u32 token;        /* the token passed with the AU this picture came from */
    int pict_type;    /* 1 I, 2 P, 3 B, 0 unknown (FFmpeg's AVPictureType) */
    int key;          /* keyframe (IDR for H.264) */
} VdecFfPicture;

typedef void (*vdec_ff_emit_fn)(void* ctx, const VdecFfPicture* pic);

typedef struct VdecFf VdecFf;

/* A decoder for a cellVdec codec type (CELL_VDEC_CODEC_TYPE_AVC / _MPEG2),
 * or NULL when there is none to be had. */
VdecFf* vdec_ff_open(u32 codec_type);
void    vdec_ff_close(VdecFf* d);

/* Decode one AU, calling emit for each picture that comes out (display
 * order, so possibly none, or one from an earlier AU). Returns 0, or a
 * negative FFmpeg error for a damaged AU; the decoder stays usable. */
int  vdec_ff_decode(VdecFf* d, const u8* au, u32 size, u32 token,
                    vdec_ff_emit_fn emit, void* ctx);

/* End of sequence: emit every picture still held for reordering, then reset
 * so the next sequence starts clean. */
void vdec_ff_drain(VdecFf* d, vdec_ff_emit_fn emit, void* ctx);

/* Drop everything held and start over (cellVdecStartSeq). */
void vdec_ff_reset(VdecFf* d);

/* Store an I420 picture at guest address dst as cellVdecGetPicture's
 * formatType (0 ARGB32, 1 RGBA32, 2 UYVY422, 3 YUV420 planar) with
 * colorMatrixType (0 BT.601, 1 BT.709) and the given alpha. */
void vdec_store_picture(u32 dst, const u8* yuv, u32 w, u32 h,
                        u32 format_type, u32 color_matrix, u8 alpha);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_VDEC_FFMPEG_H */
