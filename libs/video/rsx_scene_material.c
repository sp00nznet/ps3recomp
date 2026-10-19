#include "rsx_scene_material.h"

#include <math.h>

#ifdef __FAST_MATH__
#error "rsx_scene_material requires finite checks; disable fast-math"
#endif

static int hash_equal(const char *a, const char *b)
{
    unsigned i;
    if (!a) return 0;
    for (i = 0; i < 64; ++i) {
        unsigned char c = (unsigned char)a[i];
        if (!c) return 0;
        if (c >= 'A' && c <= 'F') c = (unsigned char)(c + ('a' - 'A'));
        if (c != (unsigned char)b[i]) return 0;
    }
    return a[64] == 0;
}

int rsx_scene_material_source_supported(uint32_t id, const char *hash)
{
    return id == 0u &&
        (hash_equal(hash, "84d877ddc6083545452948630259c21ca796e7985fcd5e7b5a005666e4a0d634") ||
         hash_equal(hash, "4b43ead21c9dce22ed005919077251929e3c5efb9890013745ece3afb0cfd4b7"));
}

static void invalidate(rsx_scene_material_output *out)
{
    unsigned i;
    out->valid = out->base_color_proxy_valid = 0;
    out->physical_albedo_valid = out->world_normal_valid = 0;
    out->roughness_valid = out->light_direction_valid = 0;
    out->roughness = NAN;
    for (i = 0; i < 3; ++i) {
        out->base_color_proxy[i] = NAN;
        out->physical_albedo[i] = NAN;
        out->world_normal[i] = NAN;
        out->light_direction[i] = NAN;
    }
}

rsx_scene_material_status rsx_scene_material_evaluate(
    const rsx_scene_material_input *in, rsx_scene_material_output *out)
{
    float tex[4], proxy[3];
    unsigned i;
    if (!out) return RSX_SCENE_MATERIAL_INVALID;
    invalidate(out);
    if (!in) return RSX_SCENE_MATERIAL_INVALID;
    if (!rsx_scene_material_source_supported(in->contract_id, in->source_sha256))
        return RSX_SCENE_MATERIAL_UNSUPPORTED;
    if (!in->fp || in->fp_bytes < 16u * 4u * sizeof(float) ||
        !in->vertex || in->vertex_bytes < 12u * 4u * sizeof(float) || !in->sample)
        return RSX_SCENE_MATERIAL_INVALID;
    if (!isfinite(in->texture0_lod) || !isfinite(in->vertex[4][0]) ||
        !isfinite(in->vertex[4][1])) return RSX_SCENE_MATERIAL_NUMERIC_UNSUPPORTED;
    for (i = 0; i < 3; ++i)
        if (!isfinite(in->vertex[1][i])) return RSX_SCENE_MATERIAL_NUMERIC_UNSUPPORTED;
    if (!in->sample(in->sample_context, 0u, in->vertex[4][0],
                    in->vertex[4][1], in->texture0_lod, tex))
        return RSX_SCENE_MATERIAL_SAMPLE_UNAVAILABLE;
    for (i = 0; i < 3; ++i) {
        if (!isfinite(tex[i])) return RSX_SCENE_MATERIAL_NUMERIC_UNSUPPORTED;
        proxy[i] = 2.0f * (in->vertex[1][i] * tex[i]);
        if (!isfinite(proxy[i])) return RSX_SCENE_MATERIAL_NUMERIC_UNSUPPORTED;
    }
    for (i = 0; i < 3; ++i) out->base_color_proxy[i] = proxy[i];
    out->base_color_proxy_valid = out->valid = 1;
    return RSX_SCENE_MATERIAL_OK;
}
