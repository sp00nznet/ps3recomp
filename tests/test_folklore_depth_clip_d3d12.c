/* Exercise the real D3D12 constant-buffer recorder without a GPU or guest data. */
#include "../libs/video/rsx_d3d12_backend.c"
#include <assert.h>
uint8_t* vm_base;
const uint64_t function_table_count = 0;
const struct { uint64_t addr; void* func; const char* name; } function_table[1] = {{0,0,0}};
int main(int argc,char** argv)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = graphics_pso_defaults();
    assert(pso.RasterizerState.DepthClipEnable == TRUE);
    assert(pso.pRootSignature == NULL && pso.NumRenderTargets == 0);
    rsx_state state;
    rsx_state_init(&state);
    state.surface_format=8u|(1u<<8);
    if(argc==2) {
        s_d3d.current_rsx_state=&state;
        state.surface_clip_w=state.surface_clip_h=16;
        state.surface_color_pitch[0]=64;
        state.surface_color_offset[0]=0x8000;
        state.surface_zeta_offset=0x8000;
        state.surface_zeta_pitch=64;
        if(!strcmp(argv[1],"layout"))state.surface_format=8u|(2u<<8);
        else if(!strcmp(argv[1],"origin"))state.surface_clip_x=1;
        else if(!strcmp(argv[1],"msaa"))state.surface_format|=1u<<12;
        else {
            s_guest_depth_count=1;
            s_guest_depths[0]=(GuestDepth){0};
            s_guest_depths[0].offset=0x8000;s_guest_depths[0].pitch=64;
            s_guest_depths[0].width=s_guest_depths[0].height=16;s_guest_depths[0].bytes=4;
            if(!strcmp(argv[1],"color-alias"))state.color_target=1;
            else if(!strcmp(argv[1],"color-nonoverlap")) {
                state.color_target=1;state.surface_color_offset[0]=0x4000;
                D3D12DrawRecord dr={0};snapshot_draw_depth(&dr);return 0;
            } else if(!strncmp(argv[1],"texture-",8)) {
                D3D12DrawRecord dr={0};dr.tex[0].set=1;dr.tex[0].location=1;
                dr.tex[0].raw=0x8080;dr.tex[0].fmt=0x85;
                dr.tex[0].w=dr.tex[0].h=16;dr.tex[0].pitch=64;dr.tex[0].mips=1;
                if(!strcmp(argv[1],"texture-nonoverlap"))dr.tex[0].raw=0x4000;
                if(!strcmp(argv[1],"texture-main"))dr.tex[0].location=2;
                if(!strcmp(argv[1],"texture-overflow")) {
                    dr.tex[0].fmt=0xA5;dr.tex[0].w=dr.tex[0].h=8192;dr.tex[0].pitch=0x80000;
                }
                if(!strcmp(argv[1],"texture-pitch-overflow")) {
                    dr.tex[0].fmt=0xA5;dr.tex[0].w=dr.tex[0].h=4096;dr.tex[0].pitch=0xFFFFF;
                }
                if(!strcmp(argv[1],"texture-range"))dr.tex[0].raw=256u*1024u*1024u-512;
                if(!strcmp(argv[1],"texture-cube-range")) {
                    dr.tex[0].raw=256u*1024u*1024u-4096;dr.tex[0].cube=1;
                }
                guard_texture_depth_overlap(&dr,0);
                return (!strcmp(argv[1],"texture-nonoverlap") || !strcmp(argv[1],"texture-main"))?0:99;
            } else if(!strcmp(argv[1],"depth-alias")) {
                s_guest_depth_count=0;s_guest_color_count=1;
                s_guest_color_ranges[0]=(GuestColorRange){0x8000,0x8400};
                state.surface_format|=2u<<5;guest_depth_get(&state);return 99;
            } else return 2;
        }
        D3D12DrawRecord dr={0};snapshot_draw_depth(&dr);return 99;
    }
    static unsigned char constants[VP_CB_STRIDE * 3];
    s_d3d.vp_cb_mapped = constants;
    s_d3d.current_rsx_state = &state;
    state.vertex_constants[0][0] = -2.0f; /* valid guest data, never replace */
    D3D12DrawRecord first = {0}, second = {0};
    state.clip_min = 0.25f;
    state.clip_max = 0.75f;
    first.depth_clip = current_depth_clip();
    snapshot_draw_depth(&first);
    vp_record_cb(0, -1, &first);
    const float* a = (const float*)(constants + RSX_MAX_VERTEX_CONSTANTS * 16);
    assert(a[2] == 1.0f && a[6] == 0.5f);
    assert(*(const float*)constants == -2.0f);
    assert(same_depth_clip(&first));
    state.viewport_scale[2] = 0.0f; /* zero scale is a real transform */
    state.viewport_offset[2] = 0.375f;
    second.depth_clip = current_depth_clip();
    snapshot_draw_depth(&second);
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
