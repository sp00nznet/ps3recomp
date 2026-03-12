/*
 * ps3recomp - cellJpgEnc HLE implementation
 *
 * Stub. Handle management works, encode returns NOT_SUPPORTED.
 * A real implementation would use stb_image_write or libjpeg-turbo.
 */

#include "cellJpgEnc.h"
#include <stdio.h>
#include <string.h>

/* Internal state */

typedef struct {
    int in_use;
    CellJpgEncParam param;
} JpgEncHandle;

static JpgEncHandle s_handles[CELL_JPGENC_HANDLE_MAX];

/* API */

s32 cellJpgEncQueryAttr(const CellJpgEncParam* param, u32* workMemSize)
{
    (void)param;
    if (!workMemSize) return (s32)CELL_JPGENC_ERROR_INVALID_ARGUMENT;
    *workMemSize = 512 * 1024; /* 512 KB */
    return CELL_OK;
}

s32 cellJpgEncCreate(const CellJpgEncParam* param, CellJpgEncHandle* handle)
{
    printf("[cellJpgEnc] Create()\n");
    if (!param || !handle)
        return (s32)CELL_JPGENC_ERROR_INVALID_ARGUMENT;

    for (int i = 0; i < CELL_JPGENC_HANDLE_MAX; i++) {
        if (!s_handles[i].in_use) {
            memset(&s_handles[i], 0, sizeof(JpgEncHandle));
            s_handles[i].in_use = 1;
            s_handles[i].param = *param;
            *handle = (u32)i;
            return CELL_OK;
        }
    }
    return (s32)CELL_JPGENC_ERROR_OUT_OF_MEMORY;
}

s32 cellJpgEncDestroy(CellJpgEncHandle handle)
{
    printf("[cellJpgEnc] Destroy(%u)\n", handle);
    if (handle >= CELL_JPGENC_HANDLE_MAX || !s_handles[handle].in_use)
        return (s32)CELL_JPGENC_ERROR_INVALID_ARGUMENT;
    s_handles[handle].in_use = 0;
    return CELL_OK;
}

s32 cellJpgEncEncode(CellJpgEncHandle handle, const void* inputData,
                       CellJpgEncOutputInfo* outputInfo)
{
    (void)inputData;
    printf("[cellJpgEnc] Encode() - not supported\n");
    if (handle >= CELL_JPGENC_HANDLE_MAX || !s_handles[handle].in_use)
        return (s32)CELL_JPGENC_ERROR_INVALID_ARGUMENT;
    if (!outputInfo) return (s32)CELL_JPGENC_ERROR_INVALID_ARGUMENT;
    return (s32)CELL_JPGENC_ERROR_NOT_SUPPORTED;
}

s32 cellJpgEncReset(CellJpgEncHandle handle)
{
    if (handle >= CELL_JPGENC_HANDLE_MAX || !s_handles[handle].in_use)
        return (s32)CELL_JPGENC_ERROR_INVALID_ARGUMENT;
    return CELL_OK;
}
