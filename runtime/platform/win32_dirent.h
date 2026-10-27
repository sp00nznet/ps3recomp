/*
 * ps3recomp - <dirent.h> for Windows
 *
 * runtime/ppu/ppu_fs.cpp walks host directories with the POSIX API --
 * opendir/readdir/closedir -- to answer the guest's cellFsOpendir. MSVC ships
 * no <dirent.h>, so that file did not compile on Windows without help, and
 * every game port quietly carried its own copy of a Win32 shim on the include
 * path to get past it. Putting one here deletes that copy from all of them.
 *
 * On POSIX this is just the system header, so call sites include this and stop
 * thinking about it.
 *
 * Scope is deliberately the four names ppu_fs.cpp actually uses, with d_name
 * as the only dirent field: no d_type, no rewinddir/seekdir/telldir, no
 * wide-character variant. FindFirstFileA reports "." and ".." exactly as
 * readdir does, so the caller's skip logic needs no special case.
 */
#ifndef PS3RECOMP_WIN32_DIRENT_H
#define PS3RECOMP_WIN32_DIRENT_H

#ifndef _WIN32

#include <dirent.h>

#else

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdlib.h>
#include <string.h>

struct dirent {
    char d_name[MAX_PATH];
};

typedef struct DIR {
    HANDLE           h;
    WIN32_FIND_DATAA fd;
    int              pending;   /* FindFirstFile already produced an entry */
    struct dirent    ent;
} DIR;

static inline DIR* opendir(const char* path)
{
    if (!path || !*path) return NULL;

    size_t n = strlen(path);
    char*  pattern = (char*)malloc(n + 3);   /* + "\*" + NUL */
    if (!pattern) return NULL;
    memcpy(pattern, path, n);
    /* Tolerate a trailing separator: "dir/" and "dir" must both work. Appending
     * blindly would produce a doubled separator before the wildcard, which
     * matches nothing. */
    if (n && (pattern[n - 1] == '/' || pattern[n - 1] == '\\')) n--;
    pattern[n]     = '\\';
    pattern[n + 1] = '*';
    pattern[n + 2] = '\0';

    DIR* d = (DIR*)calloc(1, sizeof(DIR));
    if (!d) { free(pattern); return NULL; }

    d->h = FindFirstFileA(pattern, &d->fd);
    free(pattern);
    if (d->h == INVALID_HANDLE_VALUE) { free(d); return NULL; }

    d->pending = 1;
    return d;
}

static inline struct dirent* readdir(DIR* d)
{
    if (!d || d->h == INVALID_HANDLE_VALUE) return NULL;

    if (d->pending) d->pending = 0;
    else if (!FindNextFileA(d->h, &d->fd)) return NULL;

    /* memcpy rather than strncpy: both buffers are MAX_PATH and cFileName is
     * already NUL-terminated, and strncpy is deprecated by the CRT. */
    size_t n = strlen(d->fd.cFileName);
    if (n >= sizeof d->ent.d_name) n = sizeof d->ent.d_name - 1;
    memcpy(d->ent.d_name, d->fd.cFileName, n);
    d->ent.d_name[n] = '\0';
    return &d->ent;
}

static inline int closedir(DIR* d)
{
    if (!d) return -1;
    if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
    free(d);
    return 0;
}

#endif /* _WIN32 */
#endif /* PS3RECOMP_WIN32_DIRENT_H */
