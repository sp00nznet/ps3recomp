/*
 * ps3recomp - cellCamera HLE implementation
 *
 * Stub. Init/end work, all device queries report no camera.
 */

#include "cellCamera.h"
#include <stdio.h>
#include <string.h>

/* Internal state */

static int s_initialized = 0;

/* API */

s32 cellCameraInit(void)
{
    printf("[cellCamera] Init()\n");
    if (s_initialized)
        return (s32)CELL_CAMERA_ERROR_ALREADY_INITIALIZED;
    s_initialized = 1;
    return CELL_OK;
}

s32 cellCameraEnd(void)
{
    printf("[cellCamera] End()\n");
    s_initialized = 0;
    return CELL_OK;
}

s32 cellCameraOpen(s32 devNum, CellCameraInfo* info)
{
    (void)devNum; (void)info;
    printf("[cellCamera] Open(%d) - no device\n", devNum);
    return (s32)CELL_CAMERA_ERROR_DEVICE_NOT_FOUND;
}

s32 cellCameraClose(s32 devNum)
{
    (void)devNum;
    return CELL_OK;
}

s32 cellCameraStart(s32 devNum)
{
    (void)devNum;
    return (s32)CELL_CAMERA_ERROR_DEVICE_NOT_FOUND;
}

s32 cellCameraStop(s32 devNum)
{
    (void)devNum;
    return CELL_OK;
}

s32 cellCameraRead(s32 devNum, CellCameraReadInfo* info)
{
    (void)devNum; (void)info;
    return (s32)CELL_CAMERA_ERROR_DEVICE_NOT_FOUND;
}

s32 cellCameraReadEx(s32 devNum, CellCameraReadInfo* info)
{
    (void)devNum; (void)info;
    return (s32)CELL_CAMERA_ERROR_DEVICE_NOT_FOUND;
}

s32 cellCameraIsAvailable(s32 devNum)
{
    (void)devNum;
    return 0; /* not available */
}

s32 cellCameraIsAttached(s32 devNum)
{
    (void)devNum;
    return 0; /* not attached */
}

s32 cellCameraIsOpen(s32 devNum)
{
    (void)devNum;
    return 0; /* not open */
}

s32 cellCameraIsStarted(s32 devNum)
{
    (void)devNum;
    return 0; /* not started */
}

s32 cellCameraGetType(s32 devNum, s32* type)
{
    (void)devNum;
    if (!type) return (s32)CELL_CAMERA_ERROR_INVALID_ARGUMENT;
    *type = CELL_CAMERA_TYPE_UNKNOWN;
    return CELL_OK;
}

s32 cellCameraSetNotifyCallback(s32 devNum, CellCameraCallback cb, void* arg)
{
    (void)devNum; (void)cb; (void)arg;
    return CELL_OK;
}

s32 cellCameraRemoveNotifyCallback(s32 devNum)
{
    (void)devNum;
    return CELL_OK;
}
