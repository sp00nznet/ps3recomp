/*
 * ps3recomp - cellGameRecording HLE
 *
 * In-game video recording. Captures framebuffer output for replay/sharing.
 * Stub — init/end lifecycle, recording is a no-op (no framebuffer capture).
 */

#ifndef PS3RECOMP_CELL_GAME_RECORDING_H
#define PS3RECOMP_CELL_GAME_RECORDING_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes (PS3 sysutil_rec / cellRec, base 0x8002C500; verified against RPCS3) */
#define CELL_REC_ERROR_OUT_OF_MEMORY            (CELL_ERROR_BASE_SYSUTIL_REC | 0x01) /* 0x8002C501 */
#define CELL_REC_ERROR_FATAL                    (CELL_ERROR_BASE_SYSUTIL_REC | 0x02) /* 0x8002C502 */
#define CELL_REC_ERROR_INVALID_VALUE            (CELL_ERROR_BASE_SYSUTIL_REC | 0x03) /* 0x8002C503 */
#define CELL_REC_ERROR_FILE_OPEN                (CELL_ERROR_BASE_SYSUTIL_REC | 0x04) /* 0x8002C504 */
#define CELL_REC_ERROR_FILE_WRITE               (CELL_ERROR_BASE_SYSUTIL_REC | 0x05) /* 0x8002C505 */
#define CELL_REC_ERROR_INVALID_STATE            (CELL_ERROR_BASE_SYSUTIL_REC | 0x06) /* 0x8002C506 */
#define CELL_REC_ERROR_FILE_NO_DATA             (CELL_ERROR_BASE_SYSUTIL_REC | 0x07) /* 0x8002C507 */

#define CELL_GAME_REC_ERROR_NOT_INITIALIZED     CELL_REC_ERROR_INVALID_STATE         /* 0x8002C506 */
#define CELL_GAME_REC_ERROR_ALREADY_INITIALIZED CELL_REC_ERROR_INVALID_STATE         /* 0x8002C506 */
#define CELL_GAME_REC_ERROR_INVALID_ARGUMENT    CELL_REC_ERROR_INVALID_VALUE         /* 0x8002C503 */
#define CELL_GAME_REC_ERROR_OUT_OF_MEMORY       CELL_REC_ERROR_OUT_OF_MEMORY         /* 0x8002C501 */
#define CELL_GAME_REC_ERROR_NOT_SUPPORTED       CELL_REC_ERROR_FATAL                 /* 0x8002C502 */
#define CELL_GAME_REC_ERROR_BUSY                CELL_REC_ERROR_INVALID_STATE         /* 0x8002C506 */

/* Recording quality */
#define CELL_GAME_REC_QUALITY_LOW     0
#define CELL_GAME_REC_QUALITY_MEDIUM  1
#define CELL_GAME_REC_QUALITY_HIGH    2

/* Recording parameters */
typedef struct CellGameRecordingParam {
    u32 quality;
    u32 maxDuration; /* seconds */
    u32 width;
    u32 height;
    u32 fps;
    u32 flags;
    u32 reserved[4];
} CellGameRecordingParam;

/* Functions */
s32 cellGameRecordingInit(const CellGameRecordingParam* param);
s32 cellGameRecordingEnd(void);
s32 cellGameRecordingStart(void);
s32 cellGameRecordingStop(void);
s32 cellGameRecordingPause(void);
s32 cellGameRecordingResume(void);
s32 cellGameRecordingIsRecording(void);
s32 cellGameRecordingGetDuration(float* duration);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_GAME_RECORDING_H */
