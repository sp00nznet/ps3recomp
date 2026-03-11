/*
 * ps3recomp - cellGame HLE
 *
 * Game utility module: boot check, content access, PARAM.SFO access.
 */

#ifndef PS3RECOMP_CELL_GAME_H
#define PS3RECOMP_CELL_GAME_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/
#define CELL_GAME_PATH_MAX          1055

#define CELL_GAME_GAMETYPE_DISC     1
#define CELL_GAME_GAMETYPE_HDD      2
#define CELL_GAME_GAMETYPE_GAMEDATA 3
#define CELL_GAME_GAMETYPE_HOME     4

/* Boot status */
#define CELL_GAME_ATTRIBUTE_PATCH           (1 << 0)
#define CELL_GAME_ATTRIBUTE_APP_HOME        (1 << 1)
#define CELL_GAME_ATTRIBUTE_DEBUG           (1 << 2)
#define CELL_GAME_ATTRIBUTE_XMBBUY          (1 << 3)
#define CELL_GAME_ATTRIBUTE_COMMERCE2_BROWSER (1 << 4)
#define CELL_GAME_ATTRIBUTE_INVITE_MESSAGE  (1 << 5)
#define CELL_GAME_ATTRIBUTE_CUSTOM_DATA_MESSAGE (1 << 6)
#define CELL_GAME_ATTRIBUTE_WEB_BROWSER     (1 << 8)

/* PARAM.SFO integer parameter IDs */
#define CELL_GAME_PARAMID_TITLE_ID              100
#define CELL_GAME_PARAMID_PARENTAL_LEVEL        101
#define CELL_GAME_PARAMID_RESOLUTION             102
#define CELL_GAME_PARAMID_SOUND_FORMAT           103
#define CELL_GAME_PARAMID_APP_VER               104

/* PARAM.SFO string parameter IDs */
#define CELL_GAME_PARAMID_TITLE                  200
#define CELL_GAME_PARAMID_TITLE_DEFAULT          201
#define CELL_GAME_PARAMID_APP_VER_STR            202
#define CELL_GAME_PARAMID_VERSION                203

/* Size info mode */
#define CELL_GAME_SIZEKB_NOTCALC    (-1)

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
#define CELL_GAME_ERROR_NOTFOUND       (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x01)
#define CELL_GAME_ERROR_BROKEN         (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x02)
#define CELL_GAME_ERROR_INTERNAL       (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x03)
#define CELL_GAME_ERROR_PARAM          (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x04)
#define CELL_GAME_ERROR_NOAPP          (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x05)
#define CELL_GAME_ERROR_ACCESS_ERROR   (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x06)
#define CELL_GAME_ERROR_NOSPACE        (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x07)
#define CELL_GAME_ERROR_NOTSUPPORTED   (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x08)
#define CELL_GAME_ERROR_FAILURE        (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x09)
#define CELL_GAME_ERROR_BUSY           (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x0A)
#define CELL_GAME_ERROR_IN_SHUTDOWN    (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x0B)
#define CELL_GAME_ERROR_INVALID_ID     (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x0C)
#define CELL_GAME_ERROR_EXIST          (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x0D)
#define CELL_GAME_ERROR_NOTPATCH       (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x0E)
#define CELL_GAME_ERROR_INVALID_THEME_FILE (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x0F)
#define CELL_GAME_ERROR_BOOTPATH       (s32)(CELL_ERROR_BASE_SYSUTIL_GAME | 0x10)

/* ---------------------------------------------------------------------------
 * Structures
 * -----------------------------------------------------------------------*/

typedef struct CellGameContentSize {
    s32 hddFreeSizeKB;
    s32 sizeKB;
    s32 sysSizeKB;
} CellGameContentSize;

/* ---------------------------------------------------------------------------
 * Configuration (call before game boots)
 * -----------------------------------------------------------------------*/

/* Set the game's title ID (e.g., "BLUS30443") */
void cellGame_set_title_id(const char* title_id);

/* Set the game's title string */
void cellGame_set_title(const char* title);

/* Set the content info path root */
void cellGame_set_content_path(const char* path);

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 cellGameBootCheck(u32* type, u32* attributes, CellGameContentSize* size,
                       char* dirName);

s32 cellGameContentPermit(char* contentInfoPath, char* usrdirPath);

s32 cellGameDataCheck(u32 type, const char* dirName, CellGameContentSize* size);

s32 cellGameGetParamInt(s32 id, s32* value);

s32 cellGameGetParamString(s32 id, char* buf, u32 bufsize);

s32 cellGameCreateGameData(CellGameContentSize* size, char* dirName);

s32 cellGameDeleteGameData(const char* dirName);

s32 cellGameSetExitParam(const char* param);

s32 cellGameGetSizeKB(s32* sizeKB);

s32 cellGameGetLocalWebContentPath(char* path);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_GAME_H */
