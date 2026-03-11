/*
 * ps3recomp - cellAudio HLE stub
 *
 * Audio output: port management, initialization, event notification.
 */

#ifndef PS3RECOMP_CELL_AUDIO_H
#define PS3RECOMP_CELL_AUDIO_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

#define CELL_AUDIO_PORT_MAX                 8
#define CELL_AUDIO_BLOCK_SAMPLES            256
#define CELL_AUDIO_BLOCK_8                  8
#define CELL_AUDIO_BLOCK_16                 16
#define CELL_AUDIO_BLOCK_32                 32

/* Channel count */
#define CELL_AUDIO_PORT_2CH                 2
#define CELL_AUDIO_PORT_8CH                 8

/* Sample rate (always 48 kHz on PS3) */
#define CELL_AUDIO_SAMPLE_RATE              48000

/* ---------------------------------------------------------------------------
 * Structures
 * -----------------------------------------------------------------------*/

/* Parameters used to open an audio port */
typedef struct CellAudioPortParam {
    u64 nChannel;       /* CELL_AUDIO_PORT_2CH or CELL_AUDIO_PORT_8CH */
    u64 nBlock;         /* number of blocks (8, 16, or 32) */
    u64 attr;           /* attribute flags */
    float level;        /* volume level (0.0 .. 1.0) */
} CellAudioPortParam;

/* Returned after opening a port -- describes the allocated resources */
typedef struct CellAudioPortConfig {
    u64 readIndexAddr;      /* guest address of the read index counter */
    u32 status;             /* port status */
    u64 nChannel;           /* channels */
    u64 nBlock;             /* blocks */
    u32 portSize;           /* total port buffer size in bytes */
    u64 portAddr;           /* guest address of the port audio buffer */
} CellAudioPortConfig;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

/* NID: 0x0B168F92 */
s32 cellAudioInit(void);

/* NID: 0x4129FE2D */
s32 cellAudioQuit(void);

/* NID: 0xCD7BC431 */
s32 cellAudioPortOpen(const CellAudioPortParam* param, u32* portNum);

/* NID: 0x56DFFE09 */
s32 cellAudioPortClose(u32 portNum);

/* NID: 0x04AF134E */
s32 cellAudioPortStart(u32 portNum);

/* NID: 0x05DEAB16 */
s32 cellAudioPortStop(u32 portNum);

/* NID: 0x74A66AF0 */
s32 cellAudioSetNotifyEventQueue(u64 key);

/* NID: 0x4109D08C */
s32 cellAudioGetPortConfig(u32 portNum, CellAudioPortConfig* config);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_AUDIO_H */
