/*
 * ps3recomp - cellVideoExport HLE
 *
 * Video export utility for saving game-captured video to the XMB video column.
 * Stub — init/end lifecycle, export returns NOT_SUPPORTED.
 */

#ifndef PS3RECOMP_CELL_VIDEO_EXPORT_H
#define PS3RECOMP_CELL_VIDEO_EXPORT_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define CELL_VIDEO_EXPORT_ERROR_NOT_INITIALIZED     0x80610501
#define CELL_VIDEO_EXPORT_ERROR_ALREADY_INITIALIZED 0x80610502
#define CELL_VIDEO_EXPORT_ERROR_INVALID_ARGUMENT    0x80610503
#define CELL_VIDEO_EXPORT_ERROR_OUT_OF_MEMORY       0x80610504
#define CELL_VIDEO_EXPORT_ERROR_NOT_SUPPORTED       0x80610505
#define CELL_VIDEO_EXPORT_ERROR_BUSY                0x80610506

/* Export format */
#define CELL_VIDEO_EXPORT_FORMAT_AVI    0
#define CELL_VIDEO_EXPORT_FORMAT_MP4    1

/* Export parameters */
typedef struct CellVideoExportParam {
    const char* filePath;
    const char* title;
    const char* gameTitle;
    u32 format;
    u32 flags;
    u32 reserved[4];
} CellVideoExportParam;

/* Callback */
typedef void (*CellVideoExportCallback)(s32 result, void* userdata);

/* Functions */
s32 cellVideoExportInit(void);
s32 cellVideoExportEnd(void);
s32 cellVideoExportStart(const CellVideoExportParam* param,
                          CellVideoExportCallback callback, void* userdata);
s32 cellVideoExportAbort(void);
s32 cellVideoExportGetProgress(float* progress);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_VIDEO_EXPORT_H */
