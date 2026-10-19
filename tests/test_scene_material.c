#include "rsx_scene_material.h"

#include <assert.h>
#include <math.h>
#include <string.h>

static const char *source_hash =
    "84d877ddc6083545452948630259c21ca796e7985fcd5e7b5a005666e4a0d634";

static int sample(void *context, unsigned unit, float u, float v,
                  float lod, float rgba[4])
{
    const float *tex = (const float *)context;
    assert(unit == 0u);
    assert(u == 0.25f && v == 0.75f && lod == 2.0f);
    memcpy(rgba, tex, 4u * sizeof(float));
    return 1;
}

int main(void)
{
    float fp[16][4] = {{0}};
    float vertex[12][4] = {{0}};
    float tex[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    rsx_scene_material_input in = {0};
    rsx_scene_material_output out;
    char uppercase[65];
    unsigned i;
    vertex[1][0] = 0.4f;
    vertex[1][1] = 0.6f;
    vertex[1][2] = 0.8f;
    vertex[4][0] = 0.25f;
    vertex[4][1] = 0.75f;
    in.contract_id = 0;
    in.source_sha256 = source_hash;
    in.fp = (const float (*)[4])fp;
    in.fp_bytes = sizeof(fp);
    in.vertex = (const float (*)[4])vertex;
    in.vertex_bytes = sizeof(vertex);
    in.texture0_lod = 2.0f;
    in.sample = sample;
    in.sample_context = tex;
    assert(rsx_scene_material_evaluate(&in, &out) == RSX_SCENE_MATERIAL_OK);
    assert(out.valid && out.base_color_proxy_valid);
    assert(fabsf(out.base_color_proxy[0] - 0.2f) < 1e-6f);
    assert(fabsf(out.base_color_proxy[1] - 0.6f) < 1e-6f);
    assert(fabsf(out.base_color_proxy[2] - 1.2f) < 1e-6f);
    assert(!out.physical_albedo_valid && isnan(out.physical_albedo[0]));
    assert(!out.world_normal_valid && isnan(out.world_normal[0]));
    assert(!out.roughness_valid && isnan(out.roughness));
    assert(!out.light_direction_valid && isnan(out.light_direction[0]));
    memcpy(uppercase, source_hash, 65);
    for (i = 0; i < 64; ++i)
        if (uppercase[i] >= 'a' && uppercase[i] <= 'f')
            uppercase[i] = (char)(uppercase[i] - 'a' + 'A');
    in.source_sha256 = uppercase;
    assert(rsx_scene_material_evaluate(&in, &out) == RSX_SCENE_MATERIAL_OK);
    in.contract_id = 1;
    assert(rsx_scene_material_evaluate(&in, &out) == RSX_SCENE_MATERIAL_UNSUPPORTED);
    assert(!out.valid && isnan(out.base_color_proxy[0]));
    in.contract_id = 0;
    in.fp_bytes = sizeof(fp) - 1;
    assert(rsx_scene_material_evaluate(&in, &out) == RSX_SCENE_MATERIAL_INVALID);
    in.fp_bytes = sizeof(fp);
    tex[0] = NAN;
    assert(rsx_scene_material_evaluate(&in, &out) == RSX_SCENE_MATERIAL_NUMERIC_UNSUPPORTED);
    assert(!out.valid);
    return 0;
}
