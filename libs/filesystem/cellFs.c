/*
 * ps3recomp - cellFs HLE stub implementation
 *
 * Maps PS3 virtual filesystem paths (/dev_hdd0/, /dev_bdvd/, /app_home/, etc.)
 * to host filesystem paths.  For now, stubs log and return success/error.
 */

#include "cellFs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

#define MAX_OPEN_FILES 256
#define MAX_OPEN_DIRS  64

typedef struct {
    int    in_use;
    char   path[CELL_FS_MAX_FS_PATH_LENGTH];
    FILE*  host_fp;
    s32    flags;
} FsFileSlot;

typedef struct {
    int  in_use;
    char path[CELL_FS_MAX_FS_PATH_LENGTH];
    /* TODO: host DIR* handle for real directory enumeration */
} FsDirSlot;

static FsFileSlot s_files[MAX_OPEN_FILES];
static FsDirSlot  s_dirs[MAX_OPEN_DIRS];

static CellFsFd alloc_fd(void)
{
    for (int i = 3; i < MAX_OPEN_FILES; i++) {  /* skip 0,1,2 = stdin/out/err */
        if (!s_files[i].in_use) {
            s_files[i].in_use = 1;
            return i;
        }
    }
    return -1;
}

static CellFsDir alloc_dir(void)
{
    for (int i = 0; i < MAX_OPEN_DIRS; i++) {
        if (!s_dirs[i].in_use) {
            s_dirs[i].in_use = 1;
            return i;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 * File operations
 * -----------------------------------------------------------------------*/

/* NID: 0x718BF5F8 */
s32 cellFsOpen(const char* path, s32 flags, CellFsFd* fd, const void* arg, u64 size)
{
    printf("[cellFs] Open(path='%s', flags=0x%X)\n", path ? path : "<null>", flags);

    if (!path || !fd)
        return CELL_EFAULT;

    CellFsFd slot = alloc_fd();
    if (slot < 0)
        return CELL_EMFILE;

    strncpy(s_files[slot].path, path, CELL_FS_MAX_FS_PATH_LENGTH - 1);
    s_files[slot].path[CELL_FS_MAX_FS_PATH_LENGTH - 1] = '\0';
    s_files[slot].flags = flags;
    s_files[slot].host_fp = NULL;

    /*
     * TODO: Translate the PS3 virtual path to a host path and fopen() it.
     * For now we succeed so games can proceed past file-open calls during
     * early bring-up.
     */

    *fd = slot;
    return CELL_OK;
}

/* NID: 0x4D5FF8E2 */
s32 cellFsClose(CellFsFd fd)
{
    printf("[cellFs] Close(fd=%d)\n", fd);

    if (fd < 0 || fd >= MAX_OPEN_FILES || !s_files[fd].in_use)
        return CELL_EBADF;

    if (s_files[fd].host_fp) {
        fclose(s_files[fd].host_fp);
        s_files[fd].host_fp = NULL;
    }
    s_files[fd].in_use = 0;

    return CELL_OK;
}

/* NID: 0xBABF9143 */
s32 cellFsRead(CellFsFd fd, void* buf, u64 nbytes, u64* nread)
{
    if (fd < 0 || fd >= MAX_OPEN_FILES || !s_files[fd].in_use)
        return CELL_EBADF;

    if (!buf)
        return CELL_EFAULT;

    u64 bytes_read = 0;

    if (s_files[fd].host_fp) {
        bytes_read = (u64)fread(buf, 1, (size_t)nbytes, s_files[fd].host_fp);
    }

    if (nread)
        *nread = bytes_read;

    return CELL_OK;
}

/* NID: 0x1E9B6714 */
s32 cellFsWrite(CellFsFd fd, const void* buf, u64 nbytes, u64* nwrite)
{
    if (fd < 0 || fd >= MAX_OPEN_FILES || !s_files[fd].in_use)
        return CELL_EBADF;

    if (!buf)
        return CELL_EFAULT;

    u64 bytes_written = 0;

    if (s_files[fd].host_fp) {
        bytes_written = (u64)fwrite(buf, 1, (size_t)nbytes, s_files[fd].host_fp);
    }

    if (nwrite)
        *nwrite = bytes_written;

    return CELL_OK;
}

/* NID: 0xA397D042 */
s32 cellFsLseek(CellFsFd fd, s64 offset, s32 whence, u64* pos)
{
    if (fd < 0 || fd >= MAX_OPEN_FILES || !s_files[fd].in_use)
        return CELL_EBADF;

    if (s_files[fd].host_fp) {
        int host_whence = SEEK_SET;
        if (whence == CELL_FS_SEEK_CUR) host_whence = SEEK_CUR;
        if (whence == CELL_FS_SEEK_END) host_whence = SEEK_END;

#ifdef _MSC_VER
        _fseeki64(s_files[fd].host_fp, offset, host_whence);
        s64 cur = _ftelli64(s_files[fd].host_fp);
#else
        fseeko(s_files[fd].host_fp, (off_t)offset, host_whence);
        s64 cur = (s64)ftello(s_files[fd].host_fp);
#endif
        if (pos)
            *pos = (u64)cur;
    } else {
        if (pos)
            *pos = 0;
    }

    return CELL_OK;
}

/* NID: 0xEF3BBD5A */
s32 cellFsFstat(CellFsFd fd, CellFsStat* sb)
{
    printf("[cellFs] Fstat(fd=%d)\n", fd);

    if (fd < 0 || fd >= MAX_OPEN_FILES || !s_files[fd].in_use)
        return CELL_EBADF;
    if (!sb)
        return CELL_EFAULT;

    memset(sb, 0, sizeof(CellFsStat));

    /* TODO: stat the underlying host file and fill in real values. */
    sb->st_mode = CELL_FS_S_IFREG | CELL_FS_S_IRUSR | CELL_FS_S_IWUSR;
    sb->st_size = 0;
    sb->st_blksize = 4096;

    return CELL_OK;
}

/* NID: 0x2CB51F0D */
s32 cellFsStat(const char* path, CellFsStat* sb)
{
    printf("[cellFs] Stat(path='%s')\n", path ? path : "<null>");

    if (!path || !sb)
        return CELL_EFAULT;

    memset(sb, 0, sizeof(CellFsStat));

    /* TODO: Translate path and perform host stat(). */
    sb->st_mode = CELL_FS_S_IFREG | CELL_FS_S_IRUSR | CELL_FS_S_IWUSR;
    sb->st_blksize = 4096;

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Directory operations
 * -----------------------------------------------------------------------*/

/* NID: 0x5C74903D */
s32 cellFsOpendir(const char* path, CellFsDir* fd)
{
    printf("[cellFs] Opendir(path='%s')\n", path ? path : "<null>");

    if (!path || !fd)
        return CELL_EFAULT;

    CellFsDir slot = alloc_dir();
    if (slot < 0)
        return CELL_EMFILE;

    strncpy(s_dirs[slot].path, path, CELL_FS_MAX_FS_PATH_LENGTH - 1);
    s_dirs[slot].path[CELL_FS_MAX_FS_PATH_LENGTH - 1] = '\0';

    *fd = slot;
    return CELL_OK;
}

/* NID: 0x9F951810 */
s32 cellFsReaddir(CellFsDir fd, CellFsDirectoryEntry* entry, u64* nread)
{
    if (fd < 0 || fd >= MAX_OPEN_DIRS || !s_dirs[fd].in_use)
        return CELL_EBADF;

    /* TODO: Enumerate host directory entries. */
    /* Return 0 entries read to signal end-of-directory. */
    if (nread)
        *nread = 0;

    return CELL_OK;
}

/* NID: 0xFF42DCC3 */
s32 cellFsClosedir(CellFsDir fd)
{
    printf("[cellFs] Closedir(fd=%d)\n", fd);

    if (fd < 0 || fd >= MAX_OPEN_DIRS || !s_dirs[fd].in_use)
        return CELL_EBADF;

    s_dirs[fd].in_use = 0;
    return CELL_OK;
}

/* NID: 0x7C1B2FCC */
s32 cellFsMkdir(const char* path, s32 mode)
{
    printf("[cellFs] Mkdir(path='%s', mode=0%o)\n", path ? path : "<null>", mode);

    if (!path)
        return CELL_EFAULT;

    /* TODO: Create host directory. */
    return CELL_OK;
}

/* NID: 0xE3F6F665 */
s32 cellFsRename(const char* from, const char* to)
{
    printf("[cellFs] Rename(from='%s', to='%s')\n",
           from ? from : "<null>", to ? to : "<null>");

    if (!from || !to)
        return CELL_EFAULT;

    /* TODO: Translate paths and rename on host. */
    return CELL_OK;
}

/* NID: 0x196CE171 */
s32 cellFsUnlink(const char* path)
{
    printf("[cellFs] Unlink(path='%s')\n", path ? path : "<null>");

    if (!path)
        return CELL_EFAULT;

    /* TODO: Translate path and remove on host. */
    return CELL_OK;
}
