/* rsx_light_space.h - recover authored light parameters in view space.
 *
 * Given the affine matrices a game used to place the same model into its
 * light space and into view space, light_to_view = model_to_view *
 * inverse(model_to_light) maps authored light parameters into view space.
 * This only re-expresses existing authored data; it never invents lights or
 * radiometry. Callers separately prove the exact source and matrix
 * relationships.
 *
 * Matrices are row-major affine 3x4: [R | t], implicit last row (0 0 0 1).
 *
 * Qualification rules:
 *  - The inverted 3x3 of model_to_light must have a nonzero finite
 *    determinant and Frobenius condition estimate ||A||_F * ||A^-1||_F <=
 *    RSX_LIGHT_SPACE_MAX_CONDITION.
 *  - The light_to_view 3x3 must be orthogonal: every entry of R^T R must be
 *    within RSX_LIGHT_SPACE_ORTHO_TOLERANCE of the identity (entries are unit
 *    scale, so this is a relative tolerance). Determinant -1 is allowed; dot
 *    products are preserved, stretching and shear are not.
 *  - All inputs must be finite and all results representable as finite floats.
 *
 * Every function returns 1 on success and 0 on failure. On failure no output
 * is written, and outputs may alias any input.
 */
#ifndef RSX_LIGHT_SPACE_H
#define RSX_LIGHT_SPACE_H

#include <float.h>
#include <math.h>

#if defined(__FAST_MATH__)
#error "rsx_light_space.h requires IEEE semantics; do not build with fast-math"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_LIGHT_SPACE_ORTHO_TOLERANCE 1e-4
#define RSX_LIGHT_SPACE_MAX_CONDITION 1e9

static inline int rsx_light_space__finite(const float *v, int n)
{
    int i;
    for (i = 0; i < n; ++i)
        if (!isfinite(v[i]))
            return 0;
    return 1;
}

static inline int rsx_light_space__store(double v, float *out)
{
    if (!(fabs(v) <= (double)FLT_MAX))
        return 0;
    *out = (float)v;
    return 1;
}

static inline int rsx_light_space__orthogonal(const float (*m)[4])
{
    int i, j;
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j) {
            double d = (double)m[0][i] * m[0][j] + (double)m[1][i] * m[1][j] +
                       (double)m[2][i] * m[2][j];
            if (!(fabs(d - (i == j ? 1.0 : 0.0)) <= RSX_LIGHT_SPACE_ORTHO_TOLERANCE))
                return 0;
        }
    return 1;
}

static inline int rsx_light_space__qualified(const float (*m)[4])
{
    int i;
    for (i = 0; i < 3; ++i)
        if (!rsx_light_space__finite(m[i], 4))
            return 0;
    return rsx_light_space__orthogonal(m);
}

/* light_to_view = model_to_view * inverse(model_to_light). */
static inline int rsx_light_space_build(const float model_to_light[3][4],
                                        const float model_to_view[3][4],
                                        float light_to_view[3][4])
{
    double a[3][3], inv[3][3], lt[3], det, norm_a = 0.0, norm_inv = 0.0;
    float out[3][4];
    int i, j;

    if (!model_to_light || !model_to_view || !light_to_view)
        return 0;
    for (i = 0; i < 3; ++i)
        if (!rsx_light_space__finite(model_to_light[i], 4) ||
            !rsx_light_space__finite(model_to_view[i], 4))
            return 0;

    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j)
            a[i][j] = model_to_light[i][j];
    inv[0][0] = a[1][1] * a[2][2] - a[1][2] * a[2][1];
    inv[0][1] = a[0][2] * a[2][1] - a[0][1] * a[2][2];
    inv[0][2] = a[0][1] * a[1][2] - a[0][2] * a[1][1];
    inv[1][0] = a[1][2] * a[2][0] - a[1][0] * a[2][2];
    inv[1][1] = a[0][0] * a[2][2] - a[0][2] * a[2][0];
    inv[1][2] = a[0][2] * a[1][0] - a[0][0] * a[1][2];
    inv[2][0] = a[1][0] * a[2][1] - a[1][1] * a[2][0];
    inv[2][1] = a[0][1] * a[2][0] - a[0][0] * a[2][1];
    inv[2][2] = a[0][0] * a[1][1] - a[0][1] * a[1][0];
    det = a[0][0] * inv[0][0] + a[0][1] * inv[1][0] + a[0][2] * inv[2][0];
    if (!(det != 0.0) || !isfinite(det))
        return 0;
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j) {
            inv[i][j] /= det;
            norm_a += a[i][j] * a[i][j];
            norm_inv += inv[i][j] * inv[i][j];
        }
    if (!(sqrt(norm_a) * sqrt(norm_inv) <= RSX_LIGHT_SPACE_MAX_CONDITION))
        return 0;

    for (i = 0; i < 3; ++i)
        lt[i] = inv[i][0] * model_to_light[0][3] + inv[i][1] * model_to_light[1][3] +
                inv[i][2] * model_to_light[2][3];
    for (i = 0; i < 3; ++i) {
        const float *v = model_to_view[i];
        for (j = 0; j < 3; ++j)
            if (!rsx_light_space__store(v[0] * inv[0][j] + v[1] * inv[1][j] + v[2] * inv[2][j],
                                        &out[i][j]))
                return 0;
        if (!rsx_light_space__store(v[3] - (v[0] * lt[0] + v[1] * lt[1] + v[2] * lt[2]),
                                    &out[i][3]))
            return 0;
    }
    if (!rsx_light_space__orthogonal((const float (*)[4])out))
        return 0;

    for (i = 0; i < 3; ++i)
        for (j = 0; j < 4; ++j)
            light_to_view[i][j] = out[i][j];
    return 1;
}

/* output = (R * source.xyz + t * source.w, source.w). The authored w is kept
 * exactly; nothing is normalized or promoted to a point. The source vector at
 * a shaded position P is output.xyz - P * output.w. */
static inline int rsx_light_space_homogeneous(const float light_to_view[3][4],
                                              const float source[4], float output[4])
{
    float out[4];
    int i;

    if (!light_to_view || !source || !output)
        return 0;
    if (!rsx_light_space__qualified(light_to_view) || !rsx_light_space__finite(source, 4))
        return 0;
    for (i = 0; i < 3; ++i) {
        const float *m = light_to_view[i];
        double v = (double)m[0] * source[0] + (double)m[1] * source[1] +
                   (double)m[2] * source[2] + (double)m[3] * source[3];
        if (!rsx_light_space__store(v, &out[i]))
            return 0;
    }
    out[3] = source[3];

    for (i = 0; i < 4; ++i)
        output[i] = out[i];
    return 1;
}

/* Physical shadow-ray toward a view-space homogeneous light.
 * w == 0: direction = normalize(xyz), distance = INFINITY.
 * w > 0: center = xyz / w, direction = normalize(center - view_position),
 *        distance = |center - view_position|.
 * Rejects w < 0, zero vectors, coincident points, nonfinite input and any
 * center or distance not representable as a finite normal float. */
static inline int rsx_light_space_ray(const float homogeneous[4], const float view_position[3],
                                      float direction[3], float *distance)
{
    double w, d[3], len2 = 0.0, len;
    float dir[3], dist = INFINITY;
    int i;

    if (!homogeneous || !view_position || !direction || !distance)
        return 0;
    if (!rsx_light_space__finite(homogeneous, 4) || !rsx_light_space__finite(view_position, 3))
        return 0;
    w = homogeneous[3];
    if (w < 0.0)
        return 0;
    for (i = 0; i < 3; ++i) {
        if (w == 0.0) {
            d[i] = homogeneous[i];
        } else {
            double center = homogeneous[i] / w;
            if (!(fabs(center) <= (double)FLT_MAX))
                return 0;
            d[i] = center - view_position[i];
        }
        len2 += d[i] * d[i];
    }
    len = sqrt(len2);
    if (!(len > 0.0) || !isfinite(len))
        return 0;
    if (w != 0.0) {
        if (!(len >= (double)FLT_MIN && len <= (double)FLT_MAX))
            return 0;
        dist = (float)len;
    }
    for (i = 0; i < 3; ++i)
        dir[i] = (float)(d[i] / len);

    for (i = 0; i < 3; ++i)
        direction[i] = dir[i];
    *distance = dist;
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* RSX_LIGHT_SPACE_H */
