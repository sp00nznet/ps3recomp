/*
 * ps3recomp - cellVideoExport HLE implementation
 *
 * Stub. Init/term work, export returns NOT_SUPPORTED.
 */

#include "cellVideoExport.h"
#include <stdio.h>

/* Internal state */

static int s_initialized = 0;

/* API */

s32 cellVideoExportInit(void)
{
    printf("[cellVideoExport] Init()\n");
    if (s_initialized)
        return (s32)CELL_VIDEO_EXPORT_ERROR_ALREADY_INITIALIZED;
    s_initialized = 1;
    return CELL_OK;
}

s32 cellVideoExportEnd(void)
{
    printf("[cellVideoExport] End()\n");
    s_initialized = 0;
    return CELL_OK;
}

s32 cellVideoExportStart(const CellVideoExportParam* param,
                          CellVideoExportCallback callback, void* userdata)
{
    (void)param;
    printf("[cellVideoExport] Start() - not supported\n");
    if (!s_initialized)
        return (s32)CELL_VIDEO_EXPORT_ERROR_NOT_INITIALIZED;

    /* Immediately call back with NOT_SUPPORTED */
    if (callback)
        callback((s32)CELL_VIDEO_EXPORT_ERROR_NOT_SUPPORTED, userdata);

    return (s32)CELL_VIDEO_EXPORT_ERROR_NOT_SUPPORTED;
}

s32 cellVideoExportAbort(void)
{
    if (!s_initialized)
        return (s32)CELL_VIDEO_EXPORT_ERROR_NOT_INITIALIZED;
    return CELL_OK;
}

s32 cellVideoExportGetProgress(float* progress)
{
    if (!s_initialized)
        return (s32)CELL_VIDEO_EXPORT_ERROR_NOT_INITIALIZED;
    if (!progress) return (s32)CELL_VIDEO_EXPORT_ERROR_INVALID_ARGUMENT;
    *progress = 0.0f;
    return CELL_OK;
}
