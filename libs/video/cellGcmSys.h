/*
 * ps3recomp - cellGcmSys HLE module
 *
 * RSX (Reality Synthesizer) graphics system interface.
 * Manages initialization, display buffers, flip control, command buffer
 * control, tile/zcull configuration, IO memory mapping, report/label
 * areas, and address-to-offset translation for the GPU.
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

/* Surface color formats */
#define CELL_GCM_SURFACE_X1R5G5B5_Z1R5G5B5   1
#define CELL_GCM_SURFACE_X1R5G5B5_O1R5G5B5   2
#define CELL_GCM_SURFACE_R5G6B5               3
#define CELL_GCM_SURFACE_X8R8G8B8_Z8R8G8B8   4
#define CELL_GCM_SURFACE_X8R8G8B8             5
#define CELL_GCM_SURFACE_A8R8G8B8             8
#define CELL_GCM_SURFACE_B8                   9
#define CELL_GCM_SURFACE_G8B8                 10
#define CELL_GCM_SURFACE_F_W16Z16Y16X16       11
#define CELL_GCM_SURFACE_F_W32Z32Y32X32       12
#define CELL_GCM_SURFACE_F_X32                13
#define CELL_GCM_SURFACE_X8B8G8R8_Z8B8G8R8   14
#define CELL_GCM_SURFACE_X8B8G8R8_O8B8G8R8   15
#define CELL_GCM_SURFACE_A8B8G8R8             16

/* Depth buffer formats */
#define CELL_GCM_SURFACE_Z16                  1
#define CELL_GCM_SURFACE_Z24S8                2

/* Max display buffers */
#define CELL_GCM_MAX_DISPLAY_BUFFER_NUM 8

/* Tile / Zcull limits */
#define CELL_GCM_MAX_TILE_COUNT         15
#define CELL_GCM_MAX_ZCULL_COUNT        8

/* Report / label limits */
#define CELL_GCM_MAX_REPORT_COUNT       256
#define CELL_GCM_MAX_LABEL_COUNT        256

/* Report data size (timestamp u64 + value u32 + pad u32 = 16 bytes) */
#define CELL_GCM_REPORT_DATA_SIZE       16

/* Location */
#define CELL_GCM_LOCATION_LOCAL         0  /* video memory */
#define CELL_GCM_LOCATION_MAIN          1  /* main memory */

/* IO mapping limits */
#define CELL_GCM_MAX_IO_MAPPINGS        16

/* Debug output levels */
#define CELL_GCM_DEBUG_LEVEL0           0
#define CELL_GCM_DEBUG_LEVEL1           1
#define CELL_GCM_DEBUG_LEVEL2           2

/* ---------------------------------------------------------------------------
 * Structures
 * -----------------------------------------------------------------------*/

/* Command buffer control registers (mapped to RSX FIFO control area) */
typedef struct CellGcmControl {
    volatile u32 put;       /* write pointer (offset into cmd buffer) */
    volatile u32 get;       /* read pointer (offset into cmd buffer) */
    volatile u32 ref;       /* reference value (written by RSX on completion) */
} CellGcmControl;

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

/* Tile configuration */
typedef struct CellGcmTileInfo {
    u32 tile;           /* tile register value */
    u32 limit;          /* tile limit address */
    u32 pitch;          /* pitch in bytes */
    u32 format;         /* compression format */
    u32 offset;         /* base offset in local memory */
    u32 size;           /* tile region size in bytes */
    u32 base;           /* base address for compression */
    u32 bank;           /* memory bank */
    u8  bound;          /* 1 if bound to RSX, 0 if unbound */
} CellGcmTileInfo;

/* Zcull configuration */
typedef struct CellGcmZcullInfo {
    u32 offset;         /* offset in local memory */
    u32 width;          /* width in pixels */
    u32 height;         /* height in pixels */
    u32 cullStart;      /* zcull start address */
    u32 zFormat;        /* depth format */
    u32 aaFormat;       /* anti-alias format */
    u32 zcullDir;       /* direction */
    u32 zcullFormat;    /* zcull format register */
    u32 sFunc;          /* stencil function */
    u32 sRef;           /* stencil reference */
    u32 sMask;          /* stencil mask */
    u8  bound;          /* 1 if bound to RSX, 0 if unbound */
} CellGcmZcullInfo;

/* Report data (16 bytes, matching RSX report format) */
typedef struct CellGcmReportData {
    u64 timestamp;      /* RSX timestamp */
    u32 value;          /* report value */
    u32 pad;            /* padding to 16 bytes */
} CellGcmReportData;

/* Notify data (same layout as report) */
typedef struct CellGcmNotifyData {
    u64 timestamp;
    u64 zero;           /* always 0 on notification */
} CellGcmNotifyData;

/* Callback types */
typedef void (*CellGcmFlipHandler)(u32 head);
typedef void (*CellGcmVBlankHandler)(u32 head);
typedef void (*CellGcmUserHandler)(u32 cause);
typedef void (*CellGcmSecondVHandler)(u32 head);
typedef s32  (*CellGcmContextCallback)(void* context, u32 count);
typedef void (*CellGcmGraphicsHandler)(u32 val);
typedef void (*CellGcmQueueHandler)(u32 head);

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

/* Initialization */

/* NID: 0xB2E761D4 */
s32 cellGcmInit(u32 cmdSize, u32 ioSize, u32 ioAddress);

/* NID: 0x15BAE46B */
s32 cellGcmGetConfiguration(CellGcmConfig* config);

/* Command buffer control */

/* NID: 0x8572A8E0 */
CellGcmControl* cellGcmGetControlRegister(void);

/* Display / flip */

/* NID: 0xDB23E867 */
u32 cellGcmGetCurrentField(void);

/* NID: 0xA53D12AE */
void cellGcmSetFlipMode(u32 mode);

/* NID: 0xC44D8F34 */
void cellGcmSetWaitFlip(void);

/* NID: 0x51C9D62B */
void cellGcmResetFlipStatus(void);

/* NID: 0xE315A0B2 */
u32 cellGcmGetFlipStatus(void);

/* NID: 0xDC09357E */
s32 cellGcmSetDisplayBuffer(u32 bufferId, u32 offset, u32 pitch,
                            u32 width, u32 height);

/* NID: 0xEAA52F23 */
s32 cellGcmSetFlipCommand(u32 bufferId);

/* NID: 0xD01B570F */
s32 cellGcmSetFlipCommandWithWaitLabel(u32 bufferId, u32 labelIndex, u32 labelValue);

/* NID: 0xA2478CA3 */
s32 cellGcmSetPrepareFlip(void* ctx, u32 bufferId);

/* NID: 0x1BFAB6EE */
CellGcmDisplayInfo* cellGcmGetDisplayBufferByFlipIndex(u32 index);

/* NID: 0x8BADE8BE */
u32 cellGcmGetCurrentDisplayBufferId(void);

/* NID: 0xD9B7653E */
void cellGcmSetFlipHandler(CellGcmFlipHandler handler);

/* NID: 0xA547ADDE */
void cellGcmSetVBlankHandler(CellGcmVBlankHandler handler);

/* NID: 0xF9BFCDA3 */
void cellGcmSetSecondVHandler(CellGcmSecondVHandler handler);

/* NID: 0x0B4B62D5 */
void cellGcmSetUserHandler(CellGcmUserHandler handler);

/* NID: 0x21AC3697 */
u64 cellGcmGetLastFlipTime(void);

/* Address translation / IO mapping */

/* NID: 0x0E6B0DFF */
s32 cellGcmGetOffsetTable(CellGcmOffsetTable* table);

/* NID: 0xDB769B32 */
s32 cellGcmAddressToOffset(u32 address, u32* offset);

/* NID: 0x2A6FBA9C */
s32 cellGcmMapMainMemory(u32 ea, u32 size, u32* offset);

/* NID: 0x5A41C10F */
s32 cellGcmMapEaIoAddress(u32 ea, u32 io, u32 size);

/* NID: 0xDB23E867 (disambiguation by context) */
s32 cellGcmUnmapEaIoAddress(u32 ea);

/* NID: 0x3B9BD5BD */
s32 cellGcmUnmapIoAddress(u32 io);

/* NID: 0xC47D0812 */
s32 cellGcmIoOffsetToAddress(u32 ioOffset, u32* ea);

/* Label / report / timestamp */

/* NID: 0x21397818 */
u32* cellGcmGetLabelAddress(u8 index);

/* NID: 0x8572ADE4 */
CellGcmReportData* cellGcmGetReportDataAddress(u32 index);

/* NID: 0x97FC4B73 */
u64 cellGcmGetTimeStamp(u32 index);

/* Tile / Zcull */

/* NID: 0x0B4B62D5 */
s32 cellGcmSetTile(u8 index, u8 location, u32 offset, u32 size,
                   u32 pitch, u8 comp, u16 base, u8 bank);

/* NID: 0x06EDEA25 */
s32 cellGcmSetZcull(u8 index, u32 offset, u32 width, u32 height,
                    u32 cullStart, u32 zFormat, u32 aaFormat,
                    u32 zcullDir, u32 zcullFormat,
                    u32 sFunc, u32 sRef, u32 sMask);

/* NID: 0x2BB1F1E5 */
s32 cellGcmBindTile(u8 index);

/* NID: 0x1F61F9D3 */
s32 cellGcmBindZcull(u8 index);

/* NID: 0x75843042 */
s32 cellGcmUnbindTile(u8 index);

/* NID: 0xA093BE1C */
s32 cellGcmUnbindZcull(u8 index);

/* Misc */

/* NID: 0x107BF789 */
s32 cellGcmGetTiledPitchSize(u32 size, u32* pitch);

/* NID: 0xBC982946 */
void cellGcmSetDebugOutputLevel(u32 level);

/* --- Additional functions needed by Tokyo Jungle --- */

/* Tile info (alternative to cellGcmSetTile) */
s32 cellGcmSetTileInfo(u8 index, u8 location, u32 offset, u32 size,
                       u32 pitch, u8 comp, u16 base, u8 bank);

/* Notify data area (similar to report data) */
void* cellGcmGetNotifyDataAddress(u32 index);

/* Timestamp location query */
u32 cellGcmGetTimeStampLocation(u32 index, u32* location);

/* Default FIFO size configuration */
s32 cellGcmSetDefaultFifoSize(u32 size);

/* Internal flip commands (called by game code directly) */
s32 _cellGcmSetFlipCommand(u32 bufferId);
s32 _cellGcmSetFlipCommandWithWaitLabel(u32 bufferId, u32 labelIndex, u32 labelValue);

/* --- Additional functions (RPCS3 parity) --- */

/* FIFO command buffer callback */
s32 cellGcmCallback(void* context, u32 count);

/* Map RSX local memory */
s32 cellGcmMapLocalMemory(u32* address, u32* size);

/* Shutdown RSX */
void cellGcmTerminate(void);

/* IO map size queries */
u32 cellGcmGetMaxIoMapSize(void);
s32 cellGcmReserveIoMapSize(u32 size);
s32 cellGcmUnreserveIoMapSize(u32 size);

/* VBlank counter */
u32 cellGcmGetVBlankCount(void);

/* Tile / Zcull info getters */
CellGcmTileInfo* cellGcmGetTileInfo(u8 index);
CellGcmZcullInfo* cellGcmGetZcullInfo(u8 index);

/* Display info by index */
CellGcmDisplayInfo* cellGcmGetDisplayInfo(u32 index);

/* Default FIFO mode / command buffer */
void cellGcmInitDefaultFifoMode(s32 mode);
void cellGcmSetDefaultCommandBuffer(void);

/* Debug dump (no-op) */
void cellGcmDumpGraphicsError(void);

/* Default word sizes */
u32 cellGcmGetDefaultCommandWordSize(void);
u32 cellGcmGetDefaultSegmentWordSize(void);

/* Immediate flip */
s32 cellGcmSetFlipImmediate(u32 bufferId);

/* Flip status setter */
void cellGcmSetFlipStatus(u32 status);

/* Graphics / queue handler setters */
void cellGcmSetGraphicsHandler(CellGcmGraphicsHandler handler);
void cellGcmSetQueueHandler(CellGcmQueueHandler handler);

/* Frequency / VBlank configuration */
void cellGcmSetSecondVFrequency(u32 freq);
void cellGcmSetVBlankFrequency(u32 freq);

/* User command */
void cellGcmSetUserCommand(u32 cmd);

/* Invalidate tile */
s32 cellGcmSetInvalidateTile(u8 index);

/* EA IO address remap (stub) */
void cellGcmSortRemapEaIoAddress(void);

/* Report data with location */
CellGcmReportData* cellGcmGetReportDataAddressLocation(u32 index, u32 location);
u32 cellGcmGetReportDataLocation(u32 index, u32 location);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_GCM_SYS_H */
