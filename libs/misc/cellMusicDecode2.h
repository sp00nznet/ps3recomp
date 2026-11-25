/*
 * ps3recomp - cellMusicDecode2 HLE
 *
 * Extended background music decoding with format info.
 * Stub — init/term work, decode operations return NOT_SUPPORTED.
 */

#ifndef PS3RECOMP_CELL_MUSIC_DECODE2_H
#define PS3RECOMP_CELL_MUSIC_DECODE2_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes (shared with cellMusicDecode; base 0x8002C100 verified against
 * Sony firmware 4.93 libsysutil_music_decode.sprx: Initialize2(0)/GetDecodeStatus2(0) -> 0x8002C102) */
#define CELL_MUSIC_DECODE2_ERROR_NOT_INITIALIZED     (CELL_ERROR_BASE_SYSUTIL_MUSIC_DECODE | 0x01) /* 0x8002C101 */
#define CELL_MUSIC_DECODE2_ERROR_PARAM               (CELL_ERROR_BASE_SYSUTIL_MUSIC_DECODE | 0x02) /* 0x8002C102 */
#define CELL_MUSIC_DECODE2_ERROR_INVALID_ARGUMENT    CELL_MUSIC_DECODE2_ERROR_PARAM
#define CELL_MUSIC_DECODE2_ERROR_ALREADY_INITIALIZED (CELL_ERROR_BASE_SYSUTIL_MUSIC_DECODE | 0x03) /* 0x8002C103 */
#define CELL_MUSIC_DECODE2_ERROR_NOT_SUPPORTED       (CELL_ERROR_BASE_SYSUTIL_MUSIC_DECODE | 0x04) /* 0x8002C104 */

/* Types */
typedef struct CellMusicDecode2Info {
    u32 sampleRate;
    u32 channels;
    u32 bitsPerSample;
    u32 codecType;
    u32 reserved[4];
} CellMusicDecode2Info;

typedef void (*CellMusicDecode2Callback)(s32 event, void* arg, s32 result);

/* Functions */
s32 cellMusicDecode2Init(CellMusicDecode2Callback callback, void* arg);
s32 cellMusicDecode2Finalize(void);

s32 cellMusicDecode2SetDecodeCommand(s32 command);
s32 cellMusicDecode2GetDecodeStatus(s32* status);
s32 cellMusicDecode2Read(void* buf, u32* size);

s32 cellMusicDecode2GetInfo(CellMusicDecode2Info* info);
s32 cellMusicDecode2GetSelectionContext(void* ctx);
s32 cellMusicDecode2SetSelectionContext(const void* ctx);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_MUSIC_DECODE2_H */
