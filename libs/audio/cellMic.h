/*
 * ps3recomp - cellMic HLE
 *
 * Microphone input capture. Stub — reports no microphone available.
 */

#ifndef PS3RECOMP_CELL_MIC_H
#define PS3RECOMP_CELL_MIC_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes (PS3 cell/mic.h, base 0x80140100; verified against RPCS3) */
#define CELL_MIC_ERROR_ALREADY_INITIALIZED (CELL_ERROR_BASE_MIC | 0x01) /* 0x80140101 */
#define CELL_MIC_ERROR_DEVICE              (CELL_ERROR_BASE_MIC | 0x02) /* 0x80140102 */
#define CELL_MIC_ERROR_NOT_INITIALIZED     (CELL_ERROR_BASE_MIC | 0x03) /* 0x80140103 */
#define CELL_MIC_ERROR_INVALID_ARGUMENT    (CELL_ERROR_BASE_MIC | 0x04) /* 0x80140104 */
#define CELL_MIC_ERROR_NOT_OPEN            (CELL_ERROR_BASE_MIC | 0x05) /* 0x80140105 */
#define CELL_MIC_ERROR_NOT_STARTED         (CELL_ERROR_BASE_MIC | 0x06) /* 0x80140106 */
#define CELL_MIC_ERROR_BUSY                (CELL_ERROR_BASE_MIC | 0x07) /* 0x80140107 */
#define CELL_MIC_ERROR_DEVICE_NOT_FOUND    (CELL_ERROR_BASE_MIC | 0x0E) /* 0x8014010E */

/* Constants */
#define CELL_MIC_MAX_DEVICES   4
#define CELL_MIC_SAMPLE_RATE_48000  48000
#define CELL_MIC_SAMPLE_RATE_16000  16000

/* Device types */
#define CELL_MIC_TYPE_UNKNOWN     0
#define CELL_MIC_TYPE_USBAUDIO    1
#define CELL_MIC_TYPE_BLUETOOTH   2
#define CELL_MIC_TYPE_EYETOY      3

/* Types */
typedef struct CellMicDeviceAttr {
    u32 sampleRate;
    u32 channels;
    u32 reserved[4];
} CellMicDeviceAttr;

typedef void (*CellMicCallback)(s32 devNum, s32 event, void* arg);

/* Functions */
s32 cellMicInit(void);
s32 cellMicEnd(void);

s32 cellMicOpen(s32 devNum, u32 sampleRate);
s32 cellMicClose(s32 devNum);

s32 cellMicStart(s32 devNum);
s32 cellMicStop(s32 devNum);

s32 cellMicRead(s32 devNum, void* data, u32 maxBytes, u32* readBytes);
s32 cellMicGetDeviceAttr(s32 devNum, CellMicDeviceAttr* attr);

s32 cellMicIsAttached(s32 devNum);
s32 cellMicIsOpen(s32 devNum);

s32 cellMicGetType(s32 devNum, s32* type);
s32 cellMicSetNotifyCallback(CellMicCallback callback, void* arg);

s32 cellMicGetSignalState(s32 devNum, s32* signalState);
s32 cellMicSetSignalAttr(s32 devNum, u32 attr, u32 value);

s32 cellMicGetDeviceGUID(s32 devNum, u64* guid);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_MIC_H */
