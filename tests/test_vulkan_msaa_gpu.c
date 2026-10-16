/* Actual Vulkan attachments, raster samples and expanded readbacks.
 * Synthetic geometry only; no game/firmware bytes. */
#define SDL_MAIN_HANDLED
#include "../libs/video/rsx_vulkan_backend.c"
#include <assert.h>
#include <math.h>
uint8_t* vm_base;
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while(0)

int main(void)
{
    SDL_SetMainReady();
    CHECK(rsx_vulkan_backend_init(8,4,"MSAA sample regression")==0);
    static const char vs[]="float4 main(float4 p:ATTR0):SV_Position{return p;}";
    static const char fs[]="float4 main():SV_Target0{return float4(1,0,0,1);}";
    rsx_vertex_layout_plan layout={0};layout.count=1;layout.stride=16;layout.attrs[0]=0;
    rsx_be_render_state state={0};state.color_mask=0x01010101u;
    state.depth_test=1;state.depth_write=1;state.depth_func=0x201;
    u32 pipeline=vk_eng_pipeline_create(NULL,vs,fs,&state,&layout,16,RSX_BE_FMT_R8G8B8A8,1);
    CHECK(pipeline!=0);
    const float vertices[6][4]={{-1,-1,.25f,1},{.125f,-1,.25f,1},{.125f,1,.25f,1},
                               {-1,-1,.25f,1},{.125f,1,.25f,1},{-1,1,.25f,1}};
    const u32 counts[]={2,4,2,1};
    for(unsigned round=0;round<4;++round) {
        u32 count=counts[round];
        u32 color=count>1?vk_eng_color_multisample(NULL,RSX_BE_FMT_R8G8B8A8,8,4,count):vk_eng_color_target_create(NULL,RSX_BE_FMT_R8G8B8A8,8,4,NULL,0);
        u32 depth=count>1?vk_eng_depth_multisample(NULL,8,4,count):vk_eng_depth_target_create(NULL,8,4);
        CHECK(color&&depth);
        const float blue[4]={0,0,1,1};vk_eng_clear_color(NULL,color,blue);
        vk_eng_bind_targets(NULL,&color,1,depth);vk_eng_bind_pipeline(NULL,pipeline);
        vk_eng_set_viewport(NULL,0,0,8,4,0,1);vk_eng_set_scissor(NULL,0,0,8,4);
        vk_eng_draw(NULL,RSX_TOPOLOGY_TRIANGLES,vertices,6,16,NULL,0);
        unsigned sx=count>1?2:1,sy=count>2?2:1,w=8*sx,h=4*sy;
        uint8_t pixels[16*8*4];float depths[16*8];
        memset(pixels,0x55,sizeof pixels);memset(depths,0,sizeof depths);
        vk_eng_readback(NULL,color,0,0,w,h,pixels,w*4);
        CHECK(vk_eng_depth_readback(NULL,depth,0,0,w,h,depths,w*4)==0);
        for(unsigned y=0;y<4;++y) for(unsigned x=0;x<8;++x) {
            unsigned red=0,blue_count=0;
            for(unsigned dy=0;dy<sy;++dy) for(unsigned dx=0;dx<sx;++dx) {
                unsigned i=(y*sy+dy)*w+x*sx+dx;uint8_t* p=pixels+i*4;
                CHECK(p[1]==0&&p[3]==255);
                CHECK((p[0]==255&&p[2]==0)||(p[0]==0&&p[2]==255));
                if(p[0]==255) { ++red;CHECK(fabsf(depths[i]-.25f)<1e-5f); }
                else { ++blue_count;CHECK(fabsf(depths[i]-1.f)<1e-5f); }
            }
            if(x<4) CHECK(red==count);
            if(x>4) CHECK(blue_count==count);
            if(x==4&&count>1) CHECK(red==count/2&&blue_count==count/2);
        }
        /* A clear after expansion must invalidate cached samples. */
        const float green[4]={0,1,0,1};vk_eng_clear_color(NULL,color,green);
        vk_eng_clear_depth_stencil(NULL,depth,RSX_BE_CLEAR_DEPTH,.75f,0);
        vk_eng_readback(NULL,color,0,0,w,h,pixels,w*4);
        CHECK(vk_eng_depth_readback(NULL,depth,0,0,w,h,depths,w*4)==0);
        for(unsigned i=0;i<w*h;++i) { CHECK(pixels[i*4]==0&&pixels[i*4+1]==255&&pixels[i*4+2]==0);CHECK(fabsf(depths[i]-.75f)<1e-5f); }
        vk_eng_release(NULL,color);vk_eng_release(NULL,depth);
        printf("MSAA %u-sample color/depth round passed\n",count);
    }
    vk_eng_pipeline_release(NULL,pipeline);rsx_vulkan_backend_shutdown();
    printf("MSAA GPU: %u checks passed\n",checks);return 0;
}
