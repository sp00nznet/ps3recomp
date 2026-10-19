/* Immutable draw inputs for one current-frame depth clear/write epoch.
 * Pipeline and texture handles remain caller-owned: invalidate the epoch
 * before destroying, replacing or uploading any referenced resource. */
#ifndef RSX_SCENE_EPOCH_H
#define RSX_SCENE_EPOCH_H
#include "rsx_draw_engine.h"
#include <stddef.h>
#include <stdint.h>
#define RSX_SCENE_EPOCH_MAX_DRAWS 256u
#define RSX_SCENE_EPOCH_MAX_VERTICES 262144u
#define RSX_SCENE_EPOCH_MAX_INDICES 1048575u
#define RSX_SCENE_EPOCH_MAX_BYTES (64u*1024u*1024u)
enum rsx_scene_epoch_reason {
    RSX_SCENE_EPOCH_OK, RSX_SCENE_EPOCH_NO_CLEAR, RSX_SCENE_EPOCH_STALE,
    RSX_SCENE_EPOCH_UNSUPPORTED, RSX_SCENE_EPOCH_CAPACITY,
    RSX_SCENE_EPOCH_RESOURCE_CHANGED, RSX_SCENE_EPOCH_ALLOCATION
};
typedef struct rsx_scene_draw_input {
    uint32_t pipeline, vertex_count, index_count, texture_mask;
    rsx_vertex_layout_plan layout;
    rsx_be_render_state state;
    uint32_t textures[16];
    rsx_be_sampler_desc samplers[16];
    float viewport[6];
    uint32_t scissor[4];
    const void *vertices, *vs_constants, *ps_constants;
    const uint32_t* indices; /* NULL means sequential triangles. */
    size_t vs_bytes, ps_bytes;
} rsx_scene_draw_input;
typedef struct rsx_scene_draw_snapshot {
    rsx_scene_draw_input draw; /* pointers are resolved by get(), not stored */
    size_t vertex_offset, index_offset, vs_offset, ps_offset;
} rsx_scene_draw_snapshot;
typedef struct rsx_scene_epoch {
    uint64_t generation, frame, serial;
    uint32_t depth, width, height, samples, draw_count, vertex_count, index_count;
    int armed, sealed;
    enum rsx_scene_epoch_reason reason;
    size_t used, capacity;
    unsigned char* bytes;
    rsx_scene_draw_snapshot draws[RSX_SCENE_EPOCH_MAX_DRAWS];
} rsx_scene_epoch;
/* Object must initially be zero initialized. Clear retains allocation; only a
 * full far clear arms an epoch. Caller assigns monotonically unique generation. */
void rsx_scene_epoch_clear(rsx_scene_epoch*,uint64_t generation,uint64_t frame,
    uint64_t serial,uint32_t depth,uint32_t width,uint32_t height,uint32_t samples,
    float clear_depth);
void rsx_scene_epoch_invalidate(rsx_scene_epoch*,enum rsx_scene_epoch_reason);
int rsx_scene_epoch_append(rsx_scene_epoch*,uint64_t frame,uint64_t serial,
    const rsx_scene_draw_input*);
int rsx_scene_epoch_seal(rsx_scene_epoch*,uint64_t frame,uint64_t serial);
/* get() borrows the immutable arena until clear/destroy. It does not seal. */
int rsx_scene_epoch_get(const rsx_scene_epoch*,unsigned index,rsx_scene_draw_input*);
int rsx_scene_epoch_uses_resource(const rsx_scene_epoch*,uint32_t texture);
int rsx_scene_epoch_uses_pipeline(const rsx_scene_epoch*,uint32_t pipeline);
void rsx_scene_epoch_destroy(rsx_scene_epoch*);
#endif
