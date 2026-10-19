/* Contract 0 authored captured-shader RGB. Host must validate the exact source
 * SHA256 alias before passing qualifiedSource=true. No physical PBR claim.
 * Define RSX_SCENE_RGB_MATERIAL_TYPE and these hooks before inclusion:
 * bool rsx_scene_rgb_load_constant(material, index, out vec4);
 * bool rsx_scene_rgb_sample(material, unit, uv, lod, out vec4);
 * void rsx_scene_rgb_invalidate(material, status);
 * The hooks own bounds checking and exact texture/sampler semantics.
 */
#ifndef RSX_SCENE_RGB_MATERIAL_TYPE
#define RSX_SCENE_RGB_MATERIAL_TYPE uint
#endif

const int RSX_SCENE_RGB_OK = 0;
const int RSX_SCENE_RGB_UNSUPPORTED = -1;
const int RSX_SCENE_RGB_INVALID = -2;
const int RSX_SCENE_RGB_SAMPLE_UNAVAILABLE = -3;
const int RSX_SCENE_RGB_NUMERIC_UNSUPPORTED = -4;
const uint RSX_SCENE_RGB_TEXTURE_MASK = 7u;
const uint RSX_SCENE_RGB_FP_MIN_BYTES = 256u;

/* Exact authored source terms. No physical light or material identification. */
struct RsxSceneRgbTerms {
    precise vec3 lut[2];
    precise vec3 constant_base;
    precise vec3 vertex_weighted[2];
    precise vec3 vertex_combined;
    precise vec3 prefloor;
    precise float floor_value;
    precise vec3 floor_residual;
    precise vec3 additive;
    precise vec3 overlay;
    precise vec2 fog_weights;
    precise vec3 fog_colors[2];
    precise float final_scalar;
};

struct RsxSceneRgbOutput {
    bool valid;
    precise vec3 authoredRGB;
    precise vec3 baseRGB; /* vertex-color/texture proxy, not albedo */
    precise float rawAlpha;
    precise float clampedCoverage;
    bool keep;
    RsxSceneRgbTerms source;
};

bool rsx_scene_rgb_finite(float x) { return !isnan(x) && !isinf(x); }
bool rsx_scene_rgb_finite3(vec3 x) { return all(not(isnan(x))) && all(not(isinf(x))); }
bool rsx_scene_rgb_finite4(vec4 x) { return all(not(isnan(x))) && all(not(isinf(x))); }

int rsx_scene_rgb_fail(RSX_SCENE_RGB_MATERIAL_TYPE material, int status,
                       out RsxSceneRgbOutput result)
{
    float nanValue = uintBitsToFloat(0x7fc00000u);
    result.valid = false;
    result.authoredRGB = vec3(nanValue);
    result.baseRGB = vec3(nanValue);
    result.rawAlpha = nanValue;
    result.clampedCoverage = nanValue;
    result.keep = false;
    result.source.floor_value = nanValue;
    result.source.final_scalar = nanValue;
    result.source.fog_weights = vec2(nanValue);
    result.source.constant_base = vec3(nanValue);
    result.source.vertex_combined = vec3(nanValue);
    result.source.prefloor = vec3(nanValue);
    result.source.floor_residual = vec3(nanValue);
    result.source.additive = vec3(nanValue);
    result.source.overlay = vec3(nanValue);
    for (int i=0;i<2;++i) {
        result.source.lut[i] = vec3(nanValue);
        result.source.vertex_weighted[i] = vec3(nanValue);
        result.source.fog_colors[i] = vec3(nanValue);
    }
    rsx_scene_rgb_invalidate(material, status);
    return status;
}

int rsx_scene_rgb_compose(const in RsxSceneRgbOutput inputValue,
                         vec2 visibility, vec3 indirect, out vec3 result)
{
    precise vec3 rgb;
    float nanValue = uintBitsToFloat(0x7fc00000u);
    result = vec3(nanValue);
    if (!inputValue.valid) return RSX_SCENE_RGB_INVALID;
    if (!rsx_scene_rgb_finite(visibility.x) || !rsx_scene_rgb_finite(visibility.y) ||
        !rsx_scene_rgb_finite3(indirect) || any(lessThan(visibility,vec2(0))) ||
        any(greaterThan(visibility,vec2(1)))) return RSX_SCENE_RGB_NUMERIC_UNSUPPORTED;
    RsxSceneRgbTerms t = inputValue.source;
    if (!rsx_scene_rgb_finite(t.floor_value) || !rsx_scene_rgb_finite(t.final_scalar) ||
        !rsx_scene_rgb_finite(t.fog_weights.x) || !rsx_scene_rgb_finite(t.fog_weights.y))
        return RSX_SCENE_RGB_NUMERIC_UNSUPPORTED;
    for (int i=0;i<3;++i) {
        precise float q = visibility.x==1.0 ? t.lut[0][i] : t.lut[0][i]*visibility.x;
        precise float r = visibility.y==1.0 ? t.lut[1][i] : t.lut[1][i]*visibility.y;
        precise float s,a,b;
        if (!rsx_scene_rgb_finite(q) || !rsx_scene_rgb_finite(r) ||
            !rsx_scene_rgb_finite(t.constant_base[i]) || !rsx_scene_rgb_finite(t.vertex_combined[i]) ||
            !rsx_scene_rgb_finite(t.additive[i]) || !rsx_scene_rgb_finite(t.overlay[i]) ||
            !rsx_scene_rgb_finite(t.fog_colors[0][i]) || !rsx_scene_rgb_finite(t.fog_colors[1][i]))
            return RSX_SCENE_RGB_NUMERIC_UNSUPPORTED;
        r = r+q;
        r = t.constant_base[i]+r;
        s = t.vertex_combined[i]+r;
        if (indirect[i]!=0.0) s = s+indirect[i];
        if (!rsx_scene_rgb_finite(r) || !rsx_scene_rgb_finite(s))
            return RSX_SCENE_RGB_NUMERIC_UNSUPPORTED;
        s = max(s,t.floor_value);
        s = s+t.additive[i];
        s = t.overlay[i]*s;
        a = (-t.fog_weights.x*t.fog_colors[0][i]+t.fog_colors[0][i])/2.0;
        s = (s*t.fog_weights.x+a)*2.0;
        b = -t.fog_weights.y*t.fog_colors[1][i]+t.fog_colors[1][i];
        s = s*t.fog_weights.y+b;
        rgb[i] = s*t.final_scalar;
        if (!rsx_scene_rgb_finite(a) || !rsx_scene_rgb_finite(b) ||
            !rsx_scene_rgb_finite(s) || !rsx_scene_rgb_finite(rgb[i]))
            return RSX_SCENE_RGB_NUMERIC_UNSUPPORTED;
    }
    result = rgb;
    return RSX_SCENE_RGB_OK;
}

int rsx_scene_rgb_evaluate(RSX_SCENE_RGB_MATERIAL_TYPE material,
                           bool qualifiedSource, uint contractId,
                           const in vec4 vertex[12], vec3 lod,
                           out RsxSceneRgbOutput result)
{
    precise vec4 c[16];
    precise vec3 n, a, b;
    precise float len2, len, dot0, dot1, u0, u1;
    precise vec4 lut0, lut1, tex, overlay;
    precise vec3 base, authored;
    precise float baseAlpha, rawAlpha;
    RsxSceneRgbTerms terms;
    if (!qualifiedSource || contractId != 0u)
        return rsx_scene_rgb_fail(material, RSX_SCENE_RGB_UNSUPPORTED, result);
    for (int i = 0; i < 16; ++i)
        if (!rsx_scene_rgb_load_constant(material, i, c[i]))
            return rsx_scene_rgb_fail(material, RSX_SCENE_RGB_INVALID, result);
    for (int i = 0; i < 16; ++i) {
        if (i == 0 || i == 1 || i == 6 || i == 7 || i == 8 ||
            i == 10 || i == 12 || i == 13) {
            if (!rsx_scene_rgb_finite3(c[i].rgb))
                return rsx_scene_rgb_fail(material, RSX_SCENE_RGB_NUMERIC_UNSUPPORTED, result);
        } else if (!rsx_scene_rgb_finite(c[i].x))
            return rsx_scene_rgb_fail(material, RSX_SCENE_RGB_NUMERIC_UNSUPPORTED, result);
    }
    if (!rsx_scene_rgb_finite4(vertex[1]) || !rsx_scene_rgb_finite4(vertex[2]) ||
        !rsx_scene_rgb_finite3(vec3(vertex[4].xy, vertex[5].z)) ||
        !rsx_scene_rgb_finite(vertex[5].w) ||
        !rsx_scene_rgb_finite3(vertex[6].xyz) ||
        !rsx_scene_rgb_finite3(vertex[7].xyz) ||
        !rsx_scene_rgb_finite3(vertex[11].xyz) || !rsx_scene_rgb_finite3(lod))
        return rsx_scene_rgb_fail(material, RSX_SCENE_RGB_NUMERIC_UNSUPPORTED, result);
    len2 = (vertex[6].x * vertex[6].x + vertex[6].y * vertex[6].y) + vertex[6].z * vertex[6].z;
    if (!rsx_scene_rgb_finite(len2) || len2 <= 0.0)
        return rsx_scene_rgb_fail(material, RSX_SCENE_RGB_NUMERIC_UNSUPPORTED, result);
    len = sqrt(len2);
    n = vertex[6].xyz / len;
    len2 = (vertex[7].x * vertex[7].x + vertex[7].y * vertex[7].y) + vertex[7].z * vertex[7].z;
    if (!rsx_scene_rgb_finite(len2) || len2 <= 0.0)
        return rsx_scene_rgb_fail(material, RSX_SCENE_RGB_NUMERIC_UNSUPPORTED, result);
    len = sqrt(len2);
    a = vertex[7].xyz / len;
    len2 = (vertex[11].x * vertex[11].x + vertex[11].y * vertex[11].y) + vertex[11].z * vertex[11].z;
    if (!rsx_scene_rgb_finite(len2) || len2 <= 0.0)
        return rsx_scene_rgb_fail(material, RSX_SCENE_RGB_NUMERIC_UNSUPPORTED, result);
    len = sqrt(len2);
    b = vertex[11].xyz / len;
    if (!rsx_scene_rgb_finite3(n) || !rsx_scene_rgb_finite3(a) || !rsx_scene_rgb_finite3(b))
        return rsx_scene_rgb_fail(material, RSX_SCENE_RGB_NUMERIC_UNSUPPORTED, result);
    dot0 = (n.x * b.x + n.y * b.y) + n.z * b.z;
    dot1 = (n.x * a.x + n.y * a.y) + n.z * a.z;
    u0 = dot0 / 2.0 + c[4].x;
    u1 = dot1 / 2.0 + c[5].x;
    if (!rsx_scene_rgb_finite(u0) || !rsx_scene_rgb_finite(u1))
        return rsx_scene_rgb_fail(material, RSX_SCENE_RGB_NUMERIC_UNSUPPORTED, result);
    if (!rsx_scene_rgb_sample(material, 1, vec2(u0, c[2].x), lod.y, lut0) ||
        !rsx_scene_rgb_sample(material, 1, vec2(u1, c[3].x), lod.y, lut1) ||
        !rsx_scene_rgb_sample(material, 0, vertex[4].xy, lod.x, tex) ||
        !rsx_scene_rgb_sample(material, 2, vertex[5].zw, lod.z, overlay))
        return rsx_scene_rgb_fail(material, RSX_SCENE_RGB_SAMPLE_UNAVAILABLE, result);
    if (!rsx_scene_rgb_finite(lut0.r) || !rsx_scene_rgb_finite(lut1.r) ||
        !rsx_scene_rgb_finite4(tex) || !rsx_scene_rgb_finite3(overlay.rgb))
        return rsx_scene_rgb_fail(material, RSX_SCENE_RGB_NUMERIC_UNSUPPORTED, result);
    for (int i = 0; i < 3; ++i) {
        precise float vertexLight = vertex[2].w * c[0][i];
        precise float q, r, lit, s, fogA, fogB;
        vertexLight = vertex[2].z * c[1][i] + vertexLight;
        base[i] = 2.0 * (vertex[1][i] * tex[i]);
        q = (base[i] * lut0.r) * c[6][i];
        terms.lut[0][i] = q;
        r = base[i] * lut1.r;
        terms.lut[1][i] = r*c[7][i];
        r = r * c[7][i] + q;
        terms.constant_base[i] = base[i]*c[8][i];
        r = base[i] * c[8][i] + r;
        terms.vertex_weighted[0][i] = base[i]*(vertex[2].w*c[0][i]);
        terms.vertex_weighted[1][i] = base[i]*(vertex[2].z*c[1][i]);
        terms.vertex_combined[i] = base[i]*vertexLight;
        lit = base[i] * vertexLight + r;
        terms.prefloor[i] = lit;
        s = max(lit, c[9].x);
        terms.floor_residual[i] = s-lit;
        terms.additive[i] = c[10][i];
        terms.overlay[i] = overlay[i];
        terms.fog_colors[0][i] = c[12][i];
        terms.fog_colors[1][i] = c[13][i];
        s = s + c[10][i];
        s = overlay[i] * s;
        fogA = (-vertex[2].x * c[12][i] + c[12][i]) / 2.0;
        s = (s * vertex[2].x + fogA) * 2.0;
        fogB = -vertex[2].y * c[13][i] + c[13][i];
        s = s * vertex[2].y + fogB;
        authored[i] = s * c[14].x;
        if (!rsx_scene_rgb_finite(base[i]) || !rsx_scene_rgb_finite(vertexLight) ||
            !rsx_scene_rgb_finite(q) || !rsx_scene_rgb_finite(r) ||
            !rsx_scene_rgb_finite(lit) || !rsx_scene_rgb_finite(s) ||
            !rsx_scene_rgb_finite(fogA) || !rsx_scene_rgb_finite(fogB) ||
            !rsx_scene_rgb_finite(authored[i]) || !rsx_scene_rgb_finite(terms.lut[1][i]) ||
            !rsx_scene_rgb_finite(terms.constant_base[i]) ||
            !rsx_scene_rgb_finite(terms.vertex_weighted[0][i]) ||
            !rsx_scene_rgb_finite(terms.vertex_weighted[1][i]) ||
            !rsx_scene_rgb_finite(terms.vertex_combined[i]) ||
            !rsx_scene_rgb_finite(terms.floor_residual[i]))
            return rsx_scene_rgb_fail(material, RSX_SCENE_RGB_NUMERIC_UNSUPPORTED, result);
    }
    baseAlpha = 2.0 * (vertex[1].a * tex.a);
    rawAlpha = baseAlpha * c[11].x;
    if (!rsx_scene_rgb_finite(baseAlpha) || !rsx_scene_rgb_finite(rawAlpha))
        return rsx_scene_rgb_fail(material, RSX_SCENE_RGB_NUMERIC_UNSUPPORTED, result);
    result.valid = true;
    terms.floor_value = c[9].x;
    terms.fog_weights = vertex[2].xy;
    terms.final_scalar = c[14].x;
    result.source = terms;
    result.authoredRGB = authored;
    result.baseRGB = base;
    result.rawAlpha = rawAlpha;
    result.clampedCoverage = clamp(rawAlpha, 0.0, 1.0);
    result.keep = rawAlpha > c[15].x;
    return RSX_SCENE_RGB_OK;
}
