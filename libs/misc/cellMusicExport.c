/*
 * ps3recomp - cellMusicExport HLE implementation
 *
 * Stub. Init/term work, export returns NOT_SUPPORTED.
 */

#include "cellMusicExport.h"
#include <stdio.h>

/* Internal state */

static int s_initialized = 0;

/* API */

s32 cellMusicExportInit(void)
{
    printf("[cellMusicExport] Init()\n");
    if (s_initialized)
        return (s32)CELL_MUSIC_EXPORT_ERROR_ALREADY_INITIALIZED;
    s_initialized = 1;
    return CELL_OK;
}

s32 cellMusicExportEnd(void)
{
    printf("[cellMusicExport] End()\n");
    s_initialized = 0;
    return CELL_OK;
}

s32 cellMusicExportStart(const CellMusicExportParam* param,
                          CellMusicExportCallback callback, void* userdata)
{
    (void)param;
    printf("[cellMusicExport] Start() - not supported\n");
    if (!s_initialized)
        return (s32)CELL_MUSIC_EXPORT_ERROR_NOT_INITIALIZED;

    if (callback)
        callback((s32)CELL_MUSIC_EXPORT_ERROR_NOT_SUPPORTED, userdata);

    return (s32)CELL_MUSIC_EXPORT_ERROR_NOT_SUPPORTED;
}

s32 cellMusicExportGetProgress(float* progress)
{
    if (!s_initialized)
        return (s32)CELL_MUSIC_EXPORT_ERROR_NOT_INITIALIZED;
    if (!progress) return (s32)CELL_MUSIC_EXPORT_ERROR_INVALID_ARGUMENT;
    *progress = 0.0f;
    return CELL_OK;
}
