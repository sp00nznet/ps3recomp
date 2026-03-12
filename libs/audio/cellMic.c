/*
 * ps3recomp - cellMic HLE implementation
 *
 * Stub. Init/end work, all queries report no microphone.
 */

#include "cellMic.h"
#include <stdio.h>
#include <string.h>

/* Internal state */

static int s_initialized = 0;

/* API */

s32 cellMicInit(void)
{
    printf("[cellMic] Init()\n");
    if (s_initialized)
        return (s32)CELL_MIC_ERROR_ALREADY_INITIALIZED;
    s_initialized = 1;
    return CELL_OK;
}

s32 cellMicEnd(void)
{
    printf("[cellMic] End()\n");
    s_initialized = 0;
    return CELL_OK;
}

s32 cellMicOpen(s32 devNum, u32 sampleRate)
{
    (void)devNum; (void)sampleRate;
    printf("[cellMic] Open(%d) - no device\n", devNum);
    return (s32)CELL_MIC_ERROR_DEVICE_NOT_FOUND;
}

s32 cellMicClose(s32 devNum)
{
    (void)devNum;
    return CELL_OK;
}

s32 cellMicStart(s32 devNum)
{
    (void)devNum;
    return (s32)CELL_MIC_ERROR_DEVICE_NOT_FOUND;
}

s32 cellMicStop(s32 devNum)
{
    (void)devNum;
    return CELL_OK;
}

s32 cellMicRead(s32 devNum, void* data, u32 maxBytes, u32* readBytes)
{
    (void)devNum; (void)data; (void)maxBytes;
    if (readBytes) *readBytes = 0;
    return (s32)CELL_MIC_ERROR_DEVICE_NOT_FOUND;
}

s32 cellMicGetDeviceAttr(s32 devNum, CellMicDeviceAttr* attr)
{
    (void)devNum;
    if (!attr) return (s32)CELL_MIC_ERROR_INVALID_ARGUMENT;
    return (s32)CELL_MIC_ERROR_DEVICE_NOT_FOUND;
}

s32 cellMicIsAttached(s32 devNum)
{
    (void)devNum;
    return 0; /* not attached */
}

s32 cellMicIsOpen(s32 devNum)
{
    (void)devNum;
    return 0; /* not open */
}

s32 cellMicGetType(s32 devNum, s32* type)
{
    (void)devNum;
    if (!type) return (s32)CELL_MIC_ERROR_INVALID_ARGUMENT;
    *type = CELL_MIC_TYPE_UNKNOWN;
    return CELL_OK;
}

s32 cellMicSetNotifyCallback(CellMicCallback callback, void* arg)
{
    (void)callback; (void)arg;
    if (!s_initialized) return (s32)CELL_MIC_ERROR_NOT_INITIALIZED;
    return CELL_OK;
}

s32 cellMicGetSignalState(s32 devNum, s32* signalState)
{
    (void)devNum;
    if (!signalState) return (s32)CELL_MIC_ERROR_INVALID_ARGUMENT;
    *signalState = 0;
    return CELL_OK;
}

s32 cellMicSetSignalAttr(s32 devNum, u32 attr, u32 value)
{
    (void)devNum; (void)attr; (void)value;
    return CELL_OK;
}

s32 cellMicGetDeviceGUID(s32 devNum, u64* guid)
{
    (void)devNum;
    if (!guid) return (s32)CELL_MIC_ERROR_INVALID_ARGUMENT;
    *guid = 0;
    return CELL_OK;
}
