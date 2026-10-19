#include "rsx_scene_rgb.h"

#include <math.h>
#include <string.h>

#ifdef __FAST_MATH__
#error "rsx_scene_rgb requires fast-math disabled"
#endif
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

static int hex_equal(const char *value, const char *expected)
{
    size_t i;
    if (!value) return 0;
    /* Bounded read: reject short strings and any 65th character. */
    for (i = 0; i < 64; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (c == 0 || !((c >= '0' && c <= '9') ||
                        (c >= 'a' && c <= 'f') ||
                        (c >= 'A' && c <= 'F'))) return 0;
        if (c >= 'A' && c <= 'F') c = (unsigned char)(c - 'A' + 'a');
        if (c != (unsigned char)expected[i]) return 0;
    }
    return value[64] == 0;
}

int rsx_scene_rgb_source_supported(uint32_t contract_id, const char *sha256)
{
    return contract_id == RSX_SCENE_RGB_CONTRACT_ID &&
        (hex_equal(sha256, "84d877ddc6083545452948630259c21ca796e7985fcd5e7b5a005666e4a0d634") ||
         hex_equal(sha256, "4b43ead21c9dce22ed005919077251929e3c5efb9890013745ece3afb0cfd4b7"));
}

static int finite_components(const float *v, unsigned count)
{
    unsigned i;
    for (i = 0; i < count; ++i) if (!isfinite(v[i])) return 0;
    return 1;
}

static int normalize3(const float in[4], float out[3])
{
    float x = in[0] * in[0];
    float y = in[1] * in[1];
    float z = in[2] * in[2];
    float length_squared = (x + y) + z;
    float length;
    unsigned i;
    if (!isfinite(length_squared) || length_squared <= 0.0f) return 0;
    length = sqrtf(length_squared);
    if (!isfinite(length) || length == 0.0f) return 0;
    for (i = 0; i < 3; ++i) {
        out[i] = in[i] / length;
        if (!isfinite(out[i])) return 0;
    }
    return 1;
}

static float dot3(const float a[3], const float b[3])
{
    float x = a[0] * b[0];
    float y = a[1] * b[1];
    float z = a[2] * b[2];
    return (x + y) + z;
}

static void invalidate(rsx_scene_rgb_output *out)
{
    unsigned i;
    out->valid = 0;
    out->keep = 0;
    out->raw_alpha = NAN;
    out->clamped_coverage = NAN;
    out->source.floor_value = out->source.final_scalar = NAN;
    out->source.fog_weights[0] = out->source.fog_weights[1] = NAN;
    for (i = 0; i < 3; ++i) {
        out->authored_rgb[i] = out->base_rgb[i] = NAN;
        out->source.constant_base[i] = out->source.vertex_combined[i] = NAN;
        out->source.prefloor[i] = out->source.floor_residual[i] = NAN;
        out->source.additive[i] = out->source.overlay[i] = NAN;
        for (unsigned j = 0; j < 2; ++j)
            out->source.lut[j][i] = out->source.vertex_weighted[j][i] =
                out->source.fog_colors[j][i] = NAN;
    }
}

rsx_scene_rgb_status rsx_scene_rgb_compose(const rsx_scene_rgb_output *in,
    const float visibility[2], const float indirect[3], float out[3])
{
    float rgb[3];
    unsigned i;
    if (!out) return RSX_SCENE_RGB_INVALID;
    for (i = 0; i < 3; ++i) out[i] = NAN;
    if (!in || !in->valid || !visibility || !indirect) return RSX_SCENE_RGB_INVALID;
    if (!finite_components(visibility, 2) || !finite_components(indirect, 3) ||
        visibility[0] < 0 || visibility[0] > 1 || visibility[1] < 0 || visibility[1] > 1)
        return RSX_SCENE_RGB_NUMERIC_UNSUPPORTED;
    const rsx_scene_rgb_terms *t = &in->source;
    if (!isfinite(t->floor_value) || !isfinite(t->final_scalar) ||
        !finite_components(t->fog_weights, 2)) return RSX_SCENE_RGB_NUMERIC_UNSUPPORTED;
    for (i = 0; i < 3; ++i) {
        float q = visibility[0] == 1 ? t->lut[0][i] : t->lut[0][i] * visibility[0];
        float r = visibility[1] == 1 ? t->lut[1][i] : t->lut[1][i] * visibility[1];
        float s, a, b;
        if (!isfinite(q) || !isfinite(r) || !isfinite(t->constant_base[i]) ||
            !isfinite(t->vertex_combined[i]) || !isfinite(t->additive[i]) ||
            !isfinite(t->overlay[i]) || !isfinite(t->fog_colors[0][i]) ||
            !isfinite(t->fog_colors[1][i])) return RSX_SCENE_RGB_NUMERIC_UNSUPPORTED;
        r = r + q;
        r = t->constant_base[i] + r;
        s = t->vertex_combined[i] + r;
        if (indirect[i] != 0.0f) s = s + indirect[i];
        if (!isfinite(r) || !isfinite(s)) return RSX_SCENE_RGB_NUMERIC_UNSUPPORTED;
        s = fmaxf(s, t->floor_value);
        s = s + t->additive[i];
        s = t->overlay[i] * s;
        a = ((-t->fog_weights[0] * t->fog_colors[0][i]) + t->fog_colors[0][i]) / 2.0f;
        s = ((s * t->fog_weights[0]) + a) * 2.0f;
        b = (-t->fog_weights[1] * t->fog_colors[1][i]) + t->fog_colors[1][i];
        s = (s * t->fog_weights[1]) + b;
        rgb[i] = s * t->final_scalar;
        if (!isfinite(a) || !isfinite(b) || !isfinite(s) || !isfinite(rgb[i]))
            return RSX_SCENE_RGB_NUMERIC_UNSUPPORTED;
    }
    for (i = 0; i < 3; ++i) out[i] = rgb[i];
    return RSX_SCENE_RGB_OK;
}

rsx_scene_rgb_status rsx_scene_rgb_evaluate(const rsx_scene_rgb_input *in,
                                            rsx_scene_rgb_output *out)
{
    const float (*c)[4];
    const float (*v)[4];
    float n[3], a[3], b[3], lut0[4], lut1[4], tex[4], overlay[4];
    float d0, d1, u0, u1, base_alpha, raw_alpha, authored[3], base[3];
    rsx_scene_rgb_terms terms;
    unsigned i;
    static const unsigned rgb_constants[] = {0, 1, 6, 7, 8, 10, 12, 13};
    static const unsigned scalar_constants[] = {2, 3, 4, 5, 9, 11, 14, 15};
    if (!out) return RSX_SCENE_RGB_INVALID;
    invalidate(out);
    if (!in) return RSX_SCENE_RGB_INVALID;
    if (!rsx_scene_rgb_source_supported(in->contract_id, in->source_sha256))
        return RSX_SCENE_RGB_UNSUPPORTED;
    if (!in->fp || !in->vertex || !in->sample ||
        in->fp_bytes < RSX_SCENE_RGB_FP_MIN_BYTES ||
        in->vertex_bytes < RSX_SCENE_RGB_VERTEX_FLOAT4_COUNT * 4u * sizeof(float))
        return RSX_SCENE_RGB_INVALID;
    c = in->fp;
    v = in->vertex;
    for (i = 0; i < sizeof(rgb_constants)/sizeof(rgb_constants[0]); ++i)
        if (!finite_components(c[rgb_constants[i]], 3)) return RSX_SCENE_RGB_NUMERIC_UNSUPPORTED;
    for (i = 0; i < sizeof(scalar_constants)/sizeof(scalar_constants[0]); ++i)
        if (!isfinite(c[scalar_constants[i]][0])) return RSX_SCENE_RGB_NUMERIC_UNSUPPORTED;
    if (!finite_components(v[1], 4) || !finite_components(v[2], 4) ||
        !finite_components(v[4], 2) || !isfinite(v[5][2]) || !isfinite(v[5][3]) ||
        !finite_components(v[6], 3) || !finite_components(v[7], 3) ||
        !finite_components(v[11], 3) || !finite_components(in->lod, 3))
        return RSX_SCENE_RGB_NUMERIC_UNSUPPORTED;
    if (!normalize3(v[6], n) || !normalize3(v[7], a) || !normalize3(v[11], b))
        return RSX_SCENE_RGB_NUMERIC_UNSUPPORTED;
    d0 = dot3(n, b);
    d1 = dot3(n, a);
    u0 = (d0 / 2.0f) + c[4][0];
    u1 = (d1 / 2.0f) + c[5][0];
    if (!isfinite(u0) || !isfinite(u1)) return RSX_SCENE_RGB_NUMERIC_UNSUPPORTED;
    if (!in->sample(in->sample_context, 1, u0, c[2][0], in->lod[1], lut0) ||
        !in->sample(in->sample_context, 1, u1, c[3][0], in->lod[1], lut1) ||
        !in->sample(in->sample_context, 0, v[4][0], v[4][1], in->lod[0], tex) ||
        !in->sample(in->sample_context, 2, v[5][2], v[5][3], in->lod[2], overlay))
        return RSX_SCENE_RGB_SAMPLE_UNAVAILABLE;
    if (!isfinite(lut0[0]) || !isfinite(lut1[0]) ||
        !finite_components(tex, 4) || !finite_components(overlay, 3))
        return RSX_SCENE_RGB_NUMERIC_UNSUPPORTED;
    for (i = 0; i < 3; ++i) {
        float vertex_light = v[2][3] * c[0][i];
        float q, r, lit, s, fog_a, fog_b;
        vertex_light = (v[2][2] * c[1][i]) + vertex_light;
        base[i] = 2.0f * (v[1][i] * tex[i]);
        q = (base[i] * lut0[0]) * c[6][i];
        terms.lut[0][i] = q;
        r = base[i] * lut1[0];
        terms.lut[1][i] = r * c[7][i];
        r = (r * c[7][i]) + q;
        terms.constant_base[i] = base[i] * c[8][i];
        r = (base[i] * c[8][i]) + r;
        terms.vertex_weighted[0][i] = base[i] * (v[2][3] * c[0][i]);
        terms.vertex_weighted[1][i] = base[i] * (v[2][2] * c[1][i]);
        terms.vertex_combined[i] = base[i] * vertex_light;
        lit = (base[i] * vertex_light) + r;
        terms.prefloor[i] = lit;
        s = fmaxf(lit, c[9][0]);
        terms.floor_residual[i] = s - lit;
        terms.additive[i] = c[10][i];
        terms.overlay[i] = overlay[i];
        terms.fog_colors[0][i] = c[12][i];
        terms.fog_colors[1][i] = c[13][i];
        s = s + c[10][i];
        s = overlay[i] * s;
        fog_a = ((-v[2][0] * c[12][i]) + c[12][i]) / 2.0f;
        s = ((s * v[2][0]) + fog_a) * 2.0f;
        fog_b = (-v[2][1] * c[13][i]) + c[13][i];
        s = (s * v[2][1]) + fog_b;
        authored[i] = s * c[14][0];
        if (!isfinite(base[i]) || !isfinite(vertex_light) || !isfinite(q) ||
            !isfinite(r) || !isfinite(lit) || !isfinite(s) ||
            !isfinite(fog_a) || !isfinite(fog_b) || !isfinite(authored[i]) ||
            !isfinite(terms.lut[1][i]) || !isfinite(terms.constant_base[i]) ||
            !isfinite(terms.vertex_weighted[0][i]) || !isfinite(terms.vertex_weighted[1][i]) ||
            !isfinite(terms.vertex_combined[i]) || !isfinite(terms.floor_residual[i]))
            return RSX_SCENE_RGB_NUMERIC_UNSUPPORTED;
    }
    base_alpha = 2.0f * (v[1][3] * tex[3]);
    raw_alpha = base_alpha * c[11][0];
    if (!isfinite(base_alpha) || !isfinite(raw_alpha))
        return RSX_SCENE_RGB_NUMERIC_UNSUPPORTED;
    for (i = 0; i < 3; ++i) {
        out->base_rgb[i] = base[i];
        out->authored_rgb[i] = authored[i];
    }
    out->raw_alpha = raw_alpha;
    terms.floor_value = c[9][0];
    terms.fog_weights[0] = v[2][0]; terms.fog_weights[1] = v[2][1];
    terms.final_scalar = c[14][0];
    out->source = terms;
    out->clamped_coverage = fminf(fmaxf(raw_alpha, 0.0f), 1.0f);
    out->keep = raw_alpha > c[15][0];
    out->valid = 1;
    return RSX_SCENE_RGB_OK;
}
