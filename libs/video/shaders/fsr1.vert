#version 450

// Fullscreen triangle for the FSR1 spatial passes. No vertex buffers are bound;
// the three vertices are generated from gl_VertexIndex and cover the whole
// viewport. The viewport has positive height and Vulkan's top-left origin, so
// no Y flip is applied.

void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
