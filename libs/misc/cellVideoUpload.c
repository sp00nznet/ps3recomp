/*
 * ps3recomp - cellVideoUpload HLE implementation
 *
 * Stub. Init/term work, upload returns NOT_SUPPORTED.
 */

#include "cellVideoUpload.h"
#include <stdio.h>

/* Internal state */

static int s_initialized = 0;

/* API */

s32 cellVideoUploadInit(void)
{
    printf("[cellVideoUpload] Init()\n");
    if (s_initialized)
        return (s32)CELL_VIDEO_UPLOAD_ERROR_ALREADY_INITIALIZED;
    s_initialized = 1;
    return CELL_OK;
}

s32 cellVideoUploadTerm(void)
{
    printf("[cellVideoUpload] Term()\n");
    s_initialized = 0;
    return CELL_OK;
}

s32 cellVideoUploadStart(const CellVideoUploadParam* param,
                           CellVideoUploadCallback callback, void* arg)
{
    (void)param; (void)callback; (void)arg;
    printf("[cellVideoUpload] Start() - not supported\n");
    if (!s_initialized) return (s32)CELL_VIDEO_UPLOAD_ERROR_NOT_INITIALIZED;
    return (s32)CELL_VIDEO_UPLOAD_ERROR_NOT_SUPPORTED;
}

s32 cellVideoUploadCancel(void)
{
    if (!s_initialized) return (s32)CELL_VIDEO_UPLOAD_ERROR_NOT_INITIALIZED;
    return CELL_OK;
}

s32 cellVideoUploadGetStatus(s32* status)
{
    if (!s_initialized) return (s32)CELL_VIDEO_UPLOAD_ERROR_NOT_INITIALIZED;
    if (!status) return (s32)CELL_VIDEO_UPLOAD_ERROR_INVALID_ARGUMENT;
    *status = 0; /* idle */
    return CELL_OK;
}
