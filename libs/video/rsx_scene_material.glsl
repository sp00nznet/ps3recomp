/* Captured PS contract 0. Host validates SHA256 before qualifiedSource=true.
 * Hooks: bool rsx_scene_material_load_constant(material, index, out vec4);
 *        bool rsx_scene_material_sample(material, unit, uv, lod, out vec4);
 * The source's prelighting color is a proxy, never physical albedo.
 */
#ifndef RSX_SCENE_MATERIAL_TYPE
#define RSX_SCENE_MATERIAL_TYPE uint
#endif

const int RSX_SCENE_MATERIAL_OK = 0;
const int RSX_SCENE_MATERIAL_UNSUPPORTED = -1;
const int RSX_SCENE_MATERIAL_INVALID = -2;
const int RSX_SCENE_MATERIAL_SAMPLE_UNAVAILABLE = -3;
const int RSX_SCENE_MATERIAL_NUMERIC_UNSUPPORTED = -4;

struct RsxSceneMaterialOutput {
    bool valid;
    precise vec3 baseColorProxy;
    bool baseColorProxyValid;
    precise vec3 physicalAlbedo;
    bool physicalAlbedoValid;
    precise vec3 worldNormal;
    bool worldNormalValid;
    precise float roughness;
    bool roughnessValid;
    precise vec3 lightDirection;
    bool lightDirectionValid;
};

bool rsx_scene_material_finite(float x) { return !isnan(x) && !isinf(x); }
bool rsx_scene_material_finite3(vec3 x) {
    return all(not(isnan(x))) && all(not(isinf(x)));
}

void rsx_scene_material_clear(out RsxSceneMaterialOutput result) {
    float n = uintBitsToFloat(0x7fc00000u);
    result.valid = false;
    result.baseColorProxy = vec3(n);
    result.baseColorProxyValid = false;
    result.physicalAlbedo = vec3(n);
    result.physicalAlbedoValid = false;
    result.worldNormal = vec3(n);
    result.worldNormalValid = false;
    result.roughness = n;
    result.roughnessValid = false;
    result.lightDirection = vec3(n);
    result.lightDirectionValid = false;
}

int rsx_scene_material_evaluate(RSX_SCENE_MATERIAL_TYPE material,
    bool qualifiedSource, uint contractId, const in vec4 vertex[12], float lod0,
    out RsxSceneMaterialOutput result)
{
    precise vec4 c, tex;
    precise vec3 proxy;
    rsx_scene_material_clear(result);
    if (!qualifiedSource || contractId != 0u) return RSX_SCENE_MATERIAL_UNSUPPORTED;
    /* Require the same 16 constant slots as the captured contract. Values do
     * not enter this specific prelighting expression. */
    for (int i = 0; i < 16; ++i)
        if (!rsx_scene_material_load_constant(material, i, c))
            return RSX_SCENE_MATERIAL_INVALID;
    if (!rsx_scene_material_finite3(vertex[1].rgb) ||
        !rsx_scene_material_finite(vertex[4].x) ||
        !rsx_scene_material_finite(vertex[4].y) ||
        !rsx_scene_material_finite(lod0))
        return RSX_SCENE_MATERIAL_NUMERIC_UNSUPPORTED;
    if (!rsx_scene_material_sample(material, 0, vertex[4].xy, lod0, tex))
        return RSX_SCENE_MATERIAL_SAMPLE_UNAVAILABLE;
    if (!rsx_scene_material_finite3(tex.rgb))
        return RSX_SCENE_MATERIAL_NUMERIC_UNSUPPORTED;
    proxy = 2.0 * (vertex[1].rgb * tex.rgb);
    if (!rsx_scene_material_finite3(proxy))
        return RSX_SCENE_MATERIAL_NUMERIC_UNSUPPORTED;
    result.valid = true;
    result.baseColorProxy = proxy;
    result.baseColorProxyValid = true;
    return RSX_SCENE_MATERIAL_OK;
}
