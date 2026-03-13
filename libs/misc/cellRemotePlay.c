/*
 * ps3recomp - cellRemotePlay HLE implementation
 *
 * Stub. Init/term work, remote play is never available.
 */

#include "cellRemotePlay.h"
#include <stdio.h>

/* Internal state */

static int s_initialized = 0;

/* API */

s32 cellRemotePlayInit(void)
{
    printf("[cellRemotePlay] Init()\n");
    if (s_initialized)
        return (s32)CELL_REMOTE_PLAY_ERROR_ALREADY_INITIALIZED;
    s_initialized = 1;
    return CELL_OK;
}

s32 cellRemotePlayEnd(void)
{
    printf("[cellRemotePlay] End()\n");
    s_initialized = 0;
    return CELL_OK;
}

s32 cellRemotePlayIsAvailable(void)
{
    if (!s_initialized)
        return (s32)CELL_REMOTE_PLAY_ERROR_NOT_INITIALIZED;
    return 0; /* Never available */
}

s32 cellRemotePlayGetStatus(u32* status)
{
    if (!s_initialized)
        return (s32)CELL_REMOTE_PLAY_ERROR_NOT_INITIALIZED;
    if (!status) return (s32)CELL_REMOTE_PLAY_ERROR_INVALID_ARGUMENT;
    *status = 0; /* Disconnected */
    return CELL_OK;
}
