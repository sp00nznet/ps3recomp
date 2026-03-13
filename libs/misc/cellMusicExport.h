/*
 * ps3recomp - cellMusicExport HLE
 *
 * Music export utility for saving audio to the XMB music column.
 * Stub — init/end lifecycle, export returns NOT_SUPPORTED.
 */

#ifndef PS3RECOMP_CELL_MUSIC_EXPORT_H
#define PS3RECOMP_CELL_MUSIC_EXPORT_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define CELL_MUSIC_EXPORT_ERROR_NOT_INITIALIZED     0x80610601
#define CELL_MUSIC_EXPORT_ERROR_ALREADY_INITIALIZED 0x80610602
#define CELL_MUSIC_EXPORT_ERROR_INVALID_ARGUMENT    0x80610603
#define CELL_MUSIC_EXPORT_ERROR_OUT_OF_MEMORY       0x80610604
#define CELL_MUSIC_EXPORT_ERROR_NOT_SUPPORTED       0x80610605

/* Export format */
#define CELL_MUSIC_EXPORT_FORMAT_AAC    0
#define CELL_MUSIC_EXPORT_FORMAT_AT3    1  /* ATRAC3 */

/* Export parameters */
typedef struct CellMusicExportParam {
    const char* filePath;
    const char* title;
    const char* artist;
    const char* album;
    u32 format;
    u32 flags;
    u32 reserved[4];
} CellMusicExportParam;

/* Callback */
typedef void (*CellMusicExportCallback)(s32 result, void* userdata);

/* Functions */
s32 cellMusicExportInit(void);
s32 cellMusicExportEnd(void);
s32 cellMusicExportStart(const CellMusicExportParam* param,
                          CellMusicExportCallback callback, void* userdata);
s32 cellMusicExportGetProgress(float* progress);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_MUSIC_EXPORT_H */
