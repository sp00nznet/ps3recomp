/* Run the shared synthetic FIFO/pixel oracle through the real D3D12 backend. */
#include "../libs/video/rsx_d3d12_backend.c"
#include <assert.h>
const uint64_t function_table_count = 0;
const struct { uint64_t addr; void* func; const char* name; } function_table[1] = {{0,0,0}};
static u32 fixture_draws;
static int fixture_init(u32 width,u32 height,const char* title)
{
    int result=rsx_d3d12_backend_init(width,height,title);
    if(!result) ShowWindow(s_d3d.hwnd,SW_HIDE);
    return result;
}
static void fixture_present(void)
{
    fixture_draws=0;
    for(u32 i=0;i<s_d3d.draw_count;++i) if(!s_d3d.draws[i].is_clear) ++fixture_draws;
    u64 before=rsx_d3d12_backend_successful_presents();
    const int dumps_before=s_d3d.dump_frames_left;
    assert(dumps_before>0 && s_d3d.dump_skip_left==0);
    rsx_d3d12_backend_present();
    assert(rsx_d3d12_backend_successful_presents()==before+1);
    assert(s_d3d.dump_frames_left==dumps_before-1); /* a fresh copy and GPU wait */
    assert(s_d3d.readback_buf);
    void* mapped=NULL;
    D3D12_RANGE read={0,(SIZE_T)s_d3d.readback_pitch*s_d3d.height};
    assert(SUCCEEDED(s_d3d.readback_buf->lpVtbl->Map(s_d3d.readback_buf,0,&read,&mapped)) && mapped);
    const char* path=getenv("PS3RECOMP_VK_DUMP");assert(path);
    FILE* file=fopen(path,"wb");assert(file);
    fprintf(file,"P6\n%u %u\n255\n",s_d3d.width,s_d3d.height);
    u8* row=malloc(s_d3d.width*3);assert(row);
    for(u32 y=0;y<s_d3d.height;++y) {
        const u8* source=(const u8*)mapped+(size_t)y*s_d3d.readback_pitch;
        for(u32 x=0;x<s_d3d.width;++x) memcpy(row+x*3,source+x*4,3);
        assert(fwrite(row,3,s_d3d.width,file)==s_d3d.width);
    }
    free(row);assert(fclose(file)==0);
    D3D12_RANGE written={0,0};s_d3d.readback_buf->lpVtbl->Unmap(s_d3d.readback_buf,0,&written);
}
static u32 fixture_color(void) { return 0; }
static u32 fixture_guest_draws(void) { return fixture_draws; }
#define PS3RECOMP_HOST_FIXTURE_BACKEND 1
#define HOST_BACKEND_NAME "D3D12"
#define HOST_BACKEND_GUEST_SHADERS 1
#define host_backend_init fixture_init
#define host_backend_shutdown rsx_d3d12_backend_shutdown
#define host_backend_pump rsx_d3d12_backend_pump_messages
#define host_backend_present fixture_present
#define host_backend_color fixture_color
#define host_backend_center fixture_color
#define host_backend_guest_draws fixture_guest_draws
#include "test_folklore_depth_clip_gpu.c"
