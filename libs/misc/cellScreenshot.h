/*
 * ps3recomp - cellScreenshot HLE
 *
 * Screenshot capture: enable/disable, set parameters and overlay.
 */

#ifndef PS3RECOMP_CELL_SCREENSHOT_H
#define PS3RECOMP_CELL_SCREENSHOT_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

#define CELL_SCREENSHOT_MAX_PATH        1024
#define CELL_SCREENSHOT_MAX_OVERLAY     4

/* Parameter flags */
#define CELL_SCREENSHOT_PHOTO_TITLE_LEN 64
#define CELL_SCREENSHOT_GAME_TITLE_LEN  64
#define CELL_SCREENSHOT_GAME_COMMENT_LEN 256

/* ---------------------------------------------------------------------------
 * Structures
 * -----------------------------------------------------------------------*/

typedef struct CellScreenShotSetParam {
    char photoTitle[CELL_SCREENSHOT_PHOTO_TITLE_LEN];
    char gameTitle[CELL_SCREENSHOT_GAME_TITLE_LEN];
    char gameComment[CELL_SCREENSHOT_GAME_COMMENT_LEN];
} CellScreenShotSetParam;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 cellScreenShotEnable(void);
s32 cellScreenShotDisable(void);
s32 cellScreenShotSetParameter(const CellScreenShotSetParam* param);
s32 cellScreenShotSetOverlayImage(const char* srcDir, const char* srcFile,
                                   s32 offsetX, s32 offsetY);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_SCREENSHOT_H */
