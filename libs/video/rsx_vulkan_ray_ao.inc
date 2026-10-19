/* Explicit live current-frame ray-query AO consumer. Include after scene_batch
 * and rt_capture, with embedded rsx_scene_{gather,trace,alpha}_glsl strings.
 * No file scene, CPU position readback, second device, or automatic fallback AO.
 * One synchronous integration submission initially; source snapshot helpers can
 * have their existing synchronous preparation boundaries. No performance claim.
 */
#ifndef RSX_VULKAN_RAY_AO_INC
#define RSX_VULKAN_RAY_AO_INC
#include "rsx_scene_shader_sources.h"
#define VK_RAY_TEXTURES 32u
#define VK_RAY_MAX_ALLOCATION (256u*1024u*1024u)
typedef struct vk_ray_material {u32 alpha[4],raster[4],textures[4];} vk_ray_material;
typedef struct vk_ray_parameters {float projection[4];u32 dims[4],counts[4];float settings[4];} vk_ray_parameters;
_Static_assert(sizeof(vk_ray_material)==48,"GLSL std430 material layout");
_Static_assert(sizeof(vk_ray_parameters)==64 && offsetof(vk_ray_parameters,settings)==48,"GLSL std140 parameters layout");
static struct {
    int ready,failed,warned;u32 color,cw,ch,aw,ah;VkFormat format;u64 device_generation;
    VkDescriptorSetLayout layout;VkPipelineLayout pipeline_layout;VkDescriptorPool pool;VkDescriptorSet set;
    VkPipeline gather,trace[2];VkAccelerationStructureKHR blas,tlas;
    vk_buffer indices,materials,fp,params,triangles,ao,instances,blas_store,tlas_store,bscratch,tscratch,flag_readback;
    PFN_vkCreateComputePipelines create_compute;PFN_vkCmdDispatch dispatch;
    PFN_vkGetBufferDeviceAddress address;PFN_vkCreateAccelerationStructureKHR create_as;
    PFN_vkDestroyAccelerationStructureKHR destroy_as;PFN_vkGetAccelerationStructureBuildSizesKHR sizes;
    PFN_vkGetAccelerationStructureDeviceAddressKHR as_address;PFN_vkCmdBuildAccelerationStructuresKHR build;
    PFN_vkCreateQueryPool create_queries;PFN_vkDestroyQueryPool destroy_queries;
    PFN_vkCmdResetQueryPool reset_queries;PFN_vkCmdWriteTimestamp timestamp;
    PFN_vkGetQueryPoolResults query_results;VkQueryPool queries;u32 timestamp_bits;
    PFN_vkCmdCopyBuffer copy_buffer;
    PFN_vkCmdFillBuffer fill_buffer;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR as_properties;VkPhysicalDeviceProperties properties;
    VkSampler alpha_samplers[VK_RAY_TEXTURES];VkDescriptorImageInfo alpha_images[VK_RAY_TEXTURES];u32 texture_count;
    VkSamplerCreateInfo alpha_sampler_info[VK_RAY_TEXTURES];
    u64 attempted,completed,traced,rejected,last_copy_ticks,last_copy_bytes;u32 last_flags;
    u64 invalid_position,invalid_alpha,invalid_depth,invalid_numeric,empty_ao;u32 gpu_stats[4];double cpu_ms,gpu_ms[5];
} s_ray_ao;
static u64 s_ray_device_generation;
static void vk_ray_destroy_buffer(vk_buffer *b)
{
    if(b->ptr)pvkUnmapMemory(s_vk.device,b->mem);if(b->buf)pvkDestroyBuffer(s_vk.device,b->buf,NULL);
    if(b->mem)pvkFreeMemory(s_vk.device,b->mem,NULL);memset(b,0,sizeof *b);
}
static int vk_ray_buffer(vk_buffer *b,VkDeviceSize bytes,VkBufferUsageFlags usage,int host,int bda)
{
    if(!bytes || bytes>VK_RAY_MAX_ALLOCATION)return 0;
    if(b->size>=bytes)return 1;
    vk_ray_destroy_buffer(b);
    VkBufferCreateInfo ci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes,.usage=usage|(bda?VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT:0),.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    if(pvkCreateBuffer(s_vk.device,&ci,NULL,&b->buf)!=VK_SUCCESS)goto fail;
    VkMemoryRequirements r;pvkGetBufferMemoryRequirements(s_vk.device,b->buf,&r);u32 type;
    VkMemoryPropertyFlags flags=host?VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT:VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if(vk_find_memory_type(r.memoryTypeBits,flags,&type))goto fail;
    VkMemoryAllocateFlagsInfo af={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=bda?&af:NULL,.allocationSize=r.size,.memoryTypeIndex=type};
    if(pvkAllocateMemory(s_vk.device,&ai,NULL,&b->mem)!=VK_SUCCESS || pvkBindBufferMemory(s_vk.device,b->buf,b->mem,0)!=VK_SUCCESS)goto fail;
    if(host && pvkMapMemory(s_vk.device,b->mem,0,VK_WHOLE_SIZE,0,&b->ptr)!=VK_SUCCESS)goto fail;
    b->size=bytes;b->coherent=host;return 1;
fail:vk_ray_destroy_buffer(b);return 0;
}
static VkDeviceAddress vk_ray_address(vk_buffer *b)
{
    VkBufferDeviceAddressInfo i={.sType=VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,.buffer=b->buf};return s_ray_ao.address(s_vk.device,&i);
}
static void vk_ray_memory(VkPipelineStageFlags src,VkPipelineStageFlags dst,VkAccessFlags source,VkAccessFlags destination)
{
    VkMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=source,.dstAccessMask=destination};
    pvkCmdPipelineBarrier(s_vk.cmd,src,dst,0,1,&b,0,NULL,0,NULL);
}
static char *vk_ray_replace(const char *source,const char *marker,const char *replacement)
{
    const char *at=strstr(source,marker);if(!at || strstr(at+strlen(marker),marker))return NULL;
    size_t before=(size_t)(at-source),tail=strlen(at+strlen(marker)),add=strlen(replacement);
    if(before+tail+add>1024u*1024u)return NULL;
    char *out=(char*)malloc(before+tail+add+1);if(!out)return NULL;
    memcpy(out,source,before);memcpy(out+before,replacement,add);memcpy(out+before+add,at+strlen(marker),tail+1);return out;
}
static VkPipeline vk_ray_pipeline(const char *source)
{
    u32 *words=(u32*)malloc(262144u*4),count=0;char log[2048];VkShaderModule module=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;
    if(!words)return VK_NULL_HANDLE;
    if(rsx_glsl_compute_to_spirv(source,words,262144,&count,log,sizeof log) || !count || count>262144){VK_LOG("ray AO shader rejected: %s\n",log);goto done;}
    VkShaderModuleCreateInfo sm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)count*4,.pCode=words};
    if(pvkCreateShaderModule(s_vk.device,&sm,NULL,&module)!=VK_SUCCESS)goto done;
    VkComputePipelineCreateInfo pc={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"},.layout=s_ray_ao.pipeline_layout};
    if(s_ray_ao.create_compute(s_vk.device,VK_NULL_HANDLE,1,&pc,NULL,&pipeline)!=VK_SUCCESS){if(pipeline)pvkDestroyPipeline(s_vk.device,pipeline,NULL);pipeline=VK_NULL_HANDLE;}
done:if(module)pvkDestroyShaderModule(s_vk.device,module,NULL);free(words);return pipeline;
}
static int vk_ray_initialize(void)
{
    if(s_ray_ao.ready)return 1;if(s_ray_ao.failed || !s_vk.rt_enabled)return 0;
#define VK_RAY_LOAD(field,type,name) s_ray_ao.field=(PFN_##type)pvkGetDeviceProcAddr(s_vk.device,name);if(!s_ray_ao.field)goto fail
    VK_RAY_LOAD(create_compute,vkCreateComputePipelines,"vkCreateComputePipelines");
    VK_RAY_LOAD(dispatch,vkCmdDispatch,"vkCmdDispatch");VK_RAY_LOAD(address,vkGetBufferDeviceAddress,"vkGetBufferDeviceAddress");
    VK_RAY_LOAD(copy_buffer,vkCmdCopyBuffer,"vkCmdCopyBuffer");
    VK_RAY_LOAD(fill_buffer,vkCmdFillBuffer,"vkCmdFillBuffer");
    VK_RAY_LOAD(create_as,vkCreateAccelerationStructureKHR,"vkCreateAccelerationStructureKHR");
    VK_RAY_LOAD(destroy_as,vkDestroyAccelerationStructureKHR,"vkDestroyAccelerationStructureKHR");
    VK_RAY_LOAD(sizes,vkGetAccelerationStructureBuildSizesKHR,"vkGetAccelerationStructureBuildSizesKHR");
    VK_RAY_LOAD(as_address,vkGetAccelerationStructureDeviceAddressKHR,"vkGetAccelerationStructureDeviceAddressKHR");
    VK_RAY_LOAD(build,vkCmdBuildAccelerationStructuresKHR,"vkCmdBuildAccelerationStructuresKHR");
#undef VK_RAY_LOAD
    PFN_vkGetPhysicalDeviceProperties2 properties2=(PFN_vkGetPhysicalDeviceProperties2)pvkGetInstanceProcAddr(s_vk.instance,"vkGetPhysicalDeviceProperties2");
    if(!properties2)goto fail;
    s_ray_ao.as_properties=(VkPhysicalDeviceAccelerationStructurePropertiesKHR){.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 properties={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&s_ray_ao.as_properties};properties2(s_vk.phys,&properties);s_ray_ao.properties=properties.properties;
    const VkPhysicalDeviceLimits *l=&s_ray_ao.properties.limits;
    if(l->maxPerStageDescriptorSamplers<34 || l->maxPerStageDescriptorSampledImages<34 || l->maxDescriptorSetSamplers<34 || l->maxDescriptorSetSampledImages<34 ||
       l->maxPerStageDescriptorStorageBuffers<7 || l->maxDescriptorSetStorageBuffers<7 || l->maxPerStageDescriptorStorageImages<1 || l->maxDescriptorSetStorageImages<1 || l->maxPerStageResources<44 || l->maxPushConstantsSize<4 ||
       !s_ray_ao.as_properties.maxPerStageDescriptorAccelerationStructures || !s_ray_ao.as_properties.maxDescriptorSetAccelerationStructures ||
       !s_ray_ao.as_properties.minAccelerationStructureScratchOffsetAlignment || !s_ray_ao.as_properties.maxInstanceCount)goto fail;
    VkFormatProperties vertex_format;pvkGetPhysicalDeviceFormatProperties(s_vk.phys,VK_FORMAT_R32G32B32_SFLOAT,&vertex_format);
    if(!(vertex_format.bufferFeatures&VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR))goto fail;
    VkDescriptorSetLayoutBinding binding[44];u32 n=0;
    for(u32 i=0;i<7;i++)binding[n++]=(VkDescriptorSetLayoutBinding){.binding=i,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    binding[n++]=(VkDescriptorSetLayoutBinding){.binding=7,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    binding[n++]=(VkDescriptorSetLayoutBinding){.binding=8,.descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    for(u32 i=9;i<=10;i++)binding[n++]=(VkDescriptorSetLayoutBinding){.binding=i,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    binding[n++]=(VkDescriptorSetLayoutBinding){.binding=11,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    for(u32 i=0;i<VK_RAY_TEXTURES;i++)binding[n++]=(VkDescriptorSetLayoutBinding){.binding=16+i,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo sl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=n,.pBindings=binding};
    if(pvkCreateDescriptorSetLayout(s_vk.device,&sl,NULL,&s_ray_ao.layout)!=VK_SUCCESS)goto fail;
    VkPushConstantRange push={VK_SHADER_STAGE_COMPUTE_BIT,0,4};
    VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&s_ray_ao.layout,.pushConstantRangeCount=1,.pPushConstantRanges=&push};
    if(pvkCreatePipelineLayout(s_vk.device,&pl,NULL,&s_ray_ao.pipeline_layout)!=VK_SUCCESS)goto fail;
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,7},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1},{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,1},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,34},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1}};
    VkDescriptorPoolCreateInfo dp={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=5,.pPoolSizes=sizes};
    if(pvkCreateDescriptorPool(s_vk.device,&dp,NULL,&s_ray_ao.pool)!=VK_SUCCESS)goto fail;
    VkDescriptorSetAllocateInfo da={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=s_ray_ao.pool,.descriptorSetCount=1,.pSetLayouts=&s_ray_ao.layout};
    if(pvkAllocateDescriptorSets(s_vk.device,&da,&s_ray_ao.set)!=VK_SUCCESS)goto fail;
    s_ray_ao.gather=vk_ray_pipeline(rsx_scene_gather_glsl);if(!s_ray_ao.gather)goto fail;
    char *with_alpha=vk_ray_replace(rsx_scene_trace_glsl,"/*RSX_SCENE_ALPHA_HELPER*/",rsx_scene_alpha_glsl);if(!with_alpha)goto fail;
    for(unsigned i=0;i<2;i++) {char *text=vk_ray_replace(with_alpha,"/*RSX_SCENE_OUTPUT_FORMAT*/",i?"rgba16f":"rgba8");if(text){s_ray_ao.trace[i]=vk_ray_pipeline(text);free(text);}}
    free(with_alpha);if(!s_ray_ao.trace[0] || !s_ray_ao.trace[1])goto fail;
    s_ray_ao.create_queries=(PFN_vkCreateQueryPool)pvkGetDeviceProcAddr(s_vk.device,"vkCreateQueryPool");s_ray_ao.destroy_queries=(PFN_vkDestroyQueryPool)pvkGetDeviceProcAddr(s_vk.device,"vkDestroyQueryPool");
    s_ray_ao.reset_queries=(PFN_vkCmdResetQueryPool)pvkGetDeviceProcAddr(s_vk.device,"vkCmdResetQueryPool");s_ray_ao.timestamp=(PFN_vkCmdWriteTimestamp)pvkGetDeviceProcAddr(s_vk.device,"vkCmdWriteTimestamp");s_ray_ao.query_results=(PFN_vkGetQueryPoolResults)pvkGetDeviceProcAddr(s_vk.device,"vkGetQueryPoolResults");
    u32 queues=0;pvkGetPhysicalDeviceQueueFamilyProperties(s_vk.phys,&queues,NULL);VkQueueFamilyProperties *q=queues && queues<=1024?(VkQueueFamilyProperties*)calloc(queues,sizeof *q):NULL;
    if(q){pvkGetPhysicalDeviceQueueFamilyProperties(s_vk.phys,&queues,q);if(s_vk.queue_family<queues)s_ray_ao.timestamp_bits=q[s_vk.queue_family].timestampValidBits;free(q);}
    if(s_ray_ao.timestamp_bits && s_ray_ao.create_queries && s_ray_ao.destroy_queries && s_ray_ao.reset_queries && s_ray_ao.timestamp && s_ray_ao.query_results){
        VkQueryPoolCreateInfo qc={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=6};
        if(s_ray_ao.create_queries(s_vk.device,&qc,NULL,&s_ray_ao.queries)!=VK_SUCCESS)s_ray_ao.queries=VK_NULL_HANDLE;
    }
    if(s_ray_device_generation==UINT64_MAX)goto fail;
    s_ray_ao.device_generation=++s_ray_device_generation;s_ray_ao.ready=1;return 1;
fail:s_ray_ao.failed=1;return 0;
}
static void vk_ray_stamp(u32 index,VkPipelineStageFlagBits stage)
{if(s_ray_ao.queries)s_ray_ao.timestamp(s_vk.cmd,stage,s_ray_ao.queries,index);}
static void vk_ray_alpha_reset(void)
{
    for(u32 i=0;i<s_ray_ao.texture_count;i++)if(s_ray_ao.alpha_samplers[i])pvkDestroySampler(s_vk.device,s_ray_ao.alpha_samplers[i],NULL);
    memset(s_ray_ao.alpha_samplers,0,sizeof s_ray_ao.alpha_samplers);s_ray_ao.texture_count=0;
    for(u32 i=0;i<VK_RAY_TEXTURES;i++)s_ray_ao.alpha_images[i]=(VkDescriptorImageInfo){s_vk.sampler,s_vk.dummy.view,VK_IMAGE_LAYOUT_GENERAL};
}
static int vk_ray_alpha_texture(u32 texture,const rsx_be_sampler_desc *d,u32 *slot)
{
    if(!isfinite(d->min_lod) || !isfinite(d->max_lod))return 0;
    const vk_eng_obj *object=vk_eng_get(texture,VK_ENG_TEXTURE);if(!object || object->cube || object->surface || object->expanded || !object->im.view)return 0;
    float maximum=d->mip_present?d->max_lod:d->min_lod;if(maximum<d->min_lod)maximum=d->min_lod;
    VkSamplerCreateInfo si={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=d->mag_linear?VK_FILTER_LINEAR:VK_FILTER_NEAREST,.minFilter=d->min_linear?VK_FILTER_LINEAR:VK_FILTER_NEAREST,
        .mipmapMode=d->mip_linear?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=vk_gcm_wrap(d->wrap_s),.addressModeV=vk_gcm_wrap(d->wrap_t),.addressModeW=vk_gcm_wrap(d->wrap_r),
        .minLod=d->min_lod,.maxLod=maximum,.borderColor=VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK};
    rsx_vk_anisotropy_apply(&si,d->vertex_sampler?0u:d->anisotropy_code,s_vk.anisotropy_override,s_vk.anisotropy_ok,s_vk.max_anisotropy,!d->vertex_sampler);
    // Deduplicate exact effective sampler state AND view; no padding/hash identity.
    for(u32 i=0;i<s_ray_ao.texture_count;i++)if(s_ray_ao.alpha_images[i].imageView==object->im.view){
        const VkSamplerCreateInfo *old=&s_ray_ao.alpha_sampler_info[i];
        if(old->magFilter==si.magFilter && old->minFilter==si.minFilter && old->mipmapMode==si.mipmapMode && old->addressModeU==si.addressModeU && old->addressModeV==si.addressModeV && old->addressModeW==si.addressModeW &&
           old->minLod==si.minLod && old->maxLod==si.maxLod && old->anisotropyEnable==si.anisotropyEnable && old->maxAnisotropy==si.maxAnisotropy && old->borderColor==si.borderColor){*slot=i;return 1;}
    }
    if(s_ray_ao.texture_count>=VK_RAY_TEXTURES)return 0;
    u32 index=s_ray_ao.texture_count;
    if(pvkCreateSampler(s_vk.device,&si,NULL,&s_ray_ao.alpha_samplers[index])!=VK_SUCCESS)return 0;
    s_ray_ao.alpha_images[index]=(VkDescriptorImageInfo){s_ray_ao.alpha_samplers[index],object->im.view,VK_IMAGE_LAYOUT_GENERAL};s_ray_ao.alpha_sampler_info[index]=si;s_ray_ao.texture_count++;*slot=index;return 1;
}
static int vk_ray_as(VkAccelerationStructureTypeKHR type,VkAccelerationStructureGeometryKHR *geometry,u32 count,VkAccelerationStructureKHR *handle,vk_buffer *store,vk_buffer *scratch,VkAccelerationStructureBuildGeometryInfoKHR *build)
{
    *build=(VkAccelerationStructureBuildGeometryInfoKHR){.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,.type=type,.flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,.mode=VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,.geometryCount=1,.pGeometries=geometry};
    VkAccelerationStructureBuildSizesInfoKHR size={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};s_ray_ao.sizes(s_vk.device,VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,build,&count,&size);
    u32 alignment=s_ray_ao.as_properties.minAccelerationStructureScratchOffsetAlignment;
    if(!size.accelerationStructureSize || !size.buildScratchSize || size.accelerationStructureSize>VK_RAY_MAX_ALLOCATION || size.buildScratchSize>VK_RAY_MAX_ALLOCATION-alignment)return 0;
    if(store->size<size.accelerationStructureSize && *handle){s_ray_ao.destroy_as(s_vk.device,*handle,NULL);*handle=VK_NULL_HANDLE;}
    if(!vk_ray_buffer(store,size.accelerationStructureSize,VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,0,1) || !vk_ray_buffer(scratch,size.buildScratchSize+alignment,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,0,1))return 0;
    if(!*handle){VkAccelerationStructureCreateInfoKHR ci={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,.buffer=store->buf,.size=store->size,.type=type};if(s_ray_ao.create_as(s_vk.device,&ci,NULL,handle)!=VK_SUCCESS)return 0;}
    VkDeviceAddress address=vk_ray_address(scratch);if(!address)return 0;
    build->dstAccelerationStructure=*handle;build->scratchData.deviceAddress=(address+alignment-1)&~((VkDeviceAddress)alignment-1);return 1;
}
static int vk_ray_screen(u32 width,u32 height,VkFormat format)
{
    if(s_ray_ao.color && s_ray_ao.cw==width && s_ray_ao.ch==height && s_ray_ao.format==format)return 1;
    if(s_ray_ao.color)vk_eng_release(NULL,s_ray_ao.color);s_ray_ao.color=0;
    VkFormatProperties properties;pvkGetPhysicalDeviceFormatProperties(s_vk.phys,format,&properties);
    if(!(properties.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))return 0;
    u32 color=vk_eng_alloc();if(!color)return 0;vk_eng_obj *out=&s_eobj[color];
    if(vk_create_image(&out->im,format,width,height,VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_IMAGE_ASPECT_COLOR_BIT)){
        vk_destroy_image(&out->im);memset(out,0,sizeof *out);return 0;
    }
    out->kind=VK_ENG_COLOR;out->fmt=format;out->w=width;out->h=height;out->samples=1;out->bpp=format==VK_FORMAT_R16G16B16A16_SFLOAT?8:4;
    s_ray_ao.color=color;s_ray_ao.cw=width;s_ray_ao.ch=height;s_ray_ao.format=format;return 1;
}
static void vk_rt_ao_shutdown(void)
{
    if(!s_vk.device)return;
    // Backend shutdown owns the drain. No module allocation retires early.
    pvkDeviceWaitIdle(s_vk.device);vk_ray_alpha_reset();
    if(s_ray_ao.color)vk_eng_release(NULL,s_ray_ao.color);
    if(s_ray_ao.tlas)s_ray_ao.destroy_as(s_vk.device,s_ray_ao.tlas,NULL);if(s_ray_ao.blas)s_ray_ao.destroy_as(s_vk.device,s_ray_ao.blas,NULL);
    vk_buffer *buffers[]={&s_ray_ao.indices,&s_ray_ao.materials,&s_ray_ao.fp,&s_ray_ao.params,&s_ray_ao.triangles,&s_ray_ao.ao,&s_ray_ao.instances,&s_ray_ao.blas_store,&s_ray_ao.tlas_store,&s_ray_ao.bscratch,&s_ray_ao.tscratch,&s_ray_ao.flag_readback};
    for(unsigned i=0;i<sizeof buffers/sizeof buffers[0];i++)vk_ray_destroy_buffer(buffers[i]);
    if(s_ray_ao.queries)s_ray_ao.destroy_queries(s_vk.device,s_ray_ao.queries,NULL);
    if(s_ray_ao.gather)pvkDestroyPipeline(s_vk.device,s_ray_ao.gather,NULL);for(unsigned i=0;i<2;i++)if(s_ray_ao.trace[i])pvkDestroyPipeline(s_vk.device,s_ray_ao.trace[i],NULL);
    if(s_ray_ao.pool)pvkDestroyDescriptorPool(s_vk.device,s_ray_ao.pool,NULL);if(s_ray_ao.pipeline_layout)pvkDestroyPipelineLayout(s_vk.device,s_ray_ao.pipeline_layout,NULL);if(s_ray_ao.layout)pvkDestroyDescriptorSetLayout(s_vk.device,s_ray_ao.layout,NULL);
    rsx_vulkan_scene_batch_shutdown();memset(&s_ray_ao,0,sizeof s_ray_ao);
}
/* Original color is retained on every rejected/failed epoch. GPU-discovered
 * failures select original texels for the ENTIRE output in a separate pass. */
static u32 vk_eng_ray_world(void *user,u32 color,u32 depth,u32 logical_w,u32 logical_h,const float projection[16],u32 remap,u32 rsx_format,float radius,float strength)
{
    (void)user;u64 start=SDL_GetPerformanceCounter(),record_start=0,submit_start=0;u64 token=0;int recorded=0,submitted=0;u32 replacement=0;
    const char *failure="epoch/projection qualification";
    rsx_scene_epoch *epoch=vk_rt_epoch(depth);s_ray_ao.attempted++;
    if(!s_vk.rt_enabled || s_rt_capture.overflow || !epoch || epoch->sealed || !epoch->armed || epoch->frame!=s_rt_capture.frame || epoch->reason!=RSX_SCENE_EPOCH_OK || !epoch->draw_count || !projection || !logical_w || !logical_h || !isfinite(radius)||radius<=.00001f||radius>1000 || !isfinite(strength)||strength<0||strength>1)goto fail;
    for(unsigned i=0;i<16;i++)if(!isfinite(projection[i]))goto fail;
    if(projection[0]<=0 || projection[5]<=0 || projection[10]<1 || projection[14]>=0 || projection[11]!=1 || projection[15]!=0 ||
       projection[1]!=0 || projection[2]!=0 || projection[3]!=0 || projection[4]!=0 || projection[6]!=0 || projection[7]!=0 || projection[8]!=0 || projection[9]!=0 || projection[12]!=0 || projection[13]!=0)goto fail;
    vk_eng_obj *c=vk_eng_get(color,VK_ENG_COLOR),*z=vk_eng_get(depth,VK_ENG_DEPTH);
    if(!c || !z)goto fail;
    u32 samples=c->samples?c->samples:1,depth_samples=z->samples?z->samples:1;
    if(samples!=depth_samples || c->scale!=z->scale || (samples!=1 && samples!=2) || epoch->width!=z->w || epoch->height!=z->h || epoch->samples!=depth_samples)goto fail;
    u32 scale=c->scale?c->scale:1,gx=samples==2?2:1;
    u64 width64=(u64)logical_w*scale,height64=(u64)logical_h*scale;if(!width64 || width64>UINT32_MAX/gx || !height64 || height64>UINT32_MAX)goto fail;
    u32 width=(u32)width64,height=(u32)height64;
    if(c->w!=width || c->h!=height || z->w!=width || z->h!=height || (c->fmt!=VK_FORMAT_R8G8B8A8_UNORM && c->fmt!=VK_FORMAT_R16G16B16A16_SFLOAT))goto fail;
    vk_flush_pending(0);
    failure="device/shader/descriptor capabilities";
    if(!vk_ray_initialize() || (gx==2 && !s_ray_ao.properties.limits.standardSampleLocations))goto fail;
    u32 triangles=epoch->index_count/3;if(!triangles || epoch->index_count%3 || triangles>s_ray_ao.as_properties.maxPrimitiveCount)goto fail;
    u32 source_color=gx==2?vk_eng_expand_samples(color):color;
    u32 source_depth=vk_eng_depth_snapshot(NULL,depth,logical_w*gx,logical_h);
    vk_eng_obj *source=source_color?&s_eobj[source_color]:NULL,*snapshot=vk_eng_get(source_depth,VK_ENG_TEXTURE);
    failure="matching color/depth snapshot";
    if(!source || !snapshot || source->w!=width*gx || source->h!=height || snapshot->w!=width*gx || snapshot->h!=height || snapshot->fmt!=VK_FORMAT_R32_SFLOAT)goto fail;
    u32 aw=width<320?width:320,ah=height<180?height:180;
    // All resources/samplers retire after the prior synchronous submission.
    vk_ray_alpha_reset();
    failure="bounded resident allocation";
    if(!vk_ray_screen(width*gx,height,source->fmt) ||
       !vk_ray_buffer(&s_ray_ao.indices,(VkDeviceSize)triangles*16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,1,0) ||
       !vk_ray_buffer(&s_ray_ao.materials,(VkDeviceSize)epoch->draw_count*sizeof(vk_ray_material),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,1,0) ||
       !vk_ray_buffer(&s_ray_ao.fp,16u*1024u*1024u,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,1,0) ||
       !vk_ray_buffer(&s_ray_ao.params,sizeof(vk_ray_parameters),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,1,0) ||
       !vk_ray_buffer(&s_ray_ao.triangles,(VkDeviceSize)triangles*36,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,0,1) ||
       !vk_ray_buffer(&s_ray_ao.ao,(VkDeviceSize)aw*ah*4+16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,0,0) ||
       !vk_ray_buffer(&s_ray_ao.instances,sizeof(VkAccelerationStructureInstanceKHR),VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,1,1) ||
       !vk_ray_buffer(&s_ray_ao.flag_readback,20,VK_BUFFER_USAGE_TRANSFER_DST_BIT,1,0))goto fail;
    failure="batch capture/cache";
    if(rsx_vulkan_scene_batch_begin(s_ray_ao.device_generation,1,&token))goto fail;
    size_t fp_used=0;u32 primitive=0;
    static const u32 fp_counts[8]={16,13,44,42,36,39,38,24};
    for(u32 i=0;i<epoch->draw_count;i++){
        rsx_scene_draw_input draw;if(!rsx_scene_epoch_get(epoch,i,&draw) || !draw.pipeline || draw.pipeline>VK_ENG_MAX_PIPE)goto fail;
        const vk_eng_pipe *pipeline=&s_epipe[draw.pipeline-1];const rsx_be_render_state *rs=&draw.state;
        failure="draw state/viewport qualification";
        if(!pipeline->used || !pipeline->scene_vs || !pipeline->scene_ps || rs->blend_enable || rs->stencil_enable || (rs->antialias_control&0x110u) || !rs->depth_test || !rs->depth_write || (rs->depth_func!=0x201u && rs->depth_func!=0x203u) ||
           draw.viewport[0]!=0 || draw.viewport[1]!=0 || draw.viewport[2]!=(float)logical_w || draw.viewport[3]!=(float)logical_h || draw.viewport[4]!=0 || draw.viewport[5]!=1 ||
           draw.scissor[0]!=0 || draw.scissor[1]!=0 || draw.scissor[2]!=logical_w || draw.scissor[3]!=logical_h)goto fail;
        u32 mask=rs->antialias_control>>16,needed=(1u<<samples)-1;if((mask&needed)!=needed)goto fail;
        int contract=rs->alpha_test_enable && rs->alpha_func!=0x207u?pipeline->scene_alpha_contract:-1;
        failure="exact alpha contract/resources";
        // The generated decompiler emits active OP_KIL as discard; (never
        // predicates emit nothing). Opaque/ALWAYS state cannot hide that kill.
        if(contract<0 && (strstr(pipeline->scene_ps,"discard;") || strstr(pipeline->scene_ps,"clip(")))goto fail;
        if(rs->alpha_test_enable && rs->alpha_func!=0x207u && (rs->alpha_func!=0x204u || contract<0 || contract>=8 || draw.ps_bytes!=(size_t)fp_counts[contract]*16))goto fail;
        if(fp_used+draw.ps_bytes>s_ray_ao.fp.size)goto fail;
        vk_ray_material material={{contract<0?UINT32_MAX:(u32)contract,(u32)(fp_used/16),(u32)(draw.ps_bytes/16),draw.texture_mask},{rs->cull_enable,rs->cull_face,rs->front_face==0x901u,0},{UINT32_MAX,UINT32_MAX,0,0}};
        if(contract>=0)for(u32 unit=0;unit<(contract<2?1u:2u);unit++)if(!(draw.texture_mask&(1u<<unit)) || !vk_ray_alpha_texture(draw.textures[unit],&draw.samplers[unit],&material.textures[unit]))goto fail;
        if(draw.ps_bytes)memcpy((unsigned char*)s_ray_ao.fp.ptr+fp_used,draw.ps_constants,draw.ps_bytes);
        fp_used+=draw.ps_bytes;((vk_ray_material*)s_ray_ao.materials.ptr)[i]=material;
        rsx_scene_batch_input input={pipeline->scene_vs,draw.layout,draw.vertices,(size_t)draw.vertex_count*draw.layout.stride,draw.vertex_count,draw.vs_constants,draw.vs_bytes};u32 base=0;
        failure="batch immutable upload/cache";
        if(rsx_vulkan_scene_batch_append(token,&input,&base))goto fail;
        for(u32 j=0;j<draw.index_count;j+=3){u32 *destination=(u32*)s_ray_ao.indices.ptr+(size_t)primitive++*4;for(u32 k=0;k<3;k++)destination[k]=base+draw.indices[j+k];destination[3]=i;}
    }
    if(primitive!=triangles || !rsx_scene_epoch_seal(epoch,s_rt_capture.frame,epoch->serial))goto fail;
    /* Opaque shaders can have no fragment constants. Vulkan still requires
     * a nonzero descriptor range even when the shader never accesses it. */
    if(!fp_used){memset(s_ray_ao.fp.ptr,0,16);fp_used=16;}
    vk_ray_parameters parameters={{projection[0],projection[5],projection[10],projection[14]},{aw,ah,width,height},{triangles,epoch->vertex_count,gx,0},{radius,fmaxf(.00001f,radius*.001f),strength,.005f}};
    memcpy(s_ray_ao.params.ptr,&parameters,sizeof parameters);
    failure="AS sizes/address preparation";
    VkDeviceAddress triangle_address=vk_ray_address(&s_ray_ao.triangles),instance_address=vk_ray_address(&s_ray_ao.instances);if(!triangle_address || !instance_address)goto fail;
    VkAccelerationStructureGeometryKHR bg={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,.geometryType=VK_GEOMETRY_TYPE_TRIANGLES_KHR};
    bg.geometry.triangles=(VkAccelerationStructureGeometryTrianglesDataKHR){.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,.vertexFormat=VK_FORMAT_R32G32B32_SFLOAT,.vertexData.deviceAddress=triangle_address,.vertexStride=12,.maxVertex=triangles*3-1,.indexType=VK_INDEX_TYPE_NONE_KHR};
    VkAccelerationStructureBuildGeometryInfoKHR bb,tb;
    if(!vk_ray_as(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,&bg,triangles,&s_ray_ao.blas,&s_ray_ao.blas_store,&s_ray_ao.bscratch,&bb))goto fail;
    VkAccelerationStructureDeviceAddressInfoKHR as_info={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,.accelerationStructure=s_ray_ao.blas};
    VkDeviceAddress blas_address=s_ray_ao.as_address(s_vk.device,&as_info);if(!blas_address)goto fail;
    VkAccelerationStructureInstanceKHR instance={.transform={.matrix={{1,0,0,0},{0,1,0,0},{0,0,1,0}}},.mask=255,.flags=VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR,.accelerationStructureReference=blas_address};memcpy(s_ray_ao.instances.ptr,&instance,sizeof instance);
    VkAccelerationStructureGeometryKHR tg={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,.geometryType=VK_GEOMETRY_TYPE_INSTANCES_KHR};tg.geometry.instances=(VkAccelerationStructureGeometryInstancesDataKHR){.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,.data.deviceAddress=instance_address};
    if(!vk_ray_as(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,&tg,1,&s_ray_ao.tlas,&s_ray_ao.tlas_store,&s_ray_ao.tscratch,&tb))goto fail;
    failure="command recording";record_start=SDL_GetPerformanceCounter();
    if(vk_begin() || pvkResetFences(s_vk.device,1,&s_vk.fence)!=VK_SUCCESS)goto fail;
    if(s_ray_ao.queries){s_ray_ao.reset_queries(s_vk.cmd,s_ray_ao.queries,0,6);vk_ray_stamp(0,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);}
    rsx_scene_batch_output batch;if(rsx_vulkan_scene_batch_record(token,s_vk.cmd,s_vk.fence,&batch))goto abandon;recorded=1;
    vk_ray_stamp(1,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vk_eng_obj *out=vk_eng_get(s_ray_ao.color,VK_ENG_COLOR);if(!out)goto abandon;
    // Discard only the private output's old contents; guest color stays intact.
    vk_barrier_image(out->im.img,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED);
    VkDescriptorBufferInfo bi[8]={{batch.vertices,0,batch.vertex_bytes},{s_ray_ao.indices.buf,0,(VkDeviceSize)triangles*16},{s_ray_ao.triangles.buf,0,(VkDeviceSize)triangles*36},{batch.validity,0,4},
        {s_ray_ao.materials.buf,0,(VkDeviceSize)epoch->draw_count*sizeof(vk_ray_material)},{s_ray_ao.fp.buf,0,fp_used},{s_ray_ao.ao.buf,0,(VkDeviceSize)aw*ah*4+16},{s_ray_ao.params.buf,0,sizeof parameters}};
    VkWriteDescriptorSet writes[44];u32 wn=0;for(u32 i=0;i<8;i++)writes[wn++]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=s_ray_ao.set,.dstBinding=i,.descriptorCount=1,.descriptorType=i==7?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&bi[i]};
    VkWriteDescriptorSetAccelerationStructureKHR wa={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,.accelerationStructureCount=1,.pAccelerationStructures=&s_ray_ao.tlas};
    writes[wn++]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.pNext=&wa,.dstSet=s_ray_ao.set,.dstBinding=8,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};
    VkDescriptorImageInfo images[3]={{s_vk.sampler,source->im.view,VK_IMAGE_LAYOUT_GENERAL},{s_vk.sampler,snapshot->im.view,VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,out->im.view,VK_IMAGE_LAYOUT_GENERAL}};
    for(u32 i=0;i<3;i++)writes[wn++]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=s_ray_ao.set,.dstBinding=9+i,.descriptorCount=1,.descriptorType=i==2?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&images[i]};
    for(u32 i=0;i<VK_RAY_TEXTURES;i++)writes[wn++]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=s_ray_ao.set,.dstBinding=16+i,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&s_ray_ao.alpha_images[i]};
    pvkUpdateDescriptorSets(s_vk.device,wn,writes,0,NULL);
    s_ray_ao.fill_buffer(s_vk.cmd,s_ray_ao.ao.buf,(VkDeviceSize)aw*ah*4,16,0);
    vk_ray_memory(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
    vk_ray_memory(VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_UNIFORM_READ_BIT|VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    pvkCmdBindDescriptorSets(s_vk.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s_ray_ao.pipeline_layout,0,1,&s_ray_ao.set,0,NULL);pvkCmdBindPipeline(s_vk.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s_ray_ao.gather);s_ray_ao.dispatch(s_vk.cmd,(triangles+63)/64,1,1);
    vk_ray_memory(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);vk_ray_stamp(2,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    VkAccelerationStructureBuildRangeInfoKHR br={.primitiveCount=triangles},tr={.primitiveCount=1};const VkAccelerationStructureBuildRangeInfoKHR *brp=&br,*trp=&tr;
    s_ray_ao.build(s_vk.cmd,1,&bb,&brp);vk_ray_memory(VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    s_ray_ao.build(s_vk.cmd,1,&tb,&trp);vk_ray_memory(VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);vk_ray_stamp(3,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);
    u32 mode=0;pvkCmdBindPipeline(s_vk.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s_ray_ao.trace[source->fmt==VK_FORMAT_R16G16B16A16_SFLOAT]);pvkCmdPushConstants(s_vk.cmd,s_ray_ao.pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&mode);s_ray_ao.dispatch(s_vk.cmd,(aw+7)/8,(ah+7)/8,1);
    vk_ray_memory(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);vk_ray_stamp(4,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    mode=1;pvkCmdPushConstants(s_vk.cmd,s_ray_ao.pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&mode);s_ray_ao.dispatch(s_vk.cmd,(width*gx+7)/8,(height+7)/8,1);
    vk_ray_memory(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);vk_ray_stamp(5,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    // Twenty bytes at the existing synchronous boundary report GPU validity
    // and actual primary/matched/secondary/alpha-rejection counters. No vertices.
    vk_ray_memory(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferCopy flag_copy={.size=4};s_ray_ao.copy_buffer(s_vk.cmd,batch.validity,s_ray_ao.flag_readback.buf,1,&flag_copy);
    VkBufferCopy counter_copy={.srcOffset=(VkDeviceSize)aw*ah*4,.dstOffset=4,.size=16};s_ray_ao.copy_buffer(s_vk.cmd,s_ray_ao.ao.buf,s_ray_ao.flag_readback.buf,1,&counter_copy);
    vk_ray_memory(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
    failure="synchronous integration submission";submit_start=SDL_GetPerformanceCounter();
    if(vk_submit_and_wait())goto abandon;
    submitted=1;if(rsx_vulkan_scene_batch_mark_submitted(token) || rsx_vulkan_scene_batch_reset(token))goto fail;recorded=0;token=0;
    memcpy(&s_ray_ao.last_flags,s_ray_ao.flag_readback.ptr,4);s_ray_ao.traced++;
    memcpy(s_ray_ao.gpu_stats,(unsigned char*)s_ray_ao.flag_readback.ptr+4,16);
    if(s_ray_ao.last_flags&1u)s_ray_ao.invalid_position++;if(s_ray_ao.last_flags&4u)s_ray_ao.invalid_alpha++;if(s_ray_ao.last_flags&8u)s_ray_ao.invalid_depth++;if(s_ray_ao.last_flags&16u)s_ray_ao.invalid_numeric++;
    if(!s_ray_ao.gpu_stats[1] || !s_ray_ao.gpu_stats[2])s_ray_ao.empty_ao++;
    if(!(s_ray_ao.last_flags&~2u) && s_ray_ao.gpu_stats[1] && s_ray_ao.gpu_stats[2]){out->scale=scale;out->grid_x=gx;out->grid_y=1;replacement=vk_eng_surface_view(NULL,s_ray_ao.color,remap,rsx_format);if(replacement)s_ray_ao.completed++;}
    if(!replacement)s_ray_ao.rejected++;
    if(s_ray_ao.queries){u64 times[6];if(s_ray_ao.query_results(s_vk.device,s_ray_ao.queries,0,6,sizeof times,times,8,VK_QUERY_RESULT_64_BIT)==VK_SUCCESS){u64 mask_bits=s_ray_ao.timestamp_bits>=64?UINT64_MAX:(1ull<<s_ray_ao.timestamp_bits)-1;for(unsigned i=0;i<5;i++)s_ray_ao.gpu_ms[i]=(double)((times[i+1]-times[i])&mask_bits)*s_ray_ao.properties.limits.timestampPeriod/1000000.0;}}
    s_ray_ao.cpu_ms=(double)(SDL_GetPerformanceCounter()-start)*1000.0/(double)SDL_GetPerformanceFrequency();
    double copy_ms=(double)(s_rt_capture.copy_ticks-s_ray_ao.last_copy_ticks)*1000.0/(double)SDL_GetPerformanceFrequency();
    u64 copied=s_rt_capture.copied_bytes-s_ray_ao.last_copy_bytes;s_ray_ao.last_copy_ticks=s_rt_capture.copy_ticks;s_ray_ao.last_copy_bytes=s_rt_capture.copied_bytes;
    if(s_ray_ao.traced<=4 || s_ray_ao.traced%120==0 || (!replacement && s_ray_ao.rejected<=8))VK_LOG("ray AO frame=%llu epoch=%llu requested=%llu traced=%llu validAO=%llu fallback=%llu flags=0x%x reasonCounts(pos/alpha/depth/numeric)=%llu/%llu/%llu/%llu draws=%u vertices=%u triangles=%u AO=%ux%u secondary=4 alphaLOD=0 copied=%llu copyCPU=%.3fms totalCPU=%.3fms prepare/record/wait=%.3f/%.3f/%.3fms GPU(VS/gather/AS/trace/composite)=%.3f/%.3f/%.3f/%.3f/%.3fms timestampBits=%u\n",(unsigned long long)epoch->frame,(unsigned long long)epoch->generation,(unsigned long long)s_ray_ao.attempted,(unsigned long long)s_ray_ao.traced,(unsigned long long)s_ray_ao.completed,(unsigned long long)s_ray_ao.rejected,s_ray_ao.last_flags,(unsigned long long)s_ray_ao.invalid_position,(unsigned long long)s_ray_ao.invalid_alpha,(unsigned long long)s_ray_ao.invalid_depth,(unsigned long long)s_ray_ao.invalid_numeric,epoch->draw_count,epoch->vertex_count,triangles,aw,ah,(unsigned long long)copied,copy_ms,s_ray_ao.cpu_ms,(double)(record_start-start)*1000.0/(double)SDL_GetPerformanceFrequency(),(double)(submit_start-record_start)*1000.0/(double)SDL_GetPerformanceFrequency(),(double)(SDL_GetPerformanceCounter()-submit_start)*1000.0/(double)SDL_GetPerformanceFrequency(),s_ray_ao.gpu_ms[0],s_ray_ao.gpu_ms[1],s_ray_ao.gpu_ms[2],s_ray_ao.gpu_ms[3],s_ray_ao.gpu_ms[4],s_ray_ao.timestamp_bits);
    if(s_ray_ao.traced<=4 || s_ray_ao.traced%120==0 || (!replacement && s_ray_ao.rejected<=8))VK_LOG("ray AO actual queries primary=%u matched=%u secondary=%u alphaRejected=%u emptyFrames=%llu\n",s_ray_ao.gpu_stats[0],s_ray_ao.gpu_stats[1],s_ray_ao.gpu_stats[2],s_ray_ao.gpu_stats[3],(unsigned long long)s_ray_ao.empty_ao);
    return replacement;
abandon:
    // If submission failed, drain before invalidating recorded command storage.
    if(pvkDeviceWaitIdle(s_vk.device)==VK_SUCCESS){
        pvkResetCommandBuffer(s_vk.cmd,0);s_batch.recording=0;s_batch.pending=0;
        // The complete-device drain proves no GPU reference survives, even
        // when the helper could not report whether a failed submit was queued.
        if(recorded && !submitted){rsx_vulkan_scene_batch_discard_recording(token);recorded=0;}
    } else s_ray_ao.failed=1; // Retain batch ownership until device teardown.
fail:
    if(token && !recorded)rsx_vulkan_scene_batch_reset(token);
    if(epoch)rsx_scene_epoch_invalidate(epoch,RSX_SCENE_EPOCH_UNSUPPORTED);s_ray_ao.rejected++;
    if(!s_ray_ao.warned || s_ray_ao.rejected<=8){VK_LOG("ray AO CPU fallback reason=%s requested=%llu traced=%llu validAO=%llu fallback=%llu frame=%llu epoch=%llu; original world color retained\n",failure,(unsigned long long)s_ray_ao.attempted,(unsigned long long)s_ray_ao.traced,(unsigned long long)s_ray_ao.completed,(unsigned long long)s_ray_ao.rejected,(unsigned long long)(epoch?epoch->frame:0),(unsigned long long)(epoch?epoch->generation:0));s_ray_ao.warned=1;}
    return 0;
}
#endif /* RSX_VULKAN_RAY_AO_INC */
