#define _POSIX_C_SOURCE 200809L
#include "ps3emu/strict_fs.h"
#include <sys/wait.h>

/* Every fixture, including the simulated outside tree, lives below the unique
 * directory in argv[1]. Cleanup only unlinks explicitly recorded fixture names. */
static char files[32][4096], dirs[16][4096];
static size_t file_count, dir_count;
static int failures, checks, skips;
static void check(int ok, const char* name) {
    ++checks;
    if (!ok) { fprintf(stderr, "FAIL %s (errno=%d)\n", name, errno); ++failures; }
}
static void cleanup(void) {
    while (file_count) {
        int result = unlink(files[--file_count]);
        check(!result || errno == ENOENT, "cleanup recorded fixture file");
    }
    while (dir_count) check(rmdir(dirs[--dir_count]) == 0, "cleanup recorded fixture directory");
}
static void require(int ok, const char* name) {
    if (!ok) { perror(name); exit(2); }
}
static void join(char* out, const char* parent, const char* leaf) {
    int n = snprintf(out, 4096, "%s/%s", parent, leaf);
    require(n > 0 && n < 4096, "fixture path");
}
static void remember_file(const char* path) {
    require(file_count < 32, "file capacity");
    strcpy(files[file_count++], path);
}
static void make_dir(const char* path) {
    require(dir_count < 16 && mkdir(path, 0700) == 0, "mkdir fixture");
    strcpy(dirs[dir_count++], path);
}
static void make_file(const char* path) {
    FILE* f = fopen(path, "wx");
    require(f != NULL, "create fixture");
    require(fputs("original", f) >= 0 && fclose(f) == 0, "write fixture");
    remember_file(path);
}
static int file_original(const char* path) {
    char buf[16] = {0};
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf, f);
    int result = !ferror(f) && n == 8 && !memcmp(buf, "original", 8);
    fclose(f);
    return result;
}
static void policy_child(const char* root, const char* path, int readonly, int stream) {
    fflush(NULL);
    pid_t pid = fork();
    require(pid >= 0, "fork");
    if (!pid) {
        if (setenv("PS3_STRICT_FS_WRITE_ROOT", root, 1) ||
            setenv("PS3_STRICT_FS_READONLY", readonly ? "1" : "0", 1)) _Exit(3);
        if (stream) {
            FILE* f = fopen(path, "r+");
            if (!f) _Exit(4);
            ps3_strict_fs_fwrite("changed", 1, 7, f);
            fclose(f);
        } else {
            FILE* f = ps3_strict_fs_fopen(path, "w");
            if (f) fclose(f);
        }
        _Exit(5);
    }
    int status;
    require(waitpid(pid, &status, 0) == pid, "waitpid");
    check(WIFEXITED(status) && WEXITSTATUS(status) == 86, "policy stops child before mutation");
    check(file_original(path), "rejected write preserved original");
}
int main(int argc, char** argv) {
    if (argc != 2) { fprintf(stderr, "usage: test_strict_fs_posix OWNED_PARENT\n"); return 2; }
    char base[4096], root[4096], sibling[4096], sub[4096], file[4096], outside[4096];
    char path[4096], alias[4096], link_path[4096], renamed[4096];
    join(base, argv[1], "strict-fs-XXXXXX");
    require(mkdtemp(base) != NULL, "mkdtemp");
    strcpy(dirs[dir_count++], base);
    require(atexit(cleanup) == 0, "register fixture cleanup");
    join(root, base, "allowed"); make_dir(root);
    join(sibling, base, "allowed-sibling"); make_dir(sibling);
    join(sub, root, "sub"); make_dir(sub);
    join(file, sub, "file"); make_file(file);
    join(outside, sibling, "outside"); make_file(outside);
#define ALLOW(p, label) check(ps3_strict_fs_posix_path(root, (p)) == NULL, (label))
#define DENY(p, label) check(ps3_strict_fs_posix_path(root, (p)) != NULL, (label))
    ALLOW(root, "exact root"); ALLOW(file, "existing file"); ALLOW(sub, "existing directory");
    join(path, root, "new/deep/file"); ALLOW(path, "missing descendants");
    join(path, root, "new/deep/"); ALLOW(path, "new directory trailing slash");
    join(path, root, "sub/"); ALLOW(path, "existing directory trailing slash");
    join(path, file, ""); DENY(path, "file trailing slash");
    join(path, file, "child"); DENY(path, "regular file ancestor");
    DENY(sibling, "prefix sibling"); DENY(base, "parent outside"); DENY(NULL, "null path");
    DENY("relative", "relative path"); DENY("", "empty path");
    join(path, root, "../allowed/sub/file"); DENY(path, "dotdot component");
    join(path, root, "./sub/file"); DENY(path, "dot component");
    join(path, root, "/sub/file"); DENY(path, "repeated separator");
    join(path, root, "new/../sub/file"); DENY(path, "dotdot after absent ancestor");
    check(ps3_strict_fs_posix_path(NULL, file) != NULL, "null root");
    check(ps3_strict_fs_posix_path(file, file) != NULL, "regular file root");
    join(path, base, "missing-root");
    check(ps3_strict_fs_posix_path(path, path) != NULL, "nonexistent root");
    join(path, root, "utf8-\xc3\xa9 space"); make_file(path); ALLOW(path, "UTF8 and spaces");
    memset(path, 'a', sizeof path); path[0] = '/'; path[4095] = 0;
    DENY(path, "long component");
    char huge[4097]; memset(huge, 'a', sizeof huge); huge[0] = '/'; huge[4096] = 0;
    DENY(huge, "overlong total path");
    join(alias, base, "root-alias"); require(!symlink(root, alias), "symlink root"); remember_file(alias);
    check(ps3_strict_fs_posix_path(alias, file) != NULL, "symlink root rejected");
    join(path, alias, "sub");
    check(ps3_strict_fs_posix_path(path, path) != NULL, "symlink above root rejected");
    join(link_path, root, "alias"); require(!symlink(sub, link_path), "symlink directory"); remember_file(link_path);
    DENY(link_path, "symlink leaf"); join(path, link_path, "file"); DENY(path, "symlink ancestor");
    join(path, root, "dangling"); require(!symlink(outside, path), "symlink outside"); remember_file(path);
    DENY(path, "symlink outside rejected");
    join(path, root, "broken"); require(!symlink("absent", path), "broken symlink"); remember_file(path);
    DENY(path, "broken symlink rejected");
    join(path, root, "hardlink"); require(!link(file, path), "hardlink"); remember_file(path);
    DENY(path, "hardlink leaf"); DENY(file, "original hardlinked file");
    FILE* f = fopen(file, "r+"); require(f != NULL, "open linked stream");
    check(ps3_strict_fs_posix_stream(root, f) != NULL, "hardlinked stream denied");
    require(!unlink(path), "unlink hardlink");
    check(ps3_strict_fs_posix_stream(root, f) == NULL, "regular live stream");
    require(!fclose(f), "close stream");
    join(path, root, "fifo");
    if (!mkfifo(path, 0600)) {
        remember_file(path); DENY(path, "special file");
    } else if (errno == ENOTSUP || errno == EOPNOTSUPP) {
        ++skips; puts("SKIP FIFO path: fixture filesystem does not support FIFO creation");
    } else require(0, "fifo");
    check(ps3_strict_fs_posix_stream(root, NULL) != NULL, "null stream");
    int pipes[2]; require(!pipe(pipes), "pipe"); f = fdopen(pipes[0], "r"); require(f != NULL, "fdopen");
    check(ps3_strict_fs_posix_stream(root, f) != NULL, "nonregular stream"); fclose(f); close(pipes[1]);
    f = fopen(outside, "r+"); require(f != NULL, "outside stream");
    check(ps3_strict_fs_posix_stream(root, f) != NULL, "outside stream denied"); fclose(f);
    join(path, root, "deleted"); make_file(path); f = fopen(path, "r+"); require(f != NULL, "deleted stream");
    require(!unlink(path), "delete open file");
    check(ps3_strict_fs_posix_stream(root, f) != NULL, "deleted stream denied"); fclose(f);
    join(path, root, "moved"); make_file(path); f = fopen(path, "r+"); require(f != NULL, "moved stream");
    join(renamed, sibling, "moved"); require(!rename(path, renamed), "move outside"); remember_file(renamed);
    check(ps3_strict_fs_posix_stream(root, f) != NULL, "moved outside stream denied"); fclose(f);
    policy_child(root, outside, 0, 0); policy_child(root, outside, 0, 1);
    policy_child(root, file, 1, 0); policy_child(root, file, 1, 1);
    join(path, root, "dangling"); policy_child(root, path, 0, 0);
    require(!setenv("PS3_STRICT_FS_WRITE_ROOT", root, 1), "set root");
    require(!setenv("PS3_STRICT_FS_READONLY", "0", 1), "set readonly");
    join(path, root, "wrapped"); f = ps3_strict_fs_fopen(path, "wx"); require(f != NULL, "wrapped fopen"); remember_file(path);
    check(ps3_strict_fs_fwrite("test", 1, 4, f) == 4, "wrapped fwrite"); require(!fclose(f), "wrapped fclose");
    join(renamed, root, "committed"); remember_file(renamed);
    check(ps3_strict_fs_commit_new_file(path, renamed) == 0, "commit new contained file");
    ALLOW(renamed, "commit leaves single linked file");
    make_file(path);
    check(ps3_strict_fs_commit_new_file(path, renamed) == -1 && errno == EEXIST, "commit never overwrites");
    check(ps3_strict_fs_replace_file(path, renamed) == 0, "atomic replacement");
    check(file_original(renamed), "replacement content");
    check(ps3_strict_fs_mkdir(base, 0700) == -1 && errno == EEXIST, "existing outer ancestor is mutation-free");
    join(path, root, "wrapped-dir");
    check(ps3_strict_fs_mkdir(path, 0700) == 0, "wrapped mkdir"); strcpy(dirs[dir_count++], path);
    check(ps3_strict_fs_unlink(renamed) == 0, "wrapped unlink");
    cleanup();
    printf("strict POSIX filesystem: %d checks, %d failures, %d skipped\n", checks, failures, skips);
    return failures ? 1 : 0;
}
