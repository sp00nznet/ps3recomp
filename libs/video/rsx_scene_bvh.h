/* CPU triangle-ray reference only. No scene-space or material inference. */
#ifndef RSX_SCENE_BVH_H
#define RSX_SCENE_BVH_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct rsx_scene_bvh rsx_scene_bvh;
typedef struct rsx_scene_bvh_hit {
    size_t triangle_index;
    float t, u, v;
    float normal[3];
} rsx_scene_bvh_hit;
/* Copies triangle_count groups of nine finite XYZ floats. Empty input is valid;
 * NULL is allowed only for empty input. NULL result means invalid input or OOM.
 * At most 1,000,000 triangles. Degenerate triangles are retained but never hit. */
rsx_scene_bvh* rsx_scene_bvh_create(const float* triangles, size_t triangle_count);
void rsx_scene_bvh_destroy(rsx_scene_bvh* bvh);
/* Double-sided nearest intersection in the inclusive interval [tmin,tmax].
 * Direction need not be normalized but must be finite and nonzero. Bounds must
 * be finite and ordered. Equal-t hits choose the lowest original input index.
 * Normal follows input winding, not ray orientation. Invalid input returns 0;
 * output is unchanged on invalid input or miss. Immutable BVHs allow concurrent
 * queries with separate hit outputs. The input buffer must contain 9*count floats. */
int rsx_scene_bvh_intersect(const rsx_scene_bvh* bvh, const float origin[3],
                            const float direction[3], float tmin, float tmax,
                            rsx_scene_bvh_hit* hit);
#ifdef __cplusplus
}
#endif
#endif
