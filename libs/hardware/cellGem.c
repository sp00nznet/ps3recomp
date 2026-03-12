/*
 * ps3recomp - cellGem HLE implementation
 *
 * Stub. Init/end work, all queries report no Move controllers connected.
 */

#include "cellGem.h"
#include <stdio.h>
#include <string.h>

/* Internal state */

static int s_initialized = 0;
static u32 s_max_connect = 0;

/* API */

s32 cellGemInit(const CellGemAttribute* attr)
{
    printf("[cellGem] Init()\n");
    if (s_initialized)
        return (s32)CELL_GEM_ERROR_ALREADY_INITIALIZED;
    if (!attr) return (s32)CELL_GEM_ERROR_INVALID_ARGUMENT;
    s_max_connect = attr->maxConnect;
    if (s_max_connect > CELL_GEM_MAX_NUM) s_max_connect = CELL_GEM_MAX_NUM;
    s_initialized = 1;
    return CELL_OK;
}

s32 cellGemEnd(void)
{
    printf("[cellGem] End()\n");
    s_initialized = 0;
    return CELL_OK;
}

s32 cellGemGetInfo(CellGemInfo* info)
{
    if (!s_initialized) return (s32)CELL_GEM_ERROR_NOT_INITIALIZED;
    if (!info) return (s32)CELL_GEM_ERROR_INVALID_ARGUMENT;

    memset(info, 0, sizeof(CellGemInfo));
    info->maxConnect = s_max_connect;
    info->nowConnect = 0; /* no controllers */
    for (u32 i = 0; i < CELL_GEM_MAX_NUM; i++)
        info->status[i] = CELL_GEM_STATUS_DISCONNECTED;
    return CELL_OK;
}

s32 cellGemGetState(u32 gemNum, u32 flag, u64 timestamp, CellGemState* state)
{
    (void)gemNum; (void)flag; (void)timestamp;
    if (!s_initialized) return (s32)CELL_GEM_ERROR_NOT_INITIALIZED;
    if (!state) return (s32)CELL_GEM_ERROR_INVALID_ARGUMENT;
    return (s32)CELL_GEM_ERROR_NOT_CONNECTED;
}

s32 cellGemIsTrackableHue(u32 hue)
{
    (void)hue;
    if (!s_initialized) return (s32)CELL_GEM_ERROR_NOT_INITIALIZED;
    return 1; /* any hue is "trackable" */
}

s32 cellGemGetHue(u32 gemNum, u32* hue)
{
    (void)gemNum;
    if (!s_initialized) return (s32)CELL_GEM_ERROR_NOT_INITIALIZED;
    if (!hue) return (s32)CELL_GEM_ERROR_INVALID_ARGUMENT;
    return (s32)CELL_GEM_ERROR_NOT_CONNECTED;
}

s32 cellGemTrackHues(const u32* reqHues, u32* resHues)
{
    (void)reqHues; (void)resHues;
    if (!s_initialized) return (s32)CELL_GEM_ERROR_NOT_INITIALIZED;
    return CELL_OK;
}

s32 cellGemCalibrate(u32 gemNum)
{
    (void)gemNum;
    if (!s_initialized) return (s32)CELL_GEM_ERROR_NOT_INITIALIZED;
    return (s32)CELL_GEM_ERROR_NOT_CONNECTED;
}

s32 cellGemGetStatusFlags(u32 gemNum, u64* flags)
{
    (void)gemNum;
    if (!s_initialized) return (s32)CELL_GEM_ERROR_NOT_INITIALIZED;
    if (!flags) return (s32)CELL_GEM_ERROR_INVALID_ARGUMENT;
    *flags = 0;
    return CELL_OK;
}

s32 cellGemUpdateStart(const void* cameraFrame, u64 timestamp)
{
    (void)cameraFrame; (void)timestamp;
    if (!s_initialized) return (s32)CELL_GEM_ERROR_NOT_INITIALIZED;
    return CELL_OK;
}

s32 cellGemUpdateFinish(void)
{
    if (!s_initialized) return (s32)CELL_GEM_ERROR_NOT_INITIALIZED;
    return CELL_OK;
}

s32 cellGemReset(u32 gemNum)
{
    (void)gemNum;
    if (!s_initialized) return (s32)CELL_GEM_ERROR_NOT_INITIALIZED;
    return CELL_OK;
}

s32 cellGemEnableMagnetometer(u32 gemNum, s32 enable)
{
    (void)gemNum; (void)enable;
    if (!s_initialized) return (s32)CELL_GEM_ERROR_NOT_INITIALIZED;
    return CELL_OK;
}

s32 cellGemEnableCameraPitchAngleCorrection(s32 enable)
{
    (void)enable;
    if (!s_initialized) return (s32)CELL_GEM_ERROR_NOT_INITIALIZED;
    return CELL_OK;
}
