/*
 * ps3recomp - cellVoice HLE implementation
 *
 * Stub. Init/end work, port creation succeeds but no voice
 * data is captured or played. Games handle gracefully.
 */

#include "cellVoice.h"
#include <stdio.h>
#include <string.h>
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_write*: guest EA -> host */

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
            s_ports[i].param = *GUEST_PTR(param, const CellVoicePortParam*);
            vm_write32((u32)(uintptr_t)portId, (u32)i);
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
    *GUEST_PTR(param, CellVoicePortParam*) = s_ports[portId].param;
    return CELL_OK;
}

s32 cellVoiceWriteToIPort(CellVoicePortId portId, const void* data, u32* size)
{
    (void)data;
    if (!s_initialized) return (s32)CELL_VOICE_ERROR_NOT_INITIALIZED;
    if (portId >= CELL_VOICE_PORT_MAX || !s_ports[portId].in_use)
        return (s32)CELL_VOICE_ERROR_PORT_NOT_FOUND;
    /* Accept and discard voice data */
    if (size) vm_write32((u32)(uintptr_t)size, 0);
    return CELL_OK;
}

s32 cellVoiceReadFromOPort(CellVoicePortId portId, void* data, u32* size)
{
    (void)data;
    if (!s_initialized) return (s32)CELL_VOICE_ERROR_NOT_INITIALIZED;
    if (portId >= CELL_VOICE_PORT_MAX || !s_ports[portId].in_use)
        return (s32)CELL_VOICE_ERROR_PORT_NOT_FOUND;
    /* No voice data to return */
    if (size) vm_write32((u32)(uintptr_t)size, 0);
    return CELL_OK;
}

s32 cellVoiceSetCallback(CellVoiceEventCallback callback, void* arg)
{
    (void)callback; (void)arg;
    if (!s_initialized) return (s32)CELL_VOICE_ERROR_NOT_INITIALIZED;
    return CELL_OK;
}

/* Port control (RPCS3 rpcs3/Emu/Cell/Modules/cellVoice.cpp semantics).
 *
 * A port that does not exist is CELL_VOICE_ERROR_TOPOLOGY there (its
 * access_port check), not PORT_NOT_FOUND as the older calls above return. No
 * voice data flows here, so there is nothing to flush on a reset and no signal
 * to measure: each call logs once so the missing audio path stays visible. */

static int voice_port_valid(CellVoicePortId portId)
{
    return portId < CELL_VOICE_PORT_MAX && s_ports[portId].in_use;
}

s32 cellVoiceResetPort(CellVoicePortId portId)
{
    static int logged = 0;
    if (!logged) { logged = 1; printf("[cellVoice] ResetPort(%u): no voice buffers to flush (stub)\n", portId); }
    if (!s_initialized) return (s32)CELL_VOICE_ERROR_NOT_INITIALIZED;
    if (!voice_port_valid(portId)) return (s32)CELL_VOICE_ERROR_TOPOLOGY;
    return CELL_OK;
}

s32 cellVoiceUpdatePort(CellVoicePortId portId, const CellVoicePortParam* param)
{
    static int logged = 0;
    if (!logged) { logged = 1; printf("[cellVoice] UpdatePort(%u): parameters stored, no voice processing\n", portId); }
    if (!s_initialized) return (s32)CELL_VOICE_ERROR_NOT_INITIALIZED;
    if (!param) return (s32)CELL_VOICE_ERROR_ARGUMENT_INVALID;
    if (!voice_port_valid(portId)) return (s32)CELL_VOICE_ERROR_TOPOLOGY;
    /* s_ports[].param holds the guest's bytes as CreatePort copied them.
     * Like RPCS3, update threshold, bMute and volume (bytes 4..11) and, for
     * voice ports (IN_VOICE 2 / OUT_VOICE 4), the bitrate (bytes 12..15);
     * the port type and PCM format stay as created. */
    uint8_t* dst = (uint8_t*)&s_ports[portId].param;
    const uint8_t* src = GUEST_PTR(param, const uint8_t*);
    u32 type = ((u32)dst[0] << 24) | ((u32)dst[1] << 16) | ((u32)dst[2] << 8) | dst[3];
    memcpy(dst + 4, src + 4, 8);
    if (type == 2 || type == 4)
        memcpy(dst + 12, src + 12, 4);
    return CELL_OK;
}

s32 cellVoiceGetSignalState(CellVoicePortId portId, u32 attr, void* attrValue)
{
    static int logged = 0;
    if (!logged) { logged = 1; printf("[cellVoice] GetSignalState(%u, attr=%u): no microphone -> DEVICE_NOT_PRESENT\n", portId, attr); }
    (void)attrValue;
    if (!s_initialized) return (s32)CELL_VOICE_ERROR_NOT_INITIALIZED;
    if (!voice_port_valid(portId)) return (s32)CELL_VOICE_ERROR_TOPOLOGY;
    /* RPCS3 reports a detached microphone and does not write attrValue. */
    return (s32)CELL_VOICE_ERROR_DEVICE_NOT_PRESENT;
}
