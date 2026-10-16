/* Software depth-texture comparison ("shadow" sampling) for the native
 * renderer. The backend snapshots depth into a plain R32_FLOAT sampled
 * texture, so hardware SampleCmp (which needs a depth-format view) is not
 * usable. Instead the helper below fetches individual texel centres through
 * the guest's own sampler (so its wrap modes still apply), compares each
 * fetched depth against the reference, and only then filters the comparison
 * results. A bilinearly filtered depth is never compared.
 *
 * Scope: 2D, single-mip depth textures. Cubemap and multi-mip shadow inputs
 * are rejected by the caller and are deliberately not handled here.
 *
 * Semantics derived from RPCS3 (GPL-2.0), revision
 * 4d5def5880217135554ed434cd5055cceebe9cc2:
 *   https://github.com/RPCS3/rpcs3/blob/4d5def5880217135554ed434cd5055cceebe9cc2/rpcs3/Emu/RSX/RSXThread.cpp
 *     (depth formats with a texture zfunc of 1..6 and alpha kill disabled
 *      are treated as shadow inputs)
 *   https://github.com/RPCS3/rpcs3/blob/4d5def5880217135554ed434cd5055cceebe9cc2/rpcs3/Emu/RSX/Program/FragmentProgramDecompiler.cpp
 *     (shadow TEX/TXP replicate the scalar comparison as .xxxx)
 * Only the semantics are followed; no upstream code is copied.
 *
 * Header is C and C++ compatible: it only defines constants and one string. */
#ifndef RSX_SHADOW_SAMPLING_H
#define RSX_SHADOW_SAMPLING_H

/* compare_func values (the 3-bit RSX texture zfunc field). */
#define RSX_SHADOW_NEVER    0u
#define RSX_SHADOW_LESS     1u
#define RSX_SHADOW_EQUAL    2u
#define RSX_SHADOW_LEQUAL   3u
#define RSX_SHADOW_GREATER  4u
#define RSX_SHADOW_NOTEQUAL 5u
#define RSX_SHADOW_GEQUAL   6u
#define RSX_SHADOW_ALWAYS   7u

/* Standalone HLSL helper definitions: no entry point, no resources.
 *
 * float4 rsx_shadow_sample(Texture2D tex, SamplerState samp, float4 coord,
 *                          uint compare_func, uint projected, uint unnormalized,
 *                          uint min_linear, uint mag_linear)
 *
 * - projected: xyz are divided by w first; otherwise the reference is coord.z.
 * - unnormalized: xy are texel-space coordinates; z is never rescaled.
 * - The result is (reference OP depth) as 0/1, bilinearly blended when the
 *   selected filter is linear, replicated into all four channels.
 * - Filter selection: minification when the texel-space screen derivative
 *   rho exceeds 1, i.e. log2(rho) > 0, the same lambda > 0 test the
 *   Vulkan/D3D level-of-detail computation uses for a single-mip texture
 *   (sampler LOD bias is not modelled). Derivatives are taken before any
 *   branch, so call it from uniform control flow. */
static const char rsx_shadow_hlsl[] =
"float rsx_shadow_compare(float ref, float depth, uint func)\n"
"{\n"
"    func = func & 7u;\n"
"    if (func == 1u) return ref <  depth ? 1.0 : 0.0;\n"
"    if (func == 2u) return ref == depth ? 1.0 : 0.0;\n"
"    if (func == 3u) return ref <= depth ? 1.0 : 0.0;\n"
"    if (func == 4u) return ref >  depth ? 1.0 : 0.0;\n"
"    if (func == 5u) return ref != depth ? 1.0 : 0.0;\n"
"    if (func == 6u) return ref >= depth ? 1.0 : 0.0;\n"
"    if (func == 7u) return 1.0;\n"
"    return 0.0;\n"
"}\n"
"\n"
"#define RSX_SHADOW_FETCH(texel) tex.SampleLevel(samp, ((texel) + 0.5) / size, 0.0).r\n"
"float4 rsx_shadow_sample(Texture2D tex, SamplerState samp, float4 coord, uint compare_func,\n"
"                         uint projected, uint unnormalized, uint min_linear, uint mag_linear)\n"
"{\n"
"    float3 c = coord.xyz;\n"
"    if (projected != 0u) c = coord.xyz / coord.w;\n"
"    uint w, h;\n"
"    tex.GetDimensions(w, h);\n"
"    float2 size = float2(w, h);\n"
"    /* Texel-space position; unnormalized xy already are texel space. */\n"
"    float2 t = c.xy * size;\n"
"    if (unnormalized != 0u) t = c.xy;\n"
"    float2 tdx = ddx(t);\n"
"    float2 tdy = ddy(t);\n"
"    uint lin = mag_linear;\n"
"    if (max(dot(tdx, tdx), dot(tdy, tdy)) > 1.0) lin = min_linear;\n"
"    float r;\n"
"    if (lin != 0u) {\n"
"        float2 q = t - 0.5;\n"
"        float2 b = floor(q);\n"
"        float2 f = q - b;\n"
"        float s00 = rsx_shadow_compare(c.z, RSX_SHADOW_FETCH(b), compare_func);\n"
"        float s10 = rsx_shadow_compare(c.z, RSX_SHADOW_FETCH(b + float2(1.0, 0.0)), compare_func);\n"
"        float s01 = rsx_shadow_compare(c.z, RSX_SHADOW_FETCH(b + float2(0.0, 1.0)), compare_func);\n"
"        float s11 = rsx_shadow_compare(c.z, RSX_SHADOW_FETCH(b + float2(1.0, 1.0)), compare_func);\n"
"        r = lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);\n"
"    } else {\n"
"        r = rsx_shadow_compare(c.z, RSX_SHADOW_FETCH(floor(t)), compare_func);\n"
"    }\n"
"    return float4(r, r, r, r);\n"
"}\n"
"#undef RSX_SHADOW_FETCH\n";

#endif /* RSX_SHADOW_SAMPLING_H */
