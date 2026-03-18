/*
 * ps3recomp - cellPngDec HLE implementation
 *
 * Real PNG decoding using stb_image.h.
 * Guest memory is accessed through vm_base (host_ptr = vm_base + guest_addr).
 * PS3 file paths are translated through cellfs_translate_path().
 *
 * NOTE: You must vendor stb_image.h at libs/codec/stb_image.h.
 *       Download from https://github.com/nothings/stb/blob/master/stb_image.h
 */

#include "cellPngDec.h"
#include "cellFs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* vm_base: base pointer for the PS3 guest address space */
extern uint8_t* vm_base;

/* ---------------------------------------------------------------------------
 * stb_image inclusion
 *
 * cellPngDec.c is the "owner" TU — it defines STB_IMAGE_IMPLEMENTATION.
 * cellJpgDec.c and cellGifDec.c just include the header without the impl.
 * -----------------------------------------------------------------------*/
#if __has_include("stb_image.h")
  #define PNGDEC_HAS_STB 1
  #define STB_IMAGE_IMPLEMENTATION
  #define STBI_ONLY_PNG
  #define STBI_ONLY_JPEG
  #define STBI_ONLY_GIF
  #include "stb_image.h"
#elif defined(PS3RECOMP_HAS_STB_IMAGE)
  #define PNGDEC_HAS_STB 1
  #include "stb_image.h"
#else
  #define PNGDEC_HAS_STB 0
  /* stb_image.h not found — place it at libs/codec/stb_image.h */
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

#define PNGDEC_MAX_HANDLES      4
#define PNGDEC_MAX_SUBHANDLES   8

typedef struct {
    int  in_use;
} PngDecMainState;

typedef struct {
    int               in_use;
    u32               main_handle;
    CellPngDecSrc     src;
    CellPngDecInfo    info;
    CellPngDecInParam in_param;
    int               header_read;
    int               param_set;
    /* Source data loaded into host memory for stb_image */
    u8*               src_data;
    u32               src_size;
    int               src_allocated; /* 1 if we malloc'd src_data */
} PngDecSubState;

static PngDecMainState s_png_main[PNGDEC_MAX_HANDLES];
static PngDecSubState  s_png_sub[PNGDEC_MAX_SUBHANDLES];

/* ---------------------------------------------------------------------------
 * Helper: load source data into host memory
 *
 * For BUFFER sources, the guest provides a PS3 address — we translate it
 * through vm_base. For FILE sources, we translate the PS3 path to a host
 * path and read the file.
 * -----------------------------------------------------------------------*/

static int pngdec_load_source(PngDecSubState* sub)
{
    if (sub->src_data) return 0; /* already loaded */

    if (sub->src.srcSelect == CELL_PNGDEC_BUFFER) {
        /* streamPtr is a 32-bit guest address */
        u32 guest_addr = (u32)sub->src.streamPtr;
        if (!vm_base) {
            printf("[cellPngDec] ERROR: vm_base is NULL, cannot translate guest addr 0x%08X\n", guest_addr);
            return -1;
        }
        sub->src_data = vm_base + guest_addr;
        sub->src_size = sub->src.streamSize;
        sub->src_allocated = 0;
        return 0;
    }

    if (sub->src.srcSelect == CELL_PNGDEC_FILE) {
        /* Translate PS3 path (e.g. "/dev_hdd0/game/NPUA80001/ICON0.PNG")
         * to a host filesystem path. */
        char host_path[CELL_FS_MAX_FS_PATH_LENGTH];
        if (cellfs_translate_path(sub->src.fileName, host_path, sizeof(host_path)) != 0) {
            printf("[cellPngDec] Cannot translate path: %s\n", sub->src.fileName);
            return -1;
        }

        FILE* f = fopen(host_path, "rb");
        if (!f) {
            printf("[cellPngDec] Cannot open file: %s (host: %s)\n",
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
            printf("[cellPngDec] File is empty or offset past end: %s\n", host_path);
            return -1;
        }

        fseek(f, read_offset, SEEK_SET);

        sub->src_data = (u8*)malloc((size_t)read_size);
        if (!sub->src_data) {
            fclose(f);
            return -1;
        }

        sub->src_size = (u32)fread(sub->src_data, 1, (size_t)read_size, f);
        sub->src_allocated = 1;
        fclose(f);

        printf("[cellPngDec] Loaded %u bytes from '%s'\n", sub->src_size, host_path);
        return 0;
    }

    return -1;
}

static void pngdec_free_source(PngDecSubState* sub)
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

s32 cellPngDecCreate(CellPngDecMainHandle* mainHandle,
                     const CellPngDecThreadInParam* threadInParam,
                     CellPngDecThreadOutParam* threadOutParam)
{
    (void)threadInParam;
    printf("[cellPngDec] Create()\n");

    if (!mainHandle)
        return CELL_PNGDEC_ERROR_ARG;

    for (u32 i = 0; i < PNGDEC_MAX_HANDLES; i++) {
        if (!s_png_main[i].in_use) {
            s_png_main[i].in_use = 1;
            *mainHandle = i;
            if (threadOutParam)
                threadOutParam->pngCodecVersion = 0x00010000;
            return CELL_OK;
        }
    }

    return CELL_PNGDEC_ERROR_BUSY;
}

s32 cellPngDecDestroy(CellPngDecMainHandle mainHandle)
{
    printf("[cellPngDec] Destroy(handle=%u)\n", mainHandle);

    if (mainHandle >= PNGDEC_MAX_HANDLES || !s_png_main[mainHandle].in_use)
        return CELL_PNGDEC_ERROR_ARG;

    for (u32 i = 0; i < PNGDEC_MAX_SUBHANDLES; i++) {
        if (s_png_sub[i].in_use && s_png_sub[i].main_handle == mainHandle) {
            pngdec_free_source(&s_png_sub[i]);
            s_png_sub[i].in_use = 0;
        }
    }

    s_png_main[mainHandle].in_use = 0;
    return CELL_OK;
}

s32 cellPngDecOpen(CellPngDecMainHandle mainHandle,
                   CellPngDecSubHandle* subHandle,
                   const CellPngDecSrc* src,
                   CellPngDecOpnInfo* openInfo)
{
    printf("[cellPngDec] Open(main=%u, srcSelect=%u)\n", mainHandle,
           src ? src->srcSelect : 0xFFFFFFFF);

    if (mainHandle >= PNGDEC_MAX_HANDLES || !s_png_main[mainHandle].in_use)
        return CELL_PNGDEC_ERROR_SEQ;

    if (!subHandle || !src)
        return CELL_PNGDEC_ERROR_ARG;

    for (u32 i = 0; i < PNGDEC_MAX_SUBHANDLES; i++) {
        if (!s_png_sub[i].in_use) {
            memset(&s_png_sub[i], 0, sizeof(PngDecSubState));
            s_png_sub[i].in_use = 1;
            s_png_sub[i].main_handle = mainHandle;
            s_png_sub[i].src = *src;
            *subHandle = i;
            if (openInfo)
                openInfo->initSpaceAllocated = 0;

            if (src->srcSelect == CELL_PNGDEC_FILE)
                printf("[cellPngDec]   file: '%s'\n", src->fileName);
            else if (src->srcSelect == CELL_PNGDEC_BUFFER)
                printf("[cellPngDec]   buffer: guest_addr=0x%08X size=%u\n",
                       (u32)src->streamPtr, src->streamSize);

            return CELL_OK;
        }
    }

    return CELL_PNGDEC_ERROR_BUSY;
}

s32 cellPngDecReadHeader(CellPngDecMainHandle mainHandle,
                         CellPngDecSubHandle subHandle,
                         CellPngDecInfo* info)
{
    (void)mainHandle;
    printf("[cellPngDec] ReadHeader(sub=%u)\n", subHandle);

    if (subHandle >= PNGDEC_MAX_SUBHANDLES || !s_png_sub[subHandle].in_use)
        return CELL_PNGDEC_ERROR_SEQ;

    if (!info)
        return CELL_PNGDEC_ERROR_ARG;

    PngDecSubState* sub = &s_png_sub[subHandle];

    if (pngdec_load_source(sub) < 0)
        return CELL_PNGDEC_ERROR_OPEN_FILE;

#if PNGDEC_HAS_STB
    {
        int w, h, comp;
        if (!stbi_info_from_memory(sub->src_data, (int)sub->src_size, &w, &h, &comp)) {
            printf("[cellPngDec] stbi_info failed: %s\n", stbi_failure_reason());
            return CELL_PNGDEC_ERROR_HEADER;
        }

        sub->info.imageWidth    = (u32)w;
        sub->info.imageHeight   = (u32)h;
        sub->info.numComponents = (u32)comp;
        sub->info.bitDepth      = 8;
        sub->info.interlaceMethod = 0;
        sub->info.chunkInformation = 0;

        switch (comp) {
            case 1: sub->info.colorSpace = CELL_PNGDEC_GRAYSCALE; break;
            case 2: sub->info.colorSpace = CELL_PNGDEC_GRAYSCALE_ALPHA; break;
            case 3: sub->info.colorSpace = CELL_PNGDEC_RGB; break;
            case 4: sub->info.colorSpace = CELL_PNGDEC_RGBA; break;
            default: sub->info.colorSpace = CELL_PNGDEC_RGBA; break;
        }

        printf("[cellPngDec]   %ux%u, %u components\n", (u32)w, (u32)h, (u32)comp);

        sub->header_read = 1;
        *info = sub->info;
        return CELL_OK;
    }
#else
    /* Fallback: minimal PNG header parsing without stb_image */
    if (sub->src_size < 33) /* minimum: 8 sig + 25 IHDR */
        return CELL_PNGDEC_ERROR_HEADER;

    /* Check PNG signature */
    static const u8 png_sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (memcmp(sub->src_data, png_sig, 8) != 0)
        return CELL_PNGDEC_ERROR_STREAM_FORMAT;

    /* Parse IHDR (starts at offset 8, length=4, type=4, data=13) */
    const u8* ihdr = sub->src_data + 16; /* skip sig(8) + length(4) + "IHDR"(4) */
    u32 w = ((u32)ihdr[0] << 24) | ((u32)ihdr[1] << 16) | ((u32)ihdr[2] << 8) | ihdr[3];
    u32 h = ((u32)ihdr[4] << 24) | ((u32)ihdr[5] << 16) | ((u32)ihdr[6] << 8) | ihdr[7];
    u8 bit_depth   = ihdr[8];
    u8 color_type  = ihdr[9];
    u8 interlace   = ihdr[12];

    sub->info.imageWidth  = w;
    sub->info.imageHeight = h;
    sub->info.bitDepth    = bit_depth;
    sub->info.interlaceMethod = interlace;

    switch (color_type) {
        case 0: sub->info.colorSpace = CELL_PNGDEC_GRAYSCALE;       sub->info.numComponents = 1; break;
        case 2: sub->info.colorSpace = CELL_PNGDEC_RGB;             sub->info.numComponents = 3; break;
        case 3: sub->info.colorSpace = CELL_PNGDEC_PALETTE;         sub->info.numComponents = 1; break;
        case 4: sub->info.colorSpace = CELL_PNGDEC_GRAYSCALE_ALPHA; sub->info.numComponents = 2; break;
        case 6: sub->info.colorSpace = CELL_PNGDEC_RGBA;            sub->info.numComponents = 4; break;
        default: sub->info.colorSpace = CELL_PNGDEC_RGBA;           sub->info.numComponents = 4; break;
    }

    printf("[cellPngDec]   %ux%u (header-only parse, no stb_image)\n", w, h);

    sub->header_read = 1;
    *info = sub->info;
    return CELL_OK;
#endif
}

s32 cellPngDecSetParameter(CellPngDecMainHandle mainHandle,
                           CellPngDecSubHandle subHandle,
                           const CellPngDecInParam* inParam,
                           CellPngDecOutParam* outParam)
{
    (void)mainHandle;
    printf("[cellPngDec] SetParameter(sub=%u, outputColorSpace=%u)\n",
           subHandle, inParam ? inParam->outputColorSpace : 0);

    if (subHandle >= PNGDEC_MAX_SUBHANDLES || !s_png_sub[subHandle].in_use)
        return CELL_PNGDEC_ERROR_SEQ;

    if (!inParam || !outParam)
        return CELL_PNGDEC_ERROR_ARG;

    PngDecSubState* sub = &s_png_sub[subHandle];
    if (!sub->header_read)
        return CELL_PNGDEC_ERROR_SEQ;

    sub->in_param = *inParam;
    sub->param_set = 1;

    /* Compute output parameters */
    u32 out_cs = inParam->outputColorSpace;
    u32 out_comp;
    switch (out_cs) {
        case CELL_PNGDEC_COLOR_SPACE_GRAYSCALE: out_comp = 1; break;
        case CELL_PNGDEC_COLOR_SPACE_RGB:       out_comp = 3; break;
        case CELL_PNGDEC_COLOR_SPACE_RGBA:      out_comp = 4; break;
        case CELL_PNGDEC_COLOR_SPACE_ARGB:      out_comp = 4; break;
        default:                                 out_comp = 4; out_cs = CELL_PNGDEC_COLOR_SPACE_RGBA; break;
    }

    outParam->outputWidth      = sub->info.imageWidth;
    outParam->outputHeight     = sub->info.imageHeight;
    outParam->outputComponents = out_comp;
    outParam->outputBitDepth   = 8;
    outParam->outputColorSpace = out_cs;
    outParam->outputWidthByte  = (u64)(sub->info.imageWidth * out_comp);
    outParam->outputMode       = 0;
    outParam->useMemorySpace   = (u32)(outParam->outputWidthByte * sub->info.imageHeight);

    return CELL_OK;
}

s32 cellPngDecDecodeData(CellPngDecMainHandle mainHandle,
                         CellPngDecSubHandle subHandle,
                         u8* data,
                         CellPngDecDataInfo* dataInfo)
{
    (void)mainHandle;
    printf("[cellPngDec] DecodeData(sub=%u)\n", subHandle);

    if (subHandle >= PNGDEC_MAX_SUBHANDLES || !s_png_sub[subHandle].in_use)
        return CELL_PNGDEC_ERROR_SEQ;

    if (!data || !dataInfo)
        return CELL_PNGDEC_ERROR_ARG;

    PngDecSubState* sub = &s_png_sub[subHandle];
    if (!sub->header_read || !sub->param_set)
        return CELL_PNGDEC_ERROR_SEQ;

#if PNGDEC_HAS_STB
    {
        int w, h, comp;
        int desired_comp;

        switch (sub->in_param.outputColorSpace) {
            case CELL_PNGDEC_COLOR_SPACE_GRAYSCALE: desired_comp = 1; break;
            case CELL_PNGDEC_COLOR_SPACE_RGB:       desired_comp = 3; break;
            case CELL_PNGDEC_COLOR_SPACE_RGBA:      desired_comp = 4; break;
            case CELL_PNGDEC_COLOR_SPACE_ARGB:      desired_comp = 4; break;
            default:                                 desired_comp = 4; break;
        }

        u8* pixels = stbi_load_from_memory(sub->src_data, (int)sub->src_size,
                                            &w, &h, &comp, desired_comp);
        if (!pixels) {
            printf("[cellPngDec] stbi_load failed: %s\n", stbi_failure_reason());
            dataInfo->status = CELL_PNGDEC_DEC_STATUS_STOP;
            return CELL_PNGDEC_ERROR_STREAM_FORMAT;
        }

        u32 row_bytes = (u32)w * (u32)desired_comp;
        u32 total_bytes = row_bytes * (u32)h;

        /* Handle ARGB output: stb gives us RGBA, we need to swizzle to ARGB
         * which is the PS3 native pixel format. */
        if (sub->in_param.outputColorSpace == CELL_PNGDEC_COLOR_SPACE_ARGB && desired_comp == 4) {
            for (u32 i = 0; i < total_bytes; i += 4) {
                u8 r = pixels[i+0], g = pixels[i+1], b = pixels[i+2], a = pixels[i+3];
                pixels[i+0] = a;
                pixels[i+1] = r;
                pixels[i+2] = g;
                pixels[i+3] = b;
            }
        }

        /* data is a host pointer (caller has already translated from guest addr).
         * Copy decoded pixels to the output buffer. */
        memcpy(data, pixels, total_bytes);
        stbi_image_free(pixels);

        printf("[cellPngDec]   decoded %ux%u (%u bytes)\n", (u32)w, (u32)h, total_bytes);

        dataInfo->chunkInformation = 0;
        dataInfo->numText = 0;
        dataInfo->numUnknownChunk = 0;
        dataInfo->status = CELL_PNGDEC_DEC_STATUS_FINISH;

        return CELL_OK;
    }
#else
    printf("[cellPngDec] DecodeData: stb_image not available — place stb_image.h in libs/codec/\n");
    dataInfo->status = CELL_PNGDEC_DEC_STATUS_STOP;
    return (s32)CELL_ENOSYS;
#endif
}

s32 cellPngDecClose(CellPngDecMainHandle mainHandle,
                    CellPngDecSubHandle subHandle)
{
    (void)mainHandle;
    printf("[cellPngDec] Close(sub=%u)\n", subHandle);

    if (subHandle >= PNGDEC_MAX_SUBHANDLES || !s_png_sub[subHandle].in_use)
        return CELL_PNGDEC_ERROR_SEQ;

    pngdec_free_source(&s_png_sub[subHandle]);
    s_png_sub[subHandle].in_use = 0;
    return CELL_OK;
}
