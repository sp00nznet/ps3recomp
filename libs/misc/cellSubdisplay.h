/*
 * ps3recomp - cellSubdisplay HLE
 *
 * Sub-display output for PS Vita Remote Play and similar second-screen features.
 * Stub — init/end lifecycle, video/touch data exchange all succeed as no-ops.
 */

#ifndef PS3RECOMP_CELL_SUBDISPLAY_H
#define PS3RECOMP_CELL_SUBDISPLAY_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define CELL_SUBDISPLAY_ERROR_NOT_INITIALIZED     0x80029501
#define CELL_SUBDISPLAY_ERROR_ALREADY_INITIALIZED 0x80029502
#define CELL_SUBDISPLAY_ERROR_INVALID_ARGUMENT    0x80029503
#define CELL_SUBDISPLAY_ERROR_OUT_OF_MEMORY       0x80029504
#define CELL_SUBDISPLAY_ERROR_NOT_SUPPORTED       0x80029505
#define CELL_SUBDISPLAY_ERROR_NOT_CONNECTED       0x80029506

/* Video mode */
#define CELL_SUBDISPLAY_VIDEO_MODE_DEFAULT   0
#define CELL_SUBDISPLAY_VIDEO_MODE_480P      1
#define CELL_SUBDISPLAY_VIDEO_MODE_272P      2

/* Config */
typedef struct CellSubdisplayConfig {
    u32 videoMode;
    u32 width;
    u32 height;
    u32 fps;
    u32 flags;
    u32 reserved[4];
} CellSubdisplayConfig;

/* Touch data from sub-display */
typedef struct CellSubdisplayTouchData {
    u32 count;
    struct {
        u16 x;
        u16 y;
        u16 pressure;
        u16 reserved;
    } points[4];
} CellSubdisplayTouchData;

/* Functions */
s32 cellSubdisplayInit(const CellSubdisplayConfig* config);
s32 cellSubdisplayEnd(void);
s32 cellSubdisplayStart(void);
s32 cellSubdisplayStop(void);
s32 cellSubdisplayGetRequiredMemory(u32* size);
s32 cellSubdisplayGetTouchData(CellSubdisplayTouchData* data);
s32 cellSubdisplayIsConnected(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_SUBDISPLAY_H */
