/*
 * ps3recomp - cellMusicDecode2 HLE implementation
 *
 * Stub. Init/term work, decode returns NOT_SUPPORTED.
 */

#include "cellMusicDecode2.h"
#include <stdio.h>

/* Internal state */

static int s_initialized2 = 0;

/* API */

s32 cellMusicDecode2Init(CellMusicDecode2Callback callback, void* arg)
{
    (void)callback; (void)arg;
    printf("[cellMusicDecode2] Init()\n");
    if (s_initialized2)
        return (s32)CELL_MUSIC_DECODE2_ERROR_ALREADY_INITIALIZED;
    s_initialized2 = 1;
    return CELL_OK;
}

s32 cellMusicDecode2Finalize(void)
{
    printf("[cellMusicDecode2] Finalize()\n");
    s_initialized2 = 0;
    return CELL_OK;
}

s32 cellMusicDecode2SetDecodeCommand(s32 command)
{
    (void)command;
    if (!s_initialized2) return (s32)CELL_MUSIC_DECODE2_ERROR_NOT_INITIALIZED;
    return (s32)CELL_MUSIC_DECODE2_ERROR_NOT_SUPPORTED;
}

s32 cellMusicDecode2GetDecodeStatus(s32* status)
{
    if (!s_initialized2) return (s32)CELL_MUSIC_DECODE2_ERROR_NOT_INITIALIZED;
    if (!status) return (s32)CELL_MUSIC_DECODE2_ERROR_INVALID_ARGUMENT;
    *status = 0; /* dormant */
    return CELL_OK;
}

s32 cellMusicDecode2Read(void* buf, u32* size)
{
    (void)buf;
    if (!s_initialized2) return (s32)CELL_MUSIC_DECODE2_ERROR_NOT_INITIALIZED;
    if (size) *size = 0;
    return (s32)CELL_MUSIC_DECODE2_ERROR_NOT_SUPPORTED;
}

s32 cellMusicDecode2GetInfo(CellMusicDecode2Info* info)
{
    if (!s_initialized2) return (s32)CELL_MUSIC_DECODE2_ERROR_NOT_INITIALIZED;
    if (!info) return (s32)CELL_MUSIC_DECODE2_ERROR_INVALID_ARGUMENT;
    return (s32)CELL_MUSIC_DECODE2_ERROR_NOT_SUPPORTED;
}

s32 cellMusicDecode2GetSelectionContext(void* ctx)
{
    (void)ctx;
    if (!s_initialized2) return (s32)CELL_MUSIC_DECODE2_ERROR_NOT_INITIALIZED;
    return (s32)CELL_MUSIC_DECODE2_ERROR_NOT_SUPPORTED;
}

s32 cellMusicDecode2SetSelectionContext(const void* ctx)
{
    (void)ctx;
    if (!s_initialized2) return (s32)CELL_MUSIC_DECODE2_ERROR_NOT_INITIALIZED;
    return (s32)CELL_MUSIC_DECODE2_ERROR_NOT_SUPPORTED;
}
