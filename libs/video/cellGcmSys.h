/*
 * ps3recomp - cellGcmSys HLE stub
 *
 * RSX (Reality Synthesizer) graphics system interface.
 * Manages initialization, display buffers, flip control, and
 * address-to-offset translation for the GPU.
 *
 * Note: Full RSX command buffer processing (the NV47xx methods, fragment/
 * vertex programs, texture setup, etc.) is a separate massive undertaking
 * handled by the RSX command processor module, not this file.
 */

#ifndef PS3RECOMP_CELL_GCM_SYS_H
#define PS3RECOMP_CELL_GCM_SYS_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

/* Flip modes */
#define CELL_GCM_DISPLAY_HSYNC          1
#define CELL_GCM_DISPLAY_VSYNC          2

/* Flip status */
#define CELL_GCM_FLIP_STATUS_DONE       0
#define CELL_GCM_FLIP_STATUS_WAITING    1

/* Surface color format */
#define CELL_GCM_SURFACE_A8R8G8B8       8
#define CELL_GCM_SURFACE_X8R8G8B8       5

/* Max display buffers */
#define CELL_GCM_MAX_DISPLAY_BUFFER_NUM 8

/* Location */
#define CELL_GCM_LOCATION_LOCAL         0  /* video memory */
#define CELL_GCM_LOCATION_MAIN          1  /* main memory */

/* ---------------------------------------------------------------------------
 * Structures
 * -----------------------------------------------------------------------*/

/* Display buffer configuration */
typedef struct CellGcmDisplayInfo {
    u32 offset;         /* offset in local memory */
    u32 pitch;          /* pitch in bytes */
    u32 width;
    u32 height;
} CellGcmDisplayInfo;

/* Offset table for address translation */
typedef struct CellGcmOffsetTable {
    u16* ioAddress;     /* main memory -> IO offset mapping */
    u16* eaAddress;     /* IO offset -> main memory mapping */
} CellGcmOffsetTable;

/* GCM configuration returned by cellGcmInit */
typedef struct CellGcmConfig {
    u32 localAddress;       /* start of local (video) memory in guest space */
    u32 ioAddress;          /* start of IO-mapped main memory */
    u32 localSize;          /* size of local memory */
    u32 ioSize;             /* size of IO-mapped region */
    u32 memoryFrequency;    /* RSX memory clock */
    u32 coreFrequency;      /* RSX core clock */
} CellGcmConfig;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

/* NID: 0xB2E761D4 */
s32 cellGcmInit(u32 cmdSize, u32 ioSize, u32 ioAddress);

/* NID: 0xDB23E867 */
u32 cellGcmGetCurrentField(void);

/* NID: 0xA53D12AE */
void cellGcmSetFlipMode(u32 mode);

/* NID: 0xC44D8F34 */
void cellGcmSetWaitFlip(void);

/* NID: 0x51C9D62B */
void cellGcmResetFlipStatus(void);

/* NID: 0xDC09357E */
s32 cellGcmSetDisplayBuffer(u32 bufferId, u32 offset, u32 pitch,
                            u32 width, u32 height);

/* NID: 0xE315A0B2 */
u32 cellGcmGetFlipStatus(void);

/* NID: 0x0E6B0DFF */
s32 cellGcmGetOffsetTable(CellGcmOffsetTable* table);

/* NID: 0xDB769B32 */
s32 cellGcmAddressToOffset(u32 address, u32* offset);

/* NID: 0x15BAE46B */
s32 cellGcmGetConfiguration(CellGcmConfig* config);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_GCM_SYS_H */
