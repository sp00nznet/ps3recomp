/*
 * ps3recomp - cellVideoOut HLE
 *
 * Video output configuration: resolution, display mode, device info.
 */

#ifndef PS3RECOMP_CELL_VIDEOOUT_H
#define PS3RECOMP_CELL_VIDEOOUT_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/
#define CELL_VIDEO_OUT_PRIMARY      0
#define CELL_VIDEO_OUT_SECONDARY    1

/* Resolution IDs */
#define CELL_VIDEO_OUT_RESOLUTION_UNDEFINED  0
#define CELL_VIDEO_OUT_RESOLUTION_1080       1
#define CELL_VIDEO_OUT_RESOLUTION_720        2
#define CELL_VIDEO_OUT_RESOLUTION_480        4
#define CELL_VIDEO_OUT_RESOLUTION_576        5
#define CELL_VIDEO_OUT_RESOLUTION_1600x1080  10
#define CELL_VIDEO_OUT_RESOLUTION_1440x1080  11
#define CELL_VIDEO_OUT_RESOLUTION_1280x1080  12
#define CELL_VIDEO_OUT_RESOLUTION_960x1080   13

/* Scan mode */
#define CELL_VIDEO_OUT_SCAN_MODE_INTERLACE   0
#define CELL_VIDEO_OUT_SCAN_MODE_PROGRESSIVE 1

/* Aspect ratio */
#define CELL_VIDEO_OUT_ASPECT_AUTO   0
#define CELL_VIDEO_OUT_ASPECT_4_3    1
#define CELL_VIDEO_OUT_ASPECT_16_9   2

/* Output type (PS3 CELL_VIDEO_OUT_PORT_HDMI = 1; verified against RPCS3) */
#define CELL_VIDEO_OUT_OUTPUT_HDMI   1

/* Device state (CellVideoOutDeviceInfo.state) */
#define CELL_VIDEO_OUT_DEVICE_STATE_UNAVAILABLE 0
#define CELL_VIDEO_OUT_DEVICE_STATE_AVAILABLE   1

/* Refresh rate bitmask (CellVideoOutDisplayMode.refreshRates) */
#define CELL_VIDEO_OUT_REFRESH_RATE_59_94HZ  0x0001
#define CELL_VIDEO_OUT_REFRESH_RATE_50HZ     0x0004
#define CELL_VIDEO_OUT_REFRESH_RATE_60HZ     0x0008

/* Color space */
#define CELL_VIDEO_OUT_COLOR_SPACE_RGB   0x01
#define CELL_VIDEO_OUT_COLOR_SPACE_YUV   0x02

/* Buffer color format */
#define CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_X8R8G8B8  1
#define CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_X8B8G8R8  2
#define CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_R16G16B16X16_FLOAT  10

/* Display mode */
#define CELL_VIDEO_OUT_DISPLAY_MODE_720_480_59_94HZ    0x00000001
#define CELL_VIDEO_OUT_DISPLAY_MODE_720_576_50HZ       0x00000002
#define CELL_VIDEO_OUT_DISPLAY_MODE_1280_720_59_94HZ   0x00000004
#define CELL_VIDEO_OUT_DISPLAY_MODE_1920_1080_59_94HZ  0x00000008
#define CELL_VIDEO_OUT_DISPLAY_MODE_1280_720_50HZ      0x00000040
#define CELL_VIDEO_OUT_DISPLAY_MODE_1920_1080_50HZ     0x00000080

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
#define CELL_VIDEO_OUT_ERROR_NOT_IMPLEMENTED  (s32)(CELL_ERROR_BASE_VIDEO | 0x01)
#define CELL_VIDEO_OUT_ERROR_ILLEGAL_CONFIGURATION (s32)(CELL_ERROR_BASE_VIDEO | 0x02)
#define CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER (s32)(CELL_ERROR_BASE_VIDEO | 0x03)
#define CELL_VIDEO_OUT_ERROR_PARAMETER_OUT_OF_RANGE (s32)(CELL_ERROR_BASE_VIDEO | 0x04)
#define CELL_VIDEO_OUT_ERROR_DEVICE_NOT_FOUND (s32)(CELL_ERROR_BASE_VIDEO | 0x05)
#define CELL_VIDEO_OUT_ERROR_UNSUPPORTED_VIDEO_OUT (s32)(CELL_ERROR_BASE_VIDEO | 0x06)
#define CELL_VIDEO_OUT_ERROR_UNSUPPORTED_DISPLAY_MODE (s32)(CELL_ERROR_BASE_VIDEO | 0x07)
#define CELL_VIDEO_OUT_ERROR_CONDITION_BUSY (s32)(CELL_ERROR_BASE_VIDEO | 0x08)

/* ---------------------------------------------------------------------------
 * Structures
 * -----------------------------------------------------------------------*/

typedef struct CellVideoOutResolution {
    u16 width;
    u16 height;
} CellVideoOutResolution;

typedef struct CellVideoOutDisplayModeBytes {
    u8  resolutionId;
    u8  scanMode;
    u8  conversion;
    u8  aspect;
    u8  reserved[2];
    u16 refreshRates;   /* big-endian in guest memory */
} CellVideoOutDisplayModeBytes;

typedef struct CellVideoOutState {
    u8  state;          /* 0=disabled, 2=enabled */
    u8  colorSpace;
    u8  reserved[6];
    /* SDK layout: an 8-byte CellVideoOutDisplayMode struct, NOT a u32 mode id.
     * Guests read displayMode.aspect (byte 11) to pick 4:3 vs 16:9; the old
     * u32 fed them a byte of a mode constant ("unknown aspect ratio 4"). */
    CellVideoOutDisplayModeBytes displayMode;
} CellVideoOutState;

typedef struct CellVideoOutConfiguration {
    u8  resolutionId;
    u8  format;
    u8  aspect;
    u8  reserved[9];
    u32 pitch;
} CellVideoOutConfiguration;

typedef struct CellVideoOutDisplayMode {
    u8  resolutionId;
    u8  scanMode;
    u8  conversion;
    u8  aspect;
    u8  reserved[2];
    u16 refreshRates;
} CellVideoOutDisplayMode;

typedef struct CellVideoOutColorInfo {
    u16 redX;
    u16 redY;
    u16 greenX;
    u16 greenY;
    u16 blueX;
    u16 blueY;
    u16 whiteX;
    u16 whiteY;
    u32 gamma;
} CellVideoOutColorInfo; /* 20 bytes, all fields big-endian in guest memory */

typedef struct CellVideoOutDeviceInfo {
    u8  portType;
    u8  colorSpace;
    u16 latency;                /* big-endian in guest memory */
    u8  availableModeCount;
    u8  state;
    u8  rgbOutputRange;
    u8  reserved[5];
    CellVideoOutColorInfo   colorInfo;           /* +0x0C..+0x1F (20 bytes) */
    CellVideoOutDisplayMode availableModes[32];  /* +0x20..+0x11F (256 bytes) */
} CellVideoOutDeviceInfo; /* 288 bytes (0x120) */

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/

/* Set the default resolution (call before game boots) */
void cellVideoOut_set_resolution(u8 resolutionId);

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 cellVideoOutGetState(u32 videoOut, u32 deviceIndex, CellVideoOutState* state);

s32 cellVideoOutGetResolution(u32 resolutionId, CellVideoOutResolution* resolution);

s32 cellVideoOutConfigure(u32 videoOut, CellVideoOutConfiguration* config,
                            void* option, u32 waitForEvent);

s32 cellVideoOutGetConfiguration(u32 videoOut, CellVideoOutConfiguration* config,
                                   void* option);

s32 cellVideoOutGetDeviceInfo(u32 videoOut, u32 deviceIndex,
                               CellVideoOutDeviceInfo* info);

s32 cellVideoOutGetNumberOfDevice(u32 videoOut);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_VIDEOOUT_H */
