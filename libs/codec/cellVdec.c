/*
 * ps3recomp - cellVdec HLE implementation
 *
 * H.264 and MPEG-2 decode through libavcodec when the runtime is built with
 * PS3RECOMP_FFMPEG_RUNTIME and the library is present (vdec_ffmpeg.c,
 * ffmpeg_runtime.c): every AU is decoded, pictures are queued in display
 * order and announced with PICOUT, and GetPicture converts them to the
 * requested format. Otherwise this is the callback-only decoder it always
 * was: AUDONE and PICOUT per AU, and a black picture.
 */

#include "cellVdec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vdec_ffmpeg.h"
#include "../guest_struct.h"   /* GUEST_EA, vm_read/vm_write: guest EA -> host */
#include "ps3emu/guest_call.h" /* g_ps3_guest_caller -- cbFunc is a GUEST OPD */
#include "../../runtime/memory/vm.h"     /* VM_HLE_INJECT_BASE */

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/
#define MAX_VDEC 4
#define VDEC_QUEUE   32   /* decoded pictures waiting for GetPicture */
#define VDEC_BUSY_AT 16   /* refuse new AUs past this many (EndSeq can add more) */
#define VDEC_AU_RING 64   /* AUs remembered for pictures that come out late */

/* A decoded picture waiting for the title (decoder path only). */
typedef struct {
    u8* yuv;            /* I420, width x height */
    CellVdecPicItem item;
    int pictType;       /* 1 I, 2 P, 3 B, 0 unknown */
    int key;
} VdecPic;

typedef struct {
    int in_use;
    u32 codecType;
    u32 cbFunc;         /* guest EA of the callback's OPD */
    u32 cbArg;          /* guest EA handed back to it     */
    int seqStarted;
    CellVdecPicItem lastPic;
    int hasPic;
    u32 auCount;        /* total AUs decoded */
    u16 width;          /* configured resolution (0 = use default) */
    u16 height;
    /* Decoder path: NULL when there is no libavcodec, and then nothing
     * below is used. */
    VdecFf* ff;
    VdecPic queue[VDEC_QUEUE];
    u32 qHead, qCount;
    CellVdecAuInfo aus[VDEC_AU_RING];   /* by auCount % VDEC_AU_RING */
    u32 newPics;        /* pictures queued by the current decode call */
} VdecSlot;

static VdecSlot s_vdec[MAX_VDEC];

/* Deliver a vdec message to the guest callback (handle, msgType, msgData,
 * cbArg). It is a guest OPD, so it goes through g_ps3_guest_caller; this used
 * to call it as a host function pointer, which crashes the first time a
 * title's callback is reached.
 * ponytail: synchronous on the caller's thread, like adec_notify; give it a
 * decoder thread if a title's callback ever blocks on the DecodeAu caller. */
static void vdec_notify(CellVdecHandle handle, u32 msg_type, s32 msg_data)
{
    if (handle >= MAX_VDEC || !s_vdec[handle].in_use) return;
    const VdecSlot* v = &s_vdec[handle];
    if (!v->cbFunc || !g_ps3_guest_caller) return;
    g_ps3_guest_caller(v->cbFunc, (u64)handle, (u64)msg_type,
                       (u64)(s64)msg_data, (u64)v->cbArg, 0, 0, 0, 0);
}

/* ---------------------------------------------------------------------------
 * Decoder path (libavcodec present)
 * -----------------------------------------------------------------------*/

static void vdec_queue_clear(VdecSlot* v)
{
    for (u32 i = 0; i < v->qCount; i++)
        free(v->queue[(v->qHead + i) % VDEC_QUEUE].yuv);
    v->qHead = v->qCount = 0;
}

/* vdec_ff_emit_fn: queue a decoded picture under the item of the AU it was
 * decoded from. Pictures leave the decoder in display order, so with B-frames
 * that is not the AU just submitted; the token is that AU's auCount. */
static void vdec_on_picture(void* ctx, const VdecFfPicture* pic)
{
    VdecSlot* v = (VdecSlot*)ctx;
    if (v->qCount == VDEC_QUEUE) {
        printf("[cellVdec] picture queue full; dropping the oldest\n");
        free(v->queue[v->qHead].yuv);
        v->qHead = (v->qHead + 1) % VDEC_QUEUE;
        v->qCount--;
    }
    const CellVdecAuInfo* au = &v->aus[pic->token % VDEC_AU_RING];
    VdecPic* q = &v->queue[(v->qHead + v->qCount) % VDEC_QUEUE];
    memset(q, 0, sizeof(*q));
    q->yuv = pic->yuv;
    q->pictType = pic->pict_type;
    q->key = pic->key;
    q->item.codecType = v->codecType;
    q->item.startAddr = au->startAddr;
    q->item.size      = au->size;
    q->item.auNum     = pic->token;
    q->item.pts       = au->pts;
    q->item.dts       = au->dts;
    q->item.userData  = au->userData;
    q->item.status    = 0;
    q->item.picFmt    = CELL_VDEC_PIC_FMT_YUV420P;
    q->item.width     = (u16)pic->width;
    q->item.height    = (u16)pic->height;
    v->qCount++;
    v->newPics++;
}

/* PICOUT for each picture the last decoder call queued. */
static void vdec_notify_new_pics(CellVdecHandle handle)
{
    u32 n = s_vdec[handle].newPics;
    s_vdec[handle].newPics = 0;
    while (n--)
        vdec_notify(handle, CELL_VDEC_MSG_TYPE_PICOUT, CELL_OK);
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

/* cellVdecQueryAttr: how much work memory the decoder wants, before Open.
 * CellVdecAttr (guest, big-endian): +0 u32 memSize, +4 u8 cmdDepth,
 * +8 u32 decoderVerUpper, +0xC u32 decoderVerLower.
 * Decoding is host-side, so the guest's buffer is never touched; memSize only
 * has to be something the title can allocate. cmdDepth sizes the title's own
 * AU queue, so it must not be 0. Unimplemented, the call left the struct
 * unfilled and a movie player (Resistance: Fall of Man) gave up and retried
 * forever. */
s32 cellVdecQueryAttr(const CellVdecType* type, void* attr)
{
    u32 ea = (u32)(uintptr_t)attr;
    if (!type || !ea)
        return (s32)CELL_VDEC_ERROR_ARG;
    vm_write32(ea + 0x0, 0x100000);   /* memSize: 1 MB */
    vm_write8 (ea + 0x4, 4);          /* cmdDepth */
    vm_write32(ea + 0x8, 0x00010000); /* decoderVerUpper */
    vm_write32(ea + 0xC, 0);          /* decoderVerLower */
    return CELL_OK;
}

/* cellVdecOpen(type, res, cb, handle) -- FOUR arguments, like cellAdecOpen.
 * cb is a guest pointer to { u32 cbFunc; u32 cbArg; }. Declared with five,
 * `handle` came from r7 instead of r6: the handle was written somewhere
 * random and the title's own handle stayed 0 (Resistance: Fall of Man opened
 * handle 1, then called StartSeq(0)); four Opens later every call was BUSY. */
s32 cellVdecOpen(const CellVdecType* type, const CellVdecResource* res,
                  const CellVdecCb* cb, CellVdecHandle* handle)
{
    (void)res;
    u32 cb_ea = (u32)(uintptr_t)cb;

    u32 codec_type = type ? vm_read32(GUEST_EA(type)) : 0;   /* codecType */
    printf("[cellVdec] Open(codecType=%u)\n", codec_type);

    if (!type || !handle)
        return (s32)CELL_VDEC_ERROR_ARG;

    for (int i = 0; i < MAX_VDEC; i++) {
        if (!s_vdec[i].in_use) {
            memset(&s_vdec[i], 0, sizeof(VdecSlot));
            s_vdec[i].in_use = 1;
            s_vdec[i].codecType = codec_type;
            s_vdec[i].cbFunc = cb_ea ? vm_read32(cb_ea + 0) : 0;
            s_vdec[i].cbArg  = cb_ea ? vm_read32(cb_ea + 4) : 0;
            s_vdec[i].ff = vdec_ff_open(codec_type);
            vm_write32((u32)(uintptr_t)handle, (u32)i);
            printf("[cellVdec] Open -> handle=%u\n", i);
            if (s_vdec[i].ff)
                printf("[cellVdec] decoding with libavcodec\n");
            return CELL_OK;
        }
    }
    return (s32)CELL_VDEC_ERROR_BUSY;
}

s32 cellVdecClose(CellVdecHandle handle)
{
    printf("[cellVdec] Close(handle=%u)\n", handle);

    if (handle >= MAX_VDEC || !s_vdec[handle].in_use)
        return (s32)CELL_VDEC_ERROR_ARG;

    if (s_vdec[handle].ff) {
        vdec_queue_clear(&s_vdec[handle]);
        vdec_ff_close(s_vdec[handle].ff);
        s_vdec[handle].ff = NULL;
    }
    s_vdec[handle].in_use = 0;
    return CELL_OK;
}

s32 cellVdecStartSeq(CellVdecHandle handle)
{
    printf("[cellVdec] StartSeq(handle=%u)\n", handle);

    if (handle >= MAX_VDEC || !s_vdec[handle].in_use)
        return (s32)CELL_VDEC_ERROR_ARG;

    s_vdec[handle].seqStarted = 1;
    if (s_vdec[handle].ff) {
        /* A new sequence: nothing from the last one is shown. */
        vdec_ff_reset(s_vdec[handle].ff);
        vdec_queue_clear(&s_vdec[handle]);
    }
    return CELL_OK;
}

s32 cellVdecEndSeq(CellVdecHandle handle)
{
    printf("[cellVdec] EndSeq(handle=%u)\n", handle);

    if (handle >= MAX_VDEC || !s_vdec[handle].in_use)
        return (s32)CELL_VDEC_ERROR_ARG;

    s_vdec[handle].seqStarted = 0;

    /* Pictures the decoder still holds for reordering come out now, each
     * with its PICOUT, before SEQDONE. */
    if (s_vdec[handle].ff) {
        vdec_ff_drain(s_vdec[handle].ff, vdec_on_picture, &s_vdec[handle]);
        vdec_notify_new_pics(handle);
    }

    /* Notify sequence done */
    vdec_notify(handle, CELL_VDEC_MSG_TYPE_SEQDONE, CELL_OK);

    return CELL_OK;
}

s32 cellVdecDecodeAu(CellVdecHandle handle, s32 mode, const CellVdecAuInfo* auInfo)
{
    (void)mode;

    if (handle >= MAX_VDEC || !s_vdec[handle].in_use)
        return (s32)CELL_VDEC_ERROR_ARG;
    if (!auInfo)
        return (s32)CELL_VDEC_ERROR_ARG;

    VdecSlot* v = &s_vdec[handle];

    /* CellVdecAuInfo mixes u32 and u64, so it comes across field by field: a
     * big-endian u64 is high word first, and a word-at-a-time copy would put
     * the two halves the wrong way round on a little-endian host. */
    u32 au_ea = GUEST_EA(auInfo);
    CellVdecAuInfo au;
    au.startAddr = vm_read32(au_ea + (u32)offsetof(CellVdecAuInfo, startAddr));
    au.size      = vm_read32(au_ea + (u32)offsetof(CellVdecAuInfo, size));
    au.pts       = vm_read64(au_ea + (u32)offsetof(CellVdecAuInfo, pts));
    au.dts       = vm_read64(au_ea + (u32)offsetof(CellVdecAuInfo, dts));
    au.userData  = vm_read64(au_ea + (u32)offsetof(CellVdecAuInfo, userData));

    printf("[cellVdec] DecodeAu(handle=%u, addr=0x%X, size=%u, pts=%llu)\n",
           handle, au.startAddr, au.size,
           (unsigned long long)au.pts);

    if (v->ff) {
        if (v->qCount >= VDEC_BUSY_AT)
            return (s32)CELL_VDEC_ERROR_BUSY;  /* title must take pictures */
        v->auCount++;
        v->aus[v->auCount % VDEC_AU_RING] = au;
        v->newPics = 0;
        if (au.size)   /* an empty packet would mean end of stream to FFmpeg */
            vdec_ff_decode(v->ff, vm_ptr8(au.startAddr), au.size, v->auCount,
                           vdec_on_picture, v);
        vdec_notify(handle, CELL_VDEC_MSG_TYPE_AUDONE, CELL_OK);
        vdec_notify_new_pics(handle);
        return CELL_OK;
    }

    /* Step 1: Report AU consumed */
    vdec_notify(handle, CELL_VDEC_MSG_TYPE_AUDONE, CELL_OK);

    /* Step 2: Generate a dummy PICOUT callback.
     * Without FFmpeg, we can't actually decode video. But games expect the
     * PICOUT callback to fire for each AU, so we generate a placeholder
     * picture. Games that only check for completion (not pixel data) will
     * proceed correctly. */
    v->auCount++;
    memset(&v->lastPic, 0, sizeof(v->lastPic));
    v->lastPic.codecType = v->codecType;
    v->lastPic.startAddr = au.startAddr;
    v->lastPic.size      = au.size;
    v->lastPic.auNum     = v->auCount;
    v->lastPic.pts       = au.pts;
    v->lastPic.dts       = au.dts;
    v->lastPic.userData  = au.userData;
    v->lastPic.status    = 0; /* OK */
    v->lastPic.picFmt    = CELL_VDEC_PIC_FMT_YUV420P;
    v->lastPic.width     = v->width ? v->width : 1280;
    v->lastPic.height    = v->height ? v->height : 720;
    v->hasPic = 1;

    vdec_notify(handle, CELL_VDEC_MSG_TYPE_PICOUT, CELL_OK);

    return CELL_OK;
}

/* Guest-resident pic items. cellVdecGetPicItem hands the title a POINTER to a
 * CellVdecPicItem, so it has to live in guest memory, big-endian; writing
 * &host_struct through the guest's out-pointer crashed Resistance: Fall of Man
 * inside its PICOUT callback. One 0x100-byte block per decoder after cellAdec's
 * scratch (VM_HLE_INJECT_BASE + 0x40000..0x50000): the item at +0, the codec
 * info it points to at +0x80.
 *
 * Guest CellVdecPicItem: +0x00 codecType, +0x04 startAddr, +0x08 size,
 * +0x0C u8 auNum, +0x10 auPts[2] {upper,lower}, +0x20 auDts[2],
 * +0x30 u64 auUserData[2], +0x40 status, +0x44 attr (0 normal, 1 skipped),
 * +0x48 picInfo. CellVdecAvcInfo and CellVdecMpeg2Info both begin with
 * u16 horizontalSize, u16 verticalSize; the rest of the info block is zero. */
#define VDEC_ITEM_EA(h)  (VM_HLE_INJECT_BASE + 0x50000u + (u32)(h) * 0x100u)
#define VDEC_INFO_EA(h)  (VDEC_ITEM_EA(h) + 0x80u)

static void vdec_write_item(CellVdecHandle handle, const CellVdecPicItem* p)
{
    const VdecSlot* v = &s_vdec[handle];
    u32 it = VDEC_ITEM_EA(handle), info = VDEC_INFO_EA(handle);
    for (u32 o = 0; o < 0x100; o += 4) vm_write32(it + o, 0);
    vm_write32(it + 0x00, v->codecType);
    vm_write32(it + 0x04, p->startAddr);
    vm_write32(it + 0x08, p->size);
    vm_write8 (it + 0x0C, 1);                          /* one AU per picture */
    vm_write32(it + 0x10, (u32)(p->pts >> 32));        /* auPts[0] */
    vm_write32(it + 0x14, (u32)p->pts);
    vm_write32(it + 0x18, 0xFFFFFFFFu);                /* auPts[1]: unused */
    vm_write32(it + 0x1C, 0xFFFFFFFFu);
    vm_write32(it + 0x20, (u32)(p->dts >> 32));        /* auDts[0] */
    vm_write32(it + 0x24, (u32)p->dts);
    vm_write32(it + 0x28, 0xFFFFFFFFu);
    vm_write32(it + 0x2C, 0xFFFFFFFFu);
    vm_write64(it + 0x30, p->userData);                /* auUserData[0] */
    vm_write32(it + 0x40, 0);                          /* status: OK */
    vm_write32(it + 0x44, 0);                          /* attr: normal */
    vm_write32(it + 0x48, info);
    vm_write16(info + 0, p->width);
    vm_write16(info + 2, p->height);
}

/* The decoder path also reports the picture type.
 * ASSUMPTION (field order of the SDK's CellVdecAvcInfo / CellVdecMpeg2Info,
 * not yet checked against a title):
 *   AVC   +0x04 u8 pictureType[2] (0 I, 1 P, 2 B, 3 unknown),
 *         +0x06 u8 idrPictureFlag;
 *   MPEG2 +0x12 u8 picture_coding_type[2] (1 I, 2 P, 3 B, as in the bitstream).
 * pict_type is FFmpeg's (1 I, 2 P, 3 B, 0 none). */
static void vdec_write_pic_type(CellVdecHandle handle, const VdecPic* q)
{
    u32 info = VDEC_INFO_EA(handle);
    int t = q->pictType;
    if (s_vdec[handle].codecType == CELL_VDEC_CODEC_TYPE_AVC) {
        vm_write8(info + 0x04, (u8)(t >= 1 && t <= 3 ? t - 1 : 3));
        vm_write8(info + 0x05, 3);
        vm_write8(info + 0x06, (u8)(q->key && t == 1));
    } else if (s_vdec[handle].codecType == CELL_VDEC_CODEC_TYPE_MPEG2) {
        vm_write8(info + 0x12, (u8)(t >= 1 && t <= 3 ? t : 0));
    }
}

/* cellVdecGetPicItem(handle, &item): the NEXT picture's item, without
 * consuming it; cellVdecGetPicture is what takes it off the queue. */
s32 cellVdecGetPicItem(CellVdecHandle handle, const CellVdecPicItem** picItem)
{
    u32 out = (u32)(uintptr_t)picItem;
    if (handle >= MAX_VDEC || !s_vdec[handle].in_use || !out)
        return (s32)CELL_VDEC_ERROR_ARG;
    const VdecSlot* v = &s_vdec[handle];
    if (v->ff) {
        if (!v->qCount)
            return (s32)CELL_VDEC_ERROR_EMPTY;
        const VdecPic* q = &v->queue[v->qHead];
        vdec_write_item(handle, &q->item);
        vdec_write_pic_type(handle, q);
        vm_write32(out, VDEC_ITEM_EA(handle));
        return CELL_OK;
    }
    if (!s_vdec[handle].hasPic)
        return (s32)CELL_VDEC_ERROR_EMPTY;
    vdec_write_item(handle, &v->lastPic);
    vm_write32(out, VDEC_ITEM_EA(handle));
    return CELL_OK;
}

/* cellVdecGetPicture(handle, format, outBuff): copy the next picture out in
 * the requested format and consume it. format is a guest CellVdecPicFormat
 * { u32 formatType; u32 colorMatrixType; u8 alpha; }; formatType 0 ARGB32,
 * 1 RGBA32, 2 UYVY422, 3 YUV420 planar.
 * With libavcodec the picture is the decoded one (vdec_take_picture). Without
 * it the picture is black at the item's size (1280x720 unless set); titles
 * that only pace playback on PICOUT/GetPicture run as they should. */
static s32 vdec_take_picture(VdecSlot* v, u32 dst, u32 type, u32 matrix, u8 alpha)
{
    if (!v->qCount)
        return (s32)CELL_VDEC_ERROR_EMPTY;
    VdecPic* q = &v->queue[v->qHead];
    v->qHead = (v->qHead + 1) % VDEC_QUEUE;
    v->qCount--;
    if (dst)                                           /* NULL: a skip */
        vdec_store_picture(dst, q->yuv, q->item.width, q->item.height,
                           type, matrix, alpha);
    free(q->yuv);
    q->yuv = NULL;
    return CELL_OK;
}

s32 cellVdecGetPicture(CellVdecHandle handle, const void* format, void* outBuff)
{
    u32 fmt_ea = (u32)(uintptr_t)format, dst = (u32)(uintptr_t)outBuff;
    if (handle >= MAX_VDEC || !s_vdec[handle].in_use || !fmt_ea)
        return (s32)CELL_VDEC_ERROR_ARG;
    VdecSlot* v = &s_vdec[handle];
    if (v->ff)
        return vdec_take_picture(v, dst, vm_read32(fmt_ea + 0),
                                 vm_read32(fmt_ea + 4), vm_read8(fmt_ea + 8));
    if (!v->hasPic)
        return (s32)CELL_VDEC_ERROR_EMPTY;
    v->hasPic = 0;
    if (!dst)
        return CELL_OK;                                /* a skip: consume only */

    u32 type  = vm_read32(fmt_ea + 0);
    u8  alpha = vm_read8(fmt_ea + 8);
    u32 w = v->lastPic.width, h = v->lastPic.height;
    u8 row[4096 * 4];
    u32 rowlen;
    switch (type) {
    case 0: case 1:                                    /* ARGB32 / RGBA32 */
        rowlen = w * 4; memset(row, 0, rowlen);
        for (u32 x = 0; x < w; x++) row[x * 4 + (type == 0 ? 0 : 3)] = alpha;
        break;
    case 2:                                            /* UYVY: U Y V Y */
        rowlen = w * 2;
        for (u32 x = 0; x < rowlen; x += 2) { row[x] = 0x80; row[x + 1] = 0x10; }
        break;
    default: {                                         /* YUV420 planar */
        memset(row, 0x10, w);
        for (u32 y = 0; y < h; y++) guest_struct_store(dst + y * w, row, w);
        memset(row, 0x80, w / 2);
        u32 c = dst + w * h;
        for (u32 y = 0; y < h; y++) guest_struct_store(c + y * (w / 2), row, w / 2);
        return CELL_OK; }
    }
    for (u32 y = 0; y < h; y++) guest_struct_store(dst + y * rowlen, row, rowlen);
    return CELL_OK;
}

/* cellVdecGetPictureExt(handle, format2, outBuff, arg4): GetPicture with a
 * CellVdecPicFormat2. ASSUMPTION (SDK field order): +0 u32 formatType,
 * +4 u32 colorMatrixType, +8 u32 unk0, +0xC u8 alpha; arg4 is unused here.
 * Without a decoder this does what the call did before it was implemented
 * (an unregistered import): return CELL_OK and touch nothing. */
s32 cellVdecGetPictureExt(CellVdecHandle handle, const void* format,
                          void* outBuff, u32 arg4)
{
    (void)arg4;
    u32 fmt_ea = (u32)(uintptr_t)format, dst = (u32)(uintptr_t)outBuff;
    if (handle >= MAX_VDEC || !s_vdec[handle].in_use || !s_vdec[handle].ff)
        return CELL_OK;
    if (!fmt_ea)
        return (s32)CELL_VDEC_ERROR_ARG;
    return vdec_take_picture(&s_vdec[handle], dst, vm_read32(fmt_ea + 0),
                             vm_read32(fmt_ea + 4), vm_read8(fmt_ea + 0xC));
}

s32 cellVdecSetFrameRate(CellVdecHandle handle, u32 frameRateCode)
{
    printf("[cellVdec] SetFrameRate(handle=%u, code=%u)\n",
           handle, frameRateCode);

    if (handle >= MAX_VDEC || !s_vdec[handle].in_use)
        return (s32)CELL_VDEC_ERROR_ARG;

    return CELL_OK;
}
