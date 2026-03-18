/*
 * ps3recomp - cellAdec HLE implementation
 *
 * Stub audio decoder. Accepts AU data and delivers AUDONE callbacks.
 * Actual decoding (AAC, ATRAC3+, etc.) requires integration with
 * an audio codec library (e.g., FFmpeg).
 */

#include "cellAdec.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/
#define MAX_ADEC 4

typedef struct {
    int in_use;
    u32 codecType;
    CellAdecCbMsg cbFunc;
    void* cbArg;
    int seqStarted;
    CellAdecPcmItem lastPcm;
    int hasPcm;
    u32 auCount;        /* total AUs decoded */
} AdecSlot;

static AdecSlot s_adec[MAX_ADEC];

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellAdecOpen(const CellAdecType* type, const CellAdecResource* res,
                  CellAdecCbMsg cbFunc, void* cbArg, CellAdecHandle* handle)
{
    (void)res;

    printf("[cellAdec] Open(codecType=%u)\n", type ? type->audioCodecType : 0);

    if (!type || !handle)
        return (s32)CELL_ADEC_ERROR_ARG;

    for (int i = 0; i < MAX_ADEC; i++) {
        if (!s_adec[i].in_use) {
            memset(&s_adec[i], 0, sizeof(AdecSlot));
            s_adec[i].in_use = 1;
            s_adec[i].codecType = type->audioCodecType;
            s_adec[i].cbFunc = cbFunc;
            s_adec[i].cbArg = cbArg;
            *handle = (u32)i;
            printf("[cellAdec] Open -> handle=%u\n", i);
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
    return CELL_OK;
}

s32 cellAdecEndSeq(CellAdecHandle handle)
{
    printf("[cellAdec] EndSeq(handle=%u)\n", handle);

    if (handle >= MAX_ADEC || !s_adec[handle].in_use)
        return (s32)CELL_ADEC_ERROR_ARG;

    s_adec[handle].seqStarted = 0;

    if (s_adec[handle].cbFunc)
        s_adec[handle].cbFunc(handle, CELL_ADEC_MSG_TYPE_SEQDONE,
                               CELL_OK, s_adec[handle].cbArg);

    return CELL_OK;
}

s32 cellAdecDecodeAu(CellAdecHandle handle, const CellAdecAuInfo* auInfo)
{
    if (handle >= MAX_ADEC || !s_adec[handle].in_use)
        return (s32)CELL_ADEC_ERROR_ARG;
    if (!auInfo)
        return (s32)CELL_ADEC_ERROR_ARG;

    AdecSlot* a = &s_adec[handle];

    printf("[cellAdec] DecodeAu(handle=%u, addr=0x%X, size=%u)\n",
           handle, auInfo->startAddr, auInfo->size);

    /* Step 1: Report AU consumed */
    if (a->cbFunc)
        a->cbFunc(handle, CELL_ADEC_MSG_TYPE_AUDONE, CELL_OK, a->cbArg);

    /* Step 2: Generate PCMOUT callback with dummy PCM info.
     * Without FFmpeg, we produce silence. But games that check for
     * decode completion via callbacks will proceed correctly. */
    a->auCount++;
    memset(&a->lastPcm, 0, sizeof(a->lastPcm));
    a->lastPcm.pcmSize       = 256 * 2 * sizeof(float); /* 256 samples, stereo, float */
    a->lastPcm.channelNumber = 2;
    a->lastPcm.samplingRate  = 48000;
    a->lastPcm.bitsPerSample = 32; /* float */
    a->lastPcm.status        = 0;
    a->hasPcm = 1;

    if (a->cbFunc)
        a->cbFunc(handle, CELL_ADEC_MSG_TYPE_PCMOUT, CELL_OK, a->cbArg);

    return CELL_OK;
}

s32 cellAdecGetPcm(CellAdecHandle handle, void* outBuffer)
{
    (void)outBuffer;

    if (handle >= MAX_ADEC || !s_adec[handle].in_use)
        return (s32)CELL_ADEC_ERROR_ARG;

    if (!s_adec[handle].hasPcm)
        return (s32)CELL_ADEC_ERROR_EMPTY;

    s_adec[handle].hasPcm = 0;
    return CELL_OK;
}

s32 cellAdecGetPcmItem(CellAdecHandle handle, const CellAdecPcmItem** pcmItem)
{
    if (handle >= MAX_ADEC || !s_adec[handle].in_use)
        return (s32)CELL_ADEC_ERROR_ARG;

    if (!s_adec[handle].hasPcm)
        return (s32)CELL_ADEC_ERROR_EMPTY;

    if (pcmItem)
        *pcmItem = &s_adec[handle].lastPcm;

    return CELL_OK;
}
