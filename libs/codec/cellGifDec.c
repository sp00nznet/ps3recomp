/*
 * ps3recomp - cellGifDec HLE implementation
 *
 * GIF decoding using stb_image.h. Same pattern as cellPngDec/cellJpgDec.
 */

#include "cellGifDec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Try to include stb_image */
#if __has_include("stb_image.h")
  #define GIFDEC_HAS_STB 1
  #ifndef STB_IMAGE_IMPLEMENTATION
    /* Already defined elsewhere */
  #endif
  #include "stb_image.h"
#elif defined(PS3RECOMP_HAS_STB_IMAGE)
  #define GIFDEC_HAS_STB 1
  #include "stb_image.h"
#else
  #define GIFDEC_HAS_STB 0
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

#define GIFDEC_MAX_HANDLES      4
#define GIFDEC_MAX_SUBHANDLES   8

typedef struct { int in_use; } GifDecMainState;

typedef struct {
    int               in_use;
    u32               main_handle;
    CellGifDecSrc     src;
    CellGifDecInfo    info;
    CellGifDecInParam in_param;
    int               header_read;
    int               param_set;
    u8*               src_data;
    u32               src_size;
    int               src_allocated;
    /* Decoded dimensions (from stb or header) */
    u32               image_width;
    u32               image_height;
} GifDecSubState;

static GifDecMainState s_gif_main[GIFDEC_MAX_HANDLES];
static GifDecSubState  s_gif_sub[GIFDEC_MAX_SUBHANDLES];

static int gifdec_load_source(GifDecSubState* sub)
{
    if (sub->src_data) return 0;

    if (sub->src.srcSelect == CELL_GIFDEC_BUFFER) {
        sub->src_data = (u8*)(uintptr_t)sub->src.streamPtr;
        sub->src_size = sub->src.streamSize;
        sub->src_allocated = 0;
        return 0;
    }

    if (sub->src.srcSelect == CELL_GIFDEC_FILE) {
        FILE* f = fopen(sub->src.fileName, "rb");
        if (!f) return -1;

        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, (long)sub->src.fileOffset, SEEK_SET);

        if (sub->src.fileSize > 0 && sub->src.fileSize < (u32)size)
            size = (long)sub->src.fileSize;
        else
            size -= (long)sub->src.fileOffset;

        sub->src_data = (u8*)malloc((size_t)size);
        if (!sub->src_data) { fclose(f); return -1; }
        sub->src_size = (u32)fread(sub->src_data, 1, (size_t)size, f);
        sub->src_allocated = 1;
        fclose(f);
        return 0;
    }

    return -1;
}

static void gifdec_free_source(GifDecSubState* sub)
{
    if (sub->src_allocated && sub->src_data)
        free(sub->src_data);
    sub->src_data = NULL;
    sub->src_size = 0;
    sub->src_allocated = 0;
}

/* ---------------------------------------------------------------------------
 * API
 * -----------------------------------------------------------------------*/

s32 cellGifDecCreate(CellGifDecMainHandle* mainHandle,
                     const CellGifDecThreadInParam* threadInParam,
                     CellGifDecThreadOutParam* threadOutParam)
{
    (void)threadInParam;
    if (!mainHandle) return CELL_GIFDEC_ERROR_ARG;

    for (u32 i = 0; i < GIFDEC_MAX_HANDLES; i++) {
        if (!s_gif_main[i].in_use) {
            s_gif_main[i].in_use = 1;
            *mainHandle = i;
            if (threadOutParam) threadOutParam->gifCodecVersion = 0x00010000;
            return CELL_OK;
        }
    }
    return CELL_GIFDEC_ERROR_BUSY;
}

s32 cellGifDecDestroy(CellGifDecMainHandle mainHandle)
{
    if (mainHandle >= GIFDEC_MAX_HANDLES || !s_gif_main[mainHandle].in_use)
        return CELL_GIFDEC_ERROR_ARG;

    for (u32 i = 0; i < GIFDEC_MAX_SUBHANDLES; i++) {
        if (s_gif_sub[i].in_use && s_gif_sub[i].main_handle == mainHandle) {
            gifdec_free_source(&s_gif_sub[i]);
            s_gif_sub[i].in_use = 0;
        }
    }
    s_gif_main[mainHandle].in_use = 0;
    return CELL_OK;
}

s32 cellGifDecOpen(CellGifDecMainHandle mainHandle,
                   CellGifDecSubHandle* subHandle,
                   const CellGifDecSrc* src,
                   CellGifDecOpnInfo* openInfo)
{
    if (mainHandle >= GIFDEC_MAX_HANDLES || !s_gif_main[mainHandle].in_use)
        return CELL_GIFDEC_ERROR_SEQ;
    if (!subHandle || !src) return CELL_GIFDEC_ERROR_ARG;

    for (u32 i = 0; i < GIFDEC_MAX_SUBHANDLES; i++) {
        if (!s_gif_sub[i].in_use) {
            memset(&s_gif_sub[i], 0, sizeof(GifDecSubState));
            s_gif_sub[i].in_use = 1;
            s_gif_sub[i].main_handle = mainHandle;
            s_gif_sub[i].src = *src;
            *subHandle = i;
            if (openInfo) openInfo->initSpaceAllocated = 0;
            return CELL_OK;
        }
    }
    return CELL_GIFDEC_ERROR_BUSY;
}

s32 cellGifDecReadHeader(CellGifDecMainHandle mainHandle,
                         CellGifDecSubHandle subHandle,
                         CellGifDecInfo* info)
{
    (void)mainHandle;
    if (subHandle >= GIFDEC_MAX_SUBHANDLES || !s_gif_sub[subHandle].in_use)
        return CELL_GIFDEC_ERROR_SEQ;
    if (!info) return CELL_GIFDEC_ERROR_ARG;

    GifDecSubState* sub = &s_gif_sub[subHandle];
    if (gifdec_load_source(sub) < 0)
        return CELL_GIFDEC_ERROR_OPEN_FILE;

    /* Parse GIF header (at minimum 13 bytes: signature + logical screen descriptor) */
    if (sub->src_size < 13)
        return CELL_GIFDEC_ERROR_HEADER;

    /* Check GIF signature: "GIF87a" or "GIF89a" */
    if (memcmp(sub->src_data, "GIF", 3) != 0)
        return CELL_GIFDEC_ERROR_STREAM_FORMAT;

    const u8* lsd = sub->src_data + 6; /* Logical Screen Descriptor */
    u32 w = (u32)lsd[0] | ((u32)lsd[1] << 8);
    u32 h = (u32)lsd[2] | ((u32)lsd[3] << 8);
    u8 packed = lsd[4];

    sub->image_width  = w;
    sub->image_height = h;

    memset(info, 0, sizeof(CellGifDecInfo));
    info->SWidth  = w;
    info->SHeight = h;
    info->SGlobalColorTableFlag    = (packed >> 7) & 1;
    info->SColorResolution         = ((packed >> 4) & 0x07) + 1;
    info->SSortFlag                = (packed >> 3) & 1;
    info->SSizeOfGlobalColorTable  = 1u << ((packed & 0x07) + 1);
    info->SBackGroundColor         = lsd[5];
    info->SPixelAspectRatio        = lsd[6];

    sub->info = *info;
    sub->header_read = 1;
    return CELL_OK;
}

s32 cellGifDecSetParameter(CellGifDecMainHandle mainHandle,
                           CellGifDecSubHandle subHandle,
                           const CellGifDecInParam* inParam,
                           CellGifDecOutParam* outParam)
{
    (void)mainHandle;
    if (subHandle >= GIFDEC_MAX_SUBHANDLES || !s_gif_sub[subHandle].in_use)
        return CELL_GIFDEC_ERROR_SEQ;
    if (!inParam || !outParam) return CELL_GIFDEC_ERROR_ARG;

    GifDecSubState* sub = &s_gif_sub[subHandle];
    if (!sub->header_read) return CELL_GIFDEC_ERROR_SEQ;

    sub->in_param = *inParam;
    sub->param_set = 1;

    u32 out_comp = 4; /* GIF always decoded to RGBA */
    outParam->outputWidth      = sub->image_width;
    outParam->outputHeight     = sub->image_height;
    outParam->outputComponents = out_comp;
    outParam->outputColorSpace = inParam->outputColorSpace ? inParam->outputColorSpace : CELL_GIFDEC_RGBA;
    outParam->outputWidthByte  = (u64)(sub->image_width * out_comp);
    outParam->useMemorySpace   = (u32)(outParam->outputWidthByte * sub->image_height);

    return CELL_OK;
}

s32 cellGifDecDecodeData(CellGifDecMainHandle mainHandle,
                         CellGifDecSubHandle subHandle,
                         u8* data,
                         CellGifDecDataInfo* dataInfo)
{
    (void)mainHandle;
    if (subHandle >= GIFDEC_MAX_SUBHANDLES || !s_gif_sub[subHandle].in_use)
        return CELL_GIFDEC_ERROR_SEQ;
    if (!data || !dataInfo) return CELL_GIFDEC_ERROR_ARG;

    GifDecSubState* sub = &s_gif_sub[subHandle];
    if (!sub->header_read || !sub->param_set)
        return CELL_GIFDEC_ERROR_SEQ;

#if GIFDEC_HAS_STB
    {
        int w, h, comp;
        u8* pixels = stbi_load_from_memory(sub->src_data, (int)sub->src_size,
                                            &w, &h, &comp, 4);
        if (!pixels) {
            dataInfo->status = CELL_GIFDEC_DEC_STATUS_STOP;
            return CELL_GIFDEC_ERROR_STREAM_FORMAT;
        }

        u32 total = (u32)w * (u32)h * 4;

        if (sub->in_param.outputColorSpace == CELL_GIFDEC_ARGB) {
            for (u32 i = 0; i < total; i += 4) {
                u8 r = pixels[i+0], g = pixels[i+1], b = pixels[i+2], a = pixels[i+3];
                pixels[i+0] = a; pixels[i+1] = r; pixels[i+2] = g; pixels[i+3] = b;
            }
        }

        memcpy(data, pixels, total);
        stbi_image_free(pixels);

        dataInfo->recordType = 0;
        dataInfo->status = CELL_GIFDEC_DEC_STATUS_FINISH;
        return CELL_OK;
    }
#else
    printf("[cellGifDec] DecodeData: stb_image not available\n");
    dataInfo->status = CELL_GIFDEC_DEC_STATUS_STOP;
    return (s32)CELL_ENOSYS;
#endif
}

s32 cellGifDecClose(CellGifDecMainHandle mainHandle,
                    CellGifDecSubHandle subHandle)
{
    (void)mainHandle;
    if (subHandle >= GIFDEC_MAX_SUBHANDLES || !s_gif_sub[subHandle].in_use)
        return CELL_GIFDEC_ERROR_SEQ;

    gifdec_free_source(&s_gif_sub[subHandle]);
    s_gif_sub[subHandle].in_use = 0;
    return CELL_OK;
}
