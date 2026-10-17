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
 * Translations are cached. glslang takes 20-150 ms per shader, on the thread
 * that walks the FIFO, and the engine asks for the same HLSL again for every
 * pipeline that shares a program (Twisted Metal: 236 translations in its
 * first minute, 74 distinct). The cache key is the stage plus the whole HLSL
 * text, so a hit returns exactly the words that text translated to. With
 * PS3RECOMP_SPIRV_CACHE=<existing directory> the cache is also kept on disk,
 * one file per shader, and the next run starts warm. A file whose key,
 * glslang version or translator build differs from the request is a miss,
 * and is rewritten.
 */

#include "rsx_shader_spirv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(PS3RECOMP_HAVE_SPIRV_TRANSLATION)

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

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

int translate(const char* hlsl, int stage, std::vector<unsigned int>& spirv,
              char* log, u32 log_size)
{
    if (!glslang_ready()) {
        put_log(log, log_size, "glslang::InitializeProcess failed", nullptr);
        return -1;
    }

    const EShLanguage lang = (stage == RSX_SHADER_STAGE_VERTEX) ? EShLangVertex
                                                                : EShLangFragment;
    const EShMessages msgs =
        (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl);

    glslang::TShader shader(lang);
    const char* strings[1] = { hlsl };
    shader.setStrings(strings, 1);
    shader.setEnvInput(glslang::EShSourceHlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    shader.setEntryPoint("main");
    shader.setAutoMapBindings(true);
    shader.setAutoMapLocations(true);
    shader.setShiftBinding(glslang::EResTexture, RSX_SPIRV_TEXTURE_BINDING);
    shader.setShiftBinding(glslang::EResSampler, RSX_SPIRV_SAMPLER_BINDING);
    if (lang == EShLangVertex) shader.setInvertY(true);

    if (!shader.parse(GetDefaultResources(), 100, false, msgs)) {
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

    spv::SpvBuildLogger logger;
    glslang::SpvOptions opts;
    opts.disableOptimizer = false;     /* run the HLSL legalization passes */
    glslang::GlslangToSpv(*program.getIntermediate(lang), spirv, &logger, &opts);
    if (spirv.empty()) {
        put_log(log, log_size, "SPIR-V generation", logger.getAllMessages().c_str());
        return -1;
    }
    return 0;
}

/* ---- translation cache ------------------------------------------------- */

const u32 kCacheFormat = 1;
const u32 kCacheMagic = 0x56505352u;   /* "RSPV" */
/* This file's compile time stands for translate()'s options and the binding
 * contract in rsx_shader_spirv.h: when either changes, this file is rebuilt,
 * and the files an older build wrote become misses. */
const char kTranslatorBuild[24] = __DATE__ " " __TIME__;

struct Cache {
    std::mutex lock;
    std::unordered_map<std::string, std::vector<unsigned int>> map;
    std::string dir;
    bool dir_read = false;
    RsxSpirvCacheStats stats = {};
};

Cache& cache()
{
    static Cache c;
    return c;
}

const std::string& cache_dir(Cache& c)
{
    if (!c.dir_read) {
        const char* e = getenv("PS3RECOMP_SPIRV_CACHE");
        c.dir = (e && *e) ? e : "";
        c.dir_read = true;
    }
    return c.dir;
}

/* The key is the stage, then the HLSL text. */
std::string cache_key(const char* hlsl, int stage)
{
    std::string key(1, (char)('0' + stage));
    key += hlsl;
    return key;
}

std::string cache_path(const std::string& dir, const std::string& key)
{
    unsigned long long h = 0xcbf29ce484222325ull;       /* FNV-1a 64 */
    for (unsigned char ch : key) { h ^= ch; h *= 0x100000001b3ull; }
    char name[40];
    snprintf(name, sizeof name, "/%016llx.spv", h);
    return dir + name;
}

/* Header: magic, format, glslang major/minor/patch, translator build (six
 * words), key bytes, word count; then the key and the words. */
const int kHeaderWords = 13;

void cache_header(u32 hdr[kHeaderWords], const std::string& key, size_t nwords)
{
    const glslang::Version v = glslang::GetVersion();
    hdr[0] = kCacheMagic; hdr[1] = kCacheFormat;
    hdr[2] = (u32)v.major; hdr[3] = (u32)v.minor; hdr[4] = (u32)v.patch;
    memcpy(&hdr[5], kTranslatorBuild, sizeof kTranslatorBuild);
    hdr[11] = (u32)key.size(); hdr[12] = (u32)nwords;
}

bool cache_load(const std::string& path, const std::string& key,
                std::vector<unsigned int>& spirv)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    u32 want[kHeaderWords], hdr[kHeaderWords];
    cache_header(want, key, 0);
    bool ok = fread(hdr, sizeof hdr, 1, f) == 1 &&
              memcmp(hdr, want, (kHeaderWords - 1) * sizeof(u32)) == 0 &&
              hdr[kHeaderWords - 1] != 0 && hdr[kHeaderWords - 1] <= (1u << 24);
    if (ok) {
        std::string stored(key.size(), '\0');
        ok = fread(&stored[0], 1, stored.size(), f) == stored.size() && stored == key;
    }
    if (ok) {
        spirv.resize(hdr[kHeaderWords - 1]);
        ok = fread(spirv.data(), sizeof(u32), spirv.size(), f) == spirv.size();
    }
    fclose(f);
    if (!ok) spirv.clear();
    return ok;
}

/* Written under a temporary name and renamed, so a reader never sees half a file. */
void cache_store(const std::string& path, const std::string& key,
                 const std::vector<unsigned int>& spirv)
{
    char suffix[48];
    snprintf(suffix, sizeof suffix, ".%zx.tmp", std::hash<std::thread::id>{}(std::this_thread::get_id()));
    const std::string tmp = path + suffix;
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    u32 hdr[kHeaderWords];
    cache_header(hdr, key, spirv.size());
    bool ok = fwrite(hdr, sizeof hdr, 1, f) == 1 &&
              fwrite(key.data(), 1, key.size(), f) == key.size() &&
              fwrite(spirv.data(), sizeof(u32), spirv.size(), f) == spirv.size();
    ok = (fclose(f) == 0) && ok;
#ifdef _WIN32
    if (ok) remove(path.c_str());      /* rename does not replace on Windows */
#endif
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) remove(tmp.c_str());
}

} // namespace

extern "C" int rsx_hlsl_to_spirv_available(void) { return 1; }

extern "C" int rsx_hlsl_to_spirv(const char* hlsl, int stage,
                                 u32* words, u32 max_words, u32* out_words,
                                 char* log, u32 log_size)
{
    if (out_words) *out_words = 0;
    if (!hlsl || !words || !out_words || max_words == 0) {
        put_log(log, log_size, "bad arguments", nullptr);
        return -1;
    }
    if (stage != RSX_SHADER_STAGE_VERTEX && stage != RSX_SHADER_STAGE_FRAGMENT) {
        put_log(log, log_size, "unknown shader stage", nullptr);
        return -1;
    }

    /* One lock around the whole lookup and translation: glslang's process
     * state is not documented as thread-safe either. */
    Cache& c = cache();
    std::lock_guard<std::mutex> guard(c.lock);
    const std::string key = cache_key(hlsl, stage);
    auto it = c.map.find(key);
    if (it != c.map.end()) {
        c.stats.memory_hits++;
    } else {
        std::vector<unsigned int> spirv;
        const std::string& dir = cache_dir(c);
        const std::string path = dir.empty() ? std::string() : cache_path(dir, key);
        if (!dir.empty() && cache_load(path, key, spirv)) {
            c.stats.disk_hits++;
        } else {
            if (translate(hlsl, stage, spirv, log, log_size)) return -1;
            c.stats.translations++;
            if (!dir.empty()) cache_store(path, key, spirv);
        }
        it = c.map.emplace(key, std::move(spirv)).first;
    }
    const std::vector<unsigned int>& spirv = it->second;
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

extern "C" void rsx_hlsl_to_spirv_cache_stats(RsxSpirvCacheStats* out)
{
    Cache& c = cache();
    std::lock_guard<std::mutex> guard(c.lock);
    if (out) *out = c.stats;
}

extern "C" void rsx_hlsl_to_spirv_cache_reset(void)
{
    Cache& c = cache();
    std::lock_guard<std::mutex> guard(c.lock);
    c.map.clear();
    c.dir_read = false;
    c.stats = {};
}

#else /* !PS3RECOMP_HAVE_SPIRV_TRANSLATION */

extern "C" int rsx_hlsl_to_spirv_available(void) { return 0; }

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

extern "C" void rsx_hlsl_to_spirv_cache_stats(RsxSpirvCacheStats* out)
{
    if (out) memset(out, 0, sizeof *out);
}

extern "C" void rsx_hlsl_to_spirv_cache_reset(void) {}

#endif
