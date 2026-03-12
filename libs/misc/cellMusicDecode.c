/*
 * ps3recomp - cellMusicDecode HLE implementation
 *
 * Stub. Init/term work, decode returns NOT_SUPPORTED.
 */

#include "cellMusicDecode.h"
#include <stdio.h>

/* Internal state */

static int s_initialized = 0;

/* API */

s32 cellMusicDecodeInit(CellMusicDecodeCallback callback, void* arg)
{
    (void)callback; (void)arg;
    printf("[cellMusicDecode] Init()\n");
    if (s_initialized)
        return (s32)CELL_MUSIC_DECODE_ERROR_ALREADY_INITIALIZED;
    s_initialized = 1;
    return CELL_OK;
}

s32 cellMusicDecodeFinish(void)
{
    printf("[cellMusicDecode] Finish()\n");
    s_initialized = 0;
    return CELL_OK;
}

s32 cellMusicDecodeSetDecodeCommand(s32 command)
{
    (void)command;
    if (!s_initialized) return (s32)CELL_MUSIC_DECODE_ERROR_NOT_INITIALIZED;
    return (s32)CELL_MUSIC_DECODE_ERROR_NOT_SUPPORTED;
}

s32 cellMusicDecodeGetDecodeStatus(s32* status)
{
    if (!s_initialized) return (s32)CELL_MUSIC_DECODE_ERROR_NOT_INITIALIZED;
    if (!status) return (s32)CELL_MUSIC_DECODE_ERROR_INVALID_ARGUMENT;
    *status = CELL_MUSIC_DECODE_STATUS_DORMANT;
    return CELL_OK;
}

s32 cellMusicDecodeRead(void* buf, u32* size)
{
    (void)buf;
    if (!s_initialized) return (s32)CELL_MUSIC_DECODE_ERROR_NOT_INITIALIZED;
    if (size) *size = 0;
    return (s32)CELL_MUSIC_DECODE_ERROR_NOT_SUPPORTED;
}

s32 cellMusicDecodeGetSelectionContext(void* ctx)
{
    (void)ctx;
    if (!s_initialized) return (s32)CELL_MUSIC_DECODE_ERROR_NOT_INITIALIZED;
    return (s32)CELL_MUSIC_DECODE_ERROR_NOT_SUPPORTED;
}

s32 cellMusicDecodeSetSelectionContext(const void* ctx)
{
    (void)ctx;
    if (!s_initialized) return (s32)CELL_MUSIC_DECODE_ERROR_NOT_INITIALIZED;
    return (s32)CELL_MUSIC_DECODE_ERROR_NOT_SUPPORTED;
}
