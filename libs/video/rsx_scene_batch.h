/* Default-off, single-owner, current-batch generated guest VS replay.
 * No API here submits, waits, reads back, enables device features, or captures
 * game state automatically. Include rsx_vulkan_scene_batch.inc after scene.inc.
 * All calls are serialized on the backend's controller thread.
 */
#ifndef RSX_SCENE_BATCH_H
#define RSX_SCENE_BATCH_H
#include "rsx_scene_bridge.h"
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define RSX_SCENE_BATCH_MAX_DRAWS 256u
#define RSX_SCENE_BATCH_MAX_VERTICES 262144u
#define RSX_SCENE_BATCH_CONSTANT_BYTES 8224u
#define RSX_SCENE_BATCH_MAX_INPUT_BYTES (64u*1024u*1024u)
#define RSX_SCENE_BATCH_MAX_SOURCE_BYTES (1024u*1024u)
enum rsx_scene_batch_result {
    RSX_SCENE_BATCH_OK=0, RSX_SCENE_BATCH_UNAVAILABLE=-1,
    RSX_SCENE_BATCH_INVALID=-2, RSX_SCENE_BATCH_CAPACITY=-3,
    RSX_SCENE_BATCH_MEMORY=-4, RSX_SCENE_BATCH_BUSY=-5,
    RSX_SCENE_BATCH_STALE=-6, RSX_SCENE_BATCH_GPU=-7,
    RSX_SCENE_BATCH_COMPILE=-8
};
enum rsx_scene_batch_gpu_flags {
    RSX_SCENE_BATCH_INVALID_POSITION=1u,
    /* Informational only. Unused varying NaNs retain their original bits.
     * Material users must validate only their actual dependency channels. */
    RSX_SCENE_BATCH_NONFINITE_VARYING=2u
};
typedef struct rsx_scene_batch_input {
    const char *generated_vs;       /* complete generated VS; bounded NUL string */
    rsx_vertex_layout_plan layout;  /* compact float4 attributes */
    const void *vertices;
    size_t vertex_bytes;            /* exactly vertex_count * layout.stride */
    uint32_t vertex_count;
    const void *constants;          /* immutable snapshot copied by append */
    size_t constant_bytes;          /* exactly 514 float4 values */
} rsx_scene_batch_input;
typedef struct rsx_scene_batch_output {
    uint64_t token,device_generation;
    VkBuffer vertices;             /* contiguous rsx_scene_vertex_output[vertex_count] */
    VkDeviceSize vertex_bytes;
    VkBuffer validity;             /* one uint32 GPU flag, zero initially */
    VkDeviceAddress vertex_address,validity_address; /* zero unless BDA opted in */
    uint32_t vertex_count,draw_count;
} rsx_scene_batch_output;
/* Pure CPU source generation, reused from the diagnostic's validated wrapper.
 * Dynamic b1 uniform count/bases mean one pipeline handles all vertex counts.
 * Nonfinite positions become finite zero; varying values/bits are untouched.
 * The caller frees the returned string. NULL means unsupported/malformed input. */
char *rsx_scene_batch_compute_source(const char *vs,const rsx_vertex_layout_plan *layout);

/* Begin one batch, creating resources only on explicit first use. generation
 * identifies this VkDevice lifetime. bda_enabled must describe ACTUALLY enabled
 * device features, not physical support. A generation/BDA change needs shutdown.
 * Cache is bounded to 256 exact source/layout keys and 64MiB key text. No cache
 * eviction or cross-frame transformed-vertex reuse occurs. */
int rsx_vulkan_scene_batch_begin(uint64_t device_generation,int bda_enabled,uint64_t *token);
/* Copies source (on cache miss), compact bytes and all constants before return.
 * out_vertex_base is this draw's first vertex in the contiguous result. Any
 * append rejection poisons the entire collecting batch; record then fails. */
int rsx_vulkan_scene_batch_append(uint64_t token,const rsx_scene_batch_input *input,uint32_t *out_vertex_base);
/* Caller supplies its already-recording primary command buffer OUTSIDE a render
 * pass and an UNSIGNALED fence for its next queue submission. Module records
 * upload visibility, zero flag, all dispatches and compute->compute buffer
 * visibility, but no submit/wait/readback. No command-buffer reset/begin/end.
 * Compute binding state is changed; graphics binding/dynamic state is preserved.
 * Caller must retain all handles until retirement and add its gather->AS barrier.
 * Do not use returned buffers after reset/shutdown, even when storage is reused. */
int rsx_vulkan_scene_batch_record(uint64_t token,VkCommandBuffer command,VkFence completion_fence,rsx_scene_batch_output *output);
/* Immediately after successful submission using the supplied fence. Do not
 * reset/reuse/destroy that fence until reset/shutdown returns success. The
 * module cannot infer whether an unmarked command buffer was submitted. */
int rsx_vulkan_scene_batch_mark_submitted(uint64_t token);
/* For an unsubmitted recorded batch ONLY, after caller has invalidated/reset
 * every command buffer containing the recording. Never cancel submitted work. */
int rsx_vulkan_scene_batch_discard_recording(uint64_t token);
/* Polls owning fence once if submitted. BUSY preserves every resource. No wait.
 * Collecting/rejected batches reset directly; pipelines remain cached. */
int rsx_vulkan_scene_batch_reset(uint64_t token);
/* Before backend device destruction. BUSY for recorded/unretired work. Must be
 * called with the original s_vk.device still alive. Does not flush raster work. */
int rsx_vulkan_scene_batch_shutdown(void);
#ifdef __cplusplus
}
#endif
#endif
