#version 450
#extension GL_GOOGLE_include_directive : require

// FSR1 EASU (edge adaptive spatial upsampling) pass.
// The algorithm comes unchanged from AMD FidelityFX FSR 1 (MIT license),
// pinned under third_party/fsr1; see the license file there.
// Input and output are RGBA8_UNORM display-encoded color with no implicit sRGB
// conversion and no premultiply/unpremultiply.

layout(set = 0, binding = 0) uniform sampler2D source_texture;

// Constants produced by the standard CPU FsrEasuCon.
layout(push_constant) uniform EasuConstants {
    layout(offset = 0) uvec4 con0;
    layout(offset = 16) uvec4 con1;
    layout(offset = 32) uvec4 con2;
    layout(offset = 48) uvec4 con3;
} pc;

layout(location = 0) out vec4 out_color;

#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"

#define FSR_EASU_F 1
#include "ffx_fsr1.h"

// Gather callbacks declared by ffx_fsr1.h. The sampler is linear clamp-to-edge.
AF4 FsrEasuRF(AF2 p) { return textureGather(source_texture, p, 0); }
AF4 FsrEasuGF(AF2 p) { return textureGather(source_texture, p, 1); }
AF4 FsrEasuBF(AF2 p) { return textureGather(source_texture, p, 2); }

void main()
{
    AU2 ip = AU2(gl_FragCoord.xy);

    AF3 pix;
    FsrEasuF(pix, ip, pc.con0, pc.con1, pc.con2, pc.con3);

    // EASU only reconstructs RGB. Alpha is plain bilinear scaling at the same
    // source position: con0.xy = output-to-input ratio, con0.zw = 0.5*ratio-0.5,
    // con1.xy = 1/input size.
    vec2 ratio = uintBitsToFloat(pc.con0.xy);
    vec2 offset = uintBitsToFloat(pc.con0.zw);
    vec2 inverse_input_size = uintBitsToFloat(pc.con1.xy);
    vec2 uv = (vec2(ip) * ratio + offset + 0.5) * inverse_input_size;
    float alpha = textureLod(source_texture, uv, 0.0).a;

    out_color = vec4(pix, alpha);
}
