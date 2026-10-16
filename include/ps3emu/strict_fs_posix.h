/* Linux guest-I/O containment for a trusted development host. This is not an
 * OS sandbox and does not prevent concurrent path substitution (TOCTOU).
 * Compile strict C translation units with _POSIX_C_SOURCE >= 200809L or
 * _GNU_SOURCE before system headers. No helper mutates files or owns streams. */
#ifndef PS3EMU_STRICT_FS_POSIX_H
#define PS3EMU_STRICT_FS_POSIX_H
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

enum { PS3_STRICT_FS_POSIX_CAP = 4096 };

/* Without symlinks, dot components or repeated separators, the spelling is
 * canonical. A single trailing separator is accepted only for directories. */
static inline const char* ps3_strict_fs_posix_syntax(const char* path, char* out) {
    if (!path || path[0] != '/') return "invalid-absolute-path";
    size_t n = strnlen(path, PS3_STRICT_FS_POSIX_CAP);
    if (n == PS3_STRICT_FS_POSIX_CAP) return "path-too-long";
    const char* start = path + 1;
    for (const char* p = start; ; ++p) {
        if (*p && *p != '/') continue;
        size_t len = (size_t)(p - start);
        if ((!len && *p) || (len == 1 && start[0] == '.') ||
            (len == 2 && start[0] == '.' && start[1] == '.'))
            return "ambiguous-path-component";
        if (len > 255) return "component-too-long";
        if (!*p) break;
        start = p + 1;
    }
    memcpy(out, path, n + 1);
    if (n > 1 && out[n - 1] == '/') out[n - 1] = 0;
    return NULL;
}

/* Inspect all existing ancestors with lstat, including ancestors of the root.
 * Only ENOENT after a verified directory allows prospective descendants. */
static inline const char* ps3_strict_fs_posix_walk(char* path, int must_exist,
                                                 int must_directory) {
    size_t n = strlen(path);
    struct stat st;
    if (lstat("/", &st) || !S_ISDIR(st.st_mode)) return "invalid-root-directory";
    if (n == 1) return NULL;
    for (size_t i = 1; i <= n; ++i) {
        if (path[i] && path[i] != '/') continue;
        char saved = path[i];
        path[i] = 0;
        int result = lstat(path, &st);
        int error = errno;
        path[i] = saved;
        if (result) {
            if (error == ENOENT && !must_exist) return NULL;
            return "unresolved-path-component";
        }
        if (S_ISLNK(st.st_mode)) return "symbolic-link-path";
        if (i < n || must_directory) {
            if (!S_ISDIR(st.st_mode)) return "non-directory-ancestor";
        } else if (S_ISREG(st.st_mode)) {
            if (st.st_nlink != 1) return "ambiguous-file-links";
        } else if (!S_ISDIR(st.st_mode)) return "special-file-path";
    }
    return NULL;
}

static inline const char* ps3_strict_fs_posix_path(const char* root, const char* path) {
    char checked_root[PS3_STRICT_FS_POSIX_CAP], checked_path[PS3_STRICT_FS_POSIX_CAP];
    const char* error = ps3_strict_fs_posix_syntax(root, checked_root);
    if (error) return error;
    error = ps3_strict_fs_posix_walk(checked_root, 1, 1);
    if (error) return error;
    error = ps3_strict_fs_posix_syntax(path, checked_path);
    if (error) return error;
    size_t root_len = strlen(checked_root);
    if (strncmp(checked_root, checked_path, root_len) ||
        (root_len > 1 && checked_path[root_len] && checked_path[root_len] != '/'))
        return "outside-write-root";
    size_t path_len = strlen(path);
    return ps3_strict_fs_posix_walk(checked_path, 0, path[path_len - 1] == '/');
}

static inline const char* ps3_strict_fs_posix_stream(const char* root, FILE* stream) {
    if (!stream) return "invalid-stream";
    int fd = fileno(stream);
    struct stat open_info, path_info;
    if (fd < 0 || fstat(fd, &open_info)) return "invalid-stream-descriptor";
    if (!S_ISREG(open_info.st_mode)) return "non-regular-stream";
    if (open_info.st_nlink != 1) return "ambiguous-file-links";
    char proc_path[64], path[PS3_STRICT_FS_POSIX_CAP];
    int count = snprintf(proc_path, sizeof proc_path, "/proc/self/fd/%d", fd);
    if (count < 0 || (size_t)count >= sizeof proc_path) return "unresolved-stream-path";
    ssize_t n = readlink(proc_path, path, sizeof path);
    if (n <= 0 || (size_t)n >= sizeof path) return "unresolved-stream-path";
    path[n] = 0;
    const char* error = ps3_strict_fs_posix_path(root, path);
    if (error) return error;
    if (lstat(path, &path_info) || !S_ISREG(path_info.st_mode) ||
        path_info.st_nlink != 1 || path_info.st_dev != open_info.st_dev ||
        path_info.st_ino != open_info.st_ino) return "stream-path-identity-mismatch";
    return NULL;
}
#endif
