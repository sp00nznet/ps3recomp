/*
 * ps3recomp - FFmpeg, loaded at run time
 *
 * The codec modules (cellVdec today; cellAdec, cellDmux later) decode through
 * libavcodec when the host has it. It is never linked: the library is opened
 * with dlopen / LoadLibrary the first time a decoder is created, and only the
 * handful of functions the modules call are resolved into a table. So
 *
 *   - a build without PS3RECOMP_FFMPEG_RUNTIME has no FFmpeg code at all and
 *     every codec module keeps its callback-only behaviour;
 *   - a build with it, run where FFmpeg is absent, behaves the same way;
 *   - FFmpeg stays a separate, user-supplied LGPL library.
 *
 * Only the FFmpeg headers are needed at build time (-DPS3RECOMP_FFMPEG_RUNTIME
 * =ON). The modules read AVFrame/AVPacket fields directly, and FFmpeg only
 * keeps those layouts stable within a major version, so the library opened is
 * the major the headers describe (libavcodec.so.63 / avcodec-63.dll for
 * FFmpeg 9.0) and is rejected if avcodec_version() reports another one.
 *
 * Run-time switches:
 *   PS3RECOMP_FFMPEG=0          never load FFmpeg (callback-only behaviour)
 *   PS3RECOMP_FFMPEG_AVCODEC=p  open libavcodec from path p instead
 *   PS3RECOMP_FFMPEG_AVUTIL=p   open libavutil from path p instead
 */

#ifndef PS3RECOMP_FFMPEG_RUNTIME_H
#define PS3RECOMP_FFMPEG_RUNTIME_H

#ifdef PS3RECOMP_FFMPEG_RUNTIME

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PS3FFmpeg {
    unsigned (*avcodec_version)(void);
    const AVCodec* (*avcodec_find_decoder)(enum AVCodecID id);
    AVCodecContext* (*avcodec_alloc_context3)(const AVCodec* codec);
    int  (*avcodec_open2)(AVCodecContext* ctx, const AVCodec* codec, AVDictionary** opts);
    void (*avcodec_free_context)(AVCodecContext** ctx);
    int  (*avcodec_send_packet)(AVCodecContext* ctx, const AVPacket* pkt);
    int  (*avcodec_receive_frame)(AVCodecContext* ctx, AVFrame* frame);
    void (*avcodec_flush_buffers)(AVCodecContext* ctx);
    AVPacket* (*av_packet_alloc)(void);
    void (*av_packet_free)(AVPacket** pkt);
    unsigned (*avutil_version)(void);
    AVFrame* (*av_frame_alloc)(void);
    void (*av_frame_free)(AVFrame** frame);
    void (*av_frame_unref)(AVFrame* frame);
    void (*av_log_set_level)(int level);
} PS3FFmpeg;

/* The function table, loading FFmpeg on the first call. NULL when the build
 * option is off, PS3RECOMP_FFMPEG=0, or no usable library was found; the
 * outcome is printed once and then cached. */
const PS3FFmpeg* ps3_ffmpeg_get(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_FFMPEG_RUNTIME */

#endif /* PS3RECOMP_FFMPEG_RUNTIME_H */
