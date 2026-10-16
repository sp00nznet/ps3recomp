/* CPU transfer boundary test for the actual Vulkan resolution implementation. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef struct { int img; } test_image;
typedef struct { u32 w,h,scale,grid_x,grid_y,bpp;test_image im; } vk_eng_obj;
static vk_eng_obj object;
static u8 image_bytes[65536];
static unsigned checks,failures;
#define CHECK(v) do {++checks;if(!(v)){++failures;fprintf(stderr,"line%d: %s\n",__LINE__,#v);}}while(0)
#define VK_LOG(...) ((void)0)
static u32 vk_max_dim(void){return 64;}
static int vk_eng_upload_region(int image,u32 layer,u32 mip,u32 x,u32 y,u32 w,u32 h,
                                size_t tight,u32 rows,const void* source,u32 pitch)
{
    (void)image;(void)layer;(void)mip;
    if(x>object.w||w>object.w-x||y>object.h||h>object.h-y||rows!=h||tight!=(size_t)w*4||pitch<tight)return -1;
    for(u32 iy=0;iy<h;++iy)memcpy(image_bytes+((size_t)(y+iy)*object.w+x)*4,(const u8*)source+(size_t)iy*pitch,tight);
    return 0;
}
#include "../rsx_vulkan_resolution.inc"
static int vk_eng_readback_rows_raw(u32 handle,u32 x,u32 y,u32 w,u32 h,void* out,u32 pitch)
{
    (void)handle;
    if(x>object.w||w>object.w-x||y>object.h||h>object.h-y||pitch<(size_t)w*4)return -1;
    for(u32 iy=0;iy<h;++iy)memcpy((u8*)out+(size_t)iy*pitch,image_bytes+((size_t)(y+iy)*object.w+x)*4,(size_t)w*4);
    return 0;
}
static int vk_eng_depth_readback_raw(u32 handle,u32 x,u32 y,u32 w,u32 h,void* out,u32 pitch)
{return vk_eng_readback_rows_raw(handle,x,y,w,h,out,pitch);}
static void set_scale(const char* value)
{
#ifdef _WIN32
    _putenv_s("PS3RECOMP_RENDER_SCALE",value?value:"");
#else
    if(value)setenv("PS3RECOMP_RENDER_SCALE",value,1);else unsetenv("PS3RECOMP_RENDER_SCALE");
#endif
}
static void transfer_case(u32 scale,u32 gx,u32 gy)
{
    const u32 gw=3*gx,gh=3*gy;
    object=(vk_eng_obj){gw*scale,gh*scale,scale,gx,gy,4,{1}};
    const size_t bytes=(size_t)object.w*object.h*4;
    for(size_t i=0;i<bytes;++i)image_bytes[i]=(u8)((i*13u+71u)%251u);
    u8 expected[65536];memcpy(expected,image_bytes,bytes);
    /* The rectangle starts on sample1 for expanded layouts. */
    const u32 x=1,y=1,w=2,h=2,pitch=12;
    u8 incoming[24];for(u32 i=0;i<sizeof incoming;++i)incoming[i]=(u8)(210u+i);
    /* Independent forward replication from each guest sample to host pixels. */
    for(u32 sy=0;sy<h;++sy)for(u32 sx=0;sx<w;++sx)
        for(u32 dy=0;dy<scale;++dy)for(u32 dx=0;dx<scale;++dx){
            const u32 lx=x+sx,ly=y+sy;
            const u32 hx=((lx/gx)*scale+dx)*gx+lx%gx;
            const u32 hy=((ly/gy)*scale+dy)*gy+ly%gy;
            memcpy(expected+((size_t)hy*object.w+hx)*4,incoming+(size_t)sy*pitch+sx*4,4);
        }
    CHECK(vk_eng_scaled_write(1,&object,x,y,w,h,incoming,pitch)==0);
    CHECK(memcmp(expected,image_bytes,bytes)==0);
    u8 result[1024];memset(result,0xA5,sizeof result);
    const u32 output_pitch=gw*4+8;
    CHECK(vk_eng_scaled_read(1,&object,4,0,0,gw,gh,result,output_pitch,vk_eng_readback_rows_raw)==0);
    for(u32 ly=0;ly<gh;++ly){
        for(u32 lx=0;lx<gw;++lx){
            const u32 hx=((lx/gx)*scale+scale/2)*gx+lx%gx;
            const u32 hy=((ly/gy)*scale+scale/2)*gy+ly%gy;
            CHECK(memcmp(result+(size_t)ly*output_pitch+lx*4,expected+((size_t)hy*object.w+hx)*4,4)==0);
        }
        for(u32 pad=gw*4;pad<output_pitch;++pad)CHECK(result[(size_t)ly*output_pitch+pad]==0xA5);
    }
    CHECK(vk_eng_scaled_read(1,&object,4,x,y,w,h,result,16,vk_eng_depth_readback_raw)==0);
    for(u32 iy=0;iy<h;++iy)CHECK(memcmp(result+iy*16,incoming+iy*pitch,w*4)==0);
    memcpy(expected,image_bytes,bytes);
    CHECK(vk_eng_scaled_write(1,&object,UINT32_MAX,0,2,2,incoming,8)==-1);
    CHECK(vk_eng_scaled_write(1,&object,0,0,2,2,incoming,7)==-1);
    CHECK(vk_eng_scaled_read(1,&object,4,0,0,gw,gh,result,gw*4-1,vk_eng_readback_rows_raw)==-1);
    CHECK(memcmp(expected,image_bytes,bytes)==0);
}
int main(void)
{
    for(u32 k=1;k<=4;++k){
        char value[2]={(char)('0'+k),0};set_scale(value);
        CHECK(vk_eng_resolution_config()==0&&s_render_scale==k);
        u32 w=3,h=5;CHECK(vk_eng_scale_extent(&w,&h,1)==0&&w==3*k&&h==5*k);
        w=UINT32_MAX;h=1;CHECK(vk_eng_scale_extent(&w,&h,4)==-1);
        w=64/k/2;h=64/k/2;CHECK(vk_eng_scale_extent(&w,&h,4)==0);
        transfer_case(k,1,1);transfer_case(k,2,1);transfer_case(k,2,2);
    }
    const char* invalid[]={"0","5","-1","1bad","","100"};
    for(u32 i=0;i<sizeof invalid/sizeof invalid[0];++i){
        set_scale(invalid[i]);
#ifdef _WIN32
        if(!invalid[i][0]){CHECK(vk_eng_resolution_config()==0&&s_render_scale==1);continue;}
#endif
        CHECK(vk_eng_resolution_config()==-1);
    }
    set_scale(NULL);CHECK(vk_eng_resolution_config()==0&&s_render_scale==1);
    printf("Vulkan resolution transfers: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
