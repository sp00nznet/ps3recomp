#ifndef RSX_TEMPORAL_GEOMETRY_H
#define RSX_TEMPORAL_GEOMETRY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Host-side clip history. One owning render thread; no internal synchronization. */
typedef struct rsx_tg_history rsx_tg_history;

typedef struct rsx_tg_clip {
    float x, y, z, w;
} rsx_tg_clip;

typedef struct rsx_tg_motion {
    float x, y; /* Previous unjittered pixel minus current unjittered pixel. */
} rsx_tg_motion;

typedef enum rsx_tg_result {
    RSX_TG_OK = 0,
    RSX_TG_INVALID_ARGUMENT,
    RSX_TG_OUT_OF_MEMORY,
    RSX_TG_BAD_STATE,
    RSX_TG_DUPLICATE_ID,
    RSX_TG_CAPACITY,
    RSX_TG_UNAVAILABLE
} rsx_tg_result;

typedef enum rsx_tg_reason {
    RSX_TG_VALID = 0,
    RSX_TG_NO_PREVIOUS_FRAME,
    RSX_TG_FRAME_DISCONTINUITY,
    RSX_TG_EXTENT_CHANGED,
    RSX_TG_CAMERA_CUT,
    RSX_TG_HISTORY_FAILED,
    RSX_TG_MISSING_DRAW,
    RSX_TG_TOPOLOGY_CHANGED
} rsx_tg_reason;

/* max_vertices is the total clip capacity per frame, not per draw. Both
 * frame buffers are allocated at creation. Limits are checked for overflow. */
rsx_tg_result rsx_tg_create(rsx_tg_history **out, size_t max_draws,
                           size_t max_vertices);
void rsx_tg_destroy(rsx_tg_history *history); /* NULL is allowed. */
void rsx_tg_reset(rsx_tg_history *history);

/* frame_id must advance by one for history reuse. A cut or discontinuity
 * starts a new history epoch. Jitter is in screen pixels: +x right, +y down.
 * The submitted clip coordinates already contain that frame's jitter. */
rsx_tg_result rsx_tg_begin_frame(rsx_tg_history *history, uint64_t frame_id,
                                 uint32_t width, uint32_t height,
                                 float jitter_x_pixels, float jitter_y_pixels,
                                 int camera_cut);

/* Submit final post-skinning, post-vertex-program clip float4 values. draw_id
 * must be stable and nonzero; topology_signature must identify primitive and
 * vertex correspondence. A duplicate or failed append poisons this frame,
 * including history queries, until another begin_frame/reset. Finite vertices
 * outside the clip volume are retained; clipping belongs to the consumer and
 * must not poison history for an otherwise valid mesh. */
rsx_tg_result rsx_tg_append_draw(rsx_tg_history *history, uint64_t draw_id,
                                 uint64_t topology_signature,
                                 const rsx_tg_clip *clips, size_t vertex_count);

typedef struct rsx_tg_match {
    const rsx_tg_clip *current;
    const rsx_tg_clip *previous;
    size_t vertex_count;
    uint32_t width, height;
    float current_jitter_x, current_jitter_y;
    float previous_jitter_x, previous_jitter_y;
    rsx_tg_reason reason;
} rsx_tg_match;

/* Query after append. On unavailable history only reason is written.
 * Clip pointers are borrowed and expire on the next mutating history call. */
rsx_tg_result rsx_tg_match_draw(const rsx_tg_history *history,
                                uint64_t draw_id, rsx_tg_match *out);

/* Seal a complete frame. A failed/incomplete frame cannot become history. */
rsx_tg_result rsx_tg_end_frame(rsx_tg_history *history);

/* Pure helper. bary is CURRENT raster screen barycentric order, with values
 * in [0,1] to within 1e-5 and sum within 1e-5 of one. Perspective weights
 * are derived from CURRENT w and applied to BOTH clip triangles. Each clip
 * must have finite values, w > 0, and depth 0 <= z <= w. Neither visibility,
 * disocclusion nor clipping is inferred. Output is unchanged on failure. */
rsx_tg_result rsx_tg_triangle_motion(const rsx_tg_clip current[3],
                                     const rsx_tg_clip previous[3],
                                     const float bary[3],
                                     uint32_t width, uint32_t height,
                                     float current_jitter_x,
                                     float current_jitter_y,
                                     float previous_jitter_x,
                                     float previous_jitter_y,
                                     rsx_tg_motion *out);

#ifdef __cplusplus
}
#endif
#endif
