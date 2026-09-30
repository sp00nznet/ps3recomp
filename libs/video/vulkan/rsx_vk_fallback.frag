#version 450
// Untextured: the flat colour. Textured: a nearest, wrapping point sample of
// texture unit 0 (the sampler is created NEAREST/REPEAT), alpha forced opaque.
layout(location = 0) in vec4 v_col;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
layout(set = 0, binding = 0) uniform sampler2D u_tex0;
layout(push_constant) uniform PC { int textured; } pc;
void main()
{
    o_col = (pc.textured != 0) ? vec4(texture(u_tex0, v_uv).rgb, 1.0)
                               : vec4(v_col.rgb, 1.0);
}
