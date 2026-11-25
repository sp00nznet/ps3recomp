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

/* Error codes (PS3 sysutil subdisplay, base 0x80029800; verified against RPCS3) */
#define CELL_SUBDISPLAY_ERROR_OUT_OF_MEMORY       (CELL_ERROR_BASE_SYSUTIL_SUBDISPLAY | 0x51) /* 0x80029851 */
#define CELL_SUBDISPLAY_ERROR_INVALID_ARGUMENT    (CELL_ERROR_BASE_SYSUTIL_SUBDISPLAY | 0x52) /* 0x80029852 */
#define CELL_SUBDISPLAY_ERROR_NOT_SUPPORTED       (CELL_ERROR_BASE_SYSUTIL_SUBDISPLAY | 0x53) /* 0x80029853 */
#define CELL_SUBDISPLAY_ERROR_ALREADY_INITIALIZED (CELL_ERROR_BASE_SYSUTIL_SUBDISPLAY | 0x54) /* 0x80029854 */
#define CELL_SUBDISPLAY_ERROR_NOT_INITIALIZED     (CELL_ERROR_BASE_SYSUTIL_SUBDISPLAY | 0x55) /* 0x80029855 */
#define CELL_SUBDISPLAY_ERROR_NOT_CONNECTED       (CELL_ERROR_BASE_SYSUTIL_SUBDISPLAY | 0x56) /* 0x80029856 */

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
