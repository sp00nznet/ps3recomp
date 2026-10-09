/*
 * ps3recomp - cellVdec's decoder back end (see vdec_ffmpeg.h)
 */

#include "vdec_ffmpeg.h"
#include "cellVdec.h"
#include "ffmpeg_runtime.h"
#include "../guest_struct.h"   /* vm_memcpy_to */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Picture store: I420 -> the guest's requested format
 * -----------------------------------------------------------------------*/

static inline u8 clamp_u8(int v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }

void vdec_store_picture(u32 dst, const u8* yuv, u32 w, u32 h,
                        u32 format_type, u32 color_matrix, u8 alpha)
{
    const u32 cw = (w + 1) / 2, ch = (h + 1) / 2;
    const u8* py = yuv;
    const u8* pu = yuv + (size_t)w * h;
    const u8* pv = pu + (size_t)cw * ch;

    if (format_type >= 3) {                           /* YUV420 planar */
        vm_memcpy_to(dst, yuv, (size_t)w * h + 2 * (size_t)cw * ch);
        return;
    }

    u8* row = (u8*)malloc((size_t)w * 4);
    if (!row) return;
    if (format_type == 2) {                            /* UYVY422: U Y0 V Y1 */
        for (u32 y = 0; y < h; y++) {
            const u8* ly = py + (size_t)y * w;
            const u8* lu = pu + (size_t)(y / 2) * cw;
            const u8* lv = pv + (size_t)(y / 2) * cw;
            for (u32 x = 0; x < w; x += 2) {
                row[x * 2 + 0] = lu[x / 2];
                row[x * 2 + 1] = ly[x];
                row[x * 2 + 2] = lv[x / 2];
                row[x * 2 + 3] = (x + 1 < w) ? ly[x + 1] : ly[x];
            }
            vm_memcpy_to(dst + y * w * 2, row, (size_t)w * 2);
        }
        free(row);
        return;
    }

    /* ARGB32 (bytes A R G B) / RGBA32 (R G B A), limited-range input.
     * Coefficients are the BT.601 / BT.709 ones scaled by 65536. */
    const int bt709 = color_matrix == 1;
    const int cy = 76309;                              /* 255/219 */
    const int rv = bt709 ? 117489 : 104597;
    const int gu = bt709 ?  13954 :  25675;
    const int gv = bt709 ?  34925 :  53279;
    const int bu = bt709 ? 138438 : 132201;
    const int ao = format_type == 0 ? 0 : 3;           /* alpha byte */
    const int ro = format_type == 0 ? 1 : 0;           /* R G B follow */
    for (u32 y = 0; y < h; y++) {
        const u8* ly = py + (size_t)y * w;
        const u8* lu = pu + (size_t)(y / 2) * cw;
        const u8* lv = pv + (size_t)(y / 2) * cw;
        for (u32 x = 0; x < w; x++) {
            int yy = (ly[x] - 16) * cy + 32768;
            int u = lu[x / 2] - 128, v = lv[x / 2] - 128;
            u8* p = row + x * 4;
            p[ao]     = alpha;
            p[ro + 0] = clamp_u8((yy + rv * v) >> 16);
            p[ro + 1] = clamp_u8((yy - gu * u - gv * v) >> 16);
            p[ro + 2] = clamp_u8((yy + bu * u) >> 16);
        }
        vm_memcpy_to(dst + y * w * 4, row, (size_t)w * 4);
    }
    free(row);
}

#ifdef PS3RECOMP_FFMPEG_RUNTIME

/* ---------------------------------------------------------------------------
 * Decoder
 * -----------------------------------------------------------------------*/

struct VdecFf {
    const PS3FFmpeg* ff;
    AVCodecContext* ctx;
    AVPacket* pkt;
    AVFrame* frame;
    u8* buf;          /* the AU plus FFmpeg's zeroed input padding */
    u32 buf_cap;
};

VdecFf* vdec_ff_open(u32 codec_type)
{
    enum AVCodecID id;
    if (codec_type == CELL_VDEC_CODEC_TYPE_AVC)        id = AV_CODEC_ID_H264;
    else if (codec_type == CELL_VDEC_CODEC_TYPE_MPEG2) id = AV_CODEC_ID_MPEG2VIDEO;
    else return NULL;

    const PS3FFmpeg* ff = ps3_ffmpeg_get();
    if (!ff) return NULL;
    const AVCodec* codec = ff->avcodec_find_decoder(id);
    if (!codec) {
        printf("[cellVdec] libavcodec has no decoder for codecType %u\n", codec_type);
        return NULL;
    }
    VdecFf* d = (VdecFf*)calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->ff = ff;
    d->ctx = ff->avcodec_alloc_context3(codec);
    d->pkt = ff->av_packet_alloc();
    d->frame = ff->av_frame_alloc();
    if (d->ctx) {
        /* One thread: pictures come out of the call that decoded them (or
         * the next ones, for reordering) rather than a frame-thread later. */
        d->ctx->thread_count = 1;
    }
    if (!d->ctx || !d->pkt || !d->frame || ff->avcodec_open2(d->ctx, codec, NULL) < 0) {
        printf("[cellVdec] could not open the libavcodec decoder\n");
        vdec_ff_close(d);
        return NULL;
    }
    return d;
}

void vdec_ff_close(VdecFf* d)
{
    if (!d) return;
    if (d->ctx) d->ff->avcodec_free_context(&d->ctx);
    if (d->pkt) d->ff->av_packet_free(&d->pkt);
    if (d->frame) d->ff->av_frame_free(&d->frame);
    free(d->buf);
    free(d);
}

/* Hand every picture the decoder has ready to emit. */
static int vdec_ff_receive(VdecFf* d, vdec_ff_emit_fn emit, void* cb_ctx)
{
    for (;;) {
        AVFrame* f = d->frame;
        int r = d->ff->avcodec_receive_frame(d->ctx, f);
        if (r < 0)
            return r;                                  /* EAGAIN / EOF / error */
        if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P) {
            /* The PS3 decoders are 8-bit 4:2:0 only. */
            printf("[cellVdec] skipping a picture in pixel format %d\n", f->format);
            d->ff->av_frame_unref(f);
            continue;
        }
        VdecFfPicture pic;
        memset(&pic, 0, sizeof(pic));
        u32 w = (u32)f->width, h = (u32)f->height;
        u32 cw = (w + 1) / 2, ch = (h + 1) / 2;
        pic.yuv = (u8*)malloc((size_t)w * h + 2 * (size_t)cw * ch);
        if (pic.yuv) {
            u8* o = pic.yuv;
            for (u32 y = 0; y < h; y++, o += w)
                memcpy(o, f->data[0] + (size_t)y * f->linesize[0], w);
            for (int p = 1; p <= 2; p++)
                for (u32 y = 0; y < ch; y++, o += cw)
                    memcpy(o, f->data[p] + (size_t)y * f->linesize[p], cw);
            pic.width = w;
            pic.height = h;
            pic.token = (u32)(f->pts != AV_NOPTS_VALUE ? f->pts : f->best_effort_timestamp);
            pic.pict_type = (int)f->pict_type;
            pic.key = (f->flags & AV_FRAME_FLAG_KEY) != 0;
            emit(cb_ctx, &pic);                        /* takes pic.yuv */
        }
        d->ff->av_frame_unref(f);
    }
}

int vdec_ff_decode(VdecFf* d, const u8* au, u32 size, u32 token,
                   vdec_ff_emit_fn emit, void* cb_ctx)
{
    if (size + AV_INPUT_BUFFER_PADDING_SIZE > d->buf_cap) {
        u8* nb = (u8*)realloc(d->buf, size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!nb) return AVERROR(ENOMEM);
        d->buf = nb;
        d->buf_cap = size + AV_INPUT_BUFFER_PADDING_SIZE;
    }
    memcpy(d->buf, au, size);
    memset(d->buf + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    AVPacket* pkt = d->pkt;
    pkt->data = d->buf;
    pkt->size = (int)size;
    pkt->pts = token;              /* comes back on the picture, reordered */
    pkt->dts = AV_NOPTS_VALUE;

    int r = d->ff->avcodec_send_packet(d->ctx, pkt);
    if (r == AVERROR(EAGAIN)) {
        /* The decoder is full: take its pictures, then it accepts the AU. */
        vdec_ff_receive(d, emit, cb_ctx);
        r = d->ff->avcodec_send_packet(d->ctx, pkt);
    }
    pkt->data = NULL;
    pkt->size = 0;
    if (r < 0) {
        printf("[cellVdec] AU %u did not decode (%d)\n", token, r);
        vdec_ff_receive(d, emit, cb_ctx);
        return r;
    }
    vdec_ff_receive(d, emit, cb_ctx);
    return 0;
}

void vdec_ff_drain(VdecFf* d, vdec_ff_emit_fn emit, void* cb_ctx)
{
    d->ff->avcodec_send_packet(d->ctx, NULL);
    vdec_ff_receive(d, emit, cb_ctx);
    d->ff->avcodec_flush_buffers(d->ctx);   /* leave EOF so it decodes again */
}

void vdec_ff_reset(VdecFf* d)
{
    d->ff->avcodec_flush_buffers(d->ctx);
}

#else /* !PS3RECOMP_FFMPEG_RUNTIME */

VdecFf* vdec_ff_open(u32 codec_type) { (void)codec_type; return NULL; }
void vdec_ff_close(VdecFf* d) { (void)d; }
int  vdec_ff_decode(VdecFf* d, const u8* au, u32 size, u32 token,
                    vdec_ff_emit_fn emit, void* ctx)
{ (void)d; (void)au; (void)size; (void)token; (void)emit; (void)ctx; return -1; }
void vdec_ff_drain(VdecFf* d, vdec_ff_emit_fn emit, void* ctx)
{ (void)d; (void)emit; (void)ctx; }
void vdec_ff_reset(VdecFf* d) { (void)d; }

#endif /* PS3RECOMP_FFMPEG_RUNTIME */
