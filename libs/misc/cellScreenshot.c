/*
 * ps3recomp - cellScreenshot HLE implementation
 *
 * Tracks screenshot state. Actual capture requires graphics integration
 * which would be done in the video/GCM layer.
 */

#include "cellScreenshot.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int s_screenshot_enabled = 0;
static CellScreenShotSetParam s_param;

typedef struct {
    int  in_use;
    char srcDir[CELL_SCREENSHOT_MAX_PATH];
    char srcFile[CELL_SCREENSHOT_MAX_PATH];
    s32  offsetX;
    s32  offsetY;
} OverlaySlot;

static OverlaySlot s_overlays[CELL_SCREENSHOT_MAX_OVERLAY];

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellScreenShotEnable(void)
{
    printf("[cellScreenshot] Enable()\n");
    s_screenshot_enabled = 1;
    return CELL_OK;
}

s32 cellScreenShotDisable(void)
{
    printf("[cellScreenshot] Disable()\n");
    s_screenshot_enabled = 0;
    return CELL_OK;
}

s32 cellScreenShotSetParameter(const CellScreenShotSetParam* param)
{
    printf("[cellScreenshot] SetParameter()\n");

    if (!param)
        return CELL_EINVAL;

    s_param = *param;
    return CELL_OK;
}

s32 cellScreenShotSetOverlayImage(const char* srcDir, const char* srcFile,
                                   s32 offsetX, s32 offsetY)
{
    printf("[cellScreenshot] SetOverlayImage(dir=%s, file=%s, x=%d, y=%d)\n",
           srcDir ? srcDir : "(null)",
           srcFile ? srcFile : "(null)",
           offsetX, offsetY);

    if (!srcDir || !srcFile)
        return CELL_EINVAL;

    /* Find a free overlay slot */
    for (int i = 0; i < CELL_SCREENSHOT_MAX_OVERLAY; i++) {
        if (!s_overlays[i].in_use) {
            s_overlays[i].in_use = 1;
            strncpy(s_overlays[i].srcDir, srcDir, CELL_SCREENSHOT_MAX_PATH - 1);
            s_overlays[i].srcDir[CELL_SCREENSHOT_MAX_PATH - 1] = '\0';
            strncpy(s_overlays[i].srcFile, srcFile, CELL_SCREENSHOT_MAX_PATH - 1);
            s_overlays[i].srcFile[CELL_SCREENSHOT_MAX_PATH - 1] = '\0';
            s_overlays[i].offsetX = offsetX;
            s_overlays[i].offsetY = offsetY;
            return CELL_OK;
        }
    }

    /* All slots full, overwrite first */
    s_overlays[0].in_use = 1;
    strncpy(s_overlays[0].srcDir, srcDir, CELL_SCREENSHOT_MAX_PATH - 1);
    s_overlays[0].srcDir[CELL_SCREENSHOT_MAX_PATH - 1] = '\0';
    strncpy(s_overlays[0].srcFile, srcFile, CELL_SCREENSHOT_MAX_PATH - 1);
    s_overlays[0].srcFile[CELL_SCREENSHOT_MAX_PATH - 1] = '\0';
    s_overlays[0].offsetX = offsetX;
    s_overlays[0].offsetY = offsetY;
    return CELL_OK;
}
