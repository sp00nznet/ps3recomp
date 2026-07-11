/*
 * ps3recomp - cellGame HLE implementation
 *
 * Game utility module: boot check, content access, PARAM.SFO access.
 */

#include "cellGame.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base, vm_write32 (guest mem) */
#include "ps3emu/guest_call.h"              /* g_ps3_guest_caller (funcStat dispatch) */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
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

/* ---------------------------------------------------------------------------
 * PARAM.SFO -> title id (2026-06-21): the ROBUST fix for title-id paths.
 *
 * Root cause of the /dev_hdd0/game/BLES00000 bug: s_title_id was a hardcoded
 * placeholder and cellGame_set_title_id() was never called, so EVERY title-id
 * path (cellGameContentPermit/BootCheck/DataCheck/web + cellDiscGameGetBootDiscInfo)
 * used the wrong id. Fix: read the real id from the game's PARAM.SFO once at boot
 * (main.cpp -> cellGame_init_from_paramsfo) so it's correct for ANY title.
 *
 * SFO format is LITTLE-ENDIAN: header @0 (magic "\0PSF", key_table@0x08,
 * data_table@0x0C, entries@0x10), then entries[0x10 each]: key_off(u16)@0,
 * fmt(u16)@2, data_len(u32)@4, data_max(u32)@8, data_off(u32)@0xC.
 * -----------------------------------------------------------------------*/
static int sfo_read_string(const char* sfo_path, const char* key,
                           char* out, int out_size)
{
    FILE* fp = fopen(sfo_path, "rb");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (sz < 0x14 || sz > (1 << 20)) { fclose(fp); return -1; }
    unsigned char* d = (unsigned char*)malloc((size_t)sz);
    if (!d) { fclose(fp); return -1; }
    int ok = (fread(d, 1, (size_t)sz, fp) == (size_t)sz);
    fclose(fp);
    if (!ok) { free(d); return -1; }

    int ret = -1;
    if (d[0]==0x00 && d[1]==0x50 && d[2]==0x53 && d[3]==0x46) {  /* "\0PSF" */
        #define SFO_RD32(o) ((u32)d[o] | ((u32)d[(o)+1]<<8) | ((u32)d[(o)+2]<<16) | ((u32)d[(o)+3]<<24))
        #define SFO_RD16(o) ((u16)d[o] | ((u16)d[(o)+1]<<8))
        u32 key_tab  = SFO_RD32(0x08);
        u32 data_tab = SFO_RD32(0x0C);
        u32 n        = SFO_RD32(0x10);
        for (u32 i = 0; i < n; i++) {
            u32 e = 0x14 + i * 0x10;
            if (e + 0x10 > (u32)sz) break;
            u32 key_off  = SFO_RD16(e + 0x00);
            u32 data_len = SFO_RD32(e + 0x04);
            u32 data_off = SFO_RD32(e + 0x0C);
            if (key_tab + key_off >= (u32)sz) continue;
            if (strcmp((const char*)(d + key_tab + key_off), key) != 0) continue;
            u32 src = data_tab + data_off;
            if (src >= (u32)sz) break;
            int copy = (int)data_len;
            if (copy > (int)((u32)sz - src)) copy = (int)((u32)sz - src);
            if (copy >= out_size) copy = out_size - 1;
            if (copy < 0) copy = 0;
            memcpy(out, d + src, (size_t)copy);
            out[copy] = '\0';
            ret = 0;
            break;
        }
        #undef SFO_RD32
        #undef SFO_RD16
    }
    free(d);
    return ret;
}

/* Read TITLE_ID / TITLE / APP_VER from the game's PARAM.SFO at boot. Call once
 * from main.cpp before the guest runs. Falls back to the defaults if the SFO
 * can't be read (keeps the game working without it). */
void cellGame_init_from_paramsfo(const char* sfo_path)
{
    char tmp[256];
    if (sfo_read_string(sfo_path, "TITLE_ID", tmp, sizeof(tmp)) == 0 && tmp[0]) {
        strncpy(s_title_id, tmp, sizeof(s_title_id) - 1);
        s_title_id[sizeof(s_title_id) - 1] = '\0';
        printf("[cellGame] title id from PARAM.SFO ('%s'): '%s'\n", sfo_path, s_title_id);
    } else {
        printf("[cellGame] PARAM.SFO not read ('%s'); keeping title id '%s'\n",
               sfo_path, s_title_id);
    }
    if (sfo_read_string(sfo_path, "TITLE", tmp, sizeof(tmp)) == 0 && tmp[0]) {
        strncpy(s_title, tmp, sizeof(s_title) - 1); s_title[sizeof(s_title) - 1] = '\0';
    }
    if (sfo_read_string(sfo_path, "APP_VER", tmp, sizeof(tmp)) == 0 && tmp[0]) {
        strncpy(s_app_ver, tmp, sizeof(s_app_ver) - 1); s_app_ver[sizeof(s_app_ver) - 1] = '\0';
    }
}

/* Central title-id accessor so other modules (cellSysutil etc.) don't hardcode it. */
const char* cellGame_get_title_id(void) { return s_title_id; }

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

    /* The HLE bridge passes the GUEST addresses of these out-params (the PPC
     * r3-r6 values), not host pointers. Translate through vm_base and write
     * big-endian via vm_write32 -- dereferencing them directly would fault on
     * the guest stack address (e.g. 0x0FEFFB60). */
    uint32_t type_ea = (uint32_t)(uintptr_t)type;
    uint32_t attr_ea = (uint32_t)(uintptr_t)attributes;
    uint32_t size_ea = (uint32_t)(uintptr_t)size;
    uint32_t dir_ea  = (uint32_t)(uintptr_t)dirName;

    if (type_ea) vm_write32(type_ea, (uint32_t)s_game_type);
    if (attr_ea) vm_write32(attr_ea, 0);

    if (size_ea) {
        vm_write32(size_ea + 0, 1024 * 1024);                 /* hddFreeSizeKB = 1 GB */
        vm_write32(size_ea + 4, (uint32_t)CELL_GAME_SIZEKB_NOTCALC);
        vm_write32(size_ea + 8, 0);                           /* sysSizeKB */
    }

    if (dir_ea) {
        size_t len = strlen(s_title_id);
        if (len > CELL_GAME_PATH_MAX - 1) len = CELL_GAME_PATH_MAX - 1;
        memcpy(vm_base + dir_ea, s_title_id, len);
        vm_base[dir_ea + len] = '\0';
    }

    s_boot_checked = 1;
    printf("[cellGame] BootCheck: type=%u, titleId='%s'\n", s_game_type, s_title_id);
    return CELL_OK;
}

s32 cellGameContentPermit(char* contentInfoPath, char* usrdirPath)
{
    printf("[cellGame] ContentPermit()\n");

    /* Ensure directories exist. IMPORTANT: also create them at the SAME host
     * location the VFS (ppu_fs.cpp host_path) maps /dev_hdd0/game/<title>/ to,
     * i.e. $ppu_vfs_root/game/<title>/. cellGame historically only created
     * ./gamedata/dev_hdd0/game/... which cellFs never looks at, so a game that
     * polls cellFsStat for its game-data USRDIR (LBP's boot bringup stage) stalls
     * forever and exits. Create it where cellFs will actually find it. */
    ensure_dirs(s_content_info_path);
    ensure_dirs(s_usrdir_path);
    {
        extern const char* ppu_vfs_root;
        char vpath[CELL_GAME_PATH_MAX];
        snprintf(vpath, sizeof vpath, "%s/game/%s/USRDIR", ppu_vfs_root, s_title_id);
        ensure_dirs(vpath);
    }

    /* Both paths are GUEST addresses; format into a host temp, copy into the
     * guest buffer through vm_base (a raw snprintf to the guest addr faults). */
    char tmp[CELL_GAME_PATH_MAX];
    uint32_t cip_ea = (uint32_t)(uintptr_t)contentInfoPath;
    uint32_t usr_ea = (uint32_t)(uintptr_t)usrdirPath;
    if (cip_ea) {
        int n = snprintf(tmp, sizeof tmp, "/dev_hdd0/game/%s", s_title_id);
        memcpy(vm_base + cip_ea, tmp, (size_t)n + 1);
    }
    if (usr_ea) {
        int n = snprintf(tmp, sizeof tmp, "/dev_hdd0/game/%s/USRDIR", s_title_id);
        memcpy(vm_base + usr_ea, tmp, (size_t)n + 1);
    }

    return CELL_OK;
}

s32 cellGameDataCheck(u32 type, const char* dirName, CellGameContentSize* size)
{
    printf("[cellGame] DataCheck(type=%u, dir='%s')\n",
           type, dirName ? dirName : "<null>");

    uint32_t dir_ea = (uint32_t)(uintptr_t)dirName;   /* guest addr */
    const char* check_dir = dir_ea ? (const char*)(vm_base + dir_ea) : s_title_id;
    char path[CELL_GAME_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", s_content_path, check_dir);

    uint32_t size_ea = (uint32_t)(uintptr_t)size;
    if (size_ea) {
        vm_write32(size_ea + 0, 1024 * 1024);
        vm_write32(size_ea + 4, (uint32_t)CELL_GAME_SIZEKB_NOTCALC);
        vm_write32(size_ea + 8, 0);
    }

    if (dir_exists(path)) {
        return CELL_OK;
    }

    return CELL_GAME_ERROR_NOTFOUND;
}

/* cellGameDataCheckCreate2 (NID 0xC9645C41) -- check/prepare game data, invoking the
 * title's funcStat callback (CellGameDataStatCallback) exactly like firmware so the
 * save/boot state machine advances. Struct layout + behavior cross-referenced against
 * RPCS3 cellGame.cpp. Demon's Souls's DsSaveLoadMan thread calls this; without the
 * callback firing, its game-data check never completes, the sema/SLSession chain
 * stalls, and the cellSaveDataAutoLoad2 (auto-save notice) dialog never appears.
 *
 * funcStat(cbResult, get, set): the title reads `get` (isNewData, free space, params)
 * and writes cbResult->result. We report existing data (isNewData=0) + ample space;
 * DeS's callback returns CELL_GAMEDATA_CBRESULT_OK_CANCEL (check-only) -> CELL_OK. */
#define GAMEDATA_CB_BASE   0x02520000u   /* guest scratch (distinct from savedata's) */
#define CELL_GAMEDATA_CBRESULT_OK_CANCEL  1
#define CELL_GAMEDATA_CBRESULT_OK         0

s32 cellGameDataCheckCreate2(u32 version, const char* dirName, u32 errDialog,
                             void* funcStat, u32 container)
{
    (void)errDialog; (void)container;
    uint32_t dir_ea  = (uint32_t)(uintptr_t)dirName;
    uint32_t func_opd = (uint32_t)(uintptr_t)funcStat;
    const char* dir = dir_ea ? (const char*)(vm_base + dir_ea) : "";
    printf("[cellGame] DataCheckCreate2(version=%u dir='%s' errDialog=%u funcStat=0x%08X)\n",
           version, dir, errDialog, func_opd);

    if (version != 0 || !dir_ea || !func_opd)
        return CELL_GAME_ERROR_PARAM;
    if (!g_ps3_guest_caller)
        return CELL_OK;

    /* Guest scratch for the 3 callback structs: CBResult(0x18)/StatGet(0xBE8)/StatSet(0x0C). */
    uint32_t cb  = GAMEDATA_CB_BASE;
    uint32_t get = cb + 0x20;
    uint32_t set = get + 0xC00;
    memset(vm_base + cb, 0, (set + 0x20) - cb);

    /* CellGameDataStatGet (offsets per SDK, RPCS3-verified). */
    vm_write32(get + 0x000, 40u * 1024u * 1024u - 256u);  /* hddFreeSizeKB (~40 GB) */
    vm_write32(get + 0x004, 0);                            /* isNewData = 0 (data exists) */
    snprintf((char*)(vm_base + get + 0x008), 96, "/dev_hdd0/game/%s", dir);          /* contentInfoPath */
    snprintf((char*)(vm_base + get + 0x427), 96, "/dev_hdd0/game/%s/USRDIR", dir);   /* gameDataPath */
    vm_write32(get + 0xB9C, 0xFFFFFFFFu);                  /* sizeKB = NOTCALC (-1) */
    vm_write32(get + 0xBA0, 0);                            /* sysSizeKB */
    /* getParam (CellGameDataSystemFileParam @0x860): title / titleId / dataVersion / attribute. */
    uint32_t gp = get + 0x860;
    snprintf((char*)(vm_base + gp + 0x000), 128, "%s", s_title);
    snprintf((char*)(vm_base + gp + 0xA80), 10,  "%s", dir);        /* titleId = dir (e.g. BLUS30443) */
    snprintf((char*)(vm_base + gp + 0xA8C), 6,   "%s", s_app_ver);  /* dataVersion */
    vm_write32(gp + 0xA98, 0);                                      /* attribute = CELL_GAMEDATA_ATTR_NORMAL */

    /* StatSet left zeroed (setParam = NULL: callback chooses whether to modify). */

    g_ps3_guest_caller(func_opd, cb, get, set, 0);

    s32 result = (s32)vm_read32(cb + 0x000);
    printf("[cellGame] DataCheckCreate2: funcStat returned result=%d\n", result);
    if (result < 0)
        return CELL_GAME_ERROR_PARAM;   /* callback reported an error */
    /* OK / OK_CANCEL: succeed (our HLE doesn't create/modify the on-disk data). */
    return CELL_OK;
}

s32 cellGameGetParamInt(s32 id, s32* value)
{
    printf("[cellGame] GetParamInt(id=%d)\n", id);

    /* `value` is a GUEST address; write big-endian via vm_write32. */
    uint32_t value_ea = (uint32_t)(uintptr_t)value;
    if (!value_ea)
        return CELL_GAME_ERROR_PARAM;

    uint32_t v = 0;
    switch (id) {
    case CELL_GAME_PARAMID_APP_VER:
        v = 100; /* 1.00 as integer */
        break;
    case CELL_GAME_PARAMID_PARENTAL_LEVEL:
    case CELL_GAME_PARAMID_RESOLUTION:
    case CELL_GAME_PARAMID_SOUND_FORMAT:
        v = 0;
        break;
    default:
        printf("[cellGame] WARNING: unknown param int id %d\n", id);
        v = 0;
        break;
    }
    vm_write32(value_ea, v);

    return CELL_OK;
}

s32 cellGameGetParamString(s32 id, char* buf, u32 bufsize)
{
    printf("[cellGame] GetParamString(id=%d, bufsize=%u)\n", id, bufsize);

    /* `buf` is a GUEST address (raw r4); translate through vm_base. Strings are
     * byte data, so no byte-swap -- just write into the guest buffer. A raw
     * strncpy(buf, ...) faulted on the guest stack address. */
    uint32_t buf_ea = (uint32_t)(uintptr_t)buf;
    if (!buf_ea || bufsize == 0)
        return CELL_GAME_ERROR_PARAM;
    char* hbuf = (char*)(vm_base + buf_ea);

    const char* src = "";
    switch (id) {
    case CELL_GAME_PARAMID_TITLE:
    case CELL_GAME_PARAMID_TITLE_DEFAULT:  src = s_title;    break;
    case CELL_GAME_PARAMID_TITLE_ID:       src = s_title_id; break;
    case CELL_GAME_PARAMID_APP_VER_STR:
    case CELL_GAME_PARAMID_VERSION:        src = s_app_ver;  break;
    default:
        printf("[cellGame] WARNING: unknown param string id %d\n", id);
        break;
    }
    strncpy(hbuf, src, bufsize - 1);
    hbuf[bufsize - 1] = '\0';
    return CELL_OK;
}

s32 cellGameCreateGameData(CellGameSetInitParams* init, char* tmp_contentInfoPath,
                            char* tmp_usrdirPath)
{
    printf("[cellGame] CreateGameData()\n");

    uint32_t init_ea = (uint32_t)(uintptr_t)init;   /* guest addrs */
    if (!init_ea)
        return CELL_GAME_ERROR_PARAM;

    char path[CELL_GAME_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", s_content_path, s_title_id);

    ensure_dirs(path);

    /* Also create USRDIR */
    char usrdir[CELL_GAME_PATH_MAX];
    snprintf(usrdir, sizeof(usrdir), "%s/USRDIR", path);
    ensure_dirs(usrdir);

    uint32_t cip_ea = (uint32_t)(uintptr_t)tmp_contentInfoPath;
    uint32_t usr_ea = (uint32_t)(uintptr_t)tmp_usrdirPath;
    if (cip_ea) {
        size_t len = strlen(path);
        if (len > CELL_GAME_PATH_MAX - 1) len = CELL_GAME_PATH_MAX - 1;
        memcpy(vm_base + cip_ea, path, len);
        vm_base[cip_ea + len] = '\0';
    }
    if (usr_ea) {
        size_t len = strlen(usrdir);
        if (len > CELL_GAME_PATH_MAX - 1) len = CELL_GAME_PATH_MAX - 1;
        memcpy(vm_base + usr_ea, usrdir, len);
        vm_base[usr_ea + len] = '\0';
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

/* cellGameSetExitParam is implemented in cellGameExec.c with the correct
 * PS3 SDK signature (CellGameExecBootParam*). Not duplicated here. */

s32 cellGameGetSizeKB(s32* sizeKB)
{
    printf("[cellGame] GetSizeKB()\n");

    uint32_t sizeKB_ea = (uint32_t)(uintptr_t)sizeKB;
    if (!sizeKB_ea)
        return CELL_GAME_ERROR_PARAM;

    /* Report the content directory's size (guest addr; 0 for now). */
    vm_write32(sizeKB_ea, 0);
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
