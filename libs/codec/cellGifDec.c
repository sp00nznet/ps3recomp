/*
 * ps3recomp - cellGifDec HLE implementation
 *
 * Real GIF decoding using stb_image.h.
 * Guest memory is accessed through vm_base (host_ptr = vm_base + guest_addr).
 * PS3 file paths are translated through cellfs_translate_path().
 *
 * NOTE: STB_IMAGE_IMPLEMENTATION is defined in cellPngDec.c.
 *       This file only includes stb_image.h for the declarations.
 *       You must vendor stb_image.h at libs/codec/stb_image.h.
 */

#include "cellGifDec.h"
#include "cellFs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* vm_base: base pointer for the PS3 guest address space */
extern uint8_t* vm_base;

/* ---------------------------------------------------------------------------
 * stb_image inclusion (declarations only — impl is in cellPngDec.c)
 * -----------------------------------------------------------------------*/
#if __has_include("stb_image.h")
  #define GIFDEC_HAS_STB 1
  #include "stb_image.h"
#elif defined(PS3RECOMP_HAS_STB_IMAGE)
  #define GIFDEC_HAS_STB 1
  #include "stb_image.h"
#else
  #define GIFDEC_HAS_STB 0
  /* stb_image.h not found — place it at libs/codec/stb_image.h */
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
    /* Decoded dimensions (from stb or GIF header) */
    u32               image_width;
    u32               image_height;
} GifDecSubState;

static GifDecMainState s_gif_main[GIFDEC_MAX_HANDLES];
static GifDecSubState  s_gif_sub[GIFDEC_MAX_SUBHANDLES];

/* ---------------------------------------------------------------------------
 * Helper: load source data into host memory
 * -----------------------------------------------------------------------*/

static int gifdec_load_source(GifDecSubState* sub)
{
    if (sub->src_data) return 0;

    if (sub->src.srcSelect == CELL_GIFDEC_BUFFER) {
        /* streamPtr is a 32-bit guest address */
        u32 guest_addr = (u32)sub->src.streamPtr;
        if (!vm_base) {
            printf("[cellGifDec] ERROR: vm_base is NULL, cannot translate guest addr 0x%08X\n", guest_addr);
            return -1;
        }
        sub->src_data = vm_base + guest_addr;
        sub->src_size = sub->src.streamSize;
        sub->src_allocated = 0;
        return 0;
    }

    if (sub->src.srcSelect == CELL_GIFDEC_FILE) {
        char host_path[CELL_FS_MAX_FS_PATH_LENGTH];
        if (cellfs_translate_path(sub->src.fileName, host_path, sizeof(host_path)) != 0) {
            printf("[cellGifDec] Cannot translate path: %s\n", sub->src.fileName);
            return -1;
        }

        FILE* f = fopen(host_path, "rb");
        if (!f) {
            printf("[cellGifDec] Cannot open file: %s (host: %s)\n",
                   sub->src.fileName, host_path);
            return -1;
        }

        fseek(f, 0, SEEK_END);
        long total_size = ftell(f);

        long read_offset = (long)sub->src.fileOffset;
        long read_size;

        if (sub->src.fileSize > 0 && (long)sub->src.fileSize < (total_size - read_offset))
            read_size = (long)sub->src.fileSize;
        else
            read_size = total_size - read_offset;

        if (read_size <= 0) {
            fclose(f);
            printf("[cellGifDec] File is empty or offset past end: %s\n", host_path);
            return -1;
        }

        fseek(f, read_offset, SEEK_SET);

        sub->src_data = (u8*)malloc((size_t)read_size);
        if (!sub->src_data) { fclose(f); return -1; }
        sub->src_size = (u32)fread(sub->src_data, 1, (size_t)read_size, f);
        sub->src_allocated = 1;
        fclose(f);

        printf("[cellGifDec] Loaded %u bytes from '%s'\n", sub->src_size, host_path);
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
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellGifDecCreate(CellGifDecMainHandle* mainHandle,
                     const CellGifDecThreadInParam* threadInParam,
                     CellGifDecThreadOutParam* threadOutParam)
{
    (void)threadInParam;
    printf("[cellGifDec] Create()\n");

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
    printf("[cellGifDec] Destroy(handle=%u)\n", mainHandle);

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
    printf("[cellGifDec] Open(main=%u, srcSelect=%u)\n", mainHandle,
           src ? src->srcSelect : 0xFFFFFFFF);

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

            if (src->srcSelect == CELL_GIFDEC_FILE)
                printf("[cellGifDec]   file: '%s'\n", src->fileName);
            else if (src->srcSelect == CELL_GIFDEC_BUFFER)
                printf("[cellGifDec]   buffer: guest_addr=0x%08X size=%u\n",
                       (u32)src->streamPtr, src->streamSize);

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
    printf("[cellGifDec] ReadHeader(sub=%u)\n", subHandle);

    if (subHandle >= GIFDEC_MAX_SUBHANDLES || !s_gif_sub[subHandle].in_use)
        return CELL_GIFDEC_ERROR_SEQ;
    if (!info) return CELL_GIFDEC_ERROR_ARG;

    GifDecSubState* sub = &s_gif_sub[subHandle];
    if (gifdec_load_source(sub) < 0)
        return CELL_GIFDEC_ERROR_OPEN_FILE;

#if GIFDEC_HAS_STB
    {
        /* Use stb_image to validate and get dimensions */
        int w, h, comp;
        if (!stbi_info_from_memory(sub->src_data, (int)sub->src_size, &w, &h, &comp)) {
            printf("[cellGifDec] stbi_info failed: %s\n", stbi_failure_reason());
            /* Fall through to manual GIF header parse below */
            goto manual_parse;
        }

        sub->image_width  = (u32)w;
        sub->image_height = (u32)h;

        memset(info, 0, sizeof(CellGifDecInfo));
        info->SWidth  = (u32)w;
        info->SHeight = (u32)h;

        /* Also parse GIF LSD for the extra fields if we have enough data */
        if (sub->src_size >= 13 && memcmp(sub->src_data, "GIF", 3) == 0) {
            const u8* lsd = sub->src_data + 6;
            u8 packed = lsd[4];
            info->SGlobalColorTableFlag   = (packed >> 7) & 1;
            info->SColorResolution        = ((packed >> 4) & 0x07) + 1;
            info->SSortFlag               = (packed >> 3) & 1;
            info->SSizeOfGlobalColorTable = 1u << ((packed & 0x07) + 1);
            info->SBackGroundColor        = lsd[5];
            info->SPixelAspectRatio       = lsd[6];
        }

        printf("[cellGifDec]   %ux%u\n", (u32)w, (u32)h);

        sub->info = *info;
        sub->header_read = 1;
        return CELL_OK;
    }
manual_parse:
#endif
    /* Fallback: parse GIF header manually (13 bytes: signature + LSD) */
    if (sub->src_size < 13)
        return CELL_GIFDEC_ERROR_HEADER;

    if (memcmp(sub->src_data, "GIF", 3) != 0)
        return CELL_GIFDEC_ERROR_STREAM_FORMAT;

    const u8* lsd = sub->src_data + 6;
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

    printf("[cellGifDec]   %ux%u (header parse)\n", w, h);

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

    u32 out_comp = 4; /* GIF always decoded to 4-component (RGBA or ARGB) */
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
    printf("[cellGifDec] DecodeData(sub=%u)\n", subHandle);

    if (subHandle >= GIFDEC_MAX_SUBHANDLES || !s_gif_sub[subHandle].in_use)
        return CELL_GIFDEC_ERROR_SEQ;
    if (!data || !dataInfo) return CELL_GIFDEC_ERROR_ARG;

    GifDecSubState* sub = &s_gif_sub[subHandle];
    if (!sub->header_read || !sub->param_set)
        return CELL_GIFDEC_ERROR_SEQ;

#if GIFDEC_HAS_STB
    {
        int w, h, comp;
        /* Always decode GIF to 4 channels (RGBA) */
        u8* pixels = stbi_load_from_memory(sub->src_data, (int)sub->src_size,
                                            &w, &h, &comp, 4);
        if (!pixels) {
            printf("[cellGifDec] stbi_load failed: %s\n", stbi_failure_reason());
            dataInfo->status = CELL_GIFDEC_DEC_STATUS_STOP;
            return CELL_GIFDEC_ERROR_STREAM_FORMAT;
        }

        u32 total = (u32)w * (u32)h * 4;

        /* Handle ARGB: convert from RGBA to ARGB (PS3 native) */
        if (sub->in_param.outputColorSpace == CELL_GIFDEC_ARGB) {
            for (u32 i = 0; i < total; i += 4) {
                u8 r = pixels[i+0], g = pixels[i+1], b = pixels[i+2], a = pixels[i+3];
                pixels[i+0] = a; pixels[i+1] = r; pixels[i+2] = g; pixels[i+3] = b;
            }
        }

        memcpy(data, pixels, total);
        stbi_image_free(pixels);

        printf("[cellGifDec]   decoded %ux%u (%u bytes)\n", (u32)w, (u32)h, total);

        dataInfo->recordType = 0;
        dataInfo->status = CELL_GIFDEC_DEC_STATUS_FINISH;
        return CELL_OK;
    }
#else
    printf("[cellGifDec] DecodeData: stb_image not available — place stb_image.h in libs/codec/\n");
    dataInfo->status = CELL_GIFDEC_DEC_STATUS_STOP;
    return (s32)CELL_ENOSYS;
#endif
}

s32 cellGifDecClose(CellGifDecMainHandle mainHandle,
                    CellGifDecSubHandle subHandle)
{
    (void)mainHandle;
    printf("[cellGifDec] Close(sub=%u)\n", subHandle);

    if (subHandle >= GIFDEC_MAX_SUBHANDLES || !s_gif_sub[subHandle].in_use)
        return CELL_GIFDEC_ERROR_SEQ;

    gifdec_free_source(&s_gif_sub[subHandle]);
    s_gif_sub[subHandle].in_use = 0;
    return CELL_OK;
}
