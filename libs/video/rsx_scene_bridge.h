/* Experimental geometry bridge. This is exact vertex-program replay, not a
 * ray tracer, a material decoder or a complete persistent world scene. */
#ifndef RSX_SCENE_BRIDGE_H
#define RSX_SCENE_BRIDGE_H
#include "rsx_vertex_compact.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Build a compute wrapper around the generated VS. Returns allocated source
 * (caller frees), or NULL for unsupported/malformed/oversized input. Textured
 * vertex programs are deliberately rejected until their resources are bound. */
char* rsx_scene_compute_source(const char* vs, const rsx_vertex_layout_plan* layout,
                               unsigned vertex_count);
/* Exact generated VSOutput field order, fourteen float4 values per vertex.
 * pos is pre-Vulkan-Y-inversion clip space, including the guest viewport
 * epilogue. Varyings retain their shader output bits, including unused NaNs. */
typedef struct rsx_scene_vertex_output {
    float pos[4], col0[4], col1[4], fog[4];
    float t0[4], t1[4], t2[4], t3[4], t4[4], t5[4], t6[4], t7[4], t8[4], t9[4];
} rsx_scene_vertex_output;
/* Same bounds/resource restrictions as rsx_scene_compute_source. The input
 * shader must expose the complete generated VSOutput fields above. The
 * compute output is fourteen consecutive float4 values per vertex. */
char* rsx_scene_compute_outputs_source(const char* vs, const rsx_vertex_layout_plan* layout,
                                       unsigned vertex_count);
/* Convert pre-Vulkan-Y-flip clip output to view-space XYZ using the qualified
 * final positive-view-Z projection (including the viewport epilogue).
 * The caller must establish matching depth convention and projection.
 * Returns 0 and leaves output unchanged for nonfinite/singular input. */
int rsx_scene_clip_to_view(const float clip[4], const float projection[16], float xyz[3]);
#ifdef __cplusplus
}
#endif
#endif
