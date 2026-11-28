/* NV4097 point sprites: draw-type identity and bounded HLSL patching.
 * PointCoord is Vulkan-only; other translators retain their existing path. */
#ifndef PS3RECOMP_RSX_POINT_SPRITE_H
#define PS3RECOMP_RSX_POINT_SPRITE_H
#include <stddef.h>
#include <stdio.h>
#include <string.h>
static inline unsigned rsx_point_sprite_key(unsigned primitive, unsigned control)
{
    return primitive == 1u ? 0x40000000u | ((control & 1u) ? ((control >> 8) & 0x3FFu) : 0u) : 0u;
}
static inline float rsx_point_sprite_grid(unsigned sx, unsigned sy, float ratio[2])
{
    if (!sx) sx = 1;
    if (!sy) sy = 1;
    unsigned m = sx > sy ? sx : sy;
    ratio[0] = (float)sx / m; ratio[1] = (float)sy / m;
    return (float)m;
}
static inline void rsx_point_sprite_insert(char* s, size_t at, const char* text)
{
    size_t n = strlen(text);
    memmove(s + at + n, s + at, strlen(s + at) + 1);
    memcpy(s + at, text, n);
}
static inline int rsx_point_sprite_patch(char* s, size_t cap, unsigned key)
{
    if (!key) return 0;
    if (!s || !cap) return -1;
    if (strstr(s, "_rsx_sprite_point")) return 0;
    char* st = strstr(s, "struct PSInput {");
    char* cb = strstr(s, "cbuffer PSConstants");
    char* main = strstr(s, "main(PSInput input)");
    if (!st || !cb || !main) return -1;
    char* send = strstr(st, "};");
    char* cend = strstr(cb, "};");
    char* body = strchr(main, '{');
    if (!send || !cend || !body || send >= cb || cend >= body) return -1;
    const char* member = "\n#ifdef RSX_POINT_COORD_BUILTIN\n"
        "    [[vk::builtin(\"PointCoord\")]] float2 _rsx_sprite_point : TEXCOORD15;\n#endif\n";
    const char* uniform = "\n#ifdef RSX_POINT_COORD_BUILTIN\n    float4 fp_pointgrid;\n#endif\n";
    char prefix[2048];
    int n = snprintf(prefix, sizeof prefix,
        "\n#ifdef RSX_POINT_COORD_BUILTIN\n"
        "    float2 _rsx_sprite_uv = (input._rsx_sprite_point - 0.5) / max(fp_pointgrid.xy, float2(0.000001,0.000001)) + 0.5;\n"
        "    if ((fp_pointgrid.x < 1.0 && (_rsx_sprite_uv.x < 0.0 || _rsx_sprite_uv.x > 1.0)) || (fp_pointgrid.y < 1.0 && (_rsx_sprite_uv.y < 0.0 || _rsx_sprite_uv.y > 1.0))) discard;\n");
    if (n < 0 || (size_t)n >= sizeof prefix) return -1;
    for (unsigned u = 0; u < 10; ++u) {
        if (!(key & (1u << u))) continue;
        char ref[32]; snprintf(ref, sizeof ref, "input.tc%u", u);
        /* Only a declared/read interpolant can need replacement. */
        if (!strstr(s, ref)) continue;
        int added = snprintf(prefix + n, sizeof prefix - (size_t)n,
            "    input.tc%u = float4(_rsx_sprite_uv, 0.0, 1.0);\n", u);
        if (added < 0 || (size_t)added >= sizeof prefix - (size_t)n) return -1;
        n += added;
    }
    if ((size_t)n + sizeof "#endif\n" > sizeof prefix) return -1;
    memcpy(prefix + n, "#endif\n", sizeof "#endif\n");
    size_t total = strlen(s) + strlen(member) + strlen(uniform) + strlen(prefix) + 1;
    if (total > cap) return -1; /* no mutation on capacity failure */
    size_t a = (size_t)(send - s), b = (size_t)(cend - s), c = (size_t)(body - s) + 1;
    rsx_point_sprite_insert(s, c, prefix);
    rsx_point_sprite_insert(s, b, uniform);
    rsx_point_sprite_insert(s, a, member);
    return 1;
}
#endif
