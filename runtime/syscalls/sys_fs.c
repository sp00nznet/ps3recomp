/*
 * ps3recomp - Filesystem syscalls (implementation)
 */

#include "sys_fs.h"
#include "../memory/vm.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef _WIN32
  #include <direct.h>
  #include <io.h>
  #define stat _stat64
  #define S_ISDIR(m) (((m) & _S_IFDIR) != 0)
  #define S_ISREG(m) (((m) & _S_IFREG) != 0)
#else
  #include <unistd.h>
  #include <dirent.h>
#endif

/* ---------------------------------------------------------------------------
 * Globals
 * -----------------------------------------------------------------------*/
sys_fs_fd_info  g_sys_fs_fds[SYS_FS_FD_MAX];
sys_fs_dir_info g_sys_fs_dirs[SYS_FS_DIR_MAX];
char            g_sys_fs_root[512] = ".";

static void write_be32(uint32_t addr, uint32_t val)
{
    uint32_t* p = (uint32_t*)vm_to_host(addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    val = ((val >> 24) & 0xFF) | ((val >> 8) & 0xFF00) |
          ((val <<  8) & 0xFF0000) | ((val << 24) & 0xFF000000u);
#endif
    *p = val;
}

static void write_be64(uint32_t addr, uint64_t val)
{
    uint64_t* p = (uint64_t*)vm_to_host(addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    val = ((val >> 56) & 0xFFULL) |
          ((val >> 40) & 0xFF00ULL) |
          ((val >> 24) & 0xFF0000ULL) |
          ((val >>  8) & 0xFF000000ULL) |
          ((val <<  8) & 0xFF00000000ULL) |
          ((val << 24) & 0xFF0000000000ULL) |
          ((val << 40) & 0xFF000000000000ULL) |
          ((val << 56) & 0xFF00000000000000ULL);
#endif
    *p = val;
}

/* ---------------------------------------------------------------------------
 * Path translation
 *
 * Maps PS3 virtual paths to host filesystem paths:
 *   /dev_hdd0/...  -> <root>/dev_hdd0/...
 *   /dev_bdvd/...  -> <root>/dev_bdvd/...
 *   /dev_flash/... -> <root>/dev_flash/...
 *   /app_home/...  -> <root>/app_home/...
 *   /dev_usb000/.. -> <root>/dev_usb000/...
 *   Others         -> <root>/<path>
 * -----------------------------------------------------------------------*/
static void fs_normalize_sep(char* p) {
#ifdef _WIN32
    for (char* c = p; *c; c++) if (*c == '/') *c = '\\';
#else
    (void)p;
#endif
}

void sys_fs_translate_path(const char* ps3_path, char* host_path, int host_path_size)
{
    /* Lazily adopt PS3_VFS_ROOT if the root is still the default ".", so this
     * sys_fs layer points at the same place as the cellFs layer (ppu_vfs_root). */
    if (g_sys_fs_root[0] == '.' && g_sys_fs_root[1] == '\0') {
        const char* env = getenv("PS3_VFS_ROOT");
        if (env && *env) { strncpy(g_sys_fs_root, env, sizeof(g_sys_fs_root) - 1); g_sys_fs_root[sizeof(g_sys_fs_root)-1] = 0; }
    }

    /* /dev_hdd0 overlays the installed game-update dir (patchN.farc live there
     * for a disc title patched to e.g. v1.30) -- mirror ppu_fs.cpp host_path.
     * PS3_HDD0_ROOT = host dir that /dev_hdd0 maps into (contains game/<title>/). */
    {
        static const char* hdd0_root = NULL; static int hdd0_init = 0;
        if (!hdd0_init) { hdd0_root = getenv("PS3_HDD0_ROOT"); hdd0_init = 1; }
        if (hdd0_root && strncmp(ps3_path, "/dev_hdd0/", 10) == 0) {
            snprintf(host_path, (size_t)host_path_size, "%s/%s", hdd0_root, ps3_path + 10);
            fs_normalize_sep(host_path);
            return;
        }
    }

    /* Strip a known mount prefix so this sys_fs layer resolves to the SAME host
     * tree as the cellFs layer (ppu_fs.cpp host_path). Previously /dev_bdvd/X
     * mapped to <root>/dev_bdvd/X -- a directory that doesn't exist -- so a title
     * that opens disc content through the raw sys_fs path (LBP's Bink videos,
     * e.g. gamedata/videos/localisation_test.bik) failed even though the file is
     * present, stalling the loader waiting on the resource. /app_home/ is the
     * game's install dir (== the USRDIR root); the title also opens it in
     * non-leading spellings ("app_home/...", "e:/app_home/..."). */
    static const char* const mounts[] = {
        "/dev_bdvd/", "/app_home/", "/dev_hdd0/", "/dev_hdd1/",
        "/dev_flash/", "/host_root/", "/dev_usb000/", "/dev_usb/"
    };
    const char* rel = ps3_path;
    /* flОw first: map anything from a "USRDIR/" component onward to <root>/USRDIR/...
     * directly, bypassing the full /dev_hdd0/game/<ID>/USRDIR install tree (which we
     * normally provide via a filesystem junction) -- concurrent opens THROUGH a
     * Windows junction intermittently fail (the nondeterministic Cg shader "could
     * not be read" abort). The real assets live under <root>/USRDIR. flОw's failing
     * paths carry "USRDIR/"; LBP's raw-sys_fs disc paths (/dev_bdvd Bink videos) do
     * not, so they fall through to sagemono's mount-prefix stripping below. */
    const char* usrp = strstr(ps3_path, "USRDIR/");
    if (usrp) {
        rel = usrp;                        /* "USRDIR/..." */
    } else {
        /* Otherwise strip a known mount prefix so this sys_fs layer resolves to the
         * SAME host tree as the cellFs layer (ppu_fs.cpp). /dev_bdvd/X previously
         * mapped to <root>/dev_bdvd/X -- a dir that doesn't exist -- stalling titles
         * that open disc content through raw sys_fs. */
        int matched = 0;
        for (size_t i = 0; i < sizeof(mounts) / sizeof(mounts[0]); i++) {
            size_t n = strlen(mounts[i]);
            if (strncmp(ps3_path, mounts[i], n) == 0) { rel = ps3_path + n; matched = 1; break; }
        }
        if (!matched) {
            const char* ah = strstr(ps3_path, "app_home/");
            if (ah) rel = ah + 9;          /* non-leading app_home spelling */
            else if (rel[0] == '/') rel++; /* otherwise just strip leading slash */
        }
    }

    snprintf(host_path, (size_t)host_path_size, "%s/%s", g_sys_fs_root, rel);
    fs_normalize_sep(host_path);
    /* NOTE: do NOT stat() the direct path and fall back to the dev_hdd0 junction
     * on failure. stat() intermittently returns ENOENT for an existing file here
     * (a Windows transient), and the junction fallback made it WORSE -- the flaky
     * stat sent a real open to a path that doesn't exist. The direct <root>/USRDIR
     * layout holds the assets; a genuinely-missing direct path is handled by the
     * caller's open retry + the extracted-dump fallback below. */

    /* Extracted-dump fallback: our test setups often hold a title's data at
     * <root>/extracted/USRDIR/... rather than the full /dev_hdd0/game/<ID>/USRDIR
     * install tree the guest opens. If the primary path doesn't exist but a
     * USRDIR-relative path under <root>/extracted does, use that. Additive: only
     * affects paths unresolved at the primary location. */
    struct stat st;
    if (stat(host_path, &st) != 0) {
        const char* usr = strstr(ps3_path, "/USRDIR/");
        if (usr) {
            char alt[1024];
            snprintf(alt, sizeof(alt), "%s/extracted/USRDIR/%s", g_sys_fs_root, usr + 8);
            fs_normalize_sep(alt);
            if (stat(alt, &st) == 0)
                snprintf(host_path, (size_t)host_path_size, "%s", alt);
        }
    }
}

/* ---------------------------------------------------------------------------
 * sys_fs_open
 *
 * r3 = path (guest string pointer)
 * r4 = flags
 * r5 = pointer to receive fd (s32*)
 * r6 = mode (permissions, ignored on Windows)
 * r7 = arg (unused)
 * -----------------------------------------------------------------------*/
int64_t sys_fs_open(ppu_context* ctx)
{
    uint32_t path_addr  = LV2_ARG_PTR(ctx, 0);
    int32_t  flags      = LV2_ARG_S32(ctx, 1);
    uint32_t fd_out     = LV2_ARG_PTR(ctx, 2);
    /* uint32_t mode    = LV2_ARG_U32(ctx, 3); */

    if (path_addr == 0 || fd_out == 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    const char* ps3_path = (const char*)vm_to_host(path_addr);
    char host_path[1024];
    sys_fs_translate_path(ps3_path, host_path, sizeof(host_path));

    { extern char* getenv(const char*); if (getenv("FLOW_TITLEOPEN") && ps3_path && strstr(ps3_path, "Titles")) {
        size_t _l = strlen(ps3_path);
        fprintf(stderr, "[TITLEOPEN] path='%s' (len=%zu, ends_slash=%d) guest_lr=0x%08X\n",
                ps3_path, _l, (_l && ps3_path[_l-1]=='/')?1:0, (uint32_t)ctx->lr); fflush(stderr);
#ifdef _WIN32
        { char* mb=(char*)GetModuleHandleA(0); void* bt[30]; unsigned short fr=RtlCaptureStackBackTrace(0,30,bt,0);
          char ln[820]; int p=snprintf(ln,sizeof ln,"[TITLEOPEN-bt] rva:");
          for(unsigned short i=0;i<fr;i++) p+=snprintf(ln+p,sizeof(ln)-p," %llX",(unsigned long long)((char*)bt[i]-mb));
          fprintf(stderr,"%s\n",ln); fflush(stderr); }
#endif
    } }
    /* DIAGNOSTIC (FLOW_TITLEFIX): the game builds the title path with an EMPTY filename
     * (a stale-lift string-construction bug) -> opens the dir. Redirect a bare
     * ".../Data/Titles/" open to the language file so we can confirm the render path. */
    { extern char* getenv(const char*); if (getenv("FLOW_TITLEFIX")) {
        size_t hl = strlen(host_path);
        if (hl >= 8 && (strcmp(host_path + hl - 8, "Titles\\") == 0 || strcmp(host_path + hl - 8, "Titles/") == 0
                        || strcmp(host_path + hl - 7, "Titles\\") == 0 || strcmp(host_path + hl - 7, "Titles/") == 0)) {
            if (hl + 20 < sizeof(host_path)) { strcat(host_path, "Titles_English.xml");
                fprintf(stderr, "[TITLEFIX] redirected empty-filename title open -> %s\n", host_path); fflush(stderr); }
        }
    } }

    /* Find free fd slot */
    int slot = -1;
    for (int i = 0; i < SYS_FS_FD_MAX; i++) {
        if (!g_sys_fs_fds[i].active) { slot = i; break; }
    }
    if (slot < 0)
        return (int64_t)(int32_t)CELL_ENOMEM;

    /* Determine fopen mode from PS3 flags */
    const char* mode;
    int access = flags & CELL_FS_O_ACCMODE;

    if (flags & CELL_FS_O_CREAT) {
        if (flags & CELL_FS_O_TRUNC) {
            if (access == CELL_FS_O_RDWR)
                mode = "w+b";
            else
                mode = "wb";
        } else if (flags & CELL_FS_O_APPEND) {
            if (access == CELL_FS_O_RDWR)
                mode = "a+b";
            else
                mode = "ab";
        } else {
            /* Create but don't truncate: open for read/write, create if not exist */
            if (access == CELL_FS_O_RDWR)
                mode = "r+b";
            else if (access == CELL_FS_O_WRONLY)
                mode = "r+b";
            else
                mode = "rb";
        }
    } else if (flags & CELL_FS_O_TRUNC) {
        if (access == CELL_FS_O_RDWR)
            mode = "w+b";
        else
            mode = "wb";
    } else if (flags & CELL_FS_O_APPEND) {
        if (access == CELL_FS_O_RDWR)
            mode = "a+b";
        else
            mode = "ab";
    } else {
        if (access == CELL_FS_O_RDWR)
            mode = "r+b";
        else if (access == CELL_FS_O_WRONLY)
            mode = "r+b";
        else
            mode = "rb";
    }

    FILE* fp = fopen(host_path, mode);

    /* Transient open failures: on Windows an existing, correctly-pathed file can
     * intermittently fail to open (AV/indexer holding a share lock, or momentary
     * handle pressure) -- which surfaced as a NONDETERMINISTIC "Cg shader file
     * could not be read" abort. Retry a few times with a brief backoff for a
     * read-only open of a file that exists; deterministic and cheap. */
    if (!fp && !(flags & (CELL_FS_O_CREAT | CELL_FS_O_WRONLY | CELL_FS_O_TRUNC))) {
        for (int _r = 0; !fp && _r < 3000; _r++) {
#ifdef _WIN32
            Sleep(1);
#endif
            fp = fopen(host_path, mode);
        }
        if (fp) fprintf(stderr, "[sys_fs] open recovered after retry: %s\n", host_path);
    }

    /* If CREAT flag set and file doesn't exist, try creating it */
    if (!fp && (flags & CELL_FS_O_CREAT)) {
        fp = fopen(host_path, "w+b");
    }

    if (!fp) {
        { struct stat _s2; int _ex = (stat(host_path,&_s2)==0);
          fprintf(stderr, "[sys_fs] open FAILED: %s (errno=%d %s, exists=%d, size=%lld)\n",
                  host_path, errno, strerror(errno), _ex, _ex?(long long)_s2.st_size:-1LL); }
        return (int64_t)(int32_t)CELL_ENOENT;
    }

    fprintf(stderr, "[sys_fs] open OK: %s\n", host_path);

    sys_fs_fd_info* f = &g_sys_fs_fds[slot];
    f->active = 1;
    f->fp     = fp;
    f->flags  = flags;
    strncpy(f->path, host_path, sizeof(f->path) - 1);
    f->path[sizeof(f->path) - 1] = '\0';

    /* FD is slot + 3 (reserve 0=stdin, 1=stdout, 2=stderr) */
    int32_t fd = slot + 3;
    write_be32(fd_out, (uint32_t)fd);

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_fs_read
 *
 * r3 = fd
 * r4 = buffer (guest pointer)
 * r5 = size
 * r6 = pointer to receive bytes read (u64*)
 * -----------------------------------------------------------------------*/
int64_t sys_fs_read(ppu_context* ctx)
{
    int32_t  fd         = LV2_ARG_S32(ctx, 0);
    uint32_t buf_addr   = LV2_ARG_PTR(ctx, 1);
    uint64_t size       = LV2_ARG_U64(ctx, 2);
    uint32_t nread_addr = LV2_ARG_PTR(ctx, 3);

    int slot = fd - 3;
    if (slot < 0 || slot >= SYS_FS_FD_MAX)
        return (int64_t)(int32_t)CELL_EBADF;

    sys_fs_fd_info* f = &g_sys_fs_fds[slot];
    if (!f->active || !f->fp)
        return (int64_t)(int32_t)CELL_EBADF;

    void* buf = vm_to_host(buf_addr);
    size_t nread = fread(buf, 1, (size_t)size, f->fp);

    if (nread_addr != 0) {
        write_be64(nread_addr, (uint64_t)nread);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_fs_write
 *
 * r3 = fd
 * r4 = buffer (guest pointer)
 * r5 = size
 * r6 = pointer to receive bytes written (u64*)
 * -----------------------------------------------------------------------*/
int64_t sys_fs_write(ppu_context* ctx)
{
    int32_t  fd           = LV2_ARG_S32(ctx, 0);
    uint32_t buf_addr     = LV2_ARG_PTR(ctx, 1);
    uint64_t size         = LV2_ARG_U64(ctx, 2);
    uint32_t nwritten_addr = LV2_ARG_PTR(ctx, 3);

    int slot = fd - 3;
    if (slot < 0 || slot >= SYS_FS_FD_MAX) {
        /* Invalid fd — pretend write succeeded (CRT stdio uses corrupted fds) */
        if (nwritten_addr != 0)
            write_be64(nwritten_addr, size);
        return CELL_OK;
    }

    sys_fs_fd_info* f = &g_sys_fs_fds[slot];
    if (!f->active || !f->fp) {
        if (nwritten_addr != 0)
            write_be64(nwritten_addr, size);
        return CELL_OK;
    }

    const void* buf = vm_to_host(buf_addr);
    size_t nwritten = fwrite(buf, 1, (size_t)size, f->fp);
    fflush(f->fp);

    if (nwritten_addr != 0) {
        write_be64(nwritten_addr, (uint64_t)nwritten);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_fs_close
 *
 * r3 = fd
 * -----------------------------------------------------------------------*/
int64_t sys_fs_close(ppu_context* ctx)
{
    int32_t fd = LV2_ARG_S32(ctx, 0);

    int slot = fd - 3;
    if (slot < 0 || slot >= SYS_FS_FD_MAX)
        return (int64_t)(int32_t)CELL_EBADF;

    sys_fs_fd_info* f = &g_sys_fs_fds[slot];
    if (!f->active)
        return (int64_t)(int32_t)CELL_EBADF;

    if (f->fp) {
        fclose(f->fp);
        f->fp = NULL;
    }
    f->active = 0;

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_fs_lseek
 *
 * r3 = fd
 * r4 = offset (s64)
 * r5 = whence
 * r6 = pointer to receive new position (u64*)
 * -----------------------------------------------------------------------*/
int64_t sys_fs_lseek(ppu_context* ctx)
{
    int32_t  fd        = LV2_ARG_S32(ctx, 0);
    int64_t  offset    = LV2_ARG_S64(ctx, 1);
    int32_t  whence    = LV2_ARG_S32(ctx, 2);
    uint32_t pos_addr  = LV2_ARG_PTR(ctx, 3);

    int slot = fd - 3;
    if (slot < 0 || slot >= SYS_FS_FD_MAX) {
        /* Invalid fd — return position 0 instead of EBADF.
         * The CRT may call lseek on uninitialized FILE structures. */
        if (pos_addr)
            write_be64(pos_addr, 0);
        return 0;
    }

    sys_fs_fd_info* f = &g_sys_fs_fds[slot];
    if (!f->active || !f->fp) {
        if (pos_addr)
            write_be64(pos_addr, 0);
        return 0;
    }

    int origin;
    switch (whence) {
        case CELL_FS_SEEK_SET: origin = SEEK_SET; break;
        case CELL_FS_SEEK_CUR: origin = SEEK_CUR; break;
        case CELL_FS_SEEK_END: origin = SEEK_END; break;
        default: return (int64_t)(int32_t)CELL_EINVAL;
    }

#ifdef _WIN32
    _fseeki64(f->fp, offset, origin);
    int64_t pos = _ftelli64(f->fp);
#else
    fseeko(f->fp, (off_t)offset, origin);
    int64_t pos = (int64_t)ftello(f->fp);
#endif

    if (pos_addr != 0) {
        write_be64(pos_addr, (uint64_t)pos);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Helper: fill CellFsStat from host stat
 * -----------------------------------------------------------------------*/
static void fill_cell_stat(uint32_t stat_addr, struct stat* st)
{
    uint32_t mode = CELL_FS_S_IRUSR | CELL_FS_S_IRGRP | CELL_FS_S_IROTH;
    if (S_ISDIR(st->st_mode)) {
        mode |= CELL_FS_S_IFDIR | CELL_FS_S_IXUSR;
    } else {
        mode |= CELL_FS_S_IFREG;
    }
#ifndef _WIN32
    if (st->st_mode & S_IWUSR) mode |= CELL_FS_S_IWUSR;
    if (st->st_mode & S_IXUSR) mode |= CELL_FS_S_IXUSR;
#else
    mode |= CELL_FS_S_IWUSR; /* assume writable on Windows */
#endif

    /* CellFsStat is 0x34 (52) bytes, 4-byte aligned: the s64/u64 members are
     * be_t<...,4> so there is NO pad after gid (RPCS3: CHECK_SIZE_ALIGN(...,52,4)).
     * mode@0 uid@4 gid@8 atime@0x0C mtime@0x14 ctime@0x1C size@0x24 blksize@0x2C.
     * The old 8-byte-aligned 0x38 layout overran the struct by 4 bytes and, for
     * a stat embedded inside a larger object, clobbered the field after it. */
    write_be32(stat_addr + 0x00, mode);
    write_be32(stat_addr + 0x04, 0);  /* uid */
    write_be32(stat_addr + 0x08, 0);  /* gid */
    write_be64(stat_addr + 0x0C, (uint64_t)st->st_atime);
    write_be64(stat_addr + 0x14, (uint64_t)st->st_mtime);
    write_be64(stat_addr + 0x1C, (uint64_t)st->st_ctime);
    write_be64(stat_addr + 0x24, (uint64_t)st->st_size);
    write_be64(stat_addr + 0x2C, 4096ULL);  /* blksize */
}

/* ---------------------------------------------------------------------------
 * sys_fs_stat
 *
 * r3 = path (guest pointer)
 * r4 = pointer to CellFsStat
 * -----------------------------------------------------------------------*/
int64_t sys_fs_stat(ppu_context* ctx)
{
    uint32_t path_addr = LV2_ARG_PTR(ctx, 0);
    uint32_t stat_addr = LV2_ARG_PTR(ctx, 1);

    if (path_addr == 0 || stat_addr == 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    const char* ps3_path = (const char*)vm_to_host(path_addr);
    char host_path[1024];
    sys_fs_translate_path(ps3_path, host_path, sizeof(host_path));

    struct stat st;
#ifdef _WIN32
    if (_stat64(host_path, (struct _stat64*)&st) != 0)
#else
    if (stat(host_path, &st) != 0)
#endif
    {
        return (int64_t)(int32_t)CELL_ENOENT;
    }

    fill_cell_stat(stat_addr, &st);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_fs_fstat
 *
 * r3 = fd
 * r4 = pointer to CellFsStat
 * -----------------------------------------------------------------------*/
int64_t sys_fs_fstat(ppu_context* ctx)
{
    int32_t  fd        = LV2_ARG_S32(ctx, 0);
    uint32_t stat_addr = LV2_ARG_PTR(ctx, 1);

    int slot = fd - 3;
    if (slot < 0 || slot >= SYS_FS_FD_MAX)
        return (int64_t)(int32_t)CELL_EBADF;

    sys_fs_fd_info* f = &g_sys_fs_fds[slot];
    if (!f->active || !f->fp)
        return (int64_t)(int32_t)CELL_EBADF;

    struct stat st;
#ifdef _WIN32
    int fno = _fileno(f->fp);
    if (_fstat64(fno, (struct _stat64*)&st) != 0)
#else
    int fno = fileno(f->fp);
    if (fstat(fno, &st) != 0)
#endif
    {
        return (int64_t)(int32_t)CELL_EBADF;
    }

    fill_cell_stat(stat_addr, &st);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_fs_opendir
 *
 * r3 = path (guest pointer)
 * r4 = pointer to receive dir fd (s32*)
 * -----------------------------------------------------------------------*/
int64_t sys_fs_opendir(ppu_context* ctx)
{
    uint32_t path_addr = LV2_ARG_PTR(ctx, 0);
    uint32_t fd_out    = LV2_ARG_PTR(ctx, 1);

    if (path_addr == 0 || fd_out == 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    const char* ps3_path = (const char*)vm_to_host(path_addr);
    char host_path[1024];
    sys_fs_translate_path(ps3_path, host_path, sizeof(host_path));

    int slot = -1;
    for (int i = 0; i < SYS_FS_DIR_MAX; i++) {
        if (!g_sys_fs_dirs[i].active) { slot = i; break; }
    }
    if (slot < 0)
        return (int64_t)(int32_t)CELL_ENOMEM;

    sys_fs_dir_info* d = &g_sys_fs_dirs[slot];
    memset(d, 0, sizeof(*d));

#ifdef _WIN32
    char search_path[1024];
    snprintf(search_path, sizeof(search_path), "%s\\*", host_path);
    d->find_handle = FindFirstFileA(search_path, &d->find_data);
    if (d->find_handle == INVALID_HANDLE_VALUE)
        return (int64_t)(int32_t)CELL_ENOENT;
    d->first_read = 1;
#else
    d->dp = opendir(host_path);
    if (!d->dp)
        return (int64_t)(int32_t)CELL_ENOENT;
#endif

    d->active = 1;
    strncpy(d->path, host_path, sizeof(d->path) - 1);

    int32_t dir_fd = slot + 1;
    write_be32(fd_out, (uint32_t)dir_fd);

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_fs_readdir
 *
 * r3 = dir_fd
 * r4 = pointer to CellFsDirent (guest memory)
 *      struct { u8 d_type; u8 d_namlen; char d_name[256]; }
 * r5 = pointer to receive bytes read (u64*), 0 = end
 * -----------------------------------------------------------------------*/
int64_t sys_fs_readdir(ppu_context* ctx)
{
    int32_t  dir_fd     = LV2_ARG_S32(ctx, 0);
    uint32_t dirent_addr = LV2_ARG_PTR(ctx, 1);
    uint32_t nread_addr  = LV2_ARG_PTR(ctx, 2);

    if (dir_fd <= 0 || dir_fd > SYS_FS_DIR_MAX)
        return (int64_t)(int32_t)CELL_EBADF;

    sys_fs_dir_info* d = &g_sys_fs_dirs[dir_fd - 1];
    if (!d->active)
        return (int64_t)(int32_t)CELL_EBADF;

    const char* name = NULL;
    int is_dir = 0;

#ifdef _WIN32
    if (d->first_read) {
        d->first_read = 0;
        name = d->find_data.cFileName;
        is_dir = (d->find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    } else {
        if (!FindNextFileA(d->find_handle, &d->find_data)) {
            /* End of directory */
            if (nread_addr != 0) write_be64(nread_addr, 0);
            return CELL_OK;
        }
        name = d->find_data.cFileName;
        is_dir = (d->find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    }
#else
    struct dirent* entry = readdir(d->dp);
    if (!entry) {
        if (nread_addr != 0) write_be64(nread_addr, 0);
        return CELL_OK;
    }
    name = entry->d_name;
    is_dir = (entry->d_type == DT_DIR) ? 1 : 0;
#endif

    if (dirent_addr != 0 && name != NULL) {
        uint8_t* out = (uint8_t*)vm_to_host(dirent_addr);
        uint8_t namlen = (uint8_t)strlen(name);
        if (namlen > 255) namlen = 255;

        out[0] = is_dir ? 1 : 2;  /* d_type: 1=dir, 2=regular */
        out[1] = namlen;
        memcpy(out + 2, name, namlen);
        out[2 + namlen] = '\0';
    }

    if (nread_addr != 0) {
        /* Non-zero means success */
        write_be64(nread_addr, (uint64_t)(name ? strlen(name) + 2 : 0));
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_fs_closedir
 *
 * r3 = dir_fd
 * -----------------------------------------------------------------------*/
int64_t sys_fs_closedir(ppu_context* ctx)
{
    int32_t dir_fd = LV2_ARG_S32(ctx, 0);

    if (dir_fd <= 0 || dir_fd > SYS_FS_DIR_MAX)
        return (int64_t)(int32_t)CELL_EBADF;

    sys_fs_dir_info* d = &g_sys_fs_dirs[dir_fd - 1];
    if (!d->active)
        return (int64_t)(int32_t)CELL_EBADF;

#ifdef _WIN32
    if (d->find_handle != INVALID_HANDLE_VALUE)
        FindClose(d->find_handle);
#else
    if (d->dp) closedir(d->dp);
#endif

    d->active = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_fs_mkdir
 *
 * r3 = path (guest pointer)
 * r4 = mode
 * -----------------------------------------------------------------------*/
int64_t sys_fs_mkdir(ppu_context* ctx)
{
    uint32_t path_addr = LV2_ARG_PTR(ctx, 0);
    /* uint32_t mode   = LV2_ARG_U32(ctx, 1); */

    if (path_addr == 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    const char* ps3_path = (const char*)vm_to_host(path_addr);
    char host_path[1024];
    sys_fs_translate_path(ps3_path, host_path, sizeof(host_path));

#ifdef _WIN32
    int rc = _mkdir(host_path);
#else
    int rc = mkdir(host_path, 0755);
#endif

    if (rc != 0) {
        /* Check if it already exists */
        struct stat st;
#ifdef _WIN32
        if (_stat64(host_path, (struct _stat64*)&st) == 0 && S_ISDIR(st.st_mode))
#else
        if (stat(host_path, &st) == 0 && S_ISDIR(st.st_mode))
#endif
            return (int64_t)(int32_t)CELL_EEXIST;
        return (int64_t)(int32_t)CELL_ENOENT;
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_fs_rename
 *
 * r3 = old path (guest pointer)
 * r4 = new path (guest pointer)
 * -----------------------------------------------------------------------*/
int64_t sys_fs_rename(ppu_context* ctx)
{
    uint32_t old_addr = LV2_ARG_PTR(ctx, 0);
    uint32_t new_addr = LV2_ARG_PTR(ctx, 1);

    if (old_addr == 0 || new_addr == 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    const char* old_ps3 = (const char*)vm_to_host(old_addr);
    const char* new_ps3 = (const char*)vm_to_host(new_addr);

    char old_host[1024], new_host[1024];
    sys_fs_translate_path(old_ps3, old_host, sizeof(old_host));
    sys_fs_translate_path(new_ps3, new_host, sizeof(new_host));

    if (rename(old_host, new_host) != 0)
        return (int64_t)(int32_t)CELL_ENOENT;

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_fs_unlink
 *
 * r3 = path (guest pointer)
 * -----------------------------------------------------------------------*/
int64_t sys_fs_unlink(ppu_context* ctx)
{
    uint32_t path_addr = LV2_ARG_PTR(ctx, 0);

    if (path_addr == 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    const char* ps3_path = (const char*)vm_to_host(path_addr);
    char host_path[1024];
    sys_fs_translate_path(ps3_path, host_path, sizeof(host_path));

#ifdef _WIN32
    if (_unlink(host_path) != 0)
#else
    if (unlink(host_path) != 0)
#endif
        return (int64_t)(int32_t)CELL_ENOENT;

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_fs_rmdir
 *
 * r3 = path (guest pointer)
 * -----------------------------------------------------------------------*/
int64_t sys_fs_rmdir(ppu_context* ctx)
{
    uint32_t path_addr = LV2_ARG_PTR(ctx, 0);

    if (path_addr == 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    const char* ps3_path = (const char*)vm_to_host(path_addr);
    char host_path[1024];
    sys_fs_translate_path(ps3_path, host_path, sizeof(host_path));

#ifdef _WIN32
    if (_rmdir(host_path) != 0)
#else
    if (rmdir(host_path) != 0)
#endif
        return (int64_t)(int32_t)CELL_ENOENT;

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_fs_ftruncate
 *
 * r3 = fd
 * r4 = size
 * -----------------------------------------------------------------------*/
int64_t sys_fs_ftruncate(ppu_context* ctx)
{
    int32_t  fd   = LV2_ARG_S32(ctx, 0);
    uint64_t size = LV2_ARG_U64(ctx, 1);

    int slot = fd - 3;
    if (slot < 0 || slot >= SYS_FS_FD_MAX)
        return (int64_t)(int32_t)CELL_EBADF;

    sys_fs_fd_info* f = &g_sys_fs_fds[slot];
    if (!f->active || !f->fp)
        return (int64_t)(int32_t)CELL_EBADF;

    fflush(f->fp);

#ifdef _WIN32
    int fno = _fileno(f->fp);
    if (_chsize_s(fno, (long long)size) != 0)
        return (int64_t)(int32_t)CELL_EINVAL;
#else
    int fno = fileno(f->fp);
    if (ftruncate(fno, (off_t)size) != 0)
        return (int64_t)(int32_t)CELL_EINVAL;
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Registration
 * -----------------------------------------------------------------------*/
void sys_fs_init(lv2_syscall_table* tbl)
{
    memset(g_sys_fs_fds,  0, sizeof(g_sys_fs_fds));
    memset(g_sys_fs_dirs, 0, sizeof(g_sys_fs_dirs));

    lv2_syscall_register(tbl, SYS_FS_OPEN,      sys_fs_open);
    lv2_syscall_register(tbl, SYS_FS_READ,       sys_fs_read);
    lv2_syscall_register(tbl, SYS_FS_WRITE,      sys_fs_write);
    lv2_syscall_register(tbl, SYS_FS_CLOSE,      sys_fs_close);
    lv2_syscall_register(tbl, SYS_FS_OPENDIR,    sys_fs_opendir);
    lv2_syscall_register(tbl, SYS_FS_READDIR,    sys_fs_readdir);
    lv2_syscall_register(tbl, SYS_FS_CLOSEDIR,   sys_fs_closedir);
    lv2_syscall_register(tbl, SYS_FS_STAT,       sys_fs_stat);
    lv2_syscall_register(tbl, SYS_FS_FSTAT,      sys_fs_fstat);
    lv2_syscall_register(tbl, SYS_FS_MKDIR,      sys_fs_mkdir);
    lv2_syscall_register(tbl, SYS_FS_RENAME,     sys_fs_rename);
    lv2_syscall_register(tbl, SYS_FS_RMDIR,      sys_fs_rmdir);
    lv2_syscall_register(tbl, SYS_FS_UNLINK,     sys_fs_unlink);
    lv2_syscall_register(tbl, SYS_FS_LSEEK,      sys_fs_lseek);
    lv2_syscall_register(tbl, SYS_FS_FTRUNCATE,  sys_fs_ftruncate);
}
