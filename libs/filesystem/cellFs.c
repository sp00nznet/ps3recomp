/*
 * ps3recomp - cellFs HLE implementation
 *
 * Maps PS3 virtual filesystem paths (/dev_hdd0/, /dev_bdvd/, /app_home/, etc.)
 * to host filesystem paths and performs real I/O.
 */

#include "cellFs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef _WIN32
#  include <windows.h>
#  include <io.h>
#  include <direct.h>
#  define HOST_MKDIR(p)   _mkdir(p)
#  define HOST_STAT       _stat64
#  define HOST_STAT_T     struct __stat64
#  define HOST_FSTAT      _fstat64
#else
#  include <unistd.h>
#  include <dirent.h>
#  include <sys/types.h>
#  define HOST_MKDIR(p)   mkdir(p, 0755)
#  define HOST_STAT       stat
#  define HOST_STAT_T     struct stat
#  define HOST_FSTAT      fstat
#endif

/* ---------------------------------------------------------------------------
 * Path translation
 * -----------------------------------------------------------------------*/

#define MAX_PATH_MAPPINGS 16

typedef struct {
    char ps3_prefix[128];
    char host_path[CELL_FS_MAX_FS_PATH_LENGTH];
    int  in_use;
} PathMapping;

static PathMapping s_path_mappings[MAX_PATH_MAPPINGS];
static char s_root_path[CELL_FS_MAX_FS_PATH_LENGTH] = ".";

static void init_default_mappings(void)
{
    static int s_initialized = 0;
    if (s_initialized)
        return;
    s_initialized = 1;

    cellfs_add_path_mapping("/dev_hdd0/",  "gamedata/dev_hdd0/");
    cellfs_add_path_mapping("/dev_bdvd/",  "gamedata/dev_bdvd/");
    cellfs_add_path_mapping("/dev_flash/", "gamedata/dev_flash/");
    cellfs_add_path_mapping("/app_home/",  "gamedata/app_home/");
    cellfs_add_path_mapping("/dev_usb000/","gamedata/dev_usb/");
}

void cellfs_set_root_path(const char* root)
{
    if (!root) return;
    strncpy(s_root_path, root, sizeof(s_root_path) - 1);
    s_root_path[sizeof(s_root_path) - 1] = '\0';
}

void cellfs_add_path_mapping(const char* ps3_prefix, const char* host_path)
{
    if (!ps3_prefix || !host_path) return;

    /* Try to find existing mapping for this prefix */
    for (int i = 0; i < MAX_PATH_MAPPINGS; i++) {
        if (s_path_mappings[i].in_use &&
            strcmp(s_path_mappings[i].ps3_prefix, ps3_prefix) == 0) {
            strncpy(s_path_mappings[i].host_path, host_path, sizeof(s_path_mappings[i].host_path) - 1);
            s_path_mappings[i].host_path[sizeof(s_path_mappings[i].host_path) - 1] = '\0';
            return;
        }
    }

    /* Add new mapping */
    for (int i = 0; i < MAX_PATH_MAPPINGS; i++) {
        if (!s_path_mappings[i].in_use) {
            s_path_mappings[i].in_use = 1;
            strncpy(s_path_mappings[i].ps3_prefix, ps3_prefix, sizeof(s_path_mappings[i].ps3_prefix) - 1);
            s_path_mappings[i].ps3_prefix[sizeof(s_path_mappings[i].ps3_prefix) - 1] = '\0';
            strncpy(s_path_mappings[i].host_path, host_path, sizeof(s_path_mappings[i].host_path) - 1);
            s_path_mappings[i].host_path[sizeof(s_path_mappings[i].host_path) - 1] = '\0';
            return;
        }
    }
}

/* Translate a PS3 path to a host path. Returns 0 on success, -1 if no mapping found. */
static int translate_path(const char* ps3_path, char* host_buf, size_t buf_size)
{
    init_default_mappings();

    if (!ps3_path || !host_buf || buf_size == 0)
        return -1;

    /* Find longest matching prefix */
    int best = -1;
    size_t best_len = 0;
    for (int i = 0; i < MAX_PATH_MAPPINGS; i++) {
        if (!s_path_mappings[i].in_use)
            continue;
        size_t plen = strlen(s_path_mappings[i].ps3_prefix);
        if (plen > best_len && strncmp(ps3_path, s_path_mappings[i].ps3_prefix, plen) == 0) {
            best = i;
            best_len = plen;
        }
    }

    if (best < 0)
        return -1;

    const char* remainder = ps3_path + best_len;
    snprintf(host_buf, buf_size, "%s/%s%s", s_root_path,
             s_path_mappings[best].host_path, remainder);

    /* Normalize slashes */
    for (char* p = host_buf; *p; p++) {
#ifdef _WIN32
        if (*p == '/') *p = '\\';
#else
        if (*p == '\\') *p = '/';
#endif
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * Host stat -> CellFsStat conversion
 * -----------------------------------------------------------------------*/
static void fill_cellfs_stat(CellFsStat* sb, const HOST_STAT_T* hst)
{
    memset(sb, 0, sizeof(CellFsStat));

#ifdef _WIN32
    if (hst->st_mode & _S_IFDIR)
        sb->st_mode = CELL_FS_S_IFDIR;
    else
        sb->st_mode = CELL_FS_S_IFREG;

    if (hst->st_mode & _S_IREAD)
        sb->st_mode |= CELL_FS_S_IRUSR | CELL_FS_S_IRGRP | CELL_FS_S_IROTH;
    if (hst->st_mode & _S_IWRITE)
        sb->st_mode |= CELL_FS_S_IWUSR | CELL_FS_S_IWGRP | CELL_FS_S_IWOTH;
    if (hst->st_mode & _S_IEXEC)
        sb->st_mode |= CELL_FS_S_IXUSR | CELL_FS_S_IXGRP | CELL_FS_S_IXOTH;
#else
    if (S_ISDIR(hst->st_mode))
        sb->st_mode = CELL_FS_S_IFDIR;
    else if (S_ISLNK(hst->st_mode))
        sb->st_mode = CELL_FS_S_IFLNK;
    else
        sb->st_mode = CELL_FS_S_IFREG;

    if (hst->st_mode & S_IRUSR) sb->st_mode |= CELL_FS_S_IRUSR;
    if (hst->st_mode & S_IWUSR) sb->st_mode |= CELL_FS_S_IWUSR;
    if (hst->st_mode & S_IXUSR) sb->st_mode |= CELL_FS_S_IXUSR;
    if (hst->st_mode & S_IRGRP) sb->st_mode |= CELL_FS_S_IRGRP;
    if (hst->st_mode & S_IWGRP) sb->st_mode |= CELL_FS_S_IWGRP;
    if (hst->st_mode & S_IXGRP) sb->st_mode |= CELL_FS_S_IXGRP;
    if (hst->st_mode & S_IROTH) sb->st_mode |= CELL_FS_S_IROTH;
    if (hst->st_mode & S_IWOTH) sb->st_mode |= CELL_FS_S_IWOTH;
    if (hst->st_mode & S_IXOTH) sb->st_mode |= CELL_FS_S_IXOTH;
#endif

    sb->st_uid   = 0;
    sb->st_gid   = 0;
    sb->st_atime = (s64)hst->st_atime;
    sb->st_mtime = (s64)hst->st_mtime;
    sb->st_ctime = (s64)hst->st_ctime;
    sb->st_size  = (u64)hst->st_size;
    sb->st_blksize = 4096;
}

/* ---------------------------------------------------------------------------
 * Internal file/dir state
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
    char host_path[CELL_FS_MAX_FS_PATH_LENGTH];
#ifdef _WIN32
    HANDLE           find_handle;
    WIN32_FIND_DATAA find_data;
    int              first_read;
    int              done;
#else
    DIR* host_dir;
#endif
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
            memset(&s_dirs[i], 0, sizeof(FsDirSlot));
            s_dirs[i].in_use = 1;
            return i;
        }
    }
    return -1;
}

/* Build fopen mode string from PS3 flags */
static const char* flags_to_mode(s32 flags)
{
    int access  = flags & CELL_FS_O_ACCMODE;
    int create  = flags & CELL_FS_O_CREAT;
    int trunc   = flags & CELL_FS_O_TRUNC;
    int append  = flags & CELL_FS_O_APPEND;

    if (access == CELL_FS_O_RDONLY) {
        return "rb";
    } else if (access == CELL_FS_O_WRONLY) {
        if (append)       return "ab";
        if (create && trunc) return "wb";
        if (create)       return "wb";
        if (trunc)        return "wb";
        return "r+b";  /* write-only to existing file */
    } else { /* RDWR */
        if (append)       return "a+b";
        if (create && trunc) return "w+b";
        if (create)       return "a+b";  /* create if needed, don't truncate */
        if (trunc)        return "w+b";
        return "r+b";
    }
}

/* Recursively create directories for a path (like mkdir -p) */
static void ensure_parent_dirs(const char* path)
{
    char tmp[CELL_FS_MAX_FS_PATH_LENGTH];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    /* Find last separator */
    char* last_sep = NULL;
    for (char* p = tmp; *p; p++) {
        if (*p == '/' || *p == '\\')
            last_sep = p;
    }
    if (!last_sep) return;
    *last_sep = '\0';

    /* Create each component */
    for (char* p = tmp; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = '\0';
            if (strlen(tmp) > 0)
                HOST_MKDIR(tmp);
            *p = saved;
        }
    }
    if (strlen(tmp) > 0)
        HOST_MKDIR(tmp);
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

    char host_path[CELL_FS_MAX_FS_PATH_LENGTH];
    if (translate_path(path, host_path, sizeof(host_path)) != 0) {
        printf("[cellFs] Open: no path mapping for '%s'\n", path);
        return (s32)CELL_ENOENT;
    }

    /* If creating, ensure parent directories exist */
    if (flags & CELL_FS_O_CREAT) {
        ensure_parent_dirs(host_path);
    }

    const char* mode = flags_to_mode(flags);
    FILE* fp = fopen(host_path, mode);

    /* If open for read failed and CREAT is set, try creating */
    if (!fp && (flags & CELL_FS_O_CREAT)) {
        fp = fopen(host_path, "w+b");
    }

    if (!fp) {
        printf("[cellFs] Open: fopen('%s', '%s') failed: %s\n", host_path, mode, strerror(errno));
        if (errno == ENOENT) return (s32)CELL_ENOENT;
        if (errno == EACCES) return (s32)CELL_EPERM;
        return (s32)CELL_ENOENT;
    }

    CellFsFd slot = alloc_fd();
    if (slot < 0) {
        fclose(fp);
        return CELL_FS_ERROR_EMFILE;
    }

    strncpy(s_files[slot].path, path, CELL_FS_MAX_FS_PATH_LENGTH - 1);
    s_files[slot].path[CELL_FS_MAX_FS_PATH_LENGTH - 1] = '\0';
    s_files[slot].flags   = flags;
    s_files[slot].host_fp = fp;

    *fd = slot;
    printf("[cellFs] Open: fd=%d -> '%s'\n", slot, host_path);
    return CELL_OK;
}

/* NID: 0x4D5FF8E2 */
s32 cellFsClose(CellFsFd fd)
{
    printf("[cellFs] Close(fd=%d)\n", fd);

    if (fd < 0 || fd >= MAX_OPEN_FILES || !s_files[fd].in_use)
        return CELL_FS_ERROR_EBADF;

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
        return CELL_FS_ERROR_EBADF;

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
        return CELL_FS_ERROR_EBADF;

    if (!buf)
        return CELL_EFAULT;

    u64 bytes_written = 0;

    if (s_files[fd].host_fp) {
        bytes_written = (u64)fwrite(buf, 1, (size_t)nbytes, s_files[fd].host_fp);
        fflush(s_files[fd].host_fp);
    }

    if (nwrite)
        *nwrite = bytes_written;

    return CELL_OK;
}

/* NID: 0xA397D042 */
s32 cellFsLseek(CellFsFd fd, s64 offset, s32 whence, u64* pos)
{
    if (fd < 0 || fd >= MAX_OPEN_FILES || !s_files[fd].in_use)
        return CELL_FS_ERROR_EBADF;

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
        return CELL_FS_ERROR_EBADF;
    if (!sb)
        return CELL_EFAULT;

    if (s_files[fd].host_fp) {
#ifdef _WIN32
        int file_no = _fileno(s_files[fd].host_fp);
        HOST_STAT_T hst;
        if (HOST_FSTAT(file_no, &hst) == 0) {
            fill_cellfs_stat(sb, &hst);
            return CELL_OK;
        }
#else
        int file_no = fileno(s_files[fd].host_fp);
        HOST_STAT_T hst;
        if (HOST_FSTAT(file_no, &hst) == 0) {
            fill_cellfs_stat(sb, &hst);
            return CELL_OK;
        }
#endif
    }

    /* Fallback */
    memset(sb, 0, sizeof(CellFsStat));
    sb->st_mode = CELL_FS_S_IFREG | CELL_FS_S_IRUSR | CELL_FS_S_IWUSR;
    sb->st_blksize = 4096;
    return CELL_OK;
}

/* NID: 0x2CB51F0D */
s32 cellFsStat(const char* path, CellFsStat* sb)
{
    printf("[cellFs] Stat(path='%s')\n", path ? path : "<null>");

    if (!path || !sb)
        return CELL_EFAULT;

    char host_path[CELL_FS_MAX_FS_PATH_LENGTH];
    if (translate_path(path, host_path, sizeof(host_path)) != 0)
        return (s32)CELL_ENOENT;

    HOST_STAT_T hst;
    if (HOST_STAT(host_path, &hst) != 0) {
        printf("[cellFs] Stat: host stat('%s') failed: %s\n", host_path, strerror(errno));
        return (s32)CELL_ENOENT;
    }

    fill_cellfs_stat(sb, &hst);
    return CELL_OK;
}

/* NID: 0x6D3BB15B */
s32 cellFsTruncate(const char* path, u64 size)
{
    printf("[cellFs] Truncate(path='%s', size=%llu)\n", path ? path : "<null>",
           (unsigned long long)size);

    if (!path)
        return CELL_EFAULT;

    char host_path[CELL_FS_MAX_FS_PATH_LENGTH];
    if (translate_path(path, host_path, sizeof(host_path)) != 0)
        return (s32)CELL_ENOENT;

#ifdef _WIN32
    HANDLE hFile = CreateFileA(host_path, GENERIC_WRITE, 0, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return (s32)CELL_ENOENT;

    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)size;
    SetFilePointerEx(hFile, li, NULL, FILE_BEGIN);
    SetEndOfFile(hFile);
    CloseHandle(hFile);
#else
    if (truncate(host_path, (off_t)size) != 0)
        return (s32)CELL_ENOENT;
#endif

    return CELL_OK;
}

/* NID: 0x82D3AB53 */
s32 cellFsFtruncate(CellFsFd fd, u64 size)
{
    printf("[cellFs] Ftruncate(fd=%d, size=%llu)\n", fd, (unsigned long long)size);

    if (fd < 0 || fd >= MAX_OPEN_FILES || !s_files[fd].in_use)
        return CELL_FS_ERROR_EBADF;

    if (s_files[fd].host_fp) {
        fflush(s_files[fd].host_fp);
#ifdef _WIN32
        int file_no = _fileno(s_files[fd].host_fp);
        _chsize_s(file_no, (long long)size);
#else
        int file_no = fileno(s_files[fd].host_fp);
        ftruncate(file_no, (off_t)size);
#endif
    }

    return CELL_OK;
}

/* NID: 0xC1C507E7 */
s32 cellFsGetBlockSize(const char* path, u64* sector_size, u64* block_size)
{
    printf("[cellFs] GetBlockSize(path='%s')\n", path ? path : "<null>");

    if (!path)
        return CELL_EFAULT;

    if (sector_size) *sector_size = 512;
    if (block_size)  *block_size  = 4096;

    return CELL_OK;
}

/* NID: 0x2C2C5F71 */
s32 cellFsGetFreeSize(const char* path, u32* block_size, u64* free_block_count)
{
    printf("[cellFs] GetFreeSize(path='%s')\n", path ? path : "<null>");

    if (!path)
        return CELL_EFAULT;

    if (block_size) *block_size = 4096;

    /* Report ~1GB free by default */
    if (free_block_count) *free_block_count = (u64)(1024ULL * 1024 * 1024 / 4096);

#ifdef _WIN32
    {
        char host_path[CELL_FS_MAX_FS_PATH_LENGTH];
        if (translate_path(path, host_path, sizeof(host_path)) == 0) {
            ULARGE_INTEGER free_bytes;
            if (GetDiskFreeSpaceExA(host_path, &free_bytes, NULL, NULL)) {
                if (free_block_count) *free_block_count = (u64)(free_bytes.QuadPart / 4096);
            }
        }
    }
#endif

    return CELL_OK;
}

/* NID: 0x3F61245C */
s32 cellFsChmod(const char* path, s32 mode)
{
    printf("[cellFs] Chmod(path='%s', mode=0%o)\n", path ? path : "<null>", mode);

    if (!path)
        return CELL_EFAULT;

    /* On host we mostly ignore PS3 permission bits; just succeed */
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

    char host_path[CELL_FS_MAX_FS_PATH_LENGTH];
    if (translate_path(path, host_path, sizeof(host_path)) != 0)
        return (s32)CELL_ENOENT;

    CellFsDir slot = alloc_dir();
    if (slot < 0)
        return CELL_FS_ERROR_EMFILE;

    strncpy(s_dirs[slot].host_path, host_path, CELL_FS_MAX_FS_PATH_LENGTH - 1);
    s_dirs[slot].host_path[CELL_FS_MAX_FS_PATH_LENGTH - 1] = '\0';

#ifdef _WIN32
    {
        char search_path[CELL_FS_MAX_FS_PATH_LENGTH];
        snprintf(search_path, sizeof(search_path), "%s\\*", host_path);
        s_dirs[slot].find_handle = FindFirstFileA(search_path, &s_dirs[slot].find_data);
        if (s_dirs[slot].find_handle == INVALID_HANDLE_VALUE) {
            s_dirs[slot].in_use = 0;
            printf("[cellFs] Opendir: FindFirstFile('%s') failed\n", search_path);
            return (s32)CELL_ENOENT;
        }
        s_dirs[slot].first_read = 1;
        s_dirs[slot].done = 0;
    }
#else
    s_dirs[slot].host_dir = opendir(host_path);
    if (!s_dirs[slot].host_dir) {
        s_dirs[slot].in_use = 0;
        printf("[cellFs] Opendir: opendir('%s') failed: %s\n", host_path, strerror(errno));
        return (s32)CELL_ENOENT;
    }
#endif

    *fd = slot;
    printf("[cellFs] Opendir: dir_fd=%d -> '%s'\n", slot, host_path);
    return CELL_OK;
}

/* NID: 0x9F951810 */
s32 cellFsReaddir(CellFsDir fd, CellFsDirectoryEntry* entry, u64* nread)
{
    if (fd < 0 || fd >= MAX_OPEN_DIRS || !s_dirs[fd].in_use)
        return CELL_FS_ERROR_EBADF;

    if (!entry || !nread)
        return CELL_EFAULT;

#ifdef _WIN32
    if (s_dirs[fd].done) {
        *nread = 0;
        return CELL_OK;
    }

    if (!s_dirs[fd].first_read) {
        if (!FindNextFileA(s_dirs[fd].find_handle, &s_dirs[fd].find_data)) {
            s_dirs[fd].done = 1;
            *nread = 0;
            return CELL_OK;
        }
    }
    s_dirs[fd].first_read = 0;

    memset(entry, 0, sizeof(CellFsDirectoryEntry));
    strncpy(entry->entry_name, s_dirs[fd].find_data.cFileName,
            CELL_FS_MAX_FS_FILE_NAME_LENGTH - 1);
    entry->entry_name[CELL_FS_MAX_FS_FILE_NAME_LENGTH - 1] = '\0';

    /* Fill attributes */
    if (s_dirs[fd].find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        entry->attribute.st_mode = CELL_FS_S_IFDIR | CELL_FS_S_IRUSR |
                                   CELL_FS_S_IWUSR | CELL_FS_S_IXUSR;
    } else {
        entry->attribute.st_mode = CELL_FS_S_IFREG | CELL_FS_S_IRUSR | CELL_FS_S_IWUSR;
        ULARGE_INTEGER file_size;
        file_size.HighPart = s_dirs[fd].find_data.nFileSizeHigh;
        file_size.LowPart  = s_dirs[fd].find_data.nFileSizeLow;
        entry->attribute.st_size = (u64)file_size.QuadPart;
    }
    entry->attribute.st_blksize = 4096;

    *nread = 1;
#else
    struct dirent* de = readdir(s_dirs[fd].host_dir);
    if (!de) {
        *nread = 0;
        return CELL_OK;
    }

    memset(entry, 0, sizeof(CellFsDirectoryEntry));
    strncpy(entry->entry_name, de->d_name, CELL_FS_MAX_FS_FILE_NAME_LENGTH - 1);
    entry->entry_name[CELL_FS_MAX_FS_FILE_NAME_LENGTH - 1] = '\0';

    /* Stat the entry for full info */
    char full_path[CELL_FS_MAX_FS_PATH_LENGTH];
    snprintf(full_path, sizeof(full_path), "%s/%s", s_dirs[fd].host_path, de->d_name);
    HOST_STAT_T hst;
    if (HOST_STAT(full_path, &hst) == 0) {
        fill_cellfs_stat(&entry->attribute, &hst);
    } else {
        /* Minimal fallback */
        entry->attribute.st_mode = CELL_FS_S_IFREG | CELL_FS_S_IRUSR | CELL_FS_S_IWUSR;
        entry->attribute.st_blksize = 4096;
    }

    *nread = 1;
#endif

    return CELL_OK;
}

/* NID: 0xFF42DCC3 */
s32 cellFsClosedir(CellFsDir fd)
{
    printf("[cellFs] Closedir(fd=%d)\n", fd);

    if (fd < 0 || fd >= MAX_OPEN_DIRS || !s_dirs[fd].in_use)
        return CELL_FS_ERROR_EBADF;

#ifdef _WIN32
    if (s_dirs[fd].find_handle != INVALID_HANDLE_VALUE) {
        FindClose(s_dirs[fd].find_handle);
        s_dirs[fd].find_handle = INVALID_HANDLE_VALUE;
    }
#else
    if (s_dirs[fd].host_dir) {
        closedir(s_dirs[fd].host_dir);
        s_dirs[fd].host_dir = NULL;
    }
#endif

    s_dirs[fd].in_use = 0;
    return CELL_OK;
}

/* NID: 0x7C1B2FCC */
s32 cellFsMkdir(const char* path, s32 mode)
{
    printf("[cellFs] Mkdir(path='%s', mode=0%o)\n", path ? path : "<null>", mode);

    if (!path)
        return CELL_EFAULT;

    char host_path[CELL_FS_MAX_FS_PATH_LENGTH];
    if (translate_path(path, host_path, sizeof(host_path)) != 0)
        return (s32)CELL_ENOENT;

    ensure_parent_dirs(host_path);

    int ret = HOST_MKDIR(host_path);
    if (ret != 0 && errno != EEXIST) {
        printf("[cellFs] Mkdir: mkdir('%s') failed: %s\n", host_path, strerror(errno));
        return (s32)CELL_ENOENT;
    }
    if (ret != 0 && errno == EEXIST) {
        return (s32)CELL_EEXIST;
    }

    return CELL_OK;
}

/* NID: 0xE3F6F665 */
s32 cellFsRename(const char* from, const char* to)
{
    printf("[cellFs] Rename(from='%s', to='%s')\n",
           from ? from : "<null>", to ? to : "<null>");

    if (!from || !to)
        return CELL_EFAULT;

    char host_from[CELL_FS_MAX_FS_PATH_LENGTH];
    char host_to[CELL_FS_MAX_FS_PATH_LENGTH];

    if (translate_path(from, host_from, sizeof(host_from)) != 0)
        return (s32)CELL_ENOENT;
    if (translate_path(to, host_to, sizeof(host_to)) != 0)
        return (s32)CELL_ENOENT;

    ensure_parent_dirs(host_to);

    if (rename(host_from, host_to) != 0) {
        printf("[cellFs] Rename: rename('%s', '%s') failed: %s\n",
               host_from, host_to, strerror(errno));
        return (s32)CELL_ENOENT;
    }

    return CELL_OK;
}

/* NID: 0x196CE171 */
s32 cellFsUnlink(const char* path)
{
    printf("[cellFs] Unlink(path='%s')\n", path ? path : "<null>");

    if (!path)
        return CELL_EFAULT;

    char host_path[CELL_FS_MAX_FS_PATH_LENGTH];
    if (translate_path(path, host_path, sizeof(host_path)) != 0)
        return (s32)CELL_ENOENT;

    if (remove(host_path) != 0) {
        printf("[cellFs] Unlink: remove('%s') failed: %s\n", host_path, strerror(errno));
        return (s32)CELL_ENOENT;
    }

    return CELL_OK;
}
