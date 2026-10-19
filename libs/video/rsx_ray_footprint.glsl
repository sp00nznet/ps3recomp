#ifndef RSX_RAY_FOOTPRINT_GLSL
#define RSX_RAY_FOOTPRINT_GLSL

// Conservative isotropic ray-cone texture LOD; mirrors rsx_ray_footprint.h.
// See the C header for the full contract and documented thresholds.
// On failure returns false and does not assign lod; GLSL copy-out then leaves
// the caller's variable undefined, so always test the return value.
// Include after #version; no extensions required.

const float RSX_RAY_FOOTPRINT_FLT_MIN = 1.17549435e-38;
const float RSX_RAY_FOOTPRINT_MIN_SIN2 = 1.0e-8;
const float RSX_RAY_FOOTPRINT_MIN_EDGE_RATIO2 = 1.0e-12;
const float RSX_RAY_FOOTPRINT_MIN_INCIDENCE = 1.0e-4;

// Bit test so the check survives drivers that assume no NaN/inf.
bool rsx_ray_footprint_finite_(float x)
{
    return (floatBitsToUint(x) & 0x7F800000u) != 0x7F800000u;
}

bool rsx_ray_footprint_finite3_(vec3 v)
{
    return rsx_ray_footprint_finite_(v.x) && rsx_ray_footprint_finite_(v.y) &&
           rsx_ray_footprint_finite_(v.z);
}

bool rsx_ray_footprint_finite2_(vec2 v)
{
    return rsx_ray_footprint_finite_(v.x) && rsx_ray_footprint_finite_(v.y);
}

float rsx_ray_footprint_absmax3_(vec3 v)
{
    vec3 a = abs(v);
    return max(max(a.x, a.y), a.z);
}

bool rsx_ray_footprint_lod(vec3 p0, vec3 p1, vec3 p2, vec2 uv0, vec2 uv1, vec2 uv2,
                           vec3 direction, float cone_radius, uvec2 texture_size,
                           out float lod)
{
    if (texture_size.x == 0u || texture_size.y == 0u)
        return false;
    if (!rsx_ray_footprint_finite3_(p0) || !rsx_ray_footprint_finite3_(p1) ||
        !rsx_ray_footprint_finite3_(p2) || !rsx_ray_footprint_finite2_(uv0) ||
        !rsx_ray_footprint_finite2_(uv1) || !rsx_ray_footprint_finite2_(uv2) ||
        !rsx_ray_footprint_finite3_(direction) || !rsx_ray_footprint_finite_(cone_radius))
        return false;
    if (!(cone_radius >= 0.0))
        return false;

    // Half edges cannot overflow; rescale so the largest component is 1.
    vec3 e1 = 0.5 * p1 - 0.5 * p0;
    vec3 e2 = 0.5 * p2 - 0.5 * p0;
    float s = max(rsx_ray_footprint_absmax3_(e1), rsx_ray_footprint_absmax3_(e2));
    if (!(s >= RSX_RAY_FOOTPRINT_FLT_MIN))
        return false;
    e1 /= s;
    e2 /= s;

    float l1 = dot(e1, e1);
    float l2 = dot(e2, e2);
    float g12 = dot(e1, e2);
    if (!(min(l1, l2) >= RSX_RAY_FOOTPRINT_MIN_EDGE_RATIO2 * max(l1, l2)))
        return false;

    // |e1 x e2|^2 equals the Gram determinant without its cancellation.
    vec3 n = cross(e1, e2);
    float n2 = dot(n, n);
    if (!(n2 > RSX_RAY_FOOTPRINT_MIN_SIN2 * l1 * l2))
        return false;

    float ds = rsx_ray_footprint_absmax3_(direction);
    if (!(ds >= RSX_RAY_FOOTPRINT_FLT_MIN))
        return false;
    vec3 dn = direction / ds;

    float max_mip = float(findMSB(max(texture_size.x, texture_size.y)));

    if (cone_radius < RSX_RAY_FOOTPRINT_FLT_MIN) {
        lod = 0.0;
        return true;
    }

    vec2 d1 = 0.5 * uv1 - 0.5 * uv0;
    vec2 d2 = 0.5 * uv2 - 0.5 * uv0;
    float m = max(max(abs(d1.x), abs(d1.y)), max(abs(d2.x), abs(d2.y)));
    if (!(m >= RSX_RAY_FOOTPRINT_FLT_MIN)) {
        lod = 0.0;
        return true;
    }
    d1 /= m;
    d2 /= m;

    // grad = (a * e1 + b * e2) / det with [a b] = adj(G) * [d1 d2]; x = u, y = v.
    vec2 a = l2 * d1 - g12 * d2;
    vec2 b = l1 * d2 - g12 * d1;
    vec3 gu = a.x * e1 + b.x * e2;
    vec3 gv = a.y * e1 + b.y * e2;
    float texel = max(float(texture_size.x) * sqrt(dot(gu, gu)),
                      float(texture_size.y) * sqrt(dot(gv, gv)));
    if (!(texel >= RSX_RAY_FOOTPRINT_FLT_MIN)) {
        lod = 0.0;
        return true;
    }

    float inc = abs(dot(n, dn)) / (sqrt(n2) * sqrt(dot(dn, dn)));
    if (!(inc >= RSX_RAY_FOOTPRINT_MIN_INCIDENCE))
        inc = RSX_RAY_FOOTPRINT_MIN_INCIDENCE;
    inc = min(inc, 1.0);

    // log2(2r / inc * texel * 2m / (2s * det')) with every term finite.
    float l = 1.0 + log2(cone_radius) + log2(m) - log2(s) - log2(n2) - log2(inc) +
              log2(texel);
    if (!rsx_ray_footprint_finite_(l))
        return false;
    lod = clamp(l, 0.0, max_mip);
    return true;
}

#endif // RSX_RAY_FOOTPRINT_GLSL
