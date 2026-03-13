/*
 * ps3recomp - cellAdecAtrac3p HLE
 *
 * ATRAC3plus audio decoder — Sony's advanced lossy audio codec.
 * Used for game audio and background music on PS3.
 * Stub — decoder management works, decode produces silence.
 */

#ifndef PS3RECOMP_CELL_ADEC_ATRAC3P_H
#define PS3RECOMP_CELL_ADEC_ATRAC3P_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define CELL_ADEC_ATRAC3P_ERROR_NOT_INITIALIZED     0x80612101
#define CELL_ADEC_ATRAC3P_ERROR_ALREADY_INITIALIZED 0x80612102
#define CELL_ADEC_ATRAC3P_ERROR_INVALID_ARGUMENT    0x80612103
#define CELL_ADEC_ATRAC3P_ERROR_OUT_OF_MEMORY       0x80612104
#define CELL_ADEC_ATRAC3P_ERROR_DECODE_FAILED       0x80612105

/* Channel config */
#define CELL_ADEC_ATRAC3P_CHANNELS_MONO    1
#define CELL_ADEC_ATRAC3P_CHANNELS_STEREO  2

/* Sample rate */
#define CELL_ADEC_ATRAC3P_SRATE_44100  44100
#define CELL_ADEC_ATRAC3P_SRATE_48000  48000

/* Samples per frame */
#define CELL_ADEC_ATRAC3P_SAMPLES_PER_FRAME 2048

/* Handle */
typedef u32 CellAdecAtrac3pHandle;

/* Config */
typedef struct CellAdecAtrac3pConfig {
    u32 channels;
    u32 sampleRate;
    u32 bitRate;
    u32 flags;
    u32 reserved[4];
} CellAdecAtrac3pConfig;

/* PCM output info */
typedef struct CellAdecAtrac3pPcmInfo {
    u32 channels;
    u32 sampleRate;
    u32 numSamples;
    u32 bitsPerSample;
} CellAdecAtrac3pPcmInfo;

/* Functions */
s32 cellAdecAtrac3pOpen(const CellAdecAtrac3pConfig* config, CellAdecAtrac3pHandle* handle);
s32 cellAdecAtrac3pClose(CellAdecAtrac3pHandle handle);
s32 cellAdecAtrac3pDecode(CellAdecAtrac3pHandle handle, const void* au, u32 auSize,
                           void* pcmOut, u32 pcmBufSize, CellAdecAtrac3pPcmInfo* info);
s32 cellAdecAtrac3pReset(CellAdecAtrac3pHandle handle);
s32 cellAdecAtrac3pGetPcmInfo(CellAdecAtrac3pHandle handle, CellAdecAtrac3pPcmInfo* info);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_ADEC_ATRAC3P_H */
