/*
 * ps3recomp - cellAvconfExt HLE
 *
 * Extended audio/video configuration. Queries display capabilities,
 * HDMI info, and audio output parameters.
 */

#ifndef PS3RECOMP_CELL_AVCONF_EXT_H
#define PS3RECOMP_CELL_AVCONF_EXT_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Audio output
 * -----------------------------------------------------------------------*/
#define CELL_AUDIO_OUT_PRIMARY             0
#define CELL_AUDIO_OUT_SECONDARY           1

/* Output modes */
#define CELL_AUDIO_OUT_CODING_TYPE_LPCM    0
#define CELL_AUDIO_OUT_CODING_TYPE_AC3     4
#define CELL_AUDIO_OUT_CODING_TYPE_DTS     8

/* Channel counts */
#define CELL_AUDIO_OUT_CHNUM_2             2
#define CELL_AUDIO_OUT_CHNUM_6             6
#define CELL_AUDIO_OUT_CHNUM_8             8

/* Sampling rates */
#define CELL_AUDIO_OUT_FS_32KHZ            0x01
#define CELL_AUDIO_OUT_FS_44KHZ            0x02
#define CELL_AUDIO_OUT_FS_48KHZ            0x04
#define CELL_AUDIO_OUT_FS_88KHZ            0x08
#define CELL_AUDIO_OUT_FS_96KHZ            0x10
#define CELL_AUDIO_OUT_FS_176KHZ           0x20
#define CELL_AUDIO_OUT_FS_192KHZ           0x40

/* ---------------------------------------------------------------------------
 * Types
 * -----------------------------------------------------------------------*/

typedef struct CellAudioOutSoundMode {
    u8  type;
    u8  channel;
    u8  fs;
    u8  reserved;
} CellAudioOutSoundMode;

typedef struct CellAudioOutDeviceInfo {
    u8  portType;
    u8  availableModeCount;
    u8  state;
    u8  reserved[3];
    CellAudioOutSoundMode availableModes[16];
} CellAudioOutDeviceInfo;

typedef struct CellAudioOutConfiguration {
    u8  channel;
    u8  encoder;
    u8  reserved[10];
    u32 downMixer;
} CellAudioOutConfiguration;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 cellAudioOutGetSoundAvailability(u32 audioOut, u32 type, u32 fs, u32 option);
s32 cellAudioOutGetSoundAvailability2(u32 audioOut, u32 type, u32 fs, u32 ch, u32 option);

s32 cellAudioOutGetDeviceInfo(u32 audioOut, u32 deviceIndex,
                               CellAudioOutDeviceInfo* info);
s32 cellAudioOutGetConfiguration(u32 audioOut,
                                  CellAudioOutConfiguration* config,
                                  void* option, u32 optionSize);
s32 cellAudioOutSetCopyControl(u32 audioOut, u32 control);

/* HDMI/display audio */
s32 cellAudioOutGetNumberOfDevice(u32 audioOut);

/* Video output extended queries */
s32 cellVideoOutGetGamma(u32 videoOut, float* gamma);
s32 cellVideoOutSetGamma(u32 videoOut, float gamma);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_AVCONF_EXT_H */
