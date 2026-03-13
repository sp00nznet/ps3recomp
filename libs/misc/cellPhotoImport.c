/*
 * ps3recomp - cellPhotoImport HLE implementation
 *
 * Stub. Init/term work, import returns NOT_SUPPORTED.
 */

#include "cellPhotoImport.h"
#include <stdio.h>

/* Internal state */

static int s_initialized = 0;

/* API */

s32 cellPhotoImportInit(void)
{
    printf("[cellPhotoImport] Init()\n");
    if (s_initialized)
        return (s32)CELL_PHOTO_IMPORT_ERROR_ALREADY_INITIALIZED;
    s_initialized = 1;
    return CELL_OK;
}

s32 cellPhotoImportEnd(void)
{
    printf("[cellPhotoImport] End()\n");
    s_initialized = 0;
    return CELL_OK;
}

s32 cellPhotoImportStart(CellPhotoImportCallback callback, void* userdata)
{
    printf("[cellPhotoImport] Start() - not supported\n");
    if (!s_initialized)
        return (s32)CELL_PHOTO_IMPORT_ERROR_NOT_INITIALIZED;

    if (callback)
        callback((s32)CELL_PHOTO_IMPORT_ERROR_NOT_SUPPORTED, NULL, userdata);

    return (s32)CELL_PHOTO_IMPORT_ERROR_NOT_SUPPORTED;
}
