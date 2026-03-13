/*
 * ps3recomp - cellGameRecording HLE implementation
 *
 * Stub. Init/end lifecycle works, recording is a silent no-op.
 */

#include "cellGameRecording.h"
#include <stdio.h>

/* Internal state */

static int s_initialized = 0;
static int s_recording = 0;

/* API */

s32 cellGameRecordingInit(const CellGameRecordingParam* param)
{
    (void)param;
    printf("[cellGameRecording] Init()\n");
    if (s_initialized)
        return (s32)CELL_GAME_REC_ERROR_ALREADY_INITIALIZED;
    s_initialized = 1;
    s_recording = 0;
    return CELL_OK;
}

s32 cellGameRecordingEnd(void)
{
    printf("[cellGameRecording] End()\n");
    s_initialized = 0;
    s_recording = 0;
    return CELL_OK;
}

s32 cellGameRecordingStart(void)
{
    printf("[cellGameRecording] Start()\n");
    if (!s_initialized)
        return (s32)CELL_GAME_REC_ERROR_NOT_INITIALIZED;
    s_recording = 1;
    return CELL_OK;
}

s32 cellGameRecordingStop(void)
{
    printf("[cellGameRecording] Stop()\n");
    if (!s_initialized)
        return (s32)CELL_GAME_REC_ERROR_NOT_INITIALIZED;
    s_recording = 0;
    return CELL_OK;
}

s32 cellGameRecordingPause(void)
{
    if (!s_initialized)
        return (s32)CELL_GAME_REC_ERROR_NOT_INITIALIZED;
    s_recording = 0;
    return CELL_OK;
}

s32 cellGameRecordingResume(void)
{
    if (!s_initialized)
        return (s32)CELL_GAME_REC_ERROR_NOT_INITIALIZED;
    s_recording = 1;
    return CELL_OK;
}

s32 cellGameRecordingIsRecording(void)
{
    if (!s_initialized)
        return (s32)CELL_GAME_REC_ERROR_NOT_INITIALIZED;
    return s_recording;
}

s32 cellGameRecordingGetDuration(float* duration)
{
    if (!s_initialized)
        return (s32)CELL_GAME_REC_ERROR_NOT_INITIALIZED;
    if (!duration) return (s32)CELL_GAME_REC_ERROR_INVALID_ARGUMENT;
    *duration = 0.0f; /* No actual recording */
    return CELL_OK;
}
