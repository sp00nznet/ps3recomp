/* Same-device world SR. Called after hybrid transport and before guest HUD. */
typedef struct vk_dlss_draw_key {u64 geometry,slots,topology;u32 base,count;} vk_dlss_draw_key;
typedef struct vk_dlss_frame_params {float reprojection[16],projection[4],jitter[4];u32 sizes[4],history[4];} vk_dlss_frame_params;
static struct {
    int initialized,failed,configured,have_camera;u32 expanded,previous_count,bank;
    u64 frame,completed,last_log,camera_key,ticks;float camera[16];
    vk_dlss_draw_key previous[256];
    vk_image color,depth,motion,bias,output;
    vk_buffer clips,draws,params;
    VkDescriptorSetLayout layout;VkPipelineLayout pipeline_layout;
    VkDescriptorPool pool;VkDescriptorSet set;VkPipeline pipeline;VkSampler sampler;
} s_dlss_frame;
static float vk_dlss_halton(u64 index,u32 base){float value=0,f=1;while(index){f/=base;value+=f*(index%base);index/=base;}return value;}
static void vk_dlss_jitter(float out[2])
{
    out[0]=out[1]=0;
    if(!s_dlss.context || s_dlss_frame.failed)return;
    u64 index=s_rt_capture.frame%8+1;
    out[0]=vk_dlss_halton(index,2)-.5f;out[1]=vk_dlss_halton(index,3)-.5f;
}
static void vk_dlss_jitter_upload(void *constants,size_t bytes)
{
    if(bytes<514*16 || !s_eng_bound.nrt)return;
    const vk_eng_obj *o=&s_eobj[s_eng_bound.rt[0]];
    if(o->guest_w!=1280 || o->guest_h!=720)return;
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
    if(s_dlss_frame.failed || !s_dlss.context)return 0;
    VkDescriptorSetLayoutBinding bindings[14];
    for(u32 i=0;i<14;i++)bindings[i]=(VkDescriptorSetLayoutBinding){i,
        i==0||i==1||i==12?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        i>=2&&i<=5?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:i==6?VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
        i==7?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL};
    VkDescriptorSetLayoutCreateInfo layout={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=14,.pBindings=bindings};
    if(pvkCreateDescriptorSetLayout(s_vk.device,&layout,NULL,&s_dlss_frame.layout)!=VK_SUCCESS)goto fail;
    VkPushConstantRange push={VK_SHADER_STAGE_COMPUTE_BIT,0,4};
    VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&s_dlss_frame.layout,.pushConstantRangeCount=1,.pPushConstantRanges=&push};
    if(pvkCreatePipelineLayout(s_vk.device,&pl,NULL,&s_dlss_frame.pipeline_layout)!=VK_SUCCESS)goto fail;
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,3},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,4},{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,1},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,5}};
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
static int vk_dlss_frame_images(void)
{
    if(s_dlss_frame.expanded)return 1;
    VkImageUsageFlags use=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if(vk_create_image(&s_dlss_frame.color,VK_FORMAT_R16G16B16A16_SFLOAT,1707,960,use,VK_IMAGE_ASPECT_COLOR_BIT)||
       vk_create_image(&s_dlss_frame.depth,VK_FORMAT_R32_SFLOAT,1707,960,use,VK_IMAGE_ASPECT_COLOR_BIT)||
       vk_create_image(&s_dlss_frame.motion,VK_FORMAT_R32G32_SFLOAT,1707,960,use,VK_IMAGE_ASPECT_COLOR_BIT)||
       vk_create_image(&s_dlss_frame.bias,VK_FORMAT_R8_UNORM,1707,960,use,VK_IMAGE_ASPECT_COLOR_BIT)||
       vk_create_image(&s_dlss_frame.output,VK_FORMAT_R16G16B16A16_SFLOAT,2560,1440,use,VK_IMAGE_ASPECT_COLOR_BIT))return 0;
    u32 expanded=vk_eng_alloc();if(!expanded)return 0;
    vk_eng_obj *o=&s_eobj[expanded];
    if(vk_create_image(&o->im,VK_FORMAT_R8G8B8A8_UNORM,5120,1440,use,VK_IMAGE_ASPECT_COLOR_BIT))return 0;
    o->kind=VK_ENG_COLOR;o->fmt=VK_FORMAT_R8G8B8A8_UNORM;o->bpp=4;o->w=5120;o->h=1440;o->scale=2;o->grid_x=2;o->grid_y=1;
    s_dlss_frame.expanded=expanded;
    if(!vk_ray_buffer(&s_dlss_frame.clips,(524288ull+1707*960)*16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,0,0)||
       !vk_ray_buffer(&s_dlss_frame.draws,258*16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,1,0)||
       !vk_ray_buffer(&s_dlss_frame.params,sizeof(vk_dlss_frame_params),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,1,0))return 0;
    if(vk_begin())return 0;
    vk_image *images[]={&s_dlss_frame.color,&s_dlss_frame.depth,&s_dlss_frame.motion,&s_dlss_frame.bias,&s_dlss_frame.output,&o->im};
    for(u32 i=0;i<6;i++)vk_barrier_image(images[i]->img,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED);
    s_ray_ao.fill_buffer(s_vk.cmd,s_dlss_frame.clips.buf,0,s_dlss_frame.clips.size,0);
    return !vk_submit_and_wait();
}
static rsx_dlss_image vk_dlss_image(vk_image *im,VkFormat format,u32 w,u32 h,VkImageLayout layout)
{return (rsx_dlss_image){im->img,im->view,format,w,h,{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},layout};}
static u32 vk_dlss_world(u32 source,u32 depth,VkBuffer vertices,const rsx_scene_epoch *epoch,const float projection[16],u32 remap,u32 format)
{
    if(!s_dlss.context || s_dlss_frame.failed || !epoch || s_eobj[source].w!=3414 || s_eobj[source].h!=960)return 0;
    if(!vk_dlss_frame_initialize()||!vk_dlss_frame_images())goto fail;
    if(!s_dlss_frame.configured){
        if(vk_begin())goto fail;
        rsx_dlss_config config={1,1707,960,2560,1440,RSX_DLSS_QUALITY,1};
        if(s_dlss.loader.configure(s_dlss.context,s_vk.cmd,&config)!=RSX_DLSS_OK){vk_submit_and_wait();goto fail;}
        if(vk_submit_and_wait())goto fail;
        s_dlss_frame.configured=1;
    }
    vk_dlss_frame_params p={0};p.projection[0]=projection[0];p.projection[1]=projection[5];p.projection[2]=projection[10];p.projection[3]=projection[14];
    vk_dlss_jitter(p.jitter);p.sizes[0]=1707;p.sizes[1]=960;p.sizes[2]=epoch->vertex_count;
    p.history[0]=s_dlss_frame.bank*262144u;p.history[1]=(1-s_dlss_frame.bank)*262144u;
    int reset=!s_dlss_frame.completed || epoch->frame!=s_dlss_frame.frame+1;
    vk_dlss_draw_key current[256];memset(current,0,sizeof current);
    u32 (*map)[4]=s_dlss_frame.draws.ptr;memset(map,0,258*16);
    float camera[16]={0},inverse[16];int have_camera=0;u64 camera_key=0;u32 base=0;
    for(u32 i=0;i<epoch->draw_count;i++){
        rsx_scene_draw_input draw;if(!rsx_scene_epoch_get(epoch,i,&draw))goto fail;
        u64 topology=1469598103934665603ull;
        for(u32 k=0;k<draw.index_count;k++)topology=(topology^draw.indices[k])*1099511628211ull;
        topology=(topology^draw.pipeline)*1099511628211ull;
        current[i]=(vk_dlss_draw_key){draw.geometry_identity,draw.constant_slots,topology,base,draw.vertex_count};
        map[i][1]=base;u32 matches=0,found=0,duplicates=0;
        if(draw.geometry_identity && draw.constant_slots && !reset){
            for(u32 j=0;j<s_dlss_frame.previous_count;j++)if(s_dlss_frame.previous[j].geometry==draw.geometry_identity && s_dlss_frame.previous[j].slots==draw.constant_slots && s_dlss_frame.previous[j].topology==topology && s_dlss_frame.previous[j].count==draw.vertex_count){matches++;found=j;}
            for(u32 j=0;j<i;j++)if(current[j].geometry==draw.geometry_identity && current[j].slots==draw.constant_slots && current[j].topology==topology){duplicates++;map[j][2]=0;}
            if(matches==1 && !duplicates){map[i][0]=s_dlss_frame.previous[found].base;map[i][2]=1;}
        }
        base+=draw.vertex_count;
        const rsx_authored_lights *light=&s_ray_ao.authored_lights[i];
        if(!have_camera && light->contract_id==2 && light->lights[0].status!=RSX_AUTHORED_UNAVAILABLE){
            for(u32 c=0;c<4;c++){
                camera[c]=projection[0]*light->light_to_view[0][c];
                camera[4+c]=projection[5]*light->light_to_view[1][c];
                camera[8+c]=projection[10]*light->light_to_view[2][c]+(c==3?projection[14]:0);
                camera[12+c]=light->light_to_view[2][c];
            }
            camera_key=draw.geometry_identity;have_camera=vk_dlss_inverse(camera,inverse);
        }
    }
    if(!have_camera || !s_dlss_frame.have_camera || camera_key!=s_dlss_frame.camera_key)reset=1;
    if(have_camera && s_dlss_frame.have_camera){
        float maximum=0;for(u32 i=0;i<16;i++)maximum=fmaxf(maximum,fabsf(camera[i]-s_dlss_frame.camera[i])/fmaxf(1,fabsf(s_dlss_frame.camera[i])));
        if(maximum>.35f)reset=1;
        for(u32 r=0;r<4;r++)for(u32 c=0;c<4;c++){float value=0;for(u32 k=0;k<4;k++)value+=s_dlss_frame.camera[r*4+k]*inverse[k*4+c];p.reprojection[c*4+r]=value;}
    }else for(u32 i=0;i<4;i++)p.reprojection[i*5]=1;
    p.sizes[3]=reset;memcpy(s_dlss_frame.params.ptr,&p,sizeof p);
    VkDescriptorBufferInfo buffers[]={{vertices,0,(VkDeviceSize)epoch->vertex_count*224},{s_ray_ao.indices.buf,0,(VkDeviceSize)epoch->index_count/3*16},{s_dlss_frame.clips.buf,0,s_dlss_frame.clips.size},{s_dlss_frame.draws.buf,0,258*16},{s_dlss_frame.params.buf,0,sizeof p}};
    VkDescriptorImageInfo images[]={{s_dlss_frame.sampler,s_eobj[source].im.view,VK_IMAGE_LAYOUT_GENERAL},{s_dlss_frame.sampler,s_eobj[depth].im.view,VK_IMAGE_LAYOUT_GENERAL},
        {0,s_dlss_frame.color.view,VK_IMAGE_LAYOUT_GENERAL},{0,s_dlss_frame.depth.view,VK_IMAGE_LAYOUT_GENERAL},{0,s_dlss_frame.motion.view,VK_IMAGE_LAYOUT_GENERAL},{0,s_dlss_frame.bias.view,VK_IMAGE_LAYOUT_GENERAL},
        {s_dlss_frame.sampler,s_dlss_frame.output.view,VK_IMAGE_LAYOUT_GENERAL},{0,s_eobj[s_dlss_frame.expanded].im.view,VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[14];memset(writes,0,sizeof writes);u32 wi=0;
    for(u32 i=0;i<2;i++)writes[wi++]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=s_dlss_frame.set,.dstBinding=i,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&images[i]};
    for(u32 i=0;i<4;i++)writes[wi++]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=s_dlss_frame.set,.dstBinding=2+i,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&buffers[i]};
    VkWriteDescriptorSetAccelerationStructureKHR as={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,.accelerationStructureCount=1,.pAccelerationStructures=&s_ray_ao.tlas};
    writes[wi++]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.pNext=&as,.dstSet=s_dlss_frame.set,.dstBinding=6,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};
    writes[wi++]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=s_dlss_frame.set,.dstBinding=7,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.pBufferInfo=&buffers[4]};
    for(u32 i=0;i<6;i++)writes[wi++]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=s_dlss_frame.set,.dstBinding=8+i,.descriptorCount=1,.descriptorType=i==4?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,.pImageInfo=&images[2+i]};
    pvkUpdateDescriptorSets(s_vk.device,wi,writes,0,NULL);
    if(vk_begin())goto fail;
    vk_image *input[]={&s_dlss_frame.color,&s_dlss_frame.depth,&s_dlss_frame.motion,&s_dlss_frame.bias};
    if(s_dlss_frame.completed)for(u32 i=0;i<4;i++)vk_dlss_layout(input[i],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
    vk_ray_memory(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
    pvkCmdBindPipeline(s_vk.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s_dlss_frame.pipeline);pvkCmdBindDescriptorSets(s_vk.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s_dlss_frame.pipeline_layout,0,1,&s_dlss_frame.set,0,NULL);
    u32 phase=0;pvkCmdPushConstants(s_vk.cmd,s_dlss_frame.pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&phase);s_ray_ao.dispatch(s_vk.cmd,(1707+7)/8,120,1);
    vk_ray_memory(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_WRITE_BIT);
    phase=1;pvkCmdPushConstants(s_vk.cmd,s_dlss_frame.pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&phase);s_ray_ao.dispatch(s_vk.cmd,128,(epoch->vertex_count+8191)/8192,1);
    for(u32 i=0;i<4;i++)vk_dlss_layout(input[i],VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vk_ray_memory(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
    if(vk_submit_and_wait())goto fail;
    if(map[256][0] && map[256][1]>map[256][0]/2)reset=1;
    u64 ticks=SDL_GetPerformanceCounter();float dt=s_dlss_frame.ticks?(float)(1000.0*(ticks-s_dlss_frame.ticks)/SDL_GetPerformanceFrequency()):33.333f;
    rsx_dlss_frame frame={0};frame.config_generation=1;frame.frame_serial=epoch->frame;frame.motion_frame_serial=epoch->frame;frame.motion_provenance=RSX_DLSS_MOTION_CAMERA_AND_OBJECT_UNJITTERED;
    frame.color=vk_dlss_image(&s_dlss_frame.color,VK_FORMAT_R16G16B16A16_SFLOAT,1707,960,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    frame.depth=vk_dlss_image(&s_dlss_frame.depth,VK_FORMAT_R32_SFLOAT,1707,960,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    frame.motion=vk_dlss_image(&s_dlss_frame.motion,VK_FORMAT_R32G32_SFLOAT,1707,960,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    frame.bias=vk_dlss_image(&s_dlss_frame.bias,VK_FORMAT_R8_UNORM,1707,960,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    frame.output=vk_dlss_image(&s_dlss_frame.output,VK_FORMAT_R16G16B16A16_SFLOAT,2560,1440,VK_IMAGE_LAYOUT_GENERAL);
    frame.jitter_x=p.jitter[0];frame.jitter_y=p.jitter[1];frame.delta_milliseconds=fmaxf(.01f,dt);frame.pre_exposure=frame.exposure_scale=1;frame.reset=reset;
    if(vk_begin())goto fail;
    rsx_dlss_status status=s_dlss.loader.evaluate(s_dlss.context,s_vk.cmd,&frame);
    if(status!=RSX_DLSS_OK){VK_LOG("DLSS evaluate rejected status=%d\n",status);pvkResetCommandBuffer(s_vk.cmd,0);s_batch.recording=0;s_batch.pending=0;goto fail;}
    vk_ray_memory(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
    pvkCmdBindPipeline(s_vk.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s_dlss_frame.pipeline);pvkCmdBindDescriptorSets(s_vk.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s_dlss_frame.pipeline_layout,0,1,&s_dlss_frame.set,0,NULL);
    phase=2;pvkCmdPushConstants(s_vk.cmd,s_dlss_frame.pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&phase);s_ray_ao.dispatch(s_vk.cmd,640,180,1);
    vk_ray_memory(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
    if(vk_submit_and_wait())goto fail;
    s_dlss_frame.completed++;s_dlss_frame.frame=epoch->frame;s_dlss_frame.ticks=ticks;s_dlss_frame.bank=1-s_dlss_frame.bank;
    if(map[257][0]){VK_LOG("DLSS output nonfinite samples=%u; output rejected\n",map[257][0]);goto fail;}
    memcpy(s_dlss_frame.previous,current,sizeof current);s_dlss_frame.previous_count=epoch->draw_count;
    memcpy(s_dlss_frame.camera,camera,sizeof camera);s_dlss_frame.have_camera=have_camera;s_dlss_frame.camera_key=camera_key;
    if(!s_dlss_frame.last_log || ticks-s_dlss_frame.last_log>=SDL_GetPerformanceFrequency()){
        VK_LOG("DLSS SR frame=%llu input=1707x960 output=2560x1440 completedEvaluations=%llu matchedMotionPixels=%u cameraFallbackPixels=%u reset=%d worldReplacement=1 HUD=output-resolution\n",(unsigned long long)epoch->frame,(unsigned long long)s_dlss_frame.completed,map[256][2],map[256][3],reset);s_dlss_frame.last_log=ticks;
    }
    const char *pair=getenv("PS3RECOMP_DLSS_PAIR"),*request=getenv("PS3RECOMP_DLSS_PAIR_REQUEST");
    if(pair && request){FILE *f=fopen(request,"rb");if(f){fclose(f);remove(request);
        vk_eng_obj *before=&s_eobj[source],*after=&s_eobj[s_dlss_frame.expanded];
        u8 *pixels=(u8*)malloc((size_t)after->w*after->h*4);
        if(pixels){int ok=!vk_eng_readback_rows_raw(source,0,0,before->w,before->h,pixels,before->w*4) && vk_ray_pair_ppm(pair,"off",pixels,before->w,before->h);
            ok=ok && !vk_eng_readback_rows_raw(s_dlss_frame.expanded,0,0,after->w,after->h,pixels,after->w*4) && vk_ray_pair_ppm(pair,"on",pixels,after->w,after->h);
            VK_LOG("DLSS same-frame world comparison captured=%d frame=%llu\n",ok,(unsigned long long)epoch->frame);free(pixels);}
    }}
    return vk_eng_surface_view(NULL,s_dlss_frame.expanded,remap,format);
fail:
    s_dlss_frame.failed=1;VK_LOG("DLSS SR frame path failed; raster/PT retained, evaluations=%llu\n",(unsigned long long)s_dlss_frame.completed);return 0;
}
static void vk_dlss_frame_destroy(void)
{
    vk_ray_destroy_buffer(&s_dlss_frame.clips);vk_ray_destroy_buffer(&s_dlss_frame.draws);vk_ray_destroy_buffer(&s_dlss_frame.params);
    vk_destroy_image(&s_dlss_frame.color);vk_destroy_image(&s_dlss_frame.depth);vk_destroy_image(&s_dlss_frame.motion);vk_destroy_image(&s_dlss_frame.bias);vk_destroy_image(&s_dlss_frame.output);
    if(s_dlss_frame.expanded)vk_eng_release(NULL,s_dlss_frame.expanded);
    if(s_dlss_frame.pipeline)pvkDestroyPipeline(s_vk.device,s_dlss_frame.pipeline,NULL);
    if(s_dlss_frame.sampler)pvkDestroySampler(s_vk.device,s_dlss_frame.sampler,NULL);
    if(s_dlss_frame.pool)pvkDestroyDescriptorPool(s_vk.device,s_dlss_frame.pool,NULL);
    if(s_dlss_frame.pipeline_layout)pvkDestroyPipelineLayout(s_vk.device,s_dlss_frame.pipeline_layout,NULL);
    if(s_dlss_frame.layout)pvkDestroyDescriptorSetLayout(s_vk.device,s_dlss_frame.layout,NULL);
    memset(&s_dlss_frame,0,sizeof s_dlss_frame);
}
