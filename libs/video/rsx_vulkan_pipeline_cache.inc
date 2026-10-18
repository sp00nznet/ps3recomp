/* Persistent Vulkan pipeline cache.
 *
 * Included by rsx_vulkan_backend.c. With PS3RECOMP_SHADER_CACHE_DIR=<dir>
 * (the same directory as the SPIR-V cache) the driver's pipeline cache is
 * loaded from <dir>/vk-pipeline-cache.bin when the device is created and
 * every pipeline create call passes it, so a pipeline seen in an earlier
 * session is rebuilt from the driver's compiled form instead of compiling the
 * shaders again (the remaining multi-second stall when a scene first draws).
 *
 * The file is our header (magic, version, payload length, two payload hashes)
 * followed by the driver's blob. The blob is only handed to the driver when
 * our header checks out and the driver's own header names this device's
 * vendorID, deviceID and pipelineCacheUUID; anything else starts an empty
 * cache, so a different GPU or driver never sees stale data. New pipelines
 * mark the cache dirty; it is written via a temporary file and rename, on a
 * background thread at most every PCACHE_SAVE_INTERVAL_MS while dirty, and
 * once more at shutdown. Unset, no cache object exists and every create call
 * gets VK_NULL_HANDLE exactly as before. Any failure falls back to the same.
 */

#if defined(_WIN32)
/* Declared here rather than through <windows.h>, whose macros (near, far,
 * min, max...) would leak into the rest of the backend. */
__declspec(dllimport) int __stdcall MoveFileExA(const char*, const char*, unsigned long);
__declspec(dllimport) unsigned long __stdcall GetCurrentProcessId(void);
#ifndef MOVEFILE_REPLACE_EXISTING
#define MOVEFILE_REPLACE_EXISTING 0x1
#endif
#else
#include <unistd.h>
#endif

#define PCACHE_FILE_VERSION 1u
#define PCACHE_SAVE_INTERVAL_MS 10000u
#define PCACHE_MAX_BYTES (512u * 1024u * 1024u)

typedef struct {
    char magic[8];                 /* "RSXVKPC\1" */
    u32 version;                   /* PCACHE_FILE_VERSION */
    u32 reserved;
    unsigned long long bytes;      /* driver blob length */
    unsigned long long hash_a;     /* FNV-1a of the blob */
    unsigned long long hash_b;     /* independent mix of the blob */
} vk_pcache_header;

static const char k_pcache_magic[8] = { 'R', 'S', 'X', 'V', 'K', 'P', 'C', 1 };

static struct {
    VkPipelineCache cache;
    char path[1024];
    VkPhysicalDeviceProperties props;
    SDL_atomic_t dirty;            /* pipelines created since the last save started */
    SDL_atomic_t saving;           /* a background save is running */
    SDL_atomic_t created, loaded_ok;
    Uint32 last_save_ms;
    unsigned saves, save_failures;
} s_pcache;

static PFN_vkCreatePipelineCache pvkCreatePipelineCache;
static PFN_vkDestroyPipelineCache pvkDestroyPipelineCache;
static PFN_vkGetPipelineCacheData pvkGetPipelineCacheData;

static unsigned long long vk_pcache_fnv(const void* p, size_t n)
{
    const unsigned char* b = (const unsigned char*)p;
    unsigned long long h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

static unsigned long long vk_pcache_mix(const void* p, size_t n)
{
    const unsigned char* b = (const unsigned char*)p;
    unsigned long long h = 0x243F6A8885A308D3ull;
    for (size_t i = 0; i < n; i++) { h = (h ^ b[i]) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    return h;
}

/* The pipeline cache to pass to vkCreate*Pipelines (VK_NULL_HANDLE if off). */
static VkPipelineCache vk_pcache(void) { return s_pcache.cache; }

/* Call after every successful pipeline creation. */
static void vk_pcache_created(void)
{
    if (!s_pcache.cache) return;
    SDL_AtomicAdd(&s_pcache.dirty, 1);
    SDL_AtomicAdd(&s_pcache.created, 1);
}

/* Driver header (VkPipelineCacheHeaderVersionOne): length, version, vendor,
 * device, 16-byte UUID; 32 bytes, little-endian. */
static int vk_pcache_blob_matches(const unsigned char* blob, size_t n, const VkPhysicalDeviceProperties* p)
{
    if (n < 32) return 0;
    u32 len, ver, vendor, device;
    memcpy(&len, blob, 4); memcpy(&ver, blob + 4, 4);
    memcpy(&vendor, blob + 8, 4); memcpy(&device, blob + 12, 4);
    return len >= 32 && len <= n && ver == (u32)VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
           vendor == p->vendorID && device == p->deviceID &&
           memcmp(blob + 16, p->pipelineCacheUUID, VK_UUID_SIZE) == 0;
}

/* Reads and validates the file; returns a malloc'd driver blob or NULL. */
static unsigned char* vk_pcache_read(const char* path, size_t* out_n, const char** why)
{
    *out_n = 0;
    FILE* f = fopen(path, "rb");
    if (!f) { *why = "no file"; return NULL; }
    vk_pcache_header h;
    unsigned char* blob = NULL;
    *why = "bad header";
    if (fread(&h, sizeof h, 1, f) == 1 && memcmp(h.magic, k_pcache_magic, sizeof k_pcache_magic) == 0 &&
        h.version == PCACHE_FILE_VERSION && h.bytes >= 32 && h.bytes <= PCACHE_MAX_BYTES) {
        blob = (unsigned char*)malloc((size_t)h.bytes);
        *why = "short or damaged payload";
        if (blob && (fread(blob, 1, (size_t)h.bytes, f) != (size_t)h.bytes || fgetc(f) != EOF ||
                     vk_pcache_fnv(blob, (size_t)h.bytes) != h.hash_a ||
                     vk_pcache_mix(blob, (size_t)h.bytes) != h.hash_b)) {
            free(blob); blob = NULL;
        }
    }
    fclose(f);
    if (blob) *out_n = (size_t)h.bytes;
    return blob;
}

static int vk_pcache_write(const char* path, const unsigned char* blob, size_t n)
{
    char tmp[1100];
#if defined(_WIN32)
    snprintf(tmp, sizeof tmp, "%s.%lu.tmp", path, (unsigned long)GetCurrentProcessId());
#else
    snprintf(tmp, sizeof tmp, "%s.%lu.tmp", path, (unsigned long)getpid());
#endif
    FILE* f = fopen(tmp, "wb");
    if (!f) return -1;
    vk_pcache_header h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, k_pcache_magic, sizeof k_pcache_magic);
    h.version = PCACHE_FILE_VERSION;
    h.bytes = n; h.hash_a = vk_pcache_fnv(blob, n); h.hash_b = vk_pcache_mix(blob, n);
    int ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(blob, 1, n, f) == n;
    ok = (fclose(f) == 0) && ok;
#if defined(_WIN32)
    ok = ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    ok = ok && rename(tmp, path) == 0;
#endif
    if (!ok) { remove(tmp); return -1; }
    return 0;
}

/* Copies the driver's cache and writes it. Safe off the render thread:
 * vkGetPipelineCacheData does not require external synchronisation of the
 * cache against concurrent vkCreate*Pipelines calls. */
static int vk_pcache_save_now(VkDevice device)
{
    size_t n = 0;
    if (pvkGetPipelineCacheData(device, s_pcache.cache, &n, NULL) != VK_SUCCESS || n < 32 ||
        n > PCACHE_MAX_BYTES)
        return -1;
    unsigned char* blob = (unsigned char*)malloc(n);
    if (!blob) return -1;
    VkResult r = pvkGetPipelineCacheData(device, s_pcache.cache, &n, blob);
    /* VK_INCOMPLETE: the cache grew between the two calls; the prefix the
     * driver returned is still a valid cache, but the next save gets the rest. */
    int rc = -1;
    if ((r == VK_SUCCESS || r == VK_INCOMPLETE) && vk_pcache_blob_matches(blob, n, &s_pcache.props))
        rc = vk_pcache_write(s_pcache.path, blob, n);
    if (r == VK_INCOMPLETE) SDL_AtomicAdd(&s_pcache.dirty, 1);
    free(blob);
    if (rc == 0) s_pcache.saves++; else s_pcache.save_failures++;
    return rc;
}

typedef struct { VkDevice device; } vk_pcache_job;

static int SDLCALL vk_pcache_save_thread(void* ud)
{
    vk_pcache_job* job = (vk_pcache_job*)ud;
    const Uint32 t0 = SDL_GetTicks();
    const int rc = vk_pcache_save_now(job->device);
    VK_LOG("pipeline cache: background save %s in %u ms (pipelines created this session %d)\n",
           rc == 0 ? "ok" : "failed", SDL_GetTicks() - t0, SDL_AtomicGet(&s_pcache.created));
    free(job);
    SDL_AtomicSet(&s_pcache.saving, 0);
    return rc;
}

/* Called once per presented frame: starts a background save when new
 * pipelines appeared and the last save is old enough. Costs one atomic read
 * per frame otherwise. */
static void vk_pcache_tick(VkDevice device)
{
    if (!s_pcache.cache || SDL_AtomicGet(&s_pcache.dirty) == 0) return;
    const Uint32 now = SDL_GetTicks();
    if (s_pcache.last_save_ms && now - s_pcache.last_save_ms < PCACHE_SAVE_INTERVAL_MS) return;
    if (!SDL_AtomicCAS(&s_pcache.saving, 0, 1)) return;
    vk_pcache_job* job = (vk_pcache_job*)malloc(sizeof *job);
    if (!job) { SDL_AtomicSet(&s_pcache.saving, 0); return; }
    job->device = device;
    SDL_AtomicSet(&s_pcache.dirty, 0);
    s_pcache.last_save_ms = now ? now : 1;
    SDL_Thread* t = SDL_CreateThread(vk_pcache_save_thread, "vk-pcache-save", job);
    if (t) SDL_DetachThread(t);
    else { free(job); SDL_AtomicSet(&s_pcache.dirty, 1); SDL_AtomicSet(&s_pcache.saving, 0); }
}

/* After vkCreateDevice and device function loading. */
static void vk_pcache_init(VkInstance instance, VkPhysicalDevice phys, VkDevice device)
{
    memset(&s_pcache, 0, sizeof s_pcache);
    const char* dir = getenv("PS3RECOMP_SHADER_CACHE_DIR");
    if (!dir || !*dir) return;
    pvkCreatePipelineCache = (PFN_vkCreatePipelineCache)pvkGetDeviceProcAddr(device, "vkCreatePipelineCache");
    pvkDestroyPipelineCache = (PFN_vkDestroyPipelineCache)pvkGetDeviceProcAddr(device, "vkDestroyPipelineCache");
    pvkGetPipelineCacheData = (PFN_vkGetPipelineCacheData)pvkGetDeviceProcAddr(device, "vkGetPipelineCacheData");
    (void)instance;
    if (!pvkCreatePipelineCache || !pvkDestroyPipelineCache || !pvkGetPipelineCacheData) {
        VK_LOG("pipeline cache: entry points missing; off\n");
        return;
    }
    if (snprintf(s_pcache.path, sizeof s_pcache.path, "%s/vk-pipeline-cache.bin", dir) >= (int)sizeof s_pcache.path) {
        VK_LOG("pipeline cache: directory path too long; off\n");
        return;
    }
    pvkGetPhysicalDeviceProperties(phys, &s_pcache.props);

    size_t n = 0;
    const char* why = "";
    unsigned char* blob = vk_pcache_read(s_pcache.path, &n, &why);
    if (blob && !vk_pcache_blob_matches(blob, n, &s_pcache.props)) {
        why = "different GPU, driver or cache format";
        free(blob); blob = NULL; n = 0;
    }
    VkPipelineCacheCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                                     .initialDataSize = blob ? n : 0, .pInitialData = blob };
    VkResult r = pvkCreatePipelineCache(device, &ci, NULL, &s_pcache.cache);
    if (r != VK_SUCCESS && blob) {
        /* The driver refused our data: start empty rather than run without. */
        why = "driver rejected the data";
        free(blob); blob = NULL; n = 0;
        ci.initialDataSize = 0; ci.pInitialData = NULL;
        r = pvkCreatePipelineCache(device, &ci, NULL, &s_pcache.cache);
    }
    if (r != VK_SUCCESS) {
        s_pcache.cache = VK_NULL_HANDLE;
        VK_LOG("pipeline cache: vkCreatePipelineCache failed (%d); off\n", (int)r);
        free(blob);
        return;
    }
    if (blob) SDL_AtomicSet(&s_pcache.loaded_ok, 1);
    VK_LOG("pipeline cache: %s (%zu bytes) from %s\n",
           blob ? "loaded" : "starting empty", n, blob ? s_pcache.path : why);
    free(blob);
}

/* Before vkDestroyDevice: waits for a running background save, writes once
 * more if anything is new, and destroys the cache. */
static void vk_pcache_shutdown(VkDevice device)
{
    if (!s_pcache.cache) return;
    for (int i = 0; i < 500 && SDL_AtomicGet(&s_pcache.saving); i++) SDL_Delay(10);
    if (!SDL_AtomicGet(&s_pcache.saving)) {
        if (SDL_AtomicGet(&s_pcache.dirty) && vk_pcache_save_now(device) == 0)
            SDL_AtomicSet(&s_pcache.dirty, 0);
        VK_LOG("pipeline cache: shutdown, created=%d saves=%u failures=%u\n",
               SDL_AtomicGet(&s_pcache.created), s_pcache.saves, s_pcache.save_failures);
        pvkDestroyPipelineCache(device, s_pcache.cache, NULL);
    }
    /* A save that never finished keeps the cache object alive; the device is
     * being destroyed anyway and the file on disk is the previous good copy. */
    s_pcache.cache = VK_NULL_HANDLE;
}
