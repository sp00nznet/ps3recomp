/*
 * ps3recomp - cellJpgEnc HLE
 *
 * JPEG encoding: converts raw pixel data to JPEG format.
 * Stub — encoder handles are tracked but no actual encoding occurs.
 */

#ifndef PS3RECOMP_CELL_JPGENC_H
#define PS3RECOMP_CELL_JPGENC_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define CELL_JPGENC_ERROR_NOT_INITIALIZED     0x80611101
#define CELL_JPGENC_ERROR_INVALID_ARGUMENT    0x80611102
#define CELL_JPGENC_ERROR_OUT_OF_MEMORY       0x80611103
#define CELL_JPGENC_ERROR_NOT_SUPPORTED       0x80611104
#define CELL_JPGENC_ERROR_FATAL               0x80611105

/* Constants */
#define CELL_JPGENC_HANDLE_MAX   4

/* Color space */
#define CELL_JPGENC_COLOR_SPACE_ARGB      0
#define CELL_JPGENC_COLOR_SPACE_RGBA      1
#define CELL_JPGENC_COLOR_SPACE_YCbCr     2
#define CELL_JPGENC_COLOR_SPACE_GRAYSCALE 3

/* Types */
typedef u32 CellJpgEncHandle;

typedef struct CellJpgEncParam {
    u32 width;
    u32 height;
    u32 colorSpace;
    u32 quality;         /* 1-100 */
    u32 reserved[4];
} CellJpgEncParam;

typedef struct CellJpgEncOutputInfo {
    void* outputData;
    u32 outputSize;
    u32 reserved;
} CellJpgEncOutputInfo;

/* Functions */
s32 cellJpgEncCreate(const CellJpgEncParam* param, CellJpgEncHandle* handle);
s32 cellJpgEncDestroy(CellJpgEncHandle handle);
s32 cellJpgEncEncode(CellJpgEncHandle handle, const void* inputData,
                       CellJpgEncOutputInfo* outputInfo);
s32 cellJpgEncQueryAttr(const CellJpgEncParam* param, u32* workMemSize);
s32 cellJpgEncReset(CellJpgEncHandle handle);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_JPGENC_H */
