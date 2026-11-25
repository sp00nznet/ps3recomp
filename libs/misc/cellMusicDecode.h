/*
 * ps3recomp - cellMusicDecode HLE
 *
 * Background music decoding from XMB music library.
 * Stub — init/term work, decode operations return NOT_SUPPORTED.
 */

#ifndef PS3RECOMP_CELL_MUSIC_DECODE_H
#define PS3RECOMP_CELL_MUSIC_DECODE_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes (sysutil/sysutil_music_decode.h; base 0x8002C100 verified against
 * Sony firmware 4.93 libsysutil_music_decode.sprx: GetDecodeStatus(0)/Read/Init(0) -> 0x8002C102) */
#define CELL_MUSIC_DECODE_ERROR_NOT_INITIALIZED     (CELL_ERROR_BASE_SYSUTIL_MUSIC_DECODE | 0x01) /* 0x8002C101 */
#define CELL_MUSIC_DECODE_ERROR_PARAM               (CELL_ERROR_BASE_SYSUTIL_MUSIC_DECODE | 0x02) /* 0x8002C102 */
#define CELL_MUSIC_DECODE_ERROR_INVALID_ARGUMENT    CELL_MUSIC_DECODE_ERROR_PARAM
#define CELL_MUSIC_DECODE_ERROR_ALREADY_INITIALIZED (CELL_ERROR_BASE_SYSUTIL_MUSIC_DECODE | 0x03) /* 0x8002C103 */
#define CELL_MUSIC_DECODE_ERROR_NOT_SUPPORTED       (CELL_ERROR_BASE_SYSUTIL_MUSIC_DECODE | 0x04) /* 0x8002C104 */

/* Decode status */
#define CELL_MUSIC_DECODE_STATUS_DORMANT     0
#define CELL_MUSIC_DECODE_STATUS_DECODING    1
#define CELL_MUSIC_DECODE_STATUS_FINISHED    2

/* Types */
typedef void (*CellMusicDecodeCallback)(s32 event, void* arg, s32 result);

/* Functions */
s32 cellMusicDecodeInit(CellMusicDecodeCallback callback, void* arg);
s32 cellMusicDecodeFinish(void);

s32 cellMusicDecodeSetDecodeCommand(s32 command);
s32 cellMusicDecodeGetDecodeStatus(s32* status);
s32 cellMusicDecodeRead(void* buf, u32* size);

s32 cellMusicDecodeGetSelectionContext(void* ctx);
s32 cellMusicDecodeSetSelectionContext(const void* ctx);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_MUSIC_DECODE_H */
