/*
 * ps3recomp - cellPngDec HLE
 *
 * PNG image decoding. Uses stb_image.h if available (place in include/).
 */

#ifndef PS3RECOMP_CELL_PNGDEC_H
#define PS3RECOMP_CELL_PNGDEC_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/

#define CELL_PNGDEC_ERROR_HEADER        (CELL_ERROR_BASE_PNG | 0x01)
#define CELL_PNGDEC_ERROR_STREAM_FORMAT (CELL_ERROR_BASE_PNG | 0x02)
#define CELL_PNGDEC_ERROR_ARG           (CELL_ERROR_BASE_PNG | 0x03)
#define CELL_PNGDEC_ERROR_SEQ           (CELL_ERROR_BASE_PNG | 0x04)
#define CELL_PNGDEC_ERROR_BUSY          (CELL_ERROR_BASE_PNG | 0x05)
#define CELL_PNGDEC_ERROR_FATAL         (CELL_ERROR_BASE_PNG | 0x06)
#define CELL_PNGDEC_ERROR_OPEN_FILE     (CELL_ERROR_BASE_PNG | 0x07)
#define CELL_PNGDEC_ERROR_SPU_UNSUPPORT (CELL_ERROR_BASE_PNG | 0x08)
#define CELL_PNGDEC_ERROR_CB_PARAM      (CELL_ERROR_BASE_PNG | 0x09)

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

/* Source stream type */
#define CELL_PNGDEC_FILE        0
#define CELL_PNGDEC_BUFFER      1

/* Color space */
#define CELL_PNGDEC_GRAYSCALE       1
#define CELL_PNGDEC_RGB             2
#define CELL_PNGDEC_PALETTE         4
#define CELL_PNGDEC_GRAYSCALE_ALPHA 9
#define CELL_PNGDEC_RGBA            10
#define CELL_PNGDEC_ARGB            20

/* Output color space for SetParameter */
#define CELL_PNGDEC_COLOR_SPACE_GRAYSCALE       1
#define CELL_PNGDEC_COLOR_SPACE_RGB             2
#define CELL_PNGDEC_COLOR_SPACE_RGBA            10
#define CELL_PNGDEC_COLOR_SPACE_ARGB            20

/* Decode status */
#define CELL_PNGDEC_DEC_STATUS_FINISH   0
#define CELL_PNGDEC_DEC_STATUS_STOP     1

/* Maximum file path length */
#define CELL_PNGDEC_MAX_PATH    1024

/* ---------------------------------------------------------------------------
 * Structures
 * -----------------------------------------------------------------------*/

/* Opaque handles */
typedef u32 CellPngDecMainHandle;
typedef u32 CellPngDecSubHandle;

/* Callbacks for memory allocation (simplified) */
typedef struct CellPngDecCbCtrlMallocArg {
    u32 size;
} CellPngDecCbCtrlMallocArg;

typedef struct CellPngDecThreadInParam {
    u32 spuThreadEnable;
    u32 ppuThreadPriority;
    u32 spuThreadPriority;
    u32 cbCtrlMallocFunc;
    u32 cbCtrlMallocArg;
    u32 cbCtrlFreeFunc;
    u32 cbCtrlFreeArg;
} CellPngDecThreadInParam;

typedef struct CellPngDecThreadOutParam {
    u32 pngCodecVersion;
} CellPngDecThreadOutParam;

/* Source descriptor */
typedef struct CellPngDecSrc {
    u32  srcSelect;                 /* CELL_PNGDEC_FILE or CELL_PNGDEC_BUFFER */
    char fileName[CELL_PNGDEC_MAX_PATH]; /* File path (if FILE) */
    u64  fileOffset;                /* Offset within file */
    u32  fileSize;                  /* Size of data */
    u64  streamPtr;                 /* Buffer pointer (if BUFFER, guest addr) */
    u32  streamSize;                /* Buffer size */
    u32  spuThreadEnable;
} CellPngDecSrc;

/* Image info from header */
typedef struct CellPngDecInfo {
    u32  imageWidth;
    u32  imageHeight;
    u32  numComponents;
    u32  colorSpace;        /* CELL_PNGDEC_GRAYSCALE, _RGB, etc. */
    u32  bitDepth;
    u32  interlaceMethod;
    u32  chunkInformation;
} CellPngDecInfo;

/* Input parameters for decode */
typedef struct CellPngDecInParam {
    u32  commandPtr;        /* volatile, decode command */
    u32  outputMode;
    u32  outputColorSpace;  /* desired output color space */
    u32  outputBitDepth;
    u32  outputPackFlag;
    u32  outputAlphaSelect;
    u32  outputColorAlpha;
} CellPngDecInParam;

/* Output parameters after parameter set */
typedef struct CellPngDecOutParam {
    u64  outputWidthByte;
    u32  outputWidth;
    u32  outputHeight;
    u32  outputComponents;
    u32  outputBitDepth;
    u32  outputMode;
    u32  outputColorSpace;
    u32  useMemorySpace;
} CellPngDecOutParam;

/* Open info returned by Open */
typedef struct CellPngDecOpnInfo {
    u32  initSpaceAllocated;
} CellPngDecOpnInfo;

/* Data info returned by DecodeData */
typedef struct CellPngDecDataInfo {
    u32  chunkInformation;
    u32  numText;
    u32  numUnknownChunk;
    u32  status;            /* CELL_PNGDEC_DEC_STATUS_FINISH etc. */
} CellPngDecDataInfo;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 cellPngDecCreate(CellPngDecMainHandle* mainHandle,
                     const CellPngDecThreadInParam* threadInParam,
                     CellPngDecThreadOutParam* threadOutParam);

s32 cellPngDecDestroy(CellPngDecMainHandle mainHandle);

s32 cellPngDecOpen(CellPngDecMainHandle mainHandle,
                   CellPngDecSubHandle* subHandle,
                   const CellPngDecSrc* src,
                   CellPngDecOpnInfo* openInfo);

s32 cellPngDecReadHeader(CellPngDecMainHandle mainHandle,
                         CellPngDecSubHandle subHandle,
                         CellPngDecInfo* info);

s32 cellPngDecSetParameter(CellPngDecMainHandle mainHandle,
                           CellPngDecSubHandle subHandle,
                           const CellPngDecInParam* inParam,
                           CellPngDecOutParam* outParam);

s32 cellPngDecDecodeData(CellPngDecMainHandle mainHandle,
                         CellPngDecSubHandle subHandle,
                         u8* data,
                         CellPngDecDataInfo* dataInfo);

s32 cellPngDecClose(CellPngDecMainHandle mainHandle,
                    CellPngDecSubHandle subHandle);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_PNGDEC_H */
