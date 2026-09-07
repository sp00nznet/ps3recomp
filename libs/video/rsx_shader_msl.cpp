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
#include <string.h>

#if defined(PS3RECOMP_HAVE_MSL_TRANSLATION)

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <spirv_cross_c.h>
#include <string>
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

/* SPIR-V -> MSL into `out`. Returns false with the reason in `log`. */
bool spirv_to_msl(const std::vector<unsigned int>& spirv, char* out, u32 out_size,
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
        const size_t len = strlen(msl);
        if (len + 1 > out_size) {
            put_log(log, log_size, "MSL output buffer too small", nullptr);
            goto done;
        }
        memcpy(out, msl, len + 1);
    }
    ok = true;
done:
    /* The MSL string belongs to the context; it was copied out above. */
    spvc_context_destroy(ctx);
    return ok;
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
    if (!glslang_ready()) {
        put_log(log, log_size, "glslang::InitializeProcess failed", nullptr);
        return -1;
    }
    const EShLanguage lang = (stage == RSX_SHADER_STAGE_VERTEX) ? EShLangVertex : EShLangFragment;
    std::vector<unsigned int> spirv;
    if (!hlsl_to_spirv(hlsl, lang, spirv, log, log_size)) return -1;
    if (!spirv_to_msl(spirv, out, out_size, log, log_size)) return -1;
    return 0;
}

#else /* !PS3RECOMP_HAVE_MSL_TRANSLATION */

extern "C" int rsx_hlsl_to_msl_available(void) { return 0; }

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
