/*
 * ps3recomp - cellAdec HLE implementation
 *
 * Audio decoder. Accepts AU data and delivers AUDONE + PCMOUT callbacks.
 * AC3 decodes through FFmpeg loaded at run time (PS3RECOMP_FFMPEG_RUNTIME,
 * ffmpeg_runtime.c, the loader cellVdec uses) into interleaved big-endian
 * float PCM, one 1536-sample frame per AU in the stream's own channels.
 * Every other codec, and AC3 when FFmpeg is absent or PS3RECOMP_FFMPEG=0,
 * delivers silence of the codec's frame size.
 */

#include "cellAdec.h"
#include <stdio.h>
#include <stdlib.h>   /* getenv -- an implicit decl returns int, truncating the pointer */
#include <string.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */
#include "../../runtime/ppu/ppu_context.h" /* g_active_ctx -> the guest lr */
#include "../guest_struct.h"   /* GUEST_EA, guest_struct_load/store */
#include "ps3emu/guest_call.h" /* g_ps3_guest_caller -- cbFunc is a GUEST OPD */
#include "../../runtime/memory/vm.h"     /* VM_HLE_INJECT_BASE */
#include "ps3emu/log_first.h"
#include "ffmpeg_runtime.h"   /* PS3FFmpeg; empty without PS3RECOMP_FFMPEG_RUNTIME */

/* CellAdecPcmItem as the guest reads it, from RPCS3 Modules/cellAdec.h:339.
 * ps1_netemu copies the whole thing out with six 8-byte loads from offsets
 * 0x00..0x28 (at 0x000ED964), and that copy is what confirms these offsets
 * rather than a guess about padding: auInfo lands at 0x18, not 0x14, because it
 * contains a u64. The block must be 0x30 readable GUEST bytes. */
#define PCMITEM_PCM_HANDLE   0x00u
#define PCMITEM_STATUS       0x04u
#define PCMITEM_START_ADDR   0x08u
#define PCMITEM_SIZE         0x0Cu
#define PCMITEM_BSI_INFO     0x10u
#define PCMITEM_AU_START     0x18u
#define PCMITEM_AU_SIZE      0x1Cu
#define PCMITEM_AU_PTS_HI    0x20u
#define PCMITEM_AU_PTS_LO    0x24u
#define PCMITEM_AU_USERDATA  0x28u
#define PCMITEM_BYTES        0x30u

/* PCM per AU, float (4 bytes a sample), interleaved.
 *  - AC3: one A/52 frame is 1536 samples (6 blocks of 256) in the frame's own
 *    channels (acmod + LFE from its header: 6 for a 5.1 track). A movie
 *    player that sizes its ring as channels x 4 bytes a sample and takes each
 *    AU's PCM size from bsiInfo+0x14 needs the block to be the real frame:
 *    1024 stereo samples (8 KB) fed its 5.1 mixer a third of the audio an AU
 *    stands for.
 *  - the other codecs: 1024 stereo float samples of silence, the ATRAC3 frame
 *    the first title on this stub (codecType 5) decodes. */
#define ADEC_PCM_SAMPLES  1024u
#define ADEC_PCM_BYTES    (ADEC_PCM_SAMPLES * 2u * 4u)
#define ADEC_AC3_SAMPLES  1536u
/* The largest block one AU produces here: an AC3 frame in 7.1. */
#define ADEC_PCM_MAX_BYTES (ADEC_AC3_SAMPLES * 8u * 4u)

/* Guest-visible scratch, in the HLE inject window rather than from the guest's
 * own heap. Taking 8 KB out of that heap in cellAdecOpen stalled the title
 * before it even reached cellAdecStartSeq -- ps1_netemu allocates its own
 * buffers from the same bump allocator, and perturbing it is not worth it for
 * two fixed-size blocks whose lifetime is the whole process.
 *
 * +0x40000 is clear of everything else in the window: labels +0x0000, control
 * +0x2000, callback +0x2F00, the offset tables +0x3000/+0x5000, and sys_rsx's
 * device/driver-info/reports pages at +0x30000/+0x31000/+0x38000. */
#define ADEC_SCRATCH_BASE   (VM_HLE_INJECT_BASE + 0x40000u)
#define ADEC_SCRATCH_STRIDE 0x4000u
#define ADEC_ITEM_EA(h)     (ADEC_SCRATCH_BASE + (u32)(h) * ADEC_SCRATCH_STRIDE)
/* The PCM block is up to 48 KB (ADEC_PCM_MAX_BYTES), more than the 16 KB
 * stride holds, so it lives in its own 64 KB per handle at +0x80000..+0xC0000,
 * past cellVdec's pic items at +0x50000. */
#define ADEC_PCM_EA(h)      (VM_HLE_INJECT_BASE + 0x80000u + (u32)(h) * 0x10000u)
/* The codec info block the PcmItem's bsiInfo points to, in the gap between
 * the 0x30-byte item and the PCM buffer. */
#define ADEC_BSI_EA(h)      (ADEC_ITEM_EA(h) + 0x40u)
#define ADEC_BSI_BYTES      0x40u

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/
#define MAX_ADEC 4

typedef struct {
    int in_use;
    u32 codecType;
    u32 cbFunc;         /* guest EA of the callback's OPD */
    u32 cbArg;          /* guest EA handed back to it     */
    int seqStarted;
    u32 itemEa;         /* guest EA of the 0x30-byte CellAdecPcmItem */
    u32 pcmEa;          /* guest EA of the PCM buffer it points at    */
    int hasPcm;
    u32 pcmBytes;       /* size of the PCM block at pcmEa */
    u32 auCount;        /* total AUs decoded */
    void* ff;           /* AdecFf: the libavcodec decoder, or NULL */
    u8* au;             /* host copy of the AU being decoded */
    u32 auCap;
} AdecSlot;

static AdecSlot s_adec[MAX_ADEC];

/* ---------------------------------------------------------------------------
 * AC3 frame header: the frame's channel count (A/52 5.4.2), for the size of
 * the silent block when there is no decoder. 0 when it is not an AC3 frame.
 * -----------------------------------------------------------------------*/
static u32 adec_ac3_channels(const u8* f, u32 n)
{
    static const u8 nfchans[8] = { 2, 1, 2, 3, 3, 4, 4, 5 };
    if (!f || n < 8 || f[0] != 0x0B || f[1] != 0x77) return 0;
    if ((f[5] >> 3) > 10) return 0;          /* bsid > 10: E-AC3, not parsed */
    u32 bits = ((u32)f[6] << 8) | f[7];      /* acmod(3) [cmixlev surmixlev dsurmod] lfeon */
    u32 acmod = bits >> 13;
    int pos = 3;
    if ((acmod & 1u) && acmod != 1u) pos += 2;   /* cmixlev */
    if (acmod & 4u) pos += 2;                    /* surmixlev */
    if (acmod == 2u) pos += 2;                   /* dsurmod */
    return nfchans[acmod] + ((bits >> (15 - pos)) & 1u);
}

/* ---------------------------------------------------------------------------
 * libavcodec back end (AC3)
 * -----------------------------------------------------------------------*/
#ifdef PS3RECOMP_FFMPEG_RUNTIME

typedef struct AdecFf {
    const PS3FFmpeg* ff;
    AVCodecContext* ctx;
    AVPacket* pkt;
    AVFrame* frame;
    u8* buf;            /* the AU plus FFmpeg's zeroed input padding */
    u32 bufCap;
    u32* pcm;           /* big-endian float words, ADEC_PCM_MAX_BYTES */
} AdecFf;

static void adec_ff_close(void* p)
{
    AdecFf* d = (AdecFf*)p;
    if (!d) return;
    if (d->ctx) d->ff->avcodec_free_context(&d->ctx);
    if (d->pkt) d->ff->av_packet_free(&d->pkt);
    if (d->frame) d->ff->av_frame_free(&d->frame);
    free(d->buf);
    free(d->pcm);
    free(d);
}

static void* adec_ff_open(u32 codec_type)
{
    if (codec_type != CELL_ADEC_TYPE_AC3) return NULL;
    const PS3FFmpeg* ff = ps3_ffmpeg_get();
    if (!ff) return NULL;
    const AVCodec* codec = ff->avcodec_find_decoder(AV_CODEC_ID_AC3);
    if (!codec) {
        printf("[cellAdec] libavcodec has no AC3 decoder; silence\n");
        return NULL;
    }
    AdecFf* d = (AdecFf*)calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->ff = ff;
    d->ctx = ff->avcodec_alloc_context3(codec);
    d->pkt = ff->av_packet_alloc();
    d->frame = ff->av_frame_alloc();
    d->pcm = (u32*)malloc(ADEC_PCM_MAX_BYTES);
    if (!d->ctx || !d->pkt || !d->frame || !d->pcm ||
        ff->avcodec_open2(d->ctx, codec, NULL) < 0) {
        printf("[cellAdec] could not open the libavcodec AC3 decoder; silence\n");
        adec_ff_close(d);
        return NULL;
    }
    return d;
}

static void adec_ff_flush(void* p)
{
    AdecFf* d = (AdecFf*)p;
    if (d) d->ff->avcodec_flush_buffers(d->ctx);
}

static float adec_ff_sample(const AVFrame* f, int ch, int c, int s)
{
    const u8* const* x = (const u8* const*)f->extended_data;
    switch (f->format) {
    case AV_SAMPLE_FMT_FLTP: return ((const float*)x[c])[s];
    case AV_SAMPLE_FMT_FLT:  return ((const float*)x[0])[s * ch + c];
    case AV_SAMPLE_FMT_S16P: return ((const int16_t*)x[c])[s] * (1.0f / 32768.0f);
    case AV_SAMPLE_FMT_S16:  return ((const int16_t*)x[0])[s * ch + c] * (1.0f / 32768.0f);
    case AV_SAMPLE_FMT_S32P: return (float)((const int32_t*)x[c])[s] * (1.0f / 2147483648.0f);
    case AV_SAMPLE_FMT_S32:  return (float)((const int32_t*)x[0])[s * ch + c] * (1.0f / 2147483648.0f);
    default:                 return 0.0f;
    }
}

/* Decode one AU and store its PCM at guest dst as interleaved big-endian
 * float, frame after frame. Returns the bytes stored (0: nothing came out);
 * *channels gets the last frame's channel count. */
static u32 adec_ff_decode(void* p, const u8* au, u32 size, u32 dst, u32* channels)
{
    AdecFf* d = (AdecFf*)p;
    const PS3FFmpeg* ff = d->ff;
    if (size + AV_INPUT_BUFFER_PADDING_SIZE > d->bufCap) {
        u8* nb = (u8*)realloc(d->buf, size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!nb) return 0;
        d->buf = nb;
        d->bufCap = size + AV_INPUT_BUFFER_PADDING_SIZE;
    }
    memcpy(d->buf, au, size);
    memset(d->buf + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    d->pkt->data = d->buf;
    d->pkt->size = (int)size;
    int r = ff->avcodec_send_packet(d->ctx, d->pkt);
    d->pkt->data = NULL;
    d->pkt->size = 0;
    u32 out = 0;
    if (r < 0) {
        static int _n = 0;
        if (PS3_LOG_FIRST(_n, 8))
            printf("[cellAdec] AC3 AU of %u bytes did not decode (%d)\n", size, r);
    }
    for (;;) {
        AVFrame* f = d->frame;
        if (ff->avcodec_receive_frame(d->ctx, f) < 0)
            break;                                  /* EAGAIN / EOF / error */
        int ch = f->ch_layout.nb_channels, ns = f->nb_samples;
        u32 bytes = (u32)ns * (u32)ch * 4u;
        if (ch < 1 || ch > 8 || ns <= 0 || out + bytes > ADEC_PCM_MAX_BYTES) {
            static int _n = 0;
            if (PS3_LOG_FIRST(_n, 4))
                printf("[cellAdec] dropping a decoded frame (%d samples x %d ch, %u bytes held)\n",
                       ns, ch, out);
            ff->av_frame_unref(f);
            continue;
        }
        u32 i = 0;
        for (int s = 0; s < ns; s++)
            for (int c = 0; c < ch; c++) {
                float v = adec_ff_sample(f, ch, c, s);
                u32 w;
                memcpy(&w, &v, 4);
                d->pcm[i++] = (w >> 24) | ((w >> 8) & 0xFF00u) | ((w << 8) & 0xFF0000u) | (w << 24);
            }
        vm_memcpy_to(dst + out, d->pcm, bytes);
        { static int _n = 0;
          if (PS3_LOG_FIRST(_n, 1))
              printf("[cellAdec] AC3 through libavcodec: %d samples x %d ch at %d Hz, %u bytes an AU\n",
                     ns, ch, f->sample_rate, bytes); }
        out += bytes;
        *channels = (u32)ch;
        ff->av_frame_unref(f);
    }
    return out;
}

#else /* !PS3RECOMP_FFMPEG_RUNTIME */

static void* adec_ff_open(u32 codec_type) { (void)codec_type; return NULL; }
static void  adec_ff_close(void* p) { (void)p; }
static void  adec_ff_flush(void* p) { (void)p; }
static u32   adec_ff_decode(void* p, const u8* au, u32 size, u32 dst, u32* channels)
{ (void)p; (void)au; (void)size; (void)dst; (void)channels; return 0; }

#endif /* PS3RECOMP_FFMPEG_RUNTIME */

/* Fire one guest callback.
 *
 * The guest's cbFunc is a guest EA naming an OPD, so it goes through the
 * recompiled code's dispatcher. It used to be cast to a host function pointer
 * and called directly, which jumped to the guest address as host code:
 *
 *     [CRASH] code=0xC0000005 rip=00000000001B5F00
 *     [CRASH] last HLE NID (cellAdecDecodeAu)
 *
 * 0x1B5F00 is the guest OPD. That crash is what this function exists to stop.
 *
 * ponytail: dispatched SYNCHRONOUSLY on the caller's thread rather than from a
 * decoder thread as lv2 does. The ceiling is a guest callback that blocks
 * waiting on the thread that called DecodeAu -- give this its own thread if a
 * title ever deadlocks here. */
static void adec_notify(CellAdecHandle handle, u32 msg_type, s32 msg_data)
{
    if (handle >= MAX_ADEC || !s_adec[handle].in_use) return;
    const AdecSlot* a = &s_adec[handle];
    { static int _n = 0;
      if (PS3_LOG_FIRST(_n, 4))
          printf("[cellAdec] notify h=%u msg=%u data=0x%X cb=0x%08X arg=0x%08X caller=%d\n",
                 handle, msg_type, (unsigned)msg_data, a->cbFunc, a->cbArg,
                 g_ps3_guest_caller ? 1 : 0); }
    if (!a->cbFunc || !g_ps3_guest_caller) return;
    g_ps3_guest_caller(a->cbFunc, (u64)handle, (u64)msg_type,
                       (u64)(s64)msg_data, (u64)a->cbArg, 0, 0, 0, 0);
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

/* cellAdecQueryAttr: how much work memory the decoder wants, before Open.
 * CellAdecAttr (guest, big-endian): +0 u32 workMemSize, +4 u32 adecVerUpper,
 * +8 u32 adecVerLower. Decoding is host-side and never touches the buffer;
 * see cellVdecQueryAttr. */
s32 cellAdecQueryAttr(const CellAdecType* type, void* attr)
{
    u32 ea = (u32)(uintptr_t)attr;
    if (!type || !ea)
        return (s32)CELL_ADEC_ERROR_ARG;
    vm_write32(ea + 0x0, 0x40000);    /* workMemSize: 256 KB */
    vm_write32(ea + 0x4, 0x00010000); /* adecVerUpper */
    vm_write32(ea + 0x8, 0);          /* adecVerLower */
    return CELL_OK;
}

/* cellAdecOpen(type, res, cb, handle) -- FOUR arguments.
 *
 * This took five, splitting the guest's CellAdecCb struct into cbFunc + cbArg,
 * which pushed `handle` off r6 onto r7. r7 held whatever happened to be there, so
 * the null check failed and every Open returned CELL_ADEC_ERROR_ARG. ps1_netemu
 * opens a decoder for CD-DA/XA audio inside its CD-ROM constructor and gives up on
 * the error -- so the disc was never mounted and the emulator sat in its run loop
 * with nothing to run. Signature confirmed against RPCS3 (Modules/cellAdec.cpp).
 *
 * cb is a guest pointer to { u32 cbFunc; u32 cbArg; }. */
s32 cellAdecOpen(const CellAdecType* type, const CellAdecResource* res,
                 const CellAdecCb* cb, CellAdecHandle* handle)
{
    (void)res;

    u32 codec_type = type ? vm_read32(GUEST_EA(type)) : 0;   /* audioCodecType */
    u32 cb_ea      = (u32)(uintptr_t)cb;
    u32 handle_ea  = (u32)(uintptr_t)handle;
    printf("[cellAdec] Open(codecType=%u, cb=0x%08X, handle=0x%08X)\n",
           codec_type, cb_ea, handle_ea);

    if (!type || !handle_ea)
        return (s32)CELL_ADEC_ERROR_ARG;

    for (int i = 0; i < MAX_ADEC; i++) {
        if (!s_adec[i].in_use) {
            memset(&s_adec[i], 0, sizeof(AdecSlot));
            s_adec[i].in_use    = 1;
            s_adec[i].codecType = codec_type;
            if (cb_ea) {
                s_adec[i].cbFunc = vm_read32(cb_ea + 0);
                s_adec[i].cbArg  = vm_read32(cb_ea + 4);
            }
            /* The PcmItem is handed to the guest BY POINTER, so it cannot live
             * in host memory -- returning &host_struct is what crashed here
             * once already. */
            s_adec[i].itemEa = ADEC_ITEM_EA(i);
            s_adec[i].pcmEa  = ADEC_PCM_EA(i);
            s_adec[i].ff     = adec_ff_open(codec_type);
            vm_write32(handle_ea, (u32)i);
            printf("[cellAdec] Open -> handle=%u item=0x%08X pcm=0x%08X%s\n",
                   i, s_adec[i].itemEa, s_adec[i].pcmEa,
                   s_adec[i].ff ? " (decoding with libavcodec)" : "");
            return CELL_OK;
        }
    }
    return (s32)CELL_ADEC_ERROR_BUSY;
}

s32 cellAdecClose(CellAdecHandle handle)
{
    printf("[cellAdec] Close(handle=%u)\n", handle);

    if (handle >= MAX_ADEC || !s_adec[handle].in_use)
        return (s32)CELL_ADEC_ERROR_ARG;

    adec_ff_close(s_adec[handle].ff);
    free(s_adec[handle].au);
    s_adec[handle].ff = NULL;
    s_adec[handle].au = NULL;
    s_adec[handle].auCap = 0;
    s_adec[handle].in_use = 0;
    return CELL_OK;
}

s32 cellAdecStartSeq(CellAdecHandle handle, void* param)
{
    (void)param;
    printf("[cellAdec] StartSeq(handle=%u)\n", handle);

    if (handle >= MAX_ADEC || !s_adec[handle].in_use)
        return (s32)CELL_ADEC_ERROR_ARG;

    s_adec[handle].seqStarted = 1;
    adec_ff_flush(s_adec[handle].ff);   /* a new sequence starts clean */
    return CELL_OK;
}

s32 cellAdecEndSeq(CellAdecHandle handle)
{
    /* ADEC_WHO=1: name the guest function driving this. The title calls EndSeq
     * hundreds of times against a single StartSeq, and the image has eight
     * EndSeq call sites -- the backtrace says which one. */
    { static int _n = -1;
      if (_n < 0) _n = getenv("ADEC_WHO") ? 0 : -2;
      if (_n >= 0 && _n < 8) { _n++;
          /* The guest lr, not a host backtrace. ppu_guest_caller maps host
           * frames to the nearest lifted function, and here it landed on
           * func_00013040 -- a single `blr`, so plainly a mis-attribution.
           * lr is written by the `bl` to the import stub, so it is exactly
           * (call site + 4) for a call like this one. */
          extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
          printf("[cellAdec] EndSeq from guest lr=0x%08X tid=%llu\n",
                 g_active_ctx ? (u32)g_active_ctx->lr : 0u,
                 g_active_ctx ? (unsigned long long)g_active_ctx->thread_id : 0ull); } }
    printf("[cellAdec] EndSeq(handle=%u)\n", handle);

    if (handle >= MAX_ADEC || !s_adec[handle].in_use)
        return (s32)CELL_ADEC_ERROR_ARG;

    s_adec[handle].seqStarted = 0;

    adec_notify(handle, CELL_ADEC_MSG_TYPE_SEQDONE, CELL_OK);

    return CELL_OK;
}

s32 cellAdecDecodeAu(CellAdecHandle handle, const CellAdecAuInfo* auInfo)
{
    if (handle >= MAX_ADEC || !s_adec[handle].in_use)
        return (s32)CELL_ADEC_ERROR_ARG;
    if (!auInfo)
        return (s32)CELL_ADEC_ERROR_ARG;

    AdecSlot* a = &s_adec[handle];
    const u32 au = GUEST_EA(auInfo);
    const u32 au_addr = vm_read32(au + (u32)offsetof(CellAdecAuInfo, startAddr));
    const u32 au_size = vm_read32(au + (u32)offsetof(CellAdecAuInfo, size));
    /* The AU info, read before AUDONE gives it back to the title. */
    const u32 au_w[4] = { vm_read32(au + 0x08), vm_read32(au + 0x0C),
                          vm_read32(au + 0x10), vm_read32(au + 0x14) };

    printf("[cellAdec] DecodeAu(handle=%u, addr=0x%X, size=%u)\n",
           handle, au_addr, au_size);

    /* The AC3 bytes are copied out first, too: AUDONE hands their buffer back
     * to the title's demuxer, which may refill it at once. */
    const u8* data = NULL;
    if (a->codecType == CELL_ADEC_TYPE_AC3 && au_addr && au_size && au_size <= 0x10000u) {
        if (au_size > a->auCap) {
            u8* nb = (u8*)realloc(a->au, au_size);
            if (nb) { a->au = nb; a->auCap = au_size; }
        }
        if (au_size <= a->auCap) {
            vm_memcpy_from(a->au, au_addr, au_size);
            data = a->au;
        }
    }

    /* Step 1: report the AU consumed. msgData is the AU INFO ADDRESS, not a
     * status -- see RPCS3 Modules/cellAdec.cpp:1345. A decoder that tracks its
     * outstanding AUs by address gets a null back if this is CELL_OK. */
    adec_notify(handle, CELL_ADEC_MSG_TYPE_AUDONE, (s32)au);

    /* Step 2: the PCM. AC3 through libavcodec when it is there; otherwise
     * silence of the codec's frame size (AC3: 1536 samples in the frame's
     * channels), so a title that counts PCM against time still runs. */
    a->auCount++;
    {
        const u32 it = a->itemEa;
        u32 ch = 0;
        u32 bytes = (a->ff && data) ? adec_ff_decode(a->ff, data, au_size, a->pcmEa, &ch) : 0;
        if (bytes == 0) {
            if (a->codecType == CELL_ADEC_TYPE_AC3) {
                ch = adec_ac3_channels(data, data ? au_size : 0);
                bytes = ADEC_AC3_SAMPLES * (ch ? ch : 2u) * 4u;
            } else {
                bytes = ADEC_PCM_BYTES;
            }
            vm_memset(a->pcmEa, 0, bytes);
        }
        a->pcmBytes = bytes;

        vm_write32(it + PCMITEM_PCM_HANDLE, (u32)handle);
        vm_write32(it + PCMITEM_STATUS,     0);              /* CELL_OK */
        vm_write32(it + PCMITEM_START_ADDR, a->pcmEa);
        vm_write32(it + PCMITEM_SIZE,       bytes);
        /* bsiInfo must point at a codec info block, not be null. Resistance:
         * Fall of Man sizes each AC3 block from bsiInfo+0x14; with a null
         * pointer that read address 0x14 and came back 0, so its ring buffer
         * never filled, it never started its audio port, and its movie player
         * waited forever. Other titles' PCMOUT handlers take the AU's
         * PCM size from the same word. ponytail: only that output-size word is
         * filled; the rest of the block is zero until a title reads another
         * field. */
        const u32 bsi = ADEC_BSI_EA(handle);
        for (u32 o = 0; o < ADEC_BSI_BYTES; o += 4) vm_write32(bsi + o, 0);
        vm_write32(bsi + 0x14, bytes);
        vm_write32(it + PCMITEM_BSI_INFO,   bsi);
        /* Echo the AU back, which is what a real decoder does -- the guest
         * matches returned PCM against the AU it submitted. */
        vm_write32(it + PCMITEM_AU_START,        au_addr);
        vm_write32(it + PCMITEM_AU_SIZE,         au_size);
        vm_write32(it + PCMITEM_AU_PTS_HI,       au_w[0]);
        vm_write32(it + PCMITEM_AU_PTS_LO,       au_w[1]);
        vm_write32(it + PCMITEM_AU_USERDATA,     au_w[2]);
        vm_write32(it + PCMITEM_AU_USERDATA + 4, au_w[3]);
    }
    a->hasPcm = 1;

    adec_notify(handle, CELL_ADEC_MSG_TYPE_PCMOUT, CELL_OK);

    return CELL_OK;
}

s32 cellAdecGetPcm(CellAdecHandle handle, void* outBuffer)
{
    { static int _n = 0; if (PS3_LOG_FIRST(_n, 6))
        printf("[cellAdec] GetPcm(handle=%u out=0x%08X)\n",
               handle, GUEST_EA(outBuffer)); }
    if (handle >= MAX_ADEC || !s_adec[handle].in_use)
        return (s32)CELL_ADEC_ERROR_ARG;

    if (!s_adec[handle].hasPcm)
        return (s32)CELL_ADEC_ERROR_EMPTY;

    /* outBuffer is the GUEST's buffer: the AU's PCM block goes there, the
     * size the item and bsiInfo+0x14 announced. Leaving it untouched hands the
     * title whatever happened to be there, which it then plays. */
    { const u32 out = GUEST_EA(outBuffer);
      const AdecSlot* a = &s_adec[handle];
      if (out && a->pcmBytes)
          vm_memcpy_to(out, vm_ptr8(a->pcmEa), a->pcmBytes); }

    s_adec[handle].hasPcm = 0;
    return CELL_OK;
}

s32 cellAdecGetPcmItem(CellAdecHandle handle, const CellAdecPcmItem** pcmItem)
{
    { static int _n = 0; if (PS3_LOG_FIRST(_n, 6))
        printf("[cellAdec] GetPcmItem(handle=%u out=0x%08X hasPcm=%d)\n",
               handle, GUEST_EA(pcmItem),
               handle < MAX_ADEC ? s_adec[handle].hasPcm : -1); }
    if (handle >= MAX_ADEC || !s_adec[handle].in_use)
        return (s32)CELL_ADEC_ERROR_ARG;

    if (!s_adec[handle].hasPcm)
        return (s32)CELL_ADEC_ERROR_EMPTY;

    /* pcmItem is a GUEST CellAdecPcmItem**. Writing &host_struct here put a
     * HOST address into guest memory; the guest dereferenced it and died on a
     * wild address (rip 0x870D0E, fault 0xCFFDFFF0). Write the guest EA. */
    if (pcmItem)
        vm_write32(GUEST_EA(pcmItem), s_adec[handle].itemEa);

    return CELL_OK;
}
