/*
 * ps3recomp - cellPhotoImport HLE implementation
 *
 * Stub. Init/term work, import returns NOT_SUPPORTED.
 */

#include "cellPhotoImport.h"
#include "../system/cellSysutil.h"
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

    /* callback is a guest OPD address; queue via sysutil instead of calling directly. */
    if (callback) {
        const u64 ev[8] = {(u64)(s64)(s32)CELL_PHOTO_IMPORT_ERROR_NOT_SUPPORTED,
                           0, (u64)(uintptr_t)userdata, 0, 0, 0, 0, 0};
        cellSysutilQueueGuestCallbackArgs((u32)(uintptr_t)callback, ev);
    }

    return (s32)CELL_PHOTO_IMPORT_ERROR_NOT_SUPPORTED;
}
