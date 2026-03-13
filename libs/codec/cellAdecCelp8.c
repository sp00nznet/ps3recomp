/*
 * ps3recomp - cellAdecCelp8 HLE implementation
 *
 * CELP8 voice codec decoder.
 * Stub — decode produces silence (zero-filled PCM).
 */

#include "cellAdecCelp8.h"
#include <stdio.h>
#include <string.h>

/* Internal state */

#define CELP8_MAX_HANDLES 8

typedef struct {
    int  in_use;
    u32  bitRate;
} Celp8Slot;

static Celp8Slot s_slots[CELP8_MAX_HANDLES];

static int celp8_alloc(void)
{
    for (int i = 0; i < CELP8_MAX_HANDLES; i++) {
        if (!s_slots[i].in_use) return i;
    }
    return -1;
}

/* API */

s32 cellAdecCelp8Open(const CellAdecCelp8Config* config, CellAdecCelp8Handle* handle)
{
    printf("[cellAdecCelp8] Open()\n");
    if (!handle) return (s32)CELL_ADEC_CELP8_ERROR_INVALID_ARGUMENT;

    int slot = celp8_alloc();
    if (slot < 0) return (s32)CELL_ADEC_CELP8_ERROR_OUT_OF_MEMORY;

    s_slots[slot].in_use = 1;
    s_slots[slot].bitRate = config ? config->bitRate : CELL_ADEC_CELP8_BITRATE_6200;

    *handle = (CellAdecCelp8Handle)slot;
    return CELL_OK;
}

s32 cellAdecCelp8Close(CellAdecCelp8Handle handle)
{
    printf("[cellAdecCelp8] Close(%u)\n", handle);
    if (handle >= CELP8_MAX_HANDLES || !s_slots[handle].in_use)
        return (s32)CELL_ADEC_CELP8_ERROR_INVALID_ARGUMENT;

    s_slots[handle].in_use = 0;
    return CELL_OK;
}

s32 cellAdecCelp8Decode(CellAdecCelp8Handle handle, const void* frame, u32 frameSize,
                         s16* pcmOut, u32* numSamples)
{
    (void)frame; (void)frameSize;

    if (handle >= CELP8_MAX_HANDLES || !s_slots[handle].in_use)
        return (s32)CELL_ADEC_CELP8_ERROR_INVALID_ARGUMENT;
    if (!pcmOut) return (s32)CELL_ADEC_CELP8_ERROR_INVALID_ARGUMENT;

    /* Output silence: 160 samples of 16-bit mono */
    memset(pcmOut, 0, CELL_ADEC_CELP8_SAMPLES_PER_FRAME * sizeof(s16));
    if (numSamples) *numSamples = CELL_ADEC_CELP8_SAMPLES_PER_FRAME;
    return CELL_OK;
}

s32 cellAdecCelp8Reset(CellAdecCelp8Handle handle)
{
    if (handle >= CELP8_MAX_HANDLES || !s_slots[handle].in_use)
        return (s32)CELL_ADEC_CELP8_ERROR_INVALID_ARGUMENT;
    return CELL_OK;
}
