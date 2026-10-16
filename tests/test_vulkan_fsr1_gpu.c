/* Executes the production FSR1 pass. Software Vulkan is useful for correctness;
 * actual target GPU and game image/performance acceptance are separate. */
#define SDL_MAIN_HANDLED
#include "../libs/video/rsx_vulkan_backend.c"
#include <math.h>
uint8_t* vm_base;
static unsigned checks;
#define CHECK(v) do { ++checks; if (!(v)) { fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#v); return 1; } } while(0)

static int transfer(VkImage image, u32 w, u32 h, u8* pixels, int write)
{
    vk_buffer b={0};
    if(vk_create_host_buffer(&b,(VkDeviceSize)w*h*4,
        write?VK_BUFFER_USAGE_TRANSFER_SRC_BIT:VK_BUFFER_USAGE_TRANSFER_DST_BIT))return -1;
    if(!b.coherent || vk_begin()) {vk_destroy_buffer(&b);return -1;}
    if(write)memcpy(b.ptr,pixels,(size_t)w*h*4);
    vk_barrier_image(image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy c={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={w,h,1}};
    if(write)pvkCmdCopyBufferToImage(s_vk.cmd,b.buf,image,VK_IMAGE_LAYOUT_GENERAL,1,&c);
    else {
        pvkCmdCopyImageToBuffer(s_vk.cmd,image,VK_IMAGE_LAYOUT_GENERAL,b.buf,1,&c);
        VkBufferMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .buffer=b.buf,.size=VK_WHOLE_SIZE};
        pvkCmdPipelineBarrier(s_vk.cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
            0,0,NULL,1,&host,0,NULL);
    }
    int rc=vk_submit_and_wait();
    if(!rc && !write) {vk_invalidate(&b);memcpy(pixels,b.ptr,(size_t)w*h*4);}
    vk_destroy_buffer(&b);return rc;
}
static unsigned clamp_coord(int x,unsigned n) {return x<0?0:(unsigned)x>=n?n-1:(unsigned)x;}
static double bilinear(const u8* p,u32 iw,u32 ih,u32 ow,u32 oh,u32 x,u32 y,unsigned channel)
{
    double sx=((double)x+.5)*iw/ow-.5,sy=((double)y+.5)*ih/oh-.5;
    int ix=(int)floor(sx),iy=(int)floor(sy);double fx=sx-ix,fy=sy-iy;
    double a=p[(clamp_coord(iy,ih)*iw+clamp_coord(ix,iw))*4+channel];
    double b=p[(clamp_coord(iy,ih)*iw+clamp_coord(ix+1,iw))*4+channel];
    double c=p[(clamp_coord(iy+1,ih)*iw+clamp_coord(ix,iw))*4+channel];
    double d=p[(clamp_coord(iy+1,ih)*iw+clamp_coord(ix+1,iw))*4+channel];
    return (a*(1-fx)+b*fx)*(1-fy)+(c*(1-fx)+d*fx)*fy;
}
static PFN_vkCreateSampler real_create_sampler;
static unsigned sampler_calls, fail_sampler_at;
static VKAPI_ATTR VkResult VKAPI_CALL fail_sampler(VkDevice d,const VkSamplerCreateInfo* ci,
    const VkAllocationCallbacks* a,VkSampler* out)
{
    if(++sampler_calls==fail_sampler_at) {*out=VK_NULL_HANDLE;return VK_ERROR_OUT_OF_HOST_MEMORY;}
    return real_create_sampler(d,ci,a,out);
}
static int run_size(u32 iw,u32 ih,u32 ow,u32 oh)
{
    CHECK(rsx_vulkan_backend_init(iw,ih,"FSR1 correctness fixture")==0);
    VkPhysicalDeviceProperties props;pvkGetPhysicalDeviceProperties(s_vk.phys,&props);
    printf("FSR1 device: %s; %ux%u to %ux%u\n",props.deviceName,iw,ih,ow,oh);
    const size_t input_bytes=(size_t)iw*ih*4,output_bytes=(size_t)ow*oh*4;
    u8* input=malloc(input_bytes);u8* preserved=malloc(input_bytes);
    u8* output=malloc(output_bytes);u8* again=malloc(output_bytes);u8* easu=malloc(output_bytes);
    CHECK(input && preserved && output && again && easu);
    for(u32 y=0;y<ih;++y)for(u32 x=0;x<iw;++x) {
        size_t i=((size_t)y*iw+x)*4;input[i]=23;input[i+1]=67;input[i+2]=199;input[i+3]=(u8)((x*7+y*13)&255);
    }
    CHECK(!transfer(s_vk.color.img,iw,ih,input,1));
    VkImage image=VK_NULL_HANDLE;
    CHECK(!vk_fsr1_apply(&s_vk.color,iw,ih,ow,oh,.2f,&image));
    CHECK(image==s_fsr1[1].out.img && image!=s_vk.color.img);
    CHECK(!transfer(image,ow,oh,output,0));
    CHECK(!transfer(s_fsr1[0].out.img,ow,oh,easu,0));
    double max_alpha_error=0;
    /* Texture filtering may quantize weights independently on both axes.
     * Bound that device-reported precision plus one UNORM quantization step. */
    const double alpha_tolerance=510.0*ldexp(1.0,-(int)props.limits.subTexelPrecisionBits)+1.0;
    for(u32 y=0;y<oh;++y)for(u32 x=0;x<ow;++x) {
        size_t i=((size_t)y*ow+x)*4;
        CHECK(abs((int)output[i]-23)<=1 && abs((int)output[i+1]-67)<=1 && abs((int)output[i+2]-199)<=1);
        const double error=fabs(output[i+3]-bilinear(input,iw,ih,ow,oh,x,y,3));
        if(error>max_alpha_error)max_alpha_error=error;
        CHECK(error<=alpha_tolerance);
        CHECK(output[i+3]==easu[i+3]);
    }
    printf("FSR1 bilinear alpha max_error=%g bound=%g subtexel_bits=%u\n",
        max_alpha_error,alpha_tolerance,props.limits.subTexelPrecisionBits);
    CHECK(!vk_fsr1_apply(&s_vk.color,iw,ih,ow,oh,.2f,&image));
    CHECK(!transfer(image,ow,oh,again,0) && !memcmp(output,again,output_bytes));
    CHECK(!transfer(s_vk.color.img,iw,ih,preserved,0) && !memcmp(input,preserved,input_bytes));
    /* Invalid sizes and constants leave the caller's output handle intact. */
    VkImage keep=image;
    CHECK(vk_fsr1_apply(&s_vk.color,iw,ih,0,oh,.2f,&image)==-1 && image==keep);
    CHECK(vk_fsr1_apply(&s_vk.color,iw,ih,ow,oh,NAN,&image)==-1 && image==keep);
    if(iw>1)CHECK(vk_fsr1_apply(&s_vk.color,iw,ih,iw-1,oh,.2f,&image)==-1 && image==keep);
    if(iw>=32) {
        for(u32 y=0;y<ih;++y)for(u32 x=0;x<iw;++x) {
            size_t i=((size_t)y*iw+x)*4;
            u8 level=((int)(y-.37*x)/3)&1?194:56;
            input[i]=level;input[i+1]=(u8)(level/2+17);input[i+2]=23;
        }
        CHECK(!transfer(s_vk.color.img,iw,ih,input,1));
        CHECK(!vk_fsr1_apply(&s_vk.color,iw,ih,ow,oh,0,&image));
        CHECK(!transfer(image,ow,oh,output,0));
        CHECK(!vk_fsr1_apply(&s_vk.color,iw,ih,ow,oh,2,&image));
        CHECK(!transfer(image,ow,oh,again,0));
        unsigned changed=0,nonlinear=0;
        for(u32 y=0;y<oh;++y)for(u32 x=0;x<ow;++x) {
            size_t i=((size_t)y*ow+x)*4;
            changed+=output[i]!=again[i];
            nonlinear+=fabs(output[i]-bilinear(input,iw,ih,ow,oh,x,y,0))>2;
            CHECK(output[i+3]==again[i+3]);
        }
        CHECK(changed>10 && nonlinear>10);
        printf("FSR1 RCAS strength changed %u pixels; %u differ from bilinear\n",changed,nonlinear);
        CHECK(!rsx_dump_ppm("fsr1-output.ppm",output,ow,oh));
        /* First- and second-pass failures both clean the whole private chain. */
        for(fail_sampler_at=1;fail_sampler_at<=2;++fail_sampler_at) {
            vk_fsr1_shutdown();sampler_calls=0;
            real_create_sampler=pvkCreateSampler;pvkCreateSampler=fail_sampler;
            int failed=vk_fsr1_apply(&s_vk.color,iw,ih,ow,oh,.2f,&image);pvkCreateSampler=real_create_sampler;
            CHECK(failed==-1 && !s_fsr1[0].ready && !s_fsr1[1].ready && !s_fsr1[0].out.img && !s_fsr1[1].out.img);
        }
        CHECK(!transfer(s_vk.color.img,iw,ih,preserved,0) && !memcmp(input,preserved,input_bytes));
        CHECK(!vk_fsr1_apply(&s_vk.color,iw,ih,ow+1,oh+1,.2f,&image));
        CHECK(s_fsr1[0].width==ow+1 && s_fsr1[1].height==oh+1);
        CHECK(!vk_fsr1_apply(&s_vk.color,iw,ih,ow,oh,2,&image));
        CHECK(!transfer(image,ow,oh,output,0) && !memcmp(output,again,output_bytes));
        /* Switching from the guest image to FXAA must refresh EASU's binding.
         * Neither postprocess may overwrite the guest image or FXAA result. */
        VkImage fxaa=VK_NULL_HANDLE;
        CHECK(!vk_fxaa_init() && !vk_fxaa_apply(&fxaa));
        CHECK(fxaa==s_fxaa.out.img);
        CHECK(!transfer(fxaa,iw,ih,preserved,0));
        CHECK(!vk_fsr1_apply(&s_fxaa.out,iw,ih,ow,oh,.2f,&image));
        CHECK(s_fsr1[0].src_view==s_fxaa.out.view);
        CHECK(!transfer(fxaa,iw,ih,output,0) && !memcmp(preserved,output,input_bytes));
        CHECK(!transfer(s_vk.color.img,iw,ih,output,0) && !memcmp(input,output,input_bytes));
        CHECK(!vk_fsr1_apply(&s_vk.color,iw,ih,ow,oh,2,&image));
        CHECK(s_fsr1[0].src_view==s_vk.color.view);
        CHECK(!transfer(image,ow,oh,output,0) && !memcmp(output,again,output_bytes));
    }
    free(input);free(preserved);free(output);free(again);free(easu);
    rsx_vulkan_backend_shutdown();
    CHECK(!s_fsr1[0].ready && !s_fsr1[1].ready && !s_fsr1[0].out.img && !s_fsr1[1].out.img);
    vk_fsr1_shutdown();return 0;
}
static int run_present(void)
{
    enum { IW=16, IH=9, OW=32, OH=18 };
    u8 input[IW*IH*4], preserved[sizeof(input)];
    u8 expected[OW*OH*4], plain[sizeof(expected)], filtered[sizeof(expected)];
    CHECK(!rsx_vulkan_backend_init(OW,OH,"FSR1 production display path"));
    for(unsigned y=0;y<IH;++y)for(unsigned x=0;x<IW;++x) {
        unsigned i=(y*IW+x)*4;
        input[i]=((int)(y-.37*x)/3)&1?194:56;
        input[i+1]=(u8)(input[i]/2+17);input[i+2]=23;input[i+3]=(u8)(x*7+y*13);
    }
    u32 surface=vk_eng_color_target_create(NULL,RSX_BE_FMT_R8G8B8A8,IW,IH,NULL,0);
    CHECK(surface);
    vk_eng_obj* source=vk_eng_get(surface,VK_ENG_COLOR);
    CHECK(source && !transfer(source->im.img,IW,IH,input,1));
    s_vk.fsr1_enabled=0;vk_eng_present(NULL,surface);
    CHECK(s_vk.presented==1);memcpy(plain,s_vk.readback.ptr,sizeof(plain));
    VkImage image=VK_NULL_HANDLE;
    CHECK(!vk_fsr1_apply(&source->im,IW,IH,OW,OH,.2f,&image));
    CHECK(!transfer(image,OW,OH,expected,0));
    s_vk.fsr1_enabled=1;vk_eng_present(NULL,surface);
    CHECK(s_vk.presented==2 && s_vk.fsr1_frames==1 && !s_vk.fsr1_failed);
    CHECK(!memcmp(expected,s_vk.readback.ptr,sizeof(expected)));
    unsigned differences=0;
    for(unsigned i=0;i<sizeof(expected);i+=4)differences+=plain[i]!=expected[i];
    CHECK(differences>10);
    CHECK(!transfer(source->im.img,IW,IH,preserved,0) && !memcmp(input,preserved,sizeof(input)));
    CHECK(!vk_fxaa_init());s_vk.fxaa_enabled=1;
    vk_eng_present(NULL,surface);
    CHECK(s_vk.presented==3 && s_vk.fsr1_frames==2 && s_vk.fxaa_frames==1);
    CHECK(!transfer(s_fxaa.out.img,OW,OH,filtered,0) && !memcmp(filtered,s_vk.readback.ptr,sizeof(filtered)));
    CHECK(!transfer(source->im.img,IW,IH,preserved,0) && !memcmp(input,preserved,sizeof(input)));
    s_vk.fxaa_enabled=0;vk_fsr1_shutdown();
    sampler_calls=0;fail_sampler_at=1;real_create_sampler=pvkCreateSampler;pvkCreateSampler=fail_sampler;
    vk_eng_present(NULL,surface);pvkCreateSampler=real_create_sampler;
    CHECK(s_vk.fsr1_failed && s_vk.presented==4 && !memcmp(plain,s_vk.readback.ptr,sizeof(plain)));
    vk_eng_present(NULL,surface);
    CHECK(s_vk.presented==5 && !memcmp(plain,s_vk.readback.ptr,sizeof(plain)));
    vk_eng_release(NULL,surface);rsx_vulkan_backend_shutdown();
    printf("FSR1 production present: direct-EASU pixels match, %u pixels differ from linear; FXAA and failure fallback passed\n",differences);
    return 0;
}
int main(void)
{
    SDL_SetMainReady();
    VkImage output=VK_NULL_HANDLE;
    CHECK(vk_fsr1_apply(NULL,1,1,2,2,.2f,&output)==-1 && !output);
    const u32 sizes[][4]={{1,1,2,2},{3,5,5,7},{32,19,64,38},{33,21,49,31}};
    for(unsigned i=0;i<4;++i)if(run_size(sizes[i][0],sizes[i][1],sizes[i][2],sizes[i][3]))return 1;
    if(run_present())return 1;
    printf("Vulkan FSR1: %u checks passed\n",checks);return 0;
}
