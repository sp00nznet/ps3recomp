/*
 * ps3recomp - cellAdecCelp8 HLE
 *
 * CELP8 (Code-Excited Linear Prediction, 8kHz) voice codec decoder.
 * Used for voice chat and low-bandwidth audio on PS3.
 * Stub — decoder management works, decode produces silence.
 */

#ifndef PS3RECOMP_CELL_ADEC_CELP8_H
#define PS3RECOMP_CELL_ADEC_CELP8_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define CELL_ADEC_CELP8_ERROR_NOT_INITIALIZED     0x80612201
#define CELL_ADEC_CELP8_ERROR_ALREADY_INITIALIZED 0x80612202
#define CELL_ADEC_CELP8_ERROR_INVALID_ARGUMENT    0x80612203
#define CELL_ADEC_CELP8_ERROR_OUT_OF_MEMORY       0x80612204
#define CELL_ADEC_CELP8_ERROR_DECODE_FAILED       0x80612205

/* CELP8 operates at 8kHz mono */
#define CELL_ADEC_CELP8_SAMPLE_RATE      8000
#define CELL_ADEC_CELP8_CHANNELS         1
#define CELL_ADEC_CELP8_SAMPLES_PER_FRAME 160  /* 20ms at 8kHz */

/* Bit rate modes */
#define CELL_ADEC_CELP8_BITRATE_5700   5700
#define CELL_ADEC_CELP8_BITRATE_6200   6200
#define CELL_ADEC_CELP8_BITRATE_7700   7700
#define CELL_ADEC_CELP8_BITRATE_14400  14400

/* Handle */
typedef u32 CellAdecCelp8Handle;

/* Config */
typedef struct CellAdecCelp8Config {
    u32 bitRate;
    u32 flags;
    u32 reserved[4];
} CellAdecCelp8Config;

/* Functions */
s32 cellAdecCelp8Open(const CellAdecCelp8Config* config, CellAdecCelp8Handle* handle);
s32 cellAdecCelp8Close(CellAdecCelp8Handle handle);
s32 cellAdecCelp8Decode(CellAdecCelp8Handle handle, const void* frame, u32 frameSize,
                         s16* pcmOut, u32* numSamples);
s32 cellAdecCelp8Reset(CellAdecCelp8Handle handle);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_ADEC_CELP8_H */
