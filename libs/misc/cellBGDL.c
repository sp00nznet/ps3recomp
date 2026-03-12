/*
 * ps3recomp - cellBGDL HLE implementation
 *
 * Stub. Init/term work, download list always empty.
 */

#include "cellBGDL.h"
#include <stdio.h>
#include <string.h>

/* Internal state */

static int s_initialized = 0;

/* API */

s32 cellBgdlInit(void)
{
    printf("[cellBGDL] Init()\n");
    if (s_initialized)
        return (s32)CELL_BGDL_ERROR_ALREADY_INITIALIZED;
    s_initialized = 1;
    return CELL_OK;
}

s32 cellBgdlTerm(void)
{
    printf("[cellBGDL] Term()\n");
    s_initialized = 0;
    return CELL_OK;
}

s32 cellBgdlGetInfo(CellBgdlId id, CellBgdlInfo* info)
{
    (void)id; (void)info;
    if (!s_initialized) return (s32)CELL_BGDL_ERROR_NOT_INITIALIZED;
    return (s32)CELL_BGDL_ERROR_NOT_FOUND;
}

s32 cellBgdlGetList(CellBgdlInfo* list, u32 maxEntries, u32* numEntries)
{
    (void)list; (void)maxEntries;
    if (!s_initialized) return (s32)CELL_BGDL_ERROR_NOT_INITIALIZED;
    if (!numEntries) return (s32)CELL_BGDL_ERROR_INVALID_ARGUMENT;
    *numEntries = 0; /* no downloads */
    return CELL_OK;
}

s32 cellBgdlStartDownload(const char* url, const char* contentId,
                            CellBgdlId* id)
{
    (void)url; (void)contentId; (void)id;
    printf("[cellBGDL] StartDownload() - stub\n");
    if (!s_initialized) return (s32)CELL_BGDL_ERROR_NOT_INITIALIZED;
    return (s32)CELL_BGDL_ERROR_BUSY;
}

s32 cellBgdlPauseDownload(CellBgdlId id)
{
    (void)id;
    if (!s_initialized) return (s32)CELL_BGDL_ERROR_NOT_INITIALIZED;
    return (s32)CELL_BGDL_ERROR_NOT_FOUND;
}

s32 cellBgdlResumeDownload(CellBgdlId id)
{
    (void)id;
    if (!s_initialized) return (s32)CELL_BGDL_ERROR_NOT_INITIALIZED;
    return (s32)CELL_BGDL_ERROR_NOT_FOUND;
}

s32 cellBgdlCancelDownload(CellBgdlId id)
{
    (void)id;
    if (!s_initialized) return (s32)CELL_BGDL_ERROR_NOT_INITIALIZED;
    return (s32)CELL_BGDL_ERROR_NOT_FOUND;
}
