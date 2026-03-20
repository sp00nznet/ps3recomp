/*
 * ps3recomp - cellPngEnc HLE implementation
 *
 * Real PNG encoding via stb_image_write.h.
 * Converts raw pixel data (ARGB/RGBA/grayscale) to PNG format in memory.
 */

#include "cellPngEnc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* stb_image_write for PNG encoding */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_ASSERT(x)
#include "stb_image_write.h"

/* Internal state */

typedef struct {
    int in_use;
    CellPngEncParam param;
} PngEncHandle;

static PngEncHandle s_handles[CELL_PNGENC_HANDLE_MAX];

/* stb callback: write PNG data to a growing buffer */
typedef struct {
    u8* data;
    u32 size;
    u32 capacity;
} PngWriteContext;

static void png_write_callback(void* context, void* data, int size)
{
    PngWriteContext* ctx = (PngWriteContext*)context;
    if (ctx->size + (u32)size > ctx->capacity) {
        u32 new_cap = ctx->capacity * 2;
        if (new_cap < ctx->size + (u32)size)
            new_cap = ctx->size + (u32)size + 4096;
        u8* new_data = (u8*)realloc(ctx->data, new_cap);
        if (!new_data) return;
        ctx->data = new_data;
        ctx->capacity = new_cap;
    }
    memcpy(ctx->data + ctx->size, data, (size_t)size);
    ctx->size += (u32)size;
}

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
    printf("[cellPngEnc] Create(%ux%u, colorSpace=%u)\n",
           param ? param->width : 0, param ? param->height : 0,
           param ? param->colorSpace : 0);
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
    if (handle >= CELL_PNGENC_HANDLE_MAX || !s_handles[handle].in_use)
        return (s32)CELL_PNGENC_ERROR_INVALID_ARGUMENT;
    if (!inputData || !outputInfo)
        return (s32)CELL_PNGENC_ERROR_INVALID_ARGUMENT;

    PngEncHandle* h = &s_handles[handle];
    u32 w = h->param.width;
    u32 h_px = h->param.height;

    /* Determine channel count from color space */
    int channels;
    switch (h->param.colorSpace) {
    case CELL_PNGENC_COLOR_SPACE_ARGB:
    case CELL_PNGENC_COLOR_SPACE_RGBA:
        channels = 4;
        break;
    case CELL_PNGENC_COLOR_SPACE_GRAYSCALE_A:
        channels = 2;
        break;
    case CELL_PNGENC_COLOR_SPACE_GRAYSCALE:
    default:
        channels = 1;
        break;
    }

    printf("[cellPngEnc] Encode(%ux%u, %d channels)\n", w, h_px, channels);

    /* For ARGB input, we need to swizzle to RGBA for stb */
    const u8* src = (const u8*)inputData;
    u8* rgba_buf = NULL;

    if (h->param.colorSpace == CELL_PNGENC_COLOR_SPACE_ARGB && channels == 4) {
        rgba_buf = (u8*)malloc(w * h_px * 4);
        if (!rgba_buf) return (s32)CELL_PNGENC_ERROR_OUT_OF_MEMORY;
        for (u32 i = 0; i < w * h_px; i++) {
            rgba_buf[i * 4 + 0] = src[i * 4 + 1]; /* R */
            rgba_buf[i * 4 + 1] = src[i * 4 + 2]; /* G */
            rgba_buf[i * 4 + 2] = src[i * 4 + 3]; /* B */
            rgba_buf[i * 4 + 3] = src[i * 4 + 0]; /* A */
        }
        src = rgba_buf;
    }

    /* Encode to PNG in memory */
    PngWriteContext ctx = {0};
    ctx.capacity = w * h_px * channels + 4096;
    ctx.data = (u8*)malloc(ctx.capacity);
    if (!ctx.data) {
        free(rgba_buf);
        return (s32)CELL_PNGENC_ERROR_OUT_OF_MEMORY;
    }

    int stride = (int)(w * channels);
    int result = stbi_write_png_to_func(png_write_callback, &ctx,
                                         (int)w, (int)h_px, channels, src, stride);

    free(rgba_buf);

    if (!result) {
        free(ctx.data);
        return (s32)CELL_PNGENC_ERROR_FATAL;
    }

    outputInfo->outputData = ctx.data;
    outputInfo->outputSize = ctx.size;
    printf("[cellPngEnc] Encode complete: %u bytes\n", ctx.size);

    return CELL_OK;
}

s32 cellPngEncReset(CellPngEncHandle handle)
{
    if (handle >= CELL_PNGENC_HANDLE_MAX || !s_handles[handle].in_use)
        return (s32)CELL_PNGENC_ERROR_INVALID_ARGUMENT;
    return CELL_OK;
}
