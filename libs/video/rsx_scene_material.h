#ifndef RSX_SCENE_MATERIAL_H
#define RSX_SCENE_MATERIAL_H

#include <stddef.h>
#include <stdint.h>

/* Captured PS contract 0 only. The source expression is
 * base = 2 * input.col0.rgb * texture0.rgb. This is a color proxy, not
 * measured reflectance or PBR albedo. All physical properties remain absent.
 */
typedef enum rsx_scene_material_status {
    RSX_SCENE_MATERIAL_OK = 0,
    RSX_SCENE_MATERIAL_UNSUPPORTED = -1,
    RSX_SCENE_MATERIAL_INVALID = -2,
    RSX_SCENE_MATERIAL_SAMPLE_UNAVAILABLE = -3,
    RSX_SCENE_MATERIAL_NUMERIC_UNSUPPORTED = -4
} rsx_scene_material_status;

typedef int (*rsx_scene_material_sample_fn)(void *context, unsigned unit,
    float u, float v, float lod, float rgba[4]);

typedef struct rsx_scene_material_input {
    uint32_t contract_id;
    const char *source_sha256; /* exact 64 hex digit captured PS source hash */
    const float (*fp)[4];      /* c0..c15, including alpha threshold */
    size_t fp_bytes;
    const float (*vertex)[4];  /* pos,col0,col1,fog,t0..t7 */
    size_t vertex_bytes;
    float texture0_lod;
    rsx_scene_material_sample_fn sample;
    void *sample_context;
} rsx_scene_material_input;

typedef struct rsx_scene_material_output {
    int valid;
    float base_color_proxy[3]; /* exact authored prelighting expression */
    int base_color_proxy_valid;
    float physical_albedo[3];  /* NAN and invalid: source cannot establish it */
    int physical_albedo_valid;
    float world_normal[3];     /* NAN and invalid: no proven space transform */
    int world_normal_valid;
    float roughness;           /* NAN and invalid: no roughness channel */
    int roughness_valid;
    float light_direction[3]; /* NAN and invalid: LUT vectors are not lights */
    int light_direction_valid;
} rsx_scene_material_output;

int rsx_scene_material_source_supported(uint32_t contract_id, const char *sha256);
rsx_scene_material_status rsx_scene_material_evaluate(
    const rsx_scene_material_input *input, rsx_scene_material_output *output);

#endif
