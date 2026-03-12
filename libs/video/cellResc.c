/*
 * ps3recomp - cellResc HLE implementation
 *
 * Resolution scaling stub. Since the recompiled game renders through
 * the host GPU, actual rescaling is handled by the host graphics API.
 * This module tracks state so game code that queries RESC works correctly.
 */

#include "cellResc.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int s_initialized = 0;
static CellRescInitConfig s_config;
static u32 s_display_mode = CELL_RESC_1280x720;
static CellRescSrc s_src[8]; /* up to 8 color buffers */
static CellRescDsts s_dsts[4]; /* one per display mode */
static void (*s_flip_handler)(u32) = NULL;
static void (*s_vblank_handler)(u32) = NULL;
static s32 s_flip_status = 0;
static u64 s_last_flip_time = 0;
static float s_aspect_h = 1.0f;
static float s_aspect_v = 1.0f;
static float s_pal_ratio = 0.5f;

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellRescInit(const CellRescInitConfig* initConfig)
{
    printf("[cellResc] Init()\n");

    if (s_initialized)
        return (s32)CELL_RESC_ERROR_REINITIALIZED;

    if (!initConfig)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    s_config = *initConfig;
    memset(s_src, 0, sizeof(s_src));
    memset(s_dsts, 0, sizeof(s_dsts));
    s_flip_handler = NULL;
    s_vblank_handler = NULL;
    s_flip_status = 0;
    s_last_flip_time = 0;
    s_aspect_h = 1.0f;
    s_aspect_v = 1.0f;
    s_initialized = 1;
    return CELL_OK;
}

void cellRescExit(void)
{
    printf("[cellResc] Exit()\n");
    s_initialized = 0;
}

s32 cellRescSetDisplayMode(u32 displayMode)
{
    printf("[cellResc] SetDisplayMode(0x%x)\n", displayMode);

    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    s_display_mode = displayMode;
    return CELL_OK;
}

s32 cellRescGetNumColorBuffers(u32 displayMode, u32 palTemporalMode, u32* numBufs)
{
    (void)displayMode;

    if (!numBufs)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    /* PAL interpolation modes need extra buffers */
    switch (palTemporalMode) {
    case CELL_RESC_PAL_60_INTERPOLATE:
    case CELL_RESC_PAL_60_INTERPOLATE_30_DROP:
    case CELL_RESC_PAL_60_INTERPOLATE_DROP_FLEXIBLE:
        *numBufs = 6;
        break;
    default:
        *numBufs = 2;
        break;
    }
    return CELL_OK;
}

s32 cellRescGetBufferSize(u32* colorBufSize, u32* vertexBufSize, u32* fragmentBufSize)
{
    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    /* Provide reasonable buffer sizes for state tracking */
    if (colorBufSize)
        *colorBufSize = 1920 * 1080 * 4; /* RGBA 1080p */
    if (vertexBufSize)
        *vertexBufSize = 64 * 1024; /* 64KB vertex buffer */
    if (fragmentBufSize)
        *fragmentBufSize = 256 * 1024; /* 256KB fragment shader */

    return CELL_OK;
}

s32 cellRescSetBufferAddress(void* colorBuf, void* vertexBuf, void* fragmentBuf)
{
    (void)colorBuf;
    (void)vertexBuf;
    (void)fragmentBuf;

    printf("[cellResc] SetBufferAddress()\n");

    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    /* In host-GPU mode we don't use these buffers directly */
    return CELL_OK;
}

s32 cellRescSetSrc(s32 index, const CellRescSrc* src)
{
    printf("[cellResc] SetSrc(index=%d)\n", index);

    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    if (!src || index < 0 || index >= 8)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    s_src[index] = *src;
    return CELL_OK;
}

s32 cellRescSetDsts(u32 displayMode, const CellRescDsts* dsts)
{
    printf("[cellResc] SetDsts(mode=0x%x)\n", displayMode);

    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    if (!dsts)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    /* Map display mode to index */
    int idx = 0;
    if (displayMode & CELL_RESC_720x576) idx = 1;
    else if (displayMode & CELL_RESC_1280x720) idx = 2;
    else if (displayMode & CELL_RESC_1920x1080) idx = 3;

    s_dsts[idx] = *dsts;
    return CELL_OK;
}

s32 cellRescSetConvertAndFlip(s32 index)
{
    (void)index;

    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    /* In recomp, the host GPU handles display -- we just track the flip */
    s_flip_status = 0; /* flip complete */
    s_last_flip_time++; /* monotonic counter, actual time comes from host */

    if (s_flip_handler)
        s_flip_handler(1);

    return CELL_OK;
}

s32 cellRescSetFlipHandler(void (*handler)(u32))
{
    printf("[cellResc] SetFlipHandler(%p)\n", (void*)(uintptr_t)handler);
    s_flip_handler = handler;
    return CELL_OK;
}

s32 cellRescSetVBlankHandler(void (*handler)(u32))
{
    printf("[cellResc] SetVBlankHandler(%p)\n", (void*)(uintptr_t)handler);
    s_vblank_handler = handler;
    return CELL_OK;
}

s32 cellRescGetDisplayMode(u32* displayMode)
{
    if (!displayMode)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    *displayMode = s_display_mode;
    return CELL_OK;
}

s32 cellRescGetLastFlipTime(u64* time)
{
    if (!time)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    *time = s_last_flip_time;
    return CELL_OK;
}

void cellRescResetFlipStatus(void)
{
    s_flip_status = 1; /* waiting for next flip */
}

s32 cellRescGetFlipStatus(void)
{
    return s_flip_status;
}

s32 cellRescSetPalInterpolateDropFlexRatio(float ratio)
{
    printf("[cellResc] SetPalInterpolateDropFlexRatio(%.2f)\n", ratio);
    s_pal_ratio = ratio;
    return CELL_OK;
}

s32 cellRescCreateInterlaceTable(void* buf, float ea, u32 tableLen, s32 depth)
{
    (void)ea;
    (void)depth;

    printf("[cellResc] CreateInterlaceTable(len=%u, depth=%d)\n", tableLen, depth);

    if (!buf)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    /* Fill with simple linear interpolation weights */
    float* table = (float*)buf;
    for (u32 i = 0; i < tableLen; i++)
        table[i] = (float)i / (float)(tableLen > 1 ? tableLen - 1 : 1);

    return CELL_OK;
}

s32 cellRescAdjustAspectRatio(float horizontal, float vertical)
{
    printf("[cellResc] AdjustAspectRatio(h=%.2f, v=%.2f)\n", horizontal, vertical);

    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    s_aspect_h = horizontal;
    s_aspect_v = vertical;
    return CELL_OK;
}
