/*
 * ps3recomp - cellSubdisplay HLE implementation
 *
 * Sub-display output for PS Vita Remote Play.
 * Stub — init/end lifecycle works, always reports not connected.
 */

#include "cellSubdisplay.h"
#include <stdio.h>
#include <string.h>

/* Internal state */

static int s_initialized = 0;
static int s_started = 0;

/* API */

s32 cellSubdisplayInit(const CellSubdisplayConfig* config)
{
    (void)config;
    printf("[cellSubdisplay] Init()\n");
    if (s_initialized)
        return (s32)CELL_SUBDISPLAY_ERROR_ALREADY_INITIALIZED;
    s_initialized = 1;
    s_started = 0;
    return CELL_OK;
}

s32 cellSubdisplayEnd(void)
{
    printf("[cellSubdisplay] End()\n");
    s_initialized = 0;
    s_started = 0;
    return CELL_OK;
}

s32 cellSubdisplayStart(void)
{
    printf("[cellSubdisplay] Start()\n");
    if (!s_initialized)
        return (s32)CELL_SUBDISPLAY_ERROR_NOT_INITIALIZED;
    s_started = 1;
    return CELL_OK;
}

s32 cellSubdisplayStop(void)
{
    printf("[cellSubdisplay] Stop()\n");
    if (!s_initialized)
        return (s32)CELL_SUBDISPLAY_ERROR_NOT_INITIALIZED;
    s_started = 0;
    return CELL_OK;
}

s32 cellSubdisplayGetRequiredMemory(u32* size)
{
    if (!size) return (s32)CELL_SUBDISPLAY_ERROR_INVALID_ARGUMENT;
    /* Minimal buffer requirement */
    *size = 1024 * 1024; /* 1 MB */
    return CELL_OK;
}

s32 cellSubdisplayGetTouchData(CellSubdisplayTouchData* data)
{
    if (!s_initialized)
        return (s32)CELL_SUBDISPLAY_ERROR_NOT_INITIALIZED;
    if (!data) return (s32)CELL_SUBDISPLAY_ERROR_INVALID_ARGUMENT;

    /* No sub-display connected, return empty data */
    memset(data, 0, sizeof(*data));
    return CELL_OK;
}

s32 cellSubdisplayIsConnected(void)
{
    if (!s_initialized)
        return (s32)CELL_SUBDISPLAY_ERROR_NOT_INITIALIZED;
    /* Never connected — no Vita in a recompiled environment */
    return 0;
}
