/*
 * rsx_dlss_loader.h - explicit dynamic loader for the Folklore Vulkan DLSS bridge.
 *
 * Native Windows only. The loader owns only the loaded bridge module and the
 * identity storage (paths + project UUID). It never owns a VkDevice, an NGX
 * context or any rsx_dlss_vk instance.
 *
 * Usage contract:
 *  - Caller zero-initializes the rsx_dlss_loader and serializes all access.
 *  - rsx_dlss_loader_open returns 1 on success, 0 on failure. On failure the
 *    loader is left all-zero, except when it was already open (then it is
 *    left untouched) or when arguments are invalid (then it is not written).
 *  - error may be NULL only if error_capacity is 0.
 *  - Before rsx_dlss_loader_close the caller MUST destroy every rsx_dlss_vk
 *    created through the loader (loader->destroy) and drain all GPU work that
 *    references bridge/NGX resources. close only unloads loader->module and
 *    then zeroes the complete struct.
 *  - The loader never creates directories, alters DLL search settings,
 *    downloads anything or calls into NGX.
 */
#ifndef RSX_DLSS_LOADER_H
#define RSX_DLSS_LOADER_H

#ifndef _WIN32
#error "rsx_dlss_loader.h is native Windows only"
#endif

#include <windows.h>
#include <wchar.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "rsx_dlss_vk.h"

#define RSX_DLSS_LOADER_PATH_CAPACITY 32768

typedef rsx_dlss_status (*rsx_dlss_loader_instance_extensions_fn)(const rsx_dlss_identity *, rsx_dlss_extensions *, int32_t *);
typedef rsx_dlss_status (*rsx_dlss_loader_device_extensions_fn)(const rsx_dlss_identity *, VkInstance, VkPhysicalDevice, rsx_dlss_extensions *, int32_t *);
typedef rsx_dlss_status (*rsx_dlss_loader_create_fn)(const rsx_dlss_identity *, const rsx_dlss_device *, rsx_dlss_vk **, rsx_dlss_report *);
typedef void (*rsx_dlss_loader_destroy_fn)(rsx_dlss_vk *);
typedef rsx_dlss_status (*rsx_dlss_loader_get_report_fn)(const rsx_dlss_vk *, rsx_dlss_report *);
typedef rsx_dlss_status (*rsx_dlss_loader_optimal_size_fn)(rsx_dlss_vk *, uint32_t, uint32_t, rsx_dlss_quality, rsx_dlss_size *);
typedef rsx_dlss_status (*rsx_dlss_loader_configure_fn)(rsx_dlss_vk *, VkCommandBuffer, const rsx_dlss_config *);
typedef rsx_dlss_status (*rsx_dlss_loader_evaluate_fn)(rsx_dlss_vk *, VkCommandBuffer, const rsx_dlss_frame *);
typedef rsx_dlss_status (*rsx_dlss_loader_fg_instance_extensions_fn)(const rsx_dlss_identity *, rsx_dlss_extensions *, rsx_dlss_fg_report *);
typedef rsx_dlss_status (*rsx_dlss_loader_fg_device_extensions_fn)(const rsx_dlss_identity *, VkInstance, VkPhysicalDevice, rsx_dlss_extensions *, rsx_dlss_fg_report *);
typedef rsx_dlss_status (*rsx_dlss_loader_fg_capability_fn)(rsx_dlss_vk *, rsx_dlss_fg_report *);
typedef rsx_dlss_status (*rsx_dlss_loader_fg_get_report_fn)(const rsx_dlss_vk *, rsx_dlss_fg_report *);
typedef rsx_dlss_status (*rsx_dlss_loader_fg_configure_fn)(rsx_dlss_vk *, VkCommandBuffer, const rsx_dlss_fg_config *);
typedef rsx_dlss_status (*rsx_dlss_loader_fg_evaluate_fn)(rsx_dlss_vk *, VkCommandBuffer, const rsx_dlss_fg_frame *);

typedef struct rsx_dlss_loader {
    HMODULE module;
    wchar_t bridge_path[RSX_DLSS_LOADER_PATH_CAPACITY];
    wchar_t runtime_path[RSX_DLSS_LOADER_PATH_CAPACITY];
    wchar_t log_path[RSX_DLSS_LOADER_PATH_CAPACITY];
    char uuid[37];
    rsx_dlss_identity identity;
    rsx_dlss_loader_instance_extensions_fn instance_extensions;
    rsx_dlss_loader_device_extensions_fn device_extensions;
    rsx_dlss_loader_create_fn create;
    rsx_dlss_loader_destroy_fn destroy;
    rsx_dlss_loader_get_report_fn get_report;
    rsx_dlss_loader_optimal_size_fn optimal_size;
    rsx_dlss_loader_configure_fn configure;
    rsx_dlss_loader_evaluate_fn evaluate;
    rsx_dlss_loader_fg_instance_extensions_fn fg_instance_extensions;
    rsx_dlss_loader_fg_device_extensions_fn fg_device_extensions;
    rsx_dlss_loader_fg_capability_fn fg_capability;
    rsx_dlss_loader_fg_get_report_fn fg_get_report;
    rsx_dlss_loader_fg_configure_fn fg_configure;
    rsx_dlss_loader_fg_evaluate_fn fg_evaluate;
} rsx_dlss_loader;

typedef void (*rsx_dlss_loader_proc)(void);

static inline void rsx_dlss_loader__fail(char *error, size_t capacity, const char *subject, const char *what, DWORD code)
{
    if (!error || capacity == 0) return;
    if (code)
        snprintf(error, capacity, "rsx_dlss_loader: %s: %s (Win32 error %lu)", subject, what, (unsigned long)code);
    else
        snprintf(error, capacity, "rsx_dlss_loader: %s: %s", subject, what);
}

static inline size_t rsx_dlss_loader__wlen(const wchar_t *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n]) ++n;
    return n;
}

static inline int rsx_dlss_loader__sep(wchar_t c)
{
    return c == L'\\' || c == L'/';
}

/* Drive-root absolute (X:\...) or UNC (\\server\share...); device/verbatim prefixes rejected. */
static inline int rsx_dlss_loader__absolute_shape(const wchar_t *p)
{
    size_t i;
    if (((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) && p[1] == L':' && rsx_dlss_loader__sep(p[2]))
        return 1;
    if (!rsx_dlss_loader__sep(p[0]) || !rsx_dlss_loader__sep(p[1])) return 0;
    if (!p[2] || p[2] == L'?' || p[2] == L'.' || rsx_dlss_loader__sep(p[2])) return 0;
    for (i = 3; p[i] && !rsx_dlss_loader__sep(p[i]); ++i) {}
    if (!p[i]) return 0;
    return p[i + 1] != 0 && !rsx_dlss_loader__sep(p[i + 1]);
}

static inline int rsx_dlss_loader__wequal_ci(const wchar_t *a, int alen, const wchar_t *b, int blen)
{
    return CompareStringOrdinal(a, alen, b, blen, TRUE) == CSTR_EQUAL;
}

static inline int rsx_dlss_loader__canonical(const wchar_t *in, wchar_t *out, int directory, const char *subject, char *error, size_t capacity)
{
    size_t len = rsx_dlss_loader__wlen(in, RSX_DLSS_LOADER_PATH_CAPACITY);
    DWORD n;
    if (len == 0 || len >= RSX_DLSS_LOADER_PATH_CAPACITY) {
        rsx_dlss_loader__fail(error, capacity, subject, "path empty or too long", 0);
        return 0;
    }
    if (!rsx_dlss_loader__absolute_shape(in)) {
        rsx_dlss_loader__fail(error, capacity, subject, "path is not an absolute drive-root or UNC path", 0);
        return 0;
    }
    n = GetFullPathNameW(in, RSX_DLSS_LOADER_PATH_CAPACITY, out, NULL);
    if (n == 0) {
        rsx_dlss_loader__fail(error, capacity, subject, "GetFullPathNameW failed", GetLastError());
        return 0;
    }
    if (n >= RSX_DLSS_LOADER_PATH_CAPACITY) {
        rsx_dlss_loader__fail(error, capacity, subject, "canonical path too long", 0);
        return 0;
    }
    if (!rsx_dlss_loader__absolute_shape(out)) {
        rsx_dlss_loader__fail(error, capacity, subject, "canonical path is not a drive-root or UNC path", 0);
        return 0;
    }
    if (directory) {
        while (n > 0 && out[n - 1] == L'\\' && !(n == 3 && out[1] == L':')) out[--n] = 0;
    }
    return 1;
}

static inline int rsx_dlss_loader__check_file(const wchar_t *path, const char *subject, char *error, size_t capacity)
{
    DWORD attrs = GetFileAttributesW(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        rsx_dlss_loader__fail(error, capacity, subject, "file does not exist or is inaccessible", GetLastError());
        return 0;
    }
    if (attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE | FILE_ATTRIBUTE_REPARSE_POINT)) {
        rsx_dlss_loader__fail(error, capacity, subject, "not a regular file", 0);
        return 0;
    }
    return 1;
}

static inline int rsx_dlss_loader__check_dir(const wchar_t *path, const char *subject, char *error, size_t capacity)
{
    DWORD attrs = GetFileAttributesW(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        rsx_dlss_loader__fail(error, capacity, subject, "directory does not exist or is inaccessible", GetLastError());
        return 0;
    }
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY) || (attrs & FILE_ATTRIBUTE_DEVICE)) {
        rsx_dlss_loader__fail(error, capacity, subject, "not a directory", 0);
        return 0;
    }
    return 1;
}

static inline int rsx_dlss_loader__uuid_shape(const char *uuid)
{
    size_t i;
    for (i = 0; i < 36; ++i) {
        char c = uuid[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return 0;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return 0;
        }
    }
    return uuid[36] == 0;
}

static inline rsx_dlss_loader_proc rsx_dlss_loader__proc(HMODULE module, const char *name)
{
    FARPROC proc = GetProcAddress(module, name);
    return (rsx_dlss_loader_proc)proc;
}

static inline int rsx_dlss_loader_open(rsx_dlss_loader *loader, const wchar_t *bridge, const wchar_t *runtime, const wchar_t *logs,
                                       const char *uuid, char *error, size_t error_capacity)
{
    static const wchar_t runtime_dll[] = L"nvngx_dlss.dll";
    static const char *const names[8] = {
        "rsx_dlss_instance_extensions", "rsx_dlss_device_extensions", "rsx_dlss_create", "rsx_dlss_destroy",
        "rsx_dlss_get_report", "rsx_dlss_optimal_size", "rsx_dlss_configure", "rsx_dlss_evaluate"};
    static const char *const fg_names[6] = {
        "rsx_dlss_fg_instance_extensions", "rsx_dlss_fg_device_extensions", "rsx_dlss_fg_capability",
        "rsx_dlss_fg_get_report", "rsx_dlss_fg_configure", "rsx_dlss_fg_evaluate"};
    rsx_dlss_loader_proc procs[8];
    rsx_dlss_loader_proc fg_procs[6];
    wchar_t *scratch = NULL;
    const wchar_t *module_name;
    size_t len, i;
    DWORD got;

    if (!error && error_capacity != 0) return 0;
    if (error && error_capacity) error[0] = 0;
    if (!loader) {
        rsx_dlss_loader__fail(error, error_capacity, "loader", "null loader", 0);
        return 0;
    }
    if (!bridge || !runtime || !logs || !uuid) {
        rsx_dlss_loader__fail(error, error_capacity, "arguments", "null bridge, runtime, log or uuid argument", 0);
        return 0;
    }
    if (!rsx_dlss_loader__uuid_shape(uuid)) {
        rsx_dlss_loader__fail(error, error_capacity, "uuid", "expected 36-character 8-4-4-4-12 hex UUID", 0);
        return 0;
    }
    if (loader->module) {
        rsx_dlss_loader__fail(error, error_capacity, "loader", "already open", 0);
        return 0;
    }

    memset(loader, 0, sizeof(*loader));

    if (!rsx_dlss_loader__canonical(runtime, loader->runtime_path, 1, "runtime", error, error_capacity) ||
        !rsx_dlss_loader__check_dir(loader->runtime_path, "runtime", error, error_capacity))
        goto fail;

    /* Compose runtime\nvngx_dlss.dll in bridge_path (not yet used). */
    len = rsx_dlss_loader__wlen(loader->runtime_path, RSX_DLSS_LOADER_PATH_CAPACITY);
    if (len + 1 + (sizeof(runtime_dll) / sizeof(runtime_dll[0])) > RSX_DLSS_LOADER_PATH_CAPACITY) {
        rsx_dlss_loader__fail(error, error_capacity, "runtime", "nvngx_dlss.dll path too long", 0);
        goto fail;
    }
    memcpy(loader->bridge_path, loader->runtime_path, len * sizeof(wchar_t));
    if (len == 0 || loader->bridge_path[len - 1] != L'\\') loader->bridge_path[len++] = L'\\';
    memcpy(loader->bridge_path + len, runtime_dll, sizeof(runtime_dll));
    if (!rsx_dlss_loader__check_file(loader->bridge_path, "runtime nvngx_dlss.dll", error, error_capacity)) goto fail;
    memset(loader->bridge_path, 0, sizeof(loader->bridge_path));

    if (!rsx_dlss_loader__canonical(logs, loader->log_path, 1, "logs", error, error_capacity) ||
        !rsx_dlss_loader__check_dir(loader->log_path, "logs", error, error_capacity))
        goto fail;

    if (!rsx_dlss_loader__canonical(bridge, loader->bridge_path, 0, "bridge", error, error_capacity) ||
        !rsx_dlss_loader__check_file(loader->bridge_path, "bridge", error, error_capacity))
        goto fail;
    len = rsx_dlss_loader__wlen(loader->bridge_path, RSX_DLSS_LOADER_PATH_CAPACITY);
    if (len < 4 || !rsx_dlss_loader__wequal_ci(loader->bridge_path + len - 4, 4, L".dll", 4)) {
        rsx_dlss_loader__fail(error, error_capacity, "bridge", "not a .dll file", 0);
        goto fail;
    }

    loader->module = LoadLibraryExW(loader->bridge_path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!loader->module) {
        rsx_dlss_loader__fail(error, error_capacity, "bridge", "LoadLibraryExW failed", GetLastError());
        goto fail;
    }

    scratch = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, 2u * RSX_DLSS_LOADER_PATH_CAPACITY * sizeof(wchar_t));
    if (!scratch) {
        rsx_dlss_loader__fail(error, error_capacity, "bridge", "out of memory verifying module path", 0);
        goto fail;
    }
    got = GetModuleFileNameW(loader->module, scratch, RSX_DLSS_LOADER_PATH_CAPACITY);
    if (got == 0 || got >= RSX_DLSS_LOADER_PATH_CAPACITY) {
        rsx_dlss_loader__fail(error, error_capacity, "bridge", "GetModuleFileNameW failed", got ? 0 : GetLastError());
        goto fail;
    }
    module_name = scratch;
    if (wcsncmp(scratch, L"\\\\?\\UNC\\", 8) == 0) {
        scratch[6] = L'\\';
        module_name = scratch + 6;
    } else if (wcsncmp(scratch, L"\\\\?\\", 4) == 0) {
        module_name = scratch + 4;
    }
    got = GetFullPathNameW(module_name, RSX_DLSS_LOADER_PATH_CAPACITY, scratch + RSX_DLSS_LOADER_PATH_CAPACITY, NULL);
    if (got == 0 || got >= RSX_DLSS_LOADER_PATH_CAPACITY ||
        !rsx_dlss_loader__wequal_ci(scratch + RSX_DLSS_LOADER_PATH_CAPACITY, -1, loader->bridge_path, -1)) {
        rsx_dlss_loader__fail(error, error_capacity, "bridge", "loaded module path differs from requested bridge (alternate route)", 0);
        goto fail;
    }
    HeapFree(GetProcessHeap(), 0, scratch);
    scratch = NULL;

    for (i = 0; i < 8; ++i) {
        procs[i] = rsx_dlss_loader__proc(loader->module, names[i]);
        if (!procs[i]) {
            DWORD code = GetLastError();
            if (error && error_capacity)
                snprintf(error, error_capacity, "rsx_dlss_loader: bridge: missing export %s (Win32 error %lu)", names[i], (unsigned long)code);
            goto fail;
        }
    }
    loader->instance_extensions = (rsx_dlss_loader_instance_extensions_fn)procs[0];
    loader->device_extensions = (rsx_dlss_loader_device_extensions_fn)procs[1];
    loader->create = (rsx_dlss_loader_create_fn)procs[2];
    loader->destroy = (rsx_dlss_loader_destroy_fn)procs[3];
    loader->get_report = (rsx_dlss_loader_get_report_fn)procs[4];
    loader->optimal_size = (rsx_dlss_loader_optimal_size_fn)procs[5];
    loader->configure = (rsx_dlss_loader_configure_fn)procs[6];
    loader->evaluate = (rsx_dlss_loader_evaluate_fn)procs[7];

    /* Older SR-only bridges remain valid. Never expose a partial FG API. */
    for (i = 0; i < 6; ++i) {
        fg_procs[i] = rsx_dlss_loader__proc(loader->module, fg_names[i]);
        if (!fg_procs[i]) break;
    }
    if (i == 6) {
        loader->fg_instance_extensions = (rsx_dlss_loader_fg_instance_extensions_fn)fg_procs[0];
        loader->fg_device_extensions = (rsx_dlss_loader_fg_device_extensions_fn)fg_procs[1];
        loader->fg_capability = (rsx_dlss_loader_fg_capability_fn)fg_procs[2];
        loader->fg_get_report = (rsx_dlss_loader_fg_get_report_fn)fg_procs[3];
        loader->fg_configure = (rsx_dlss_loader_fg_configure_fn)fg_procs[4];
        loader->fg_evaluate = (rsx_dlss_loader_fg_evaluate_fn)fg_procs[5];
    }

    /* Identity pointers reference the final loader storage directly. */
    memcpy(loader->uuid, uuid, 36);
    loader->uuid[36] = 0;
    memset(&loader->identity, 0, sizeof(loader->identity));
    loader->identity.project_uuid = loader->uuid;
    loader->identity.log_directory_utf16 = (uint16_t *)loader->log_path;
    loader->identity.runtime_directory_utf16 = (uint16_t *)loader->runtime_path;
    return 1;

fail:
    if (scratch) HeapFree(GetProcessHeap(), 0, scratch);
    if (loader->module) FreeLibrary(loader->module);
    memset(loader, 0, sizeof(*loader));
    return 0;
}

/* Caller must destroy all rsx_dlss_vk contexts and drain the GPU before close. */
static inline void rsx_dlss_loader_close(rsx_dlss_loader *loader)
{
    if (!loader) return;
    if (loader->module) FreeLibrary(loader->module);
    memset(loader, 0, sizeof(*loader));
}

#endif /* RSX_DLSS_LOADER_H */
