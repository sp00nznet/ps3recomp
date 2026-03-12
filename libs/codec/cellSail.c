/*
 * ps3recomp - cellSail HLE implementation
 *
 * Stub. Player lifecycle and state management work.
 * No actual media playback occurs — games typically show
 * a black screen for cutscenes, which is acceptable for
 * initial recompilation testing.
 */

#include "cellSail.h"
#include <stdio.h>
#include <string.h>

/* Internal state */

static int s_initialized = 0;

typedef struct {
    int in_use;
    s32 state;
    CellSailPlayerCallback callback;
    void* callbackArg;
} SailPlayer;

static SailPlayer s_players[CELL_SAIL_PLAYER_MAX];

/* Lifecycle */

s32 cellSailInit(void)
{
    printf("[cellSail] Init()\n");
    if (s_initialized)
        return (s32)CELL_SAIL_ERROR_ALREADY_INITIALIZED;
    memset(s_players, 0, sizeof(s_players));
    s_initialized = 1;
    return CELL_OK;
}

s32 cellSailTerm(void)
{
    printf("[cellSail] Term()\n");
    s_initialized = 0;
    return CELL_OK;
}

/* Player management */

s32 cellSailPlayerCreate(const CellSailPlayerAttribute* attr,
                           const CellSailPlayerResource* resource,
                           CellSailPlayerCallback callback,
                           void* callbackArg,
                           CellSailPlayerHandle* handle)
{
    (void)attr; (void)resource;
    printf("[cellSail] PlayerCreate()\n");

    if (!s_initialized) return (s32)CELL_SAIL_ERROR_NOT_INITIALIZED;
    if (!handle) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;

    for (int i = 0; i < CELL_SAIL_PLAYER_MAX; i++) {
        if (!s_players[i].in_use) {
            memset(&s_players[i], 0, sizeof(SailPlayer));
            s_players[i].in_use = 1;
            s_players[i].state = CELL_SAIL_PLAYER_STATE_INITIALIZED;
            s_players[i].callback = callback;
            s_players[i].callbackArg = callbackArg;
            *handle = (u32)i;
            return CELL_OK;
        }
    }
    return (s32)CELL_SAIL_ERROR_OUT_OF_MEMORY;
}

s32 cellSailPlayerDestroy(CellSailPlayerHandle handle)
{
    printf("[cellSail] PlayerDestroy(%u)\n", handle);
    if (handle >= CELL_SAIL_PLAYER_MAX || !s_players[handle].in_use)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s_players[handle].in_use = 0;
    s_players[handle].state = CELL_SAIL_PLAYER_STATE_CLOSED;
    return CELL_OK;
}

s32 cellSailPlayerBoot(CellSailPlayerHandle handle, u64 userParam)
{
    (void)userParam;
    printf("[cellSail] PlayerBoot(%u)\n", handle);
    if (handle >= CELL_SAIL_PLAYER_MAX || !s_players[handle].in_use)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s_players[handle].state = CELL_SAIL_PLAYER_STATE_RUNNING;
    return CELL_OK;
}

s32 cellSailPlayerOpenStream(CellSailPlayerHandle handle, const char* path)
{
    printf("[cellSail] PlayerOpenStream(%u, \"%s\") - stub\n", handle,
           path ? path : "null");
    if (handle >= CELL_SAIL_PLAYER_MAX || !s_players[handle].in_use)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    /* Stub: don't actually open anything */
    return CELL_OK;
}

s32 cellSailPlayerCloseStream(CellSailPlayerHandle handle)
{
    if (handle >= CELL_SAIL_PLAYER_MAX || !s_players[handle].in_use)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    return CELL_OK;
}

s32 cellSailPlayerStart(CellSailPlayerHandle handle)
{
    printf("[cellSail] PlayerStart(%u)\n", handle);
    if (handle >= CELL_SAIL_PLAYER_MAX || !s_players[handle].in_use)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s_players[handle].state = CELL_SAIL_PLAYER_STATE_RUNNING;
    /* Immediately signal finished since we don't play anything */
    s_players[handle].state = CELL_SAIL_PLAYER_STATE_FINISHED;
    return CELL_OK;
}

s32 cellSailPlayerStop(CellSailPlayerHandle handle)
{
    printf("[cellSail] PlayerStop(%u)\n", handle);
    if (handle >= CELL_SAIL_PLAYER_MAX || !s_players[handle].in_use)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s_players[handle].state = CELL_SAIL_PLAYER_STATE_FINISHED;
    return CELL_OK;
}

s32 cellSailPlayerPause(CellSailPlayerHandle handle)
{
    if (handle >= CELL_SAIL_PLAYER_MAX || !s_players[handle].in_use)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s_players[handle].state = CELL_SAIL_PLAYER_STATE_PAUSE;
    return CELL_OK;
}

s32 cellSailPlayerGetState(CellSailPlayerHandle handle, s32* state)
{
    if (handle >= CELL_SAIL_PLAYER_MAX || !s_players[handle].in_use)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    if (!state) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    *state = s_players[handle].state;
    return CELL_OK;
}

s32 cellSailPlayerGetStreamNum(CellSailPlayerHandle handle, u32* streamNum)
{
    if (handle >= CELL_SAIL_PLAYER_MAX || !s_players[handle].in_use)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    if (!streamNum) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    *streamNum = 0; /* no streams in stub */
    return CELL_OK;
}

s32 cellSailPlayerGetStreamInfo(CellSailPlayerHandle handle, u32 streamIndex,
                                  CellSailStreamInfo* info)
{
    (void)streamIndex;
    if (handle >= CELL_SAIL_PLAYER_MAX || !s_players[handle].in_use)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    if (!info) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    return (s32)CELL_SAIL_ERROR_NOT_FOUND;
}

s32 cellSailPlayerSetSoundAdapter(CellSailPlayerHandle handle, u32 index)
{
    (void)index;
    if (handle >= CELL_SAIL_PLAYER_MAX || !s_players[handle].in_use)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    return CELL_OK;
}

s32 cellSailPlayerSetGraphicsAdapter(CellSailPlayerHandle handle, u32 index)
{
    (void)index;
    if (handle >= CELL_SAIL_PLAYER_MAX || !s_players[handle].in_use)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    return CELL_OK;
}

s32 cellSailPlayerCancel(CellSailPlayerHandle handle)
{
    if (handle >= CELL_SAIL_PLAYER_MAX || !s_players[handle].in_use)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s_players[handle].state = CELL_SAIL_PLAYER_STATE_FINISHED;
    return CELL_OK;
}

s32 cellSailPlayerIsPaused(CellSailPlayerHandle handle)
{
    if (handle >= CELL_SAIL_PLAYER_MAX || !s_players[handle].in_use)
        return 0;
    return (s_players[handle].state == CELL_SAIL_PLAYER_STATE_PAUSE) ? 1 : 0;
}
