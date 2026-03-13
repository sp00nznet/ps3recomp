/*
 * ps3recomp - cellVdecDivx HLE implementation
 *
 * DivX video decoder.
 * Stub — decode produces a black frame (zero-filled YUV).
 */

#include "cellVdecDivx.h"
#include <stdio.h>
#include <string.h>

/* Internal state */

#define DIVX_MAX_HANDLES 4

typedef struct {
    int  in_use;
    u32  maxWidth;
    u32  maxHeight;
} DivxSlot;

static DivxSlot s_slots[DIVX_MAX_HANDLES];

static int divx_alloc(void)
{
    for (int i = 0; i < DIVX_MAX_HANDLES; i++) {
        if (!s_slots[i].in_use) return i;
    }
    return -1;
}

/* API */

s32 cellVdecDivxOpen(const CellVdecDivxConfig* config, CellVdecDivxHandle* handle)
{
    printf("[cellVdecDivx] Open()\n");
    if (!handle) return (s32)CELL_VDEC_DIVX_ERROR_INVALID_ARGUMENT;

    int slot = divx_alloc();
    if (slot < 0) return (s32)CELL_VDEC_DIVX_ERROR_OUT_OF_MEMORY;

    s_slots[slot].in_use = 1;
    s_slots[slot].maxWidth = config ? config->maxWidth : 1920;
    s_slots[slot].maxHeight = config ? config->maxHeight : 1080;

    *handle = (CellVdecDivxHandle)slot;
    return CELL_OK;
}

s32 cellVdecDivxClose(CellVdecDivxHandle handle)
{
    printf("[cellVdecDivx] Close(%u)\n", handle);
    if (handle >= DIVX_MAX_HANDLES || !s_slots[handle].in_use)
        return (s32)CELL_VDEC_DIVX_ERROR_INVALID_ARGUMENT;

    s_slots[handle].in_use = 0;
    return CELL_OK;
}

s32 cellVdecDivxDecode(CellVdecDivxHandle handle, const void* au, u32 auSize,
                        void* picOut, u32 picBufSize, CellVdecDivxFrameInfo* info)
{
    (void)au; (void)auSize;

    if (handle >= DIVX_MAX_HANDLES || !s_slots[handle].in_use)
        return (s32)CELL_VDEC_DIVX_ERROR_INVALID_ARGUMENT;

    /* Output black frame if buffer provided */
    if (picOut && picBufSize > 0)
        memset(picOut, 0, picBufSize);

    if (info) {
        memset(info, 0, sizeof(*info));
        info->width = s_slots[handle].maxWidth;
        info->height = s_slots[handle].maxHeight;
    }
    return CELL_OK;
}

s32 cellVdecDivxReset(CellVdecDivxHandle handle)
{
    if (handle >= DIVX_MAX_HANDLES || !s_slots[handle].in_use)
        return (s32)CELL_VDEC_DIVX_ERROR_INVALID_ARGUMENT;
    return CELL_OK;
}
