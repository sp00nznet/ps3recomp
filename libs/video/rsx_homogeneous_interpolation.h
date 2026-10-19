/*
 * CPU reference for ray-hit varying interpolation on source vertices whose
 * homogeneous position has a non-unit w.
 *
 * The ray gather divides each replayed vertex by its recovered homogeneous
 * scale h to build a Euclidean view-space triangle, so ray barycentrics
 * (beta) are Euclidean. The rasterizer, however, interpolates varyings
 * perspective-correctly against clip.w = h * view.z, which is equivalent to
 * weighting each Euclidean barycentric by 1/h. Using beta directly is only
 * correct when all three h are equal.
 *
 * Projection is (sx, sy, A, B) for a positive-Z projection with
 * clip.z = A*view.z + B*view.w and clip.w = view.z. Clip values are the final
 * replay values before the viewport depth range is applied. The caller has
 * already verified the shader position/projection relationship.
 *
 * Scope: this fixes generic varyings only. It does not relax the separate
 * unit-homogeneous qualification for authored lights; lights on non-unit
 * meshes remain unsupported.
 *
 * The GLSL port must use the same operation order and `precise` (no FMA
 * contraction) to match this reference.
 */
#ifndef RSX_HOMOGENEOUS_INTERPOLATION_H
#define RSX_HOMOGENEOUS_INTERPOLATION_H

#include <math.h>
#include <stddef.h>

/*
 * Computes raster-equivalent varying weights for a ray hit.
 *
 * clip[i]     replay clip position (x, y, z, w) of vertex i
 * projection  (sx, sy, A, B); requires sx > 0, sy > 0, A >= 1, B < 0
 * depth_range (min, max); requires 0 <= min < max <= 1
 * bary        Euclidean ray barycentrics (beta1, beta2); not clamped, so
 *             slight negative edge roundoff is accepted
 * out         weights for vertices 0..2
 *
 * Returns 1 on success. Returns 0 on invalid input and leaves out unchanged.
 * out may alias any input.
 */
static inline int rsx_homogeneous_weights(const float clip[3][4], const float projection[4],
                                          const float depth_range[2], const float bary[2],
                                          float out[3])
{
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif
    float c[3][4];
    float sx, sy, a, b, dmin, dmax, range, bx, by;
    float h[3], beta[3], r[3], w[3], denom;
    int i, j;

    if (clip == NULL || projection == NULL || depth_range == NULL || bary == NULL || out == NULL)
        return 0;

    /* Copy everything first so out may alias any input. */
    for (i = 0; i < 3; ++i) {
        for (j = 0; j < 4; ++j) {
            c[i][j] = clip[i][j];
            if (!isfinite(c[i][j]))
                return 0;
        }
    }
    sx = projection[0];
    sy = projection[1];
    a = projection[2];
    b = projection[3];
    dmin = depth_range[0];
    dmax = depth_range[1];
    bx = bary[0];
    by = bary[1];
    if (!isfinite(sx) || !isfinite(sy) || !isfinite(a) || !isfinite(b) || !isfinite(dmin) ||
        !isfinite(dmax) || !isfinite(bx) || !isfinite(by))
        return 0;
    if (!(sx > 0.0f) || !(sy > 0.0f) || !(a >= 1.0f) || !(b < 0.0f))
        return 0;
    if (!(dmin >= 0.0f) || !(dmin < dmax) || !(dmax <= 1.0f))
        return 0;

    /*
     * Undo the viewport depth mapping, then invert clip.z = A*view.z + B*view.w
     * with view.z = clip.w. This is the same inverse homogeneous scale the ray
     * gather uses to build Euclidean view-space vertices.
     */
    range = dmax - dmin;
    for (i = 0; i < 3; ++i) {
        h[i] = ((range * c[i][2] + dmin * c[i][3]) - a * c[i][3]) / b;
        if (!isfinite(h[i]) || !(h[i] > 1e-20f))
            return 0;
    }

    beta[0] = (1.0f - bx) - by;
    beta[1] = bx;
    beta[2] = by;
    if (!isfinite(beta[0]))
        return 0;

    for (i = 0; i < 3; ++i)
        r[i] = beta[i] / h[i];
    denom = (r[0] + r[1]) + r[2];
    if (!isfinite(denom) || !(denom > 0.0f))
        return 0;

    if (h[0] == h[1] && h[1] == h[2]) {
        /* Equal scales: perspective weighting cancels; preserve beta exactly. */
        for (i = 0; i < 3; ++i)
            w[i] = beta[i];
    } else {
        for (i = 0; i < 3; ++i)
            w[i] = r[i] / denom;
    }
    for (i = 0; i < 3; ++i) {
        if (!isfinite(w[i]))
            return 0;
    }

    out[0] = w[0];
    out[1] = w[1];
    out[2] = w[2];
    return 1;
}

#endif /* RSX_HOMOGENEOUS_INTERPOLATION_H */
