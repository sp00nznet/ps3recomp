/*
 * ps3recomp - cellFsUtility HLE
 *
 * High-level filesystem utilities: recursive directory creation,
 * file copy, move, size query, and simple read/write helpers.
 */

#ifndef PS3RECOMP_CELL_FS_UTILITY_H
#define PS3RECOMP_CELL_FS_UTILITY_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes (reuses cellFs error space) */
#define CELL_FS_UTIL_ERROR_INVALID_ARGUMENT  0x80010002
#define CELL_FS_UTIL_ERROR_NOT_FOUND         0x80010006
#define CELL_FS_UTIL_ERROR_IO                0x80010005

/* Functions */

/* Create directory and all parent directories */
s32 cellFsUtilMkdirAll(const char* path, u32 mode);

/* Get file size without opening */
s32 cellFsUtilGetFileSize(const char* path, u64* size);

/* Read entire file into buffer (caller provides buffer) */
s32 cellFsUtilReadFile(const char* path, void* buf, u64 bufSize, u64* bytesRead);

/* Write buffer to file (creates/truncates) */
s32 cellFsUtilWriteFile(const char* path, const void* buf, u64 size, u64* bytesWritten);

/* Copy file from src to dst */
s32 cellFsUtilCopyFile(const char* srcPath, const char* dstPath);

/* Check if path exists (returns 1 if exists, 0 if not) */
s32 cellFsUtilExists(const char* path);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_FS_UTILITY_H */
