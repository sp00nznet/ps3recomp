/*
 * ps3recomp - cellGcmSys HLE stub implementation
 *
 * Manages RSX initialization, display buffer registration, and flip control.
 * Actual rendering is handled elsewhere -- this module just tracks state.
 */

#include "cellGcmSys.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int  s_gcm_initialized = 0;
static u32  s_flip_mode   = CELL_GCM_DISPLAY_VSYNC;
static u32  s_flip_status = CELL_GCM_FLIP_STATUS_DONE;

static CellGcmDisplayInfo s_display_buffers[CELL_GCM_MAX_DISPLAY_BUFFER_NUM];
static int s_display_buffer_set[CELL_GCM_MAX_DISPLAY_BUFFER_NUM];

static CellGcmConfig s_config;

/* Placeholder offset table storage */
static u16 s_io_address_table[65536];
static u16 s_ea_address_table[65536];

static CellGcmOffsetTable s_offset_table = {
    .ioAddress = s_io_address_table,
    .eaAddress = s_ea_address_table,
};

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

/* NID: 0xB2E761D4 */
s32 cellGcmInit(u32 cmdSize, u32 ioSize, u32 ioAddress)
{
    printf("[cellGcmSys] Init(cmdSize=0x%X, ioSize=0x%X, ioAddr=0x%08X)\n",
           cmdSize, ioSize, ioAddress);

    if (s_gcm_initialized) {
        printf("[cellGcmSys] WARNING: already initialized\n");
        return CELL_GCM_ERROR_FAILURE;
    }

    memset(s_display_buffers, 0, sizeof(s_display_buffers));
    memset(s_display_buffer_set, 0, sizeof(s_display_buffer_set));
    memset(&s_config, 0, sizeof(s_config));

    /*
     * Populate a plausible configuration.
     * In a real implementation these would reflect actual allocated memory.
     */
    s_config.localAddress    = 0xC0000000;  /* typical RSX local mem base */
    s_config.localSize       = 256 * 1024 * 1024;  /* 256 MB */
    s_config.ioAddress       = ioAddress;
    s_config.ioSize          = ioSize;
    s_config.memoryFrequency = 650000000;   /* 650 MHz */
    s_config.coreFrequency   = 500000000;   /* 500 MHz */

    s_gcm_initialized = 1;
    return CELL_OK;
}

/* NID: 0xDB23E867 */
u32 cellGcmGetCurrentField(void)
{
    /* Returns 0 or 1 for interlaced; always 0 for progressive. */
    return 0;
}

/* NID: 0xA53D12AE */
void cellGcmSetFlipMode(u32 mode)
{
    printf("[cellGcmSys] SetFlipMode(mode=%u)\n", mode);
    s_flip_mode = mode;
}

/* NID: 0xC44D8F34 */
void cellGcmSetWaitFlip(void)
{
    /*
     * Block until the current flip completes.  In the stub we consider
     * flips to be instantaneous.
     */
    s_flip_status = CELL_GCM_FLIP_STATUS_DONE;
}

/* NID: 0x51C9D62B */
void cellGcmResetFlipStatus(void)
{
    s_flip_status = CELL_GCM_FLIP_STATUS_WAITING;
}

/* NID: 0xDC09357E */
s32 cellGcmSetDisplayBuffer(u32 bufferId, u32 offset, u32 pitch,
                            u32 width, u32 height)
{
    printf("[cellGcmSys] SetDisplayBuffer(id=%u, offset=0x%X, pitch=%u, %ux%u)\n",
           bufferId, offset, pitch, width, height);

    if (bufferId >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM)
        return CELL_GCM_ERROR_INVALID_VALUE;

    s_display_buffers[bufferId].offset = offset;
    s_display_buffers[bufferId].pitch  = pitch;
    s_display_buffers[bufferId].width  = width;
    s_display_buffers[bufferId].height = height;
    s_display_buffer_set[bufferId] = 1;

    return CELL_OK;
}

/* NID: 0xE315A0B2 */
u32 cellGcmGetFlipStatus(void)
{
    return s_flip_status;
}

/* NID: 0x0E6B0DFF */
s32 cellGcmGetOffsetTable(CellGcmOffsetTable* table)
{
    if (!table)
        return CELL_GCM_ERROR_INVALID_VALUE;

    *table = s_offset_table;
    return CELL_OK;
}

/* NID: 0xDB769B32 */
s32 cellGcmAddressToOffset(u32 address, u32* offset)
{
    if (!offset)
        return CELL_GCM_ERROR_INVALID_VALUE;

    /*
     * On real hardware this translates a guest effective address to an
     * RSX-visible offset.  For local memory, offset = address - localBase.
     * For IO-mapped main memory, the offset table is consulted.
     *
     * Simplified: assume local memory.
     */
    if (address >= s_config.localAddress &&
        address < s_config.localAddress + s_config.localSize) {
        *offset = address - s_config.localAddress;
        return CELL_OK;
    }

    /* IO-mapped main memory -- use offset table (simplified) */
    if (s_config.ioAddress != 0 && address >= s_config.ioAddress &&
        address < s_config.ioAddress + s_config.ioSize) {
        *offset = address - s_config.ioAddress;
        return CELL_OK;
    }

    printf("[cellGcmSys] WARNING: AddressToOffset failed for 0x%08X\n", address);
    *offset = 0;
    return CELL_GCM_ERROR_FAILURE;
}

/* NID: 0x15BAE46B */
s32 cellGcmGetConfiguration(CellGcmConfig* config)
{
    if (!config)
        return CELL_GCM_ERROR_INVALID_VALUE;

    *config = s_config;
    return CELL_OK;
}
