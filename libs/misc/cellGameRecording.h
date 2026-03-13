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

/* Error codes */
#define CELL_GAME_REC_ERROR_NOT_INITIALIZED     0x80610701
#define CELL_GAME_REC_ERROR_ALREADY_INITIALIZED 0x80610702
#define CELL_GAME_REC_ERROR_INVALID_ARGUMENT    0x80610703
#define CELL_GAME_REC_ERROR_OUT_OF_MEMORY       0x80610704
#define CELL_GAME_REC_ERROR_NOT_SUPPORTED       0x80610705
#define CELL_GAME_REC_ERROR_BUSY                0x80610706

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
