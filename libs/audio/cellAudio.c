/*
 * ps3recomp - cellAudio HLE stub implementation
 *
 * In a full implementation this would feed PCM samples to the host audio
 * backend (WASAPI, PulseAudio, CoreAudio, SDL_Audio, etc.).  For now,
 * stubs track port state and return success.
 */

#include "cellAudio.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int s_audio_initialized = 0;

typedef struct {
    int                  in_use;
    int                  running;
    CellAudioPortParam   param;
} AudioPortSlot;

static AudioPortSlot s_ports[CELL_AUDIO_PORT_MAX];

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

/* NID: 0x0B168F92 */
s32 cellAudioInit(void)
{
    printf("[cellAudio] Init()\n");

    if (s_audio_initialized)
        return CELL_AUDIO_ERROR_ALREADY_INIT;

    memset(s_ports, 0, sizeof(s_ports));
    s_audio_initialized = 1;
    return CELL_OK;
}

/* NID: 0x4129FE2D */
s32 cellAudioQuit(void)
{
    printf("[cellAudio] Quit()\n");

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    /* Stop and close all open ports */
    for (int i = 0; i < CELL_AUDIO_PORT_MAX; i++) {
        s_ports[i].in_use  = 0;
        s_ports[i].running = 0;
    }

    s_audio_initialized = 0;
    return CELL_OK;
}

/* NID: 0xCD7BC431 */
s32 cellAudioPortOpen(const CellAudioPortParam* param, u32* portNum)
{
    printf("[cellAudio] PortOpen(nChannel=%llu, nBlock=%llu)\n",
           param ? (unsigned long long)param->nChannel : 0ULL,
           param ? (unsigned long long)param->nBlock : 0ULL);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (!param || !portNum)
        return CELL_AUDIO_ERROR_PARAM;

    /* Find a free port slot */
    for (u32 i = 0; i < CELL_AUDIO_PORT_MAX; i++) {
        if (!s_ports[i].in_use) {
            s_ports[i].in_use  = 1;
            s_ports[i].running = 0;
            s_ports[i].param   = *param;
            *portNum = i;
            return CELL_OK;
        }
    }

    return CELL_AUDIO_ERROR_PORT_FULL;
}

/* NID: 0x56DFFE09 */
s32 cellAudioPortClose(u32 portNum)
{
    printf("[cellAudio] PortClose(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    s_ports[portNum].in_use  = 0;
    s_ports[portNum].running = 0;
    return CELL_OK;
}

/* NID: 0x04AF134E */
s32 cellAudioPortStart(u32 portNum)
{
    printf("[cellAudio] PortStart(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (s_ports[portNum].running)
        return CELL_AUDIO_ERROR_PORT_ALREADY_RUN;

    s_ports[portNum].running = 1;
    return CELL_OK;
}

/* NID: 0x05DEAB16 */
s32 cellAudioPortStop(u32 portNum)
{
    printf("[cellAudio] PortStop(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (!s_ports[portNum].running)
        return CELL_AUDIO_ERROR_PORT_NOT_RUN;

    s_ports[portNum].running = 0;
    return CELL_OK;
}

/* NID: 0x74A66AF0 */
s32 cellAudioSetNotifyEventQueue(u64 key)
{
    printf("[cellAudio] SetNotifyEventQueue(key=0x%llX)\n",
           (unsigned long long)key);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    /*
     * TODO: Register the event queue so that the audio thread can send
     * notifications when a block has been consumed.
     */
    return CELL_OK;
}

/* NID: 0x4109D08C */
s32 cellAudioGetPortConfig(u32 portNum, CellAudioPortConfig* config)
{
    printf("[cellAudio] GetPortConfig(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (!config)
        return CELL_AUDIO_ERROR_PARAM;

    memset(config, 0, sizeof(CellAudioPortConfig));

    config->nChannel = s_ports[portNum].param.nChannel;
    config->nBlock   = s_ports[portNum].param.nBlock;
    config->status   = s_ports[portNum].running ? 1 : 0;

    /*
     * TODO: Allocate a real audio buffer from guest memory and provide
     * the guest addresses here.
     */
    config->portSize = (u32)(config->nChannel * config->nBlock *
                             CELL_AUDIO_BLOCK_SAMPLES * sizeof(float));
    config->portAddr       = 0;  /* placeholder */
    config->readIndexAddr  = 0;  /* placeholder */

    return CELL_OK;
}
