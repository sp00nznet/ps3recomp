/*
 * ps3recomp - cellVoice HLE
 *
 * Voice chat system: voice capture, encoding, and playback.
 * Stub — init/term work, no voice ports are available.
 */

#ifndef PS3RECOMP_CELL_VOICE_H
#define PS3RECOMP_CELL_VOICE_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define CELL_VOICE_ERROR_NOT_INITIALIZED     0x80310801
#define CELL_VOICE_ERROR_ALREADY_INITIALIZED 0x80310802
#define CELL_VOICE_ERROR_INVALID_ARGUMENT    0x80310803
#define CELL_VOICE_ERROR_PORT_NOT_FOUND      0x80310804
#define CELL_VOICE_ERROR_OUT_OF_MEMORY       0x80310805
#define CELL_VOICE_ERROR_SERVICE_DETACHED    0x80310806

/* Constants */
#define CELL_VOICE_PORT_MAX          16
#define CELL_VOICE_BITRATE_3850      3850
#define CELL_VOICE_BITRATE_4650      4650
#define CELL_VOICE_BITRATE_5700      5700
#define CELL_VOICE_BITRATE_7300      7300
#define CELL_VOICE_BITRATE_14400     14400

/* Port types */
#define CELL_VOICE_PORT_IN_MIC       0
#define CELL_VOICE_PORT_IN_PCMAUDIO  1
#define CELL_VOICE_PORT_OUT_PCMAUDIO 2
#define CELL_VOICE_PORT_OUT_SECONDARY 3

/* Types */
typedef u32 CellVoicePortId;

typedef struct CellVoiceInitParam {
    s32 appType;
    s32 reserved;
    u32 version;
    u32 reserved2[4];
} CellVoiceInitParam;

typedef struct CellVoicePortParam {
    u32 portType;
    u32 threshold;
    u32 bitsRate;
    u32 sampleRate;
    u32 volume;
    u32 reserved[4];
} CellVoicePortParam;

typedef void (*CellVoiceEventCallback)(s32 event, void* arg);

/* Functions */
s32 cellVoiceInit(const CellVoiceInitParam* param);
s32 cellVoiceEnd(void);

s32 cellVoiceCreatePort(const CellVoicePortParam* param, CellVoicePortId* portId);
s32 cellVoiceDeletePort(CellVoicePortId portId);

s32 cellVoiceConnectIPortToOPort(CellVoicePortId inPortId, CellVoicePortId outPortId);
s32 cellVoiceDisconnectIPortFromOPort(CellVoicePortId inPortId, CellVoicePortId outPortId);

s32 cellVoiceStart(void);
s32 cellVoiceStop(void);

s32 cellVoiceSetNotifyEventType(u32 eventType, u32 enableFlag);
s32 cellVoiceSetMuteFlag(CellVoicePortId portId, u32 muteFlag);
s32 cellVoiceSetVolume(CellVoicePortId portId, float volume);

s32 cellVoiceGetPortInfo(CellVoicePortId portId, CellVoicePortParam* param);
s32 cellVoiceWriteToIPort(CellVoicePortId portId, const void* data, u32* size);
s32 cellVoiceReadFromOPort(CellVoicePortId portId, void* data, u32* size);

s32 cellVoiceSetCallback(CellVoiceEventCallback callback, void* arg);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_VOICE_H */
