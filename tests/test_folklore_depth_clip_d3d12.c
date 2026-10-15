/* Exercise the real D3D12 constant-buffer recorder without a GPU or guest data. */
#include "../libs/video/rsx_d3d12_backend.c"
#include <assert.h>
uint8_t* vm_base;
const uint64_t function_table_count = 0;
const struct { uint64_t addr; void* func; const char* name; } function_table[1] = {{0,0,0}};
int main(void)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = graphics_pso_defaults();
    assert(pso.RasterizerState.DepthClipEnable == TRUE);
    assert(pso.pRootSignature == NULL && pso.NumRenderTargets == 0);
    rsx_state state;
    rsx_state_init(&state);
    static unsigned char constants[VP_CB_STRIDE * 3];
    s_d3d.vp_cb_mapped = constants;
    s_d3d.current_rsx_state = &state;
    state.vertex_constants[0][0] = -2.0f; /* valid guest data, never replace */
    D3D12DrawRecord first = {0}, second = {0};
    state.clip_min = 0.25f;
    state.clip_max = 0.75f;
    first.depth_clip = current_depth_clip();
    vp_record_cb(0, -1, &first);
    const float* a = (const float*)(constants + RSX_MAX_VERTEX_CONSTANTS * 16);
    assert(a[2] == 1.0f && a[6] == 0.5f);
    assert(*(const float*)constants == -2.0f);
    assert(same_depth_clip(&first));
    state.viewport_scale[2] = 0.0f; /* zero scale is a real transform */
    state.viewport_offset[2] = 0.375f;
    second.depth_clip = current_depth_clip();
    vp_record_cb(1, -1, &second);
    const float* b = (const float*)(constants + VP_CB_STRIDE + RSX_MAX_VERTEX_CONSTANTS * 16);
    assert(b[2] == 0.0f && b[6] == 0.25f);
    assert(!same_depth_clip(&first) && same_depth_clip(&second));
    assert(a[2] == 1.0f && a[6] == 0.5f); /* earlier slot remains intact */
    state.clip_min = 0.0f;
    state.clip_max = 1.0f;
    assert(!same_depth_clip(&second));
    vp_record_cb(2, -1, &first); /* a record does not consult later depth state */
    const float* c = (const float*)(constants + VP_CB_STRIDE * 2 + RSX_MAX_VERTEX_CONSTANTS * 16);
    assert(c[2] == 1.0f && c[6] == 0.5f);
    puts("D3D12 recorded depth coefficients, zero scale, merge boundaries and guest constants passed");
    return 0;
}
