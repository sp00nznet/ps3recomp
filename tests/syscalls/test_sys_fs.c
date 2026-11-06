/* sys_fs: the raw lv2 filesystem syscalls, against real files in a temporary
 * directory.
 *
 * Covers what a guest libc and a directory walker depend on and what used to be
 * wrong: the SDK's octal open flags (O_CREAT 0100, O_EXCL 0200, O_APPEND 02000),
 * a failed seek leaving the offset alone, 64-bit offsets, the error codes of
 * mkdir/unlink/rmdir, readdir's per-entry byte count and its "." / ".." entries,
 * and the fd tables handing out distinct slots to concurrent opens and
 * reporting EMFILE when full.
 *
 * Links sys_fs.c alone; the pieces it reaches for are stubbed below. */
#include "sys_fs.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#  include <direct.h>
#  define MKDIR(p) _mkdir(p)
#else
#  include <pthread.h>
#  include <unistd.h>
#  define MKDIR(p) mkdir((p), 0755)
#endif

/* ---- what sys_fs.c links against ---------------------------------------- */
uint8_t* vm_base;
uint32_t ppu_vm_size = 0x20000;           /* sys_fs_read clamps to it */
const char* edat_resolve(const char* host_path, char* buf, size_t cap)
{ (void)buf; (void)cap; return host_path; }                 /* no EDATs here */

/* ---- harness ------------------------------------------------------------- */
static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)
#define CHECK_RC(call, want) do { int32_t _rc = (int32_t)(call); \
    if (_rc != (int32_t)(want)) { fprintf(stderr, "FAIL line %d: %s -> 0x%08X, want 0x%08X\n", \
        __LINE__, #call, (unsigned)_rc, (unsigned)(want)); failures++; } } while (0)

#define VM_SIZE   0x20000u
#define A_PATH    0x1000u   /* guest path strings */
#define A_OUT     0x2000u   /* fd / position / nread out-values */
#define A_BUF     0x3000u   /* read / write buffer */
#define A_DIRENT  0x4000u   /* CellFsDirent */

static char s_root[512];

static uint32_t rd32(uint32_t a) { const uint8_t* p = vm_base + a; return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint64_t rd64(uint32_t a) { return (uint64_t)rd32(a) << 32 | rd32(a + 4); }

/* Calls handler f with up to six arguments; r3..r8. */
static int64_t sc(int64_t (*f)(ppu_context*), uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{
    ppu_context ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = a; ctx.gpr[4] = b; ctx.gpr[5] = c; ctx.gpr[6] = d; ctx.gpr[7] = e;
    return f(&ctx);
}
static void set_path(uint32_t at, const char* guest) { strcpy((char*)vm_base + at, guest); }

/* Open guest path "/dev_bdvd/<name>" (maps to <root>/<name>) with flags. */
static int32_t sys_open_named(const char* name, int32_t flags, int32_t* fd_out)
{
    char g[256];
    snprintf(g, sizeof g, "/dev_bdvd/%s", name);
    set_path(A_PATH, g);
    int32_t rc = (int32_t)sc(sys_fs_open, A_PATH, (uint32_t)flags, A_OUT, 0644, 0);
    if (rc == 0 && fd_out) *fd_out = (int32_t)rd32(A_OUT);
    return rc;
}
static int32_t sys_path_call(int64_t (*f)(ppu_context*), const char* name)
{
    char g[256];
    snprintf(g, sizeof g, "/dev_bdvd/%s", name);
    set_path(A_PATH, g);
    return (int32_t)sc(f, A_PATH, 0755, 0, 0, 0);
}
static int write_all(int32_t fd, const char* s)
{
    memcpy(vm_base + A_BUF, s, strlen(s));
    int32_t rc = (int32_t)sc(sys_fs_write, (uint32_t)fd, A_BUF, strlen(s), A_OUT, 0);
    return rc == 0 && rd64(A_OUT) == strlen(s);
}
static int host_file_equals(const char* name, const char* want)
{
    char p[600]; char got[64] = {0};
    snprintf(p, sizeof p, "%s/%s", s_root, name);
    FILE* f = fopen(p, "rb");
    if (!f) return 0;
    size_t n = fread(got, 1, sizeof got - 1, f);
    fclose(f);
    return n == strlen(want) && memcmp(got, want, n) == 0;
}
static int host_exists(const char* name)
{
    char p[600]; struct stat st;
    snprintf(p, sizeof p, "%s/%s", s_root, name);
    return stat(p, &st) == 0;
}
static void host_put(const char* name, const char* content)
{
    char p[600];
    snprintf(p, sizeof p, "%s/%s", s_root, name);
    FILE* f = fopen(p, "wb");
    if (f) { fputs(content, f); fclose(f); }
}

/* SDK open flags, spelt out in octal so the test does not depend on the header
 * it is checking. */
#define F_WRONLY 0001
#define F_RDWR   0002
#define F_CREAT  0100
#define F_EXCL   0200
#define F_TRUNC  01000
#define F_APPEND 02000

static void test_flag_values(void)
{
    CHECK(CELL_FS_O_CREAT  == F_CREAT);
    CHECK(CELL_FS_O_EXCL   == F_EXCL);
    CHECK(CELL_FS_O_TRUNC  == F_TRUNC);
    CHECK(CELL_FS_O_APPEND == F_APPEND);
    CHECK(CELL_FS_O_ACCMODE == 3);
}

static void test_open_flags(void)
{
    int32_t fd = -1;
    /* libc open(O_WRONLY|O_CREAT) of a file that does not exist: created. */
    CHECK_RC(sys_open_named("created.txt", F_WRONLY | F_CREAT, &fd), CELL_OK);
    CHECK(fd >= 3);
    CHECK(write_all(fd, "hello"));
    CHECK_RC(sc(sys_fs_close, (uint32_t)fd, 0, 0, 0, 0), CELL_OK);
    CHECK(host_file_equals("created.txt", "hello"));

    /* O_CREAT on an existing file keeps its contents. */
    CHECK_RC(sys_open_named("created.txt", F_WRONLY | F_CREAT, &fd), CELL_OK);
    CHECK_RC(sc(sys_fs_close, (uint32_t)fd, 0, 0, 0, 0), CELL_OK);
    CHECK(host_file_equals("created.txt", "hello"));

    /* O_CREAT|O_EXCL: an existing file is EEXIST, a new one is created. */
    CHECK_RC(sys_open_named("created.txt", F_WRONLY | F_CREAT | F_EXCL, &fd), CELL_EEXIST);
    CHECK_RC(sys_open_named("excl.txt", F_WRONLY | F_CREAT | F_EXCL, &fd), CELL_OK);
    CHECK_RC(sc(sys_fs_close, (uint32_t)fd, 0, 0, 0, 0), CELL_OK);
    CHECK(host_exists("excl.txt"));

    /* fopen("a"): O_WRONLY|O_CREAT|O_APPEND appends instead of overwriting. */
    CHECK_RC(sys_open_named("created.txt", F_WRONLY | F_CREAT | F_APPEND, &fd), CELL_OK);
    CHECK(write_all(fd, "!!"));
    CHECK_RC(sc(sys_fs_close, (uint32_t)fd, 0, 0, 0, 0), CELL_OK);
    CHECK(host_file_equals("created.txt", "hello!!"));

    /* O_TRUNC empties it. */
    CHECK_RC(sys_open_named("created.txt", F_WRONLY | F_TRUNC, &fd), CELL_OK);
    CHECK_RC(sc(sys_fs_close, (uint32_t)fd, 0, 0, 0, 0), CELL_OK);
    CHECK(host_file_equals("created.txt", ""));

    /* No O_CREAT: a missing file is ENOENT and nothing is created. */
    CHECK_RC(sys_open_named("absent.txt", F_WRONLY, &fd), CELL_ENOENT);
    CHECK(!host_exists("absent.txt"));
    CHECK_RC(sys_open_named("absent.txt", 0, &fd), CELL_ENOENT);
}

static void test_lseek(void)
{
    int32_t fd = -1;
    host_put("seek.bin", "0123456789");
    CHECK_RC(sys_open_named("seek.bin", F_RDWR, &fd), CELL_OK);

    CHECK_RC(sc(sys_fs_lseek, (uint32_t)fd, 4, CELL_FS_SEEK_SET, A_OUT, 0), CELL_OK);
    CHECK(rd64(A_OUT) == 4);
    CHECK_RC(sc(sys_fs_lseek, (uint32_t)fd, 2, CELL_FS_SEEK_CUR, A_OUT, 0), CELL_OK);
    CHECK(rd64(A_OUT) == 6);
    CHECK_RC(sc(sys_fs_lseek, (uint32_t)fd, (uint64_t)-3, CELL_FS_SEEK_END, A_OUT, 0), CELL_OK);
    CHECK(rd64(A_OUT) == 7);

    /* Before the start of the file: EINVAL, and the offset is unchanged. */
    CHECK_RC(sc(sys_fs_lseek, (uint32_t)fd, (uint64_t)-100, CELL_FS_SEEK_SET, A_OUT, 0), CELL_EINVAL);
    CHECK_RC(sc(sys_fs_lseek, (uint32_t)fd, 0, CELL_FS_SEEK_CUR, A_OUT, 0), CELL_OK);
    CHECK(rd64(A_OUT) == 7);
    CHECK_RC(sc(sys_fs_read, (uint32_t)fd, A_BUF, 3, A_OUT, 0), CELL_OK);
    CHECK(rd64(A_OUT) == 3 && memcmp(vm_base + A_BUF, "789", 3) == 0);

    /* An unknown whence. */
    CHECK_RC(sc(sys_fs_lseek, (uint32_t)fd, 0, 3, A_OUT, 0), CELL_EINVAL);

    /* 64-bit offsets: past 4 GiB, no truncation to 32 bits. */
    CHECK_RC(sc(sys_fs_lseek, (uint32_t)fd, 0x140000000ull, CELL_FS_SEEK_SET, A_OUT, 0), CELL_OK);
    CHECK(rd64(A_OUT) == 0x140000000ull);
    CHECK_RC(sc(sys_fs_close, (uint32_t)fd, 0, 0, 0, 0), CELL_OK);
}

static void test_dir_calls(void)
{
    CHECK_RC(sys_path_call(sys_fs_mkdir, "d1"), CELL_OK);
    CHECK_RC(sys_path_call(sys_fs_mkdir, "d1"), CELL_EEXIST);      /* a directory */
    host_put("afile", "x");
    CHECK_RC(sys_path_call(sys_fs_mkdir, "afile"), CELL_EEXIST);   /* a file of that name */

    CHECK_RC(sys_path_call(sys_fs_unlink, "d1"), CELL_EISDIR);
    CHECK_RC(sys_path_call(sys_fs_unlink, "nothing"), CELL_ENOENT);
    CHECK_RC(sys_path_call(sys_fs_unlink, "afile"), CELL_OK);
    CHECK(!host_exists("afile"));

    host_put("d1/inner", "x");
    CHECK_RC(sys_path_call(sys_fs_rmdir, "d1"), CELL_ENOTEMPTY);
    host_put("plain", "x");
    CHECK_RC(sys_path_call(sys_fs_rmdir, "plain"), CELL_ENOTDIR);
    CHECK_RC(sys_path_call(sys_fs_rmdir, "nothing"), CELL_ENOENT);
    CHECK_RC(sys_path_call(sys_fs_unlink, "d1/inner"), CELL_OK);
    CHECK_RC(sys_path_call(sys_fs_rmdir, "d1"), CELL_OK);
    CHECK(!host_exists("d1"));
}

static void test_readdir(void)
{
    CHECK_RC(sys_path_call(sys_fs_mkdir, "walk"), CELL_OK);
    host_put("walk/a.txt", "x");
    host_put("walk/b.txt", "y");

    char g[64] = "/dev_bdvd/walk";
    set_path(A_PATH, g);
    CHECK_RC(sc(sys_fs_opendir, A_PATH, A_OUT, 0, 0, 0), CELL_OK);
    const uint32_t dfd = rd32(A_OUT);

    int dot = 0, dotdot = 0, files = 0, entries = 0;
    for (int i = 0; i < 16; i++) {
        memset(vm_base + A_DIRENT, 0, 0x102);
        CHECK_RC(sc(sys_fs_readdir, dfd, A_DIRENT, A_OUT, 0, 0), CELL_OK);
        const uint64_t n = rd64(A_OUT);
        if (n == 0) break;
        CHECK(n == 0x102);                       /* one dirent, whatever the name */
        entries++;
        const uint8_t type = vm_base[A_DIRENT];
        const char* name = (const char*)vm_base + A_DIRENT + 2;
        CHECK(vm_base[A_DIRENT + 1] == strlen(name));
        if (!strcmp(name, "."))       { dot++;    CHECK(type == 1); }
        else if (!strcmp(name, "..")) { dotdot++; CHECK(type == 1); }
        else                          { files++;  CHECK(type == 2); }
    }
    CHECK(dot == 1 && dotdot == 1 && files == 2 && entries == 4);
    CHECK_RC(sc(sys_fs_closedir, dfd, 0, 0, 0, 0), CELL_OK);
    CHECK_RC(sc(sys_fs_closedir, dfd, 0, 0, 0, 0), CELL_EBADF);
}

static void test_table_full(void)
{
    host_put("full.bin", "x");
    int32_t fds[SYS_FS_FD_MAX + 1];
    int opened = 0;
    for (int i = 0; i < SYS_FS_FD_MAX; i++) {
        if (sys_open_named("full.bin", 0, &fds[i]) != 0) break;
        opened++;
    }
    CHECK(opened == SYS_FS_FD_MAX);
    /* Full: EMFILE (lv2's code), not ENOMEM, and nothing leaks. */
    int32_t extra = -1;
    CHECK_RC(sys_open_named("full.bin", 0, &extra), CELL_EMFILE);
    CHECK_RC(sc(sys_fs_close, (uint32_t)fds[10], 0, 0, 0, 0), CELL_OK);
    CHECK_RC(sys_open_named("full.bin", 0, &extra), CELL_OK);
    CHECK(extra == fds[10]);                     /* the freed slot is the one reused */
    CHECK_RC(sc(sys_fs_close, (uint32_t)extra, 0, 0, 0, 0), CELL_OK);
    for (int i = 0; i < opened; i++)
        if (i != 10) sc(sys_fs_close, (uint32_t)fds[i], 0, 0, 0, 0);

    /* Same for directory handles. */
    uint32_t dfds[SYS_FS_DIR_MAX];
    int dopened = 0;
    set_path(A_PATH, "/dev_bdvd/walk");
    for (int i = 0; i < SYS_FS_DIR_MAX; i++) {
        if ((int32_t)sc(sys_fs_opendir, A_PATH, A_OUT, 0, 0, 0) != 0) break;
        dfds[dopened++] = rd32(A_OUT);
    }
    CHECK(dopened == SYS_FS_DIR_MAX);
    CHECK_RC(sc(sys_fs_opendir, A_PATH, A_OUT, 0, 0, 0), CELL_EMFILE);
    for (int i = 0; i < dopened; i++) sc(sys_fs_closedir, dfds[i], 0, 0, 0, 0);
}

/* Concurrent opens: every thread gets a slot of its own. Each thread has its own
 * guest scratch area and ppu_context (the handlers only share the fd table). */
#define NTHREADS 8
#define PER_THREAD 12
static volatile int32_t s_thread_fds[NTHREADS][PER_THREAD];
static volatile int s_thread_ok[NTHREADS];

static void concurrent_body(int t)
{
    const uint32_t path_at = 0x8000u + (uint32_t)t * 0x400u;
    const uint32_t out_at  = path_at + 0x200u;
    set_path(path_at, "/dev_bdvd/conc.bin");
    int ok = 1;
    for (int i = 0; i < PER_THREAD; i++) {
        ppu_context ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.gpr[3] = path_at; ctx.gpr[4] = 0; ctx.gpr[5] = out_at;
        if ((int32_t)sys_fs_open(&ctx) != 0) { ok = 0; s_thread_fds[t][i] = -1; continue; }
        s_thread_fds[t][i] = (int32_t)rd32(out_at);
    }
    s_thread_ok[t] = ok;
}

#ifdef _WIN32
static DWORD WINAPI thread_main(LPVOID arg) { concurrent_body((int)(intptr_t)arg); return 0; }
#else
static void* thread_main(void* arg) { concurrent_body((int)(intptr_t)arg); return NULL; }
#endif

static void test_concurrent_open(void)
{
    host_put("conc.bin", "x");
#ifdef _WIN32
    HANDLE th[NTHREADS];
    for (int t = 0; t < NTHREADS; t++) th[t] = CreateThread(NULL, 0, thread_main, (LPVOID)(intptr_t)t, 0, NULL);
    for (int t = 0; t < NTHREADS; t++) { WaitForSingleObject(th[t], 30000); CloseHandle(th[t]); }
#else
    pthread_t th[NTHREADS];
    for (int t = 0; t < NTHREADS; t++) pthread_create(&th[t], NULL, thread_main, (void*)(intptr_t)t);
    for (int t = 0; t < NTHREADS; t++) pthread_join(th[t], NULL);
#endif
    int seen[SYS_FS_FD_MAX + 3] = {0};
    int total = 0;
    for (int t = 0; t < NTHREADS; t++) {
        CHECK(s_thread_ok[t]);
        for (int i = 0; i < PER_THREAD; i++) {
            const int32_t fd = s_thread_fds[t][i];
            if (fd < 3 || fd >= SYS_FS_FD_MAX + 3) { CHECK(0); continue; }
            CHECK(!seen[fd]);                    /* never the same slot twice */
            seen[fd] = 1;
            total++;
        }
    }
    CHECK(total == NTHREADS * PER_THREAD);
    for (int fd = 3; fd < SYS_FS_FD_MAX + 3; fd++)
        if (seen[fd]) sc(sys_fs_close, (uint32_t)fd, 0, 0, 0, 0);
}

int main(void)
{
    vm_base = (uint8_t*)calloc(1, VM_SIZE);
    if (!vm_base) return 2;

    /* A fresh directory under the temp area; the guest's "/dev_bdvd/" maps to it. */
    const char* tmp = getenv("TMPDIR");
    if (!tmp) tmp = getenv("TEMP");
    if (!tmp) tmp = getenv("TMP");
    if (!tmp) tmp = ".";
    snprintf(s_root, sizeof s_root, "%s/ps3recomp_sys_fs_test_%u", tmp,
#ifdef _WIN32
             (unsigned)GetCurrentProcessId());
#else
             (unsigned)getpid());
#endif
    for (char* c = s_root; *c; c++) if (*c == '\\') *c = '/';
    MKDIR(s_root);
    snprintf(g_sys_fs_root, sizeof g_sys_fs_root, "%s", s_root);

    test_flag_values();
    test_open_flags();
    test_lseek();
    test_dir_calls();
    test_readdir();
    test_table_full();
    test_concurrent_open();

    /* Remove what the test made, by name, and then its directory (which only
     * goes if everything else is gone). */
    {
        static const char* const files[] = {
            "created.txt", "excl.txt", "seek.bin", "full.bin", "conc.bin",
            "plain", "walk/a.txt", "walk/b.txt"
        };
        char p[600];
        for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
            snprintf(p, sizeof p, "%s/%s", s_root, files[i]);
            remove(p);
        }
        snprintf(p, sizeof p, "%s/walk", s_root);
#ifdef _WIN32
        _rmdir(p); _rmdir(s_root);
#else
        rmdir(p); rmdir(s_root);
#endif
    }

    printf("sys_fs: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
