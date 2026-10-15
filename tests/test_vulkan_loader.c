/* Exercises rsx_vulkan_loader.h against the real system Vulkan loader.
 * A missing loader is a failure, not a skip. No instance or device is created. */
#include "../libs/video/rsx_vulkan_loader.h"

#include <stdio.h>

#define SENTINEL 0xA5

static int g_failures = 0;

#define CHECK(cond, what)                                                        \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, what, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

static int untouched(const char* buf, size_t from, size_t to)
{
    size_t i;
    for (i = from; i < to; ++i)
        if ((unsigned char)buf[i] != SENTINEL)
            return 0;
    return 1;
}

static void test_null_handle(void)
{
    char buf[64];
    const char* nul;

    memset(buf, SENTINEL, sizeof buf);
    CHECK(rsx_vk_library_get_proc(NULL, buf, 0) == NULL, "NULL handle, cap 0 returns NULL");
    CHECK(untouched(buf, 0, sizeof buf), "cap 0 writes nothing");

    CHECK(rsx_vk_library_get_proc(NULL, NULL, 0) == NULL, "NULL handle, NULL buffer, cap 0");
    CHECK(rsx_vk_library_get_proc(NULL, NULL, 32) == NULL, "NULL handle, NULL buffer, cap 32");

    memset(buf, SENTINEL, sizeof buf);
    CHECK(rsx_vk_library_get_proc(NULL, buf, 1) == NULL, "NULL handle, cap 1 returns NULL");
    CHECK(buf[0] == '\0', "cap 1 yields empty string");
    CHECK(untouched(buf, 1, sizeof buf), "cap 1 writes only the terminator");

    memset(buf, SENTINEL, sizeof buf);
    CHECK(rsx_vk_library_get_proc(NULL, buf, 8) == NULL, "NULL handle, cap 8 returns NULL");
    CHECK(memchr(buf, '\0', 8) == buf + 7, "cap 8 truncates to 7 chars + NUL");
    CHECK(untouched(buf, 8, sizeof buf), "cap 8 stays in bounds");

    memset(buf, SENTINEL, sizeof buf);
    CHECK(rsx_vk_library_get_proc(NULL, buf, 48) == NULL, "NULL handle, cap 48 returns NULL");
    nul = (const char*)memchr(buf, '\0', 48);
    CHECK(nul != NULL, "cap 48 is NUL-terminated within cap");
    CHECK(nul != NULL && nul > buf, "cap 48 error text is non-empty");
    CHECK(untouched(buf, 48, sizeof buf), "cap 48 stays in bounds");
    if (nul)
        printf("NULL-handle error text: \"%s\"\n", buf);
}

static void test_real_loader(int round)
{
    char err[256];
    rsx_vk_library lib;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkVoidFunction create;

    memset(err, SENTINEL, sizeof err);
    lib = rsx_vk_library_open(err, sizeof err - 16);
    CHECK(memchr(err, '\0', sizeof err - 16) != NULL, "open error buffer is NUL-terminated");
    CHECK(untouched(err, sizeof err - 16, sizeof err), "open error buffer stays in bounds");
    if (!lib) {
        fprintf(stderr, "FAIL round %d: system Vulkan loader not available: %s\n", round,
                memchr(err, '\0', sizeof err) ? err : "(unterminated error text)");
        ++g_failures;
        return;
    }
    CHECK(err[0] == '\0', "successful open leaves empty error text");

    memset(err, SENTINEL, sizeof err);
    gipa = rsx_vk_library_get_proc(lib, err, sizeof err);
    if (!gipa) {
        fprintf(stderr, "FAIL round %d: could not resolve vkGetInstanceProcAddr: %s\n", round,
                memchr(err, '\0', sizeof err) ? err : "(unterminated error text)");
        ++g_failures;
        rsx_vk_library_close(lib);
        return;
    }
    CHECK(err[0] == '\0', "successful get_proc leaves empty error text");
    CHECK(rsx_vk_library_get_proc(lib, NULL, 0) == gipa, "get_proc without error buffer resolves same entry point");

    create = gipa(NULL, "vkCreateInstance");
    CHECK(create != NULL, "vkCreateInstance resolvable through vkGetInstanceProcAddr");
    if (create)
        printf("round %d: loader opened, vkGetInstanceProcAddr and vkCreateInstance resolved\n", round);

    rsx_vk_library_close(lib);
}

int main(void)
{
    int round;

    rsx_vk_library_close(NULL);
    test_null_handle();
    for (round = 1; round <= 2; ++round)
        test_real_loader(round);

    if (g_failures) {
        fprintf(stderr, "test_vulkan_loader: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_vulkan_loader: all checks passed\n");
    return 0;
}
