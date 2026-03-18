/*
 * ps3recomp - D3D12 Built-in Shaders
 *
 * Pre-compiled HLSL shaders for basic rendering before RSX shader
 * translation is implemented. These handle:
 *   - Vertex position + color passthrough
 *   - Solid color fill
 *   - Textured quad (for framebuffer display)
 *
 * In the future, RSX vertex/fragment programs will be translated to
 * HLSL at runtime. These built-in shaders serve as fallbacks and
 * for debug visualization.
 */

#ifndef PS3RECOMP_RSX_D3D12_SHADERS_H
#define PS3RECOMP_RSX_D3D12_SHADERS_H

/* ---------------------------------------------------------------------------
 * Basic vertex-colored shader (position + color passthrough)
 *
 * Vertex input:  float3 POSITION, float4 COLOR
 * Pixel output:  float4 color
 * -----------------------------------------------------------------------*/

static const char s_vs_basic_hlsl[] =
    "struct VSInput {\n"
    "    float3 position : POSITION;\n"
    "    float4 color    : COLOR;\n"
    "};\n"
    "struct VSOutput {\n"
    "    float4 position : SV_POSITION;\n"
    "    float4 color    : COLOR;\n"
    "};\n"
    "VSOutput main(VSInput input) {\n"
    "    VSOutput output;\n"
    "    output.position = float4(input.position, 1.0);\n"
    "    output.color = input.color;\n"
    "    return output;\n"
    "}\n";

static const char s_ps_basic_hlsl[] =
    "struct PSInput {\n"
    "    float4 position : SV_POSITION;\n"
    "    float4 color    : COLOR;\n"
    "};\n"
    "float4 main(PSInput input) : SV_TARGET {\n"
    "    return input.color;\n"
    "}\n";

/* ---------------------------------------------------------------------------
 * Solid color fill shader (no vertex attributes needed)
 *
 * Uses a constant buffer for the fill color.
 * -----------------------------------------------------------------------*/

static const char s_vs_solid_hlsl[] =
    "float4 main(uint vertexID : SV_VertexID) : SV_POSITION {\n"
    "    // Full-screen triangle trick: 3 vertices cover the entire screen\n"
    "    float2 uv = float2((vertexID << 1) & 2, vertexID & 2);\n"
    "    return float4(uv * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char s_ps_solid_hlsl[] =
    "cbuffer Constants : register(b0) {\n"
    "    float4 fillColor;\n"
    "};\n"
    "float4 main() : SV_TARGET {\n"
    "    return fillColor;\n"
    "}\n";

/* ---------------------------------------------------------------------------
 * Textured quad shader (for framebuffer display)
 *
 * Vertex input: float2 POSITION, float2 TEXCOORD
 * Samples from texture at register t0
 * -----------------------------------------------------------------------*/

static const char s_vs_textured_hlsl[] =
    "struct VSInput {\n"
    "    float2 position : POSITION;\n"
    "    float2 texcoord : TEXCOORD;\n"
    "};\n"
    "struct VSOutput {\n"
    "    float4 position : SV_POSITION;\n"
    "    float2 texcoord : TEXCOORD;\n"
    "};\n"
    "VSOutput main(VSInput input) {\n"
    "    VSOutput output;\n"
    "    output.position = float4(input.position, 0.0, 1.0);\n"
    "    output.texcoord = input.texcoord;\n"
    "    return output;\n"
    "}\n";

static const char s_ps_textured_hlsl[] =
    "Texture2D    tex0 : register(t0);\n"
    "SamplerState samp0 : register(s0);\n"
    "struct PSInput {\n"
    "    float4 position : SV_POSITION;\n"
    "    float2 texcoord : TEXCOORD;\n"
    "};\n"
    "float4 main(PSInput input) : SV_TARGET {\n"
    "    return tex0.Sample(samp0, input.texcoord);\n"
    "}\n";

#endif /* PS3RECOMP_RSX_D3D12_SHADERS_H */
