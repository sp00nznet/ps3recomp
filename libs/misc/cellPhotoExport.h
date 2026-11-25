/*
 * ps3recomp - cellPhotoExport HLE
 *
 * Photo export utility for saving screenshots to the XMB photo column.
 * Stub — init/end lifecycle, export returns NOT_SUPPORTED.
 */

#ifndef PS3RECOMP_CELL_PHOTO_EXPORT_H
#define PS3RECOMP_CELL_PHOTO_EXPORT_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes (sysutil/sysutil_photo_export.h; base 0x8002C200 verified against
 * Sony firmware 4.93 libsysutil_photo_export.sprx: Initialize(0)/FromFile(0) -> 0x8002C203) */
#define CELL_PHOTO_EXPORT_ERROR_NOT_INITIALIZED     (CELL_ERROR_BASE_SYSUTIL_PHOTO_EXPORT | 0x01) /* 0x8002C201 */
#define CELL_PHOTO_EXPORT_ERROR_ALREADY_INITIALIZED (CELL_ERROR_BASE_SYSUTIL_PHOTO_EXPORT | 0x02) /* 0x8002C202 */
#define CELL_PHOTO_EXPORT_ERROR_INVALID_ARGUMENT    (CELL_ERROR_BASE_SYSUTIL_PHOTO_EXPORT | 0x03) /* 0x8002C203 */
#define CELL_PHOTO_EXPORT_ERROR_PARAM               CELL_PHOTO_EXPORT_ERROR_INVALID_ARGUMENT
#define CELL_PHOTO_EXPORT_ERROR_OUT_OF_MEMORY       (CELL_ERROR_BASE_SYSUTIL_PHOTO_EXPORT | 0x04) /* 0x8002C204 */
#define CELL_PHOTO_EXPORT_ERROR_NOT_SUPPORTED       (CELL_ERROR_BASE_SYSUTIL_PHOTO_EXPORT | 0x05) /* 0x8002C205 */

/* Export format */
#define CELL_PHOTO_EXPORT_FORMAT_JPEG  0
#define CELL_PHOTO_EXPORT_FORMAT_PNG   1

/* Export parameters */
typedef struct CellPhotoExportParam {
    const char* filePath;
    const char* title;
    const char* gameTitle;
    u32 format;
    u32 flags;
    u32 reserved[4];
} CellPhotoExportParam;

/* Callback */
typedef void (*CellPhotoExportCallback)(s32 result, void* userdata);

/* Functions */
s32 cellPhotoExportInit(void);
s32 cellPhotoExportEnd(void);
s32 cellPhotoExportStart(const CellPhotoExportParam* param,
                          CellPhotoExportCallback callback, void* userdata);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_PHOTO_EXPORT_H */
