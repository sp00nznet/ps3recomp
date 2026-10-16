#version 450
#extension GL_GOOGLE_include_directive : require

// FSR1 RCAS (robust contrast adaptive sharpening) pass.
// The algorithm comes unchanged from AMD FidelityFX FSR 1 (MIT license),
// pinned under third_party/fsr1; see the license file there.
// Input and output are RGBA8_UNORM display-encoded color with no implicit sRGB
// conversion. FSR_RCAS_DENOISE and FSR_RCAS_PASSTHROUGH_ALPHA stay undefined.

layout(set = 0, binding = 0) uniform sampler2D source_texture;

// Constants produced by the standard CPU FsrRcasCon.
layout(push_constant) uniform RcasConstants {
    layout(offset = 0) uvec4 con0;
} pc;

layout(location = 0) out vec4 out_color;

#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"

#define FSR_RCAS_F 1
#include "ffx_fsr1.h"

// Clamp loads to the texture bounds so edge pixels and 1x1 sources stay in range.
ivec2 ClampSourceTexel(ivec2 p)
{
    return clamp(p, ivec2(0), textureSize(source_texture, 0) - ivec2(1));
}

// Load and input-transform callbacks declared by ffx_fsr1.h.
AF4 FsrRcasLoadF(ASU2 p) { return texelFetch(source_texture, ClampSourceTexel(p), 0); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}

void main()
{
    AU2 ip = AU2(gl_FragCoord.xy);

    AF1 r;
    AF1 g;
    AF1 b;
    FsrRcasF(r, g, b, ip, pc.con0);

    // Sharpening only touches RGB; alpha is copied exactly from the center texel.
    float alpha = texelFetch(source_texture, ClampSourceTexel(ivec2(ip)), 0).a;

    out_color = vec4(r, g, b, alpha);
}
