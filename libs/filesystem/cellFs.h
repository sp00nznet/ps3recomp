/*
 * ps3recomp - cellFs HLE
 *
 * PS3 filesystem operations: open/close/read/write, stat, directory listing,
 * truncate, block size, chmod.
 */

#ifndef PS3RECOMP_CELL_FS_H
#define PS3RECOMP_CELL_FS_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * FS-specific error codes
 * -----------------------------------------------------------------------*/
#define CELL_FS_ERROR_EBADF         (s32)0x80010009
#define CELL_FS_ERROR_EMFILE        (s32)0x80010018

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

/* File open flags */
#define CELL_FS_O_RDONLY    000000
#define CELL_FS_O_WRONLY    000001
#define CELL_FS_O_RDWR      000002
#define CELL_FS_O_ACCMODE   000003
#define CELL_FS_O_CREAT     000100
#define CELL_FS_O_EXCL      000200
#define CELL_FS_O_TRUNC     001000
#define CELL_FS_O_APPEND    002000
#define CELL_FS_O_MSELF     010000

/* Seek whence */
#define CELL_FS_SEEK_SET    0
#define CELL_FS_SEEK_CUR    1
#define CELL_FS_SEEK_END    2

/* File type (st_mode) */
#define CELL_FS_S_IFMT      0170000
#define CELL_FS_S_IFDIR     0040000
#define CELL_FS_S_IFREG     0100000
#define CELL_FS_S_IFLNK     0120000
#define CELL_FS_S_IRUSR     0000400
#define CELL_FS_S_IWUSR     0000200
#define CELL_FS_S_IXUSR     0000100
#define CELL_FS_S_IRGRP     0000040
#define CELL_FS_S_IWGRP     0000020
#define CELL_FS_S_IXGRP     0000010
#define CELL_FS_S_IROTH     0000004
#define CELL_FS_S_IWOTH     0000002
#define CELL_FS_S_IXOTH     0000001

/* Max path length */
#define CELL_FS_MAX_FS_PATH_LENGTH   1024
#define CELL_FS_MAX_FS_FILE_NAME_LENGTH 256
#define CELL_FS_MAX_MP_LENGTH        32

/* ---------------------------------------------------------------------------
 * Structures
 * -----------------------------------------------------------------------*/

typedef struct CellFsStat {
    s32  st_mode;
    s32  st_uid;
    s32  st_gid;
    s64  st_atime;
    s64  st_mtime;
    s64  st_ctime;
    u64  st_size;
    u64  st_blksize;
} CellFsStat;

typedef struct CellFsDirectoryEntry {
    CellFsStat attribute;
    char       entry_name[CELL_FS_MAX_FS_FILE_NAME_LENGTH];
} CellFsDirectoryEntry;

/* Opaque file/directory descriptors */
typedef s32 CellFsFd;
typedef s32 CellFsDir;

/* ---------------------------------------------------------------------------
 * Path configuration
 * -----------------------------------------------------------------------*/

/* Set the root directory for all path translations (default: ".") */
void cellfs_set_root_path(const char* root);

/* Add or override a PS3 prefix -> host path mapping */
void cellfs_add_path_mapping(const char* ps3_prefix, const char* host_path);

/* Translate a PS3 virtual path (e.g. "/dev_hdd0/game/...") to a host path.
 * Returns 0 on success, -1 if no mapping found. */
int cellfs_translate_path(const char* ps3_path, char* host_buf, size_t buf_size);

/* ---------------------------------------------------------------------------
 * File operations
 * -----------------------------------------------------------------------*/

/* NID: 0x718BF5F8 */
s32 cellFsOpen(const char* path, s32 flags, CellFsFd* fd, const void* arg, u64 size);

/* NID: 0x4D5FF8E2 */
s32 cellFsClose(CellFsFd fd);

/* NID: 0xBABF9143 */
s32 cellFsRead(CellFsFd fd, void* buf, u64 nbytes, u64* nread);

/* NID: 0x1E9B6714 */
s32 cellFsWrite(CellFsFd fd, const void* buf, u64 nbytes, u64* nwrite);

/* NID: 0xA397D042 */
s32 cellFsLseek(CellFsFd fd, s64 offset, s32 whence, u64* pos);

/* NID: 0xEF3BBD5A */
s32 cellFsFstat(CellFsFd fd, CellFsStat* sb);

/* NID: 0x2CB51F0D */
s32 cellFsStat(const char* path, CellFsStat* sb);

/* NID: 0x6D3BB15B */
s32 cellFsTruncate(const char* path, u64 size);

/* NID: 0x82D3AB53 */
s32 cellFsFtruncate(CellFsFd fd, u64 size);

/* NID: 0xC1C507E7 */
s32 cellFsGetBlockSize(const char* path, u64* sector_size, u64* block_size);

/* NID: 0x2C2C5F71 */
s32 cellFsGetFreeSize(const char* path, u32* block_size, u64* free_block_count);

/* NID: 0x3F61245C */
s32 cellFsChmod(const char* path, s32 mode);

/* ---------------------------------------------------------------------------
 * Directory operations
 * -----------------------------------------------------------------------*/

/* NID: 0x5C74903D */
s32 cellFsOpendir(const char* path, CellFsDir* fd);

/* NID: 0x9F951810 */
s32 cellFsReaddir(CellFsDir fd, CellFsDirectoryEntry* entry, u64* nread);

/* NID: 0xFF42DCC3 */
s32 cellFsClosedir(CellFsDir fd);

/* NID: 0x7C1B2FCC */
s32 cellFsMkdir(const char* path, s32 mode);

/* NID: 0xE3F6F665 */
s32 cellFsRename(const char* from, const char* to);

/* NID: 0x196CE171 */
s32 cellFsUnlink(const char* path);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_FS_H */
