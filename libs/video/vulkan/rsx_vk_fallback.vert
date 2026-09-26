#version 450
// ps3recomp Vulkan backend, fallback path (no guest shaders yet).
// Same contract as the headless null backend's software rasteriser:
// attribute 0 is the clip-space position, used as given (w == 0 -> 1).
// NV4097's NDC has +y up; Vulkan's framebuffer has +y down, hence the flip.
layout(location = 0) in vec4 in_pos;
layout(location = 1) in vec4 in_col;   // flat: the triangle's first vertex colour, replicated on the CPU
layout(location = 2) in vec2 in_uv;    // attribute 8, texcoord0
layout(location = 0) out vec4 v_col;
layout(location = 1) out vec2 v_uv;
void main()
{
    float w = (in_pos.w != 0.0) ? in_pos.w : 1.0;
    gl_Position = vec4(in_pos.x, -in_pos.y, in_pos.z, w);
    v_col = in_col;
    v_uv  = in_uv;
}
