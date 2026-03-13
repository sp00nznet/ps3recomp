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

/* Error codes */
#define CELL_PHOTO_EXPORT_ERROR_NOT_INITIALIZED     0x80610801
#define CELL_PHOTO_EXPORT_ERROR_ALREADY_INITIALIZED 0x80610802
#define CELL_PHOTO_EXPORT_ERROR_INVALID_ARGUMENT    0x80610803
#define CELL_PHOTO_EXPORT_ERROR_OUT_OF_MEMORY       0x80610804
#define CELL_PHOTO_EXPORT_ERROR_NOT_SUPPORTED       0x80610805

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
