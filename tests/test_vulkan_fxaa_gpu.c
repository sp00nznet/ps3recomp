/* Actual optional postprocess, not a CPU mock or a gameplay quality claim. */
#define SDL_MAIN_HANDLED
#include "../libs/video/rsx_vulkan_backend.c"
#include <math.h>
uint8_t* vm_base;
static unsigned checks;
#define CHECK(v) do { ++checks; if (!(v)) { fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#v); return 1; } } while(0)
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
static VKAPI_ATTR VkResult VKAPI_CALL fail_sampler(VkDevice device,const VkSamplerCreateInfo* info,
    const VkAllocationCallbacks* allocator,VkSampler* sampler)
{
    (void)device;(void)info;(void)allocator;*sampler=VK_NULL_HANDLE;
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}
static int write_frame(const u8* pixels)
{
    vk_buffer staging={0};
    if(vk_create_host_buffer(&staging,(VkDeviceSize)s_vk.width*s_vk.height*4,VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return -1;
    if(!staging.coherent || vk_begin()) { vk_destroy_buffer(&staging); return -1; }
    memcpy(staging.ptr,pixels,(size_t)s_vk.width*s_vk.height*4);
    vk_barrier_image(s_vk.color.img,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={s_vk.width,s_vk.height,1}};
    pvkCmdCopyBufferToImage(s_vk.cmd,staging.buf,s_vk.color.img,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
    int result=vk_submit_and_wait();vk_destroy_buffer(&staging);return result;
}
static int read_frame(VkImage image,u8* pixels)
{
    if(vk_begin()) return -1;
    vk_barrier_image(image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={s_vk.width,s_vk.height,1}};
    pvkCmdCopyImageToBuffer(s_vk.cmd,image,VK_IMAGE_LAYOUT_GENERAL,s_vk.readback.buf,1,&copy);
    VkBufferMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .buffer=s_vk.readback.buf,.size=VK_WHOLE_SIZE};
    pvkCmdPipelineBarrier(s_vk.cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&host,0,NULL);
    if(vk_submit_and_wait())return -1;
    vk_invalidate(&s_vk.readback);memcpy(pixels,s_vk.readback.ptr,(size_t)s_vk.width*s_vk.height*4);return 0;
}
static int run_size(u32 width,u32 height)
{
    CHECK(rsx_vulkan_backend_init(width,height,"FXAA regression")==0);
    CHECK(s_vk.fxaa_enabled && s_fxaa.ready);
    CHECK(vk_fxaa_init()==0);
    VkImage output=VK_NULL_HANDLE;
    CHECK(vk_fxaa_apply(NULL)==-1);
    const u32 saved_width=s_vk.width;s_vk.width++;
    CHECK(vk_fxaa_init()==-1 && vk_fxaa_apply(&output)==-1 && !output);
    s_vk.width=saved_width;
    u8 input[65*64*4],filtered[65*64*4],again[65*64*4],unchanged[65*64*4];
    const size_t bytes=(size_t)width*height*4;
    for(u32 y=0;y<height;y++)for(u32 x=0;x<width;x++) {
        u8* p=input+(y*width+x)*4;p[0]=23;p[1]=67;p[2]=199;p[3]=(u8)((x*7+y*13)&255);
    }
    CHECK(write_frame(input)==0 && vk_fxaa_apply(&output)==0 && output!=s_vk.color.img);
    CHECK(read_frame(output,filtered)==0);
    for(size_t i=0;i<bytes;i++) CHECK(abs((int)filtered[i]-input[i])<=(i%4==3?0:1));
    if(width>=32 && height>=32) {
        for(unsigned channel=0;channel<3;channel++) {
            /* Exact16x16 area reference for a straight slanted black/color edge. */
            double baseline_error=0,filtered_error=0;unsigned changed=0;
            for(u32 y=0;y<height;y++)for(u32 x=0;x<width;x++) {
                u8* p=input+(y*width+x)*4;int on=(double)y+.5 > .37*((double)x+.5)+18.25;
                p[0]=p[1]=p[2]=0;p[channel]=on?255:0;p[3]=(u8)((x*7+y*13)&255);
            }
            CHECK(write_frame(input)==0 && vk_fxaa_apply(&output)==0 && read_frame(output,filtered)==0);
            CHECK(read_frame(s_vk.color.img,unchanged)==0 && memcmp(input,unchanged,bytes)==0);
            CHECK(vk_fxaa_apply(&output)==0 && read_frame(output,again)==0 && memcmp(filtered,again,bytes)==0);
            for(u32 y=0;y<height;y++)for(u32 x=0;x<width;x++) {
                const size_t i=((size_t)y*width+x)*4;unsigned covered=0;
                for(unsigned sy=0;sy<16;sy++)for(unsigned sx=0;sx<16;sx++)
                    if(y+(sy+.5)/16.0 > .37*(x+(sx+.5)/16.0)+18.25)covered++;
                const double ref=255.0*covered/256.0;
                baseline_error+=fabs((double)input[i+channel]-ref);
                filtered_error+=fabs((double)filtered[i+channel]-ref);
                if(filtered[i+channel]!=input[i+channel])changed++;
                CHECK(filtered[i+3]==input[i+3]);
                for(unsigned other=0;other<3;other++)if(other!=channel)CHECK(filtered[i+other]==0);
            }
            printf("FXAA %ux%u channel%u changed=%u reference_L1: off=%.3f on=%.3f\n",width,height,channel,changed,baseline_error,filtered_error);
            CHECK(changed>10 && filtered_error<baseline_error);
            if(width==64 && height==64 && channel==0) {
                CHECK(rsx_dump_ppm("fxaa-off.ppm",input,width,height)==0);
                CHECK(rsx_dump_ppm("fxaa-on.ppm",filtered,width,height)==0);
            }
        }
        /* The production present path must publish the filtered image, too. */
        vk_cb_present(NULL,0);
        CHECK(s_vk.display_image==s_fxaa.out.img && s_vk.fxaa_frames==1);
        CHECK(memcmp(s_vk.readback.ptr,filtered,bytes)==0);
        /* Failed setup must discard only its own partial resource set. */
        vk_fxaa_shutdown();
        PFN_vkCreateSampler real_create=pvkCreateSampler;pvkCreateSampler=fail_sampler;
        const int failed_init=vk_fxaa_init();pvkCreateSampler=real_create;
        CHECK(failed_init==-1 && !s_fxaa.ready && !s_fxaa.out.img && !s_fxaa.fb && !s_fxaa.vs && !s_fxaa.fs);
        CHECK(read_frame(s_vk.color.img,unchanged)==0 && memcmp(input,unchanged,bytes)==0);
        CHECK(vk_fxaa_init()==0 && vk_fxaa_apply(&output)==0 && read_frame(output,again)==0);
        CHECK(memcmp(again,filtered,bytes)==0);
    }
    rsx_vulkan_backend_shutdown();
    CHECK(!s_fxaa.ready && !s_fxaa.out.img && !s_fxaa.pipe);
    vk_fxaa_shutdown();
    return 0;
}
int main(void)
{
    SDL_SetMainReady();
    CHECK(set_env("PS3RECOMP_ANTIALIASING","fxaa")==0);
    VkImage output=VK_NULL_HANDLE;
    CHECK(vk_fxaa_apply(&output)==-1 && !output);
    for(unsigned i=0;i<4;i++) {
        const u32 dimensions[4][2]={{1,1},{3,5},{64,64},{65,37}};
        if(run_size(dimensions[i][0],dimensions[i][1])) {rsx_vulkan_backend_shutdown();return 1;}
    }
    printf("Vulkan FXAA GPU: %u checks passed\n",checks);
    return 0;
}
