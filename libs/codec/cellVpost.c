/*
 * ps3recomp - cellVpost HLE implementation
 *
 * Stub. Init/end manage handles, Exec returns success without
 * actual video processing. Query reports minimal memory needs.
 */

#include "cellVpost.h"
#include <stdio.h>
#include <string.h>

/* Internal state */

typedef struct {
    int in_use;
    CellVpostCfgParam cfg;
} VpostHandle;

static VpostHandle s_handles[CELL_VPOST_HANDLE_MAX];

/* API */

s32 cellVpostQuery(const CellVpostCfgParam* cfgParam, u32* memSize)
{
    (void)cfgParam;
    if (!memSize) return (s32)CELL_VPOST_ERROR_INVALID_ARGUMENT;
    /* Report a reasonable working buffer size */
    *memSize = 1024 * 1024; /* 1 MB */
    return CELL_OK;
}

s32 cellVpostInit(const CellVpostCfgParam* cfgParam,
                    const CellVpostResource* resource,
                    CellVpostHandle* handle)
{
    (void)resource;
    printf("[cellVpost] Init()\n");

    if (!cfgParam || !handle)
        return (s32)CELL_VPOST_ERROR_INVALID_ARGUMENT;

    for (int i = 0; i < CELL_VPOST_HANDLE_MAX; i++) {
        if (!s_handles[i].in_use) {
            memset(&s_handles[i], 0, sizeof(VpostHandle));
            s_handles[i].in_use = 1;
            s_handles[i].cfg = *cfgParam;
            *handle = (u32)i;
            return CELL_OK;
        }
    }
    return (s32)CELL_VPOST_ERROR_OUT_OF_MEMORY;
}

s32 cellVpostEnd(CellVpostHandle handle)
{
    printf("[cellVpost] End(%u)\n", handle);
    if (handle >= CELL_VPOST_HANDLE_MAX || !s_handles[handle].in_use)
        return (s32)CELL_VPOST_ERROR_HANDLE_NOT_FOUND;
    s_handles[handle].in_use = 0;
    return CELL_OK;
}

s32 cellVpostExec(CellVpostHandle handle,
                    const void* inPicBuf,
                    const CellVpostPictureInfo* picInfo,
                    void* outPicBuf,
                    const CellVpostCtrlParam* ctrlParam)
{
    (void)inPicBuf; (void)outPicBuf;
    if (handle >= CELL_VPOST_HANDLE_MAX || !s_handles[handle].in_use)
        return (s32)CELL_VPOST_ERROR_HANDLE_NOT_FOUND;
    if (!picInfo || !ctrlParam)
        return (s32)CELL_VPOST_ERROR_INVALID_ARGUMENT;

    /* Stub: no actual processing — just return success.
       A real impl would do color conversion / scaling here. */
    return CELL_OK;
}
