/*
 * ps3recomp - cellVideoOut HLE implementation
 *
 * Video output configuration: resolution, display mode, device info.
 * Defaults to 1280x720 (720p).
 */

#include "cellVideoOut.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static u8  s_resolution_id  = CELL_VIDEO_OUT_RESOLUTION_720;
static u8  s_color_format   = CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_X8R8G8B8;
static u8  s_aspect         = CELL_VIDEO_OUT_ASPECT_16_9;
static u32 s_pitch          = 1280 * 4;

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/

void cellVideoOut_set_resolution(u8 resolutionId)
{
    s_resolution_id = resolutionId;

    /* Update pitch based on resolution */
    CellVideoOutResolution res;
    cellVideoOutGetResolution(resolutionId, &res);
    s_pitch = (u32)res.width * 4;
}

/* ---------------------------------------------------------------------------
 * Helpers
 * -----------------------------------------------------------------------*/

static void get_resolution_wh(u32 resId, u16* w, u16* h)
{
    switch (resId) {
    case CELL_VIDEO_OUT_RESOLUTION_1080:
        *w = 1920; *h = 1080; break;
    case CELL_VIDEO_OUT_RESOLUTION_720:
        *w = 1280; *h = 720;  break;
    case CELL_VIDEO_OUT_RESOLUTION_480:
        *w = 720;  *h = 480;  break;
    case CELL_VIDEO_OUT_RESOLUTION_576:
        *w = 720;  *h = 576;  break;
    case CELL_VIDEO_OUT_RESOLUTION_1600x1080:
        *w = 1600; *h = 1080; break;
    case CELL_VIDEO_OUT_RESOLUTION_1440x1080:
        *w = 1440; *h = 1080; break;
    case CELL_VIDEO_OUT_RESOLUTION_1280x1080:
        *w = 1280; *h = 1080; break;
    case CELL_VIDEO_OUT_RESOLUTION_960x1080:
        *w = 960;  *h = 1080; break;
    default:
        *w = 1280; *h = 720;  break;
    }
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellVideoOutGetState(u32 videoOut, u32 deviceIndex, CellVideoOutState* state)
{
    printf("[cellVideoOut] GetState(videoOut=%u, deviceIndex=%u)\n",
           videoOut, deviceIndex);

    if (!state)
        return CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER;

    if (videoOut != CELL_VIDEO_OUT_PRIMARY && videoOut != CELL_VIDEO_OUT_SECONDARY)
        return CELL_VIDEO_OUT_ERROR_UNSUPPORTED_VIDEO_OUT;

    memset(state, 0, sizeof(CellVideoOutState));

    if (videoOut == CELL_VIDEO_OUT_PRIMARY) {
        state->state      = 2; /* enabled */
        state->colorSpace = CELL_VIDEO_OUT_COLOR_SPACE_RGB;

        /* Set display mode based on current resolution */
        switch (s_resolution_id) {
        case CELL_VIDEO_OUT_RESOLUTION_1080:
            state->displayMode = CELL_VIDEO_OUT_DISPLAY_MODE_1920_1080_59_94HZ;
            break;
        case CELL_VIDEO_OUT_RESOLUTION_480:
            state->displayMode = CELL_VIDEO_OUT_DISPLAY_MODE_720_480_59_94HZ;
            break;
        case CELL_VIDEO_OUT_RESOLUTION_576:
            state->displayMode = CELL_VIDEO_OUT_DISPLAY_MODE_720_576_50HZ;
            break;
        case CELL_VIDEO_OUT_RESOLUTION_720:
        default:
            state->displayMode = CELL_VIDEO_OUT_DISPLAY_MODE_1280_720_59_94HZ;
            break;
        }
    } else {
        state->state = 0; /* disabled */
    }

    return CELL_OK;
}

s32 cellVideoOutGetResolution(u32 resolutionId, CellVideoOutResolution* resolution)
{
    if (!resolution)
        return CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER;

    get_resolution_wh(resolutionId, &resolution->width, &resolution->height);

    printf("[cellVideoOut] GetResolution(id=%u) -> %ux%u\n",
           resolutionId, resolution->width, resolution->height);

    return CELL_OK;
}

s32 cellVideoOutConfigure(u32 videoOut, CellVideoOutConfiguration* config,
                            void* option, u32 waitForEvent)
{
    printf("[cellVideoOut] Configure(videoOut=%u)\n", videoOut);

    if (!config)
        return CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER;

    if (videoOut != CELL_VIDEO_OUT_PRIMARY)
        return CELL_VIDEO_OUT_ERROR_UNSUPPORTED_VIDEO_OUT;

    s_resolution_id = config->resolutionId;
    s_color_format  = config->format;
    s_aspect        = config->aspect;

    if (config->pitch > 0) {
        s_pitch = config->pitch;
    } else {
        u16 w, h;
        get_resolution_wh(s_resolution_id, &w, &h);
        s_pitch = (u32)w * 4;
    }

    printf("[cellVideoOut] Configure: resId=%u, format=%u, aspect=%u, pitch=%u\n",
           s_resolution_id, s_color_format, s_aspect, s_pitch);

    return CELL_OK;
}

s32 cellVideoOutGetConfiguration(u32 videoOut, CellVideoOutConfiguration* config,
                                   void* option)
{
    printf("[cellVideoOut] GetConfiguration(videoOut=%u)\n", videoOut);

    if (!config)
        return CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER;

    if (videoOut != CELL_VIDEO_OUT_PRIMARY)
        return CELL_VIDEO_OUT_ERROR_UNSUPPORTED_VIDEO_OUT;

    memset(config, 0, sizeof(CellVideoOutConfiguration));
    config->resolutionId = s_resolution_id;
    config->format       = s_color_format;
    config->aspect       = s_aspect;
    config->pitch        = s_pitch;

    return CELL_OK;
}

s32 cellVideoOutGetDeviceInfo(u32 videoOut, u32 deviceIndex,
                               CellVideoOutDeviceInfo* info)
{
    printf("[cellVideoOut] GetDeviceInfo(videoOut=%u, deviceIndex=%u)\n",
           videoOut, deviceIndex);

    if (!info)
        return CELL_VIDEO_OUT_ERROR_ILLEGAL_PARAMETER;

    if (videoOut != CELL_VIDEO_OUT_PRIMARY)
        return CELL_VIDEO_OUT_ERROR_UNSUPPORTED_VIDEO_OUT;

    memset(info, 0, sizeof(CellVideoOutDeviceInfo));

    info->portType       = CELL_VIDEO_OUT_OUTPUT_HDMI;
    info->colorSpace     = CELL_VIDEO_OUT_COLOR_SPACE_RGB;
    info->latency        = 0;
    info->state          = 2; /* connected */
    info->rgbOutputRange = 1;

    /* Report supported modes */
    int idx = 0;

    /* 480p */
    info->availableModes[idx].resolutionId = CELL_VIDEO_OUT_RESOLUTION_480;
    info->availableModes[idx].scanMode     = CELL_VIDEO_OUT_SCAN_MODE_PROGRESSIVE;
    info->availableModes[idx].aspect       = CELL_VIDEO_OUT_ASPECT_16_9;
    idx++;

    /* 576p */
    info->availableModes[idx].resolutionId = CELL_VIDEO_OUT_RESOLUTION_576;
    info->availableModes[idx].scanMode     = CELL_VIDEO_OUT_SCAN_MODE_PROGRESSIVE;
    info->availableModes[idx].aspect       = CELL_VIDEO_OUT_ASPECT_16_9;
    idx++;

    /* 720p */
    info->availableModes[idx].resolutionId = CELL_VIDEO_OUT_RESOLUTION_720;
    info->availableModes[idx].scanMode     = CELL_VIDEO_OUT_SCAN_MODE_PROGRESSIVE;
    info->availableModes[idx].aspect       = CELL_VIDEO_OUT_ASPECT_16_9;
    idx++;

    /* 1080p */
    info->availableModes[idx].resolutionId = CELL_VIDEO_OUT_RESOLUTION_1080;
    info->availableModes[idx].scanMode     = CELL_VIDEO_OUT_SCAN_MODE_PROGRESSIVE;
    info->availableModes[idx].aspect       = CELL_VIDEO_OUT_ASPECT_16_9;
    idx++;

    info->availableModeCount = (u8)idx;

    return CELL_OK;
}

s32 cellVideoOutGetNumberOfDevice(u32 videoOut)
{
    printf("[cellVideoOut] GetNumberOfDevice(videoOut=%u)\n", videoOut);

    if (videoOut == CELL_VIDEO_OUT_PRIMARY)
        return 1;

    return 0;
}
