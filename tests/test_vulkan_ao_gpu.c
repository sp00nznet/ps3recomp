/* Synthetic GPU AO transport checks. No game or firmware data. */
#define SDL_MAIN_HANDLED
#include "../libs/video/rsx_vulkan_backend.c"
#include <math.h>
uint8_t* vm_base;
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while(0)

static u32 image(VkFormat fmt,u32 w,u32 h,u32 bpp,const void* data)
{
    u32 id=vk_eng_alloc();if(!id)return 0;
    vk_eng_obj* o=&s_eobj[id];
    if(vk_create_image(&o->im,fmt,w,h,VK_IMAGE_USAGE_SAMPLED_BIT|
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|
        VK_IMAGE_USAGE_TRANSFER_DST_BIT,VK_IMAGE_ASPECT_COLOR_BIT))return 0;
    o->kind=VK_ENG_TEXTURE;o->fmt=fmt;o->w=w;o->h=h;o->bpp=bpp;
    o->samples=o->scale=o->grid_x=o->grid_y=1;
    if(vk_begin())return 0;
    vk_barrier_image(o->im.img,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED);
    if(vk_submit_and_wait())return 0;
    if(data&&vk_eng_upload_rows(o->im.img,0,0,w,h,w*bpp,h,data,w*bpp))return 0;
    return id;
}

static int resolve(void)
{
    for(unsigned round=0;round<6;++round) {
        unsigned gx=round%3?2:1,gy=round%3==2?2:1;
        unsigned w=round<3?7:9,h=round<3?5:3,ew=w*gx,eh=h*gy;
        float input[512],actual[512],before[512],expected[128];
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
            float d=.3f+.001f*(y*w+x);expected[y*w+x]=d;
            for(unsigned sy=0;sy<gy;++sy)for(unsigned sx=0;sx<gx;++sx)
                input[(y*gy+sy)*ew+x*gx+sx]=d+.1f*(gx*gy-1-sy*gx-sx);
        }
        u32 src=image(VK_FORMAT_R32_SFLOAT,ew,eh,4,input),dst=image(VK_FORMAT_R32_SFLOAT,w,h,4,NULL);
        CHECK(src&&dst);
        CHECK(vk_ao_image_pass(&s_eobj[dst].im,VK_FORMAT_R32_SFLOAT,w,h,s_eobj[src].im.img,
            s_eobj[src].im.view,0,0,gx,gy,1,1)==1);
        CHECK(vk_eng_readback_rows_raw(dst,0,0,w,h,actual,w*4)==0);
        for(unsigned i=0;i<w*h;++i)CHECK(actual[i]==expected[i]);
        CHECK(vk_eng_readback_rows_raw(src,0,0,ew,eh,before,ew*4)==0);
        CHECK(memcmp(input,before,ew*eh*4)==0);
        CHECK(vk_ao_image_pass(&s_eobj[src].im,VK_FORMAT_R32_SFLOAT,w,h,s_eobj[src].im.img,
            s_eobj[src].im.view,0,0,gx,gy,1,1)==0);
        CHECK(vk_ao_image_pass(&s_eobj[dst].im,VK_FORMAT_R32_SFLOAT,w,h,s_eobj[src].im.img,
            s_eobj[src].im.view,0,0,3,1,1,1)==0);
        vk_eng_release(NULL,src);vk_eng_release(NULL,dst);
    }
    printf("AO nearest-depth resolve passed\n");return 0;
}

static int composite(void)
{
    for(unsigned round=0;round<6;++round) {
        unsigned gx=round%3?2:1,gy=round%3==2?2:1;
        unsigned w=round<3?7:9,h=round<3?5:3,ew=w*gx,eh=h*gy;
        uint8_t input[2048],actual[2048],before[2048];float ao[128];
        for(unsigned i=0;i<ew*eh*4;++i)input[i]=(uint8_t)(i*37+11);
        for(unsigned i=0;i<w*h;++i)ao[i]=.25f*(1+i%3);
        u32 src=image(VK_FORMAT_R8G8B8A8_UNORM,ew,eh,4,input),
            mask=image(VK_FORMAT_R32_SFLOAT,w,h,4,ao),dst=image(VK_FORMAT_R8G8B8A8_UNORM,ew,eh,4,NULL);
        CHECK(src&&mask&&dst);
        for(unsigned pass=0;pass<3;++pass) {
            float strength=pass*.5f;
            CHECK(vk_ao_image_pass(&s_eobj[dst].im,VK_FORMAT_R8G8B8A8_UNORM,ew,eh,
                s_eobj[src].im.img,s_eobj[src].im.view,s_eobj[mask].im.img,s_eobj[mask].im.view,gx,gy,strength,0)==1);
            CHECK(vk_eng_readback_rows_raw(dst,0,0,ew,eh,actual,ew*4)==0);
            for(unsigned y=0;y<eh;++y)for(unsigned x=0;x<ew;++x) {
                unsigned i=(y*ew+x)*4;float factor=1+strength*(ao[(y/gy)*w+x/gx]-1);
                for(unsigned c=0;c<3;++c)CHECK(fabsf(actual[i+c]-input[i+c]*factor)<=1.01f);
                CHECK(actual[i+3]==input[i+3]);
            }
        }
        CHECK(vk_eng_readback_rows_raw(src,0,0,ew,eh,before,ew*4)==0);
        CHECK(memcmp(input,before,ew*eh*4)==0);
        CHECK(vk_ao_image_pass(&s_eobj[dst].im,VK_FORMAT_R8G8B8A8_UNORM,ew,eh,
            s_eobj[src].im.img,s_eobj[src].im.view,s_eobj[mask].im.img,s_eobj[mask].im.view,gx,gy,NAN,0)==0);
        vk_eng_release(NULL,src);vk_eng_release(NULL,mask);vk_eng_release(NULL,dst);
    }
    {
        uint16_t input[24],out[24];float ao[6];
        for(unsigned i=0;i<6;++i) { input[i*4]=0x4000;input[i*4+1]=0x4400;input[i*4+2]=0x4600;input[i*4+3]=0x3400+i;ao[i]=.5f; }
        u32 src=image(VK_FORMAT_R16G16B16A16_SFLOAT,3,2,8,input),mask=image(VK_FORMAT_R32_SFLOAT,3,2,4,ao),
            dst=image(VK_FORMAT_R16G16B16A16_SFLOAT,3,2,8,NULL);
        CHECK(src&&mask&&dst);
        CHECK(vk_ao_image_pass(&s_eobj[dst].im,VK_FORMAT_R16G16B16A16_SFLOAT,3,2,s_eobj[src].im.img,
            s_eobj[src].im.view,s_eobj[mask].im.img,s_eobj[mask].im.view,1,1,1,0)==1);
        CHECK(vk_eng_readback_rows_raw(dst,0,0,3,2,out,24)==0);
        for(unsigned i=0;i<6;++i) { CHECK(out[i*4]==0x3c00);CHECK(out[i*4+1]==0x4000);CHECK(out[i*4+2]==0x4200);CHECK(out[i*4+3]==input[i*4+3]); }
        vk_eng_release(NULL,src);vk_eng_release(NULL,mask);vk_eng_release(NULL,dst);
    }
    printf("AO color composite, alpha and HDR passed\n");return 0;
}

static int cacao_world(void)
{
    static const char vs[]="float4 main(float4 p:ATTR0):SV_Position{return p;}";
    static const char fs[]="float4 main():SV_Target0{return float4(.8,.6,.4,.25);}";
    rsx_vertex_layout_plan layout={0};layout.count=1;layout.stride=16;layout.attrs[0]=0;
    rsx_be_render_state state={0};state.color_mask=0x01010101u;state.antialias_control=0xFFFF0001u;
    state.depth_test=1;state.depth_write=1;state.depth_func=0x201;
    u32 pipe=vk_eng_pipeline_create(NULL,vs,fs,&state,&layout,16,RSX_BE_FMT_R8G8B8A8,1);
    CHECK(pipe!=0);
    /* Perspective depth of surfaces at z=2 and z=3, near .1 and far100. */
    const float projection[16]={1,0,0,0,0,1,0,0,0,0,100.f/99.9f,1,0,0,-10.f/99.9f,0};
    const float foreground[6][4]={{-1,-1,.950951f,1},{0,-1,.950951f,1},{0,1,.950951f,1},
        {-1,-1,.950951f,1},{0,1,.950951f,1},{-1,1,.950951f,1}};
    for(unsigned round=0;round<3;++round) {
        u32 samples=round==0?1:round==1?2:4,w=round==2?96:64,h=64;
        u32 color=samples>1?vk_eng_color_multisample(NULL,RSX_BE_FMT_R8G8B8A8,w,h,samples):vk_eng_color_target_create(NULL,RSX_BE_FMT_R8G8B8A8,w,h,NULL,0);
        u32 depth=samples>1?vk_eng_depth_multisample(NULL,w,h,samples):vk_eng_depth_target_create(NULL,w,h);
        CHECK(color&&depth);const float bg[4]={.8f,.6f,.4f,.25f};
        vk_eng_clear_color(NULL,color,bg);vk_eng_clear_depth_stencil(NULL,depth,RSX_BE_CLEAR_DEPTH,.9676343f,0);
        vk_eng_bind_targets(NULL,&color,1,depth);vk_eng_bind_pipeline(NULL,pipe);
        vk_eng_set_viewport(NULL,0,0,w,h,0,1);vk_eng_set_scissor(NULL,0,0,w,h);
        vk_eng_draw(NULL,RSX_TOPOLOGY_TRIANGLES,foreground,6,16,NULL,0);
        u32 ew=w*(samples>1?2:1),eh=h*(samples>2?2:1);size_t bytes=(size_t)ew*eh*4;
        uint8_t *original=malloc(bytes),*out=malloc(bytes),*again=malloc(bytes);
        CHECK(original&&out&&again);CHECK(vk_eng_readback_rows(color,0,0,ew,eh,original,ew*4)==0);
        CHECK(vk_eng_cacao_world(NULL,color,depth,w,h,projection,0,0,1.2f,1)!=0);
        CHECK(vk_eng_readback_rows_raw(s_cacao.color,0,0,ew,eh,out,ew*4)==0);
        unsigned dark=0;
        for(size_t i=0;i<bytes;i+=4) {
            CHECK(out[i+3]==original[i+3]);
            for(unsigned c=0;c<3;++c)CHECK(out[i+c]<=original[i+c]);
            if(out[i]<original[i])++dark;
        }
        CHECK(dark>0);
        CHECK(vk_eng_readback_rows(color,0,0,ew,eh,again,ew*4)==0);CHECK(memcmp(original,again,bytes)==0);
        CHECK(vk_eng_cacao_world(NULL,color,depth,w,h,projection,0,0,1.2f,1)!=0);
        CHECK(vk_eng_readback_rows_raw(s_cacao.color,0,0,ew,eh,again,ew*4)==0);CHECK(memcmp(out,again,bytes)==0);
        CHECK(vk_eng_cacao_world(NULL,color,depth,w,h,projection,0,0,1.2f,0)!=0);
        CHECK(vk_eng_readback_rows_raw(s_cacao.color,0,0,ew,eh,again,ew*4)==0);CHECK(memcmp(original,again,bytes)==0);
        printf("CACAO world %u samples %ux%u: %u darkened pixels; source, alpha, repeat and strength0 passed\n",samples,w,h,dark);
        free(original);free(out);free(again);vk_eng_release(NULL,color);vk_eng_release(NULL,depth);
    }
    vk_eng_pipeline_release(NULL,pipe);return 0;
}

int main(void)
{
    SDL_SetMainReady();CHECK(SDL_setenv("PS3RECOMP_VK_GUEST_PROGRAMS","1",1)==0);
    CHECK(rsx_vulkan_backend_init(8,4,"AO transport regression")==0);
    CHECK(resolve()==0);CHECK(composite()==0);CHECK(cacao_world()==0);
    rsx_vulkan_backend_shutdown();
    CHECK(!s_cacao.context&&!s_cacao.color&&!s_vk_ao.base_ready);
    printf("AO GPU: %u checks passed\n",checks);return 0;
}
