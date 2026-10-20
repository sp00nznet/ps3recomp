/* Explicit live current-frame ray-query AO consumer. Include after scene_batch
 * and rt_capture, with embedded rsx_scene_{gather,trace,alpha}_glsl strings.
 * No file scene, CPU position readback, second device, or automatic fallback AO.
 * One synchronous integration submission initially; source snapshot helpers can
 * have their existing synchronous preparation boundaries. No performance claim.
 */
#ifndef RSX_VULKAN_RAY_AO_INC
#define RSX_VULKAN_RAY_AO_INC
#include "rsx_scene_shader_sources.h"
#include "rsx_scene_stencil.h"
#define VK_RAY_TEXTURES 32u
#define VK_RAY_CUBES 4u
#define VK_RAY_ALL_TEXTURES (VK_RAY_TEXTURES+VK_RAY_CUBES)
#include "rsx_authored_lights.h"
#include "rsx_ray_memory.h"
static int vk_dlss_prepare(u32 vertices);
static u32 vk_dlss_record(u32 source,u32 depth,VkBuffer vertices,VkDeviceSize vertex_bytes,const rsx_scene_epoch *epoch,
                          const float projection[16],const float camera[16],int have_camera,u64 camera_key,u32 remap,u32 format);
static void vk_dlss_complete(int submitted,u32 source);
typedef struct vk_ray_material {u32 alpha[4],raster[4],textures[4];float depth_range[4];u32 authored[4],rgb_textures[8];float light_view[4][4],light_rgb[4][4],light_to_view[3][4];} vk_ray_material;
typedef struct vk_ray_parameters {float projection[4];u32 dims[4],counts[4];float settings[4];} vk_ray_parameters;
_Static_assert(sizeof(vk_ray_material)==288 && offsetof(vk_ray_material,depth_range)==48,"GLSL std430 material layout");
_Static_assert(sizeof(vk_ray_parameters)==64 && offsetof(vk_ray_parameters,settings)==48,"GLSL std140 parameters layout");
static struct {
    rsx_authored_lights authored_lights[RSX_SCENE_EPOCH_MAX_DRAWS];
    u32 authored_draws,authored_active_lights,authored_count;
    /* Per-frame qualification counts, indexed by exact source contract. These
     * distinguish recovered lights from a fully bound ray material. */
    u32 authored_known[RSX_AUTHORED_CONTRACT_COUNT],authored_nonunit[RSX_AUTHORED_CONTRACT_COUNT],authored_recovery_failed[RSX_AUTHORED_CONTRACT_COUNT];
    u32 authored_state_failed[RSX_AUTHORED_CONTRACT_COUNT],authored_texture_failed[RSX_AUTHORED_CONTRACT_COUNT],authored_ready[RSX_AUTHORED_CONTRACT_COUNT];
    int test_disable_authored_sources,test_disable_authored_bounce,test_disable_authored_reflections;
    int ready,failed,warned;u32 color,raw_color,cw,ch,aw,ah;VkFormat format;u64 device_generation;
    VkDescriptorSetLayout layout;VkPipelineLayout pipeline_layout;VkDescriptorPool pool;VkDescriptorSet set;
    VkPipeline gather,trace[2];u64 last_hybrid_log,hybrid_nonfinite,hybrid_black_frames;u32 history_w,history_h;VkAccelerationStructureKHR blas,tlas;
    vk_buffer history;vk_buffer indices,materials,fp,params,triangles,ao,instances,blas_store,tlas_store,bscratch,tscratch,flag_readback;
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
    VkDeviceSize max_allocation;int memory_budget;
    PFN_vkGetPhysicalDeviceMemoryProperties2 memory_properties2;
    VkSampler alpha_samplers[VK_RAY_ALL_TEXTURES];VkDescriptorImageInfo alpha_images[VK_RAY_ALL_TEXTURES];u32 texture_count,cube_count,authored_pixels,authored_changes;
    VkSamplerCreateInfo alpha_sampler_info[VK_RAY_ALL_TEXTURES];
    u64 attempted,completed,traced,rejected,last_copy_ticks,last_copy_bytes;u32 last_flags;
    u64 invalid_position,invalid_alpha,invalid_depth,invalid_numeric,empty_ao;u32 gpu_stats[32];double cpu_ms,gpu_ms[7];
    /* Hybrid frame state (CPU side of rsx_hybrid_trace.glsl): the stable key
     * light, the previous camera for temporal reprojection, history bank. */
    int key_valid,key_in_light;u32 key_missing,key_challenge;
    float key_view[3],key_light[3],key_rgb[3];
    int cam_valid;u64 cam_frame;float cam_view[12],cam_projection[4];
    u32 history_bank;u64 held_frames,dlss_frames;
    /* Asynchronous transport: this frame's color and depth, copied for the
     * trace on the second queue (the originals change during the next frame). */
    vk_image async_color,async_depth;u32 async_w,async_h;VkFormat async_format;int async_fresh;u64 async_frames;
} s_ray_ao;
static u64 s_ray_device_generation;
static void vk_ray_destroy_buffer(vk_buffer *b)
{
    if(b->ptr)pvkUnmapMemory(s_vk.device,b->mem);if(b->buf)pvkDestroyBuffer(s_vk.device,b->buf,NULL);
    if(b->mem)pvkFreeMemory(s_vk.device,b->mem,NULL);memset(b,0,sizeof *b);
}
static int vk_ray_buffer_impl(vk_buffer *b,VkDeviceSize bytes,VkBufferUsageFlags usage,int host,int bda)
{
    if(!bytes || bytes>s_ray_ao.max_allocation)return 0;
    if((usage&VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) && s_ray_ao.properties.limits.maxStorageBufferRange &&
       bytes>s_ray_ao.properties.limits.maxStorageBufferRange)return 0;
    if(b->size>=bytes)return 1;
    vk_ray_destroy_buffer(b);
    VkBufferCreateInfo ci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes,.usage=usage|(bda?VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT:0),.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    if(pvkCreateBuffer(s_vk.device,&ci,NULL,&b->buf)!=VK_SUCCESS)goto fail;
    VkMemoryRequirements r;pvkGetBufferMemoryRequirements(s_vk.device,b->buf,&r);u32 type;
    VkMemoryPropertyFlags flags=host?VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT:VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    /* host==2: written by the CPU, read per pixel by the GPU (indices,
     * materials, constants). Plain host-visible memory is system RAM on a
     * discrete GPU, so every shader read crossed PCIe; prefer the
     * device-local host-visible (resizable BAR) type when there is one. */
    if(host==2 && !vk_find_memory_type(r.memoryTypeBits,flags|VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,&type))flags|=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    else if(vk_find_memory_type(r.memoryTypeBits,flags,&type))goto fail;
    u32 heap=s_vk.memprops.memoryTypes[type].heapIndex;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 memory={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,.pNext=&budget};
    int have_budget=s_ray_ao.memory_budget && s_ray_ao.memory_properties2;
    if(have_budget)s_ray_ao.memory_properties2(s_vk.phys,&memory);
    if(!rsx_ray_memory_fits(r.size,s_ray_ao.max_allocation,s_vk.memprops.memoryHeaps[heap].size,
        have_budget,budget.heapBudget[heap],budget.heapUsage[heap]))goto fail;
    VkMemoryAllocateFlagsInfo af={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=bda?&af:NULL,.allocationSize=r.size,.memoryTypeIndex=type};
    if(pvkAllocateMemory(s_vk.device,&ai,NULL,&b->mem)!=VK_SUCCESS || pvkBindBufferMemory(s_vk.device,b->buf,b->mem,0)!=VK_SUCCESS)goto fail;
    if(host && pvkMapMemory(s_vk.device,b->mem,0,VK_WHOLE_SIZE,0,&b->ptr)!=VK_SUCCESS)goto fail;
    b->size=bytes;b->coherent=host;return 1;
fail:vk_ray_destroy_buffer(b);return 0;
}
/* host: 0 device-local, 1 host-visible (CPU reads it back), 2 host-written
 * and GPU-read, placed in device-local host-visible memory when it fits. */
static int vk_ray_buffer(vk_buffer *b,VkDeviceSize bytes,VkBufferUsageFlags usage,int host,int bda)
{
    if(host==2 && vk_ray_buffer_impl(b,bytes,usage,2,bda))return 1;
    return vk_ray_buffer_impl(b,bytes,usage,host==2?1:host,bda);
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
    u64 compile_start=SDL_GetPerformanceCounter();
    VK_LOG("ray pipeline GLSL translation started\n");
    if(rsx_glsl_compute_to_spirv(source,words,262144,&count,log,sizeof log) || !count || count>262144){VK_LOG("ray AO shader rejected: %s\n",log);goto done;}
    VK_LOG("ray pipeline GLSL translation completed in %.3fms; driver compilation started (words=%u)\n",
        (double)(SDL_GetPerformanceCounter()-compile_start)*1000.0/(double)SDL_GetPerformanceFrequency(),count);
    VkShaderModuleCreateInfo sm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)count*4,.pCode=words};
    if(pvkCreateShaderModule(s_vk.device,&sm,NULL,&module)!=VK_SUCCESS)goto done;
    VkComputePipelineCreateInfo pc={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"},.layout=s_ray_ao.pipeline_layout};
    if(s_ray_ao.create_compute(s_vk.device,vk_pcache(),1,&pc,NULL,&pipeline)!=VK_SUCCESS){if(pipeline)pvkDestroyPipeline(s_vk.device,pipeline,NULL);pipeline=VK_NULL_HANDLE;}
    else {vk_pcache_created();
        if(s_pcache.cache && !SDL_AtomicGet(&s_pcache.saving) && vk_pcache_save_now(s_vk.device)==0)SDL_AtomicSet(&s_pcache.dirty,0);
        VK_LOG("ray pipeline ready after %.3fms total\n",
        (double)(SDL_GetPerformanceCounter()-compile_start)*1000.0/(double)SDL_GetPerformanceFrequency());}
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
    VkPhysicalDeviceMaintenance3Properties allocation={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES};
    s_ray_ao.as_properties.pNext=&allocation;
    VkPhysicalDeviceProperties2 properties={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&s_ray_ao.as_properties};properties2(s_vk.phys,&properties);s_ray_ao.properties=properties.properties;
    s_ray_ao.as_properties.pNext=NULL;s_ray_ao.max_allocation=allocation.maxMemoryAllocationSize;
    if(!s_ray_ao.max_allocation)goto fail;
    s_ray_ao.memory_properties2=(PFN_vkGetPhysicalDeviceMemoryProperties2)pvkGetInstanceProcAddr(s_vk.instance,"vkGetPhysicalDeviceMemoryProperties2");
    s_ray_ao.memory_budget=vk_has_device_extension(s_vk.phys,VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    VK_LOG("ray memory: driver allocation limit=%llu MiB storage range=%llu MiB live budget=%s\n",
        (unsigned long long)(s_ray_ao.max_allocation/(1024*1024)),
        (unsigned long long)(s_ray_ao.properties.limits.maxStorageBufferRange/(1024*1024)),
        s_ray_ao.memory_budget&&s_ray_ao.memory_properties2?"available":"unavailable (heap bounds plus allocation result)");
    const VkPhysicalDeviceLimits *l=&s_ray_ao.properties.limits;
    if(l->maxPerStageDescriptorSamplers<38 || l->maxPerStageDescriptorSampledImages<38 || l->maxDescriptorSetSamplers<38 || l->maxDescriptorSetSampledImages<38 ||
       l->maxPerStageDescriptorStorageBuffers<8 || l->maxDescriptorSetStorageBuffers<8 || l->maxPerStageDescriptorStorageImages<2 || l->maxDescriptorSetStorageImages<2 || l->maxPerStageResources<50 || l->maxPushConstantsSize<4 ||
       !s_ray_ao.as_properties.maxPerStageDescriptorAccelerationStructures || !s_ray_ao.as_properties.maxDescriptorSetAccelerationStructures ||
       !s_ray_ao.as_properties.minAccelerationStructureScratchOffsetAlignment || !s_ray_ao.as_properties.maxInstanceCount)goto fail;
    VkFormatProperties vertex_format;pvkGetPhysicalDeviceFormatProperties(s_vk.phys,VK_FORMAT_R32G32B32_SFLOAT,&vertex_format);
    if(!(vertex_format.bufferFeatures&VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR))goto fail;
    VkDescriptorSetLayoutBinding binding[50];u32 n=0;
    for(u32 i=0;i<7;i++)binding[n++]=(VkDescriptorSetLayoutBinding){.binding=i,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    binding[n++]=(VkDescriptorSetLayoutBinding){.binding=7,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    binding[n++]=(VkDescriptorSetLayoutBinding){.binding=8,.descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    for(u32 i=9;i<=10;i++)binding[n++]=(VkDescriptorSetLayoutBinding){.binding=i,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    binding[n++]=(VkDescriptorSetLayoutBinding){.binding=11,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    binding[n++]=(VkDescriptorSetLayoutBinding){.binding=12,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    for(u32 i=0;i<VK_RAY_ALL_TEXTURES;i++)binding[n++]=(VkDescriptorSetLayoutBinding){.binding=16+i,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    binding[n++]=(VkDescriptorSetLayoutBinding){.binding=13,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo sl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=n,.pBindings=binding};
    if(pvkCreateDescriptorSetLayout(s_vk.device,&sl,NULL,&s_ray_ao.layout)!=VK_SUCCESS)goto fail;
    VkPushConstantRange push={VK_SHADER_STAGE_COMPUTE_BIT,0,4};
    VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&s_ray_ao.layout,.pushConstantRangeCount=1,.pPushConstantRanges=&push};
    if(pvkCreatePipelineLayout(s_vk.device,&pl,NULL,&s_ray_ao.pipeline_layout)!=VK_SUCCESS)goto fail;
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,8},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1},{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,1},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,38},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2}};
    VkDescriptorPoolCreateInfo dp={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=5,.pPoolSizes=sizes};
    if(pvkCreateDescriptorPool(s_vk.device,&dp,NULL,&s_ray_ao.pool)!=VK_SUCCESS)goto fail;
    VkDescriptorSetAllocateInfo da={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=s_ray_ao.pool,.descriptorSetCount=1,.pSetLayouts=&s_ray_ao.layout};
    if(pvkAllocateDescriptorSets(s_vk.device,&da,&s_ray_ao.set)!=VK_SUCCESS)goto fail;
    s_ray_ao.gather=vk_ray_pipeline(rsx_scene_gather_glsl);if(!s_ray_ao.gather)goto fail;
    /* DLSS without PT (PS3RECOMP_RSX_TRACE=0) rebuilds the scene for motion
     * vectors only and never compiles the trace kernels. */
    const char *trace_setting=getenv("PS3RECOMP_RSX_TRACE");
    if(!(trace_setting && !strcmp(trace_setting,"0"))){
    char *hybrid_text=vk_ray_replace(rsx_scene_trace_glsl,"/*RSX_HYBRID_HELPER*/",rsx_hybrid_trace_glsl);if(!hybrid_text)goto fail;
    char *with_transport=vk_ray_replace(hybrid_text,"/*RSX_PATH_TRANSPORT_HELPER*/",rsx_path_transport_glsl);free(hybrid_text);if(!with_transport)goto fail;
    char *with_footprint=vk_ray_replace(with_transport,"/*RSX_RAY_FOOTPRINT_HELPER*/",rsx_ray_footprint_glsl);free(with_transport);if(!with_footprint)goto fail;
    char *with_alpha=vk_ray_replace(with_footprint,"/*RSX_SCENE_ALPHA_HELPER*/",rsx_scene_alpha_glsl);free(with_footprint);if(!with_alpha)goto fail;
    char *with_authored=vk_ray_replace(with_alpha,"/*RSX_POOL_RGB_HELPER*/",rsx_pool_rgb_glsl);free(with_alpha);if(!with_authored)goto fail;
    char *with_terrain=vk_ray_replace(with_authored,"/*RSX_TERRAIN_RGB_HELPER*/",rsx_terrain_rgb_glsl);free(with_authored);if(!with_terrain)goto fail;
    char *with_rgb=vk_ray_replace(with_terrain,"/*RSX_SCENE_RGB_HELPER*/",rsx_scene_rgb_glsl);free(with_terrain);if(!with_rgb)goto fail;
    const char *hybrid_compile=getenv("PS3RECOMP_RSX_HYBRID_PT");
    if(hybrid_compile && !strcmp(hybrid_compile,"1")){
        const char *denoise_compile=getenv("PS3RECOMP_RSX_RAY_DENOISE");
        char *specialized=vk_ray_replace(with_rgb,"#define RSX_TRANSPORT_FLAGS p.counts.w",
            denoise_compile && !strcmp(denoise_compile,"off")?"#define RSX_TRANSPORT_FLAGS 52u":"#define RSX_TRANSPORT_FLAGS 54u");
        free(with_rgb);with_rgb=specialized;if(!with_rgb)goto fail;
    }
    for(unsigned i=0;i<2;i++) {
        char *output_text=vk_ray_replace(with_rgb,"/*RSX_SCENE_OUTPUT_FORMAT*/",i?"rgba16f":"rgba8");
        char *text=output_text?vk_ray_replace(output_text,"/*RSX_SCENE_RAW_FORMAT*/",i?"rgba16f":"rgba8"):NULL;
        free(output_text);if(text){s_ray_ao.trace[i]=vk_ray_pipeline(text);free(text);}
    }
    free(with_rgb);if(!s_ray_ao.trace[0] || !s_ray_ao.trace[1])goto fail;
    }
    s_ray_ao.create_queries=(PFN_vkCreateQueryPool)pvkGetDeviceProcAddr(s_vk.device,"vkCreateQueryPool");s_ray_ao.destroy_queries=(PFN_vkDestroyQueryPool)pvkGetDeviceProcAddr(s_vk.device,"vkDestroyQueryPool");
    s_ray_ao.reset_queries=(PFN_vkCmdResetQueryPool)pvkGetDeviceProcAddr(s_vk.device,"vkCmdResetQueryPool");s_ray_ao.timestamp=(PFN_vkCmdWriteTimestamp)pvkGetDeviceProcAddr(s_vk.device,"vkCmdWriteTimestamp");s_ray_ao.query_results=(PFN_vkGetQueryPoolResults)pvkGetDeviceProcAddr(s_vk.device,"vkGetQueryPoolResults");
    u32 queues=0;pvkGetPhysicalDeviceQueueFamilyProperties(s_vk.phys,&queues,NULL);VkQueueFamilyProperties *q=queues && queues<=1024?(VkQueueFamilyProperties*)calloc(queues,sizeof *q):NULL;
    if(q){pvkGetPhysicalDeviceQueueFamilyProperties(s_vk.phys,&queues,q);if(s_vk.queue_family<queues)s_ray_ao.timestamp_bits=q[s_vk.queue_family].timestampValidBits;free(q);}
    if(s_ray_ao.timestamp_bits && s_ray_ao.create_queries && s_ray_ao.destroy_queries && s_ray_ao.reset_queries && s_ray_ao.timestamp && s_ray_ao.query_results){
        VkQueryPoolCreateInfo qc={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=8};
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
    for(u32 i=0;i<VK_RAY_ALL_TEXTURES;i++)if(s_ray_ao.alpha_samplers[i])pvkDestroySampler(s_vk.device,s_ray_ao.alpha_samplers[i],NULL);
    memset(s_ray_ao.alpha_samplers,0,sizeof s_ray_ao.alpha_samplers);s_ray_ao.texture_count=0;s_ray_ao.cube_count=0;
    for(u32 i=0;i<VK_RAY_ALL_TEXTURES;i++)s_ray_ao.alpha_images[i]=(VkDescriptorImageInfo){s_vk.sampler,i<VK_RAY_TEXTURES?s_vk.dummy.view:vk_eng_dummy_cube_view(),VK_IMAGE_LAYOUT_GENERAL};
}
static int vk_ray_texture(u32 texture,const rsx_be_sampler_desc *d,u32 *slot,int cube)
{
    if(!isfinite(d->min_lod) || !isfinite(d->max_lod))return 0;
    const vk_eng_obj *object=vk_eng_get(texture,VK_ENG_TEXTURE);if(!object || object->cube!=(cube!=0) || object->surface || object->expanded || !object->im.view)return 0;
    float maximum=d->mip_present?d->max_lod:d->min_lod;if(maximum<d->min_lod)maximum=d->min_lod;
    VkSamplerCreateInfo si={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=d->mag_linear?VK_FILTER_LINEAR:VK_FILTER_NEAREST,.minFilter=d->min_linear?VK_FILTER_LINEAR:VK_FILTER_NEAREST,
        .mipmapMode=d->mip_linear?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=vk_gcm_wrap(d->wrap_s),.addressModeV=vk_gcm_wrap(d->wrap_t),.addressModeW=vk_gcm_wrap(d->wrap_r),
        .minLod=d->min_lod,.maxLod=maximum,.borderColor=VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK};
    rsx_vk_anisotropy_apply(&si,d->vertex_sampler?0u:d->anisotropy_code,s_vk.anisotropy_override,s_vk.anisotropy_ok,s_vk.max_anisotropy,!d->vertex_sampler);
    // Deduplicate exact effective sampler state AND view; no padding/hash identity.
    for(u32 i=cube?VK_RAY_TEXTURES:0;i<(cube?VK_RAY_TEXTURES+s_ray_ao.cube_count:s_ray_ao.texture_count);i++)if(s_ray_ao.alpha_images[i].imageView==object->im.view){
        const VkSamplerCreateInfo *old=&s_ray_ao.alpha_sampler_info[i];
        if(old->magFilter==si.magFilter && old->minFilter==si.minFilter && old->mipmapMode==si.mipmapMode && old->addressModeU==si.addressModeU && old->addressModeV==si.addressModeV && old->addressModeW==si.addressModeW &&
           old->minLod==si.minLod && old->maxLod==si.maxLod && old->anisotropyEnable==si.anisotropyEnable && old->maxAnisotropy==si.maxAnisotropy && old->borderColor==si.borderColor){*slot=cube?i-VK_RAY_TEXTURES:i;return 1;}
    }
    if(cube?s_ray_ao.cube_count>=VK_RAY_CUBES:s_ray_ao.texture_count>=VK_RAY_TEXTURES)return 0;
    u32 index=cube?VK_RAY_TEXTURES+s_ray_ao.cube_count:s_ray_ao.texture_count;
    if(pvkCreateSampler(s_vk.device,&si,NULL,&s_ray_ao.alpha_samplers[index])!=VK_SUCCESS)return 0;
    s_ray_ao.alpha_images[index]=(VkDescriptorImageInfo){s_ray_ao.alpha_samplers[index],object->im.view,VK_IMAGE_LAYOUT_GENERAL};s_ray_ao.alpha_sampler_info[index]=si;if(cube)s_ray_ao.cube_count++;else s_ray_ao.texture_count++;*slot=cube?index-VK_RAY_TEXTURES:index;return 1;
}
static int vk_ray_alpha_texture(u32 t,const rsx_be_sampler_desc *d,u32 *slot){return vk_ray_texture(t,d,slot,0);}
static int vk_ray_as(VkAccelerationStructureTypeKHR type,VkAccelerationStructureGeometryKHR *geometry,u32 count,VkAccelerationStructureKHR *handle,vk_buffer *store,vk_buffer *scratch,VkAccelerationStructureBuildGeometryInfoKHR *build)
{
    *build=(VkAccelerationStructureBuildGeometryInfoKHR){.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,.type=type,.flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,.mode=VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,.geometryCount=1,.pGeometries=geometry};
    VkAccelerationStructureBuildSizesInfoKHR size={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};s_ray_ao.sizes(s_vk.device,VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,build,&count,&size);
    u32 alignment=s_ray_ao.as_properties.minAccelerationStructureScratchOffsetAlignment;
    if(!size.accelerationStructureSize || !size.buildScratchSize || s_ray_ao.max_allocation<alignment ||
       size.accelerationStructureSize>s_ray_ao.max_allocation || size.buildScratchSize>s_ray_ao.max_allocation-alignment)return 0;
    if(store->size<size.accelerationStructureSize && *handle){s_ray_ao.destroy_as(s_vk.device,*handle,NULL);*handle=VK_NULL_HANDLE;}
    if(!vk_ray_buffer(store,size.accelerationStructureSize,VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,0,1) || !vk_ray_buffer(scratch,size.buildScratchSize+alignment,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,0,1))return 0;
    if(!*handle){VkAccelerationStructureCreateInfoKHR ci={.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,.buffer=store->buf,.size=store->size,.type=type};if(s_ray_ao.create_as(s_vk.device,&ci,NULL,handle)!=VK_SUCCESS)return 0;}
    VkDeviceAddress address=vk_ray_address(scratch);if(!address)return 0;
    build->dstAccelerationStructure=*handle;build->scratchData.deviceAddress=(address+alignment-1)&~((VkDeviceAddress)alignment-1);return 1;
}
static int vk_ray_screen(u32 width,u32 height,VkFormat format)
{
    if(s_ray_ao.color && s_ray_ao.raw_color && s_ray_ao.cw==width && s_ray_ao.ch==height && s_ray_ao.format==format)return 1;
    if(s_ray_ao.color)vk_eng_release(NULL,s_ray_ao.color);s_ray_ao.color=0;
    if(s_ray_ao.raw_color)vk_eng_release(NULL,s_ray_ao.raw_color);s_ray_ao.raw_color=0;
    VkFormatProperties properties;pvkGetPhysicalDeviceFormatProperties(s_vk.phys,format,&properties);
    if(!(properties.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))return 0;
    u32 color=vk_eng_alloc();if(!color)return 0;vk_eng_obj *out=&s_eobj[color];
    if(vk_create_image(&out->im,format,width,height,VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_IMAGE_ASPECT_COLOR_BIT)){
        vk_destroy_image(&out->im);memset(out,0,sizeof *out);return 0;
    }
    out->kind=VK_ENG_COLOR;out->fmt=format;out->w=width;out->h=height;out->samples=1;out->bpp=format==VK_FORMAT_R16G16B16A16_SFLOAT?8:4;
    u32 raw=vk_eng_alloc();if(!raw){vk_eng_release(NULL,color);return 0;}
    vk_eng_obj *raw_out=&s_eobj[raw];
    if(vk_create_image(&raw_out->im,format,width,height,VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_IMAGE_ASPECT_COLOR_BIT)){
        vk_destroy_image(&raw_out->im);memset(raw_out,0,sizeof *raw_out);vk_eng_release(NULL,color);return 0;
    }
    raw_out->kind=VK_ENG_COLOR;raw_out->fmt=format;raw_out->w=width;raw_out->h=height;raw_out->samples=1;raw_out->bpp=out->bpp;
    s_ray_ao.raw_color=raw;
    s_ray_ao.color=color;s_ray_ao.cw=width;s_ray_ao.ch=height;s_ray_ao.format=format;return 1;
}
static void vk_rt_ao_shutdown(void)
{
    if(!s_vk.device)return;
    // Backend shutdown owns the drain. No module allocation retires early.
    pvkDeviceWaitIdle(s_vk.device);vk_ray_alpha_reset();
    vk_destroy_image(&s_ray_ao.async_color);vk_destroy_image(&s_ray_ao.async_depth);
    if(s_ray_ao.color)vk_eng_release(NULL,s_ray_ao.color);
    if(s_ray_ao.raw_color)vk_eng_release(NULL,s_ray_ao.raw_color);
    if(s_ray_ao.tlas)s_ray_ao.destroy_as(s_vk.device,s_ray_ao.tlas,NULL);if(s_ray_ao.blas)s_ray_ao.destroy_as(s_vk.device,s_ray_ao.blas,NULL);
    vk_buffer *buffers[]={&s_ray_ao.history,&s_ray_ao.indices,&s_ray_ao.materials,&s_ray_ao.fp,&s_ray_ao.params,&s_ray_ao.triangles,&s_ray_ao.ao,&s_ray_ao.instances,&s_ray_ao.blas_store,&s_ray_ao.tlas_store,&s_ray_ao.bscratch,&s_ray_ao.tscratch,&s_ray_ao.flag_readback};
    for(unsigned i=0;i<sizeof buffers/sizeof buffers[0];i++)vk_ray_destroy_buffer(buffers[i]);
    if(s_ray_ao.queries)s_ray_ao.destroy_queries(s_vk.device,s_ray_ao.queries,NULL);
    if(s_ray_ao.gather)pvkDestroyPipeline(s_vk.device,s_ray_ao.gather,NULL);for(unsigned i=0;i<2;i++)if(s_ray_ao.trace[i])pvkDestroyPipeline(s_vk.device,s_ray_ao.trace[i],NULL);
    if(s_ray_ao.pool)pvkDestroyDescriptorPool(s_vk.device,s_ray_ao.pool,NULL);if(s_ray_ao.pipeline_layout)pvkDestroyPipelineLayout(s_vk.device,s_ray_ao.pipeline_layout,NULL);if(s_ray_ao.layout)pvkDestroyDescriptorSetLayout(s_vk.device,s_ray_ao.layout,NULL);
    rsx_vulkan_scene_batch_shutdown();memset(&s_ray_ao,0,sizeof s_ray_ao);
}
/* Original color is retained on every rejected/failed epoch. GPU-discovered
 * failures select original texels for the ENTIRE output in a separate pass. */
static int vk_ray_pair_ppm(const char *prefix,const char *suffix,const u8 *rgba,u32 w,u32 h)
{
    char path[1024];int n=snprintf(path,sizeof path,"%s-%s.ppm",prefix,suffix);
    if(n<0 || n>=(int)sizeof path)return 0;
    FILE *file=fopen(path,"wb");if(!file)return 0;
    int ok=fprintf(file,"P6\n%u %u\n255\n",w,h)>0;
    u8 *row=malloc((size_t)w*3);if(!row)ok=0;
    for(u32 y=0;ok && y<h;y++){
        for(u32 x=0;x<w;x++)memcpy(row+x*3,rgba+((size_t)y*w+x)*4,3);
        ok=fwrite(row,3,w,file)==w;
    }
    free(row);if(fclose(file))ok=0;return ok;
}
/* The exact sources share their deformed position between clip and lighting.
 * Prove its homogeneous coordinate from this immutable compact draw, rather
 * than inferring it from a source hash or from an apparently valid camera. */
static float vk_ray_source_h(const rsx_scene_draw_input *draw,int contract,u32 vertex)
{
    unsigned attr=contract==1?11u:0u;int slot=draw->layout.slot_by_attr[attr];
    if(slot<0 || (unsigned)slot>=draw->layout.count || vertex>=draw->vertex_count)return NAN;
    float value[4];memcpy(value,(const u8*)draw->vertices+(size_t)vertex*draw->layout.stride+(size_t)slot*16,16);
    if(contract!=1)return value[3];
    volatile float xy=value[0]+value[1],xyz=value[2]+xy,w=value[3]+xyz;return w;
}
static u32 vk_ray_unit_position(const rsx_scene_draw_input *draw,int contract)
{
    if(!draw || !draw->vertices || !draw->vertex_count || contract<0 || (unsigned)contract>=RSX_AUTHORED_CONTRACT_COUNT)return 0;
    unsigned attr=contract==1?11u:0u;
    int slot=draw->layout.slot_by_attr[attr];
    if(slot<0 || (unsigned)slot>=draw->layout.count || draw->layout.attrs[slot]!=attr ||
       ((u32)slot+1u)*16u>draw->layout.stride)return 0;
    int all_unit=1;
    for(u32 i=0;i<draw->vertex_count;i++){
        float value[4];memcpy(value,(const u8*)draw->vertices+(size_t)i*draw->layout.stride+(size_t)slot*16,16);
        if(contract!=1){if(value[3]!=1.0f)return 0;}
        else {
            for(unsigned k=0;k<4;k++)if(!isfinite(value[k]))return 0;
            /* Match source order and float rounding, no reassociation. */
            volatile float xy=value[0]+value[1];
            volatile float xyz=value[2]+xy;
            volatile float w=value[3]+xyz;
            if(!isfinite(w) || w<=1e-20f)return 0;
            if(w!=1.0f)all_unit=0;
        }
    }
    return all_unit?RSX_AUTHORED_POSITION_UNIT_HOMOGENEOUS:RSX_AUTHORED_POSITION_POSITIVE_HOMOGENEOUS;
}
static void vk_ray_capture_lights(const char *prefix,u64 frame,u64 epoch)
{
    char path[1024];int n=snprintf(path,sizeof path,"%s-authored-lights.json",prefix);
    if(n<=0 || n>=(int)sizeof path)return;
    FILE *file=fopen(path,"wb");if(!file)return;
    fprintf(file,"{\"frame\":%llu,\"epoch\":%llu,\"scope\":\"source-qualified per-draw parameters; not unique emitters or rendered relighting\",\"draw_count\":%u,\"qualified_draws\":%u,\"active_references\":%u,\"draws\":[",(unsigned long long)frame,(unsigned long long)epoch,s_ray_ao.authored_count,s_ray_ao.authored_draws,s_ray_ao.authored_active_lights);
    int first=1;
    for(u32 d=0;d<s_ray_ao.authored_count;d++){
        const rsx_authored_lights *a=&s_ray_ao.authored_lights[d];
        if(a->lights[0].status==RSX_AUTHORED_UNAVAILABLE)continue;
        fprintf(file,"%s{\"draw\":%u,\"contract\":%u,\"lights\":[",first?"":",",d,a->contract_id);first=0;
        for(unsigned i=0;i<4;i++){
            const rsx_authored_light *l=&a->lights[i];
            fprintf(file,"%s{\"status\":%u,\"type\":%u,\"vc\":%u,\"fp\":%u,\"authored\":[%.9g,%.9g,%.9g,%.9g],\"view\":[%.9g,%.9g,%.9g,%.9g],\"rgb\":[%.9g,%.9g,%.9g],\"source_w_used\":%u,\"linear_ramp\":%u,\"ramp\":[%.9g,%.9g,%.9g,%.9g]}",i?",":"",l->status,l->type,l->vc_index,l->fp_index,l->authored[0],l->authored[1],l->authored[2],l->authored[3],l->view[0],l->view[1],l->view[2],l->view[3],l->rgb[0],l->rgb[1],l->rgb[2],l->source_w_used,l->has_linear_ramp,l->ramp_near,l->ramp_far,l->ramp_denominator_floor,l->ramp_distance_squared_floor);
        }
        fprintf(file,"]}");
    }
    fprintf(file,"],\"material_qualification\":[");
    for(u32 c=0;c<RSX_AUTHORED_CONTRACT_COUNT;c++)fprintf(file,"%s{\"contract\":%u,\"known\":%u,\"position_rejected\":%u,\"light_recovery_rejected\":%u,\"state_rejected\":%u,\"texture_rejected\":%u,\"ready\":%u}",c?",":"",c,s_ray_ao.authored_known[c],s_ray_ao.authored_nonunit[c],s_ray_ao.authored_recovery_failed[c],s_ray_ao.authored_state_failed[c],s_ray_ao.authored_texture_failed[c],s_ray_ao.authored_ready[c]);
    fprintf(file,"],\"bound_texture_descriptors\":%u,\"bound_cube_descriptors\":%u}\n",s_ray_ao.texture_count,s_ray_ao.cube_count);fclose(file);
}
static void vk_ray_capture_pair(u32 source,u32 output,u32 width,u32 height,u32 gx,u64 frame,u64 epoch,const char *mode,int denoise,float strength)
{
    static unsigned captures=0;
    const char *base=getenv("PS3RECOMP_RSX_RAY_PAIR"),*request=getenv("PS3RECOMP_RSX_RAY_PAIR_REQUEST");
    if(captures>=4 || !base || !*base || !request || !*request)return;
    FILE *trigger=fopen(request,"rb");if(!trigger)return;fclose(trigger);
    // Explicit diagnostic request file, consumed once. Retain previous pairs.
    if(remove(request))return;
    char numbered[1024];const char *prefix=base;
    if(captures){int length=snprintf(numbered,sizeof numbered,"%s-%03u",base,captures+1);if(length<=0 || length>=(int)sizeof numbered)return;prefix=numbered;}
    ++captures;
    vk_eng_obj *a=&s_eobj[source],*b=&s_eobj[output];
    if(a->fmt!=VK_FORMAT_R8G8B8A8_UNORM || b->fmt!=a->fmt || !width || !height || (u64)width*height>16777216){VK_LOG("ray AO pair capture unsupported format/size\n");return;}
    const size_t size=(size_t)width*height*4;u8 *before=malloc(size),*after=malloc(size),*raw=malloc(size);
    int ok=before && after && raw;
    if(ok)ok=!vk_eng_readback_rows_raw(source,0,0,width,height,before,width*4) && !vk_eng_readback_rows_raw(output,0,0,width,height,after,width*4) && !vk_eng_readback_rows_raw(s_ray_ao.raw_color,0,0,width,height,raw,width*4);
    u64 changed=0,delta_sum=0;u32 max_delta=0;
    if(ok){
        for(size_t i=0;i<size;i+=4){int different=0;for(u32 c=0;c<3;c++){u32 d=(u32)abs((int)before[i+c]-(int)after[i+c]);delta_sum+=d;if(d>max_delta)max_delta=d;different|=d!=0;}changed+=different;}
        ok=vk_ray_pair_ppm(prefix,"off",before,width,height) && vk_ray_pair_ppm(prefix,"on",after,width,height) && vk_ray_pair_ppm(prefix,"raw",raw,width,height);
    }
    char path[1024];int n=snprintf(path,sizeof path,"%s-pair.json",prefix);
    if(n>0 && n<(int)sizeof path){FILE *file=fopen(path,"wb");if(file){fprintf(file,"{\"captured\":%s,\"frame\":%llu,\"epoch\":%llu,\"width\":%u,\"height\":%u,\"sample_grid_x\":%u,\"changed_pixels\":%llu,\"sum_rgb_delta\":%llu,\"max_rgb_delta\":%u,\"mode\":\"%s\",\"denoiser\":\"%s\",\"strength\":%.4f,\"scope\":\"same-frame world input, raw ray transport and final replacement before later composition\"}\n",ok?"true":"false",(unsigned long long)frame,(unsigned long long)epoch,width,height,gx,(unsigned long long)changed,(unsigned long long)delta_sum,max_delta,mode,denoise?"spatial":"off",strength);fclose(file);}}
    VK_LOG("ray AO same-frame pair captured=%d frame=%llu changed=%llu maxDelta=%u\n",ok,(unsigned long long)frame,(unsigned long long)changed,max_delta);
    if(ok)vk_ray_capture_lights(prefix,frame,epoch);
    free(before);free(after);free(raw);
}
/* ---- Hybrid frame helpers ------------------------------------------------ */
static int vk_ray_invert4(const float m[16],float inverse[16])
{
    double a[4][8];for(u32 r=0;r<4;r++)for(u32 c=0;c<8;c++)a[r][c]=c<4?m[r*4+c]:(c-4==r);
    for(u32 k=0;k<4;k++){
        u32 pivot=k;for(u32 r=k+1;r<4;r++)if(fabs(a[r][k])>fabs(a[pivot][k]))pivot=r;
        if(fabs(a[pivot][k])<1e-12)return 0;
        for(u32 c=0;c<8;c++){double t=a[k][c];a[k][c]=a[pivot][c];a[pivot][c]=t;}
        double d=a[k][k];for(u32 c=0;c<8;c++)a[k][c]/=d;
        for(u32 r=0;r<4;r++)if(r!=k){d=a[r][k];for(u32 c=0;c<8;c++)a[r][c]-=d*a[k][c];}
    }
    for(u32 r=0;r<4;r++)for(u32 c=0;c<4;c++){inverse[r*4+c]=(float)a[r][c+4];if(!isfinite(inverse[r*4+c]))return 0;}
    return 1;
}
static void vk_ray_mul4(const float a[16],const float b[16],float out[16])
{
    float t[16];
    for(u32 r=0;r<4;r++)for(u32 c=0;c<4;c++){double v=0;for(u32 k=0;k<4;k++)v+=(double)a[r*4+k]*b[k*4+c];t[r*4+c]=(float)v;}
    memcpy(out,t,sizeof t);
}
static void vk_ray_view4(const float view[12],float out[16])
{memcpy(out,view,48);out[12]=out[13]=out[14]=0;out[15]=1;}
static void vk_ray_projection4(const float p[4],float out[16])
{memset(out,0,64);out[0]=p[0];out[5]=p[1];out[10]=p[2];out[11]=p[3];out[14]=1;}
/* The frame camera. The terrain contract's light space is the game's world
 * space, shared by every terrain draw, so its light_to_view is the camera. */
static int vk_ray_frame_camera(const rsx_scene_epoch *epoch,float view[12],u64 *key)
{
    for(u32 i=0;i<epoch->draw_count && i<RSX_SCENE_EPOCH_MAX_DRAWS;i++){
        const rsx_authored_lights *l=&s_ray_ao.authored_lights[i];
        if(l->contract_id!=2 || l->lights[0].status==RSX_AUTHORED_UNAVAILABLE)continue;
        const float *v=&l->light_to_view[0][0];int finite=1;
        for(u32 k=0;k<12;k++)finite&=isfinite(v[k])!=0;
        if(!finite)continue;
        memcpy(view,v,48);
        rsx_scene_draw_input draw;*key=rsx_scene_epoch_get(epoch,i,&draw)?draw.geometry_identity:0;
        return 1;
    }
    return 0;
}
static float vk_ray_luma(const float c[3]){return .2126f*c[0]+.7152f*c[1]+.0722f*c[2];}
static int vk_ray_same_rgb(const float a[3],const float b[3])
{
    float m=fmaxf(fmaxf(fabsf(a[0]),fabsf(a[1])),fmaxf(fabsf(a[2]),1e-4f));
    for(u32 c=0;c<3;c++)if(fabsf(a[c]-b[c])>.05f*m+1e-4f)return 0;
    return 1;
}
static int vk_ray_normalize3(float v[3])
{
    float n=sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
    if(!(n>1e-12f) || !isfinite(n))return 0;
    v[0]/=n;v[1]/=n;v[2]/=n;return 1;
}
/* One key light owns every traced direct shadow. Each draw carries its own
 * recovered light list, and which draws are visible (and in which order)
 * changes as the camera moves; picking the first lit draw's light made the
 * shadow owner, and with it every shadow, jump between frames. The key is
 * the brightest directional light by luminance and draw coverage. It is
 * kept while any draw still carries it, survives a short absence (in world
 * space when the camera is known), and yields to a clearly brighter light
 * only after it has stayed brighter for a quarter of a second. */
static int vk_ray_select_key(u32 draw_count,int have_camera,const float view[12],float light[4],float rgb[4])
{
    const int was_valid=s_ray_ao.key_valid;
    struct {float view[3],rgb[3],score;u32 count;} c[8];u32 n=0;
    for(u32 i=0;i<draw_count && i<RSX_SCENE_EPOCH_MAX_DRAWS;i++)for(u32 l=0;l<4;l++){
        const rsx_authored_light *a=&s_ray_ao.authored_lights[i].lights[l];
        if(a->status!=RSX_AUTHORED_ACTIVE || a->type!=RSX_AUTHORED_DIRECTIONAL || a->view[3]!=0)continue;
        float d[3]={a->view[0],a->view[1],a->view[2]};
        if(!vk_ray_normalize3(d))continue;
        float luma=vk_ray_luma(a->rgb);if(!(luma>1e-4f) || !isfinite(luma))continue;
        u32 k=0;
        for(;k<n;k++)if(c[k].view[0]*d[0]+c[k].view[1]*d[1]+c[k].view[2]*d[2]>.995f && vk_ray_same_rgb(c[k].rgb,a->rgb))break;
        if(k==n){if(n==8)continue;memcpy(c[k].view,d,12);memcpy(c[k].rgb,a->rgb,12);c[k].count=0;n++;}
        c[k].count++;
    }
    int best=-1;
    for(u32 k=0;k<n;k++){c[k].score=vk_ray_luma(c[k].rgb)*sqrtf((float)c[k].count);if(best<0 || c[k].score>c[best].score)best=(int)k;}
    /* Where the previous key should appear in this frame's view space. */
    float expect[3]={s_ray_ao.key_view[0],s_ray_ao.key_view[1],s_ray_ao.key_view[2]};
    if(s_ray_ao.key_valid && s_ray_ao.key_in_light && have_camera){
        for(u32 r=0;r<3;r++)expect[r]=view[r*4]*s_ray_ao.key_light[0]+view[r*4+1]*s_ray_ao.key_light[1]+view[r*4+2]*s_ray_ao.key_light[2];
        if(!vk_ray_normalize3(expect))memcpy(expect,s_ray_ao.key_view,12);
    }
    int match=-1;
    if(s_ray_ao.key_valid)for(u32 k=0;k<n;k++)
        if(vk_ray_same_rgb(c[k].rgb,s_ray_ao.key_rgb) && c[k].view[0]*expect[0]+c[k].view[1]*expect[1]+c[k].view[2]*expect[2]>.97f &&
           (match<0 || c[k].score>c[match].score))match=(int)k;
    int adopt=-1;
    if(match>=0){
        s_ray_ao.key_missing=0;adopt=match;
        if(best>=0 && best!=match && c[best].score>1.5f*c[match].score){
            if(++s_ray_ao.key_challenge>=15){adopt=best;s_ray_ao.key_challenge=0;}
        }else s_ray_ao.key_challenge=0;
    }else if(s_ray_ao.key_valid && s_ray_ao.key_missing<45){
        s_ray_ao.key_missing++;memcpy(s_ray_ao.key_view,expect,12);
    }else if(best>=0){adopt=best;s_ray_ao.key_missing=0;s_ray_ao.key_challenge=0;}
    else s_ray_ao.key_valid=0;
    if(adopt>=0){
        s_ray_ao.key_valid=1;memcpy(s_ray_ao.key_view,c[adopt].view,12);memcpy(s_ray_ao.key_rgb,c[adopt].rgb,12);
        s_ray_ao.key_in_light=0;
        if(have_camera){
            /* World direction: solve R*w=v with R the camera's 3x3 part. */
            float m[16],inverse[16];vk_ray_view4(view,m);m[3]=m[7]=m[11]=0;
            if(vk_ray_invert4(m,inverse)){
                for(u32 r=0;r<3;r++)s_ray_ao.key_light[r]=inverse[r*4]*c[adopt].view[0]+inverse[r*4+1]*c[adopt].view[1]+inverse[r*4+2]*c[adopt].view[2];
                s_ray_ao.key_in_light=vk_ray_normalize3(s_ray_ao.key_light);
            }
        }
    }
    memset(light,0,16);memset(rgb,0,16);
    if(s_ray_ao.key_valid){memcpy(light,s_ray_ao.key_view,12);memcpy(rgb,s_ray_ao.key_rgb,12);rgb[3]=1;}
    // History describes the old key's visibility. Camera reprojection alone
    // cannot make those shadows valid after changing the source light.
    return was_valid!=s_ray_ao.key_valid || (adopt>=0 && adopt!=match);
}
/* DLSS's camera: clip from the world light space (projection times camera). */
static const float *hybrid_dlss_camera(const float projection[16],int have_camera,const float view[12])
{
    static float camera[16];memset(camera,0,sizeof camera);
    if(!have_camera)return camera;
    const float p[4]={projection[0],projection[5],projection[10],projection[14]};
    float p4[16],v4[16];vk_ray_projection4(p,p4);vk_ray_view4(view,v4);vk_ray_mul4(p4,v4,camera);
    return camera;
}
/* Rows mapping this frame's view position to the previous frame's clip
 * space, for temporal reprojection. Without a camera on both frames this is
 * the current projection (same-pixel history) and a camera appearing,
 * vanishing or cutting resets the history. */
static void vk_ray_reprojection(const float projection[16],int have_camera,const float view[12],u64 frame,float rows[16],int *reset)
{
    const float now_p[4]={projection[0],projection[5],projection[10],projection[14]};
    vk_ray_projection4(now_p,rows);*reset=0;
    if(have_camera && s_ray_ao.cam_valid && s_ray_ao.cam_frame+1==frame){
        float maximum=0;
        for(u32 i=0;i<12;i++)maximum=fmaxf(maximum,fabsf(view[i]-s_ray_ao.cam_view[i])/fmaxf(1,fabsf(s_ray_ao.cam_view[i])));
        float now4[16],inverse[16],prev4[16],prev_p[16];
        vk_ray_view4(view,now4);vk_ray_view4(s_ray_ao.cam_view,prev4);vk_ray_projection4(s_ray_ao.cam_projection,prev_p);
        if(maximum>.35f || !vk_ray_invert4(now4,inverse)){*reset=1;return;}
        vk_ray_mul4(prev_p,prev4,rows);vk_ray_mul4(rows,inverse,rows);
        for(u32 i=0;i<16;i++)if(!isfinite(rows[i])){vk_ray_projection4(now_p,rows);*reset=1;return;}
    }else if(have_camera || s_ray_ao.cam_valid)*reset=1;
}
static u32 vk_eng_ray_world(void *user,u32 color,u32 depth,u32 logical_w,u32 logical_h,const float projection[16],u32 remap,u32 rsx_format,float radius,float strength)
{
    (void)user;u64 start=SDL_GetPerformanceCounter(),record_start=0,submit_start=0;u64 token=0;int recorded=0,submitted=0;u32 replacement=0;
    int hybrid_have_camera=0;float hybrid_view[12]={0};u64 hybrid_camera_key=0;u32 dlss_view=0;
    /* The previous frame's asynchronous trace reads every buffer, descriptor
     * and sampler this call rewrites; it has normally finished long ago. */
    vk_async_drain();
    const char *failure="epoch/projection qualification";
    rsx_scene_epoch *epoch=vk_rt_epoch(depth);s_ray_ao.attempted++;
    if(epoch && epoch->reason!=RSX_SCENE_EPOCH_OK){
        switch(epoch->reason){
        case RSX_SCENE_EPOCH_UNSUPPORTED:failure="unsupported depth-writing draw";break;
        case RSX_SCENE_EPOCH_CAPACITY:failure="scene capture capacity";break;
        case RSX_SCENE_EPOCH_RESOURCE_CHANGED:failure="captured resource changed";break;
        case RSX_SCENE_EPOCH_STALE:failure="stale capture epoch";break;
        case RSX_SCENE_EPOCH_NO_CLEAR:failure="no qualified depth clear";break;
        default:failure="invalid capture epoch";break;
        }
    }
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
    if(c->guest_w==logical_w && c->guest_h==logical_h){width=c->w;height=c->h;}
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
    const char *authored_env=getenv("PS3RECOMP_RSX_AUTHORED_LIGHTS");
    const char *hybrid_env=getenv("PS3RECOMP_RSX_HYBRID_PT");
    /* PS3RECOMP_RSX_TRACE=0: capture and rebuild the scene only for DLSS
     * motion vectors, with no ray-traced lighting (DLSS without PT). */
    const char *trace_env=getenv("PS3RECOMP_RSX_TRACE");
    const u32 trace_off=trace_env && !strcmp(trace_env,"0");
    const u32 hybrid_mode=trace_off || (hybrid_env && !strcmp(hybrid_env,"1"));
    const u32 authored_mode=hybrid_mode || (authored_env && !strcmp(authored_env,"1"));
    /* Hybrid lighting is traced once per logical pixel and shared by its
     * MSAA samples; the other modes trace every sample. */
    u32 aw=hybrid_mode?width:authored_mode?width*gx:(width<320?width:320),ah=authored_mode?height:(height<180?height:180);
    const int dlss_ready=hybrid_mode && vk_dlss_prepare(epoch->vertex_count);
    /* Asynchronous transport (default with a second queue; PS3RECOMP_RSX_ASYNC_PT=0
     * disables): this frame composites the previous trace's lighting and its
     * own trace runs on the second queue while the next frame renders. */
    const char *async_env=getenv("PS3RECOMP_RSX_ASYNC_PT");
    u32 async_mode=hybrid_mode && !trace_off && s_async.available && !(async_env && async_env[0]=='0');
    if(async_mode && (s_ray_ao.async_w!=width*gx || s_ray_ao.async_h!=height || s_ray_ao.async_format!=source->fmt || !s_ray_ao.async_color.img)){
        vk_destroy_image(&s_ray_ao.async_color);vk_destroy_image(&s_ray_ao.async_depth);s_ray_ao.async_w=0;
        failure="asynchronous transport copies";
        if(vk_create_image(&s_ray_ao.async_color,source->fmt,width*gx,height,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,VK_IMAGE_ASPECT_COLOR_BIT) ||
           vk_create_image(&s_ray_ao.async_depth,VK_FORMAT_R32_SFLOAT,width*gx,height,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,VK_IMAGE_ASPECT_COLOR_BIT)){
            vk_destroy_image(&s_ray_ao.async_color);vk_destroy_image(&s_ray_ao.async_depth);goto fail;
        }
        s_ray_ao.async_w=width*gx;s_ray_ao.async_h=height;s_ray_ao.async_format=source->fmt;s_ray_ao.async_fresh=1;
    }
    if(trace_off && !dlss_ready){failure="DLSS unavailable for the trace-off scene path";goto fail;}
    // All resources/samplers retire after the prior synchronous submission.
    vk_ray_alpha_reset();
    failure="bounded resident allocation";
    if(!vk_ray_screen(width*gx,height,source->fmt) ||
       !vk_ray_buffer(&s_ray_ao.indices,(VkDeviceSize)triangles*32,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,2,0) ||
       !vk_ray_buffer(&s_ray_ao.materials,(VkDeviceSize)epoch->draw_count*sizeof(vk_ray_material),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,2,0) ||
       !vk_ray_buffer(&s_ray_ao.fp,16u*1024u*1024u,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,2,0) ||
       !vk_ray_buffer(&s_ray_ao.history,(VkDeviceSize)aw*ah*32+16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,0,0) ||
       !vk_ray_buffer(&s_ray_ao.params,sizeof(vk_ray_parameters),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,2,0) ||
       !vk_ray_buffer(&s_ray_ao.triangles,(VkDeviceSize)triangles*36,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,0,1) ||
       !vk_ray_buffer(&s_ray_ao.ao,(VkDeviceSize)aw*ah*40+128,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,0,0) ||
       !vk_ray_buffer(&s_ray_ao.instances,sizeof(VkAccelerationStructureInstanceKHR),VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,1,1) ||
       !vk_ray_buffer(&s_ray_ao.flag_readback,132,VK_BUFFER_USAGE_TRANSFER_DST_BIT,1,0))goto fail;
    failure="batch capture/cache";
    if(rsx_vulkan_scene_batch_begin(s_ray_ao.device_generation,1,&token))goto fail;
    size_t fp_used=0;u32 primitive=0;
    memset(s_ray_ao.authored_lights,0,sizeof s_ray_ao.authored_lights);
    s_ray_ao.authored_count=epoch->draw_count;s_ray_ao.authored_draws=0;s_ray_ao.authored_active_lights=0;
    memset(s_ray_ao.authored_known,0,sizeof s_ray_ao.authored_known);
    memset(s_ray_ao.authored_nonunit,0,sizeof s_ray_ao.authored_nonunit);
    memset(s_ray_ao.authored_recovery_failed,0,sizeof s_ray_ao.authored_recovery_failed);
    memset(s_ray_ao.authored_state_failed,0,sizeof s_ray_ao.authored_state_failed);
    memset(s_ray_ao.authored_texture_failed,0,sizeof s_ray_ao.authored_texture_failed);
    memset(s_ray_ao.authored_ready,0,sizeof s_ray_ao.authored_ready);
    for(u32 i=0;i<epoch->draw_count;i++){
        rsx_scene_draw_input draw;if(!rsx_scene_epoch_get(epoch,i,&draw) || !draw.pipeline || draw.pipeline>VK_ENG_MAX_PIPE)goto fail;
        const vk_eng_pipe *pipeline=&s_epipe[draw.pipeline-1];const rsx_be_render_state *rs=&draw.state;
        failure="draw state/viewport qualification";
        /* Backend capture stores scaled pixel coordinates. The graphics path
         * clamps scissor to the target, including the guest's MSAA-expanded
         * storage extent. An oversized full scissor is the same full raster
         * coverage; an inset or undersized scissor remains unsupported. */
        const int coverage=(rs->antialias_control&0x10u) || (rs->blend_enable && !(rs->antialias_control&0x100u));
        const int standard_blend=!rs->blend_enable || (rs->sf_rgb==0x302u && rs->df_rgb==0x303u && rs->eq_rgb==0x8006u);
        if(!hybrid_mode && (!pipeline->used || !pipeline->scene_vs || !pipeline->scene_ps || !standard_blend || !rsx_scene_stencil_unconditional(rs) || !rs->depth_test || !rs->depth_write || (rs->depth_func!=0x201u && rs->depth_func!=0x203u) ||
           draw.viewport[0]!=0 || draw.viewport[1]!=0 || draw.viewport[2]!=(float)width || draw.viewport[3]!=(float)height || !vk_rt_depth_range(draw.viewport[4],draw.viewport[5]) ||
           draw.scissor[0]!=0 || draw.scissor[1]!=0 || draw.scissor[2]<width || draw.scissor[3]<height)) {
            if(s_ray_ao.rejected<8)VK_LOG("ray AO rejected draw=%u pipeline=%u blend=%u stencil=%u aa=0x%x depth=%u/%u/0x%x viewport=%.1f,%.1f,%.1f,%.1f,%.3f,%.3f scissor=%u,%u,%u,%u expected=%ux%u\n",i,draw.pipeline,rs->blend_enable,rs->stencil_enable,rs->antialias_control,rs->depth_test,rs->depth_write,rs->depth_func,draw.viewport[0],draw.viewport[1],draw.viewport[2],draw.viewport[3],draw.viewport[4],draw.viewport[5],draw.scissor[0],draw.scissor[1],draw.scissor[2],draw.scissor[3],width,height);
            if(s_ray_ao.rejected<8 && !standard_blend)VK_LOG("ray AO unsupported blend factors=0x%x/0x%x equation=0x%x\n",rs->sf_rgb,rs->df_rgb,rs->eq_rgb);
            if(s_ray_ao.rejected<8 && !rsx_scene_stencil_unconditional(rs))VK_LOG("ray AO stencil contents required front=0x%x back=0x%x twoSided=%u mask=0x%x\n",rs->s_func,rs->bs_func,rs->stencil_two_sided,rs->s_func_mask);
            goto fail;
        }
        u32 mask=rs->antialias_control>>16,needed=(1u<<samples)-1;if(!hybrid_mode && (mask&needed)!=needed)goto fail;
        int tested=rs->alpha_test_enable && rs->alpha_func!=0x207u;
        int contract=(tested || coverage || rs->blend_enable)?pipeline->scene_alpha_contract:-1;
        const rsx_scene_alpha_contract *alpha_contract=contract<0?NULL:rsx_scene_alpha_contract_for_id((unsigned)contract);
        failure="exact alpha contract/resources";
        // The generated decompiler emits active OP_KIL as discard; (never
        // predicates emit nothing). Opaque/ALWAYS state cannot hide that kill.
        if(!hybrid_mode && contract<0 && (strstr(pipeline->scene_ps,"discard;") || strstr(pipeline->scene_ps,"clip(")))goto fail;
        if((tested && rs->alpha_func!=0x204u) || ((tested || coverage || rs->blend_enable) &&
            (!alpha_contract || draw.ps_bytes!=pipeline->scene_alpha_fp_bytes || tested!=rsx_scene_alpha_contract_has_threshold(alpha_contract)))){if(hybrid_mode){contract=-1;alpha_contract=NULL;}else goto fail;}
        if(!trace_off && fp_used+draw.ps_bytes>s_ray_ao.fp.size)goto fail;
        // Motion-only DLSS needs captured geometry/depth, not PT materials
        // or authored light recovery. Keep the gather metadata below intact.
        int light_contract=trace_off?-1:rsx_authored__pair(pipeline->scene_vs_sha256,pipeline->scene_ps_sha256);
        u32 position_proof=trace_off?0:vk_ray_unit_position(&draw,light_contract);
        if(light_contract>=0){s_ray_ao.authored_known[light_contract]++;if(!position_proof)s_ray_ao.authored_nonunit[light_contract]++;}
        if(position_proof && rsx_authored_lights_recover(pipeline->scene_vs_sha256,pipeline->scene_ps_sha256,
            (const float(*)[4])draw.vs_constants,draw.vs_bytes,draw.ps_constants,draw.ps_bytes,projection,
            draw.viewport[4],draw.viewport[5],position_proof,&s_ray_ao.authored_lights[i])){
            s_ray_ao.authored_draws++;
            for(unsigned l=0;l<4;l++)s_ray_ao.authored_active_lights+=s_ray_ao.authored_lights[i].lights[l].status==RSX_AUTHORED_ACTIVE;
        }
        else if(light_contract>=0 && position_proof)s_ray_ao.authored_recovery_failed[light_contract]++;
        vk_ray_material material={{contract<0?UINT32_MAX:(u32)contract,(u32)(fp_used/16),(u32)(draw.ps_bytes/16),draw.texture_mask},{rs->cull_enable,rs->cull_face,rs->front_face==0x901u,(u32)coverage},{UINT32_MAX,UINT32_MAX,0,0},{draw.viewport[4],draw.viewport[5],0,0},{0},{0},{{0}},{{0}}};
        /* A depth-only prepass is shadow geometry, never the RGB owner.
         * Preserve partial masks as possible ambiguous color writers. */
        material.authored[1]=rs->color_mask&0x00ffffffu;
        if(!trace_off && contract>=0)for(u32 unit=0;unit<2;unit++)if(pipeline->scene_alpha_mask&(1u<<unit))if(!(draw.texture_mask&(1u<<unit)) || !vk_ray_alpha_texture(draw.textures[unit],&draw.samplers[unit],&material.textures[unit])){if(hybrid_mode)material.alpha[0]=UINT32_MAX;else goto fail;}
        /* Exact recovered source pairs only. Unknown/resource failures keep
         * this material's exact original raster RGB, without omitting geometry. */
        const rsx_authored_lights *a=&s_ray_ao.authored_lights[i];
        u32 rgb_mask=light_contract==0?7u:light_contract==1?127u:31u,rgb_count=light_contract==0?3u:light_contract==1?7u:5u;
        if(authored_mode && light_contract>=0 && a->lights[0].status!=RSX_AUTHORED_UNAVAILABLE &&
           (rs->color_mask&255u) && ((rs->color_mask>>8)&255u) && ((rs->color_mask>>16)&255u) &&
           !rs->blend_enable && !coverage && (draw.texture_mask&rgb_mask)==rgb_mask){
            int ready=1;for(u32 u=0;u<rgb_count&&ready;u++)if(!vk_ray_texture(draw.textures[u],&draw.samplers[u],&material.rgb_textures[u],light_contract==1&&u==3))ready=0;
            if(ready){s_ray_ao.authored_ready[light_contract]++;memcpy(material.light_to_view,a->light_to_view,sizeof material.light_to_view);material.authored[2]=rsx_authored__hash(pipeline->scene_ps_sha256,"7dd199ec45df75fe28c630e58143257605cf911e84c98f0841f31db7d1fc8fe4");material.authored[0]=(u32)light_contract+1;for(u32 l=0;l<4;l++){memcpy(material.light_view[l],a->lights[l].view,16);memcpy(material.light_rgb[l],a->lights[l].rgb,12);material.light_rgb[l][3]=s_ray_ao.test_disable_authored_sources?0:(float)a->lights[l].status;}}
            else s_ray_ao.authored_texture_failed[light_contract]++;
        }
        else if(authored_mode && light_contract>=0 && a->lights[0].status!=RSX_AUTHORED_UNAVAILABLE)s_ray_ao.authored_state_failed[light_contract]++;
        if(hybrid_mode && !trace_off){
            /* Generic base selection: avoid two-channel normal maps and LUTs;
             * prefer a color-sized unit 1 when unit 0 is the normal source. */
            int unit=-1;u64 score=0;
            for(u32 u=0;u<2;u++)if(draw.texture_mask&(1u<<u)){
                const vk_eng_obj *t=vk_eng_get(draw.textures[u],VK_ENG_TEXTURE);
                if(!t || t->cube || t->surface || t->w<16 || t->h<16 || t->fmt==VK_FORMAT_BC5_UNORM_BLOCK || t->fmt==VK_FORMAT_BC5_SNORM_BLOCK)continue;
                u64 value=(u64)t->w*t->h*(u==1?2u:1u);if(value>score){score=value;unit=(int)u;}
            }
            material.textures[2]=UINT32_MAX;
            if(unit>=0)vk_ray_texture(draw.textures[unit],&draw.samplers[unit],&material.textures[2],0);
            material.textures[3]=(u32)(unit<0?0:unit);
            material.depth_range[2]=rs->alpha_test_enable?(float)rs->alpha_ref_raw/255.0f:0.0f;
            /* Cutouts keep their holes in shadow rays (hybridCutoutKeep):
             * alpha-tested GREATER/GEQUAL draws at their reference, and
             * alpha-to-coverage draws (Faerypool's leaf cards) at half
             * coverage. Solid quads there made the canopy one opaque roof. */
            const int cutout_test=rs->alpha_test_enable && (rs->alpha_func==0x204u || rs->alpha_func==0x206u);
            const int to_coverage=(rs->antialias_control&0x10u)!=0;
            if((cutout_test || to_coverage) && material.textures[2]<VK_RAY_TEXTURES){
                material.textures[3]|=0x200u;
                if(!cutout_test)material.depth_range[2]=.5f;
                else if(to_coverage)material.depth_range[2]=fmaxf(material.depth_range[2],.5f);
            }
            material.depth_range[3]=(float)epoch->frame;
            /* An exact pool source explicitly carries a cube reflection. */
            if(light_contract==1 || strstr(pipeline->scene_ps,"TextureCube "))material.textures[3]|=0x100u;
            if(a->lights[0].status!=RSX_AUTHORED_UNAVAILABLE)for(u32 l=0;l<4;l++){
                memcpy(material.light_view[l],a->lights[l].view,16);memcpy(material.light_rgb[l],a->lights[l].rgb,12);
                material.light_rgb[l][3]=(float)a->lights[l].status;
            }
        }
        if(!trace_off){
            if(draw.ps_bytes)memcpy((unsigned char*)s_ray_ao.fp.ptr+fp_used,draw.ps_constants,draw.ps_bytes);
            fp_used+=draw.ps_bytes;
        }
        // Exact captured source supplies h, avoiding projection cancellation.
        // Scale metadata is used only after the full shared-position proof.
        /* Hybrid geometry comes from the captured post-transform clip output,
         * including the actual projection/depth normalization. Source skin
         * weights are not a substitute for that captured homogeneous scale. */
        material.authored[3]=!hybrid_mode && position_proof && a->lights[0].status!=RSX_AUTHORED_UNAVAILABLE;
        ((vk_ray_material*)s_ray_ao.materials.ptr)[i]=material;
        rsx_scene_batch_input input={pipeline->scene_vs,draw.layout,draw.vertices,(size_t)draw.vertex_count*draw.layout.stride,draw.vertex_count,draw.vs_constants,draw.vs_bytes};u32 base=0;
        failure="batch immutable upload/cache";
        if(rsx_vulkan_scene_batch_append(token,&input,&base))goto fail;
        for(u32 j=0;j<draw.index_count;j+=3){
            u32 *destination=(u32*)s_ray_ao.indices.ptr+(size_t)primitive*4;
            u32 *scales=(u32*)s_ray_ao.indices.ptr+((size_t)triangles+primitive)*4;
            for(u32 k=0;k<3;k++){u32 local=draw.indices[j+k];destination[k]=base+local;
                float h=material.authored[3]?vk_ray_source_h(&draw,light_contract,local):0;
                memcpy(&scales[k],&h,4);}
            destination[3]=i;scales[3]=0;primitive++;
        }
    }
    if(hybrid_mode && !trace_off){
        vk_ray_material *all=(vk_ray_material*)s_ray_ao.materials.ptr;int light_source=-1;
        for(u32 i=0;i<epoch->draw_count && light_source<0;i++)for(u32 l=0;l<4;l++)
            if(all[i].light_rgb[l][3]==1.0f && all[i].light_view[l][3]==0.0f){light_source=(int)i;break;}
        if(light_source>=0)for(u32 i=0;i<epoch->draw_count;i++){
            int active=0;for(u32 l=0;l<4;l++)active|=all[i].light_rgb[l][3]==1.0f;
            if(!active){memcpy(all[i].light_view,all[light_source].light_view,sizeof all[i].light_view);memcpy(all[i].light_rgb,all[light_source].light_rgb,sizeof all[i].light_rgb);}
        }
    }
    u32 hybrid_fog_index=(u32)(fp_used/16);
    if(hybrid_mode)hybrid_have_camera=vk_ray_frame_camera(epoch,hybrid_view,&hybrid_camera_key);
    if(hybrid_mode && !trace_off){
        float fog[4]={.08f,.08f,.08f,1.0f};int found=0;
        vk_ray_material *all=(vk_ray_material*)s_ray_ao.materials.ptr;
        for(u32 i=0;i<epoch->draw_count && !found;i++){
            u32 offset=all[i].authored[0]==3u?12u:all[i].authored[0]==2u?25u:UINT32_MAX;
            if(offset<all[i].alpha[2]){
                const float *c=(const float*)s_ray_ao.fp.ptr+(all[i].alpha[1]+offset)*4;
                if(isfinite(c[0]) && isfinite(c[1]) && isfinite(c[2])){
                    memcpy(fog,c,12);found=1;
                }
            }
        }
        if(fp_used+16>s_ray_ao.fp.size)goto fail;
        memcpy((u8*)s_ray_ao.fp.ptr+fp_used,fog,16);fp_used+=16;
        float jitter[4]={0};vk_dlss_jitter(jitter);
        if(fp_used+16>s_ray_ao.fp.size)goto fail;
        memcpy((u8*)s_ray_ao.fp.ptr+fp_used,jitter,16);fp_used+=16;
        /* fp+2 key light (view space, w=0 directional), fp+3 its radiance
         * (w=1 valid), fp+4..7 reprojection rows, fp+8 (0, bank, reset, 0). */
        float block[7][4];memset(block,0,sizeof block);
        const int light_changed=vk_ray_select_key(epoch->draw_count,hybrid_have_camera,hybrid_view,block[0],block[1]);
        int reset=0;vk_ray_reprojection(projection,hybrid_have_camera,hybrid_view,epoch->frame,&block[2][0],&reset);
        if(light_changed)reset=1;
        if(s_ray_ao.history_w!=aw || s_ray_ao.history_h!=ah)reset=1;
        // With no usable history, trace this frame before compositing it.
        // Subsequent continuous frames return to asynchronous transport.
        // This removes the initial raster-only frame without stale lighting
        // after a camera cut or resolution change.
        if(reset)async_mode=0;
        block[6][1]=(float)s_ray_ao.history_bank;block[6][2]=reset?1.0f:0.0f;
        /* Directional shadow reach in world units (Folklore: a point light's
         * authored range is 19..94 units; the camera sits ~50 units back). */
        const char *range_env=getenv("PS3RECOMP_RSX_SHADOW_RANGE");float range=range_env?strtof(range_env,NULL):150.0f;
        block[6][3]=isfinite(range)&&range>0.0f&&range<=100000.0f?range:150.0f;
        if(fp_used+sizeof block>s_ray_ao.fp.size)goto fail;
        memcpy((u8*)s_ray_ao.fp.ptr+fp_used,block,sizeof block);fp_used+=sizeof block;
    }
    if(primitive!=triangles || !rsx_scene_epoch_seal(epoch,s_rt_capture.frame,epoch->serial))goto fail;
    /* Opaque shaders can have no fragment constants. Vulkan still requires
     * a nonzero descriptor range even when the shader never accesses it. */
    if(!fp_used){memset(s_ray_ao.fp.ptr,0,16);fp_used=16;}
    const char *lighting=getenv("PS3RECOMP_RSX_PATH_PREVIEW");
    const u32 path_preview=!authored_mode && lighting && !strcmp(lighting,"1");
    const char *denoise_setting=getenv("PS3RECOMP_RSX_RAY_DENOISE");
    const u32 denoise=(hybrid_mode || !authored_mode) && (denoise_setting?!strcmp(denoise_setting,"spatial"):path_preview);
    const char *transport_mode=hybrid_mode?"hybrid-path-tracing":authored_mode?"authored-source-preview":path_preview?"diffuse-path-preview":"ray-AO";
    vk_ray_parameters parameters={{projection[0],projection[5],projection[10],projection[14]},{aw,ah,width,height},{triangles,epoch->vertex_count,gx,(hybrid_mode?32u:0u)|path_preview|(denoise?2u:0u)|(authored_mode?4u:0u)|(s_ray_ao.test_disable_authored_bounce?8u:0u)|(s_ray_ao.test_disable_authored_reflections?0u:16u)},{radius,fmaxf(.00001f,radius*.001f),strength,.005f}};
    if(authored_mode && !hybrid_mode && s_ray_ao.traced==0)VK_LOG("authored-source preview: contract0/pool91/terrain90 exact source subsets; full sample resolution; per-source shadows plus one colored bounce using bounded source diffuse proxies at both surfaces, source-directed mirror reflections on qualified hits, original cube on misses; no physical BRDF/roughness/emission; explicit cone LOD plus primary raster agreement; per-vertex source homogeneous scales with translated per-hit light correction; unknown materials unchanged; NOT full PT or DLSS\n");
    if(hybrid_mode && !trace_off && s_ray_ao.traced==0)VK_LOG("Hybrid path tracing (raster primary, traced shadows/bounce/reflections, generic materials where the original shader is not yet decoded)\n");
    if(hybrid_mode)parameters.settings[3]=(float)hybrid_fog_index;
    if(path_preview && s_ray_ao.traced==0)VK_LOG("diffuse path preview: current-frame geometry, 8 paths/pixel, 4 diffuse bounces, neutral secondary reflectance 0.65, uniform environment; original primary color retained; NOT recovered physical game materials/lights\n");
    if(s_ray_ao.traced==0)VK_LOG("ray transport mode=%s spatialFilterPasses=%u temporalHistory=%s DLSSRayReconstruction=off\n",
        trace_off?"dlss-scene-only":transport_mode,trace_off||!denoise?0u:hybrid_mode?2u:3u,
        hybrid_mode&&!trace_off?"camera-reprojected-custom":"off");
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
    if(s_ray_ao.queries){s_ray_ao.reset_queries(s_vk.cmd,s_ray_ao.queries,0,8);vk_ray_stamp(0,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);}
    rsx_scene_batch_output batch;if(rsx_vulkan_scene_batch_record(token,s_vk.cmd,s_vk.fence,&batch))goto abandon;recorded=1;
    if(hybrid_mode){
        /* Invalid post-VS vertices were replaced with zero. Gather discards
         * those triangles locally instead of discarding every other draw. */
        vk_ray_memory(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
        s_ray_ao.fill_buffer(s_vk.cmd,batch.validity,0,4,0);
        vk_ray_memory(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
    }
    vk_ray_stamp(1,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vk_eng_obj *out=vk_eng_get(s_ray_ao.color,VK_ENG_COLOR);if(!out)goto abandon;
    // Discard only the private output's old contents; guest color stays intact.
    vk_barrier_image(out->im.img,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED);
    vk_barrier_image(s_eobj[s_ray_ao.raw_color].im.img,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED);
    VkDescriptorBufferInfo bi[8]={{batch.vertices,0,batch.vertex_bytes},{s_ray_ao.indices.buf,0,(VkDeviceSize)triangles*32},{s_ray_ao.triangles.buf,0,(VkDeviceSize)triangles*36},{batch.validity,0,4},
        {s_ray_ao.materials.buf,0,(VkDeviceSize)epoch->draw_count*sizeof(vk_ray_material)},{s_ray_ao.fp.buf,0,fp_used},{s_ray_ao.ao.buf,0,(VkDeviceSize)aw*ah*40+128},{s_ray_ao.params.buf,0,sizeof parameters}};
    VkWriteDescriptorSet writes[50];u32 wn=0;for(u32 i=0;i<8;i++)writes[wn++]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=s_ray_ao.set,.dstBinding=i,.descriptorCount=1,.descriptorType=i==7?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&bi[i]};
    VkWriteDescriptorSetAccelerationStructureKHR wa={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,.accelerationStructureCount=1,.pAccelerationStructures=&s_ray_ao.tlas};
    writes[wn++]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.pNext=&wa,.dstSet=s_ray_ao.set,.dstBinding=8,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};
    VkDescriptorImageInfo images[3]={{s_vk.sampler,async_mode?s_ray_ao.async_color.view:source->im.view,VK_IMAGE_LAYOUT_GENERAL},
        {s_vk.sampler,async_mode?s_ray_ao.async_depth.view:snapshot->im.view,VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,out->im.view,VK_IMAGE_LAYOUT_GENERAL}};
    for(u32 i=0;i<3;i++)writes[wn++]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=s_ray_ao.set,.dstBinding=9+i,.descriptorCount=1,.descriptorType=i==2?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&images[i]};
    VkDescriptorImageInfo raw_image={VK_NULL_HANDLE,s_eobj[s_ray_ao.raw_color].im.view,VK_IMAGE_LAYOUT_GENERAL};
    writes[wn++]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=s_ray_ao.set,.dstBinding=12,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,.pImageInfo=&raw_image};
    for(u32 i=0;i<VK_RAY_ALL_TEXTURES;i++)writes[wn++]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=s_ray_ao.set,.dstBinding=16+i,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&s_ray_ao.alpha_images[i]};
    /* Two history banks per trace pixel plus the smoothed sky slot. */
    VkDescriptorBufferInfo history_info={s_ray_ao.history.buf,0,(VkDeviceSize)aw*ah*32+16};
    writes[wn++]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=s_ray_ao.set,.dstBinding=13,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&history_info};
    pvkUpdateDescriptorSets(s_vk.device,wn,writes,0,NULL);
    if(s_ray_ao.history_w!=aw || s_ray_ao.history_h!=ah){
        s_ray_ao.fill_buffer(s_vk.cmd,s_ray_ao.history.buf,0,(VkDeviceSize)aw*ah*32+16,0);
        s_ray_ao.history_w=aw;s_ray_ao.history_h=ah;
    }
    s_ray_ao.fill_buffer(s_vk.cmd,s_ray_ao.ao.buf,(VkDeviceSize)aw*ah*40,128,0);
    vk_ray_memory(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
    vk_ray_memory(VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_UNIFORM_READ_BIT|VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    pvkCmdBindDescriptorSets(s_vk.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s_ray_ao.pipeline_layout,0,1,&s_ray_ao.set,0,NULL);pvkCmdBindPipeline(s_vk.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s_ray_ao.gather);s_ray_ao.dispatch(s_vk.cmd,(triangles+63)/64,1,1);
    vk_ray_memory(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);vk_ray_stamp(2,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    VkAccelerationStructureBuildRangeInfoKHR br={.primitiveCount=triangles},tr={.primitiveCount=1};const VkAccelerationStructureBuildRangeInfoKHR *brp=&br,*trp=&tr;
    s_ray_ao.build(s_vk.cmd,1,&bb,&brp);vk_ray_memory(VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    s_ray_ao.build(s_vk.cmd,1,&tb,&trp);vk_ray_memory(VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);vk_ray_stamp(3,VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);
    u32 mode=0;
    if(async_mode){
        /* Copies for the asynchronous trace, then this frame's raster with
         * the previous trace's lighting (hybridLateComposite). */
        vk_barrier_image(s_ray_ao.async_color.img,VK_IMAGE_ASPECT_COLOR_BIT,s_ray_ao.async_fresh?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_GENERAL);
        vk_barrier_image(s_ray_ao.async_depth.img,VK_IMAGE_ASPECT_COLOR_BIT,s_ray_ao.async_fresh?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_GENERAL);
        s_ray_ao.async_fresh=0;
        vk_barrier_image(source->im.img,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL);
        vk_barrier_image(snapshot->im.img,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL);
        VkImageCopy region={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.extent={width*gx,height,1}};
        pvkCmdCopyImage(s_vk.cmd,source->im.img,VK_IMAGE_LAYOUT_GENERAL,s_ray_ao.async_color.img,VK_IMAGE_LAYOUT_GENERAL,1,&region);
        pvkCmdCopyImage(s_vk.cmd,snapshot->im.img,VK_IMAGE_LAYOUT_GENERAL,s_ray_ao.async_depth.img,VK_IMAGE_LAYOUT_GENERAL,1,&region);
        vk_ray_memory(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
        pvkCmdBindPipeline(s_vk.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s_ray_ao.trace[source->fmt==VK_FORMAT_R16G16B16A16_SFLOAT]);
        mode=8;pvkCmdPushConstants(s_vk.cmd,s_ray_ao.pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&mode);s_ray_ao.dispatch(s_vk.cmd,(width*gx+7)/8,(height+7)/8,1);
        vk_ray_memory(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
        vk_ray_stamp(4,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }else if(!trace_off){
        pvkCmdBindPipeline(s_vk.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s_ray_ao.trace[source->fmt==VK_FORMAT_R16G16B16A16_SFLOAT]);
        if(hybrid_mode){
            /* 7: smoothed sky estimate, read by every trace below. */
            mode=7;pvkCmdPushConstants(s_vk.cmd,s_ray_ao.pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&mode);s_ray_ao.dispatch(s_vk.cmd,1,1,1);
            vk_ray_memory(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
        }
        mode=0;pvkCmdPushConstants(s_vk.cmd,s_ray_ao.pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&mode);s_ray_ao.dispatch(s_vk.cmd,(aw+7)/8,(ah+7)/8,1);
        vk_ray_memory(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);vk_ray_stamp(4,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        mode=5;pvkCmdPushConstants(s_vk.cmd,s_ray_ao.pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&mode);
        s_ray_ao.dispatch(s_vk.cmd,1,1,1);
        vk_ray_memory(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
        /* Hybrid filters twice (steps 1, 2: planes 0->5->0); the scalar modes three times. */
        if(denoise)for(mode=2;mode<=(hybrid_mode?3u:4u);mode++){
            pvkCmdPushConstants(s_vk.cmd,s_ray_ao.pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&mode);
            s_ray_ao.dispatch(s_vk.cmd,(aw+7)/8,(ah+7)/8,1);
            vk_ray_memory(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
        }
        if(hybrid_mode){
            /* 6: temporal accumulation (plane 0 -> plane 5 and history). */
            mode=6;pvkCmdPushConstants(s_vk.cmd,s_ray_ao.pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&mode);s_ray_ao.dispatch(s_vk.cmd,(aw+7)/8,(ah+7)/8,1);
            vk_ray_memory(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
        }
        mode=1;pvkCmdPushConstants(s_vk.cmd,s_ray_ao.pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&mode);s_ray_ao.dispatch(s_vk.cmd,(width*gx+7)/8,(height+7)/8,1);
        vk_ray_memory(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
    }else vk_ray_stamp(4,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vk_ray_stamp(5,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    /* DLSS joins this submission: input from the traced composite (or the
     * raw world when tracing is off), recorded before the one wait below. */
    if(dlss_ready){
        dlss_view=vk_dlss_record(trace_off?source_color:s_ray_ao.color,source_depth,batch.vertices,batch.vertex_bytes,epoch,projection,
                                 hybrid_dlss_camera(projection,hybrid_have_camera,hybrid_view),hybrid_have_camera,hybrid_camera_key,remap,rsx_format);
        if(dlss_view==UINT32_MAX){dlss_view=0;failure="DLSS evaluation recording";goto abandon;}
    }
    vk_ray_stamp(6,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    // Existing synchronous boundary: validity, four query counters, and the
    // first alpha and depth failures' bounded diagnostic fields. No vertex readback.
    vk_ray_memory(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferCopy flag_copy={.size=4};
    VkBufferCopy counter_copy={.srcOffset=(VkDeviceSize)aw*ah*40,.dstOffset=4,.size=128};
    if(!async_mode){
        s_ray_ao.copy_buffer(s_vk.cmd,batch.validity,s_ray_ao.flag_readback.buf,1,&flag_copy);
        s_ray_ao.copy_buffer(s_vk.cmd,s_ray_ao.ao.buf,s_ray_ao.flag_readback.buf,1,&counter_copy);
    }
    vk_ray_memory(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
    failure="synchronous integration submission";submit_start=SDL_GetPerformanceCounter();
    if(async_mode)s_async.attach=1;
    if(vk_submit_and_wait())goto abandon;
    vk_dlss_complete(1,trace_off?source_color:s_ray_ao.color);
    if(async_mode){
        /* The trace for this frame: sky, trace, filter, temporal (history
         * bank for the next frame's late composite), counters. */
        VkCommandBuffer main_cmd=s_vk.cmd;s_vk.cmd=s_async.cmd;
        VkCommandBufferBeginInfo begin_info={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        int ok=pvkResetCommandBuffer(s_async.cmd,0)==VK_SUCCESS && pvkBeginCommandBuffer(s_async.cmd,&begin_info)==VK_SUCCESS;
        if(ok){
            vk_ray_memory(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT);
            pvkCmdBindDescriptorSets(s_vk.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s_ray_ao.pipeline_layout,0,1,&s_ray_ao.set,0,NULL);
            pvkCmdBindPipeline(s_vk.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s_ray_ao.trace[source->fmt==VK_FORMAT_R16G16B16A16_SFLOAT]);
            static const u32 sequence[]={7,0,5,2,3,6};
            for(u32 i=0;i<sizeof sequence/sizeof sequence[0];i++){
                mode=sequence[i];
                if((mode==2 || mode==3) && !denoise)continue;
                pvkCmdPushConstants(s_vk.cmd,s_ray_ao.pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&mode);
                if(mode==7 || mode==5)s_ray_ao.dispatch(s_vk.cmd,1,1,1);else s_ray_ao.dispatch(s_vk.cmd,(aw+7)/8,(ah+7)/8,1);
                vk_ray_memory(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
            }
            vk_ray_memory(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
            s_ray_ao.copy_buffer(s_vk.cmd,batch.validity,s_ray_ao.flag_readback.buf,1,&flag_copy);
            s_ray_ao.copy_buffer(s_vk.cmd,s_ray_ao.ao.buf,s_ray_ao.flag_readback.buf,1,&counter_copy);
            vk_ray_memory(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT|VK_ACCESS_MEMORY_READ_BIT);
            ok=pvkEndCommandBuffer(s_vk.cmd)==VK_SUCCESS;
        }
        s_vk.cmd=main_cmd;
        /* Submitted after this frame's present (vk_async_kick); even a failed
         * recording is submitted, to consume the ready signal. */
        s_async.recorded=1;s_async.recorded_ok=ok;if(ok)s_ray_ao.async_frames++;
    }
    submitted=1;if(rsx_vulkan_scene_batch_mark_submitted(token) || rsx_vulkan_scene_batch_reset(token))goto fail;recorded=0;token=0;
    memcpy(&s_ray_ao.last_flags,s_ray_ao.flag_readback.ptr,4);s_ray_ao.traced++;
    memcpy(s_ray_ao.gpu_stats,(unsigned char*)s_ray_ao.flag_readback.ptr+4,128);
    if(hybrid_mode){s_ray_ao.hybrid_nonfinite+=s_ray_ao.gpu_stats[6];if(s_ray_ao.gpu_stats[0] && !s_ray_ao.gpu_stats[7])s_ray_ao.hybrid_black_frames++;}
    if((s_ray_ao.last_flags&1u) && s_ray_ao.gpu_stats[31] && s_ray_ao.invalid_position<8){
        float values[10];memcpy(values,&s_ray_ao.gpu_stats[14],sizeof values);
        VK_LOG("ray gather position failure primitive=%u vertex=%u material=%u clip=%g,%g,%g,%g sourceH=%g depthRange=%g,%g depth=%g predicted=%g recoveredH=%g\n",
            s_ray_ao.gpu_stats[31]-1,s_ray_ao.gpu_stats[12],s_ray_ao.gpu_stats[13],values[0],values[1],values[2],values[3],values[4],values[5],values[6],values[7],values[8],values[9]);
    }
    if(s_ray_ao.last_flags&1u)s_ray_ao.invalid_position++;if(s_ray_ao.last_flags&4u)s_ray_ao.invalid_alpha++;if(s_ray_ao.last_flags&8u)s_ray_ao.invalid_depth++;if(s_ray_ao.last_flags&16u)s_ray_ao.invalid_numeric++;
    if(authored_mode && !hybrid_mode)VK_LOG("authored source frame=%llu qualifiedDraws=%u/%u rasterAgreed=%u changedSamples=%u coloredBounceSamples=%u ambiguousOwners=%u shadowedSources=%u reflectionDirections=%u reflectedHits=%u\n",(unsigned long long)epoch->frame,s_ray_ao.authored_draws,epoch->draw_count,s_ray_ao.gpu_stats[24],s_ray_ao.gpu_stats[25],s_ray_ao.gpu_stats[26],s_ray_ao.gpu_stats[27],s_ray_ao.gpu_stats[28],s_ray_ao.gpu_stats[29],s_ray_ao.gpu_stats[30]);
    if(!s_ray_ao.gpu_stats[1] || !s_ray_ao.gpu_stats[2])s_ray_ao.empty_ao++;
    if(hybrid_mode){
        /* The decision is on the GPU: a rejected gather/trace frame composited
         * the previous frame's reprojected lighting (hybridTemporal). */
        if(s_ray_ao.last_flags&~2u)s_ray_ao.held_frames++;
        if(dlss_view){replacement=dlss_view;s_ray_ao.dlss_frames++;}
        else if(!trace_off){out->scale=scale;out->grid_x=gx;out->grid_y=1;replacement=vk_eng_surface_view(NULL,s_ray_ao.color,remap,rsx_format);}
        if(!trace_off){
            s_ray_ao.history_bank^=1u;s_ray_ao.cam_valid=hybrid_have_camera;s_ray_ao.cam_frame=epoch->frame;
            memcpy(s_ray_ao.cam_view,hybrid_view,sizeof s_ray_ao.cam_view);
            s_ray_ao.cam_projection[0]=projection[0];s_ray_ao.cam_projection[1]=projection[5];s_ray_ao.cam_projection[2]=projection[10];s_ray_ao.cam_projection[3]=projection[14];
        }
        if(replacement)s_ray_ao.completed++;
    }
    else if(!(s_ray_ao.last_flags&~2u) && s_ray_ao.gpu_stats[1] && (authored_mode?s_ray_ao.gpu_stats[24]:s_ray_ao.gpu_stats[2])){out->scale=scale;out->grid_x=gx;out->grid_y=1;replacement=vk_eng_surface_view(NULL,s_ray_ao.color,remap,rsx_format);if(replacement)s_ray_ao.completed++;}
    if(!replacement)s_ray_ao.rejected++;
    int gpu_timing_valid=0,dlss_prep_valid=0;
    memset(s_ray_ao.gpu_ms,0,sizeof s_ray_ao.gpu_ms);
    if(s_ray_ao.queries){u64 times[7];gpu_timing_valid=s_ray_ao.query_results(s_vk.device,s_ray_ao.queries,0,7,sizeof times,times,8,VK_QUERY_RESULT_64_BIT)==VK_SUCCESS;
        if(gpu_timing_valid){u64 mask_bits=s_ray_ao.timestamp_bits>=64?UINT64_MAX:(1ull<<s_ray_ao.timestamp_bits)-1;for(unsigned i=0;i<6;i++)s_ray_ao.gpu_ms[i]=(double)((times[i+1]-times[i])&mask_bits)*s_ray_ao.properties.limits.timestampPeriod/1000000.0;}
        /* DLSS split: input/motion preparation, then NGX evaluation and output. */
        u64 prepared;if(gpu_timing_valid && dlss_view && s_ray_ao.query_results(s_vk.device,s_ray_ao.queries,7,1,sizeof prepared,&prepared,8,VK_QUERY_RESULT_64_BIT)==VK_SUCCESS){
            u64 mask_bits=s_ray_ao.timestamp_bits>=64?UINT64_MAX:(1ull<<s_ray_ao.timestamp_bits)-1;
            s_ray_ao.gpu_ms[6]=(double)((prepared-times[5])&mask_bits)*s_ray_ao.properties.limits.timestampPeriod/1000000.0;
            dlss_prep_valid=1;
        }else s_ray_ao.gpu_ms[6]=0;}
    s_ray_ao.cpu_ms=(double)(SDL_GetPerformanceCounter()-start)*1000.0/(double)SDL_GetPerformanceFrequency();
    if(s_ray_ao.traced<=4 || s_ray_ao.traced%120==0 || (!replacement && s_ray_ao.rejected<=8))
        VK_LOG("ray GPU timing frame=%llu stageSamplesAvailable=%d dlssPrepAvailable=%d unavailableValues=zero-not-measured\n",
            (unsigned long long)epoch->frame,gpu_timing_valid,dlss_prep_valid);
    if(hybrid_mode && !trace_off && SDL_GetTicks64()-s_ray_ao.last_hybrid_log>=1000){
        u32 world=s_ray_ao.gpu_stats[0],traced=s_ray_ao.gpu_stats[24],hits=s_ray_ao.gpu_stats[5];
        VK_LOG("hybrid PT frame=%llu world=%u traced=%u (%.2f%%) rasterFallback=%u genericHits=%.2f%% bounceHits=%u reflectionHits=%u shadows=%u primaryShadowed=%.1f%% far=%.1f PT=%.3fms flags=0x%x reflectionRays=%u nonfinite=%u nonblack=%u owners=%u families=%u/%u/%u/%u\n",
            (unsigned long long)epoch->frame,world,traced,world?100.0*traced/world:0.0,world-traced,
            hits?100.0*s_ray_ao.gpu_stats[4]/hits:0.0,s_ray_ao.gpu_stats[26],s_ray_ao.gpu_stats[30],s_ray_ao.gpu_stats[28],
            s_ray_ao.gpu_stats[13]+s_ray_ao.gpu_stats[14]?100.0*s_ray_ao.gpu_stats[14]/(s_ray_ao.gpu_stats[13]+s_ray_ao.gpu_stats[14]):0.0,
            projection[10]>1.0f?(double)(projection[14]/(1.0f-projection[10])):0.0,
            s_ray_ao.gpu_ms[1]+s_ray_ao.gpu_ms[2]+s_ray_ao.gpu_ms[3]+s_ray_ao.gpu_ms[4],s_ray_ao.last_flags,s_ray_ao.gpu_stats[29],s_ray_ao.gpu_stats[6],s_ray_ao.gpu_stats[7],s_ray_ao.gpu_stats[8],s_ray_ao.gpu_stats[9],s_ray_ao.gpu_stats[10],s_ray_ao.gpu_stats[11],s_ray_ao.gpu_stats[12]);
        s_ray_ao.last_hybrid_log=SDL_GetTicks64();
        VK_LOG("hybrid PT cumulative nonfinite=%llu blackWorldFrames=%llu completedFrames=%llu; max diffuse bounces=1 max specular rays/path=1\n",(unsigned long long)s_ray_ao.hybrid_nonfinite,(unsigned long long)s_ray_ao.hybrid_black_frames,(unsigned long long)s_ray_ao.completed);
    }
    double copy_ms=(double)(s_rt_capture.copy_ticks-s_ray_ao.last_copy_ticks)*1000.0/(double)SDL_GetPerformanceFrequency();
    u64 copied=s_rt_capture.copied_bytes-s_ray_ao.last_copy_bytes;s_ray_ao.last_copy_ticks=s_rt_capture.copy_ticks;s_ray_ao.last_copy_bytes=s_rt_capture.copied_bytes;
    if(s_ray_ao.traced<=4 || s_ray_ao.traced%120==0 || (!replacement && s_ray_ao.rejected<=8))VK_LOG("ray transport mode=%s denoiser=%s frame=%llu epoch=%llu requested=%llu traced=%llu validFrames=%llu fallback=%llu flags=0x%x reasonCounts(pos/alpha/depth/numeric)=%llu/%llu/%llu/%llu draws=%u vertices=%u triangles=%u traceGrid=%ux%u paths=%u maxBounces=%u alphaLOD=ray-cone copied=%llu copyCPU=%.3fms totalCPU=%.3fms prepare/record/wait=%.3f/%.3f/%.3fms GPU(VS/gather/AS/trace/composite/DLSS)=%.3f/%.3f/%.3f/%.3f/%.3f/%.3fms dlssPrep=%.3fms heldFrames=%llu dlssFrames=%llu key=%d timestampBits=%u\n",trace_off?"dlss-scene-only":async_mode?"hybrid-path-tracing-async":transport_mode,denoise?"spatial":"off",(unsigned long long)epoch->frame,(unsigned long long)epoch->generation,(unsigned long long)s_ray_ao.attempted,(unsigned long long)s_ray_ao.traced,(unsigned long long)s_ray_ao.completed,(unsigned long long)s_ray_ao.rejected,s_ray_ao.last_flags,(unsigned long long)s_ray_ao.invalid_position,(unsigned long long)s_ray_ao.invalid_alpha,(unsigned long long)s_ray_ao.invalid_depth,(unsigned long long)s_ray_ao.invalid_numeric,epoch->draw_count,epoch->vertex_count,triangles,aw,ah,authored_mode?1u:path_preview?8u:4u,authored_mode?1u:path_preview?4u:1u,(unsigned long long)copied,copy_ms,s_ray_ao.cpu_ms,(double)(record_start-start)*1000.0/(double)SDL_GetPerformanceFrequency(),(double)(submit_start-record_start)*1000.0/(double)SDL_GetPerformanceFrequency(),(double)(SDL_GetPerformanceCounter()-submit_start)*1000.0/(double)SDL_GetPerformanceFrequency(),s_ray_ao.gpu_ms[0],s_ray_ao.gpu_ms[1],s_ray_ao.gpu_ms[2],s_ray_ao.gpu_ms[3],s_ray_ao.gpu_ms[4],s_ray_ao.gpu_ms[5],s_ray_ao.gpu_ms[6],(unsigned long long)s_ray_ao.held_frames,(unsigned long long)s_ray_ao.dlss_frames,s_ray_ao.key_valid,s_ray_ao.timestamp_bits);
    if(s_ray_ao.traced<=4 || s_ray_ao.traced%120==0 || (!replacement && s_ray_ao.rejected<=8))VK_LOG("ray AO actual queries primary=%u matched=%u secondary=%u alphaRejected=%u emptyFrames=%llu\n",s_ray_ao.gpu_stats[0],s_ray_ao.gpu_stats[1],s_ray_ao.gpu_stats[2],s_ray_ao.gpu_stats[3],(unsigned long long)s_ray_ao.empty_ao);
    if(!hybrid_mode && s_ray_ao.gpu_stats[4] && (s_ray_ao.traced<=4 || s_ray_ao.traced%120==0)){
        float numeric_value;memcpy(&numeric_value,&s_ray_ao.gpu_stats[11],4);
        VK_LOG("ray AO alpha failure material=%u primitive=%u status=%d contract=%u fpCount=%u numericReason=%u value=%g\n",s_ray_ao.gpu_stats[5],s_ray_ao.gpu_stats[6],(int32_t)s_ray_ao.gpu_stats[7],s_ray_ao.gpu_stats[8],s_ray_ao.gpu_stats[9],s_ray_ao.gpu_stats[10],numeric_value);
    }
    if(!hybrid_mode && !(s_ray_ao.last_flags&1u) && s_ray_ao.gpu_stats[12] && (s_ray_ao.traced<=4 || s_ray_ao.traced%120==0 || (!replacement && s_ray_ao.rejected<=8))){
        float z,expected,nearest;memcpy(&z,&s_ray_ao.gpu_stats[17],4);memcpy(&expected,&s_ray_ao.gpu_stats[18],4);memcpy(&nearest,&s_ray_ao.gpu_stats[19],4);
        VK_LOG("ray AO depth failure pixel=%u,%u raster=%u,%u sample=0 z=%g expected=%g candidate=%u nearest=%g primitive=%u material=%u coverage=%u alphaContract=%u\n",s_ray_ao.gpu_stats[13],s_ray_ao.gpu_stats[14],s_ray_ao.gpu_stats[15],s_ray_ao.gpu_stats[16],z,expected,s_ray_ao.gpu_stats[20]!=UINT32_MAX,nearest,s_ray_ao.gpu_stats[20],s_ray_ao.gpu_stats[21],s_ray_ao.gpu_stats[22],s_ray_ao.gpu_stats[23]);
    }
    if(replacement && !trace_off)vk_ray_capture_pair(source_color,s_ray_ao.color,width*gx,height,gx,epoch->frame,epoch->generation,transport_mode,denoise,strength);
    return replacement;
abandon:
    vk_dlss_complete(0,0);
    // If submission failed, drain before invalidating recorded command storage.
    if(pvkDeviceWaitIdle(s_vk.device)==VK_SUCCESS){
        pvkResetCommandBuffer(s_vk.cmd,0);s_batch.recording=0;s_batch.pending=0;
        // The complete-device drain proves no GPU reference survives, even
        // when the helper could not report whether a failed submit was queued.
        if(recorded && !submitted){rsx_vulkan_scene_batch_discard_recording(token);recorded=0;}
    } else s_ray_ao.failed=1; // Retain batch ownership until device teardown.
fail:
    vk_dlss_complete(0,0);
    if(token && !recorded)rsx_vulkan_scene_batch_reset(token);
    if(epoch)rsx_scene_epoch_invalidate(epoch,RSX_SCENE_EPOCH_UNSUPPORTED);s_ray_ao.rejected++;
    if(!s_ray_ao.warned || s_ray_ao.rejected<=8 || s_ray_ao.rejected%120==0){VK_LOG("ray AO CPU fallback reason=%s requested=%llu traced=%llu validAO=%llu fallback=%llu frame=%llu epoch=%llu; original world color retained\n",failure,(unsigned long long)s_ray_ao.attempted,(unsigned long long)s_ray_ao.traced,(unsigned long long)s_ray_ao.completed,(unsigned long long)s_ray_ao.rejected,(unsigned long long)(epoch?epoch->frame:0),(unsigned long long)(epoch?epoch->generation:0));s_ray_ao.warned=1;}
    return 0;
}
#endif /* RSX_VULKAN_RAY_AO_INC */

