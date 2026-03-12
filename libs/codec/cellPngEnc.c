/*
 * ps3recomp - cellPngEnc HLE implementation
 *
 * Stub. Handle management works, encode returns NOT_SUPPORTED.
 * A real implementation would use stb_image_write or libpng.
 */

#include "cellPngEnc.h"
#include <stdio.h>
#include <string.h>

/* Internal state */

typedef struct {
    int in_use;
    CellPngEncParam param;
} PngEncHandle;

static PngEncHandle s_handles[CELL_PNGENC_HANDLE_MAX];

/* API */

s32 cellPngEncQueryAttr(const CellPngEncParam* param, u32* workMemSize)
{
    (void)param;
    if (!workMemSize) return (s32)CELL_PNGENC_ERROR_INVALID_ARGUMENT;
    *workMemSize = 1024 * 1024; /* 1 MB */
    return CELL_OK;
}

s32 cellPngEncCreate(const CellPngEncParam* param, CellPngEncHandle* handle)
{
    printf("[cellPngEnc] Create()\n");
    if (!param || !handle)
        return (s32)CELL_PNGENC_ERROR_INVALID_ARGUMENT;

    for (int i = 0; i < CELL_PNGENC_HANDLE_MAX; i++) {
        if (!s_handles[i].in_use) {
            memset(&s_handles[i], 0, sizeof(PngEncHandle));
            s_handles[i].in_use = 1;
            s_handles[i].param = *param;
            *handle = (u32)i;
            return CELL_OK;
        }
    }
    return (s32)CELL_PNGENC_ERROR_OUT_OF_MEMORY;
}

s32 cellPngEncDestroy(CellPngEncHandle handle)
{
    printf("[cellPngEnc] Destroy(%u)\n", handle);
    if (handle >= CELL_PNGENC_HANDLE_MAX || !s_handles[handle].in_use)
        return (s32)CELL_PNGENC_ERROR_INVALID_ARGUMENT;
    s_handles[handle].in_use = 0;
    return CELL_OK;
}

s32 cellPngEncEncode(CellPngEncHandle handle, const void* inputData,
                       CellPngEncOutputInfo* outputInfo)
{
    (void)inputData;
    printf("[cellPngEnc] Encode() - not supported\n");
    if (handle >= CELL_PNGENC_HANDLE_MAX || !s_handles[handle].in_use)
        return (s32)CELL_PNGENC_ERROR_INVALID_ARGUMENT;
    if (!outputInfo) return (s32)CELL_PNGENC_ERROR_INVALID_ARGUMENT;
    return (s32)CELL_PNGENC_ERROR_NOT_SUPPORTED;
}

s32 cellPngEncReset(CellPngEncHandle handle)
{
    if (handle >= CELL_PNGENC_HANDLE_MAX || !s_handles[handle].in_use)
        return (s32)CELL_PNGENC_ERROR_INVALID_ARGUMENT;
    return CELL_OK;
}
