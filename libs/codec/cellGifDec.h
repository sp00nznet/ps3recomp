/*
 * ps3recomp - cellGifDec HLE
 *
 * GIF image decoding. Uses stb_image.h if available.
 */

#ifndef PS3RECOMP_CELL_GIFDEC_H
#define PS3RECOMP_CELL_GIFDEC_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes (GIF shares base with PNG/JPG range)
 * -----------------------------------------------------------------------*/

#define CELL_GIFDEC_ERROR_BASE          0x80611400
#define CELL_GIFDEC_ERROR_HEADER        (CELL_GIFDEC_ERROR_BASE | 0x01)
#define CELL_GIFDEC_ERROR_STREAM_FORMAT (CELL_GIFDEC_ERROR_BASE | 0x02)
#define CELL_GIFDEC_ERROR_ARG           (CELL_GIFDEC_ERROR_BASE | 0x03)
#define CELL_GIFDEC_ERROR_SEQ           (CELL_GIFDEC_ERROR_BASE | 0x04)
#define CELL_GIFDEC_ERROR_BUSY          (CELL_GIFDEC_ERROR_BASE | 0x05)
#define CELL_GIFDEC_ERROR_FATAL         (CELL_GIFDEC_ERROR_BASE | 0x06)
#define CELL_GIFDEC_ERROR_OPEN_FILE     (CELL_GIFDEC_ERROR_BASE | 0x07)

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

#define CELL_GIFDEC_FILE        0
#define CELL_GIFDEC_BUFFER      1

/* Output color space */
#define CELL_GIFDEC_RGBA        10
#define CELL_GIFDEC_ARGB        20

/* Decode status */
#define CELL_GIFDEC_DEC_STATUS_FINISH   0
#define CELL_GIFDEC_DEC_STATUS_STOP     1

#define CELL_GIFDEC_MAX_PATH    1024

/* ---------------------------------------------------------------------------
 * Structures
 * -----------------------------------------------------------------------*/

typedef u32 CellGifDecMainHandle;
typedef u32 CellGifDecSubHandle;

typedef struct CellGifDecThreadInParam {
    u32 spuThreadEnable;
    u32 ppuThreadPriority;
    u32 spuThreadPriority;
    u32 cbCtrlMallocFunc;
    u32 cbCtrlMallocArg;
    u32 cbCtrlFreeFunc;
    u32 cbCtrlFreeArg;
} CellGifDecThreadInParam;

typedef struct CellGifDecThreadOutParam {
    u32 gifCodecVersion;
} CellGifDecThreadOutParam;

typedef struct CellGifDecSrc {
    u32  srcSelect;
    char fileName[CELL_GIFDEC_MAX_PATH];
    u64  fileOffset;
    u32  fileSize;
    u64  streamPtr;
    u32  streamSize;
    u32  spuThreadEnable;
} CellGifDecSrc;

typedef struct CellGifDecInfo {
    u32 SWidth;
    u32 SHeight;
    u32 SGlobalColorTableFlag;
    u32 SColorResolution;
    u32 SSortFlag;
    u32 SSizeOfGlobalColorTable;
    u32 SBackGroundColor;
    u32 SPixelAspectRatio;
} CellGifDecInfo;

typedef struct CellGifDecInParam {
    u32 commandPtr;
    u32 outputColorSpace;
    u32 outputColorAlpha;
} CellGifDecInParam;

typedef struct CellGifDecOutParam {
    u64 outputWidthByte;
    u32 outputWidth;
    u32 outputHeight;
    u32 outputComponents;
    u32 outputColorSpace;
    u32 useMemorySpace;
} CellGifDecOutParam;

typedef struct CellGifDecOpnInfo {
    u32 initSpaceAllocated;
} CellGifDecOpnInfo;

typedef struct CellGifDecDataInfo {
    u32 recordType;
    u32 status;
} CellGifDecDataInfo;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 cellGifDecCreate(CellGifDecMainHandle* mainHandle,
                     const CellGifDecThreadInParam* threadInParam,
                     CellGifDecThreadOutParam* threadOutParam);

s32 cellGifDecDestroy(CellGifDecMainHandle mainHandle);

s32 cellGifDecOpen(CellGifDecMainHandle mainHandle,
                   CellGifDecSubHandle* subHandle,
                   const CellGifDecSrc* src,
                   CellGifDecOpnInfo* openInfo);

s32 cellGifDecReadHeader(CellGifDecMainHandle mainHandle,
                         CellGifDecSubHandle subHandle,
                         CellGifDecInfo* info);

s32 cellGifDecSetParameter(CellGifDecMainHandle mainHandle,
                           CellGifDecSubHandle subHandle,
                           const CellGifDecInParam* inParam,
                           CellGifDecOutParam* outParam);

s32 cellGifDecDecodeData(CellGifDecMainHandle mainHandle,
                         CellGifDecSubHandle subHandle,
                         u8* data,
                         CellGifDecDataInfo* dataInfo);

s32 cellGifDecClose(CellGifDecMainHandle mainHandle,
                    CellGifDecSubHandle subHandle);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_GIFDEC_H */
