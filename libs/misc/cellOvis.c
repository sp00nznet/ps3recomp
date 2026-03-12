/*
 * ps3recomp - cellOvis HLE implementation
 *
 * Stub. Init/term work, overlay creation succeeds as no-op.
 */

#include "cellOvis.h"
#include <stdio.h>

/* Internal state */

static int s_initialized = 0;
static u32 s_next_handle = 1;

/* API */

s32 cellOvisInit(void)
{
    printf("[cellOvis] Init()\n");
    if (s_initialized)
        return (s32)CELL_OVIS_ERROR_ALREADY_INITIALIZED;
    s_initialized = 1;
    s_next_handle = 1;
    return CELL_OK;
}

s32 cellOvisTerm(void)
{
    printf("[cellOvis] Term()\n");
    s_initialized = 0;
    return CELL_OK;
}

s32 cellOvisGetOverlayTableSize(const char* filePath, u32* tableSize)
{
    (void)filePath;
    if (!s_initialized) return (s32)CELL_OVIS_ERROR_NOT_INITIALIZED;
    if (!tableSize) return (s32)CELL_OVIS_ERROR_INVALID_ARGUMENT;
    *tableSize = 0;
    return CELL_OK;
}

s32 cellOvisCreateOverlay(const void* table, u32 tableSize, CellOvisHandle* handle)
{
    (void)table; (void)tableSize;
    if (!s_initialized) return (s32)CELL_OVIS_ERROR_NOT_INITIALIZED;
    if (!handle) return (s32)CELL_OVIS_ERROR_INVALID_ARGUMENT;
    *handle = s_next_handle++;
    return CELL_OK;
}

s32 cellOvisDestroyOverlay(CellOvisHandle handle)
{
    (void)handle;
    if (!s_initialized) return (s32)CELL_OVIS_ERROR_NOT_INITIALIZED;
    return CELL_OK;
}

s32 cellOvisInvalidateOverlay(CellOvisHandle handle)
{
    (void)handle;
    if (!s_initialized) return (s32)CELL_OVIS_ERROR_NOT_INITIALIZED;
    return CELL_OK;
}
