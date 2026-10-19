/*
 * rsx_homogeneous_light.h - reference light-space recovery for homogeneous
 * (non-unit positive w) skinned positions.
 *
 * Source attribution: original work written for the Folklore RSX full path
 * tracing integration (homogeneous-light-reference task, base 1654139).
 * No external code was used.
 *
 * Inputs:
 *   M = [R|t]          affine light_to_view (3x4, row-major)
 *   L = (Lx,Ly,Lz,Lw)  game light (Lw = 0 direction, Lw > 0 point)
 *   P = (Px,Py,Pz,h)   post-skin homogeneous position, h > 0 (not assumed 1)
 *
 * The game shader forms Q = Lxyz - Pxyz*Lw without dividing by h. The view
 * point is X = (R*Pxyz + t*h)/h. This helper returns the apparent view light
 *
 *   E = (R*Lxyz/h + t*Lw, Lw)
 *
 * which satisfies E.xyz - X*E.w == R*Q/h for any affine M. Directions
 * (Lw == 0) ignore translation. Points (Lw > 0) sit at E.xyz/Lw. Negative Lw
 * is algebraically valid and returned unchanged; consumers may reject it.
 *
 * Rigidity is NOT validated here: the caller owns the proof that M is a rigid
 * frame (needed for distances/normals to stay meaningful in view space).
 *
 * Returns 1 on success. Returns 0, leaving out untouched, for null pointers,
 * nonfinite matrix/light/h, h <= 1e-20, or a component outside +-FLT_MAX.
 * out may alias light. Intermediates use double to avoid float overflow and
 * cancellation.
 */
#ifndef RSX_HOMOGENEOUS_LIGHT_H
#define RSX_HOMOGENEOUS_LIGHT_H

#include <float.h>
#include <math.h>
#include <stddef.h>

static inline int rsx_homogeneous_light_transform(const float matrix[3][4], const float light[4], float h, float out[4])
{
    float l[4];
    double e[3];
    double hd;
    int i, j;

    if (matrix == NULL || light == NULL || out == NULL)
        return 0;
    if (!isfinite(h) || !((double)h > 1e-20))
        return 0;
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 4; ++j)
            if (!isfinite(matrix[i][j]))
                return 0;
    for (j = 0; j < 4; ++j) {
        if (!isfinite(light[j]))
            return 0;
        l[j] = light[j];
    }

    hd = (double)h;
    for (i = 0; i < 3; ++i) {
        double rl = (double)matrix[i][0] * l[0] + (double)matrix[i][1] * l[1] + (double)matrix[i][2] * l[2];
        e[i] = rl / hd + (double)matrix[i][3] * l[3];
        if (!isfinite(e[i]) || fabs(e[i]) > (double)FLT_MAX)
            return 0;
    }

    out[0] = (float)e[0];
    out[1] = (float)e[1];
    out[2] = (float)e[2];
    out[3] = l[3];
    return 1;
}

#endif /* RSX_HOMOGENEOUS_LIGHT_H */
