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

/* Error codes (PS3 sysutil photo import, base 0x8002C700; verified against RPCS3) */
#define CELL_PHOTO_IMPORT_ERROR_NOT_INITIALIZED     (CELL_ERROR_BASE_SYSUTIL_PHOTO_IMPORT | 0x01) /* 0x8002C701 */
#define CELL_PHOTO_IMPORT_ERROR_ALREADY_INITIALIZED (CELL_ERROR_BASE_SYSUTIL_PHOTO_IMPORT | 0x02) /* 0x8002C702 */
#define CELL_PHOTO_IMPORT_ERROR_INVALID_ARGUMENT    (CELL_ERROR_BASE_SYSUTIL_PHOTO_IMPORT | 0x03) /* 0x8002C703 */
#define CELL_PHOTO_IMPORT_ERROR_OUT_OF_MEMORY       (CELL_ERROR_BASE_SYSUTIL_PHOTO_IMPORT | 0x04) /* 0x8002C704 */
#define CELL_PHOTO_IMPORT_ERROR_NOT_SUPPORTED       (CELL_ERROR_BASE_SYSUTIL_PHOTO_IMPORT | 0x05) /* 0x8002C705 */
#define CELL_PHOTO_IMPORT_ERROR_CANCELLED           (CELL_ERROR_BASE_SYSUTIL_PHOTO_IMPORT | 0x06) /* 0x8002C706 */

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
