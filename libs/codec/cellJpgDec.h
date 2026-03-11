/*
 * ps3recomp - cellJpgDec HLE
 *
 * JPEG image decoding. Uses stb_image.h if available.
 */

#ifndef PS3RECOMP_CELL_JPGDEC_H
#define PS3RECOMP_CELL_JPGDEC_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/

#define CELL_JPGDEC_ERROR_HEADER        (CELL_ERROR_BASE_JPG | 0x01)
#define CELL_JPGDEC_ERROR_STREAM_FORMAT (CELL_ERROR_BASE_JPG | 0x02)
#define CELL_JPGDEC_ERROR_ARG           (CELL_ERROR_BASE_JPG | 0x03)
#define CELL_JPGDEC_ERROR_SEQ           (CELL_ERROR_BASE_JPG | 0x04)
#define CELL_JPGDEC_ERROR_BUSY          (CELL_ERROR_BASE_JPG | 0x05)
#define CELL_JPGDEC_ERROR_FATAL         (CELL_ERROR_BASE_JPG | 0x06)
#define CELL_JPGDEC_ERROR_OPEN_FILE     (CELL_ERROR_BASE_JPG | 0x07)

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

#define CELL_JPGDEC_FILE        0
#define CELL_JPGDEC_BUFFER      1

/* Output color space */
#define CELL_JPGDEC_GRAYSCALE   1
#define CELL_JPGDEC_RGB         2
#define CELL_JPGDEC_RGBA        10
#define CELL_JPGDEC_ARGB        20
#define CELL_JPGDEC_YCbCr       40

/* Decode status */
#define CELL_JPGDEC_DEC_STATUS_FINISH   0
#define CELL_JPGDEC_DEC_STATUS_STOP     1

#define CELL_JPGDEC_MAX_PATH    1024

/* ---------------------------------------------------------------------------
 * Structures
 * -----------------------------------------------------------------------*/

typedef u32 CellJpgDecMainHandle;
typedef u32 CellJpgDecSubHandle;

typedef struct CellJpgDecThreadInParam {
    u32 spuThreadEnable;
    u32 ppuThreadPriority;
    u32 spuThreadPriority;
    u32 cbCtrlMallocFunc;
    u32 cbCtrlMallocArg;
    u32 cbCtrlFreeFunc;
    u32 cbCtrlFreeArg;
} CellJpgDecThreadInParam;

typedef struct CellJpgDecThreadOutParam {
    u32 jpgCodecVersion;
} CellJpgDecThreadOutParam;

typedef struct CellJpgDecSrc {
    u32  srcSelect;
    char fileName[CELL_JPGDEC_MAX_PATH];
    u64  fileOffset;
    u32  fileSize;
    u64  streamPtr;
    u32  streamSize;
    u32  spuThreadEnable;
} CellJpgDecSrc;

typedef struct CellJpgDecInfo {
    u32 imageWidth;
    u32 imageHeight;
    u32 numComponents;
    u32 colorSpace;
} CellJpgDecInfo;

typedef struct CellJpgDecInParam {
    u32 commandPtr;
    u32 outputMode;
    u32 outputColorSpace;
    u32 downScale;
    u32 outputBitDepth; /* not in PS3 SDK but useful */
} CellJpgDecInParam;

typedef struct CellJpgDecOutParam {
    u64 outputWidthByte;
    u32 outputWidth;
    u32 outputHeight;
    u32 outputComponents;
    u32 outputMode;
    u32 outputColorSpace;
    u32 downScale;
    u32 useMemorySpace;
} CellJpgDecOutParam;

typedef struct CellJpgDecOpnInfo {
    u32 initSpaceAllocated;
} CellJpgDecOpnInfo;

typedef struct CellJpgDecDataInfo {
    u32 status;
} CellJpgDecDataInfo;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 cellJpgDecCreate(CellJpgDecMainHandle* mainHandle,
                     const CellJpgDecThreadInParam* threadInParam,
                     CellJpgDecThreadOutParam* threadOutParam);

s32 cellJpgDecDestroy(CellJpgDecMainHandle mainHandle);

s32 cellJpgDecOpen(CellJpgDecMainHandle mainHandle,
                   CellJpgDecSubHandle* subHandle,
                   const CellJpgDecSrc* src,
                   CellJpgDecOpnInfo* openInfo);

s32 cellJpgDecReadHeader(CellJpgDecMainHandle mainHandle,
                         CellJpgDecSubHandle subHandle,
                         CellJpgDecInfo* info);

s32 cellJpgDecSetParameter(CellJpgDecMainHandle mainHandle,
                           CellJpgDecSubHandle subHandle,
                           const CellJpgDecInParam* inParam,
                           CellJpgDecOutParam* outParam);

s32 cellJpgDecDecodeData(CellJpgDecMainHandle mainHandle,
                         CellJpgDecSubHandle subHandle,
                         u8* data,
                         CellJpgDecDataInfo* dataInfo);

s32 cellJpgDecClose(CellJpgDecMainHandle mainHandle,
                    CellJpgDecSubHandle subHandle);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_JPGDEC_H */
