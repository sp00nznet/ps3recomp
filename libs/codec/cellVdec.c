/*
 * ps3recomp - cellVdec HLE implementation
 *
 * Stub video decoder. Accepts AU data and delivers AUDONE callbacks
 * but does not perform actual H.264/MPEG-2 decoding. Games that
 * require video playback will need an FFmpeg/libav integration here.
 */

#include "cellVdec.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/
#define MAX_VDEC 4

typedef struct {
    int in_use;
    u32 codecType;
    CellVdecCbMsg cbFunc;
    void* cbArg;
    int seqStarted;
    CellVdecPicItem lastPic;
    int hasPic;
} VdecSlot;

static VdecSlot s_vdec[MAX_VDEC];

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellVdecOpen(const CellVdecType* type, const CellVdecResource* res,
                  CellVdecCbMsg cbFunc, void* cbArg, CellVdecHandle* handle)
{
    (void)res;

    printf("[cellVdec] Open(codecType=%u)\n", type ? type->codecType : 0);

    if (!type || !handle)
        return (s32)CELL_VDEC_ERROR_ARG;

    for (int i = 0; i < MAX_VDEC; i++) {
        if (!s_vdec[i].in_use) {
            memset(&s_vdec[i], 0, sizeof(VdecSlot));
            s_vdec[i].in_use = 1;
            s_vdec[i].codecType = type->codecType;
            s_vdec[i].cbFunc = cbFunc;
            s_vdec[i].cbArg = cbArg;
            *handle = (u32)i;
            printf("[cellVdec] Open -> handle=%u\n", i);
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

    s_vdec[handle].in_use = 0;
    return CELL_OK;
}

s32 cellVdecStartSeq(CellVdecHandle handle)
{
    printf("[cellVdec] StartSeq(handle=%u)\n", handle);

    if (handle >= MAX_VDEC || !s_vdec[handle].in_use)
        return (s32)CELL_VDEC_ERROR_ARG;

    s_vdec[handle].seqStarted = 1;
    return CELL_OK;
}

s32 cellVdecEndSeq(CellVdecHandle handle)
{
    printf("[cellVdec] EndSeq(handle=%u)\n", handle);

    if (handle >= MAX_VDEC || !s_vdec[handle].in_use)
        return (s32)CELL_VDEC_ERROR_ARG;

    s_vdec[handle].seqStarted = 0;

    /* Notify sequence done */
    if (s_vdec[handle].cbFunc)
        s_vdec[handle].cbFunc(handle, CELL_VDEC_MSG_TYPE_SEQDONE,
                               CELL_OK, s_vdec[handle].cbArg);

    return CELL_OK;
}

s32 cellVdecDecodeAu(CellVdecHandle handle, s32 mode, const CellVdecAuInfo* auInfo)
{
    (void)mode;

    if (handle >= MAX_VDEC || !s_vdec[handle].in_use)
        return (s32)CELL_VDEC_ERROR_ARG;
    if (!auInfo)
        return (s32)CELL_VDEC_ERROR_ARG;

    printf("[cellVdec] DecodeAu(handle=%u, addr=0x%X, size=%u, pts=%llu)\n",
           handle, auInfo->startAddr, auInfo->size,
           (unsigned long long)auInfo->pts);

    /* Report AU consumed */
    if (s_vdec[handle].cbFunc)
        s_vdec[handle].cbFunc(handle, CELL_VDEC_MSG_TYPE_AUDONE,
                               CELL_OK, s_vdec[handle].cbArg);

    /* In a real implementation, decoded picture data would be placed
       in memory and PICOUT callback delivered. For now, skip actual decode. */

    return CELL_OK;
}

s32 cellVdecGetPicture(CellVdecHandle handle, const CellVdecPicItem** picItem)
{
    if (handle >= MAX_VDEC || !s_vdec[handle].in_use)
        return (s32)CELL_VDEC_ERROR_ARG;

    if (!s_vdec[handle].hasPic)
        return (s32)CELL_VDEC_ERROR_EMPTY;

    if (picItem)
        *picItem = &s_vdec[handle].lastPic;

    s_vdec[handle].hasPic = 0;
    return CELL_OK;
}

s32 cellVdecGetPicItem(CellVdecHandle handle, const CellVdecPicItem** picItem)
{
    return cellVdecGetPicture(handle, picItem);
}

s32 cellVdecSetFrameRate(CellVdecHandle handle, u32 frameRateCode)
{
    printf("[cellVdec] SetFrameRate(handle=%u, code=%u)\n",
           handle, frameRateCode);

    if (handle >= MAX_VDEC || !s_vdec[handle].in_use)
        return (s32)CELL_VDEC_ERROR_ARG;

    return CELL_OK;
}
