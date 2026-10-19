/*
 * ps3recomp - HLSL -> SPIR-V through glslang, for the Vulkan backend
 *
 * See rsx_shader_spirv.h for the binding contract. The front end is driven
 * exactly as rsx_shader_msl.cpp drives it (C++ API for setEntryPoint, Vulkan
 * rules, auto-mapped bindings and locations, then mapIO, legalized SPIR-V),
 * with three differences, all because the output goes to a Vulkan driver
 * rather than to spirv-cross:
 *
 *   - Vulkan 1.0 client, SPIR-V 1.0: the backend creates a 1.0 instance and
 *     promises to run on 1.0-level drivers. (The MSL path targets 1.3 because
 *     that is spirv-cross's floor, not a driver's.)
 *   - Texture and sampler registers are shifted apart (setShiftBinding),
 *     because Vulkan has one binding namespace per set.
 *   - The vertex stage flips Y (setInvertY): D3D clip space is +y up.
 *
 * Disk cache: with PS3RECOMP_SHADER_CACHE_DIR=<dir>, each successful
 * translation is stored as <dir>/<key>.spv and later translations of the same
 * input are read back instead of running glslang again (glslang is most of the
 * cost when a scene first draws with a new program). The key covers the HLSL
 * bytes, the stage, the glslang version and RSX_SPIRV_CACHE_VERSION, which
 * must be raised whenever this file changes how SPIR-V is produced. A file
 * whose header, input length, second hash or SPIR-V magic does not match is
 * ignored and rewritten, failures are never cached, and any cache error
 * falls back to translating. Unset, nothing is read or written.
 */

#include "rsx_shader_spirv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(PS3RECOMP_HAVE_SPIRV_TRANSLATION)

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <glslang/build_info.h>

#include <chrono>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#define RSX_SPIRV_CACHE_VERSION 1u

namespace {

void put_log(char* log, u32 log_size, const char* what, const char* detail)
{
    if (!log || log_size == 0) return;
    snprintf(log, log_size, "%s%s%s", what ? what : "",
             (what && detail && *detail) ? ": " : "", detail ? detail : "");
}

bool glslang_ready()
{
    static bool inited = false;
    if (!inited) {
        if (!glslang::InitializeProcess()) return false;
        inited = true;
    }
    return true;
}

} // namespace

extern "C" int rsx_hlsl_to_spirv_available(void) { return 1; }

static int compile_spirv(const char* hlsl, int stage,
                         u32* words, u32 max_words, u32* out_words,
                         char* log, u32 log_size, bool glsl)
{
    if (out_words) *out_words = 0;
    if (!hlsl || !words || !out_words || max_words == 0) {
        put_log(log, log_size, "bad arguments", nullptr);
        return -1;
    }
    if (stage != RSX_SHADER_STAGE_VERTEX && stage != RSX_SHADER_STAGE_FRAGMENT && stage != RSX_SHADER_STAGE_COMPUTE) {
        put_log(log, log_size, "unknown shader stage", nullptr);
        return -1;
    }
    if (!glslang_ready()) {
        put_log(log, log_size, "glslang::InitializeProcess failed", nullptr);
        return -1;
    }

    const EShLanguage lang = stage == RSX_SHADER_STAGE_VERTEX ? EShLangVertex :
                            stage == RSX_SHADER_STAGE_FRAGMENT ? EShLangFragment : EShLangCompute;
    const EShMessages msgs =
        (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules | (glsl ? 0 : EShMsgReadHlsl));

    glslang::TShader shader(lang);
    const char* strings[1] = { hlsl };
    shader.setStrings(strings, 1);
    shader.setEnvInput(glsl ? glslang::EShSourceGlsl : glslang::EShSourceHlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glsl ? glslang::EShTargetVulkan_1_2 : glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glsl ? glslang::EShTargetSpv_1_5 : glslang::EShTargetSpv_1_0);
    shader.setEntryPoint("main");
    if (!glsl) {
        shader.setAutoMapBindings(true);
        shader.setAutoMapLocations(true);
        shader.setShiftBinding(glslang::EResTexture, RSX_SPIRV_TEXTURE_BINDING);
        shader.setShiftBinding(glslang::EResSampler, RSX_SPIRV_SAMPLER_BINDING);
        if (lang == EShLangVertex) shader.setInvertY(true);
    }

    if (!shader.parse(GetDefaultResources(), glsl ? 460 : 100, false, msgs)) {
        put_log(log, log_size, "HLSL parse", shader.getInfoLog());
        return -1;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(msgs)) {
        put_log(log, log_size, "HLSL link", program.getInfoLog());
        return -1;
    }
    if (!program.mapIO()) {
        put_log(log, log_size, "HLSL io map", program.getInfoLog());
        return -1;
    }

    std::vector<unsigned int> spirv;
    spv::SpvBuildLogger logger;
    glslang::SpvOptions opts;
    opts.disableOptimizer = false;     /* run the HLSL legalization passes */
    glslang::GlslangToSpv(*program.getIntermediate(lang), spirv, &logger, &opts);
    if (spirv.empty()) {
        put_log(log, log_size, "SPIR-V generation", logger.getAllMessages().c_str());
        return -1;
    }
    if (spirv.size() > max_words) {
        char n[64];
        snprintf(n, sizeof n, "%zu words, room for %u", spirv.size(), max_words);
        put_log(log, log_size, "SPIR-V output too large", n);
        return -1;
    }
    memcpy(words, spirv.data(), spirv.size() * sizeof(u32));
    *out_words = (u32)spirv.size();
    return 0;
}

static int rsx_translate_uncached(const char* hlsl, int stage,
                                  u32* words, u32 max_words, u32* out_words,
                                  char* log, u32 log_size)
{
    return compile_spirv(hlsl, stage, words, max_words, out_words, log, log_size, false);
}

namespace {

/* File layout: header, then `words` SPIR-V words. All fields little-endian
 * host order; the cache is per machine. */
struct CacheHeader {
    char magic[8];          /* "RSXSPV\0\1" */
    u32 cache_version;      /* RSX_SPIRV_CACHE_VERSION */
    u32 glslang_version;    /* major << 16 | minor << 8 | patch */
    u32 stage;
    u32 words;
    unsigned long long hlsl_len;
    unsigned long long key;     /* names the file */
    unsigned long long check;   /* independent hash, guards against collisions */
    unsigned long long payload; /* hash of the SPIR-V words, catches a damaged file */
};
const char kMagic[8] = { 'R', 'S', 'X', 'S', 'P', 'V', 0, 1 };
const u32 kGlslangVersion = (GLSLANG_VERSION_MAJOR << 16) | (GLSLANG_VERSION_MINOR << 8) | GLSLANG_VERSION_PATCH;

unsigned long long fnv1a(const void* p, size_t n, unsigned long long h)
{
    const unsigned char* b = (const unsigned char*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

/* A second hash with different mixing, so a file is only trusted when both
 * agree (FNV-1a alone is not collision resistant enough to bet a shader on). */
unsigned long long mix_hash(const void* p, size_t n, unsigned long long h)
{
    const unsigned char* b = (const unsigned char*)p;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ b[i]) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    return h;
}

void make_keys(const char* hlsl, size_t len, int stage, unsigned long long* key, unsigned long long* check)
{
    const u32 salt[3] = { RSX_SPIRV_CACHE_VERSION, kGlslangVersion, (u32)stage };
    *key = fnv1a(hlsl, len, fnv1a(salt, sizeof salt, 0xCBF29CE484222325ull));
    *check = mix_hash(hlsl, len, mix_hash(salt, sizeof salt, 0x243F6A8885A308D3ull));
}

struct CacheStats { unsigned hits, misses, stores, rejects; double translate_ms; };
CacheStats g_stats;

const char* cache_dir()
{
    static const char* dir = nullptr;
    static bool read = false;
    if (!read) {
        const char* e = getenv("PS3RECOMP_SHADER_CACHE_DIR");
        dir = (e && *e) ? e : nullptr;
        read = true;
    }
    return dir;
}

void cache_path(char* out, size_t cap, unsigned long long key, int stage)
{
    snprintf(out, cap, "%s/%016llx-%c.spv", cache_dir(), key, stage == RSX_SHADER_STAGE_VERTEX ? 'v' : 'f');
}

bool cache_read(const char* path, size_t len, int stage, unsigned long long key, unsigned long long check,
                u32* words, u32 max_words, u32* out_words)
{
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    CacheHeader h;
    bool ok = fread(&h, sizeof h, 1, f) == 1 && memcmp(h.magic, kMagic, sizeof kMagic) == 0 &&
              h.cache_version == RSX_SPIRV_CACHE_VERSION && h.glslang_version == kGlslangVersion &&
              h.stage == (u32)stage && h.hlsl_len == len && h.key == key && h.check == check &&
              h.words > 5 && h.words <= max_words &&
              fread(words, sizeof(u32), h.words, f) == h.words && words[0] == 0x07230203u &&
              fgetc(f) == EOF && fnv1a(words, (size_t)h.words * 4u, 0xCBF29CE484222325ull) == h.payload;
    fclose(f);
    if (!ok) { g_stats.rejects++; return false; }
    *out_words = h.words;
    return true;
}

void cache_write(const char* path, size_t len, int stage, unsigned long long key, unsigned long long check,
                 const u32* words, u32 n)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.%lu.tmp", path,
#if defined(_WIN32)
             (unsigned long)GetCurrentProcessId()
#else
             (unsigned long)getpid()
#endif
    );
    FILE* f = fopen(tmp, "wb");
    if (!f) return;
    CacheHeader h;
    memcpy(h.magic, kMagic, sizeof kMagic);
    h.cache_version = RSX_SPIRV_CACHE_VERSION; h.glslang_version = kGlslangVersion;
    h.stage = (u32)stage; h.words = n; h.hlsl_len = len; h.key = key; h.check = check;
    h.payload = fnv1a(words, (size_t)n * 4u, 0xCBF29CE484222325ull);
    bool ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(words, sizeof(u32), n, f) == n;
    ok = (fclose(f) == 0) && ok;
#if defined(_WIN32)
    ok = ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    ok = ok && rename(tmp, path) == 0;
#endif
    if (!ok) { remove(tmp); return; }
    g_stats.stores++;
}

void report()
{
    const unsigned events = g_stats.hits + g_stats.misses;
    if (events == 1 || events % 64 == 0)
        fprintf(stderr, "[spirv-cache] hits=%u misses=%u stores=%u rejects=%u translate_ms=%.1f\n",
                g_stats.hits, g_stats.misses, g_stats.stores, g_stats.rejects, g_stats.translate_ms);
}

} // namespace

extern "C" void rsx_spirv_cache_stats(u32* hits, u32* misses, u32* stores, u32* rejects)
{
    if (hits) *hits = g_stats.hits;
    if (misses) *misses = g_stats.misses;
    if (stores) *stores = g_stats.stores;
    if (rejects) *rejects = g_stats.rejects;
}

extern "C" int rsx_hlsl_to_spirv(const char* hlsl, int stage,
                                 u32* words, u32 max_words, u32* out_words,
                                 char* log, u32 log_size)
{
    if (!cache_dir() || !hlsl || !words || !out_words || max_words == 0 ||
        (stage != RSX_SHADER_STAGE_VERTEX && stage != RSX_SHADER_STAGE_FRAGMENT))
        return rsx_translate_uncached(hlsl, stage, words, max_words, out_words, log, log_size);

    const size_t len = strlen(hlsl);
    unsigned long long key, check;
    make_keys(hlsl, len, stage, &key, &check);
    char path[1024];
    cache_path(path, sizeof path, key, stage);
    if (cache_read(path, len, stage, key, check, words, max_words, out_words)) {
        g_stats.hits++;
        report();
        return 0;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const int r = rsx_translate_uncached(hlsl, stage, words, max_words, out_words, log, log_size);
    g_stats.translate_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    g_stats.misses++;
    if (r == 0) cache_write(path, len, stage, key, check, words, *out_words);
    report();
    return r;
}
extern "C" int rsx_glsl_compute_to_spirv(const char* source,u32* words,u32 capacity,
    u32* written,char* log,u32 log_size)
{ return compile_spirv(source,RSX_SHADER_STAGE_COMPUTE,words,capacity,written,log,log_size,true); }

#else /* !PS3RECOMP_HAVE_SPIRV_TRANSLATION */

extern "C" int rsx_hlsl_to_spirv_available(void) { return 0; }

extern "C" void rsx_spirv_cache_stats(u32* hits, u32* misses, u32* stores, u32* rejects)
{
    if (hits) *hits = 0;
    if (misses) *misses = 0;
    if (stores) *stores = 0;
    if (rejects) *rejects = 0;
}

extern "C" int rsx_hlsl_to_spirv(const char* hlsl, int stage,
                                 u32* words, u32 max_words, u32* out_words,
                                 char* log, u32 log_size)
{
    (void)hlsl; (void)stage; (void)words; (void)max_words;
    if (out_words) *out_words = 0;
    if (log && log_size)
        snprintf(log, log_size, "HLSL -> SPIR-V translation not built (glslang not found)");
    return -1;
}

extern "C" int rsx_glsl_compute_to_spirv(const char* source,u32* words,u32 capacity,
    u32* written,char* log,u32 log_size)
{ return rsx_hlsl_to_spirv(source,RSX_SHADER_STAGE_COMPUTE,words,capacity,written,log,log_size); }

#endif
