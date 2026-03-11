/*
 * ps3recomp - Filesystem syscalls
 *
 * Kernel-level filesystem with PS3 path -> host path translation.
 */

#ifndef SYS_FS_H
#define SYS_FS_H

#include "lv2_syscall_table.h"
#include "../ppu/ppu_context.h"
#include "../../include/ps3emu/ps3types.h"
#include "../../include/ps3emu/error_codes.h"

#include <stdint.h>
#include <stdio.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <dirent.h>
  #include <sys/stat.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum open file descriptors / directory descriptors */
#define SYS_FS_FD_MAX   256
#define SYS_FS_DIR_MAX  64

/* CellFsStat structure layout (as written to guest memory, all big-endian):
 *   u32 mode        @ 0
 *   s32 uid         @ 4
 *   s32 gid         @ 8
 *   u64 atime       @ 12  (actually at 16 with padding)
 *   u64 mtime       @ 20  (at 24)
 *   u64 ctime       @ 28  (at 32)
 *   u64 size        @ 36  (at 40)
 *   u64 blksize     @ 44  (at 48)
 * Total: 52 bytes (but padded to 56 for alignment)
 *
 * We use a simpler layout matching RPCS3:
 *   u32 mode        @ 0
 *   s32 uid         @ 4
 *   s32 gid         @ 8
 *   u32 pad         @ 12
 *   u64 atime       @ 16
 *   u64 mtime       @ 24
 *   u64 ctime       @ 32
 *   u64 size        @ 40
 *   u64 blksize     @ 48
 * Total: 56 bytes
 */

/* CellFsMode flags */
#define CELL_FS_S_IFDIR   0040000
#define CELL_FS_S_IFREG   0100000
#define CELL_FS_S_IFLNK   0120000
#define CELL_FS_S_IRUSR   0000400
#define CELL_FS_S_IWUSR   0000200
#define CELL_FS_S_IXUSR   0000100
#define CELL_FS_S_IRGRP   0000040
#define CELL_FS_S_IROTH   0000004

/* Open flags */
#define CELL_FS_O_RDONLY   0x000000
#define CELL_FS_O_WRONLY   0x000001
#define CELL_FS_O_RDWR     0x000002
#define CELL_FS_O_ACCMODE  0x000003
#define CELL_FS_O_CREAT    0x000100
#define CELL_FS_O_TRUNC    0x000200
#define CELL_FS_O_EXCL     0x000400
#define CELL_FS_O_APPEND   0x002000

/* Seek modes */
#define CELL_FS_SEEK_SET   0
#define CELL_FS_SEEK_CUR   1
#define CELL_FS_SEEK_END   2

/* File descriptor entry */
typedef struct sys_fs_fd_info {
    int    active;
    FILE*  fp;
    char   path[512];  /* host path */
    int    flags;
} sys_fs_fd_info;

/* Directory descriptor entry */
typedef struct sys_fs_dir_info {
    int    active;
    char   path[512];

#ifdef _WIN32
    HANDLE           find_handle;
    WIN32_FIND_DATAA find_data;
    int              first_read;
#else
    DIR*   dp;
#endif

} sys_fs_dir_info;

extern sys_fs_fd_info  g_sys_fs_fds[SYS_FS_FD_MAX];
extern sys_fs_dir_info g_sys_fs_dirs[SYS_FS_DIR_MAX];

/* Configurable root directory for path mapping */
extern char g_sys_fs_root[512];

/* Path translation: convert PS3 path to host path */
void sys_fs_translate_path(const char* ps3_path, char* host_path, int host_path_size);

/* Syscall handlers */
int64_t sys_fs_open(ppu_context* ctx);
int64_t sys_fs_read(ppu_context* ctx);
int64_t sys_fs_write(ppu_context* ctx);
int64_t sys_fs_close(ppu_context* ctx);
int64_t sys_fs_lseek(ppu_context* ctx);
int64_t sys_fs_stat(ppu_context* ctx);
int64_t sys_fs_fstat(ppu_context* ctx);
int64_t sys_fs_opendir(ppu_context* ctx);
int64_t sys_fs_readdir(ppu_context* ctx);
int64_t sys_fs_closedir(ppu_context* ctx);
int64_t sys_fs_mkdir(ppu_context* ctx);
int64_t sys_fs_rename(ppu_context* ctx);
int64_t sys_fs_unlink(ppu_context* ctx);
int64_t sys_fs_rmdir(ppu_context* ctx);
int64_t sys_fs_ftruncate(ppu_context* ctx);

/* Registration */
void sys_fs_init(lv2_syscall_table* tbl);

#ifdef __cplusplus
}
#endif

#endif /* SYS_FS_H */
