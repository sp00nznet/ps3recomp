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

/* Guest memory access (translate + byte-swap). The guest passes POINTERS as
 * 32-bit big-endian addresses into vm_base; every struct field the guest reads
 * or writes is big-endian, so we marshal each field explicitly rather than
 * overlaying a host struct (which would be little-endian and mis-packed --
 * e.g. the real CellPngDecSrc.fileName is a char* at +4, not an inline array).
 * The real SDK <cell/codec/pngdec.h> is the ABI source of truth. */
#include "../../runtime/ppu/ppu_memory.h"

/* Read a NUL-terminated guest string into a host buffer. */
static void vm_read_str(u32 addr, char* dst, size_t cap)
{
    size_t i = 0;
    if (!addr) { if (cap) dst[0] = 0; return; }
    for (; i < cap - 1; i++) {
        char c = (char)vm_read8(addr + (u32)i);
        dst[i] = c;
        if (!c) return;
    }
    dst[i] = 0;
}

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
    char              filename[CELL_PNGDEC_MAX_PATH]; /* host copy of guest path */
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

/* All public entry points take GUEST addresses (u32) for pointer parameters --
 * the HLE bridge passes the raw gpr values, so a `CellPngDecMainHandle*` arrives
 * as a big-endian guest address, NOT a host pointer. Handles are opaque to the
 * guest, so we hand back small indices. */

s32 cellPngDecCreate(u32 mainHandle_addr, u32 threadInParam_addr, u32 threadOutParam_addr)
{
    (void)threadInParam_addr;
    printf("[cellPngDec] Create()\n");

    if (!mainHandle_addr)
        return CELL_PNGDEC_ERROR_ARG;

    for (u32 i = 0; i < PNGDEC_MAX_HANDLES; i++) {
        if (!s_png_main[i].in_use) {
            s_png_main[i].in_use = 1;
            vm_write32(mainHandle_addr, i);   /* handle value -> guest memory */
            if (threadOutParam_addr)
                vm_write32(threadOutParam_addr + 0, 0x00010000);  /* pngCodecVersion */
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

/* CellPngDecSrc guest layout (real SDK, 32 bytes):
 *   +0  srcSelect(u32)  +4 fileName(char*)  +8 fileOffset(s64)
 *   +16 fileSize(u32)   +20 streamPtr(void*) +24 streamSize(u32)
 *   +28 spuThreadEnable(u32) */
s32 cellPngDecOpen(CellPngDecMainHandle mainHandle,
                   u32 subHandle_addr, u32 src_addr, u32 openInfo_addr)
{
    if (mainHandle >= PNGDEC_MAX_HANDLES || !s_png_main[mainHandle].in_use)
        return CELL_PNGDEC_ERROR_SEQ;

    if (!subHandle_addr || !src_addr)
        return CELL_PNGDEC_ERROR_ARG;

    u32 srcSelect = vm_read32(src_addr + 0);
    printf("[cellPngDec] Open(main=%u, srcSelect=%u)\n", mainHandle, srcSelect);

    for (u32 i = 0; i < PNGDEC_MAX_SUBHANDLES; i++) {
        if (!s_png_sub[i].in_use) {
            PngDecSubState* sub = &s_png_sub[i];
            memset(sub, 0, sizeof(PngDecSubState));
            sub->in_use = 1;
            sub->main_handle = mainHandle;
            sub->src.srcSelect = srcSelect;
            sub->src.fileOffset = (u64)vm_read64(src_addr + 8);
            sub->src.fileSize   = vm_read32(src_addr + 16);
            sub->src.streamPtr  = vm_read32(src_addr + 20);  /* guest addr as value */
            sub->src.streamSize = vm_read32(src_addr + 24);
            if (srcSelect == CELL_PNGDEC_FILE) {
                u32 name_addr = vm_read32(src_addr + 4);
                vm_read_str(name_addr, sub->filename, sizeof(sub->filename));
                sub->src.fileName = sub->filename;
                printf("[cellPngDec]   file: '%s'\n", sub->filename);
            } else if (srcSelect == CELL_PNGDEC_BUFFER) {
                printf("[cellPngDec]   buffer: guest_addr=0x%08X size=%u\n",
                       (u32)sub->src.streamPtr, sub->src.streamSize);
            }

            vm_write32(subHandle_addr, i);
            if (openInfo_addr)
                vm_write32(openInfo_addr + 0, 0);   /* initSpaceAllocated */
            return CELL_OK;
        }
    }

    return CELL_PNGDEC_ERROR_BUSY;
}

/* CellPngDecInfo guest layout (7 x u32): imageWidth, imageHeight,
 * numComponents, colorSpace, bitDepth, interlaceMethod, chunkInformation. */
static void pngdec_write_info(u32 addr, const CellPngDecInfo* in)
{
    vm_write32(addr + 0,  in->imageWidth);
    vm_write32(addr + 4,  in->imageHeight);
    vm_write32(addr + 8,  in->numComponents);
    vm_write32(addr + 12, in->colorSpace);
    vm_write32(addr + 16, in->bitDepth);
    vm_write32(addr + 20, in->interlaceMethod);
    vm_write32(addr + 24, in->chunkInformation);
}

s32 cellPngDecReadHeader(CellPngDecMainHandle mainHandle,
                         CellPngDecSubHandle subHandle,
                         u32 info_addr)
{
    (void)mainHandle;
    printf("[cellPngDec] ReadHeader(sub=%u)\n", subHandle);

    if (subHandle >= PNGDEC_MAX_SUBHANDLES || !s_png_sub[subHandle].in_use)
        return CELL_PNGDEC_ERROR_SEQ;

    if (!info_addr)
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
        pngdec_write_info(info_addr, &sub->info);
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
    pngdec_write_info(info_addr, &sub->info);
    return CELL_OK;
#endif
}

/* CellPngDecInParam guest layout (7 x u32): commandPtr, outputMode,
 * outputColorSpace, outputBitDepth, outputPackFlag, outputAlphaSelect,
 * outputColorAlpha.
 * CellPngDecOutParam guest layout: outputWidthByte(u64 @0), then u32s at
 * +8 width, +12 height, +16 components, +20 bitDepth, +24 mode, +28 colorSpace,
 * +32 useMemorySpace. */
s32 cellPngDecSetParameter(CellPngDecMainHandle mainHandle,
                           CellPngDecSubHandle subHandle,
                           u32 inParam_addr, u32 outParam_addr)
{
    (void)mainHandle;

    if (subHandle >= PNGDEC_MAX_SUBHANDLES || !s_png_sub[subHandle].in_use)
        return CELL_PNGDEC_ERROR_SEQ;

    if (!inParam_addr || !outParam_addr)
        return CELL_PNGDEC_ERROR_ARG;

    PngDecSubState* sub = &s_png_sub[subHandle];
    if (!sub->header_read)
        return CELL_PNGDEC_ERROR_SEQ;

    sub->in_param.commandPtr        = vm_read32(inParam_addr + 0);
    sub->in_param.outputMode        = vm_read32(inParam_addr + 4);
    sub->in_param.outputColorSpace  = vm_read32(inParam_addr + 8);
    sub->in_param.outputBitDepth    = vm_read32(inParam_addr + 12);
    sub->in_param.outputPackFlag    = vm_read32(inParam_addr + 16);
    sub->in_param.outputAlphaSelect = vm_read32(inParam_addr + 20);
    sub->in_param.outputColorAlpha  = vm_read32(inParam_addr + 24);
    sub->param_set = 1;

    printf("[cellPngDec] SetParameter(sub=%u, outputColorSpace=%u)\n",
           subHandle, sub->in_param.outputColorSpace);

    /* Compute output parameters */
    u32 out_cs = sub->in_param.outputColorSpace;
    u32 out_comp;
    switch (out_cs) {
        case CELL_PNGDEC_COLOR_SPACE_GRAYSCALE: out_comp = 1; break;
        case CELL_PNGDEC_COLOR_SPACE_RGB:       out_comp = 3; break;
        case CELL_PNGDEC_COLOR_SPACE_RGBA:      out_comp = 4; break;
        case CELL_PNGDEC_COLOR_SPACE_ARGB:      out_comp = 4; break;
        default:                                 out_comp = 4; out_cs = CELL_PNGDEC_COLOR_SPACE_RGBA; break;
    }

    u64 widthByte = (u64)(sub->info.imageWidth * out_comp);
    vm_write64(outParam_addr + 0,  widthByte);                 /* outputWidthByte */
    vm_write32(outParam_addr + 8,  sub->info.imageWidth);      /* outputWidth */
    vm_write32(outParam_addr + 12, sub->info.imageHeight);     /* outputHeight */
    vm_write32(outParam_addr + 16, out_comp);                  /* outputComponents */
    vm_write32(outParam_addr + 20, 8);                         /* outputBitDepth */
    vm_write32(outParam_addr + 24, 0);                         /* outputMode (top-to-bottom) */
    vm_write32(outParam_addr + 28, out_cs);                    /* outputColorSpace */
    vm_write32(outParam_addr + 32, (u32)(widthByte * sub->info.imageHeight)); /* useMemorySpace */

    return CELL_OK;
}

/* CellPngDecDataOutInfo guest layout (4 x u32): chunkInformation, numText,
 * numUnknownChunk, status. */
static void pngdec_write_outinfo(u32 addr, u32 status)
{
    if (!addr) return;
    vm_write32(addr + 0,  0);       /* chunkInformation */
    vm_write32(addr + 4,  0);       /* numText */
    vm_write32(addr + 8,  0);       /* numUnknownChunk */
    vm_write32(addr + 12, status);  /* status */
}

s32 cellPngDecDecodeData(CellPngDecMainHandle mainHandle,
                         CellPngDecSubHandle subHandle,
                         u32 data_addr, u32 dataCtrlParam_addr, u32 dataOutInfo_addr)
{
    (void)mainHandle;
    printf("[cellPngDec] DecodeData(sub=%u)\n", subHandle);

    if (subHandle >= PNGDEC_MAX_SUBHANDLES || !s_png_sub[subHandle].in_use)
        return CELL_PNGDEC_ERROR_SEQ;

    if (!data_addr)
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
            pngdec_write_outinfo(dataOutInfo_addr, CELL_PNGDEC_DEC_STATUS_STOP);
            return CELL_PNGDEC_ERROR_STREAM_FORMAT;
        }

        u32 row_bytes = (u32)w * (u32)desired_comp;

        /* ARGB output: stb gives RGBA; swizzle to the PS3-native ARGB order. */
        if (sub->in_param.outputColorSpace == CELL_PNGDEC_COLOR_SPACE_ARGB && desired_comp == 4) {
            u32 total_bytes = row_bytes * (u32)h;
            for (u32 i = 0; i < total_bytes; i += 4) {
                u8 r = pixels[i+0], g = pixels[i+1], b = pixels[i+2], a = pixels[i+3];
                pixels[i+0] = a; pixels[i+1] = r; pixels[i+2] = g; pixels[i+3] = b;
            }
        }

        /* Destination is a GUEST buffer at data_addr; the guest supplies the row
         * pitch in dataCtrlParam.outputBytesPerLine (u64 @0). Copy row-by-row so
         * a padded/tiled pitch is honoured. Pixel bytes are endian-neutral. */
        u64 dst_pitch = dataCtrlParam_addr ? vm_read64(dataCtrlParam_addr + 0) : 0;
        if (dst_pitch < row_bytes) dst_pitch = row_bytes;
        for (u32 y = 0; y < (u32)h; y++)
            memcpy(vm_base + data_addr + (u64)y * dst_pitch,
                   pixels + (u64)y * row_bytes, row_bytes);
        stbi_image_free(pixels);

        printf("[cellPngDec]   decoded %ux%u -> guest 0x%08X (pitch %llu, row %u)\n",
               (u32)w, (u32)h, data_addr, (unsigned long long)dst_pitch, row_bytes);

        pngdec_write_outinfo(dataOutInfo_addr, CELL_PNGDEC_DEC_STATUS_FINISH);
        return CELL_OK;
    }
#else
    printf("[cellPngDec] DecodeData: stb_image not available — place stb_image.h in libs/codec/\n");
    pngdec_write_outinfo(dataOutInfo_addr, CELL_PNGDEC_DEC_STATUS_STOP);
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
