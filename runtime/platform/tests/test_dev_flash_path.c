/*
 * ps3recomp - test for runtime/platform/ps3_dev_flash.c (the host path of a
 * file under /dev_flash, for a PS3_DEV_FLASH of one or several roots)
 *
 *   - one root: the path is root + "/" + rel, whether or not the file exists;
 *   - several roots: the first one that holds the file wins, wherever it is in
 *     the list, and a root that does not have it is skipped;
 *   - a file that no root has resolves under the first root, so the caller's
 *     open fails the way a missing file does;
 *   - an empty entry in the list is skipped, and a trailing separator is fine.
 *
 * Build (any host):
 *   cc -std=gnu17 -Wall -Wextra -o test_dev_flash_path \
 *      runtime/platform/tests/test_dev_flash_path.c runtime/platform/ps3_dev_flash.c
 *
 * Exit status is the number of failed checks, so it works as a CI step.
 */
#include "../ps3_dev_flash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#  include <direct.h>
#  include <windows.h>
#  define MKDIR(p) _mkdir(p)
#  define SEP ";"
#else
#  include <unistd.h>
#  define MKDIR(p) mkdir((p), 0755)
#  define SEP ":"
#endif

static int g_failures;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (cond) {                                                           \
            printf("[PASS] " __VA_ARGS__);                                    \
            printf("\n");                                                     \
        } else {                                                              \
            printf("[FAIL] " __VA_ARGS__);                                    \
            printf("   (%s:%d)\n", __FILE__, __LINE__);                        \
            g_failures++;                                                     \
        }                                                                     \
    } while (0)

static void touch(const char* path)
{
    FILE* f = fopen(path, "wb");
    if (f) { fputs("x", f); fclose(f); }
}

int main(void)
{
    char base[512], a[600], b[600], c[600], f1[700], f2[700], roots[2000], out[1024], want[1024];

#ifdef _WIN32
    const char* tmp = getenv("TEMP");
    snprintf(base, sizeof base, "%s/ps3_dev_flash_test_%lu", tmp ? tmp : ".", (unsigned long)GetCurrentProcessId());
#else
    snprintf(base, sizeof base, "/tmp/ps3_dev_flash_test_%ld", (long)getpid());
#endif
    snprintf(a, sizeof a, "%s_a", base);
    snprintf(b, sizeof b, "%s_b", base);
    snprintf(c, sizeof c, "%s_c", base);
    MKDIR(a); MKDIR(b); MKDIR(c);
    snprintf(f1, sizeof f1, "%s/only_in_b.bin", b);
    touch(f1);
    snprintf(f1, sizeof f1, "%s/in_a_and_c.bin", a); touch(f1);
    snprintf(f2, sizeof f2, "%s/in_a_and_c.bin", c); touch(f2);
    snprintf(f1, sizeof f1, "%s/only_in_c.bin", c); touch(f1);

    printf("-- one root\n");
    ps3_dev_flash_path(out, sizeof out, a, "sys/external/x.pic");
    snprintf(want, sizeof want, "%s/sys/external/x.pic", a);
    CHECK(!strcmp(out, want), "root + rel for a file that is not there (%s)", out);

    printf("-- several roots\n");
    snprintf(roots, sizeof roots, "%s" SEP "%s" SEP "%s", a, b, c);
    ps3_dev_flash_path(out, sizeof out, roots, "only_in_b.bin");
    snprintf(want, sizeof want, "%s/only_in_b.bin", b);
    CHECK(!strcmp(out, want), "a file only the middle root has is found there");

    ps3_dev_flash_path(out, sizeof out, roots, "only_in_c.bin");
    snprintf(want, sizeof want, "%s/only_in_c.bin", c);
    CHECK(!strcmp(out, want), "a file only the last root has is found there");

    ps3_dev_flash_path(out, sizeof out, roots, "in_a_and_c.bin");
    snprintf(want, sizeof want, "%s/in_a_and_c.bin", a);
    CHECK(!strcmp(out, want), "the first root that has the file wins");

    snprintf(roots, sizeof roots, "%s" SEP "%s", c, a);
    ps3_dev_flash_path(out, sizeof out, roots, "in_a_and_c.bin");
    snprintf(want, sizeof want, "%s/in_a_and_c.bin", c);
    CHECK(!strcmp(out, want), "and the order of the list decides");

    snprintf(roots, sizeof roots, "%s" SEP "%s", b, c);
    ps3_dev_flash_path(out, sizeof out, roots, "nowhere.bin");
    snprintf(want, sizeof want, "%s/nowhere.bin", b);
    CHECK(!strcmp(out, want), "a file no root has resolves under the first root");

    printf("-- list syntax\n");
    snprintf(roots, sizeof roots, SEP "%s" SEP SEP "%s" SEP, a, c);
    ps3_dev_flash_path(out, sizeof out, roots, "only_in_c.bin");
    snprintf(want, sizeof want, "%s/only_in_c.bin", c);
    CHECK(!strcmp(out, want), "empty entries and a trailing separator are skipped (%s)", out);

    snprintf(roots, sizeof roots, SEP SEP "%s", b);
    ps3_dev_flash_path(out, sizeof out, roots, "nowhere.bin");
    snprintf(want, sizeof want, "%s/nowhere.bin", b);
    CHECK(!strcmp(out, want), "with no file, the first non-empty entry is the root");

    char small[16];
    ps3_dev_flash_path(small, sizeof small, a, "a/very/long/relative/path.bin");
    CHECK(strlen(small) == sizeof small - 1, "a small buffer is cut, not overrun");

    /* clean up */
    const char* dirs[3] = { a, b, c };
    const char* files[4] = { "only_in_b.bin", "in_a_and_c.bin", "only_in_c.bin", NULL };
    for (int d = 0; d < 3; d++) {
        for (int i = 0; files[i]; i++) {
            snprintf(f1, sizeof f1, "%s/%s", dirs[d], files[i]);
            remove(f1);
        }
#ifdef _WIN32
        _rmdir(dirs[d]);
#else
        rmdir(dirs[d]);
#endif
    }
    return g_failures;
}
