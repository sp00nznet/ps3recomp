/*
 * ps3recomp - cellJpgDec HLE implementation
 *
 * JPEG decoding using stb_image.h. Same pattern as cellPngDec.
 * If stb_image.h is not available, returns CELL_ENOSYS for decode operations.
 */

#include "cellJpgDec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Try to include stb_image (may already be included by cellPngDec) */
#if __has_include("stb_image.h")
  #define JPGDEC_HAS_STB 1
  #ifndef STB_IMAGE_IMPLEMENTATION
    /* Already defined elsewhere */
  #endif
  #include "stb_image.h"
#elif defined(PS3RECOMP_HAS_STB_IMAGE)
  #define JPGDEC_HAS_STB 1
  #include "stb_image.h"
#else
  #define JPGDEC_HAS_STB 0
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

#define JPGDEC_MAX_HANDLES      4
#define JPGDEC_MAX_SUBHANDLES   8

typedef struct {
    int in_use;
} JpgDecMainState;

typedef struct {
    int               in_use;
    u32               main_handle;
    CellJpgDecSrc     src;
    CellJpgDecInfo    info;
    CellJpgDecInParam in_param;
    int               header_read;
    int               param_set;
    u8*               src_data;
    u32               src_size;
    int               src_allocated; /* 1 if we allocated src_data */
} JpgDecSubState;

static JpgDecMainState s_jpg_main[JPGDEC_MAX_HANDLES];
static JpgDecSubState  s_jpg_sub[JPGDEC_MAX_SUBHANDLES];

/* ---------------------------------------------------------------------------
 * Helper: load source data
 * -----------------------------------------------------------------------*/

static int jpgdec_load_source(JpgDecSubState* sub)
{
    if (sub->src_data) return 0;

    if (sub->src.srcSelect == CELL_JPGDEC_BUFFER) {
        sub->src_data = (u8*)(uintptr_t)sub->src.streamPtr;
        sub->src_size = sub->src.streamSize;
        sub->src_allocated = 0;
        return 0;
    }

    if (sub->src.srcSelect == CELL_JPGDEC_FILE) {
        FILE* f = fopen(sub->src.fileName, "rb");
        if (!f) {
            printf("[cellJpgDec] Cannot open file: %s\n", sub->src.fileName);
            return -1;
        }

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

static void jpgdec_free_source(JpgDecSubState* sub)
{
    if (sub->src_allocated && sub->src_data)
        free(sub->src_data);
    sub->src_data = NULL;
    sub->src_size = 0;
    sub->src_allocated = 0;
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellJpgDecCreate(CellJpgDecMainHandle* mainHandle,
                     const CellJpgDecThreadInParam* threadInParam,
                     CellJpgDecThreadOutParam* threadOutParam)
{
    (void)threadInParam;
    printf("[cellJpgDec] Create()\n");

    if (!mainHandle)
        return CELL_JPGDEC_ERROR_ARG;

    for (u32 i = 0; i < JPGDEC_MAX_HANDLES; i++) {
        if (!s_jpg_main[i].in_use) {
            s_jpg_main[i].in_use = 1;
            *mainHandle = i;
            if (threadOutParam)
                threadOutParam->jpgCodecVersion = 0x00010000;
            return CELL_OK;
        }
    }

    return CELL_JPGDEC_ERROR_BUSY;
}

s32 cellJpgDecDestroy(CellJpgDecMainHandle mainHandle)
{
    printf("[cellJpgDec] Destroy(handle=%u)\n", mainHandle);

    if (mainHandle >= JPGDEC_MAX_HANDLES || !s_jpg_main[mainHandle].in_use)
        return CELL_JPGDEC_ERROR_ARG;

    for (u32 i = 0; i < JPGDEC_MAX_SUBHANDLES; i++) {
        if (s_jpg_sub[i].in_use && s_jpg_sub[i].main_handle == mainHandle) {
            jpgdec_free_source(&s_jpg_sub[i]);
            s_jpg_sub[i].in_use = 0;
        }
    }

    s_jpg_main[mainHandle].in_use = 0;
    return CELL_OK;
}

s32 cellJpgDecOpen(CellJpgDecMainHandle mainHandle,
                   CellJpgDecSubHandle* subHandle,
                   const CellJpgDecSrc* src,
                   CellJpgDecOpnInfo* openInfo)
{
    printf("[cellJpgDec] Open(main=%u)\n", mainHandle);

    if (mainHandle >= JPGDEC_MAX_HANDLES || !s_jpg_main[mainHandle].in_use)
        return CELL_JPGDEC_ERROR_SEQ;

    if (!subHandle || !src)
        return CELL_JPGDEC_ERROR_ARG;

    for (u32 i = 0; i < JPGDEC_MAX_SUBHANDLES; i++) {
        if (!s_jpg_sub[i].in_use) {
            memset(&s_jpg_sub[i], 0, sizeof(JpgDecSubState));
            s_jpg_sub[i].in_use = 1;
            s_jpg_sub[i].main_handle = mainHandle;
            s_jpg_sub[i].src = *src;
            *subHandle = i;
            if (openInfo) openInfo->initSpaceAllocated = 0;
            return CELL_OK;
        }
    }

    return CELL_JPGDEC_ERROR_BUSY;
}

s32 cellJpgDecReadHeader(CellJpgDecMainHandle mainHandle,
                         CellJpgDecSubHandle subHandle,
                         CellJpgDecInfo* info)
{
    (void)mainHandle;
    printf("[cellJpgDec] ReadHeader(sub=%u)\n", subHandle);

    if (subHandle >= JPGDEC_MAX_SUBHANDLES || !s_jpg_sub[subHandle].in_use)
        return CELL_JPGDEC_ERROR_SEQ;

    if (!info)
        return CELL_JPGDEC_ERROR_ARG;

    JpgDecSubState* sub = &s_jpg_sub[subHandle];

    if (jpgdec_load_source(sub) < 0)
        return CELL_JPGDEC_ERROR_OPEN_FILE;

#if JPGDEC_HAS_STB
    {
        int w, h, comp;
        if (!stbi_info_from_memory(sub->src_data, (int)sub->src_size, &w, &h, &comp)) {
            printf("[cellJpgDec] stbi_info failed\n");
            return CELL_JPGDEC_ERROR_HEADER;
        }

        sub->info.imageWidth    = (u32)w;
        sub->info.imageHeight   = (u32)h;
        sub->info.numComponents = (u32)comp;

        switch (comp) {
            case 1: sub->info.colorSpace = CELL_JPGDEC_GRAYSCALE; break;
            case 3: sub->info.colorSpace = CELL_JPGDEC_RGB;       break;
            default: sub->info.colorSpace = CELL_JPGDEC_RGB;      break;
        }

        sub->header_read = 1;
        *info = sub->info;
        return CELL_OK;
    }
#else
    /* Minimal JPEG header parsing: look for SOI and SOF0 */
    if (sub->src_size < 4 || sub->src_data[0] != 0xFF || sub->src_data[1] != 0xD8)
        return CELL_JPGDEC_ERROR_STREAM_FORMAT;

    /* Scan for SOF0 marker (0xFF 0xC0) */
    for (u32 i = 2; i + 10 < sub->src_size; ) {
        if (sub->src_data[i] != 0xFF) { i++; continue; }
        u8 marker = sub->src_data[i + 1];
        if (marker == 0xC0 || marker == 0xC2) { /* SOF0 or SOF2 */
            u32 h = ((u32)sub->src_data[i+5] << 8) | sub->src_data[i+6];
            u32 w = ((u32)sub->src_data[i+7] << 8) | sub->src_data[i+8];
            u32 c = sub->src_data[i+9];

            sub->info.imageWidth    = w;
            sub->info.imageHeight   = h;
            sub->info.numComponents = c;
            sub->info.colorSpace    = (c == 1) ? CELL_JPGDEC_GRAYSCALE : CELL_JPGDEC_RGB;

            sub->header_read = 1;
            *info = sub->info;
            return CELL_OK;
        }
        /* Skip marker segment */
        if (marker == 0x00 || marker == 0xFF) { i++; continue; }
        if (i + 3 < sub->src_size) {
            u32 len = ((u32)sub->src_data[i+2] << 8) | sub->src_data[i+3];
            i += 2 + len;
        } else {
            break;
        }
    }

    return CELL_JPGDEC_ERROR_HEADER;
#endif
}

s32 cellJpgDecSetParameter(CellJpgDecMainHandle mainHandle,
                           CellJpgDecSubHandle subHandle,
                           const CellJpgDecInParam* inParam,
                           CellJpgDecOutParam* outParam)
{
    (void)mainHandle;

    if (subHandle >= JPGDEC_MAX_SUBHANDLES || !s_jpg_sub[subHandle].in_use)
        return CELL_JPGDEC_ERROR_SEQ;

    if (!inParam || !outParam)
        return CELL_JPGDEC_ERROR_ARG;

    JpgDecSubState* sub = &s_jpg_sub[subHandle];
    if (!sub->header_read)
        return CELL_JPGDEC_ERROR_SEQ;

    sub->in_param = *inParam;
    sub->param_set = 1;

    u32 out_comp;
    u32 out_cs = inParam->outputColorSpace;
    switch (out_cs) {
        case CELL_JPGDEC_GRAYSCALE: out_comp = 1; break;
        case CELL_JPGDEC_RGB:       out_comp = 3; break;
        case CELL_JPGDEC_RGBA:      out_comp = 4; break;
        case CELL_JPGDEC_ARGB:      out_comp = 4; break;
        default:                     out_comp = 3; out_cs = CELL_JPGDEC_RGB; break;
    }

    u32 ds = (inParam->downScale > 0) ? inParam->downScale : 1;
    u32 out_w = sub->info.imageWidth / ds;
    u32 out_h = sub->info.imageHeight / ds;
    if (out_w == 0) out_w = 1;
    if (out_h == 0) out_h = 1;

    outParam->outputWidth      = out_w;
    outParam->outputHeight     = out_h;
    outParam->outputComponents = out_comp;
    outParam->outputColorSpace = out_cs;
    outParam->outputWidthByte  = (u64)(out_w * out_comp);
    outParam->outputMode       = 0;
    outParam->downScale        = ds;
    outParam->useMemorySpace   = (u32)(outParam->outputWidthByte * out_h);

    return CELL_OK;
}

s32 cellJpgDecDecodeData(CellJpgDecMainHandle mainHandle,
                         CellJpgDecSubHandle subHandle,
                         u8* data,
                         CellJpgDecDataInfo* dataInfo)
{
    (void)mainHandle;

    if (subHandle >= JPGDEC_MAX_SUBHANDLES || !s_jpg_sub[subHandle].in_use)
        return CELL_JPGDEC_ERROR_SEQ;

    if (!data || !dataInfo)
        return CELL_JPGDEC_ERROR_ARG;

    JpgDecSubState* sub = &s_jpg_sub[subHandle];
    if (!sub->header_read || !sub->param_set)
        return CELL_JPGDEC_ERROR_SEQ;

#if JPGDEC_HAS_STB
    {
        int w, h, comp;
        int desired_comp;

        switch (sub->in_param.outputColorSpace) {
            case CELL_JPGDEC_GRAYSCALE: desired_comp = 1; break;
            case CELL_JPGDEC_RGB:       desired_comp = 3; break;
            case CELL_JPGDEC_RGBA:      desired_comp = 4; break;
            case CELL_JPGDEC_ARGB:      desired_comp = 4; break;
            default:                     desired_comp = 3; break;
        }

        u8* pixels = stbi_load_from_memory(sub->src_data, (int)sub->src_size,
                                            &w, &h, &comp, desired_comp);
        if (!pixels) {
            printf("[cellJpgDec] stbi_load failed: %s\n", stbi_failure_reason());
            dataInfo->status = CELL_JPGDEC_DEC_STATUS_STOP;
            return CELL_JPGDEC_ERROR_STREAM_FORMAT;
        }

        u32 total = (u32)w * (u32)h * (u32)desired_comp;

        /* Handle ARGB: convert from RGBA */
        if (sub->in_param.outputColorSpace == CELL_JPGDEC_ARGB && desired_comp == 4) {
            for (u32 i = 0; i < total; i += 4) {
                u8 r = pixels[i+0], g = pixels[i+1], b = pixels[i+2], a = pixels[i+3];
                pixels[i+0] = a; pixels[i+1] = r; pixels[i+2] = g; pixels[i+3] = b;
            }
        }

        memcpy(data, pixels, total);
        stbi_image_free(pixels);

        dataInfo->status = CELL_JPGDEC_DEC_STATUS_FINISH;
        return CELL_OK;
    }
#else
    printf("[cellJpgDec] DecodeData: stb_image not available\n");
    dataInfo->status = CELL_JPGDEC_DEC_STATUS_STOP;
    return (s32)CELL_ENOSYS;
#endif
}

s32 cellJpgDecClose(CellJpgDecMainHandle mainHandle,
                    CellJpgDecSubHandle subHandle)
{
    (void)mainHandle;

    if (subHandle >= JPGDEC_MAX_SUBHANDLES || !s_jpg_sub[subHandle].in_use)
        return CELL_JPGDEC_ERROR_SEQ;

    jpgdec_free_source(&s_jpg_sub[subHandle]);
    s_jpg_sub[subHandle].in_use = 0;
    return CELL_OK;
}
