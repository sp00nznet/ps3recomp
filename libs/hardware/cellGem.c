/*
 * ps3recomp - cellGem HLE implementation
 *
 * Stub. Init/end work, all queries report no Move controllers connected.
 */

#include "cellGem.h"
#include <stdio.h>
#include <string.h>
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_read/vm_write: guest EA -> host */

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
    s_max_connect = vm_read32((u32)(uintptr_t)attr + 4);   /* maxConnect */
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

    u32 info_ea = (u32)(uintptr_t)info;
    for (u32 o = 0; o < sizeof(CellGemInfo); o += 4)
        vm_write32(info_ea + o, 0);
    vm_write32(info_ea + 0, s_max_connect);   /* maxConnect */
    vm_write32(info_ea + 4, 0);               /* nowConnect: none      */
    for (u32 i = 0; i < CELL_GEM_MAX_NUM; i++)
        vm_write32(info_ea + 8 + i * 4, CELL_GEM_STATUS_DISCONNECTED);
    return CELL_OK;
}

s32 cellGemGetState(u32 gemNum, u32 flag, u64 timestamp, CellGemState* state)
{
    (void)gemNum; (void)flag; (void)timestamp;
    if (!s_initialized) return (s32)CELL_GEM_ERROR_NOT_INITIALIZED;
    if (!state) return (s32)CELL_GEM_ERROR_INVALID_ARGUMENT;
    /* Not an error code: a title keeps the result as the controller's status.
     * Resistance 3 reads anything but CELL_GEM_NOT_CONNECTED as "a Move needs
     * calibrating" and opens its PlayStation Eye dialog, which nothing closes. */
    return CELL_GEM_NOT_CONNECTED;
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
    vm_write64((u32)(uintptr_t)flags, 0);
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

/* cellGemGetMemorySize(s32 max_connect): the memory cellGemInit wants for that
 * many controllers (RPCS3 cellGem.cpp: 0x120000 for up to two, 0x140000
 * beyond; more than CELL_GEM_MAX_NUM is an error). inFAMOUS 2 asks before
 * cellGemInit; unanswered, the import fell through to the unresolved stub. */
s32 cellGemGetMemorySize(s32 max_connect)
{
    if (max_connect <= 0 || max_connect > CELL_GEM_MAX_NUM)
        return (s32)CELL_GEM_ERROR_INVALID_ARGUMENT;
    return max_connect <= 2 ? 0x120000 : 0x140000;
}
