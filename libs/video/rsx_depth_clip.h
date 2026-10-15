/* Ordinary RSX window-depth clipping mapped to host homogeneous depth.
 * The shader normalization and viewport range must be applied together.
 * Extended, reversed and degenerate ranges require separate hardware evidence. */
#ifndef PS3RECOMP_RSX_DEPTH_CLIP_H
#define PS3RECOMP_RSX_DEPTH_CLIP_H
#include <math.h>
typedef struct rsx_depth_clip {
    float scale, offset, min_depth, max_depth;
} rsx_depth_clip;
static inline int rsx_depth_clip_build(float n, float f, float scale_z,
                                      float offset_z, rsx_depth_clip* out)
{
    if (!out || !isfinite(n) || !isfinite(f) || !isfinite(scale_z) ||
        !isfinite(offset_z) || n < 0.0f || f > 1.0f || !(f - n > 0.000001f))
        return -1;
    rsx_depth_clip value;
    value.scale = scale_z / (f - n);
    value.offset = (offset_z - n) / (f - n);
    value.min_depth = n;
    value.max_depth = f;
    if (!isfinite(value.scale) || !isfinite(value.offset)) return -1;
    *out = value;
    return 0;
}
#endif
