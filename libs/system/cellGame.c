/*
 * ps3recomp - cellGame HLE implementation
 *
 * Game utility module: boot check, content access, PARAM.SFO access.
 */

#include "cellGame.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#  define HOST_MKDIR(p) _mkdir(p)
#  define HOST_STAT     _stat64
#  define HOST_STAT_T   struct __stat64
#else
#  include <unistd.h>
#  include <sys/types.h>
#  define HOST_MKDIR(p) mkdir(p, 0755)
#  define HOST_STAT     stat
#  define HOST_STAT_T   struct stat
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static char s_title_id[64]  = "BLES00000";
static char s_title[256]    = "Unknown Title";
static char s_app_ver[16]   = "01.00";

/* Content info / usrdir paths */
static char s_content_path[CELL_GAME_PATH_MAX] = "./gamedata/dev_hdd0/game";
static char s_content_info_path[CELL_GAME_PATH_MAX] = "";
static char s_usrdir_path[CELL_GAME_PATH_MAX] = "";
static char s_tmp_path[CELL_GAME_PATH_MAX] = "";
static char s_exit_param[256] = "";

static int  s_boot_checked = 0;
static u32  s_game_type = CELL_GAME_GAMETYPE_DISC;

static void ensure_dirs(const char* path)
{
    char tmp[CELL_GAME_PATH_MAX];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = '\0';
            HOST_MKDIR(tmp);
            *p = saved;
        }
    }
    HOST_MKDIR(tmp);
}

static int dir_exists(const char* path)
{
    HOST_STAT_T st;
    if (HOST_STAT(path, &st) != 0)
        return 0;
#ifdef _WIN32
    return (st.st_mode & _S_IFDIR) != 0;
#else
    return S_ISDIR(st.st_mode);
#endif
}

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/

void cellGame_set_title_id(const char* title_id)
{
    if (!title_id) return;
    strncpy(s_title_id, title_id, sizeof(s_title_id) - 1);
    s_title_id[sizeof(s_title_id) - 1] = '\0';
}

void cellGame_set_title(const char* title)
{
    if (!title) return;
    strncpy(s_title, title, sizeof(s_title) - 1);
    s_title[sizeof(s_title) - 1] = '\0';
}

void cellGame_set_content_path(const char* path)
{
    if (!path) return;
    strncpy(s_content_path, path, sizeof(s_content_path) - 1);
    s_content_path[sizeof(s_content_path) - 1] = '\0';
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellGameBootCheck(u32* type, u32* attributes, CellGameContentSize* size,
                       char* dirName)
{
    printf("[cellGame] BootCheck()\n");

    /* Build paths based on title ID */
    snprintf(s_content_info_path, sizeof(s_content_info_path),
             "%s/%s", s_content_path, s_title_id);
    snprintf(s_usrdir_path, sizeof(s_usrdir_path),
             "%s/%s/USRDIR", s_content_path, s_title_id);
    snprintf(s_tmp_path, sizeof(s_tmp_path),
             "%s/%s_TMP", s_content_path, s_title_id);

#ifdef _WIN32
    for (char* p = s_content_info_path; *p; p++) if (*p == '/') *p = '\\';
    for (char* p = s_usrdir_path; *p; p++) if (*p == '/') *p = '\\';
    for (char* p = s_tmp_path; *p; p++) if (*p == '/') *p = '\\';
#endif

    if (type)
        *type = s_game_type;

    if (attributes)
        *attributes = 0;

    if (size) {
        size->hddFreeSizeKB = 1024 * 1024; /* 1GB free */
        size->sizeKB = CELL_GAME_SIZEKB_NOTCALC;
        size->sysSizeKB = 0;
    }

    if (dirName) {
        strncpy(dirName, s_title_id, CELL_GAME_PATH_MAX - 1);
        dirName[CELL_GAME_PATH_MAX - 1] = '\0';
    }

    s_boot_checked = 1;
    printf("[cellGame] BootCheck: type=%u, titleId='%s'\n", s_game_type, s_title_id);
    return CELL_OK;
}

s32 cellGameContentPermit(char* contentInfoPath, char* usrdirPath)
{
    printf("[cellGame] ContentPermit()\n");

    /* Ensure directories exist */
    ensure_dirs(s_content_info_path);
    ensure_dirs(s_usrdir_path);

    if (contentInfoPath) {
        /* Return PS3-style path */
        snprintf(contentInfoPath, CELL_GAME_PATH_MAX,
                 "/dev_hdd0/game/%s", s_title_id);
    }

    if (usrdirPath) {
        snprintf(usrdirPath, CELL_GAME_PATH_MAX,
                 "/dev_hdd0/game/%s/USRDIR", s_title_id);
    }

    return CELL_OK;
}

s32 cellGameDataCheck(u32 type, const char* dirName, CellGameContentSize* size)
{
    printf("[cellGame] DataCheck(type=%u, dir='%s')\n",
           type, dirName ? dirName : "<null>");

    const char* check_dir = dirName ? dirName : s_title_id;
    char path[CELL_GAME_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", s_content_path, check_dir);

    if (size) {
        size->hddFreeSizeKB = 1024 * 1024;
        size->sizeKB = CELL_GAME_SIZEKB_NOTCALC;
        size->sysSizeKB = 0;
    }

    if (dir_exists(path)) {
        return CELL_OK;
    }

    return CELL_GAME_ERROR_NOTFOUND;
}

s32 cellGameGetParamInt(s32 id, s32* value)
{
    printf("[cellGame] GetParamInt(id=%d)\n", id);

    if (!value)
        return CELL_GAME_ERROR_PARAM;

    switch (id) {
    case CELL_GAME_PARAMID_PARENTAL_LEVEL:
        *value = 0;
        break;
    case CELL_GAME_PARAMID_RESOLUTION:
        *value = 0;
        break;
    case CELL_GAME_PARAMID_SOUND_FORMAT:
        *value = 0;
        break;
    case CELL_GAME_PARAMID_APP_VER:
        *value = 100; /* 1.00 as integer */
        break;
    default:
        printf("[cellGame] WARNING: unknown param int id %d\n", id);
        *value = 0;
        break;
    }

    return CELL_OK;
}

s32 cellGameGetParamString(s32 id, char* buf, u32 bufsize)
{
    printf("[cellGame] GetParamString(id=%d, bufsize=%u)\n", id, bufsize);

    if (!buf || bufsize == 0)
        return CELL_GAME_ERROR_PARAM;

    switch (id) {
    case CELL_GAME_PARAMID_TITLE:
    case CELL_GAME_PARAMID_TITLE_DEFAULT:
        strncpy(buf, s_title, bufsize - 1);
        buf[bufsize - 1] = '\0';
        break;
    case CELL_GAME_PARAMID_TITLE_ID:
        strncpy(buf, s_title_id, bufsize - 1);
        buf[bufsize - 1] = '\0';
        break;
    case CELL_GAME_PARAMID_APP_VER_STR:
    case CELL_GAME_PARAMID_VERSION:
        strncpy(buf, s_app_ver, bufsize - 1);
        buf[bufsize - 1] = '\0';
        break;
    default:
        printf("[cellGame] WARNING: unknown param string id %d\n", id);
        buf[0] = '\0';
        break;
    }

    return CELL_OK;
}

s32 cellGameCreateGameData(CellGameContentSize* size, char* dirName)
{
    printf("[cellGame] CreateGameData()\n");

    char path[CELL_GAME_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", s_content_path, s_title_id);

    ensure_dirs(path);

    /* Also create USRDIR */
    char usrdir[CELL_GAME_PATH_MAX];
    snprintf(usrdir, sizeof(usrdir), "%s/USRDIR", path);
    ensure_dirs(usrdir);

    if (dirName) {
        strncpy(dirName, s_title_id, CELL_GAME_PATH_MAX - 1);
        dirName[CELL_GAME_PATH_MAX - 1] = '\0';
    }

    if (size) {
        size->hddFreeSizeKB = 1024 * 1024;
        size->sizeKB = 0;
        size->sysSizeKB = 0;
    }

    return CELL_OK;
}

s32 cellGameDeleteGameData(const char* dirName)
{
    printf("[cellGame] DeleteGameData(dir='%s')\n", dirName ? dirName : "<null>");

    if (!dirName)
        return CELL_GAME_ERROR_PARAM;

    /* We won't recursively delete host directories for safety.
       Just report success. The game should handle this gracefully. */
    printf("[cellGame] WARNING: DeleteGameData not performing recursive delete for safety\n");
    return CELL_OK;
}

s32 cellGameSetExitParam(const char* param)
{
    printf("[cellGame] SetExitParam(param='%s')\n", param ? param : "<null>");

    if (param) {
        strncpy(s_exit_param, param, sizeof(s_exit_param) - 1);
        s_exit_param[sizeof(s_exit_param) - 1] = '\0';
    }

    return CELL_OK;
}

s32 cellGameGetSizeKB(s32* sizeKB)
{
    printf("[cellGame] GetSizeKB()\n");

    if (!sizeKB)
        return CELL_GAME_ERROR_PARAM;

    /* Report the content directory's size.
       For now, estimate or return 0. */
    *sizeKB = 0;
    return CELL_OK;
}

s32 cellGameGetLocalWebContentPath(char* path)
{
    printf("[cellGame] GetLocalWebContentPath()\n");

    if (!path)
        return CELL_GAME_ERROR_PARAM;

    snprintf(path, CELL_GAME_PATH_MAX,
             "/dev_hdd0/game/%s/USRDIR/web", s_title_id);

    return CELL_OK;
}
