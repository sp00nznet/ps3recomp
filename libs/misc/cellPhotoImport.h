/*
 * ps3recomp - cellPhotoImport HLE
 *
 * Photo import utility — lets games browse and import photos from the XMB.
 * Stub — init/end lifecycle, import returns NOT_SUPPORTED.
 */

#ifndef PS3RECOMP_CELL_PHOTO_IMPORT_H
#define PS3RECOMP_CELL_PHOTO_IMPORT_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define CELL_PHOTO_IMPORT_ERROR_NOT_INITIALIZED     0x80610901
#define CELL_PHOTO_IMPORT_ERROR_ALREADY_INITIALIZED 0x80610902
#define CELL_PHOTO_IMPORT_ERROR_INVALID_ARGUMENT    0x80610903
#define CELL_PHOTO_IMPORT_ERROR_OUT_OF_MEMORY       0x80610904
#define CELL_PHOTO_IMPORT_ERROR_NOT_SUPPORTED       0x80610905
#define CELL_PHOTO_IMPORT_ERROR_CANCELLED           0x80610906

/* Import result */
typedef struct CellPhotoImportResult {
    char filePath[1024];
    u32 width;
    u32 height;
    u32 format;
} CellPhotoImportResult;

/* Callback */
typedef void (*CellPhotoImportCallback)(s32 result, const CellPhotoImportResult* data,
                                         void* userdata);

/* Functions */
s32 cellPhotoImportInit(void);
s32 cellPhotoImportEnd(void);
s32 cellPhotoImportStart(CellPhotoImportCallback callback, void* userdata);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_PHOTO_IMPORT_H */
