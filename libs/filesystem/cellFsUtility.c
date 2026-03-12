/*
 * ps3recomp - cellFsUtility HLE implementation
 *
 * Implemented using standard C file I/O. Path translation is
 * assumed to be handled at a higher level (cellFs layer).
 */

#include "cellFsUtility.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir_p(path, mode) _mkdir(path)
#else
#include <unistd.h>
#define mkdir_p(path, mode) mkdir(path, mode)
#endif

/* Create directory and all parents */
s32 cellFsUtilMkdirAll(const char* path, u32 mode)
{
    if (!path) return (s32)CELL_FS_UTIL_ERROR_INVALID_ARGUMENT;

    char tmp[1024];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) return (s32)CELL_FS_UTIL_ERROR_INVALID_ARGUMENT;

    memcpy(tmp, path, len + 1);

    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            tmp[i] = '\0';
            mkdir_p(tmp, mode);
            tmp[i] = '/';
        }
    }
    mkdir_p(tmp, mode);
    return CELL_OK;
}

s32 cellFsUtilGetFileSize(const char* path, u64* size)
{
    if (!path || !size) return (s32)CELL_FS_UTIL_ERROR_INVALID_ARGUMENT;

    struct stat st;
    if (stat(path, &st) != 0)
        return (s32)CELL_FS_UTIL_ERROR_NOT_FOUND;

    *size = (u64)st.st_size;
    return CELL_OK;
}

s32 cellFsUtilReadFile(const char* path, void* buf, u64 bufSize, u64* bytesRead)
{
    if (!path || !buf) return (s32)CELL_FS_UTIL_ERROR_INVALID_ARGUMENT;

    FILE* f = fopen(path, "rb");
    if (!f) return (s32)CELL_FS_UTIL_ERROR_NOT_FOUND;

    size_t n = fread(buf, 1, (size_t)bufSize, f);
    fclose(f);

    if (bytesRead) *bytesRead = (u64)n;
    return CELL_OK;
}

s32 cellFsUtilWriteFile(const char* path, const void* buf, u64 size, u64* bytesWritten)
{
    if (!path || !buf) return (s32)CELL_FS_UTIL_ERROR_INVALID_ARGUMENT;

    FILE* f = fopen(path, "wb");
    if (!f) return (s32)CELL_FS_UTIL_ERROR_IO;

    size_t n = fwrite(buf, 1, (size_t)size, f);
    fclose(f);

    if (bytesWritten) *bytesWritten = (u64)n;
    return CELL_OK;
}

s32 cellFsUtilCopyFile(const char* srcPath, const char* dstPath)
{
    if (!srcPath || !dstPath) return (s32)CELL_FS_UTIL_ERROR_INVALID_ARGUMENT;

    FILE* src = fopen(srcPath, "rb");
    if (!src) return (s32)CELL_FS_UTIL_ERROR_NOT_FOUND;

    FILE* dst = fopen(dstPath, "wb");
    if (!dst) { fclose(src); return (s32)CELL_FS_UTIL_ERROR_IO; }

    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) {
            fclose(src); fclose(dst);
            return (s32)CELL_FS_UTIL_ERROR_IO;
        }
    }

    fclose(src);
    fclose(dst);
    return CELL_OK;
}

s32 cellFsUtilExists(const char* path)
{
    if (!path) return 0;
    struct stat st;
    return (stat(path, &st) == 0) ? 1 : 0;
}
