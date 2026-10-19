#ifndef RSX_RAY_FOOTPRINT_H
#define RSX_RAY_FOOTPRINT_H

/*
 * Conservative isotropic ray-cone texture LOD for one triangle hit.
 *
 * p:           triangle vertices in Euclidean view space.
 * uv:          texture coordinates (any range; wrapping/negative allowed).
 * direction:   nonzero finite ray direction, need not be normalized.
 * cone_radius: finite >= 0, measured perpendicular to the ray at the hit.
 *
 * The cone radius is projected onto the surface with incidence
 * |dot(n, normalize(direction))| (floored to RSX_RAY_FOOTPRINT_MIN_INCIDENCE).
 * rho = footprint diameter * max(width * |grad u|, height * |grad v|), where
 * the surface gradients come from the triangle edges via the Gram inverse.
 * Result: max(0, log2(max(rho, 1))) clamped to floor(log2(max(w, h))).
 * This is not equivalent to raster derivatives.
 *
 * Edges, UV deltas and the direction are halved and rescaled by their
 * largest component, and log2(rho) is evaluated as a sum of finite log
 * terms, so extreme but finite inputs cannot overflow.
 *
 * Rejected (returns 0, *out_lod untouched): NULL pointers, nonfinite inputs,
 * negative radius, zero texture extent, direction or edge scale below
 * FLT_MIN, and near-degenerate triangles: sin(angle at p[0]) <=
 * RSX_RAY_FOOTPRINT_MIN_SIN or squared edge length ratio at p[0] below
 * RSX_RAY_FOOTPRINT_MIN_EDGE_RATIO2. Both thresholds are scale invariant.
 * A radius or UV extent below FLT_MIN yields LOD 0 after validation.
 *
 * Float-only math; mirrored by rsx_ray_footprint.glsl.
 */

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#define RSX_RAY_FOOTPRINT_MIN_SIN 1.0e-4f
#define RSX_RAY_FOOTPRINT_MIN_SIN2 1.0e-8f
#define RSX_RAY_FOOTPRINT_MIN_EDGE_RATIO2 1.0e-12f
#define RSX_RAY_FOOTPRINT_MIN_INCIDENCE 1.0e-4f

/* Bit test so the check survives compilers that assume no NaN/inf. */
static inline int rsx_ray_footprint_finite_(float x)
{
    uint32_t bits;
    memcpy(&bits, &x, sizeof bits);
    return (bits & 0x7F800000u) != 0x7F800000u;
}

static inline float rsx_ray_footprint_absmax3_(const float v[3])
{
    float m = fabsf(v[0]);
    if (fabsf(v[1]) > m)
        m = fabsf(v[1]);
    if (fabsf(v[2]) > m)
        m = fabsf(v[2]);
    return m;
}

static inline float rsx_ray_footprint_dot3_(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static inline int rsx_ray_footprint_lod(const float p[3][3], const float uv[3][2],
                                        const float direction[3], float cone_radius,
                                        unsigned texture_width, unsigned texture_height,
                                        float *out_lod)
{
    float e1[3], e2[3], n[3], dn[3], d1[2], d2[2], len[2];
    float s, l1, l2, g12, n2, ds, m, texel, inc, lod, max_mip;
    unsigned extent;
    int i, k;

    if (!p || !uv || !direction || !out_lod)
        return 0;
    if (texture_width == 0u || texture_height == 0u)
        return 0;
    for (i = 0; i < 3; ++i) {
        for (k = 0; k < 3; ++k)
            if (!rsx_ray_footprint_finite_(p[i][k]))
                return 0;
        if (!rsx_ray_footprint_finite_(uv[i][0]) || !rsx_ray_footprint_finite_(uv[i][1]))
            return 0;
        if (!rsx_ray_footprint_finite_(direction[i]))
            return 0;
    }
    if (!rsx_ray_footprint_finite_(cone_radius) || !(cone_radius >= 0.0f))
        return 0;

    /* Half edges cannot overflow; rescale so the largest component is 1. */
    for (k = 0; k < 3; ++k) {
        e1[k] = 0.5f * p[1][k] - 0.5f * p[0][k];
        e2[k] = 0.5f * p[2][k] - 0.5f * p[0][k];
    }
    s = rsx_ray_footprint_absmax3_(e1);
    if (rsx_ray_footprint_absmax3_(e2) > s)
        s = rsx_ray_footprint_absmax3_(e2);
    if (!(s >= FLT_MIN))
        return 0;
    for (k = 0; k < 3; ++k) {
        e1[k] /= s;
        e2[k] /= s;
    }

    l1 = rsx_ray_footprint_dot3_(e1, e1);
    l2 = rsx_ray_footprint_dot3_(e2, e2);
    g12 = rsx_ray_footprint_dot3_(e1, e2);
    if (!((l1 < l2 ? l1 : l2) >= RSX_RAY_FOOTPRINT_MIN_EDGE_RATIO2 * (l1 > l2 ? l1 : l2)))
        return 0;

    /* |e1 x e2|^2 equals the Gram determinant without its cancellation. */
    n[0] = e1[1] * e2[2] - e1[2] * e2[1];
    n[1] = e1[2] * e2[0] - e1[0] * e2[2];
    n[2] = e1[0] * e2[1] - e1[1] * e2[0];
    n2 = rsx_ray_footprint_dot3_(n, n);
    if (!(n2 > RSX_RAY_FOOTPRINT_MIN_SIN2 * l1 * l2))
        return 0;

    ds = rsx_ray_footprint_absmax3_(direction);
    if (!(ds >= FLT_MIN))
        return 0;
    for (k = 0; k < 3; ++k)
        dn[k] = direction[k] / ds;

    extent = texture_width > texture_height ? texture_width : texture_height;
    max_mip = 0.0f;
    while (extent >>= 1)
        max_mip += 1.0f;

    if (cone_radius < FLT_MIN) {
        *out_lod = 0.0f;
        return 1;
    }

    for (k = 0; k < 2; ++k) {
        d1[k] = 0.5f * uv[1][k] - 0.5f * uv[0][k];
        d2[k] = 0.5f * uv[2][k] - 0.5f * uv[0][k];
    }
    m = fabsf(d1[0]);
    if (fabsf(d1[1]) > m)
        m = fabsf(d1[1]);
    if (fabsf(d2[0]) > m)
        m = fabsf(d2[0]);
    if (fabsf(d2[1]) > m)
        m = fabsf(d2[1]);
    if (!(m >= FLT_MIN)) {
        *out_lod = 0.0f;
        return 1;
    }

    /* grad = (a * e1 + b * e2) / det with [a b] = adj(G) * [d1 d2]. */
    for (k = 0; k < 2; ++k) {
        float a, b, g[3];
        d1[k] /= m;
        d2[k] /= m;
        a = l2 * d1[k] - g12 * d2[k];
        b = l1 * d2[k] - g12 * d1[k];
        for (i = 0; i < 3; ++i)
            g[i] = a * e1[i] + b * e2[i];
        len[k] = sqrtf(rsx_ray_footprint_dot3_(g, g));
    }
    texel = (float)texture_width * len[0];
    if ((float)texture_height * len[1] > texel)
        texel = (float)texture_height * len[1];
    if (!(texel >= FLT_MIN)) {
        *out_lod = 0.0f;
        return 1;
    }

    inc = fabsf(rsx_ray_footprint_dot3_(n, dn)) /
          (sqrtf(n2) * sqrtf(rsx_ray_footprint_dot3_(dn, dn)));
    if (!(inc >= RSX_RAY_FOOTPRINT_MIN_INCIDENCE))
        inc = RSX_RAY_FOOTPRINT_MIN_INCIDENCE;
    if (inc > 1.0f)
        inc = 1.0f;

    /* log2(2r / inc * texel * 2m / (2s * det')) with every term finite. */
    lod = 1.0f + log2f(cone_radius) + log2f(m) - log2f(s) - log2f(n2) - log2f(inc) +
          log2f(texel);
    if (!rsx_ray_footprint_finite_(lod))
        return 0;
    if (lod < 0.0f)
        lod = 0.0f;
    if (lod > max_mip)
        lod = max_mip;
    *out_lod = lod;
    return 1;
}

#endif /* RSX_RAY_FOOTPRINT_H */
