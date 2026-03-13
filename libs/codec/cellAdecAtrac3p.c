/*
 * ps3recomp - cellAdecAtrac3p HLE implementation
 *
 * ATRAC3plus audio decoder.
 * Stub — decode produces silence (zero-filled PCM buffers).
 * Proper decoding would require an ATRAC3+ codec library.
 */

#include "cellAdecAtrac3p.h"
#include <stdio.h>
#include <string.h>

/* Internal state */

#define ATRAC3P_MAX_HANDLES 8

typedef struct {
    int  in_use;
    u32  channels;
    u32  sampleRate;
} Atrac3pSlot;

static Atrac3pSlot s_slots[ATRAC3P_MAX_HANDLES];

static int atrac3p_alloc(void)
{
    for (int i = 0; i < ATRAC3P_MAX_HANDLES; i++) {
        if (!s_slots[i].in_use) return i;
    }
    return -1;
}

/* API */

s32 cellAdecAtrac3pOpen(const CellAdecAtrac3pConfig* config, CellAdecAtrac3pHandle* handle)
{
    printf("[cellAdecAtrac3p] Open()\n");
    if (!handle) return (s32)CELL_ADEC_ATRAC3P_ERROR_INVALID_ARGUMENT;

    int slot = atrac3p_alloc();
    if (slot < 0) return (s32)CELL_ADEC_ATRAC3P_ERROR_OUT_OF_MEMORY;

    s_slots[slot].in_use = 1;
    s_slots[slot].channels = config ? config->channels : CELL_ADEC_ATRAC3P_CHANNELS_STEREO;
    s_slots[slot].sampleRate = config ? config->sampleRate : CELL_ADEC_ATRAC3P_SRATE_48000;

    *handle = (CellAdecAtrac3pHandle)slot;
    return CELL_OK;
}

s32 cellAdecAtrac3pClose(CellAdecAtrac3pHandle handle)
{
    printf("[cellAdecAtrac3p] Close(%u)\n", handle);
    if (handle >= ATRAC3P_MAX_HANDLES || !s_slots[handle].in_use)
        return (s32)CELL_ADEC_ATRAC3P_ERROR_INVALID_ARGUMENT;

    s_slots[handle].in_use = 0;
    return CELL_OK;
}

s32 cellAdecAtrac3pDecode(CellAdecAtrac3pHandle handle, const void* au, u32 auSize,
                           void* pcmOut, u32 pcmBufSize, CellAdecAtrac3pPcmInfo* info)
{
    (void)au; (void)auSize;

    if (handle >= ATRAC3P_MAX_HANDLES || !s_slots[handle].in_use)
        return (s32)CELL_ADEC_ATRAC3P_ERROR_INVALID_ARGUMENT;
    if (!pcmOut) return (s32)CELL_ADEC_ATRAC3P_ERROR_INVALID_ARGUMENT;

    Atrac3pSlot* s = &s_slots[handle];
    u32 frameBytes = CELL_ADEC_ATRAC3P_SAMPLES_PER_FRAME * s->channels * 2; /* 16-bit PCM */

    if (pcmBufSize < frameBytes)
        return (s32)CELL_ADEC_ATRAC3P_ERROR_INVALID_ARGUMENT;

    /* Output silence */
    memset(pcmOut, 0, frameBytes);

    if (info) {
        info->channels = s->channels;
        info->sampleRate = s->sampleRate;
        info->numSamples = CELL_ADEC_ATRAC3P_SAMPLES_PER_FRAME;
        info->bitsPerSample = 16;
    }
    return CELL_OK;
}

s32 cellAdecAtrac3pReset(CellAdecAtrac3pHandle handle)
{
    if (handle >= ATRAC3P_MAX_HANDLES || !s_slots[handle].in_use)
        return (s32)CELL_ADEC_ATRAC3P_ERROR_INVALID_ARGUMENT;
    return CELL_OK;
}

s32 cellAdecAtrac3pGetPcmInfo(CellAdecAtrac3pHandle handle, CellAdecAtrac3pPcmInfo* info)
{
    if (handle >= ATRAC3P_MAX_HANDLES || !s_slots[handle].in_use)
        return (s32)CELL_ADEC_ATRAC3P_ERROR_INVALID_ARGUMENT;
    if (!info) return (s32)CELL_ADEC_ATRAC3P_ERROR_INVALID_ARGUMENT;

    info->channels = s_slots[handle].channels;
    info->sampleRate = s_slots[handle].sampleRate;
    info->numSamples = CELL_ADEC_ATRAC3P_SAMPLES_PER_FRAME;
    info->bitsPerSample = 16;
    return CELL_OK;
}
