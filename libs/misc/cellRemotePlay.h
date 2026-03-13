/*
 * ps3recomp - cellRemotePlay HLE
 *
 * Remote Play for PS Vita / PSP streaming.
 * Stub — init/end lifecycle, always reports not available.
 */

#ifndef PS3RECOMP_CELL_REMOTE_PLAY_H
#define PS3RECOMP_CELL_REMOTE_PLAY_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define CELL_REMOTE_PLAY_ERROR_NOT_INITIALIZED     0x80029601
#define CELL_REMOTE_PLAY_ERROR_ALREADY_INITIALIZED 0x80029602
#define CELL_REMOTE_PLAY_ERROR_INVALID_ARGUMENT    0x80029603
#define CELL_REMOTE_PLAY_ERROR_NOT_SUPPORTED       0x80029604

/* Functions */
s32 cellRemotePlayInit(void);
s32 cellRemotePlayEnd(void);
s32 cellRemotePlayIsAvailable(void);
s32 cellRemotePlayGetStatus(u32* status);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_REMOTE_PLAY_H */
