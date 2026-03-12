/*
 * ps3recomp - cellAdec HLE
 *
 * Audio decoder: AAC, ATRAC3plus, MP3, CELP audio decoding.
 * Receives AU from cellDmux and outputs decoded PCM audio.
 */

#ifndef PS3RECOMP_CELL_ADEC_H
#define PS3RECOMP_CELL_ADEC_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
#define CELL_ADEC_ERROR_ARG            0x80610201
#define CELL_ADEC_ERROR_SEQ            0x80610202
#define CELL_ADEC_ERROR_BUSY           0x80610203
#define CELL_ADEC_ERROR_EMPTY          0x80610204
#define CELL_ADEC_ERROR_AU             0x80610205
#define CELL_ADEC_ERROR_PCM            0x80610206
#define CELL_ADEC_ERROR_FATAL          0x80610207

/* ---------------------------------------------------------------------------
 * Codec types
 * -----------------------------------------------------------------------*/
#define CELL_ADEC_TYPE_ATRACX          0   /* ATRAC3plus */
#define CELL_ADEC_TYPE_LPCM            1   /* Linear PCM */
#define CELL_ADEC_TYPE_AC3             2   /* AC3 / Dolby Digital */
#define CELL_ADEC_TYPE_MP3             3   /* MPEG-1 Layer 3 */
#define CELL_ADEC_TYPE_AAC             4   /* AAC */
#define CELL_ADEC_TYPE_CELP8           5   /* CELP 8kHz */

/* Callback message types */
#define CELL_ADEC_MSG_TYPE_AUDONE      0
#define CELL_ADEC_MSG_TYPE_PCMOUT      1
#define CELL_ADEC_MSG_TYPE_SEQDONE     2
#define CELL_ADEC_MSG_TYPE_ERROR       3

/* ---------------------------------------------------------------------------
 * Types
 * -----------------------------------------------------------------------*/
typedef u32 CellAdecHandle;

typedef struct CellAdecType {
    u32 audioCodecType;
} CellAdecType;

typedef struct CellAdecResource {
    u32 memAddr;
    u32 memSize;
    u32 ppuThreadPrio;
    u32 ppuThreadStackSize;
    u32 spuThreadPrio;
    u32 numOfSpus;
} CellAdecResource;

typedef struct CellAdecAuInfo {
    u32 startAddr;
    u32 size;
    u64 pts;
    u64 userData;
} CellAdecAuInfo;

typedef struct CellAdecPcmItem {
    u32 pcmAddr;
    u32 pcmSize;
    u32 status;
    u64 pts;
    u32 channelNumber;
    u32 samplingRate;
    u32 bitsPerSample;
    u64 userData;
} CellAdecPcmItem;

typedef u32 (*CellAdecCbMsg)(CellAdecHandle handle, u32 msgType,
                               s32 msgData, void* cbArg);

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 cellAdecOpen(const CellAdecType* type, const CellAdecResource* res,
                  CellAdecCbMsg cbFunc, void* cbArg, CellAdecHandle* handle);
s32 cellAdecClose(CellAdecHandle handle);

s32 cellAdecStartSeq(CellAdecHandle handle, void* param);
s32 cellAdecEndSeq(CellAdecHandle handle);

s32 cellAdecDecodeAu(CellAdecHandle handle, const CellAdecAuInfo* auInfo);

s32 cellAdecGetPcm(CellAdecHandle handle, void* outBuffer);
s32 cellAdecGetPcmItem(CellAdecHandle handle, const CellAdecPcmItem** pcmItem);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_ADEC_H */
