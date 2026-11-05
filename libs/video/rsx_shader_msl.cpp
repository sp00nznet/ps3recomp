/*
 * ps3recomp - HLSL -> SPIR-V -> MSL, through glslang and spirv-cross
 *
 * See rsx_shader_msl.h for why this exists and what it promises about
 * bindings. This is a straight walk through the two libraries; the choices
 * worth knowing about are:
 *
 *   - glslang is driven through its C++ API, not its C one. HLSL has no fixed
 *     entry point name, so glslang has to be told which function is the entry
 *     point or the link step reports "Entry point not found" -- and the C API
 *     only grew a setter for that in glslang 16, while Ubuntu 24.04 packages
 *     15.1. TShader::setEntryPoint has always been there.
 *   - Vulkan rules, SPIR-V 1.3. glslang's HLSL front end only targets Vulkan
 *     semantics, and 1.3 is the floor spirv-cross accepts for everything the
 *     decompilers emit.
 *   - Auto-mapped bindings and locations, then TProgram::mapIO. The HLSL
 *     carries register() numbers for every resource, which glslang keeps as
 *     SPIR-V bindings; mapIO is what numbers the varyings and the vertex
 *     attributes in declaration order.
 *   - The SPIR-V is legalized (glslang's HLSL front end asks for it; the
 *     optimizer is on for exactly that pass) before spirv-cross sees it.
 *   - spirv-cross honours the SPIR-V bindings as the MSL slot numbers
 *     (MSL_ENABLE_DECORATION_BINDING). Without it spirv-cross would allocate
 *     buffer/texture/sampler indices itself, in an order the backend could
 *     not predict. Only options that exist in the spirv-cross Debian and
 *     Ubuntu package are used.
 */
#include "rsx_shader_msl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(PS3RECOMP_HAVE_MSL_TRANSLATION)

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <spirv_cross_c.h>
#include <functional>
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

/* HLSL -> SPIR-V. Returns false with the reason in `log`. */
bool hlsl_to_spirv(const char* hlsl, EShLanguage lang, std::vector<unsigned int>& spirv,
                   char* log, u32 log_size)
{
    const EShMessages msgs = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl);

    glslang::TShader shader(lang);
    const char* strings[1] = { hlsl };
    shader.setStrings(strings, 1);
    shader.setEnvInput(glslang::EShSourceHlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    /* SPIR-V FragCoord.w is 1/w (FXC's SV_POSITION.w is w): tells the FP
     * decompiler's _rsx_inv_w helper which one it gets. */
    shader.setPreamble("#define RSX_FRAGCOORD_W_IS_RCP 1\n");
    shader.setEntryPoint("main");
    shader.setAutoMapBindings(true);
    shader.setAutoMapLocations(true);
    if (!shader.parse(GetDefaultResources(), 100, false, msgs)) {
        put_log(log, log_size, "HLSL parse", shader.getInfoLog());
        return false;
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(msgs)) {
        put_log(log, log_size, "HLSL link", program.getInfoLog());
        return false;
    }
    if (!program.mapIO()) {
        put_log(log, log_size, "HLSL io map", program.getInfoLog());
        return false;
    }

    spv::SpvBuildLogger logger;
    glslang::SpvOptions opts;
    opts.disableOptimizer = false;     /* run the HLSL legalization passes */
    glslang::GlslangToSpv(*program.getIntermediate(lang), spirv, &logger, &opts);
    if (spirv.empty()) {
        put_log(log, log_size, "SPIR-V generation", logger.getAllMessages().c_str());
        return false;
    }
    return true;
}

/* PS3RECOMP_MSL_FAST=1: keep spirv-cross's fast:: math (see spirv_to_msl). */
bool msl_keep_fast()
{
    static int keep_fast = -1;
    if (keep_fast < 0) keep_fast = getenv("PS3RECOMP_MSL_FAST") ? 1 : 0;
    return keep_fast != 0;
}

/* PS3RECOMP_MSL_INVARIANT=0: a vertex program's position is not marked
 * [[invariant]] (see spirv_to_msl). */
bool msl_invariant()
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("PS3RECOMP_MSL_INVARIANT");
        on = !(e && *e && atoi(e) == 0);
    }
    return on != 0;
}

/* SPIR-V -> MSL into `out`. Returns false with the reason in `log`. */
bool spirv_to_msl(const std::vector<unsigned int>& spirv, bool vertex, char* out, u32 out_size,
                  char* log, u32 log_size)
{
    spvc_context ctx = nullptr;
    if (spvc_context_create(&ctx) != SPVC_SUCCESS || !ctx) {
        put_log(log, log_size, "spvc_context_create failed", nullptr);
        return false;
    }
    bool ok = false;
    spvc_parsed_ir ir = nullptr;
    spvc_compiler comp = nullptr;
    spvc_compiler_options opt = nullptr;
    const char* msl = nullptr;

    if (spvc_context_parse_spirv(ctx, (const SpvId*)spirv.data(), spirv.size(), &ir) != SPVC_SUCCESS) {
        put_log(log, log_size, "SPIR-V parse", spvc_context_get_last_error_string(ctx));
        goto done;
    }
    if (spvc_context_create_compiler(ctx, SPVC_BACKEND_MSL, ir,
                                     SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &comp) != SPVC_SUCCESS) {
        put_log(log, log_size, "MSL compiler", spvc_context_get_last_error_string(ctx));
        goto done;
    }
    if (spvc_compiler_create_compiler_options(comp, &opt) != SPVC_SUCCESS) {
        put_log(log, log_size, "MSL options", spvc_context_get_last_error_string(ctx));
        goto done;
    }
    /* MSL 2.1: arrays of textures need 2.0, and every Apple Silicon Mac
     * speaks 2.3 or later, so nothing in range is excluded. */
    spvc_compiler_options_set_uint(opt, SPVC_COMPILER_OPTION_MSL_VERSION, 20100u);
    spvc_compiler_options_set_uint(opt, SPVC_COMPILER_OPTION_MSL_PLATFORM, SPVC_MSL_PLATFORM_MACOS);
    spvc_compiler_options_set_bool(opt, SPVC_COMPILER_OPTION_MSL_ENABLE_DECORATION_BINDING, SPVC_TRUE);
    if (spvc_compiler_install_compiler_options(comp, opt) != SPVC_SUCCESS) {
        put_log(log, log_size, "MSL options install", spvc_context_get_last_error_string(ctx));
        goto done;
    }
    if (spvc_compiler_compile(comp, &msl) != SPVC_SUCCESS || !msl) {
        put_log(log, log_size, "MSL emit", spvc_context_get_last_error_string(ctx));
        goto done;
    }
    {
        /* spirv-cross spells GLSL.std.450 FMax/FMin/FClamp/Normalize as
         * fast::max/min/clamp/normalize, whose NaN behaviour Metal leaves
         * undefined even under MTLMathModeSafe. D3D's max/min return the
         * number when one operand is NaN and saturate(NaN) is 0, and the
         * guest programs were written against hardware that never produced
         * NaN from those. inFAMOUS 2's sky/bloom pass did, only on Metal.
         * The precise:: overloads follow IEEE fmax/fmin. PS3RECOMP_MSL_FAST=1
         * keeps the fast:: spelling. */
        std::string text(msl);
        if (!msl_keep_fast())
            for (size_t at = 0; (at = text.find("fast::", at)) != std::string::npos; at += 9)
                text.replace(at, 6, "precise::");
        /* An invariant position. Titles draw the same geometry twice with
         * two different vertex programs, the second pass testing EQUAL
         * against the first one's depth: Resistance: Fall of Man's foliage
         * writes depth through an alpha-tested program, then colour through
         * one that also lights, with SET_DEPTH_FUNC EQUAL. The RSX executes
         * the position arithmetic instruction by instruction, so both
         * programs land on the same depth. The Metal compiler optimises each
         * program on its own (fusing a multiply and an add into one FMA in
         * one and not the other), the depths can differ in the last bit, and
         * the EQUAL pass then leaves holes. [[invariant]] (with
         * MTLCompileOptions' preserveInvariance, which the backend sets)
         * makes the compiler compute the position the same way in every
         * program that computes it the same way. PS3RECOMP_MSL_INVARIANT=0
         * turns it off (msl_invariant). */
        if (vertex && msl_invariant()) {
            const size_t at = text.find("[[position]]");
            if (at != std::string::npos) text.replace(at, 12, "[[position, invariant]]");
        }
        if (text.size() + 1 > out_size) {
            put_log(log, log_size, "MSL output buffer too small", nullptr);
            goto done;
        }
        memcpy(out, text.c_str(), text.size() + 1);
    }
    ok = true;
done:
    /* The MSL string belongs to the context; it was copied out above. */
    spvc_context_destroy(ctx);
    return ok;
}

/* ---- translation cache -------------------------------------------------
 * The translation is most of what a guest pipeline's first use costs on
 * Metal: in inFAMOUS 2 it was 83% of the 2.1 s that 218 first uses took
 * over two minutes of play, with single frames of up to 366 ms, against ~16%
 * for newLibraryWithSource and under 1% for the pipeline state (Metal keeps
 * its own cache of those). So the MSL text is kept, in memory and, with
 * PS3RECOMP_MSL_CACHE=<dir>, on disk across runs, as rsx_shader_spirv.cpp
 * keeps SPIR-V. A file stands for one stage + HLSL text + PS3RECOMP_MSL_FAST
 * setting, and its header names the glslang and spirv-cross versions and
 * this file's build: a file written by any other combination is a miss. */
const u32 kCacheFormat = 1;
const u32 kCacheMagic = 0x4C534D52u;   /* "RMSL" */
const char kTranslatorBuild[24] = __DATE__ " " __TIME__;

struct Cache {
    std::mutex lock;
    std::unordered_map<std::string, std::string> map;
    std::string dir;
    bool dir_read = false;
    RsxMslCacheStats stats = {};
};

Cache& cache()
{
    static Cache c;
    return c;
}

const std::string& cache_dir(Cache& c)
{
    if (!c.dir_read) {
        const char* e = getenv("PS3RECOMP_MSL_CACHE");
        c.dir = (e && *e) ? e : "";
        c.dir_read = true;
    }
    return c.dir;
}

/* The stage, then every switch that changes the generated MSL, then the HLSL
 * text. A switch missing here serves one setting's output under another, and
 * PS3RECOMP_MSL_CACHE outlives the process, so it would survive a restart. */
std::string cache_key(const char* hlsl, int stage)
{
    std::string key(1, (char)('0' + stage));
    key += msl_keep_fast() ? 'f' : 'p';
    key += msl_invariant() ? 'i' : 'n';
    key += hlsl;
    return key;
}

std::string cache_path(const std::string& dir, const std::string& key)
{
    unsigned long long h = 0xcbf29ce484222325ull;       /* FNV-1a 64 */
    for (unsigned char ch : key) { h ^= ch; h *= 0x100000001b3ull; }
    char name[40];
    snprintf(name, sizeof name, "/%016llx.msl", h);
    return dir + name;
}

/* Header: magic, format, glslang major/minor/patch, spirv-cross C API
 * major/minor/patch, translator build (six words), key bytes, MSL bytes;
 * then the key and the MSL. */
const int kHeaderWords = 16;

void cache_header(u32 hdr[kHeaderWords], const std::string& key, size_t msl_bytes)
{
    const glslang::Version v = glslang::GetVersion();
    unsigned sx_major = 0, sx_minor = 0, sx_patch = 0;
    spvc_get_version(&sx_major, &sx_minor, &sx_patch);
    hdr[0] = kCacheMagic; hdr[1] = kCacheFormat;
    hdr[2] = (u32)v.major; hdr[3] = (u32)v.minor; hdr[4] = (u32)v.patch;
    hdr[5] = sx_major; hdr[6] = sx_minor; hdr[7] = sx_patch;
    memcpy(&hdr[8], kTranslatorBuild, sizeof kTranslatorBuild);
    hdr[14] = (u32)key.size(); hdr[15] = (u32)msl_bytes;
}

bool cache_load(const std::string& path, const std::string& key, std::string& msl)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    u32 want[kHeaderWords], hdr[kHeaderWords];
    cache_header(want, key, 0);
    bool ok = fread(hdr, sizeof hdr, 1, f) == 1 &&
              memcmp(hdr, want, (kHeaderWords - 1) * sizeof(u32)) == 0 &&
              hdr[kHeaderWords - 1] != 0 && hdr[kHeaderWords - 1] <= (16u << 20);
    if (ok) {
        std::string stored(key.size(), '\0');
        ok = fread(&stored[0], 1, stored.size(), f) == stored.size() && stored == key;
    }
    if (ok) {
        msl.assign(hdr[kHeaderWords - 1], '\0');
        ok = fread(&msl[0], 1, msl.size(), f) == msl.size();
    }
    fclose(f);
    if (!ok) msl.clear();
    return ok;
}

/* Written under a temporary name and renamed, so a reader never sees half a file. */
void cache_store(const std::string& path, const std::string& key, const std::string& msl)
{
    char suffix[48];
    snprintf(suffix, sizeof suffix, ".%zx.tmp", std::hash<std::thread::id>{}(std::this_thread::get_id()));
    const std::string tmp = path + suffix;
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    u32 hdr[kHeaderWords];
    cache_header(hdr, key, msl.size());
    bool ok = fwrite(hdr, sizeof hdr, 1, f) == 1 &&
              fwrite(key.data(), 1, key.size(), f) == key.size() &&
              fwrite(msl.data(), 1, msl.size(), f) == msl.size();
    ok = (fclose(f) == 0) && ok;
#ifdef _WIN32
    if (ok) remove(path.c_str());      /* rename does not replace on Windows */
#endif
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) remove(tmp.c_str());
}

/* HLSL -> MSL text, uncached. */
bool translate(const char* hlsl, int stage, std::string& msl, char* log, u32 log_size)
{
    if (!glslang_ready()) {
        put_log(log, log_size, "glslang::InitializeProcess failed", nullptr);
        return false;
    }
    const EShLanguage lang = (stage == RSX_SHADER_STAGE_VERTEX) ? EShLangVertex : EShLangFragment;
    std::vector<unsigned int> spirv;
    if (!hlsl_to_spirv(hlsl, lang, spirv, log, log_size)) return false;
    std::vector<char> out(512u * 1024u);
    if (!spirv_to_msl(spirv, stage == RSX_SHADER_STAGE_VERTEX, out.data(), (u32)out.size(), log, log_size)) return false;
    msl = out.data();
    return true;
}

} // namespace

extern "C" int rsx_hlsl_to_msl_available(void) { return 1; }

extern "C" int rsx_hlsl_to_msl(const char* hlsl, int stage,
                               char* out, u32 out_size,
                               char* log, u32 log_size)
{
    if (log && log_size) log[0] = '\0';
    if (!hlsl || !out || out_size == 0) {
        put_log(log, log_size, "bad arguments", nullptr);
        return -1;
    }
    out[0] = '\0';
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
        std::string msl;
        const std::string& dir = cache_dir(c);
        const std::string path = dir.empty() ? std::string() : cache_path(dir, key);
        if (!dir.empty() && cache_load(path, key, msl)) {
            c.stats.disk_hits++;
        } else {
            if (!translate(hlsl, stage, msl, log, log_size)) return -1;
            c.stats.translations++;
            if (!dir.empty()) cache_store(path, key, msl);
        }
        it = c.map.emplace(key, std::move(msl)).first;
    }
    const std::string& msl = it->second;
    if (msl.size() + 1 > out_size) {
        put_log(log, log_size, "MSL output buffer too small", nullptr);
        return -1;
    }
    memcpy(out, msl.c_str(), msl.size() + 1);
    return 0;
}

extern "C" void rsx_hlsl_to_msl_cache_stats(RsxMslCacheStats* out)
{
    Cache& c = cache();
    std::lock_guard<std::mutex> guard(c.lock);
    if (out) *out = c.stats;
}

extern "C" void rsx_hlsl_to_msl_cache_reset(void)
{
    Cache& c = cache();
    std::lock_guard<std::mutex> guard(c.lock);
    c.map.clear();
    c.dir_read = false;
    c.stats = {};
}

#else /* !PS3RECOMP_HAVE_MSL_TRANSLATION */

extern "C" int rsx_hlsl_to_msl_available(void) { return 0; }

extern "C" void rsx_hlsl_to_msl_cache_stats(RsxMslCacheStats* out)
{
    if (out) memset(out, 0, sizeof *out);
}

extern "C" void rsx_hlsl_to_msl_cache_reset(void) {}

extern "C" int rsx_hlsl_to_msl(const char* hlsl, int stage,
                               char* out, u32 out_size,
                               char* log, u32 log_size)
{
    (void)hlsl; (void)stage;
    if (out && out_size) out[0] = '\0';
    if (log && log_size)
        snprintf(log, log_size, "HLSL to MSL translation was not built "
                                "(glslang and spirv-cross not found at configure time)");
    return -1;
}

#endif
