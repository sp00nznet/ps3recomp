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

s32 cellSailPlayerSetRepeatMode(CellSailPlayerHandle handle, s32 repeatMode)
{
    (void)repeatMode;
    printf("[cellSail] SetRepeatMode(%u, %d)\n", handle, repeatMode);
    if (handle >= CELL_SAIL_PLAYER_MAX || !s_players[handle].in_use)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Descriptor management
 * -----------------------------------------------------------------------*/

#define MAX_DESCRIPTORS 16

typedef struct {
    int in_use;
    s32 streamType;
    CellSailPlayerHandle player;
    char uri[512];
} SailDescriptor;

static SailDescriptor s_descs[MAX_DESCRIPTORS];

s32 cellSailPlayerCreateDescriptor(CellSailPlayerHandle handle,
                                     s32 streamType, void* arg,
                                     CellSailDescriptorHandle* desc)
{
    (void)arg;
    printf("[cellSail] CreateDescriptor(player=%u, type=%d)\n", handle, streamType);
    if (!desc) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;

    for (int i = 0; i < MAX_DESCRIPTORS; i++) {
        if (!s_descs[i].in_use) {
            memset(&s_descs[i], 0, sizeof(SailDescriptor));
            s_descs[i].in_use = 1;
            s_descs[i].streamType = streamType;
            s_descs[i].player = handle;
            *desc = (u32)i;
            return CELL_OK;
        }
    }
    return (s32)CELL_SAIL_ERROR_OUT_OF_MEMORY;
}

s32 cellSailPlayerDestroyDescriptor(CellSailPlayerHandle handle,
                                      CellSailDescriptorHandle desc)
{
    (void)handle;
    if (desc >= MAX_DESCRIPTORS || !s_descs[desc].in_use)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s_descs[desc].in_use = 0;
    return CELL_OK;
}

s32 cellSailPlayerAddDescriptor(CellSailPlayerHandle handle,
                                  CellSailDescriptorHandle desc)
{
    (void)handle;
    printf("[cellSail] AddDescriptor(player=%u, desc=%u)\n", handle, desc);
    if (desc >= MAX_DESCRIPTORS || !s_descs[desc].in_use)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    return CELL_OK;
}

s32 cellSailPlayerRemoveDescriptor(CellSailPlayerHandle handle,
                                     CellSailDescriptorHandle desc)
{
    (void)handle;
    if (desc >= MAX_DESCRIPTORS || !s_descs[desc].in_use)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    return CELL_OK;
}

s32 cellSailDescriptorCreateDatabase(CellSailDescriptorHandle desc,
                                       void* dbAddr, u32 dbSize, u64 arg)
{
    (void)dbAddr; (void)dbSize; (void)arg;
    printf("[cellSail] DescriptorCreateDatabase(desc=%u)\n", desc);
    if (desc >= MAX_DESCRIPTORS || !s_descs[desc].in_use)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    return CELL_OK;
}

s32 cellSailDescriptorDestroyDatabase(CellSailDescriptorHandle desc)
{
    if (desc >= MAX_DESCRIPTORS || !s_descs[desc].in_use)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    return CELL_OK;
}

s32 cellSailDescriptorGetStreamType(CellSailDescriptorHandle desc, s32* type)
{
    if (desc >= MAX_DESCRIPTORS || !s_descs[desc].in_use || !type)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    *type = s_descs[desc].streamType;
    return CELL_OK;
}

s32 cellSailDescriptorSetAutoSelection(CellSailDescriptorHandle desc, s32 enable)
{
    (void)enable;
    if (desc >= MAX_DESCRIPTORS || !s_descs[desc].in_use)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    return CELL_OK;
}

s32 cellSailDescriptorGetUri(CellSailDescriptorHandle desc, char* uri, u32 maxLen)
{
    if (desc >= MAX_DESCRIPTORS || !s_descs[desc].in_use || !uri)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    strncpy(uri, s_descs[desc].uri, maxLen - 1);
    uri[maxLen - 1] = '\0';
    return CELL_OK;
}

/* Memory allocator */

s32 cellSailMemAllocatorInitialize(CellSailMemAllocator* allocator,
                                     void* (*allocFunc)(void*, u32, u32),
                                     void  (*freeFunc)(void*, u32, void*),
                                     void* arg)
{
    printf("[cellSail] MemAllocatorInitialize()\n");
    if (!allocator) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    allocator->alloc = allocFunc;
    allocator->free = freeFunc;
    allocator->arg = arg;
    return CELL_OK;
}
