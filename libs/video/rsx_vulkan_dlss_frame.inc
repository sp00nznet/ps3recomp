/* Same-device world SR. Recorded into the hybrid transport's command buffer,
 * after its composite and before guest HUD; the transport's one submission
 * covers both, so DLSS adds no CPU wait of its own.
 *
 * Extents: the output is the world target the game would render at the
 * internal raster scale (1280x720 times PS3RECOMP_RENDER_SCALE); the input is
 * NGX's optimal size for PS3RECOMP_DLSS_MODE (quality by default). The world
 * target is created at the input size (vk_eng_target_multisample). Per-frame
 * host data is double-buffered; counters the GPU writes are read one frame
 * later, once that frame's submission has completed. */
#if defined(_WIN32)
#include "rsx_dlss_motion_match.h"
#include "rsx_dlss_motion_stats.h"
typedef rsx_dlss_draw_key vk_dlss_draw_key;
typedef struct vk_dlss_frame_params {float reprojection[16],projection[4],jitter[4];u32 sizes[4],history[4],layout[4];} vk_dlss_frame_params;
_Static_assert(sizeof(vk_dlss_frame_params)==144,"GLSL std140 DLSS parameters layout");
#define VK_DLSS_DRAWS 256u
/* Draw rows, then 256 motion counters, 257 exclusions/no-owner, 258 reactive. */
#define VK_DLSS_ROWS (VK_DLSS_DRAWS+3u)
/* Reactive mask source: the world target, sample-expanded, as it stood
 * before the frame's last run of blended draws that keep depth (particles,
 * glows). Those layers have no captured geometry, so without this they take
 * the surface's motion and DLSS accumulates them away. */
static struct {vk_image image;u32 w,h;VkFormat fmt;u64 frame;int enabled,valid,after_opaque;u32 snapshots,frame_snapshots;} s_dlss_layer={.enabled=-1};
static int vk_dlss_layer_image(VkFormat format,u32 w,u32 h);
static struct {
    int initialized,failed,configured,have_camera,extent_known,recorded;
    rsx_dlss_quality quality;u32 in_w,in_h,out_w,out_h,phases;
    u32 expanded,previous_count,current_count,bank,vertex_capacity;
    u64 frame,completed,last_log,camera_key,ticks,record_frame,record_ticks;float camera[16];
    int record_reset,record_have_camera;u64 record_camera_key;float record_camera[16];
    vk_dlss_draw_key previous[VK_DLSS_DRAWS],current[VK_DLSS_DRAWS];
    vk_image color,depth,motion,bias,output;
    vk_buffer clips,draws[2],params[2];
    VkDescriptorSetLayout layout;VkPipelineLayout pipeline_layout;
    VkDescriptorPool pool;VkDescriptorSet set;VkPipeline pipeline;VkSampler sampler;
    rsx_dlss_motion_counts last_motion;
    int have_motion_counters;
    u64 motion_counter_frame;
    u32 last_nonfinite;
    u32 color_mode,transfer;int float_logged;
} s_dlss_frame;
/* World SR runs through whichever bridge created a context: DLSS, or FSR3
 * when only its bridge loaded (rsx_vulkan_fsr3.inc). */
static int vk_sr_context(void){return s_dlss.context!=NULL || s_fsr3.context!=NULL;}
static const char *vk_sr_name(void){return s_fsr3.context?"FSR3":"DLSS";}
/* PS3RECOMP_DLSS_COLOR. Folklore's world is a display-referred RGBA8 target
 * shown on a UNORM swapchain as stored. "ldr" (default) creates NGX in LDR
 * mode (no IsHDR, no exposure) through the optional rsx_dlss_configure2 and
 * passes the stored perceptual values through, as NVIDIA's guide places such
 * input; a bridge without configure2 uses "display". "display" hands the
 * stored values to NGX created with IsHDR+AutoExposure (the earlier
 * default). "linear" decodes an 8-bit sRGB-encoded world to linear light for
 * that HDR contract and encodes the result back. Measured at 3440x1440 the
 * three differ by under 1/255 in mean luma and not in FPS or motion
 * coverage. Decided once per session. */
enum {VK_DLSS_COLOR_DISPLAY,VK_DLSS_COLOR_LINEAR,VK_DLSS_COLOR_LDR};
static u32 vk_dlss_color_mode(void)
{
    const char *m=getenv("PS3RECOMP_DLSS_COLOR");
    if(!m || !*m)return VK_DLSS_COLOR_LDR;
    if(!strcmp(m,"display"))return VK_DLSS_COLOR_DISPLAY;
    if(!strcmp(m,"linear"))return VK_DLSS_COLOR_LINEAR;
    if(!strcmp(m,"ldr"))return VK_DLSS_COLOR_LDR;
    VK_LOG("PS3RECOMP_DLSS_COLOR '%s' unknown; using display\n",m);
    return VK_DLSS_COLOR_DISPLAY;
}
static const char *vk_dlss_color_name(u32 mode)
{return mode==VK_DLSS_COLOR_LINEAR?"linear":mode==VK_DLSS_COLOR_LDR?"ldr":"display";}
static const char *vk_dlss_quality_name(rsx_dlss_quality q)
{
    switch(q){case RSX_DLSS_PERFORMANCE:return "performance";case RSX_DLSS_BALANCED:return "balanced";
    case RSX_DLSS_QUALITY:return "quality";case RSX_DLSS_ULTRA_PERFORMANCE:return "ultra-performance";
    case RSX_DLSS_DLAA:return "dlaa";default:return "ultra-quality";}
}
static rsx_dlss_quality vk_dlss_mode(void)
{
    const char *m=getenv("PS3RECOMP_DLSS_MODE");
    if(!m || !*m || !strcmp(m,"quality"))return RSX_DLSS_QUALITY;
    if(!strcmp(m,"balanced"))return RSX_DLSS_BALANCED;
    if(!strcmp(m,"performance"))return RSX_DLSS_PERFORMANCE;
    if(!strcmp(m,"ultra-performance"))return RSX_DLSS_ULTRA_PERFORMANCE;
    if(!strcmp(m,"dlaa"))return RSX_DLSS_DLAA;
    VK_LOG("PS3RECOMP_DLSS_MODE '%s' unknown; using quality\n",m);
    return RSX_DLSS_QUALITY;
}
/* The input extent of the 1280x720 two-sample world target, or 0 when DLSS
 * cannot run (no context, NGX refused the size). Decided once per session. */
static int vk_dlss_world_extent(u32 *w,u32 *h)
{
    if(!vk_sr_context() || s_dlss_frame.failed)return 0;
    if(!s_dlss_frame.extent_known){
        s_dlss_frame.extent_known=1;
        s_dlss_frame.quality=vk_dlss_mode();
        s_dlss_frame.color_mode=vk_dlss_color_mode();
        s_dlss_frame.out_w=1280u*s_render_scale;s_dlss_frame.out_h=720u*s_render_scale;
        /* The guest still samples an expanded two-lane texture. Validate the
         * physical allocation, not only NGX's logical output dimensions. */
        if(s_dlss_frame.out_w>vk_max_dim()/2u || s_dlss_frame.out_h>vk_max_dim()){
            VK_LOG("DLSS output exceeds expanded-world image limit; raster retained\n");
            s_dlss_frame.failed=1;return 0;
        }
        if(s_fsr3.context){
            /* FSR's render size and its own phase count (8 x area ratio). */
            s_fsr3.quality=vk_fsr3_mode();
            uint32_t w=0,h=0,phases=0;float jx=0,jy=0;
            if(s_fsr3.render_size(s_dlss_frame.out_w,s_dlss_frame.out_h,s_fsr3.quality,&w,&h)!=RSX_FSR3_OK ||
               !w || !h || w>s_dlss_frame.out_w || h>s_dlss_frame.out_h ||
               s_fsr3.jitter(w,s_dlss_frame.out_w,0,&jx,&jy,&phases)!=RSX_FSR3_OK || !phases){
                VK_LOG("FSR3 %s has no render size for %ux%u; world stays at the raster scale\n",
                    vk_fsr3_mode_name(s_fsr3.quality),s_dlss_frame.out_w,s_dlss_frame.out_h);
                s_dlss_frame.failed=1;return 0;
            }
            s_dlss_frame.in_w=w;s_dlss_frame.in_h=h;s_dlss_frame.phases=phases;
            VK_LOG("FSR3 %s: world input %ux%u -> output %ux%u, %u jitter phases\n",vk_fsr3_mode_name(s_fsr3.quality),
                s_dlss_frame.in_w,s_dlss_frame.in_h,s_dlss_frame.out_w,s_dlss_frame.out_h,s_dlss_frame.phases);
        }else{
        rsx_dlss_size size;memset(&size,0,sizeof size);
        rsx_dlss_status status=s_dlss.loader.optimal_size(s_dlss.context,s_dlss_frame.out_w,s_dlss_frame.out_h,s_dlss_frame.quality,&size);
        if(status!=RSX_DLSS_OK || !size.input_width || !size.input_height ||
           size.input_width>s_dlss_frame.out_w || size.input_height>s_dlss_frame.out_h){
            VK_LOG("DLSS %s has no optimal input for %ux%u (status=%d); world stays at the raster scale\n",
                vk_dlss_quality_name(s_dlss_frame.quality),s_dlss_frame.out_w,s_dlss_frame.out_h,status);
            s_dlss_frame.failed=1;return 0;
        }
        s_dlss_frame.in_w=size.input_width;s_dlss_frame.in_h=size.input_height;
        /* Halton phases: 8 times the area ratio, as NGX recommends. */
        double ratio=(double)s_dlss_frame.out_w*s_dlss_frame.out_h/((double)s_dlss_frame.in_w*s_dlss_frame.in_h);
        u32 phases=(u32)ceil(8.0*ratio);s_dlss_frame.phases=phases<8?8:phases>64?64:phases;
        VK_LOG("DLSS %s: world input %ux%u -> output %ux%u, %u jitter phases\n",vk_dlss_quality_name(s_dlss_frame.quality),
            s_dlss_frame.in_w,s_dlss_frame.in_h,s_dlss_frame.out_w,s_dlss_frame.out_h,s_dlss_frame.phases);
        }
        VK_LOG("%s extent contract guest=1280x720 sampleLanes=2 reconstruction=%ux%u expandedStorage=%ux%u canvas=%ux%u presentation=%ux%u finalComposite=guest-layout nativeDisplayDLAA=unverified\n",vk_sr_name(),
            s_dlss_frame.out_w,s_dlss_frame.out_h,s_dlss_frame.out_w*2u,s_dlss_frame.out_h,
            s_vk.width,s_vk.height,s_vk.sc_extent.width,s_vk.sc_extent.height);
    }
    if(!s_dlss_frame.in_w)return 0;
    *w=s_dlss_frame.in_w;*h=s_dlss_frame.in_h;return 1;
}
static float vk_dlss_halton(u64 index,u32 base){float value=0,f=1;while(index){f/=base;value+=f*(index%base);index/=base;}return value;}
static void vk_dlss_jitter(float out[2])
{
    out[0]=out[1]=0;
    if(!vk_sr_context() || s_dlss_frame.failed || !s_dlss_frame.in_w)return;
    u64 index=s_rt_capture.frame%(s_dlss_frame.phases?s_dlss_frame.phases:8)+1;
    out[0]=vk_dlss_halton(index,2)-.5f;out[1]=vk_dlss_halton(index,3)-.5f;
}
static void vk_dlss_jitter_upload(void *constants,size_t bytes)
{
    if(bytes<514*16 || !s_eng_bound.nrt)return;
    const vk_eng_obj *o=&s_eobj[s_eng_bound.rt[0]];
    if(!o->dlss_world)return;
    float jitter[2];vk_dlss_jitter(jitter);float *vc=(float*)constants;
    vc[513*4]+=2*jitter[0]/o->w;vc[513*4+1]-=2*jitter[1]/o->h;
}
static int vk_dlss_inverse(const float m[16],float inverse[16])
{
    double a[4][8];for(u32 r=0;r<4;r++)for(u32 c=0;c<8;c++)a[r][c]=c<4?m[r*4+c]:(c-4==r);
    for(u32 k=0;k<4;k++){
        u32 pivot=k;for(u32 r=k+1;r<4;r++)if(fabs(a[r][k])>fabs(a[pivot][k]))pivot=r;
        if(fabs(a[pivot][k])<1e-10)return 0;
        for(u32 c=0;c<8;c++){double t=a[k][c];a[k][c]=a[pivot][c];a[pivot][c]=t;}
        double d=a[k][k];for(u32 c=0;c<8;c++)a[k][c]/=d;
        for(u32 r=0;r<4;r++)if(r!=k){d=a[r][k];for(u32 c=0;c<8;c++)a[r][c]-=d*a[k][c];}
    }
    for(u32 r=0;r<4;r++)for(u32 c=0;c<4;c++){inverse[r*4+c]=(float)a[r][c+4];if(!isfinite(inverse[r*4+c]))return 0;}
    return 1;
}
static void vk_dlss_layout(vk_image *image,VkImageLayout old,VkImageLayout next)
{
    VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout=old,.newLayout=next,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image->img,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    pvkCmdPipelineBarrier(s_vk.cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,NULL,0,NULL,1,&b);
}
static int vk_dlss_frame_initialize(void)
{
    if(s_dlss_frame.initialized)return 1;
    if(s_dlss_frame.failed || !vk_sr_context())return 0;
    VkDescriptorSetLayoutBinding bindings[16];
    for(u32 i=0;i<16;i++)bindings[i]=(VkDescriptorSetLayoutBinding){i,
        i==0||i==1||i==12||i==15?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        (i>=2&&i<=5)||i==14?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:i==6?VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
        i==7?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL};
    VkDescriptorSetLayoutCreateInfo layout={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=16,.pBindings=bindings};
    if(pvkCreateDescriptorSetLayout(s_vk.device,&layout,NULL,&s_dlss_frame.layout)!=VK_SUCCESS)goto fail;
    VkPushConstantRange push={VK_SHADER_STAGE_COMPUTE_BIT,0,4};
    VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&s_dlss_frame.layout,.pushConstantRangeCount=1,.pPushConstantRanges=&push};
    if(pvkCreatePipelineLayout(s_vk.device,&pl,NULL,&s_dlss_frame.pipeline_layout)!=VK_SUCCESS)goto fail;
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,4},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,5},{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,1},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,5}};
    VkDescriptorPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=5,.pPoolSizes=sizes};
    if(pvkCreateDescriptorPool(s_vk.device,&pool,NULL,&s_dlss_frame.pool)!=VK_SUCCESS)goto fail;
    VkDescriptorSetAllocateInfo set={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=s_dlss_frame.pool,.descriptorSetCount=1,.pSetLayouts=&s_dlss_frame.layout};
    if(pvkAllocateDescriptorSets(s_vk.device,&set,&s_dlss_frame.set)!=VK_SUCCESS)goto fail;
    u32 words[16384],count=0;char log[2048];
    if(rsx_glsl_compute_to_spirv(rsx_dlss_frame_glsl,words,16384,&count,log,sizeof log)){VK_LOG("DLSS frame shader: %s\n",log);goto fail;}
    VkShaderModule module=VK_NULL_HANDLE;VkShaderModuleCreateInfo sm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=count*4u,.pCode=words};
    if(pvkCreateShaderModule(s_vk.device,&sm,NULL,&module)!=VK_SUCCESS)goto fail;
    VkComputePipelineCreateInfo pc={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"},.layout=s_dlss_frame.pipeline_layout};
    PFN_vkCreateComputePipelines create=(PFN_vkCreateComputePipelines)pvkGetDeviceProcAddr(s_vk.device,"vkCreateComputePipelines");
    VkResult result=create?create(s_vk.device,vk_pcache(),1,&pc,NULL,&s_dlss_frame.pipeline):VK_ERROR_FEATURE_NOT_PRESENT;
    pvkDestroyShaderModule(s_vk.device,module,NULL);if(result!=VK_SUCCESS)goto fail;
    VkSamplerCreateInfo sampler={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST,.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.maxLod=0};
    if(pvkCreateSampler(s_vk.device,&sampler,NULL,&s_dlss_frame.sampler)!=VK_SUCCESS)goto fail;
    s_dlss_frame.initialized=1;return 1;
fail:s_dlss_frame.failed=1;return 0;
}
/* Private DLSS images and the expanded world replacement, sized once from the
 * session's extents. Called before the transport begins recording. */
static int vk_dlss_frame_images(void)
{
    if(s_dlss_frame.expanded)return 1;
    const u32 iw=s_dlss_frame.in_w,ih=s_dlss_frame.in_h,ow=s_dlss_frame.out_w,oh=s_dlss_frame.out_h;
    if(!iw || !ih || !ow || !oh)return 0;
    VkImageUsageFlags use=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if(vk_create_image(&s_dlss_frame.color,VK_FORMAT_R16G16B16A16_SFLOAT,iw,ih,use,VK_IMAGE_ASPECT_COLOR_BIT)||
       vk_create_image(&s_dlss_frame.depth,VK_FORMAT_R32_SFLOAT,iw,ih,use,VK_IMAGE_ASPECT_COLOR_BIT)||
       vk_create_image(&s_dlss_frame.motion,VK_FORMAT_R32G32_SFLOAT,iw,ih,use,VK_IMAGE_ASPECT_COLOR_BIT)||
       vk_create_image(&s_dlss_frame.bias,VK_FORMAT_R8_UNORM,iw,ih,use,VK_IMAGE_ASPECT_COLOR_BIT)||
       vk_create_image(&s_dlss_frame.output,VK_FORMAT_R16G16B16A16_SFLOAT,ow,oh,use,VK_IMAGE_ASPECT_COLOR_BIT))return 0;
    u32 expanded=vk_eng_alloc();if(!expanded)return 0;
    vk_eng_obj *o=&s_eobj[expanded];
    if(vk_create_image(&o->im,VK_FORMAT_R8G8B8A8_UNORM,ow*2u,oh,use,VK_IMAGE_ASPECT_COLOR_BIT)){memset(o,0,sizeof *o);return 0;}
    o->kind=VK_ENG_COLOR;o->fmt=VK_FORMAT_R8G8B8A8_UNORM;o->bpp=4;o->w=ow*2u;o->h=oh;o->scale=s_render_scale;o->grid_x=2;o->grid_y=1;
    s_dlss_frame.expanded=expanded;
    for(u32 b=0;b<2;b++)
        if(!vk_ray_buffer(&s_dlss_frame.draws[b],VK_DLSS_ROWS*16u,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,2,0)||
           !vk_ray_buffer(&s_dlss_frame.params[b],sizeof(vk_dlss_frame_params),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,2,0))return 0;
    for(u32 b=0;b<2;b++)memset(s_dlss_frame.draws[b].ptr,0,VK_DLSS_ROWS*16u);
    if(vk_begin())return 0;
    vk_image *images[]={&s_dlss_frame.color,&s_dlss_frame.depth,&s_dlss_frame.motion,&s_dlss_frame.bias,&s_dlss_frame.output,&o->im};
    for(u32 i=0;i<6;i++)vk_barrier_image(images[i]->img,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED);
    return !vk_submit_and_wait();
}
/* Two vertex banks plus one per-pixel depth history. A larger scene grows it
 * and starts a new history (the previous bank is gone). */
static int vk_dlss_clip_capacity(u32 vertices,int *reset)
{
    if(vertices<=s_dlss_frame.vertex_capacity && s_dlss_frame.clips.buf)return 1;
    u32 capacity=262144u;while(capacity<vertices && capacity<(1u<<24))capacity*=2u;
    if(capacity<vertices)return 0;
    vk_ray_destroy_buffer(&s_dlss_frame.clips);s_dlss_frame.vertex_capacity=0;
    VkDeviceSize bytes=((VkDeviceSize)capacity*2u+(VkDeviceSize)s_dlss_frame.in_w*s_dlss_frame.in_h)*16u;
    if(!vk_ray_buffer(&s_dlss_frame.clips,bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,0,0))return 0;
    s_dlss_frame.vertex_capacity=capacity;*reset=1;
    if(vk_begin())return 0;
    s_ray_ao.fill_buffer(s_vk.cmd,s_dlss_frame.clips.buf,0,s_dlss_frame.clips.size,0);
    return !vk_submit_and_wait();
}
/* Everything that needs its own submission happens here, before the hybrid
 * transport begins recording. Returns 1 when vk_dlss_record may be called. */
static int vk_dlss_prepare(u32 vertices,VkFormat world_format)
{
    u32 w,h;
    if(!vk_sr_context() || s_dlss_frame.failed || !vk_dlss_world_extent(&w,&h))return 0;
    if(!vk_dlss_frame_initialize()||!vk_dlss_frame_images())goto fail;
    if(!s_dlss_frame.configured && s_fsr3.context){
        /* No command recording: FSR uploads its tables inside the first
         * evaluation. LINEAR decodes to linear light (HDR contract); the
         * default and DISPLAY pass Folklore's display-referred values. */
        const int linear=s_dlss_frame.color_mode==VK_DLSS_COLOR_LINEAR;
        rsx_fsr3_config config={1,s_dlss_frame.in_w,s_dlss_frame.in_h,s_dlss_frame.out_w,s_dlss_frame.out_h,
                                linear?RSX_FSR3_COLOR_LINEAR_HDR:RSX_FSR3_COLOR_LDR,0,0,0};
        rsx_fsr3_status configured=s_fsr3.configure(s_fsr3.context,&config);
        if(configured!=RSX_FSR3_OK){VK_LOG("FSR3 configure color=%s status=%d\n",vk_dlss_color_name(s_dlss_frame.color_mode),configured);goto fail;}
        VK_LOG("FSR3 color contract effective=%s input=RGBA16F output=RGBA8-clamped depth=[0,1]-not-inverted sharpness=%.2f reactive=%d\n",
            linear?"linear-hdr":"ldr",vk_fsr3_sharpness(),vk_fsr3_reactive());
        s_dlss_frame.configured=1;
    }
    if(!s_dlss_frame.configured){
        if(vk_begin())goto fail;
        const u32 requested=s_dlss_frame.color_mode;
        if(requested==VK_DLSS_COLOR_LDR && !s_dlss.loader.configure2){
            VK_LOG("DLSS color: bridge lacks rsx_dlss_configure2 (LDR mode); using display\n");
            s_dlss_frame.color_mode=VK_DLSS_COLOR_DISPLAY;
        }
        const int ldr=s_dlss_frame.color_mode==VK_DLSS_COLOR_LDR;
        rsx_dlss_config config={1,s_dlss_frame.in_w,s_dlss_frame.in_h,s_dlss_frame.out_w,s_dlss_frame.out_h,s_dlss_frame.quality,ldr?0u:1u};
        rsx_dlss_config2 config2={sizeof config2,ldr?RSX_DLSS_COLOR_LDR_PERCEPTUAL:RSX_DLSS_COLOR_LINEAR_HDR,config};
        rsx_dlss_status configured=ldr?s_dlss.loader.configure2(s_dlss.context,s_vk.cmd,&config2):
                                       s_dlss.loader.configure(s_dlss.context,s_vk.cmd,&config);
        if(configured!=RSX_DLSS_OK){
            VK_LOG("DLSS configure color=%s status=%d\n",vk_dlss_color_name(s_dlss_frame.color_mode),configured);
            vk_submit_and_wait();goto fail;
        }
        VK_LOG("DLSS color contract requested=%s effective=%s bridgeFlags=%s input=RGBA16F output=RGBA8-clamped transfer=%s\n",
            vk_dlss_color_name(requested),vk_dlss_color_name(s_dlss_frame.color_mode),
            ldr?"LDR(no IsHDR, no AutoExposure)":"IsHDR+AutoExposure",
            s_dlss_frame.color_mode==VK_DLSS_COLOR_LINEAR?"srgb-decode-in/encode-out (8-bit UNORM world only)":"stored-values-passthrough");
        if(vk_submit_and_wait())goto fail;
        s_dlss_frame.configured=1;
    }
    /* The reactive snapshot is always bound (the shader samples it as a
     * two-sample image), so it exists whenever DLSS records. */
    if(s_dlss_layer.enabled<0){const char *e=getenv("PS3RECOMP_DLSS_REACTIVE");s_dlss_layer.enabled=e && !strcmp(e,"1");}
    if(!vk_dlss_layer_image(world_format,s_dlss_frame.in_w,s_dlss_frame.in_h))goto fail;
    int grown=0;if(!vk_dlss_clip_capacity(vertices,&grown))goto fail;
    if(grown)s_dlss_frame.completed=0;
    return 1;
fail:
    s_dlss_frame.failed=1;VK_LOG("%s SR setup failed; raster/PT retained, evaluations=%llu\n",vk_sr_name(),(unsigned long long)s_dlss_frame.completed);return 0;
}
/* The private two-sample snapshot image, (re)created at the world target's
 * format and extent; a fresh image is moved to GENERAL in the open command
 * buffer. Returns 0 when it cannot exist (the mask is then disabled). */
static int vk_dlss_layer_image(VkFormat format,u32 w,u32 h)
{
    if(s_dlss_layer.image.img && s_dlss_layer.w==w && s_dlss_layer.h==h && s_dlss_layer.fmt==format)return 1;
    vk_destroy_image(&s_dlss_layer.image);s_dlss_layer.valid=0;
    if(vk_create_image_levels_samples(&s_dlss_layer.image,format,w,h,1,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,VK_IMAGE_ASPECT_COLOR_BIT,2)){
        vk_destroy_image(&s_dlss_layer.image);s_dlss_layer.enabled=0;VK_LOG("DLSS reactive layer snapshot unavailable; disabled\n");return 0;
    }
    s_dlss_layer.w=w;s_dlss_layer.h=h;s_dlss_layer.fmt=format;
    if(vk_begin())return 0;
    vk_barrier_image(s_dlss_layer.image.img,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED);
    return 1;
}
/* Before a draw into `target` is recorded. On the first blended draw that
 * keeps depth after any depth-writing draw this frame, the two-sample world
 * target is copied as is into a private image, so the snapshot predates the
 * last run of such layers. Engine state (expansion, render-target flags) is
 * not touched. Opt-in (PS3RECOMP_DLSS_REACTIVE=1): on Faerypool pairs it did
 * not measurably change sparkle retention. */
static void vk_dlss_reactive_before_draw(u32 target,const vk_eng_pipe *pipe)
{
    if(s_dlss_layer.enabled<0){const char *e=getenv("PS3RECOMP_DLSS_REACTIVE");s_dlss_layer.enabled=e && !strcmp(e,"1");}
    if(!s_dlss_layer.enabled || !s_dlss_frame.configured || s_dlss_frame.failed || !pipe)return;
    const vk_eng_obj *o=vk_eng_get(target,VK_ENG_COLOR);
    if(!o || !o->dlss_world || o->samples!=2)return;
    if(s_dlss_layer.frame!=s_rt_capture.frame){
        s_dlss_layer.frame=s_rt_capture.frame;s_dlss_layer.valid=0;s_dlss_layer.after_opaque=1;s_dlss_layer.frame_snapshots=0;
    }
    if(!pipe->rs.blend_enable || pipe->rs.depth_write){s_dlss_layer.after_opaque=1;return;}
    if(!s_dlss_layer.after_opaque)return;
    s_dlss_layer.after_opaque=0;s_dlss_layer.valid=0;
    if(!vk_dlss_layer_image(o->fmt,o->w,o->h) || vk_begin())return;
    vk_barrier_image(o->im.img,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(s_dlss_layer.image.img,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL);
    VkImageCopy region={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.extent={o->w,o->h,1}};
    pvkCmdCopyImage(s_vk.cmd,o->im.img,VK_IMAGE_LAYOUT_GENERAL,s_dlss_layer.image.img,VK_IMAGE_LAYOUT_GENERAL,1,&region);
    if(vk_submit_or_defer())return;
    s_dlss_layer.valid=1;s_dlss_layer.snapshots++;s_dlss_layer.frame_snapshots++;
}
static rsx_dlss_image vk_dlss_image(vk_image *im,VkFormat format,u32 w,u32 h,VkImageLayout layout)
{return (rsx_dlss_image){im->img,im->view,format,w,h,{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},layout};}
/* Records DLSS input preparation, evaluation and the expanded output into the
 * open transport command buffer. Never submits, waits or begins a command
 * buffer. camera/have_camera/camera_key: the transport's frame camera.
 * Returns the replacement surface view, or 0 (nothing was recorded). */
static u32 vk_dlss_record(u32 source,u32 depth,VkBuffer vertices,VkDeviceSize vertex_bytes,const rsx_scene_epoch *epoch,
                          const float projection[16],const float camera[16],int have_camera,u64 camera_key,u32 remap,u32 format)
{
    s_dlss_frame.recorded=0;
    if(!vk_sr_context() || s_dlss_frame.failed || !s_dlss_frame.configured || !epoch)return 0;
    const u32 iw=s_dlss_frame.in_w,ih=s_dlss_frame.in_h,ow=s_dlss_frame.out_w,oh=s_dlss_frame.out_h;
    if(s_eobj[source].w!=iw*2u || s_eobj[source].h!=ih || s_eobj[depth].w!=iw*2u || s_eobj[depth].h!=ih)return 0;
    if(epoch->vertex_count>s_dlss_frame.vertex_capacity)return 0;
    const u32 bank=s_dlss_frame.bank;
    /* The previous frame's counters: its submission completed before this
     * frame's transport began (single queue, synchronous submission). */
    u32 (*last)[4]=s_dlss_frame.draws[1-bank].ptr;
    int reset=!s_dlss_frame.completed || epoch->frame!=s_dlss_frame.frame+1;
    if(!s_dlss_frame.completed){
        memset(&s_dlss_frame.last_motion,0,sizeof s_dlss_frame.last_motion);
        s_dlss_frame.have_motion_counters=0;s_dlss_frame.motion_counter_frame=0;
    }
    if(s_dlss_frame.completed){
        s_dlss_frame.last_motion=(rsx_dlss_motion_counts){
            last[VK_DLSS_DRAWS][0],last[VK_DLSS_DRAWS][1],
            last[VK_DLSS_DRAWS][2],last[VK_DLSS_DRAWS][3],
            last[VK_DLSS_DRAWS+1][1],last[VK_DLSS_DRAWS+1][2]};
        s_dlss_frame.have_motion_counters=1;
        s_dlss_frame.motion_counter_frame=s_dlss_frame.frame;
        if(last[VK_DLSS_DRAWS+1][0]){s_dlss_frame.last_nonfinite+=last[VK_DLSS_DRAWS+1][0];reset=1;
            VK_LOG("DLSS output nonfinite samples=%u; history reset\n",last[VK_DLSS_DRAWS+1][0]);}
        if(last[VK_DLSS_DRAWS][0] && last[VK_DLSS_DRAWS][1]>last[VK_DLSS_DRAWS][0]/2)reset=1;
    }
    vk_dlss_frame_params p;memset(&p,0,sizeof p);
    p.projection[0]=projection[0];p.projection[1]=projection[5];p.projection[2]=projection[10];p.projection[3]=projection[14];
    vk_dlss_jitter(p.jitter);p.sizes[0]=iw;p.sizes[1]=ih;p.sizes[2]=epoch->vertex_count;
    const u32 capacity=s_dlss_frame.vertex_capacity;
    p.history[0]=(1-bank)*capacity;p.history[1]=bank*capacity;p.history[2]=ow;p.history[3]=oh;
    p.layout[0]=2u*capacity;p.layout[1]=2u;
    u32 (*map)[4]=s_dlss_frame.draws[bank].ptr;memset(map,0,VK_DLSS_ROWS*16u);
    vk_dlss_draw_key *current=s_dlss_frame.current;memset(current,0,sizeof s_dlss_frame.current);
    u32 base=0,draws=epoch->draw_count<VK_DLSS_DRAWS?epoch->draw_count:VK_DLSS_DRAWS;
    p.layout[2]=draws;
    if(epoch->draw_count>VK_DLSS_DRAWS)reset=1;
    for(u32 i=0;i<draws;i++){
        rsx_scene_draw_input draw;if(!rsx_scene_epoch_get(epoch,i,&draw))return 0;
        u64 topology=1469598103934665603ull;
        for(u32 k=0;k<draw.index_count;k++)topology=(topology^draw.indices[k])*1099511628211ull;
        topology=(topology^draw.pipeline)*1099511628211ull;
        current[i]=(vk_dlss_draw_key){draw.geometry_identity,draw.constant_slots,topology,base,draw.vertex_count};
        base+=draw.vertex_count;
    }
    if(!rsx_dlss_match_draws(s_dlss_frame.previous,s_dlss_frame.previous_count,current,draws,reset,map))return 0;
    float inverse[16];int invertible=have_camera && vk_dlss_inverse(camera,inverse);
    if(!invertible || !s_dlss_frame.have_camera)reset=1;
    if(invertible && s_dlss_frame.have_camera){
        /* Every terrain draw shares the world light space, so another camera
         * draw only matters when its matrix differs; a cut differs a lot. */
        float maximum=0;for(u32 i=0;i<16;i++)maximum=fmaxf(maximum,fabsf(camera[i]-s_dlss_frame.camera[i])/fmaxf(1,fabsf(s_dlss_frame.camera[i])));
        if(maximum>.35f || (camera_key!=s_dlss_frame.camera_key && maximum>.05f))reset=1;
        for(u32 r=0;r<4;r++)for(u32 c=0;c<4;c++){float value=0;for(u32 k=0;k<4;k++)value+=s_dlss_frame.camera[r*4+k]*inverse[k*4+c];p.reprojection[c*4+r]=value;}
    }else for(u32 i=0;i<4;i++)p.reprojection[i*5]=1;
    /* Linearize only a fixed-point world: float targets already hold linear
     * values. A transfer change invalidates the history's color space. */
    const VkFormat source_format=s_eobj[source].fmt;
    const int fixed_point=source_format==VK_FORMAT_R8G8B8A8_UNORM || source_format==VK_FORMAT_B8G8R8A8_UNORM;
    const int linear=s_dlss_frame.color_mode==VK_DLSS_COLOR_LINEAR;
    const u32 transfer=linear && fixed_point;
    if(linear && !fixed_point && !s_dlss_frame.float_logged){
        s_dlss_frame.float_logged=1;VK_LOG("DLSS color: world format %d is not 8-bit UNORM; stored values passed through\n",(int)source_format);
    }
    if(s_dlss_frame.completed && transfer!=s_dlss_frame.transfer)reset=1;
    s_dlss_frame.transfer=transfer;p.layout[3]=transfer;
    /* Reactive layers only against the raster world it was taken from: a
     * traced composite differs from that snapshot everywhere. */
    const vk_eng_obj *src=&s_eobj[source];
    const int layer=s_dlss_layer.valid && s_dlss_layer.frame==s_rt_capture.frame && src->kind==VK_ENG_TEXTURE &&
        s_dlss_layer.w*2u==src->w && s_dlss_layer.h==src->h && s_dlss_layer.fmt==src->fmt;
    p.jitter[2]=layer?1.0f:0.0f;
    p.sizes[3]=reset;memcpy(s_dlss_frame.params[bank].ptr,&p,sizeof p);
    VkDescriptorBufferInfo buffers[]={{vertices,0,vertex_bytes},{s_ray_ao.indices.buf,0,(VkDeviceSize)epoch->index_count/3*16},{s_dlss_frame.clips.buf,0,s_dlss_frame.clips.size},{s_dlss_frame.draws[bank].buf,0,VK_DLSS_ROWS*16u},{s_dlss_frame.params[bank].buf,0,sizeof p}};
    VkDescriptorImageInfo images[]={{s_dlss_frame.sampler,s_eobj[source].im.view,VK_IMAGE_LAYOUT_GENERAL},{s_dlss_frame.sampler,s_eobj[depth].im.view,VK_IMAGE_LAYOUT_GENERAL},
        {0,s_dlss_frame.color.view,VK_IMAGE_LAYOUT_GENERAL},{0,s_dlss_frame.depth.view,VK_IMAGE_LAYOUT_GENERAL},{0,s_dlss_frame.motion.view,VK_IMAGE_LAYOUT_GENERAL},{0,s_dlss_frame.bias.view,VK_IMAGE_LAYOUT_GENERAL},
        {s_dlss_frame.sampler,s_dlss_frame.output.view,VK_IMAGE_LAYOUT_GENERAL},{0,s_eobj[s_dlss_frame.expanded].im.view,VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[16];memset(writes,0,sizeof writes);u32 wi=0;
    for(u32 i=0;i<2;i++)writes[wi++]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=s_dlss_frame.set,.dstBinding=i,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&images[i]};
    for(u32 i=0;i<4;i++)writes[wi++]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=s_dlss_frame.set,.dstBinding=2+i,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&buffers[i]};
    VkWriteDescriptorSetAccelerationStructureKHR as={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,.accelerationStructureCount=1,.pAccelerationStructures=&s_ray_ao.tlas};
    writes[wi++]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.pNext=&as,.dstSet=s_dlss_frame.set,.dstBinding=6,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};
    writes[wi++]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=s_dlss_frame.set,.dstBinding=7,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.pBufferInfo=&buffers[4]};
    for(u32 i=0;i<6;i++)writes[wi++]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=s_dlss_frame.set,.dstBinding=8+i,.descriptorCount=1,.descriptorType=i==4?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,.pImageInfo=&images[2+i]};
    VkDescriptorBufferInfo gathered={s_ray_ao.triangles.buf,0,(VkDeviceSize)(epoch->index_count/3u)*36u};
    writes[wi++]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=s_dlss_frame.set,.dstBinding=14,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&gathered};
    VkDescriptorImageInfo layer_image={s_dlss_frame.sampler,s_dlss_layer.image.view,VK_IMAGE_LAYOUT_GENERAL};
    writes[wi++]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=s_dlss_frame.set,.dstBinding=15,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&layer_image};
    pvkUpdateDescriptorSets(s_vk.device,wi,writes,0,NULL);
    vk_image *input[]={&s_dlss_frame.color,&s_dlss_frame.depth,&s_dlss_frame.motion,&s_dlss_frame.bias};
    if(s_dlss_frame.completed)for(u32 i=0;i<4;i++)vk_dlss_layout(input[i],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
    vk_ray_memory(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
    pvkCmdBindPipeline(s_vk.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s_dlss_frame.pipeline);pvkCmdBindDescriptorSets(s_vk.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s_dlss_frame.pipeline_layout,0,1,&s_dlss_frame.set,0,NULL);
    u32 phase=0;pvkCmdPushConstants(s_vk.cmd,s_dlss_frame.pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&phase);s_ray_ao.dispatch(s_vk.cmd,(iw+7)/8,(ih+7)/8,1);
    vk_ray_memory(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_WRITE_BIT);
    phase=1;pvkCmdPushConstants(s_vk.cmd,s_dlss_frame.pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&phase);s_ray_ao.dispatch(s_vk.cmd,128,(epoch->vertex_count+8191)/8192,1);
    for(u32 i=0;i<4;i++)vk_dlss_layout(input[i],VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vk_ray_stamp(7,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT); /* inputs and motion ready; NGX follows */
    u64 ticks=SDL_GetPerformanceCounter();float dt=s_dlss_frame.ticks?(float)(1000.0*(ticks-s_dlss_frame.ticks)/SDL_GetPerformanceFrequency()):33.333f;
    if(s_fsr3.context){
        rsx_fsr3_frame f;memset(&f,0,sizeof f);
        f.config_generation=1;f.frame_serial=f.motion_frame_serial=epoch->frame;f.motion_provenance=RSX_FSR3_MOTION_CAMERA_AND_OBJECT_UNJITTERED;
        f.render_width=iw;f.render_height=ih;
        f.color=vk_fsr3_image(&s_dlss_frame.color,VK_FORMAT_R16G16B16A16_SFLOAT,iw,ih,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        f.depth=vk_fsr3_image(&s_dlss_frame.depth,VK_FORMAT_R32_SFLOAT,iw,ih,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        f.motion=vk_fsr3_image(&s_dlss_frame.motion,VK_FORMAT_R32G32_SFLOAT,iw,ih,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        f.output=vk_fsr3_image(&s_dlss_frame.output,VK_FORMAT_R16G16B16A16_SFLOAT,ow,oh,VK_IMAGE_LAYOUT_GENERAL);
        if(vk_fsr3_reactive())f.reactive=vk_fsr3_image(&s_dlss_frame.bias,VK_FORMAT_R8_UNORM,iw,ih,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        f.jitter_x=p.jitter[0];f.jitter_y=p.jitter[1];f.delta_milliseconds=fmaxf(.01f,dt);f.pre_exposure=1;
        vk_fsr3_camera(projection,&f.camera_near,&f.camera_far,&f.vertical_fov_radians);
        f.sharpness=vk_fsr3_sharpness();f.reset=reset;
        rsx_fsr3_status fsr_status=s_fsr3.evaluate(s_fsr3.context,s_vk.cmd,&f);
        if(fsr_status!=RSX_FSR3_OK){
            VK_LOG("FSR3 evaluate rejected status=%d\n",fsr_status);s_dlss_frame.failed=1;return UINT32_MAX;
        }
    }else{
    rsx_dlss_frame frame;memset(&frame,0,sizeof frame);frame.config_generation=1;frame.frame_serial=epoch->frame;frame.motion_frame_serial=epoch->frame;frame.motion_provenance=RSX_DLSS_MOTION_CAMERA_AND_OBJECT_UNJITTERED;
    frame.color=vk_dlss_image(&s_dlss_frame.color,VK_FORMAT_R16G16B16A16_SFLOAT,iw,ih,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    frame.depth=vk_dlss_image(&s_dlss_frame.depth,VK_FORMAT_R32_SFLOAT,iw,ih,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    frame.motion=vk_dlss_image(&s_dlss_frame.motion,VK_FORMAT_R32G32_SFLOAT,iw,ih,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    frame.bias=vk_dlss_image(&s_dlss_frame.bias,VK_FORMAT_R8_UNORM,iw,ih,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    frame.output=vk_dlss_image(&s_dlss_frame.output,VK_FORMAT_R16G16B16A16_SFLOAT,ow,oh,VK_IMAGE_LAYOUT_GENERAL);
    frame.jitter_x=p.jitter[0];frame.jitter_y=p.jitter[1];frame.delta_milliseconds=fmaxf(.01f,dt);frame.pre_exposure=frame.exposure_scale=1;frame.reset=reset;
    rsx_dlss_status status=s_dlss.loader.evaluate(s_dlss.context,s_vk.cmd,&frame);
    if(status!=RSX_DLSS_OK){
        /* NGX may have recorded part of a command; the caller discards the
         * whole transport recording and this session stops using DLSS. */
        VK_LOG("DLSS evaluate rejected status=%d\n",status);s_dlss_frame.failed=1;return UINT32_MAX;
    }
    }
    vk_ray_memory(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
    pvkCmdBindPipeline(s_vk.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s_dlss_frame.pipeline);pvkCmdBindDescriptorSets(s_vk.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s_dlss_frame.pipeline_layout,0,1,&s_dlss_frame.set,0,NULL);
    phase=2;pvkCmdPushConstants(s_vk.cmd,s_dlss_frame.pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&phase);s_ray_ao.dispatch(s_vk.cmd,(ow*2u+7)/8,(oh+7)/8,1);
    vk_ray_memory(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT);
    s_dlss_frame.recorded=1;s_dlss_frame.record_frame=epoch->frame;s_dlss_frame.record_ticks=ticks;s_dlss_frame.record_reset=reset;
    s_dlss_frame.current_count=draws;
    memcpy(s_dlss_frame.record_camera,camera,sizeof s_dlss_frame.record_camera);s_dlss_frame.record_have_camera=invertible;s_dlss_frame.record_camera_key=camera_key;
    return vk_eng_surface_view(NULL,s_dlss_frame.expanded,remap,format);
}
/* Next evaluation starts a new DLSS history (its input content changed). */
static void vk_dlss_history_reset(void){s_dlss_frame.completed=0;}
/* Camera-fallback pixels of the frame just completed, by why its owner draw
 * had no previous-frame mapping: pixels[] and draws[] are noIdentity (no
 * geometry/slot identity), duplicateKey (key repeats this frame, so matching
 * skips it), unmatched (no unique previous key: new, changed or count
 * differs), matchedInvalid (mapped, rejected per pixel); pixels[4] is
 * pixels with no owner hit or an owner beyond the mapped draws. */
static void vk_dlss_fallback_attribution(u32 pixels[5],u32 draws[4])
{
    const u32 (*rows)[4]=s_dlss_frame.draws[s_dlss_frame.bank].ptr;
    const vk_dlss_draw_key *k=s_dlss_frame.current;const u32 n=s_dlss_frame.current_count;
    memset(pixels,0,5*sizeof *pixels);memset(draws,0,4*sizeof *draws);
    for(u32 i=0;i<n;i++){
        const u32 count=rows[i][3];if(!count)continue;
        u32 category=3;
        if(!k[i].geometry || !k[i].slots)category=0;
        else if(!rows[i][2]){
            category=2;
            for(u32 j=0;j<n;j++)if(j!=i && k[j].geometry==k[i].geometry && k[j].slots==k[i].slots && k[j].topology==k[i].topology){category=1;break;}
        }
        pixels[category]+=count;draws[category]++;
    }
    pixels[4]=rows[VK_DLSS_DRAWS+1][3];
}
/* After the transport's submission: commit history state and diagnostics. */
static void vk_dlss_complete(int submitted,u32 source)
{
    if(!s_dlss_frame.recorded)return;
    s_dlss_frame.recorded=0;
    if(!submitted){s_dlss_frame.completed=0;return;}
    /* This frame's submission has completed (synchronous wait), so its
     * draw rows are final; read them before the bank flips. */
    const int log_due=!s_dlss_frame.last_log || s_dlss_frame.record_ticks-s_dlss_frame.last_log>=SDL_GetPerformanceFrequency();
    u32 fallback_pixels[5]={0},fallback_draws[4]={0};
    if(log_due)vk_dlss_fallback_attribution(fallback_pixels,fallback_draws);
    const u32 reactive_pixels=log_due?((const u32 (*)[4])s_dlss_frame.draws[s_dlss_frame.bank].ptr)[VK_DLSS_DRAWS+2][0]:0u;
    s_dlss_frame.completed++;s_dlss_frame.frame=s_dlss_frame.record_frame;s_dlss_frame.ticks=s_dlss_frame.record_ticks;s_dlss_frame.bank=1-s_dlss_frame.bank;
    memcpy(s_dlss_frame.previous,s_dlss_frame.current,sizeof s_dlss_frame.current);s_dlss_frame.previous_count=s_dlss_frame.current_count;
    memcpy(s_dlss_frame.camera,s_dlss_frame.record_camera,sizeof s_dlss_frame.camera);s_dlss_frame.have_camera=s_dlss_frame.record_have_camera;s_dlss_frame.camera_key=s_dlss_frame.record_camera_key;
    const u64 ticks=s_dlss_frame.record_ticks;
    if(log_due){
        const rsx_dlss_motion_counts c=s_dlss_frame.last_motion;
        const rsx_dlss_motion_stats coverage=rsx_dlss_summarize_motion(c,s_dlss_frame.in_w,s_dlss_frame.in_h);
        VK_LOG("%s SR %s frame=%llu input=%ux%u output=%ux%u completedEvaluations=%llu motionCounterFrame=%llu matchedMotionPixels=%u cameraFallbackPixels=%u reset=%d nonfinite=%u worldReplacement=1 HUD=guest-composite\n",
            vk_sr_name(),s_fsr3.context?vk_fsr3_mode_name(s_fsr3.quality):vk_dlss_quality_name(s_dlss_frame.quality),(unsigned long long)s_dlss_frame.frame,s_dlss_frame.in_w,s_dlss_frame.in_h,s_dlss_frame.out_w,s_dlss_frame.out_h,
            (unsigned long long)s_dlss_frame.completed,(unsigned long long)s_dlss_frame.motion_counter_frame,c.matched,c.fallback,s_dlss_frame.record_reset,s_dlss_frame.last_nonfinite);
        VK_LOG("%s motion coverage countersReady=%d counterFrame=%llu valid=%d available=%d eligible=%llu matched=%u cameraFallback=%u background=%u resetExcluded=%u disoccluded=%u matchedPercent=%.3f fallbackPercent=%.3f\n",
            vk_sr_name(),s_dlss_frame.have_motion_counters,(unsigned long long)s_dlss_frame.motion_counter_frame,
            s_dlss_frame.have_motion_counters && coverage.valid,s_dlss_frame.have_motion_counters && coverage.coverage_available,
            (unsigned long long)coverage.eligible,c.matched,c.fallback,c.background,c.reset,c.disoccluded,
            coverage.matched_percent,coverage.fallback_percent);
        VK_LOG("%s fallback attribution frame=%llu mappedDraws=%u pixels/draws noIdentity=%u/%u duplicateKey=%u/%u unmatched=%u/%u matchedInvalid=%u/%u noOwner=%u reactiveLayerPixels=%u layerSnapshots=%u\n",
            vk_sr_name(),(unsigned long long)s_dlss_frame.frame,s_dlss_frame.current_count,fallback_pixels[0],fallback_draws[0],fallback_pixels[1],fallback_draws[1],
            fallback_pixels[2],fallback_draws[2],fallback_pixels[3],fallback_draws[3],fallback_pixels[4],reactive_pixels,s_dlss_layer.frame_snapshots);
        s_dlss_frame.last_log=ticks;
    }
    const char *pair=getenv("PS3RECOMP_DLSS_PAIR"),*request=getenv("PS3RECOMP_DLSS_PAIR_REQUEST");
    if(pair && request && source){FILE *f=fopen(request,"rb");if(f){fclose(f);remove(request);
        vk_eng_obj *before=&s_eobj[source],*after=&s_eobj[s_dlss_frame.expanded];
        u8 *pixels=(u8*)malloc((size_t)after->w*after->h*4);
        if(pixels){int ok=!vk_eng_readback_rows_raw(source,0,0,before->w,before->h,pixels,before->w*4) && vk_ray_pair_ppm(pair,"off",pixels,before->w,before->h);
            ok=ok && !vk_eng_readback_rows_raw(s_dlss_frame.expanded,0,0,after->w,after->h,pixels,after->w*4) && vk_ray_pair_ppm(pair,"on",pixels,after->w,after->h);
            VK_LOG("DLSS same-frame world comparison captured=%d frame=%llu\n",ok,(unsigned long long)s_dlss_frame.frame);free(pixels);}
    }}
}
static void vk_dlss_frame_destroy(void)
{
    vk_ray_destroy_buffer(&s_dlss_frame.clips);
    for(u32 b=0;b<2;b++){vk_ray_destroy_buffer(&s_dlss_frame.draws[b]);vk_ray_destroy_buffer(&s_dlss_frame.params[b]);}
    vk_destroy_image(&s_dlss_layer.image);s_dlss_layer.valid=0;s_dlss_layer.w=s_dlss_layer.h=0;
    vk_destroy_image(&s_dlss_frame.color);vk_destroy_image(&s_dlss_frame.depth);vk_destroy_image(&s_dlss_frame.motion);vk_destroy_image(&s_dlss_frame.bias);vk_destroy_image(&s_dlss_frame.output);
    if(s_dlss_frame.expanded)vk_eng_release(NULL,s_dlss_frame.expanded);
    if(s_dlss_frame.pipeline)pvkDestroyPipeline(s_vk.device,s_dlss_frame.pipeline,NULL);
    if(s_dlss_frame.sampler)pvkDestroySampler(s_vk.device,s_dlss_frame.sampler,NULL);
    if(s_dlss_frame.pool)pvkDestroyDescriptorPool(s_vk.device,s_dlss_frame.pool,NULL);
    if(s_dlss_frame.pipeline_layout)pvkDestroyPipelineLayout(s_vk.device,s_dlss_frame.pipeline_layout,NULL);
    if(s_dlss_frame.layout)pvkDestroyDescriptorSetLayout(s_vk.device,s_dlss_frame.layout,NULL);
    memset(&s_dlss_frame,0,sizeof s_dlss_frame);
}
#else
/* NGX is Windows-only here: no DLSS world, no jitter. */
static int vk_dlss_world_extent(u32 *w,u32 *h){(void)w;(void)h;return 0;}
static void vk_dlss_jitter(float out[2]){out[0]=out[1]=0;}
static void vk_dlss_jitter_upload(void *constants,size_t bytes){(void)constants;(void)bytes;}
static int vk_dlss_prepare(u32 vertices,VkFormat world_format){(void)vertices;(void)world_format;return 0;}
static u32 vk_dlss_record(u32 source,u32 depth,VkBuffer vertices,VkDeviceSize vertex_bytes,const rsx_scene_epoch *epoch,
                          const float projection[16],const float camera[16],int have_camera,u64 camera_key,u32 remap,u32 format)
{(void)source;(void)depth;(void)vertices;(void)vertex_bytes;(void)epoch;(void)projection;(void)camera;(void)have_camera;(void)camera_key;(void)remap;(void)format;return 0;}
static void vk_dlss_complete(int submitted,u32 source){(void)submitted;(void)source;}
static void vk_dlss_history_reset(void){}
static void vk_dlss_reactive_before_draw(u32 target,const vk_eng_pipe *pipe){(void)target;(void)pipe;}
#endif
