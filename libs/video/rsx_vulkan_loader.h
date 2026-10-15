/* Minimal runtime Vulkan loader binding for the RSX backend (MIT).
 * Resolves only vkGetInstanceProcAddr; everything else is fetched through it.
 * Header-only, no global state, no link-time dependency on the Vulkan loader. */
#ifndef RSX_VULKAN_LOADER_H
#define RSX_VULKAN_LOADER_H

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <stddef.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif
typedef HMODULE rsx_vk_library;
#else
#include <dlfcn.h>
typedef void* rsx_vk_library;
/* POSIX guarantees object and function pointers share a representation. */
typedef char rsx_vk_fnptr_size_check[sizeof(void*) == sizeof(PFN_vkGetInstanceProcAddr) ? 1 : -1];
#endif

/* Bounded error text: *len is always <= cap - 1 and error[*len] is NUL. */
static inline void rsx_vk_error_begin(char* error, size_t cap, size_t* len)
{
    *len = 0;
    if (error && cap > 0)
        error[0] = '\0';
}

static inline void rsx_vk_error_append(char* error, size_t cap, size_t* len, const char* text)
{
    size_t n, room;
    if (!error || cap == 0 || !text)
        return;
    n = strlen(text);
    room = cap - 1 - *len;
    if (n > room)
        n = room;
    memcpy(error + *len, text, n);
    *len += n;
    error[*len] = '\0';
}

#ifdef _WIN32
static inline void rsx_vk_error_append_win32(char* error, size_t cap, size_t* len, DWORD code)
{
    char num[16];
    char msg[256];
    size_t i = sizeof num;
    DWORD rest = code;
    DWORD n;

    num[--i] = '\0';
    do {
        num[--i] = (char)('0' + rest % 10);
        rest /= 10;
    } while (rest && i > 0);
    rsx_vk_error_append(error, cap, len, "Win32 error ");
    rsx_vk_error_append(error, cap, len, num + i);

    n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0,
                       msg, (DWORD)sizeof msg, NULL);
    while (n > 0 && (msg[n - 1] == '\r' || msg[n - 1] == '\n' || msg[n - 1] == ' ' || msg[n - 1] == '.'))
        msg[--n] = '\0';
    if (n > 0) {
        rsx_vk_error_append(error, cap, len, ": ");
        rsx_vk_error_append(error, cap, len, msg);
    }
}
#endif

/* Opens the system Vulkan loader. Returns NULL and fills error on failure. */
static inline rsx_vk_library rsx_vk_library_open(char* error, size_t cap)
{
    size_t len;
    rsx_vk_error_begin(error, cap, &len);
#ifdef _WIN32
    {
        /* System32 only: never the working directory or PATH. */
        HMODULE lib = LoadLibraryExW(L"vulkan-1.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!lib) {
            DWORD code = GetLastError();
            rsx_vk_error_append(error, cap, &len, "failed to load vulkan-1.dll from System32: ");
            rsx_vk_error_append_win32(error, cap, &len, code);
        }
        return lib;
    }
#else
    {
        static const char* const names[] = { "libvulkan.so.1", "libvulkan.so" };
        size_t i;
        rsx_vk_error_append(error, cap, &len, "failed to load Vulkan loader");
        for (i = 0; i < sizeof names / sizeof names[0]; ++i) {
            void* lib;
            const char* detail;
            (void)dlerror();
            lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
            if (lib) {
                rsx_vk_error_begin(error, cap, &len);
                return lib;
            }
            detail = dlerror();
            rsx_vk_error_append(error, cap, &len, i == 0 ? ": " : "; ");
            rsx_vk_error_append(error, cap, &len, detail ? detail : names[i]);
        }
        return NULL;
    }
#endif
}

/* Resolves vkGetInstanceProcAddr. Never closes lib; the caller still owns it. */
static inline PFN_vkGetInstanceProcAddr rsx_vk_library_get_proc(rsx_vk_library lib, char* error, size_t cap)
{
    size_t len;
    rsx_vk_error_begin(error, cap, &len);
    if (!lib) {
        rsx_vk_error_append(error, cap, &len, "cannot resolve vkGetInstanceProcAddr: Vulkan library handle is NULL");
        return NULL;
    }
#ifdef _WIN32
    {
        FARPROC proc = GetProcAddress(lib, "vkGetInstanceProcAddr");
        if (!proc) {
            DWORD code = GetLastError();
            rsx_vk_error_append(error, cap, &len, "vkGetInstanceProcAddr not found in vulkan-1.dll: ");
            rsx_vk_error_append_win32(error, cap, &len, code);
            return NULL;
        }
        /* Route through the generic function pointer type to avoid cast-function-type warnings. */
        return (PFN_vkGetInstanceProcAddr)(void (*)(void))proc;
    }
#else
    {
        PFN_vkGetInstanceProcAddr fn;
        void* sym;
        (void)dlerror();
        sym = dlsym(lib, "vkGetInstanceProcAddr");
        if (!sym) {
            const char* detail = dlerror();
            rsx_vk_error_append(error, cap, &len, "vkGetInstanceProcAddr not found in Vulkan loader: ");
            rsx_vk_error_append(error, cap, &len, detail ? detail : "symbol resolved to NULL");
            return NULL;
        }
        memcpy(&fn, &sym, sizeof fn);
        return fn;
    }
#endif
}

static inline void rsx_vk_library_close(rsx_vk_library lib)
{
    if (!lib)
        return;
#ifdef _WIN32
    FreeLibrary(lib);
#else
    dlclose(lib);
#endif
}

#endif /* RSX_VULKAN_LOADER_H */
