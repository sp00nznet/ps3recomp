#ifndef RSX_SCENE_RGB_H
#define RSX_SCENE_RGB_H

#include <stddef.h>
#include <stdint.h>

/* Authored captured-shader color only: neither albedo nor physical PBR lighting. */
#define RSX_SCENE_RGB_CONTRACT_ID 0u
#define RSX_SCENE_RGB_FP_FLOAT4_COUNT 16u
#define RSX_SCENE_RGB_FP_MIN_BYTES (RSX_SCENE_RGB_FP_FLOAT4_COUNT * 4u * sizeof(float))
#define RSX_SCENE_RGB_VERTEX_FLOAT4_COUNT 12u
#define RSX_SCENE_RGB_TEXTURE_MASK 0x7u /* units 0, 1 and 2 */

typedef enum rsx_scene_rgb_status {
    RSX_SCENE_RGB_OK = 0,
    RSX_SCENE_RGB_UNSUPPORTED = -1,
    RSX_SCENE_RGB_INVALID = -2,
    RSX_SCENE_RGB_SAMPLE_UNAVAILABLE = -3,
    RSX_SCENE_RGB_NUMERIC_UNSUPPORTED = -4
} rsx_scene_rgb_status;

/* The callback owns exact texture, sampler and explicit-LOD semantics. */
typedef int (*rsx_scene_rgb_sample_fn)(void *context, unsigned unit, float u,
                                       float v, float lod, float out_rgba[4]);

typedef struct rsx_scene_rgb_input {
    uint32_t contract_id;
    const char *source_sha256; /* exactly 64 hex digits, case insensitive */
    const float (*fp)[4];      /* c0..c15; c15.x is alpha threshold */
    size_t fp_bytes;
    const float (*vertex)[4];  /* pos,col0,col1,fog,t0..t7 */
    size_t vertex_bytes;
    float lod[3];              /* explicit LOD for units 0, 1 and 2 */
    rsx_scene_rgb_sample_fn sample;
    void *sample_context;
} rsx_scene_rgb_input;

/* Exact authored source expressions, not physical lights, emission or albedo. */
typedef struct rsx_scene_rgb_terms {
    float lut[2][3];
    float constant_base[3];
    float vertex_weighted[2][3];
    float vertex_combined[3]; /* original base*(weight0*color0+weight1*color1) */
    float prefloor[3];
    float floor_value;
    float floor_residual[3];
    float additive[3];
    float overlay[3];
    float fog_weights[2];
    float fog_colors[2][3];
    float final_scalar;
} rsx_scene_rgb_terms;

typedef struct rsx_scene_rgb_output {
    int valid;                 /* false on every non-OK status; never use color then */
    float authored_rgb[3];    /* authored game-lighting output */
    float base_rgb[3];        /* vertex-color/texture proxy, NOT albedo */
    float raw_alpha;
    float clamped_coverage;
    int keep;                  /* raw_alpha > c15.x, strict */
    rsx_scene_rgb_terms source;
} rsx_scene_rgb_output;

int rsx_scene_rgb_source_supported(uint32_t contract_id, const char *sha256);
rsx_scene_rgb_status rsx_scene_rgb_evaluate(const rsx_scene_rgb_input *input,
                                            rsx_scene_rgb_output *output);
/* Visibility modulates only the two LUT contributions. Indirect is added to
 * the reconstructed prefloor sum; source floor, overlays and fog follow.
 * (1,1), zero indirect preserves original operation order and authored RGB.
 * Failure writes NAN to all three result components. Input is never changed. */
rsx_scene_rgb_status rsx_scene_rgb_compose(const rsx_scene_rgb_output *source,
    const float visibility[2], const float indirect[3], float result_rgb[3]);

#endif
