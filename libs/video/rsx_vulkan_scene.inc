/* Explicit, synchronous replay of generated vertex programs. No raster-state
 * mutation and no automatic per-frame use. A future scene collector can cache
 * the compute pipelines/buffers after this reference path is qualified. */
static int vk_scene_replay(const char* shader,const rsx_vertex_layout_plan* layout,
    const void* vertices,u32 count,const void* constants,size_t constant_bytes,
    void* destination,size_t destination_bytes,int full)
{
    const size_t output_stride=full?sizeof(rsx_scene_vertex_output):16u;
    if(!s_vk.device || !layout || !vertices || !constants || !destination ||
       constant_bytes!=514u*16u || !count || count>262144 || destination_bytes<(size_t)count*output_stride)
        return -1;
    char* source=full?rsx_scene_compute_outputs_source(shader,layout,count):
                      rsx_scene_compute_source(shader,layout,count);
    if(!source)return -1;
    int result=-1;
    u32 family_count=0;
    pvkGetPhysicalDeviceQueueFamilyProperties(s_vk.phys,&family_count,NULL);
    VkQueueFamilyProperties* families=family_count && family_count<=1024 ?
        (VkQueueFamilyProperties*)calloc(family_count,sizeof *families):NULL;
    if(!families) { free(source);return -1; }
    pvkGetPhysicalDeviceQueueFamilyProperties(s_vk.phys,&family_count,families);
    int compute=s_vk.queue_family<family_count && (families[s_vk.queue_family].queueFlags&VK_QUEUE_COMPUTE_BIT);
    free(families);
    PFN_vkCreateComputePipelines create_compute=(PFN_vkCreateComputePipelines)pvkGetDeviceProcAddr(s_vk.device,"vkCreateComputePipelines");
    PFN_vkCmdDispatch dispatch=(PFN_vkCmdDispatch)pvkGetDeviceProcAddr(s_vk.device,"vkCmdDispatch");
    if(!compute || !create_compute || !dispatch) { free(source);return -1; }
    vk_flush_pending(0);
    vk_buffer in={0},out={0},cb={0};
    VkShaderModule module=VK_NULL_HANDLE;VkPipeline pipe=VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout=VK_NULL_HANDLE;VkPipelineLayout pipeline_layout=VK_NULL_HANDLE;
    VkDescriptorPool pool=VK_NULL_HANDLE;
    u32* words=(u32*)malloc(262144u*sizeof(u32));u32 word_count=0;
    char log[2048];
    if(!words)goto end;
    if(rsx_hlsl_to_spirv(source,RSX_SHADER_STAGE_COMPUTE,words,262144,&word_count,log,sizeof log)) {
        VK_LOG("scene replay compile rejected: %s\n",log);goto end;
    }
    VkShaderModuleCreateInfo sm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=(size_t)word_count*4,.pCode=words};
    if(pvkCreateShaderModule(s_vk.device,&sm,NULL,&module)!=VK_SUCCESS)goto end;
    VkDescriptorSetLayoutBinding bindings[3]={
        {.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding=80,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding=81,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo sl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=3,.pBindings=bindings};
    if(pvkCreateDescriptorSetLayout(s_vk.device,&sl,NULL,&set_layout)!=VK_SUCCESS)goto end;
    VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount=1,.pSetLayouts=&set_layout};
    if(pvkCreatePipelineLayout(s_vk.device,&pl,NULL,&pipeline_layout)!=VK_SUCCESS)goto end;
    VkComputePipelineCreateInfo cp={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"},.layout=pipeline_layout};
    if(create_compute(s_vk.device,VK_NULL_HANDLE,1,&cp,NULL,&pipe)!=VK_SUCCESS)goto end;
    VkDescriptorPoolSize sizes[2]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2}};
    VkDescriptorPoolCreateInfo dp={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets=1,.poolSizeCount=2,.pPoolSizes=sizes};
    if(pvkCreateDescriptorPool(s_vk.device,&dp,NULL,&pool)!=VK_SUCCESS)goto end;
    VkDescriptorSet set;
    VkDescriptorSetAllocateInfo da={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&set_layout};
    if(pvkAllocateDescriptorSets(s_vk.device,&da,&set)!=VK_SUCCESS)goto end;
    VkDeviceSize in_bytes=(VkDeviceSize)count*layout->stride,out_bytes=(VkDeviceSize)count*output_stride;
    if(vk_create_host_buffer(&in,in_bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
       vk_create_host_buffer(&out,out_bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
       vk_create_host_buffer(&cb,constant_bytes,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
       !in.coherent || !out.coherent || !cb.coherent)goto end;
    memcpy(in.ptr,vertices,(size_t)in_bytes);memcpy(cb.ptr,constants,constant_bytes);
    for(size_t i=0;i<(size_t)out_bytes/sizeof(float);++i)((float*)out.ptr)[i]=NAN;
    VkDescriptorBufferInfo info[3]={{cb.buf,0,constant_bytes},{in.buf,0,in_bytes},{out.buf,0,out_bytes}};
    VkWriteDescriptorSet writes[3];
    for(unsigned i=0;i<3;++i)writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet=set,.dstBinding=bindings[i].binding,.descriptorCount=1,
        .descriptorType=bindings[i].descriptorType,.pBufferInfo=&info[i]};
    pvkUpdateDescriptorSets(s_vk.device,3,writes,0,NULL);
    if(vk_begin())goto end;
    pvkCmdBindPipeline(s_vk.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipe);
    pvkCmdBindDescriptorSets(s_vk.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&set,0,NULL);
    dispatch(s_vk.cmd,(count+63u)/64u,1,1);
    VkBufferMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .buffer=out.buf,.offset=0,.size=VK_WHOLE_SIZE};
    pvkCmdPipelineBarrier(s_vk.cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&barrier,0,NULL);
    if(vk_submit_and_wait())goto end;
    vk_invalidate(&out);
    /* Only position is a mandatory finite result. Unused varying channels in
     * captured shaders can legitimately be NaN and must retain their bits. */
    for(size_t i=0;i<count;++i)
        for(unsigned k=0;k<4;++k)
            if(!isfinite(((float*)out.ptr)[i*(output_stride/sizeof(float))+k]))goto end;
    memcpy(destination,out.ptr,(size_t)out_bytes);result=0;
end:
    free(words);free(source);
    vk_destroy_buffer(&in);vk_destroy_buffer(&out);vk_destroy_buffer(&cb);
    if(pipe)pvkDestroyPipeline(s_vk.device,pipe,NULL);
    if(pool)pvkDestroyDescriptorPool(s_vk.device,pool,NULL);
    if(pipeline_layout)pvkDestroyPipelineLayout(s_vk.device,pipeline_layout,NULL);
    if(set_layout)pvkDestroyDescriptorSetLayout(s_vk.device,set_layout,NULL);
    if(module)pvkDestroyShaderModule(s_vk.device,module,NULL);
    return result;
}

int rsx_vulkan_backend_replay_positions(const char* shader,const rsx_vertex_layout_plan* layout,
    const void* vertices,u32 count,const void* constants,size_t constant_bytes,
    float* positions,size_t position_floats)
{
    if(position_floats>SIZE_MAX/sizeof(float))return -1;
    return vk_scene_replay(shader,layout,vertices,count,constants,constant_bytes,
                           positions,position_floats*sizeof(float),0);
}

/* Explicit diagnostic capture of generated vertex outputs. Resources and
 * dispatch are shared with position-only replay; no per-frame caller exists. */
int rsx_vulkan_backend_replay_outputs(const char* shader,const rsx_vertex_layout_plan* layout,
    const void* vertices,u32 count,const void* constants,size_t constant_bytes,
    rsx_scene_vertex_output* outputs,size_t output_count)
{
    if(output_count>SIZE_MAX/sizeof(rsx_scene_vertex_output))return -1;
    return vk_scene_replay(shader,layout,vertices,count,constants,constant_bytes,
                           outputs,output_count*sizeof(rsx_scene_vertex_output),1);
}
