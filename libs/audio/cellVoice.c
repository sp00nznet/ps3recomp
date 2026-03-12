/*
 * ps3recomp - cellVoice HLE implementation
 *
 * Stub. Init/end work, port creation succeeds but no voice
 * data is captured or played. Games handle gracefully.
 */

#include "cellVoice.h"
#include <stdio.h>
#include <string.h>

/* Internal state */

static int s_initialized = 0;
static int s_started = 0;

typedef struct {
    int in_use;
    CellVoicePortParam param;
} VoicePort;

static VoicePort s_ports[CELL_VOICE_PORT_MAX];

/* API */

s32 cellVoiceInit(const CellVoiceInitParam* param)
{
    (void)param;
    printf("[cellVoice] Init()\n");
    if (s_initialized)
        return (s32)CELL_VOICE_ERROR_ALREADY_INITIALIZED;
    memset(s_ports, 0, sizeof(s_ports));
    s_initialized = 1;
    s_started = 0;
    return CELL_OK;
}

s32 cellVoiceEnd(void)
{
    printf("[cellVoice] End()\n");
    s_initialized = 0;
    s_started = 0;
    return CELL_OK;
}

s32 cellVoiceCreatePort(const CellVoicePortParam* param, CellVoicePortId* portId)
{
    if (!s_initialized) return (s32)CELL_VOICE_ERROR_NOT_INITIALIZED;
    if (!param || !portId) return (s32)CELL_VOICE_ERROR_INVALID_ARGUMENT;

    for (int i = 0; i < CELL_VOICE_PORT_MAX; i++) {
        if (!s_ports[i].in_use) {
            s_ports[i].in_use = 1;
            s_ports[i].param = *param;
            *portId = (u32)i;
            return CELL_OK;
        }
    }
    return (s32)CELL_VOICE_ERROR_OUT_OF_MEMORY;
}

s32 cellVoiceDeletePort(CellVoicePortId portId)
{
    if (!s_initialized) return (s32)CELL_VOICE_ERROR_NOT_INITIALIZED;
    if (portId >= CELL_VOICE_PORT_MAX || !s_ports[portId].in_use)
        return (s32)CELL_VOICE_ERROR_PORT_NOT_FOUND;
    s_ports[portId].in_use = 0;
    return CELL_OK;
}

s32 cellVoiceConnectIPortToOPort(CellVoicePortId inPortId, CellVoicePortId outPortId)
{
    (void)inPortId; (void)outPortId;
    if (!s_initialized) return (s32)CELL_VOICE_ERROR_NOT_INITIALIZED;
    return CELL_OK;
}

s32 cellVoiceDisconnectIPortFromOPort(CellVoicePortId inPortId, CellVoicePortId outPortId)
{
    (void)inPortId; (void)outPortId;
    if (!s_initialized) return (s32)CELL_VOICE_ERROR_NOT_INITIALIZED;
    return CELL_OK;
}

s32 cellVoiceStart(void)
{
    printf("[cellVoice] Start()\n");
    if (!s_initialized) return (s32)CELL_VOICE_ERROR_NOT_INITIALIZED;
    s_started = 1;
    return CELL_OK;
}

s32 cellVoiceStop(void)
{
    printf("[cellVoice] Stop()\n");
    if (!s_initialized) return (s32)CELL_VOICE_ERROR_NOT_INITIALIZED;
    s_started = 0;
    return CELL_OK;
}

s32 cellVoiceSetNotifyEventType(u32 eventType, u32 enableFlag)
{
    (void)eventType; (void)enableFlag;
    if (!s_initialized) return (s32)CELL_VOICE_ERROR_NOT_INITIALIZED;
    return CELL_OK;
}

s32 cellVoiceSetMuteFlag(CellVoicePortId portId, u32 muteFlag)
{
    (void)muteFlag;
    if (!s_initialized) return (s32)CELL_VOICE_ERROR_NOT_INITIALIZED;
    if (portId >= CELL_VOICE_PORT_MAX || !s_ports[portId].in_use)
        return (s32)CELL_VOICE_ERROR_PORT_NOT_FOUND;
    return CELL_OK;
}

s32 cellVoiceSetVolume(CellVoicePortId portId, float volume)
{
    (void)volume;
    if (!s_initialized) return (s32)CELL_VOICE_ERROR_NOT_INITIALIZED;
    if (portId >= CELL_VOICE_PORT_MAX || !s_ports[portId].in_use)
        return (s32)CELL_VOICE_ERROR_PORT_NOT_FOUND;
    return CELL_OK;
}

s32 cellVoiceGetPortInfo(CellVoicePortId portId, CellVoicePortParam* param)
{
    if (!s_initialized) return (s32)CELL_VOICE_ERROR_NOT_INITIALIZED;
    if (portId >= CELL_VOICE_PORT_MAX || !s_ports[portId].in_use)
        return (s32)CELL_VOICE_ERROR_PORT_NOT_FOUND;
    if (!param) return (s32)CELL_VOICE_ERROR_INVALID_ARGUMENT;
    *param = s_ports[portId].param;
    return CELL_OK;
}

s32 cellVoiceWriteToIPort(CellVoicePortId portId, const void* data, u32* size)
{
    (void)data;
    if (!s_initialized) return (s32)CELL_VOICE_ERROR_NOT_INITIALIZED;
    if (portId >= CELL_VOICE_PORT_MAX || !s_ports[portId].in_use)
        return (s32)CELL_VOICE_ERROR_PORT_NOT_FOUND;
    /* Accept and discard voice data */
    if (size) *size = 0;
    return CELL_OK;
}

s32 cellVoiceReadFromOPort(CellVoicePortId portId, void* data, u32* size)
{
    (void)data;
    if (!s_initialized) return (s32)CELL_VOICE_ERROR_NOT_INITIALIZED;
    if (portId >= CELL_VOICE_PORT_MAX || !s_ports[portId].in_use)
        return (s32)CELL_VOICE_ERROR_PORT_NOT_FOUND;
    /* No voice data to return */
    if (size) *size = 0;
    return CELL_OK;
}

s32 cellVoiceSetCallback(CellVoiceEventCallback callback, void* arg)
{
    (void)callback; (void)arg;
    if (!s_initialized) return (s32)CELL_VOICE_ERROR_NOT_INITIALIZED;
    return CELL_OK;
}
