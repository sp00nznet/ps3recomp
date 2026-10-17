/* The two sRGB post-passes over generated fragment programs:
 * rsx_fp_apply_srgb_output (SHADER_PACKER) and rsx_fp_apply_texture_gamma
 * (TEXTURE_ADDRESS gamma bits). Synthetic programs in the emitter's own
 * spellings. With an argument, also writes the patched programs to
 * <dir>/gamma_single.hlsl and gamma_mrt.hlsl for an HLSL compiler check
 * (glslangValidator -D -V -S frag -e main <file>). */
#include "../libs/video/rsx_fp_decompiler.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char kSingle[] =
    "Texture2D    rsx_tex[16] : register(t0);\n"
    "SamplerState rsx_samp[16] : register(s0);\n"
    "struct PSInput { float4 position : SV_POSITION; float4 tc0:TEXCOORD0; };\n"
    "float4 main(PSInput input) : SV_TARGET {\n"
    "    float4 r[4]; float4 h[4];\n"
    "    { float4 _v = (float4)(rsx_tex[0].Sample(rsx_samp[0], ((input.tc0).xyzw).xy)); h[0].xyzw = _v.xyzw; }\n"
    "    { float4 _v = (float4)(rsx_tex[1].SampleLevel(rsx_samp[1], (h[0]).xy, (h[0]).x)); h[1].xyzw = _v.xyzw; }\n"
    "    { float4 _v = (float4)(rsx_tex[10].Sample(rsx_samp[10], (h[1]).xy)); h[2].xyzw = _v.xyzw; }\n"
    "    { float4 _v = (float4)(rsx_tex[1].Sample(rsx_samp[1], (h[2]).xy)); h[3].xyzw = _v.xyzw; }\n"
    "    r[0] = h[0] + h[1] + h[2] + h[3];\n"
    "    float4 _o = r[0];\n"
    "    return (_o == _o) ? _o : (float4)0;\n}\n";

static const char kMrt[] =
    "Texture2D    rsx_tex[16] : register(t0);\n"
    "SamplerState rsx_samp[16] : register(s0);\n"
    "struct PSInput { float4 position : SV_POSITION; float4 tc0:TEXCOORD0; };\n"
    "struct PSOutput {\n    float4 t0 : SV_TARGET0;\n    float4 t1 : SV_TARGET1;\n};\n"
    "PSOutput main(PSInput input) {\n"
    "    float4 r[4];\n"
    "    r[0] = rsx_tex[2].SampleBias(rsx_samp[2], (input.tc0).xy, (input.tc0).z);\n"
    "    r[2] = r[0] * 0.5;\n"
    "    PSOutput _out;\n"
    "    float4 _o0 = r[0];\n"
    "    _out.t0 = (_o0 == _o0) ? _o0 : (float4)0;\n"
    "    float4 _o1 = r[2];\n"
    "    _out.t1 = (_o1 == _o1) ? _o1 : (float4)0;\n"
    "    return _out;\n}\n";

static int count(const char* s, const char* needle)
{
    int n = 0;
    for (const char* p = s; (p = strstr(p, needle)) != NULL; p += strlen(needle)) n++;
    return n;
}

static void save(const char* dir, const char* name, const char* text)
{
    if (!dir) return;
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE* f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : NULL;
    int checks = 0;

    /* Single target: units 1 (RGB) and 10 (RGBA) sRGB, unit 0 linear. */
    char hlsl[8192];
    snprintf(hlsl, sizeof hlsl, "%s", kSingle);
    u8 gamma[16] = {0};
    gamma[1] = 0x7; gamma[10] = 0xF;
    assert(rsx_fp_apply_texture_gamma(hlsl, sizeof hlsl, gamma) == 3); checks++;
    assert(count(hlsl, "rsx_gamma_tex(rsx_tex[1].Sample") == 2); checks++;
    assert(count(hlsl, "rsx_gamma_tex(rsx_tex[10].Sample(rsx_samp[10], (h[1]).xy), float4(1.0, 1.0, 1.0, 1.0))") == 1); checks++;
    assert(strstr(hlsl, "rsx_gamma_tex(rsx_tex[1].SampleLevel(rsx_samp[1], (h[0]).xy, (h[0]).x), float4(1.0, 1.0, 1.0, 0.0))")); checks++;
    assert(strstr(hlsl, "(float4)(rsx_tex[0].Sample(rsx_samp[0], ((input.tc0).xyzw).xy))")); checks++;
    assert(!strstr(hlsl, "rsx_TEX")); checks++;
    assert(count(hlsl, "/* rsx srgb decode */") == 1); checks++;
    assert(strstr(hlsl, "/* rsx srgb decode */") < strstr(hlsl, "float4 main(")); checks++;
    assert(rsx_fp_apply_srgb_output(hlsl, sizeof hlsl) == 1); checks++;
    assert(strstr(hlsl, "    float4 _o = r[0];\n    _o.rgb = rsx_srgb_encode(_o.rgb);\n    return (_o == _o)")); checks++;
    assert(strstr(hlsl, "/* rsx srgb encode */") < strstr(hlsl, "float4 main(")); checks++;
    assert(rsx_fp_apply_srgb_output(hlsl, sizeof hlsl) == 0); checks++;   /* once only */
    /* The alpha test still finds the final return after both passes. */
    assert(rsx_fp_apply_alpha_test(hlsl, sizeof hlsl, 0x204, 0.5f) == 1); checks++;
    assert(strstr(hlsl, "_rsx_out = (_o == _o) ? _o : (float4)0;")); checks++;
    save(dir, "gamma_single.hlsl", hlsl);

    /* MRT: every export encoded; SampleBias on a gamma unit. */
    snprintf(hlsl, sizeof hlsl, "%s", kMrt);
    memset(gamma, 0, sizeof gamma);
    gamma[2] = 0x8;
    assert(rsx_fp_apply_texture_gamma(hlsl, sizeof hlsl, gamma) == 1); checks++;
    assert(strstr(hlsl, "rsx_gamma_tex(rsx_tex[2].SampleBias(rsx_samp[2], (input.tc0).xy, (input.tc0).z), float4(0.0, 0.0, 0.0, 1.0))")); checks++;
    assert(rsx_fp_apply_srgb_output(hlsl, sizeof hlsl) == 2); checks++;
    assert(strstr(hlsl, "    float4 _o0 = r[0];\n    _o0.rgb = rsx_srgb_encode(_o0.rgb);\n")); checks++;
    assert(strstr(hlsl, "    float4 _o1 = r[2];\n    _o1.rgb = rsx_srgb_encode(_o1.rgb);\n")); checks++;
    assert(strstr(hlsl, "/* rsx srgb encode */") > strstr(hlsl, "struct PSOutput")); checks++;
    assert(strstr(hlsl, "/* rsx srgb encode */") < strstr(hlsl, "PSOutput main(")); checks++;
    save(dir, "gamma_mrt.hlsl", hlsl);

    /* No mask: byte-identical. Overflow: refused. */
    snprintf(hlsl, sizeof hlsl, "%s", kSingle);
    memset(gamma, 0, sizeof gamma);
    assert(rsx_fp_apply_texture_gamma(hlsl, sizeof hlsl, gamma) == 0); checks++;
    assert(!strcmp(hlsl, kSingle)); checks++;
    gamma[0] = 0x7;
    assert(rsx_fp_apply_texture_gamma(hlsl, (u32)strlen(hlsl) + 8, gamma) == -1); checks++;
    snprintf(hlsl, sizeof hlsl, "%s", kSingle);
    assert(rsx_fp_apply_srgb_output(hlsl, (u32)strlen(hlsl) + 8) == -1); checks++;
    assert(rsx_fp_apply_srgb_output(NULL, 0) == -1); checks++;

    printf("rsx fp gamma: %d checks passed\n", checks);
    return 0;
}
