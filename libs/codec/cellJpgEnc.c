/*
 * ps3recomp - cellJpgEnc HLE implementation
 *
 * Real JPEG encoding via stb_image_write.h.
 * stb_image_write is compiled in cellPngEnc.c (STB_IMAGE_WRITE_IMPLEMENTATION).
 */

#include "cellJpgEnc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* stb_image_write (declarations only — implementation in cellPngEnc.c) */
#include "stb_image_write.h"

/* Internal state */

typedef struct {
    int in_use;
    CellJpgEncParam param;
} JpgEncHandle;

static JpgEncHandle s_handles[CELL_JPGENC_HANDLE_MAX];

/* Write callback */
typedef struct { u8* data; u32 size; u32 capacity; } JpgWriteCtx;

static void jpg_write_cb(void* context, void* data, int size)
{
    JpgWriteCtx* ctx = (JpgWriteCtx*)context;
    if (ctx->size + (u32)size > ctx->capacity) {
        u32 new_cap = (ctx->capacity * 2 > ctx->size + (u32)size)
                      ? ctx->capacity * 2 : ctx->size + (u32)size + 4096;
        u8* p = (u8*)realloc(ctx->data, new_cap);
        if (!p) return;
        ctx->data = p;
        ctx->capacity = new_cap;
    }
    memcpy(ctx->data + ctx->size, data, (size_t)size);
    ctx->size += (u32)size;
}

/* API */

s32 cellJpgEncQueryAttr(const CellJpgEncParam* param, u32* workMemSize)
{
    (void)param;
    if (!workMemSize) return (s32)CELL_JPGENC_ERROR_INVALID_ARGUMENT;
    *workMemSize = 1024 * 1024;
    return CELL_OK;
}

s32 cellJpgEncCreate(const CellJpgEncParam* param, CellJpgEncHandle* handle)
{
    printf("[cellJpgEnc] Create(%ux%u, quality=%u)\n",
           param ? param->width : 0, param ? param->height : 0,
           param ? param->quality : 0);
    if (!param || !handle) return (s32)CELL_JPGENC_ERROR_INVALID_ARGUMENT;

    for (int i = 0; i < CELL_JPGENC_HANDLE_MAX; i++) {
        if (!s_handles[i].in_use) {
            memset(&s_handles[i], 0, sizeof(JpgEncHandle));
            s_handles[i].in_use = 1;
            s_handles[i].param = *param;
            if (s_handles[i].param.quality == 0) s_handles[i].param.quality = 85;
            *handle = (u32)i;
            return CELL_OK;
        }
    }
    return (s32)CELL_JPGENC_ERROR_OUT_OF_MEMORY;
}

s32 cellJpgEncDestroy(CellJpgEncHandle handle)
{
    if (handle >= CELL_JPGENC_HANDLE_MAX || !s_handles[handle].in_use)
        return (s32)CELL_JPGENC_ERROR_INVALID_ARGUMENT;
    s_handles[handle].in_use = 0;
    return CELL_OK;
}

s32 cellJpgEncEncode(CellJpgEncHandle handle, const void* inputData,
                       CellJpgEncOutputInfo* outputInfo)
{
    if (handle >= CELL_JPGENC_HANDLE_MAX || !s_handles[handle].in_use)
        return (s32)CELL_JPGENC_ERROR_INVALID_ARGUMENT;
    if (!inputData || !outputInfo) return (s32)CELL_JPGENC_ERROR_INVALID_ARGUMENT;

    JpgEncHandle* h = &s_handles[handle];
    u32 w = h->param.width, hp = h->param.height;
    int ch = 3;
    const u8* src = (const u8*)inputData;
    u8* rgb = NULL;

    if (h->param.colorSpace == CELL_JPGENC_COLOR_SPACE_ARGB) {
        rgb = (u8*)malloc(w * hp * 3);
        if (!rgb) return (s32)CELL_JPGENC_ERROR_OUT_OF_MEMORY;
        for (u32 i = 0; i < w * hp; i++) {
            rgb[i*3] = src[i*4+1]; rgb[i*3+1] = src[i*4+2]; rgb[i*3+2] = src[i*4+3];
        }
        src = rgb;
    } else if (h->param.colorSpace == CELL_JPGENC_COLOR_SPACE_RGBA) {
        rgb = (u8*)malloc(w * hp * 3);
        if (!rgb) return (s32)CELL_JPGENC_ERROR_OUT_OF_MEMORY;
        for (u32 i = 0; i < w * hp; i++) {
            rgb[i*3] = src[i*4]; rgb[i*3+1] = src[i*4+1]; rgb[i*3+2] = src[i*4+2];
        }
        src = rgb;
    } else if (h->param.colorSpace == CELL_JPGENC_COLOR_SPACE_GRAYSCALE) {
        ch = 1;
    }

    JpgWriteCtx ctx = {0};
    ctx.capacity = w * hp * ch;
    ctx.data = (u8*)malloc(ctx.capacity);
    if (!ctx.data) { free(rgb); return (s32)CELL_JPGENC_ERROR_OUT_OF_MEMORY; }

    int ok = stbi_write_jpg_to_func(jpg_write_cb, &ctx, (int)w, (int)hp,
                                     ch, src, (int)h->param.quality);
    free(rgb);
    if (!ok) { free(ctx.data); return (s32)CELL_JPGENC_ERROR_FATAL; }

    outputInfo->outputData = ctx.data;
    outputInfo->outputSize = ctx.size;
    return CELL_OK;
}

s32 cellJpgEncReset(CellJpgEncHandle handle)
{
    if (handle >= CELL_JPGENC_HANDLE_MAX || !s_handles[handle].in_use)
        return (s32)CELL_JPGENC_ERROR_INVALID_ARGUMENT;
    return CELL_OK;
}
