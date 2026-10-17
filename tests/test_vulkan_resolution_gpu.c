/* Actual scaled raster, WPOS, scissor and guest-transfer regression. */
#define SDL_MAIN_HANDLED
#include "../libs/video/rsx_vulkan_backend.c"
#include <math.h>
uint8_t* vm_base;
static unsigned checks;
#define CHECK(v) do{++checks;if(!(v)){fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#v);return 1;}}while(0)
/* The backend reads its settings with getenv. SDL_setenv does not reach that
 * on Windows (SDL2 writes the Win32 environment block there), so set the CRT copy. */
static int set_env(const char* name, const char* value)
{
#ifdef _WIN32
    return _putenv_s(name, value) ? -1 : 0;
#else
    return setenv(name, value, 1);
#endif
}
int main(void)
{
    /* The scale is applied by the register-file draw engine, which runs with
     * guest programs on, as Folklore always does. */
    SDL_SetMainReady();CHECK(set_env("PS3RECOMP_VK_GUEST_PROGRAMS","1")==0);
    CHECK(set_env("PS3RECOMP_RENDER_SCALE","3")==0);
    CHECK(rsx_vulkan_backend_init(24,12,"Internal resolution regression")==0);
    CHECK(s_render_scale==3);
    static const char vs[]="float4 main(float4 p:ATTR0):SV_Position{return p;}";
    static const char ps[]="struct PSInput { float4 position:SV_POSITION; };\n"
        "float4 main(PSInput input):SV_Target0 {\n"
        "/* RSX shader window */\n"
        "input.position.xy=input.position.xy*float2(1.0,-1.0)+float2(0.0,4.0);\n"
        "return float4(input.position.xy/float2(8.0,4.0),0.25,1.0);}";
    rsx_vertex_layout_plan layout={0};layout.count=1;layout.stride=16;layout.attrs[0]=0;
    rsx_be_render_state state={0};state.color_mask=0x01010101u;
    state.antialias_control=0xFFFF0001u;state.depth_test=1;state.depth_write=1;state.depth_func=0x201;
    const u32 pipeline=vk_eng_pipeline_create(NULL,vs,ps,&state,&layout,16,RSX_BE_FMT_R8G8B8A8,1);
    CHECK(pipeline);
    const u32 color=vk_eng_color_target_create(NULL,RSX_BE_FMT_R8G8B8A8,8,4,NULL,0);
    const u32 depth=vk_eng_depth_target_create(NULL,8,4);
    CHECK(color&&depth&&s_eobj[color].w==24&&s_eobj[color].h==12&&s_eobj[depth].w==24);
    const float clear[4]={0,0,1,1};vk_eng_clear_color(NULL,color,clear);
    const float vertices[3][4]={{-1,-1,.25f,1},{3,-1,.25f,1},{-1,3,.25f,1}};
    vk_eng_bind_targets(NULL,&color,1,depth);vk_eng_bind_pipeline(NULL,pipeline);
    vk_eng_set_viewport(NULL,0,0,8,4,0,1);vk_eng_set_scissor(NULL,2,1,3,2);
    vk_eng_draw(NULL,RSX_TOPOLOGY_TRIANGLES,vertices,3,16,NULL,0);
    u8 physical[24*12*4];float z[24*12];
    CHECK(vk_eng_readback_rows_raw(color,0,0,24,12,physical,24*4)==0);
    CHECK(vk_eng_depth_readback_raw(depth,0,0,24,12,z,24*4)==0);
    for(u32 y=0;y<12;++y)for(u32 x=0;x<24;++x){
        const u8* pixel=physical+((size_t)y*24+x)*4;
        if(x>=6&&x<15&&y>=3&&y<9){
            const float expected_r=((float)x+.5f)/24.f*255.f;
            const float expected_g=(1.f-((float)y+.5f)/12.f)*255.f;
            CHECK(fabsf(pixel[0]-expected_r)<1.1f&&fabsf(pixel[1]-expected_g)<1.1f);
            CHECK(pixel[2]==64&&pixel[3]==255&&fabsf(z[y*24+x]-.25f)<1e-6f);
        }else CHECK(pixel[0]==0&&pixel[1]==0&&pixel[2]==255&&pixel[3]==255&&z[y*24+x]==1.f);
    }
    u8 guest[8*4*4];vk_eng_readback(NULL,color,0,0,8,4,guest,8*4);
    for(u32 y=0;y<4;++y)for(u32 x=0;x<8;++x)
        CHECK(!memcmp(guest+((size_t)y*8+x)*4,physical+((size_t)(y*3+1)*24+x*3+1)*4,4));
    const u8 patch[8]={31,61,91,255,111,141,171,255};
    vk_eng_color_target_write(NULL,color,1,1,2,1,patch,8);
    vk_eng_readback(NULL,color,1,1,2,1,guest,8);CHECK(!memcmp(guest,patch,8));
    for(u32 samples=2;samples<=4;samples+=2){
        const u32 c=vk_eng_color_multisample(NULL,RSX_BE_FMT_R8G8B8A8,4,3,samples);
        const u32 d=vk_eng_depth_multisample(NULL,4,3,samples);CHECK(c&&d);
        CHECK(s_eobj[c].w==12&&s_eobj[c].h==9&&s_eobj[c].samples==samples);
        vk_eng_clear_color(NULL,c,clear);vk_eng_clear_depth_stencil(NULL,d,RSX_BE_CLEAR_DEPTH,.75f,0);
        vk_eng_color_target_write(NULL,c,1,1,2,1,patch,8);
        const u32 w=8,h=3*(samples==4?2:1);u8 pixels[8*6*4];float depths[8*6];
        vk_eng_readback(NULL,c,0,0,w,h,pixels,w*4);CHECK(vk_eng_depth_readback(NULL,d,0,0,w,h,depths,w*4)==0);
        for(u32 y=0;y<h;++y)for(u32 x=0;x<w;++x){
            const u8* pixel=pixels+((size_t)y*w+x)*4;
            if(y==1&&x>=1&&x<3)CHECK(!memcmp(pixel,patch+(x-1)*4,4));
            else CHECK(pixel[0]==0&&pixel[1]==0&&pixel[2]==255&&pixel[3]==255);
            CHECK(depths[y*w+x]==.75f);
        }
        vk_eng_release(NULL,c);vk_eng_release(NULL,d);
    }
    /* Physical 4K allocation, separate from full-game visual acceptance. */
    const u32 four_k=vk_eng_color_target_create(NULL,RSX_BE_FMT_R8G8B8A8,1280,720,NULL,0);
    CHECK(four_k&&s_eobj[four_k].w==3840&&s_eobj[four_k].h==2160);
    vk_eng_release(NULL,four_k);
    rsx_vulkan_backend_shutdown();
    printf("Vulkan internal resolution GPU: %u checks passed\n",checks);return 0;
}
