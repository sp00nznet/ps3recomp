#include "ps3emu/strict_fs.h"
#include "ps3emu/strict_boot.h"
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
#include <limits.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#  define HOST_MKDIR(p) ps3_strict_fs_mkdir((p), 0755)
#  define HOST_STAT     _stat64
#  define HOST_STAT_T   struct __stat64
#else
#  include <unistd.h>
#  include <sys/types.h>
#  include <sys/statvfs.h>
#  define HOST_MKDIR(p) ps3_strict_fs_mkdir((p), 0755)
#  define HOST_STAT     stat
#  define HOST_STAT_T   struct stat
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static char s_title_id[64]  = "BLES00000";
static char s_title[256]    = "Unknown Title";
static char s_version[16]   = "01.00";
static char s_system_ver[16] = "00.0000";
static char s_app_ver[16]   = "01.00";

/* Content info / usrdir paths.
 *
 * This is the HOST directory the guest path /dev_hdd0/game maps to, and it has
 * to be the SAME directory ppu_fs.cpp resolves that mount to. cellGame creates
 * the game-data dir and hands the title a guest path; the title then opens files
 * under that path through cellFs. If the two disagree the title writes its game
 * data into one directory and reads it back from another, finds nothing, and
 * reports the data as CORRUPT rather than as missing.
 *
 * LBP does exactly that: cellGameDataCheck saw ./gamedata/.../NPEA00241 -- left
 * over from an earlier run's cellGameCreateGameData, and empty -- answered
 * CELL_OK ("installed"), and the title then read /dev_hdd0/game/NPEA00241/USRDIR,
 * which ppu_fs maps under the VFS root, got nothing, and put up
 * cellGameContentErrorDialog(BROKEN_EXIT_GAMEDATA) and exited.
 *
 * The literal below is only the fallback for a host with no VFS configured;
 * content_root() resolves the real one on first use. */
static char s_content_path[CELL_GAME_PATH_MAX] = "./gamedata/dev_hdd0/game";
static int  s_content_path_resolved = 0;

/* Resolved the same way ppu_fs.cpp resolves the /dev_hdd0 mount: $PS3_HDD0_ROOT
 * if set, else the VFS root -- in both cases with the "/dev_hdd0/" prefix
 * stripped, which is what ppu_fs does to the guest path. */
extern const char* ppu_vfs_root;

static const char* content_root(void)
{
    if (!s_content_path_resolved) {
        s_content_path_resolved = 1;
        const char* hdd0 = getenv("PS3_HDD0_ROOT");
        if (hdd0 && *hdd0)
            snprintf(s_content_path, sizeof s_content_path, "%s/game", hdd0);
        else if (ppu_vfs_root && *ppu_vfs_root && strcmp(ppu_vfs_root, ".") != 0)
            /* NB: <root>/game, not <root>/dev_hdd0/game. ppu_fs STRIPS the mount
             * prefix -- guest /dev_hdd0/game/<id> resolves to <root>/game/<id>.
             * Spelling the device out here just recreates the same split one
             * directory over, with each side quietly making its own tree. */
            snprintf(s_content_path, sizeof s_content_path, "%s/game", ppu_vfs_root);
        for (char* q = s_content_path; *q; q++) if (*q == 0x5C) *q = 0x2F;
        printf("[cellGame] content root (host /dev_hdd0/game): %s\n", s_content_path);
    }
    return s_content_path;
}
static char s_content_info_path[CELL_GAME_PATH_MAX] = "";
static char s_usrdir_path[CELL_GAME_PATH_MAX] = "";
static char s_tmp_path[CELL_GAME_PATH_MAX] = "";
static char s_exit_param[256] = "";

static int  s_boot_checked = 0;
static u32  s_game_type = CELL_GAME_GAMETYPE_DISC;

/* Current content-check session. BootCheck/DataCheck open a session naming
 * some content; ContentPermit reports THAT content's paths and closes it.
 * Disc sessions must yield /dev_bdvd/PS3_GAME -- always returning the
 * game-data dir sent LBP hunting for data.farc on /dev_hdd0 (ENOENT), so
 * every resource load failed and the loader stalled polling its completion
 * semaphore forever. */
static int  s_check_is_disc = 1;
static char s_check_dir[64] = "";

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

/* Content may not exist on first boot. Query its nearest existing ancestor
 * so installation checks use the destination volume, without creating data. */
static s32 content_free_kb(void)
{
    char path[CELL_GAME_PATH_MAX];
    snprintf(path, sizeof(path), "%s", s_content_path[0] ? s_content_path : ".");
    for (;;) {
#ifdef _WIN32
        ULARGE_INTEGER available;
        if (GetDiskFreeSpaceExA(path, &available, NULL, NULL)) {
            u64 kb = available.QuadPart / 1024;
            return kb > INT32_MAX ? INT32_MAX : (s32)kb;
        }
        DWORD error = GetLastError();
        if (error != ERROR_PATH_NOT_FOUND && error != ERROR_FILE_NOT_FOUND) break;
#else
        struct statvfs info;
        if (statvfs(path, &info) == 0) {
            u64 block = info.f_frsize ? info.f_frsize : info.f_bsize;
            u64 count = info.f_bavail;
            if (!block) return 0;
            if (count > ((u64)INT32_MAX * 1024) / block) return INT32_MAX;
            return (s32)((count * block) / 1024);
        }
        if (errno != ENOENT && errno != ENOTDIR) break;
#endif
        size_t len = strlen(path);
#ifdef _WIN32
        if (len == 3 && path[1] == ':' && (path[2] == '/' || path[2] == '\\')) break;
#endif
        while (len > 1 && (path[len - 1] == '/' || path[len - 1] == '\\'))
            path[--len] = 0;
        char* slash = strrchr(path, '/');
#ifdef _WIN32
        char* backslash = strrchr(path, '\\');
        if (backslash && (!slash || backslash > slash)) slash = backslash;
        if (slash == path + 2 && path[1] == ':') {
            if (!slash[1]) break;
            slash[1] = 0;
            continue;
        }
#endif
        if (!slash) {
            if (strcmp(path, ".") == 0) break;
            strcpy(path, ".");
        } else if (slash == path) {
            if (!slash[1]) break;
            slash[1] = 0;
        } else {
            *slash = 0;
        }
    }
    return 0;
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

/* The game's display name, straight from PARAM.SFO's TITLE. The boot harness
 * uses this for the window caption -- it used to hardcode another title's name,
 * which is confusing the moment you run two ports side by side. */
const char* cellGame_get_title(void)
{
    return s_title;
}

void cellGame_set_content_path(const char* path)
{
    if (!path) return;
    s_content_path_resolved = 1;   /* explicit override beats content_root() */
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
    FILE* fp = ps3_strict_fs_fopen(sfo_path, "rb");
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

/* Read title/version fields from the game's PARAM.SFO at boot. Call once
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
    if (sfo_read_string(sfo_path, "VERSION", tmp, sizeof(tmp)) == 0 && tmp[0]) {
        strncpy(s_version, tmp, sizeof(s_version) - 1); s_version[sizeof(s_version) - 1] = '\0';
    }
    if (sfo_read_string(sfo_path, "PS3_SYSTEM_VER", tmp, sizeof(tmp)) == 0 && tmp[0]) {
        strncpy(s_system_ver, tmp, sizeof(s_system_ver) - 1);
        s_system_ver[sizeof(s_system_ver) - 1] = '\0';
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
             "%s/%s", content_root(), s_title_id);
    snprintf(s_usrdir_path, sizeof(s_usrdir_path),
             "%s/%s/USRDIR", content_root(), s_title_id);
    snprintf(s_tmp_path, sizeof(s_tmp_path),
             "%s/%s_TMP", content_root(), s_title_id);

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
        vm_write32(size_ea + 0, (u32)content_free_kb());
        vm_write32(size_ea + 4, (uint32_t)CELL_GAME_SIZEKB_NOTCALC);
        vm_write32(size_ea + 8, 0);                           /* sysSizeKB */
    }

    if (dir_ea) {
        size_t len = strlen(s_title_id);
        if (len > CELL_GAME_DIRNAME_SIZE - 1) len = CELL_GAME_DIRNAME_SIZE - 1;
        memcpy(vm_base + dir_ea, s_title_id, len);
        vm_base[dir_ea + len] = '\0';
    }

    s_boot_checked = 1;
    /* Open a check session for the BOOT content (disc titles boot from
     * /dev_bdvd, not from the game-data dir). */
    s_check_is_disc = (s_game_type == CELL_GAME_GAMETYPE_DISC);
    strncpy(s_check_dir, s_title_id, sizeof(s_check_dir) - 1);
    s_check_dir[sizeof(s_check_dir) - 1] = '\0';
    printf("[cellGame] BootCheck: type=%u, titleId='%s'\n", s_game_type, s_title_id);
    return CELL_OK;
}

s32 cellGameContentPermit(char* contentInfoPath, char* usrdirPath)
{
    printf("[cellGame] ContentPermit() -> %s\n",
           s_check_is_disc ? "/dev_bdvd/PS3_GAME" : "game-data dir");

    if (!s_check_is_disc) {
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
            snprintf(vpath, sizeof vpath, "%s/game/%s/USRDIR", ppu_vfs_root, s_check_dir);
            ensure_dirs(vpath);
        }
    }

    /* Both paths are GUEST addresses; format into a host temp, copy into the
     * guest buffer through vm_base (a raw snprintf to the guest addr faults).
     * The paths must match the content the current session checked: the disc
     * for BootCheck/DataCheck(DISC), the game-data dir otherwise. */
    char tmp[CELL_GAME_PATH_MAX];
    uint32_t cip_ea = (uint32_t)(uintptr_t)contentInfoPath;
    uint32_t usr_ea = (uint32_t)(uintptr_t)usrdirPath;
    if (cip_ea) {
        int n = s_check_is_disc
              ? snprintf(tmp, sizeof tmp, "/dev_bdvd/PS3_GAME")
              : snprintf(tmp, sizeof tmp, "/dev_hdd0/game/%s", s_check_dir);
        memcpy(vm_base + cip_ea, tmp, (size_t)n + 1);
    }
    if (usr_ea) {
        int n = s_check_is_disc
              ? snprintf(tmp, sizeof tmp, "/dev_bdvd/PS3_GAME/USRDIR")
              : snprintf(tmp, sizeof tmp, "/dev_hdd0/game/%s/USRDIR", s_check_dir);
        memcpy(vm_base + usr_ea, tmp, (size_t)n + 1);
    }

    return CELL_OK;
}

s32 cellGameDataCheck(u32 type, const char* dirName, CellGameContentSize* size)
{
    /* dirName is a GUEST address -- translate BEFORE any use. The old code
     * printf'd the raw EA as a host char* and crashed (LBP: read fault at
     * 0x94AEA0 = the guest EA of its dir string). */
    uint32_t dir_ea = (uint32_t)(uintptr_t)dirName;   /* guest addr */
    const char* check_dir = dir_ea ? (const char*)(vm_base + dir_ea) : s_title_id;
    printf("[cellGame] DataCheck(type=%u, dir='%s')\n", type, check_dir);

    /* Open a check session for this content; the next ContentPermit reports
     * its paths (disc -> /dev_bdvd/PS3_GAME, else the game-data dir). */
    s_check_is_disc = (type == CELL_GAME_GAMETYPE_DISC);
    strncpy(s_check_dir, check_dir, sizeof(s_check_dir) - 1);
    s_check_dir[sizeof(s_check_dir) - 1] = '\0';

    char path[CELL_GAME_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", content_root(), check_dir);

    uint32_t size_ea = (uint32_t)(uintptr_t)size;
    if (size_ea) {
        vm_write32(size_ea + 0, (u32)content_free_kb());
        vm_write32(size_ea + 4, (uint32_t)CELL_GAME_SIZEKB_NOTCALC);
        vm_write32(size_ea + 8, 0);
    }

    /* Disc-content check (type 1 = CELL_GAME_GAMETYPE_DISC): the disc is
     * inherently present (mounted via the VFS root); a NOTFOUND here makes
     * disc titles bail out of boot. */
    if (type == 1)
        return CELL_OK;

    if (dir_exists(path)) {
        return CELL_OK;
    }

    /* Missing game-data dir is NOT an error: firmware (and RPCS3) return
     * CELL_GAME_RET_NONE (2) so the title knows to cellGameCreateGameData.
     * Returning ERROR_NOTFOUND here made LBP treat first boot as fatal and
     * exit(0) politely. sizeKB must read 0 (not NOTCALC) for the no-data case. */
    if (size_ea)
        vm_write32(size_ea + 4, 0);   /* sizeKB = 0 (no data yet) */
    return CELL_GAME_RET_NONE;
}

/* Guest layout from the pinned RPCS3 cellGame.h. Keep every callback object
 * disjoint; the old 0xC00 StatGet reservation overlapped its 0x1448-byte body. */
#define GAMEDATA_CB_BASE 0x02520000u
_Static_assert(sizeof(CellGameDataSystemFileParam) == 0xB9C, "gamedata param ABI");
_Static_assert(offsetof(CellGameDataSystemFileParam, titleId) == 0xA80, "gamedata title ID offset");
_Static_assert(offsetof(CellGameDataSystemFileParam, dataVersion) == 0xA8C, "gamedata version offset");
_Static_assert(offsetof(CellGameDataSystemFileParam, parentalLevel) == 0xA94, "gamedata parental offset");
_Static_assert(offsetof(CellGameDataStatGet, gameDataPath) == 0x427, "gamedata path offset");
_Static_assert(offsetof(CellGameDataStatGet, st_atime_) == 0x848, "gamedata time offset");
_Static_assert(offsetof(CellGameDataStatGet, getParam) == 0x860, "gamedata param offset");
_Static_assert(offsetof(CellGameDataStatGet, sizeKB) == 0x13FC, "gamedata size offset");
_Static_assert(sizeof(CellGameDataStatGet) == 0x1448, "gamedata stat ABI");
_Static_assert(sizeof(CellGameDataStatSet) == 8, "gamedata set ABI");
_Static_assert(sizeof(CellGameDataCBResult) == 16, "gamedata result ABI");
static atomic_flag gamedata_in_callback = ATOMIC_FLAG_INIT;
extern uint32_t ppu_vm_size;

static u32 gamedata_le32(const unsigned char* p)
{
    return (u32)p[0] | ((u32)p[1]<<8) | ((u32)p[2]<<16) | ((u32)p[3]<<24);
}

/* Read only on-disk metadata. Missing SFO is new data; malformed SFO is broken.
 * Nothing is inferred from the boot title or synthesized as existing data. */
static s32 gamedata_read_param(const char* path, uint32_t gp, u32* is_new, int hdd)
{
    FILE* f = ps3_strict_fs_fopen(path, "rb");
    if (!f) {
        if (errno == ENOENT) { *is_new = 1; return 0; }
        return CELL_GAMEDATA_ERROR_ACCESS_ERROR;
    }
    *is_new = 0;
    if (fseek(f,0,SEEK_END)) { fclose(f); return CELL_GAMEDATA_ERROR_BROKEN; }
    long size = ftell(f);
    if (size < 20 || size > 1024*1024 || fseek(f,0,SEEK_SET)) { fclose(f); return CELL_GAMEDATA_ERROR_BROKEN; }
    unsigned char* d = malloc((size_t)size);
    if (!d) { fclose(f); return CELL_GAMEDATA_ERROR_ACCESS_ERROR; }
    int ok = fread(d,1,(size_t)size,f) == (size_t)size; fclose(f);
    if (!ok || memcmp(d,"\0PSF",4)) { free(d); return CELL_GAMEDATA_ERROR_BROKEN; }
    u32 keys=gamedata_le32(d+8), values=gamedata_le32(d+12), count=gamedata_le32(d+16);
    int category_ok=0, app_ver=0;
    if (count > ((u32)size-20)/16 || keys > (u32)size || values > (u32)size) ok=0;
    for (u32 i=0; ok && i<count; ++i) {
        const unsigned char* e=d+20+i*16;
        u32 ko=(u32)e[0]|((u32)e[1]<<8), fmt=(u32)e[2]|((u32)e[3]<<8);
        u32 len=gamedata_le32(e+4), off=gamedata_le32(e+12);
        if (ko >= (u32)size-keys || off > (u32)size-values || len > (u32)size-values-off) {ok=0;break;}
        const char* key=(const char*)d+keys+ko;
        if (!memchr(key,0,(size_t)size-keys-ko)) {ok=0;break;}
        const unsigned char* value=d+values+off;
        if (fmt==0x204 && (!len || !memchr(value,0,len))) {ok=0;break;}
        if (!strcmp(key,"CATEGORY")) {
            /* Pinned Loader/PSF.h::is_cat_hdd excludes disc/data categories. */
            const char* cat=(const char*)value;
            category_ok = fmt==0x204 && (hdd
                ? strlen(cat)==2 && cat[1]!='D' && strcmp(cat,"DG") && strcmp(cat,"MS")
                : !strcmp(cat,"GD") || !strcmp(cat,"DG"));
        }
        size_t dst=0, cap=0;
        if (!strcmp(key,"TITLE")) {dst=offsetof(CellGameDataSystemFileParam,title);cap=128;}
        else if (!strcmp(key,"TITLE_ID")) {dst=offsetof(CellGameDataSystemFileParam,titleId);cap=10;}
        else if (!strcmp(key,"APP_VER") || (!app_ver && !strcmp(key,"VERSION"))) {
            dst=offsetof(CellGameDataSystemFileParam,dataVersion);cap=6;
            if (!strcmp(key,"APP_VER")) app_ver=1;
        } else for (u32 lang=0;lang<20;++lang) {
            char name[16];snprintf(name,sizeof name,"TITLE_%02u",lang);
            if (!strcmp(key,name)) {dst=offsetof(CellGameDataSystemFileParam,titleLang)+lang*128;cap=128;break;}
        }
        if (cap) {
            if (fmt!=0x204) {ok=0;break;}
            snprintf((char*)vm_base+gp+dst,cap,"%s",(const char*)value);
        }
        const char* ints[]={"PARENTAL_LEVEL","ATTRIBUTE","RESOLUTION","SOUND_FORMAT"};
        for (u32 k=0;k<4;++k) if (!strcmp(key,ints[k]) && (k==0 || hdd)) {
            if (fmt!=0x404 || len!=4) {ok=0;break;}
            vm_write32(gp+(u32)offsetof(CellGameDataSystemFileParam,parentalLevel)+k*4,gamedata_le32(value));
        }
    }
    free(d); return ok && category_ok ? 0 : CELL_GAMEDATA_ERROR_BROKEN;
}

static s32 gamedata_stat_callback(const char* tag, u32 version, uint32_t dir_ea,
                                  uint32_t func_opd, int hdd)
{
    if (version || !vm_base || !dir_ea || !func_opd ||
        (ppu_vm_size && (uint64_t)dir_ea+32 > ppu_vm_size))
        return hdd ? CELL_HDDGAME_ERROR_PARAM : CELL_GAMEDATA_ERROR_PARAM;
    const char* guest_dir=(const char*)vm_base+dir_ea;
    char dir[32];
    size_t n=0;
    while(n<32 && guest_dir[n]) {
        unsigned char c=(unsigned char)guest_dir[n]; dir[n++]=(char)c;
        if (!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-'))
            return hdd ? CELL_HDDGAME_ERROR_PARAM : CELL_GAMEDATA_ERROR_PARAM;
    }
    if (!n || n==32 || (hdd && n!=9)) return hdd ? CELL_HDDGAME_ERROR_PARAM : CELL_GAMEDATA_ERROR_PARAM;
    dir[n]=0;
    printf("[cellGame] %s(version=%u dir='%s' funcStat=0x%08X)\n",tag,version,dir,func_opd);
    if (!g_ps3_guest_caller) {
        ps3_strict_boot_guard("gamedata-missing-callback-dispatch", "operation=%s opd=0x%08X",tag,func_opd);
        ps3_strict_fs_unimplemented(tag,"<missing callback dispatcher>");
        return hdd ? CELL_HDDGAME_ERROR_CBRESULT : CELL_GAMEDATA_ERROR_CBRESULT;
    }
    if (atomic_flag_test_and_set(&gamedata_in_callback)) {
        ps3_strict_boot_guard("gamedata-concurrent-callback", "operation=%s opd=0x%08X",tag,func_opd);
        return hdd ? CELL_HDDGAME_ERROR_CBRESULT : CELL_GAMEDATA_ERROR_CBRESULT;
    }
    uint32_t cb=GAMEDATA_CB_BASE, get=cb+0x20;
    uint32_t set=get+(u32)sizeof(CellGameDataStatGet), param=set+(u32)sizeof(CellGameDataStatSet);
    uint32_t end=param+(u32)sizeof(CellGameDataSystemFileParam);
    s32 status=0;
    if (ppu_vm_size && end>ppu_vm_size) {status=CELL_GAMEDATA_ERROR_ACCESS_ERROR;goto done;}
    memset(vm_base+cb,0,end-cb);
    char path[1024];
    int written=snprintf(path,sizeof path,"%s/%s/PARAM.SFO",content_root(),dir);
    if (written<0 || written>=(int)sizeof path) {status=CELL_GAMEDATA_ERROR_PARAM;goto done;}
    u32 is_new=0;
    status=gamedata_read_param(path,get+(u32)offsetof(CellGameDataStatGet,getParam),&is_new,hdd);
    if (status) goto done;
    vm_write32(get+(u32)offsetof(CellGameDataStatGet,hddFreeSizeKB),(u32)content_free_kb());
    vm_write32(get+(u32)offsetof(CellGameDataStatGet,isNewData),is_new);
    snprintf((char*)vm_base+get+offsetof(CellGameDataStatGet,contentInfoPath),1055,"/dev_hdd0/game/%s",dir);
    snprintf((char*)vm_base+get+offsetof(CellGameDataStatGet,gameDataPath),1055,"/dev_hdd0/game/%s/USRDIR",dir);
    vm_write32(get+(u32)offsetof(CellGameDataStatGet,sizeKB),UINT32_MAX);
    if (hdd) vm_write32(set,param);
    g_ps3_guest_caller(func_opd,cb,get,set,0,0,0,0,0);
    s32 result=(s32)vm_read32(cb);
    printf("[cellGame] %s: isNewData=%u funcStat result=%d setParam=0x%08X\n",tag,is_new,result,vm_read32(set));
    if (result==1) {status=hdd ? 1 : 0;goto done;}
    if (result!=0) {status=CELL_GAMEDATA_ERROR_CBRESULT;goto done;}
    if (is_new && !vm_read32(set)) {status=CELL_GAMEDATA_ERROR_PARAM;goto done;}
    if (is_new || vm_read32(set)) {
        ps3_strict_boot_guard("gamedata-persistence-unimplemented","operation=%s path=%s opd=0x%08X new=%u setParam=0x%08X",tag,path,func_opd,is_new,vm_read32(set));
        ps3_strict_fs_unimplemented(tag,path);
        status=CELL_GAMEDATA_ERROR_ACCESS_ERROR;
    }
 done:
    atomic_flag_clear(&gamedata_in_callback);
    if (hdd && ((u32)status&0xffffff00u)==0x8002b600u) status=(s32)((u32)status+0x400u);
    return status;
}

s32 cellGameDataCheckCreate2(u32 version, const char* dirName, u32 errDialog,
                             void* funcStat, u32 container)
{
    (void)errDialog;(void)container;
    return gamedata_stat_callback("DataCheckCreate2",version,(uint32_t)(uintptr_t)dirName,(uint32_t)(uintptr_t)funcStat,0);
}
s32 cellGameDataCheckCreate(u32 version, const char* dirName, u32 errDialog,
                            void* funcStat, u32 container)
{
    return cellGameDataCheckCreate2(version,dirName,errDialog,funcStat,container);
}
s32 cellHddGameCheck(u32 version, const char* dirName, u32 errDialog,
                     void* funcStat, u32 container)
{
    (void)errDialog;(void)container;
    return gamedata_stat_callback("HddGameCheck",version,(uint32_t)(uintptr_t)dirName,(uint32_t)(uintptr_t)funcStat,1);
}
/* The write side of GetParamString: a title that has just created its game data
 * fills PARAM.SFO in before writing it out. Twisted Metal only reaches this at
 * all once cellGameCreateGameData works, which is why the NID sat unresolved
 * until sage's cellGame work landed -- the regression gate caught it as
 * UNRESOLVED 0xDAA5CD20 the first time the two were built together.
 *
 * Stores into the same statics GetParamString reads, so Create -> Set -> Get
 * returns what the title wrote. ponytail: in memory only, not persisted to a
 * PARAM.SFO on disk. Add that when a title is found that writes the data, exits,
 * and expects to read it back on the next run. */
s32 cellGameSetParamString(s32 id, const char* buf)
{
    /* `buf` is a GUEST address (raw r4), like GetParamString's -- a host
     * dereference here faults. See [[guest-pointer-abi]]. */
    uint32_t buf_ea = (uint32_t)(uintptr_t)buf;
    if (!buf_ea)
        return CELL_GAME_ERROR_PARAM;
    const char* src = (const char*)(vm_base + buf_ea);

    char*  dst;
    size_t cap;
    if (id >= CELL_GAME_PARAMID_TITLE && id <= CELL_GAME_PARAMID_TITLE_TURKISH) {
        dst = s_title;      cap = sizeof s_title;      /* localised TITLE_## all land here */
    } else {
        switch (id) {
        case CELL_GAME_PARAMID_TITLE_ID:       dst = s_title_id;   cap = sizeof s_title_id;   break;
        case CELL_GAME_PARAMID_VERSION:        dst = s_version;    cap = sizeof s_version;    break;
        case CELL_GAME_PARAMID_PS3_SYSTEM_VER: dst = s_system_ver; cap = sizeof s_system_ver; break;
        case CELL_GAME_PARAMID_APP_VER:        dst = s_app_ver;    cap = sizeof s_app_ver;    break;
        default:
            printf("[cellGame] SetParamString: unknown param id %d (ignored)\n", id);
            return CELL_OK;
        }
    }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
    printf("[cellGame] SetParamString(id=%d) = \"%s\"\n", id, dst);
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

    const char* src;
    if (id >= CELL_GAME_PARAMID_TITLE && id <= CELL_GAME_PARAMID_TITLE_TURKISH) {
        /* Localized TITLE_## fields are not tracked separately yet. */
        src = s_title;
    } else {
        switch (id) {
        case CELL_GAME_PARAMID_TITLE_ID:       src = s_title_id;   break;
        case CELL_GAME_PARAMID_VERSION:        src = s_version;    break;
        case CELL_GAME_PARAMID_PS3_SYSTEM_VER: src = s_system_ver; break;
        case CELL_GAME_PARAMID_APP_VER:        src = s_app_ver;    break;
        default:
            printf("[cellGame] WARNING: unknown param string id %d\n", id);
            src = "";
            break;
        }
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

    /* Create the directory the title ASKED for, not s_title_id.
     *
     * The install handshake is: cellGameDataCheck(type=GAMEDATA, dirName) ->
     * CELL_GAME_RET_NONE ("no data yet") -> the title calls
     * cellGameCreateGameData -> it checks again and expects to find it. This
     * created <content>/s_title_id instead, and s_title_id is the placeholder
     * "BLES00000" unless a PARAM.SFO set it -- so DataCheck kept looking for
     * <content>/NPUA80523, never found it, and Tokyo Jungle's installer span
     * DataCheck/CreateGameData forever.
     *
     * s_check_dir is the dirName from the check session that immediately
     * precedes this call, which is exactly what firmware creates. */
    const char* create_dir = s_check_dir[0] ? s_check_dir : s_title_id;

    char path[CELL_GAME_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", content_root(), create_dir);

    ensure_dirs(path);

    /* Also create USRDIR */
    char usrdir[CELL_GAME_PATH_MAX];
    snprintf(usrdir, sizeof(usrdir), "%s/USRDIR", path);
    ensure_dirs(usrdir);

    /* Host paths are only for host filesystem access. Guest code feeds these
     * results back to cellFs, which resolves PS3 virtual paths through VFS. */
    uint32_t cip_ea = (uint32_t)(uintptr_t)tmp_contentInfoPath;
    uint32_t usr_ea = (uint32_t)(uintptr_t)tmp_usrdirPath;
    char guest_path[CELL_GAME_PATH_MAX];
    if (cip_ea) {
        snprintf(guest_path, sizeof(guest_path), "/dev_hdd0/game/%s", create_dir);
        memcpy(vm_base + cip_ea, guest_path, strlen(guest_path) + 1);
    }
    if (usr_ea) {
        snprintf(guest_path, sizeof(guest_path), "/dev_hdd0/game/%s/USRDIR", create_dir);
        memcpy(vm_base + usr_ea, guest_path, strlen(guest_path) + 1);
    }

    return CELL_OK;
}

s32 cellGameDeleteGameData(const char* dirName)
{
    ps3_strict_fs_unimplemented("cellGameDeleteGameData", "<guest game-data directory>");
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

    snprintf(GUEST_PTR(path, char*), CELL_GAME_PATH_MAX,
             "/dev_hdd0/game/%s/USRDIR/web", s_title_id);

    return CELL_OK;
}

/* cellGameContentErrorDialog -- firmware puts a system error message on screen
 * ("the game data is corrupted", "not enough space", ...) and, for the 1xx
 * types, the title exits as soon as it returns.
 *
 * It has to exist even as a stub, and it has to SAY what it was asked to show.
 * LBP calls this as the very last thing its "bringup" thread does before
 * sys_ppu_thread_exit(0): the thread's result feeds main's startup gate, main
 * sees a false, runs its teardown and calls exit(0). Unimplemented, the whole
 * sequence reads as a clean voluntary shutdown with no cause anywhere in the
 * log -- the title looks like it simply finished. The dialog type and dirName
 * are the only place the actual complaint is named, so print them.
 *
 * Returning CELL_OK is correct: firmware returns success for "the dialog was
 * shown". It does not mean the underlying content problem went away. */
s32 cellGameContentErrorDialog(s32 type, s32 errNeedSizeKB, const char* dirName)
{
    /* dirName is a GUEST address; translate before use (same as DataCheck). */
    uint32_t dir_ea = (uint32_t)(uintptr_t)dirName;
    const char* dir = dir_ea ? (const char*)(vm_base + dir_ea) : "<null>";

    const char* what;
    switch (type) {
        case 0:   what = "BROKEN_GAMEDATA";           break;
        case 1:   what = "BROKEN_HDDGAME";            break;
        case 2:   what = "NOSPACE";                   break;
        case 100: what = "BROKEN_EXIT_GAMEDATA";      break;
        case 101: what = "BROKEN_EXIT_HDDGAME";       break;
        case 102: what = "NOSPACE_EXIT";              break;
        default:  what = "<unknown type>";            break;
    }

    printf("[cellGame] *** ContentErrorDialog: %s (type=%d) dir='%s' needSizeKB=%d\n",
           what, type, dir, errNeedSizeKB);
    if (type >= 100)
        printf("[cellGame] *** this is an EXIT dialog -- the title quits after this\n");
    fflush(stdout);

    return CELL_OK;
}
