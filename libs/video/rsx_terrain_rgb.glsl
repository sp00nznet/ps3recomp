// pipe90 terrain RGB: deterministic replay of the owned source register program plus explicit
// relighting insertions. Included after the pool helper (RsxPoolRgbTerms, RsxPoolRgbOutput,
// rsx_pool_rgb_* hooks). Provenance gate: tests/test_terrain_rgb_source.py.
#ifndef RSX_TERRAIN_RGB_GLSL
#define RSX_TERRAIN_RGB_GLSL

#define RSX_TERRAIN_RGB_CONSTANT_COUNT 23u

vec4 rsx_terrain_rgb_tex(uint material, uint unit, vec2 uv, float lod[5], inout bool ok)
{
    vec4 s = vec4(0.0);
    if (!rsx_pool_rgb_sample2d(material, unit, uv, lod[unit], s)) { ok = false; return vec4(0.0); }
    return s;
}

// Per-channel equivalent of the source's (o == o) ? o : 0.
vec4 rsx_terrain_rgb_nan0(vec4 o) { return mix(o, vec4(0.0), isnan(o)); }
bool rsx_terrain_rgb_finite(vec3 x) { return !any(isnan(x)) && !any(isinf(x)); }

void rsx_terrain_rgb_clear_terms(out RsxPoolRgbTerms t)
{
    for (int i = 0; i < 4; ++i) { t.direct[i] = vec3(0.0); t.diffuseDirect[i] = vec3(0.0); }
    t.reflectionDirection = vec3(0.0); t.diffuseProxy = vec3(0.0);
    t.prefloor = vec3(0.0); t.floored = vec3(0.0); t.additive = vec3(0.0); t.overlay = vec3(0.0);
    t.postfog_additive = vec3(0.0); t.transportValid = false; t.reflectionValid = false;
    t.fog_weights = vec2(0.0); t.fog_colors[0] = vec3(0.0); t.fog_colors[1] = vec3(0.0);
}

bool rsx_terrain_rgb_replay(bool modified, uint material, vec4 c[23], RsxSceneAlphaVertexOutput v, float lod[5],
                            vec4 vis, vec3 ind, out vec4 o0, out vec4 o1, out vec4 o2, inout RsxPoolRgbTerms t)
{
    bool ok = true;
    precise vec4 r[4];
    precise vec4 h[8];
    for (int i = 0; i < 4; ++i) r[i] = vec4(0.0);
    for (int i = 0; i < 8; ++i) h[i] = vec4(0.0);
    // BEGIN SOURCE REPLAY
    { precise vec4 _v = vec4(rsx_terrain_rgb_tex(material, 0u, ((v.t0).zwzz).xy, lod, ok)); r[0].xyz = _v.xyz; }
    { precise vec4 _v = vec4(((r[0]).xyzw) * ((c[0]).xxxx) + ((c[0]).yyyy)); h[3].xyz = _v.xyz; }
    { precise vec4 _v = vec4((v.col1).xyzw); h[2].xyzw = _v.xyzw; }
    { precise vec4 _v = vec4(((h[2]).wwww) * ((c[1]).xyzw)); h[0].xyz = _v.xyz; }
    t.direct[3] = h[0].xyz; // term
    if (modified && vis.w != 1.0) h[0].xyz = h[0].xyz * vis.w; // relight
    { precise vec4 _v = vec4(vec4(normalize(((v.t4).xyzw).xyz), 1.0)); h[4].xyz = _v.xyz; }
    t.direct[2] = h[2].zzz * c[2].xyz; // term
    if (modified && vis.z != 1.0) h[2].z = h[2].z * vis.z; // relight
    { precise vec4 _v = vec4(((h[2]).zzzz) * ((c[2]).xyzw) + ((h[0]).xyzw)); h[1].xyz = _v.xyz; }
    { precise vec4 _v = vec4(vec4(normalize(((h[3]).xyzw).xyz), 1.0)); h[3].xyz = _v.xyz; }
    { precise vec4 _v = vec4((-((h[2]).yyyy)) * ((c[3]).xyzw) + ((c[3]).xyzw)); h[0].xyz = _v.xyz; }
    { precise vec4 _v = vec4(dot(((h[3]).xyzw).xyz, ((h[4]).xyzw).xyz)); r[1].y = _v.y; }
    { precise vec4 _v = vec4((-(abs((r[1]).yyyy))) + ((c[4]).xxxx)); _v = clamp(_v, 0.0, 1.0); r[3].z = _v.z; }
    { precise vec4 _v = vec4(vec4(normalize(((v.t3).xyzw).xyz), 1.0)); h[4].xyz = _v.xyz; }
    { precise vec4 _v = vec4(dot(((h[3]).xyzw).xyz, ((h[4]).xyzw).xyz)) / 2.0; r[1].y = _v.y; }
    { precise vec4 _v = vec4(((r[3]).zwzz) + ((c[5]).xyxx)); r[3].xy = _v.xy; }
    { precise vec4 _v = vec4(((r[1]).yyyy) + ((c[6]).xyzw)); r[3].z = _v.z; }
    { precise vec4 _v = vec4((c[7]).xxxx); r[3].w = _v.w; }
    { precise vec4 _v = vec4(rsx_terrain_rgb_tex(material, 1u, ((v.t0).xyzw).xy, lod, ok)); h[4].xyzw = _v.xyzw; }
    { precise vec4 _v = vec4(vec4(normalize(((v.t7).xyzw).xyz), 1.0)); h[5].xyz = _v.xyz; }
    { precise vec4 _v = vec4(dot(((h[3]).xyzw).xyz, ((h[5]).xyzw).xyz)) / 2.0; r[1].y = _v.y; }
    { precise vec4 _v = vec4((c[8]).xxxx); r[2].w = _v.w; }
    { precise vec4 _v = vec4(((r[1]).yyyy) + ((c[9]).xxxx)); r[2].z = _v.z; }
    { precise vec4 _v = vec4(rsx_terrain_rgb_tex(material, 4u, ((r[3]).xyzw).xy, lod, ok)); h[3].xyz = _v.xyz; }
    { precise vec4 _v = vec4(((h[3]).xyzw) * ((c[10]).zzzz)); h[3].xyz = _v.xyz; }
    t.postfog_additive = h[3].xyz; // term
    { precise vec4 _v = vec4(rsx_terrain_rgb_tex(material, 2u, ((r[2]).zwzz).xy, lod, ok)); h[5].x = _v.x; }
    { precise vec4 _v = vec4(((v.col0).xyzw) * ((h[4]).xyzw)) * 2.0; h[4].xyzw = _v.xyzw; }
    t.diffuseProxy = h[4].xyz; // term
    { precise vec4 _v = vec4(((h[4]).xyzw) * ((h[5]).xxxx)); h[5].xyz = _v.xyz; }
    { precise vec4 _v = vec4(((h[5]).xyzw) * ((c[11]).xyzw)); h[6].xyz = _v.xyz; }
    t.direct[0] = h[6].xyz; // term
    if (modified && vis.x != 1.0) h[6].xyz = h[6].xyz * vis.x; // relight
    { precise vec4 _v = vec4(rsx_terrain_rgb_tex(material, 2u, ((r[3]).zwzz).xy, lod, ok)); h[5].x = _v.x; }
    { precise vec4 _v = vec4(((h[4]).xyzw) * ((h[5]).xxxx)); h[7].xyz = _v.xyz; }
    { precise vec4 _v = vec4((-((h[2]).xxxx)) * ((c[12]).xyzw) + ((c[12]).xyzw)) / 2.0; h[5].xyz = _v.xyz; }
    t.direct[1] = h[7].xyz * c[13].xyz; // term
    if (modified && vis.y != 1.0) h[7].xyz = h[7].xyz * vis.y; // relight
    { precise vec4 _v = vec4(((h[7]).xyzw) * ((c[13]).xyzw) + ((h[6]).xyzw)); h[6].xyz = _v.xyz; }
    { precise vec4 _v = vec4(((h[4]).xyzw) * ((c[14]).xyzw) + ((h[6]).xyzw)); h[6].xyz = _v.xyz; }
    { precise vec4 _v = vec4(((h[4]).xyzw) * ((c[15]).xyzw) + ((h[4]).xyzw)); h[0].w = _v.w; }
    { precise vec4 _v = vec4(((h[4]).xyzw) * ((h[1]).xyzw) + ((h[6]).xyzw)); h[1].xyz = _v.xyz; }
    t.prefloor = h[1].xyz; // term
    if (modified && any(notEqual(ind, vec3(0.0)))) h[1].xyz = h[1].xyz + ind; // relight
    { precise vec4 _v = vec4(max(((h[1]).xyzw), ((c[16]).xxxx))); h[1].xyz = _v.xyz; }
    t.floored = h[1].xyz; // term
    { precise vec4 _v = vec4(((h[1]).xyzw) + ((c[17]).xyzw)); h[4].xyz = _v.xyz; }
    { precise vec4 _v = vec4(rsx_terrain_rgb_tex(material, 3u, ((v.t1).zwzz).xy, lod, ok)); h[1].xyz = _v.xyz; }
    t.overlay = h[1].xyz; // term
    { precise vec4 _v = vec4(((h[1]).xyzw) * ((h[4]).xyzw)); h[4].xyz = _v.xyz; }
    { precise vec4 _v = vec4(((h[4]).xyzw) * ((h[2]).xxxx) + ((h[5]).xyzw)) * 2.0; h[1].xyz = _v.xyz; }
    { precise vec4 _v = vec4(((h[1]).xyzw) * ((h[2]).yyyy) + ((h[0]).xyzw)); h[0].xyz = _v.xyz; }
    { precise vec4 _v = vec4(((h[0]).xyzw) * ((c[18]).xxxx) + ((h[3]).xyzw)); h[0].xyz = _v.xyz; }
    { precise vec4 _v = vec4(max(((h[0]).xyzw), ((c[19]).xxxx))); h[0].xyzw = _v.xyzw; }
    { precise vec4 _v = vec4(min(((h[0]).wwww), ((c[20]).xxxx))); h[1].x = _v.x; }
    { precise vec4 _v = vec4(((h[1]).xxxx) * ((c[21]).xxxx)); r[0].w = _v.w; }
    { precise vec4 _v = vec4((h[0]).xyzw); r[0].xyz = _v.xyz; }
    // END SOURCE REPLAY
    o0 = rsx_terrain_rgb_nan0(r[0]);
    o1 = rsx_terrain_rgb_nan0(r[2]);
    o2 = rsx_terrain_rgb_nan0(r[3]);
    return ok;
}

int rsx_terrain_rgb_evaluate(bool qualifiedSource, uint material, uint fp_base, uint fp_count,
                             RsxSceneAlphaVertexOutput v, float lod[5], vec4 visibility, vec3 indirect,
                             inout RsxPoolRgbOutput result)
{
    if (!qualifiedSource) return -1;
    if (fp_count != RSX_TERRAIN_RGB_CONSTANT_COUNT || fp_base > 0xFFFFFFFFu - (RSX_TERRAIN_RGB_CONSTANT_COUNT - 1u)) return -2;
    vec4 c[23];
    for (uint i = 0u; i < RSX_TERRAIN_RGB_CONSTANT_COUNT; ++i)
        if (!rsx_pool_rgb_load_constant(fp_base + i, c[i])) return -2;
    if (any(isnan(visibility)) || any(isinf(visibility)) || !rsx_terrain_rgb_finite(indirect) ||
        any(lessThan(visibility, vec4(0.0))) || any(greaterThan(visibility, vec4(1.0)))) return -4;
    for (int i = 0; i < 5; ++i)
        if (isnan(lod[i]) || isinf(lod[i])) return -4;

    RsxPoolRgbTerms terms;
    rsx_terrain_rgb_clear_terms(terms);
    vec4 m0, m1, m2;
    if (!rsx_terrain_rgb_replay(false, material, c, v, lod, visibility, indirect, m0, m1, m2, terms)) return -3;
    vec4 composed = m0;
    if (any(notEqual(visibility, vec4(1.0))) || any(notEqual(indirect, vec3(0.0)))) {
        RsxPoolRgbTerms scratch;
        rsx_terrain_rgb_clear_terms(scratch);
        vec4 n1, n2;
        if (!rsx_terrain_rgb_replay(true, material, c, v, lod, visibility, indirect, composed, n1, n2, scratch)) return -3;
    }

    // Vertex-light terms are multiplied by the base texture at source statement33.
    // Retain that factor in secondary radiance, just as in the primary replay.
    terms.direct[2] = terms.diffuseProxy * terms.direct[2];
    terms.direct[3] = terms.diffuseProxy * terms.direct[3];
    bool valid = rsx_terrain_rgb_finite(terms.diffuseProxy) &&
                 all(greaterThanEqual(terms.diffuseProxy, vec3(0.0))) && all(lessThanEqual(terms.diffuseProxy, vec3(1.0)));
    for (int i = 0; i < 4; ++i) {
        terms.diffuseDirect[i] = terms.direct[i];
        valid = valid && rsx_terrain_rgb_finite(terms.direct[i]) && all(greaterThanEqual(terms.direct[i], vec3(0.0)));
    }
    terms.transportValid = valid;
    terms.reflectionDirection = vec3(0.0);
    terms.reflectionValid = false;
    terms.additive = c[17].xyz;
    terms.fog_weights = v.col1.xy;
    terms.fog_colors[0] = c[12].xyz;
    terms.fog_colors[1] = c[3].xyz;

    result.originalRGBA = m0;
    result.composedRGBA = composed;
    result.mrt[0] = m0;
    result.mrt[1] = m1;
    result.mrt[2] = m2;
    result.mrt[3] = vec4(0.0);
    result.source = terms;
    return 0;
}

#endif // RSX_TERRAIN_RGB_GLSL
