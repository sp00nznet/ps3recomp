/*
 * ps3recomp - cellPngEnc HLE
 *
 * PNG encoding: converts raw pixel data to PNG format.
 * Stub — encoder handles are tracked but no actual encoding occurs.
 */

#ifndef PS3RECOMP_CELL_PNGENC_H
#define PS3RECOMP_CELL_PNGENC_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define CELL_PNGENC_ERROR_NOT_INITIALIZED     0x80611201
#define CELL_PNGENC_ERROR_INVALID_ARGUMENT    0x80611202
#define CELL_PNGENC_ERROR_OUT_OF_MEMORY       0x80611203
#define CELL_PNGENC_ERROR_NOT_SUPPORTED       0x80611204
#define CELL_PNGENC_ERROR_FATAL               0x80611205

/* Constants */
#define CELL_PNGENC_HANDLE_MAX   4

/* Color space */
#define CELL_PNGENC_COLOR_SPACE_ARGB         0
#define CELL_PNGENC_COLOR_SPACE_RGBA         1
#define CELL_PNGENC_COLOR_SPACE_GRAYSCALE    2
#define CELL_PNGENC_COLOR_SPACE_GRAYSCALE_A  3

/* Compression level */
#define CELL_PNGENC_COMPRESS_DEFAULT   6
#define CELL_PNGENC_COMPRESS_NONE      0
#define CELL_PNGENC_COMPRESS_BEST      9

/* Types */
typedef u32 CellPngEncHandle;

typedef struct CellPngEncParam {
    u32 width;
    u32 height;
    u32 colorSpace;
    u32 compressionLevel;
    u32 reserved[4];
} CellPngEncParam;

typedef struct CellPngEncOutputInfo {
    void* outputData;
    u32 outputSize;
    u32 reserved;
} CellPngEncOutputInfo;

/* Functions */
s32 cellPngEncCreate(const CellPngEncParam* param, CellPngEncHandle* handle);
s32 cellPngEncDestroy(CellPngEncHandle handle);
s32 cellPngEncEncode(CellPngEncHandle handle, const void* inputData,
                       CellPngEncOutputInfo* outputInfo);
s32 cellPngEncQueryAttr(const CellPngEncParam* param, u32* workMemSize);
s32 cellPngEncReset(CellPngEncHandle handle);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_PNGENC_H */
