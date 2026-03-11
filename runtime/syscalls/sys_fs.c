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
void sys_fs_translate_path(const char* ps3_path, char* host_path, int host_path_size)
{
    /* Strip leading slash for concatenation */
    const char* rel = ps3_path;
    if (rel[0] == '/') rel++;

    snprintf(host_path, (size_t)host_path_size, "%s/%s", g_sys_fs_root, rel);

    /* Normalize separators for Windows */
#ifdef _WIN32
    for (char* c = host_path; *c; c++) {
        if (*c == '/') *c = '\\';
    }
#endif
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

    /* If CREAT flag set and file doesn't exist, try creating it */
    if (!fp && (flags & CELL_FS_O_CREAT)) {
        fp = fopen(host_path, "w+b");
    }

    if (!fp)
        return (int64_t)(int32_t)CELL_ENOENT;

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
    if (slot < 0 || slot >= SYS_FS_FD_MAX)
        return (int64_t)(int32_t)CELL_EBADF;

    sys_fs_fd_info* f = &g_sys_fs_fds[slot];
    if (!f->active || !f->fp)
        return (int64_t)(int32_t)CELL_EBADF;

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
    if (slot < 0 || slot >= SYS_FS_FD_MAX)
        return (int64_t)(int32_t)CELL_EBADF;

    sys_fs_fd_info* f = &g_sys_fs_fds[slot];
    if (!f->active || !f->fp)
        return (int64_t)(int32_t)CELL_EBADF;

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

    write_be32(stat_addr + 0,  mode);
    write_be32(stat_addr + 4,  0);  /* uid */
    write_be32(stat_addr + 8,  0);  /* gid */
    write_be32(stat_addr + 12, 0);  /* pad */
    write_be64(stat_addr + 16, (uint64_t)st->st_atime);
    write_be64(stat_addr + 24, (uint64_t)st->st_mtime);
    write_be64(stat_addr + 32, (uint64_t)st->st_ctime);
    write_be64(stat_addr + 40, (uint64_t)st->st_size);
    write_be64(stat_addr + 48, 4096ULL);  /* blksize */
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
