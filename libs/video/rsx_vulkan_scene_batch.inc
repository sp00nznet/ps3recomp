/* Included after rsx_vulkan_scene.inc; explicit opt-in entry points only.
 * RSX_SCENE_BATCH_CPU_ONLY excludes every Vulkan operation for focused tests. */
#include "rsx_scene_batch.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <limits.h>

static size_t rsx_sb_string_length(const char *text)
{
    if(!text)return SIZE_MAX;
    for(size_t i=0;i<=RSX_SCENE_BATCH_MAX_SOURCE_BYTES;i++)if(!text[i])return i;
    return SIZE_MAX;
}
static int rsx_sb_emit(char *text,size_t capacity,size_t *used,const char *format,...)
{
    va_list args;va_start(args,format);
    int n=vsnprintf(text+*used,capacity-*used,format,args);va_end(args);
    if(n<0 || (size_t)n>=capacity-*used)return -1;
    *used+=(size_t)n;return 0;
}
char *rsx_scene_batch_compute_source(const char *vs,const rsx_vertex_layout_plan *layout)
{
    static const char marker[]="\nRWStructuredBuffer<float4> rsx_scene_in : register(u80);\n";
    static const char *const fields[]={"pos","col0","col1","fog","t0","t1","t2","t3","t4","t5","t6","t7","t8","t9"};
    if(rsx_sb_string_length(vs)==SIZE_MAX)return NULL;
    /* Reuse diagnostic validation/entry renaming. Only replace the exact
     * generated wrapper suffix, never arbitrary text inside a guest program. */
    char *reference=rsx_scene_compute_outputs_source(vs,layout,1);
    if(!reference)return NULL;
    char *at=strstr(reference,marker);
    if(!at || strstr(at+1,marker)){free(reference);return NULL;}
    size_t used=(size_t)(at-reference),capacity=used+8192;
    char *out=(char*)malloc(capacity);if(!out){free(reference);return NULL;}
    memcpy(out,reference,used);out[used]=0;free(reference);
#define RSX_SB_EMIT(...) do {if(rsx_sb_emit(out,capacity,&used,__VA_ARGS__))goto fail;}while(0)
    RSX_SB_EMIT("\ncbuffer rsx_scene_batch_parameters : register(b1) { uint rsx_scene_count; uint rsx_scene_input_base; uint rsx_scene_output_base; uint rsx_scene_reserved; };\n"
        "RWStructuredBuffer<float4> rsx_scene_in : register(u80);\n"
        "RWStructuredBuffer<float4> rsx_scene_out : register(u81);\n"
        "RWStructuredBuffer<uint> rsx_scene_validity : register(u82);\n"
        "[numthreads(64,1,1)] void main(uint3 id:SV_DispatchThreadID) {\n"
        "if(id.x>=rsx_scene_count) return; VSInput input=(VSInput)0;\n");
    for(unsigned i=0;i<layout->count;i++)
        RSX_SB_EMIT("input.a%u=rsx_scene_in[rsx_scene_input_base+id.x*%u+%u];\n",layout->attrs[i],layout->count,i);
    RSX_SB_EMIT("VSOutput result=rsx_scene_vertex(input); uint rsx_scene_old;\n"
        "if(any(isnan(result.pos)) || any(isinf(result.pos))) { result.pos=float4(0,0,0,0); InterlockedOr(rsx_scene_validity[0],1u,rsx_scene_old); }\n"
        "bool rsx_scene_varying_bad=false;\n");
    for(unsigned i=1;i<14;i++)
        if(i<12 || strstr(vs,i==12?"TEXCOORD8":"TEXCOORD9"))
        RSX_SB_EMIT("rsx_scene_varying_bad=rsx_scene_varying_bad || any(isnan(result.%s)) || any(isinf(result.%s));\n",fields[i],fields[i]);
    RSX_SB_EMIT("if(rsx_scene_varying_bad) InterlockedOr(rsx_scene_validity[0],2u,rsx_scene_old);\n");
    for(unsigned i=0;i<14;i++) {
        if(i>=12 && !strstr(vs,i==12?"TEXCOORD8":"TEXCOORD9"))
            RSX_SB_EMIT("rsx_scene_out[rsx_scene_output_base+id.x*14+%u]=(float4)0;\n",i);
        else RSX_SB_EMIT("rsx_scene_out[rsx_scene_output_base+id.x*14+%u]=result.%s;\n",i,fields[i]);
    }
    RSX_SB_EMIT("}\n");
#undef RSX_SB_EMIT
    return out;
fail:
    free(out);return NULL;
}

typedef struct rsx_sb_draw {uint32_t cache,count,base;size_t input_offset;} rsx_sb_draw;
typedef struct rsx_sb_cpu {
    unsigned char *input,*constants;
    size_t input_used,input_capacity,constant_capacity;
    uint32_t draw_count,vertex_count;
    rsx_sb_draw draw[RSX_SCENE_BATCH_MAX_DRAWS];
} rsx_sb_cpu;
static int rsx_sb_reserve(unsigned char **data,size_t *capacity,size_t wanted,size_t limit)
{
    if(wanted>limit)return RSX_SCENE_BATCH_CAPACITY;
    if(wanted<=*capacity)return 0;
    size_t next=*capacity?*capacity:4096;
    while(next<wanted){if(next>limit/2){next=limit;break;}next*=2;}
    void *fresh=realloc(*data,next);if(!fresh)return RSX_SCENE_BATCH_MEMORY;
    *data=(unsigned char*)fresh;*capacity=next;return 0;
}
static int rsx_sb_cpu_preflight(const rsx_sb_cpu *cpu,const rsx_scene_batch_input *in)
{
    if(!in || !in->vertices || !in->constants || !in->vertex_count || in->vertex_count>RSX_SCENE_BATCH_MAX_VERTICES ||
       !in->layout.count || in->layout.count>16 || in->layout.stride!=16u*in->layout.count ||
       in->constant_bytes!=RSX_SCENE_BATCH_CONSTANT_BYTES || in->vertex_bytes!=(size_t)in->vertex_count*in->layout.stride)
        return RSX_SCENE_BATCH_INVALID;
    if(cpu->draw_count>=RSX_SCENE_BATCH_MAX_DRAWS || in->vertex_count>RSX_SCENE_BATCH_MAX_VERTICES-cpu->vertex_count ||
       in->vertex_bytes>RSX_SCENE_BATCH_MAX_INPUT_BYTES-cpu->input_used)return RSX_SCENE_BATCH_CAPACITY;
    return 0;
}
static int rsx_sb_cpu_append(rsx_sb_cpu *cpu,const rsx_scene_batch_input *in,uint32_t cache,uint32_t *base)
{
    int result=rsx_sb_cpu_preflight(cpu,in);if(result)return result;
    result=rsx_sb_reserve(&cpu->input,&cpu->input_capacity,cpu->input_used+in->vertex_bytes,RSX_SCENE_BATCH_MAX_INPUT_BYTES);
    if(result)return result;
    result=rsx_sb_reserve(&cpu->constants,&cpu->constant_capacity,(size_t)(cpu->draw_count+1)*RSX_SCENE_BATCH_CONSTANT_BYTES,(size_t)RSX_SCENE_BATCH_MAX_DRAWS*RSX_SCENE_BATCH_CONSTANT_BYTES);
    if(result)return result;
    memcpy(cpu->input+cpu->input_used,in->vertices,in->vertex_bytes);
    memcpy(cpu->constants+(size_t)cpu->draw_count*RSX_SCENE_BATCH_CONSTANT_BYTES,in->constants,RSX_SCENE_BATCH_CONSTANT_BYTES);
    cpu->draw[cpu->draw_count++]=(rsx_sb_draw){cache,in->vertex_count,cpu->vertex_count,cpu->input_used};
    if(base)*base=cpu->vertex_count;
    cpu->input_used+=in->vertex_bytes;cpu->vertex_count+=in->vertex_count;return 0;
}
static void rsx_sb_cpu_dispose(rsx_sb_cpu *cpu)
{
    free(cpu->input);free(cpu->constants);memset(cpu,0,sizeof *cpu);
}

#ifndef RSX_SCENE_BATCH_CPU_ONLY
typedef struct rsx_sb_pipeline {char *key;rsx_vertex_layout_plan layout;VkPipeline pipeline;} rsx_sb_pipeline;
enum {RSX_SB_IDLE,RSX_SB_COLLECTING,RSX_SB_REJECTED,RSX_SB_RECORDED,RSX_SB_INFLIGHT};
typedef struct rsx_sb_state {
    VkDevice device;uint64_t generation,token;int bda,state,error;
    PFN_vkCreateComputePipelines create_compute;PFN_vkCmdDispatch dispatch;
    PFN_vkCmdFillBuffer fill;PFN_vkGetFenceStatus fence_status;
    PFN_vkGetBufferDeviceAddress address;
    VkDescriptorSetLayout set_layout;VkPipelineLayout pipeline_layout;
    VkDescriptorPool pool;VkDescriptorSet set;VkFence fence;
    VkPhysicalDeviceProperties properties;
    vk_buffer input,constants,output,validity;
    VkDeviceAddress output_address,validity_address;
    size_t key_bytes;uint32_t cache_count;
    rsx_sb_pipeline cache[RSX_SCENE_BATCH_MAX_DRAWS];rsx_sb_cpu cpu;
} rsx_sb_state;
static rsx_sb_state *s_scene_batch;
static uint64_t s_scene_batch_serial;
/* Raw destruction intentionally avoids vk_destroy_buffer's implicit raster
 * flush. These allocations are module-owned and their owning fence is retired. */
static void rsx_sb_buffer_destroy(vk_buffer *buffer)
{
    if(buffer->ptr)pvkUnmapMemory(s_vk.device,buffer->mem);
    if(buffer->buf)pvkDestroyBuffer(s_vk.device,buffer->buf,NULL);
    if(buffer->mem)pvkFreeMemory(s_vk.device,buffer->mem,NULL);
    memset(buffer,0,sizeof *buffer);
}
static void rsx_sb_destroy(rsx_sb_state *b)
{
    for(uint32_t i=0;i<b->cache_count;i++) {if(b->cache[i].pipeline)pvkDestroyPipeline(s_vk.device,b->cache[i].pipeline,NULL);free(b->cache[i].key);}
    if(b->pool)pvkDestroyDescriptorPool(s_vk.device,b->pool,NULL);
    if(b->pipeline_layout)pvkDestroyPipelineLayout(s_vk.device,b->pipeline_layout,NULL);
    if(b->set_layout)pvkDestroyDescriptorSetLayout(s_vk.device,b->set_layout,NULL);
    rsx_sb_buffer_destroy(&b->input);rsx_sb_buffer_destroy(&b->constants);rsx_sb_buffer_destroy(&b->output);rsx_sb_buffer_destroy(&b->validity);
    rsx_sb_cpu_dispose(&b->cpu);free(b);
}
static int rsx_sb_init(uint64_t generation,int bda)
{
    if(!s_vk.device || !generation || (bda!=0 && bda!=1))return RSX_SCENE_BATCH_UNAVAILABLE;
    rsx_sb_state *b=(rsx_sb_state*)calloc(1,sizeof *b);if(!b)return RSX_SCENE_BATCH_MEMORY;
    b->device=s_vk.device;b->generation=generation;b->bda=bda;
    b->create_compute=(PFN_vkCreateComputePipelines)pvkGetDeviceProcAddr(s_vk.device,"vkCreateComputePipelines");
    b->dispatch=(PFN_vkCmdDispatch)pvkGetDeviceProcAddr(s_vk.device,"vkCmdDispatch");
    b->fill=(PFN_vkCmdFillBuffer)pvkGetDeviceProcAddr(s_vk.device,"vkCmdFillBuffer");
    b->fence_status=(PFN_vkGetFenceStatus)pvkGetDeviceProcAddr(s_vk.device,"vkGetFenceStatus");
    if(bda) {
        b->address=(PFN_vkGetBufferDeviceAddress)pvkGetDeviceProcAddr(s_vk.device,"vkGetBufferDeviceAddress");
        if(!b->address)b->address=(PFN_vkGetBufferDeviceAddress)pvkGetDeviceProcAddr(s_vk.device,"vkGetBufferDeviceAddressKHR");
    }
    uint32_t n=0;pvkGetPhysicalDeviceQueueFamilyProperties(s_vk.phys,&n,NULL);
    VkQueueFamilyProperties *q=n && n<=1024?(VkQueueFamilyProperties*)calloc(n,sizeof *q):NULL;
    if(!q)goto unavailable;
    pvkGetPhysicalDeviceQueueFamilyProperties(s_vk.phys,&n,q);
    int compute=s_vk.queue_family<n && (q[s_vk.queue_family].queueFlags&VK_QUEUE_COMPUTE_BIT);free(q);
    if(!compute || !b->create_compute || !b->dispatch || !b->fill || !b->fence_status || (bda && !b->address))goto unavailable;
    pvkGetPhysicalDeviceProperties(s_vk.phys,&b->properties);
    if(b->properties.limits.maxUniformBufferRange<RSX_SCENE_BATCH_CONSTANT_BYTES || b->properties.limits.maxComputeWorkGroupSize[0]<64 ||
       b->properties.limits.maxComputeWorkGroupInvocations<64 || b->properties.limits.maxComputeWorkGroupCount[0]<(RSX_SCENE_BATCH_MAX_VERTICES+63)/64)
        goto unavailable;
    VkDescriptorSetLayoutBinding bindings[5]={
        {.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding=80,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding=81,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding=82,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo sl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=5,.pBindings=bindings};
    if(pvkCreateDescriptorSetLayout(s_vk.device,&sl,NULL,&b->set_layout)!=VK_SUCCESS)goto gpu;
    VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&b->set_layout};
    if(pvkCreatePipelineLayout(s_vk.device,&pl,NULL,&b->pipeline_layout)!=VK_SUCCESS)goto gpu;
    VkDescriptorPoolSize sizes[2]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,2},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,3}};
    VkDescriptorPoolCreateInfo pc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=2,.pPoolSizes=sizes};
    if(pvkCreateDescriptorPool(s_vk.device,&pc,NULL,&b->pool)!=VK_SUCCESS)goto gpu;
    VkDescriptorSetAllocateInfo da={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=b->pool,.descriptorSetCount=1,.pSetLayouts=&b->set_layout};
    if(pvkAllocateDescriptorSets(s_vk.device,&da,&b->set)!=VK_SUCCESS)goto gpu;
    s_scene_batch=b;return 0;
unavailable:rsx_sb_destroy(b);return RSX_SCENE_BATCH_UNAVAILABLE;
gpu:rsx_sb_destroy(b);return RSX_SCENE_BATCH_GPU;
}
static int rsx_sb_layout_equal(const rsx_vertex_layout_plan *a,const rsx_vertex_layout_plan *b)
{
    return a->mask==b->mask && a->stride==b->stride && a->count==b->count && !memcmp(a->attrs,b->attrs,a->count);
}
static int rsx_sb_pipeline_get(rsx_sb_state *b,const rsx_scene_batch_input *in,uint32_t *index)
{
    size_t length=rsx_sb_string_length(in->generated_vs);if(length==SIZE_MAX)return RSX_SCENE_BATCH_INVALID;
    if(!in->layout.count || in->layout.count>16)return RSX_SCENE_BATCH_INVALID;
    for(uint32_t i=0;i<b->cache_count;i++)if(rsx_sb_layout_equal(&b->cache[i].layout,&in->layout) && !strcmp(b->cache[i].key,in->generated_vs)){*index=i;return 0;}
    if(b->cache_count>=RSX_SCENE_BATCH_MAX_DRAWS || length+1>64u*1024u*1024u-b->key_bytes)return RSX_SCENE_BATCH_CAPACITY;
    char *source=rsx_scene_batch_compute_source(in->generated_vs,&in->layout);if(!source)return RSX_SCENE_BATCH_INVALID;
    char *key=(char*)malloc(length+1);uint32_t *words=(uint32_t*)malloc(262144u*4);
    if(!key || !words){free(source);free(key);free(words);return RSX_SCENE_BATCH_MEMORY;}
    memcpy(key,in->generated_vs,length+1);
    uint32_t word_count=0;char log[2048];VkShaderModule module=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;
    int result=RSX_SCENE_BATCH_COMPILE;
    if(rsx_hlsl_to_spirv(source,RSX_SHADER_STAGE_COMPUTE,words,262144,&word_count,log,sizeof log) || !word_count || word_count>262144)goto done;
    result=RSX_SCENE_BATCH_GPU;
    VkShaderModuleCreateInfo sm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)word_count*4,.pCode=words};
    if(pvkCreateShaderModule(s_vk.device,&sm,NULL,&module)!=VK_SUCCESS)goto done;
    VkComputePipelineCreateInfo pc={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"},.layout=b->pipeline_layout};
    if(b->create_compute(s_vk.device,vk_pcache(),1,&pc,NULL,&pipeline)!=VK_SUCCESS)goto done;
    vk_pcache_created();
    *index=b->cache_count;b->cache[b->cache_count++]=(rsx_sb_pipeline){key,in->layout,pipeline};b->key_bytes+=length+1;key=NULL;pipeline=VK_NULL_HANDLE;result=0;
done:
    if(module)pvkDestroyShaderModule(s_vk.device,module,NULL);if(pipeline)pvkDestroyPipeline(s_vk.device,pipeline,NULL);
    free(source);free(key);free(words);return result;
}
static int rsx_sb_host_buffer(vk_buffer *buffer,VkDeviceSize size,VkBufferUsageFlags usage)
{
    if(buffer->size>=size)return 0;
    rsx_sb_buffer_destroy(buffer);
    if(vk_create_host_buffer(buffer,size,usage) || !buffer->coherent){rsx_sb_buffer_destroy(buffer);return RSX_SCENE_BATCH_MEMORY;}return 0;
}
static int rsx_sb_device_buffer(rsx_sb_state *b,vk_buffer *buffer,VkDeviceSize size,VkDeviceAddress *address)
{
    if(buffer->size>=size)return 0;
    rsx_sb_buffer_destroy(buffer);*address=0;
    VkBufferCreateInfo ci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=size,
        .usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|(b->bda?VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT:0),.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    if(pvkCreateBuffer(s_vk.device,&ci,NULL,&buffer->buf)!=VK_SUCCESS)return RSX_SCENE_BATCH_GPU;
    VkMemoryRequirements requirements;pvkGetBufferMemoryRequirements(s_vk.device,buffer->buf,&requirements);uint32_t type;
    if(vk_find_memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,&type))goto fail;
    VkMemoryAllocateFlagsInfo flags={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=b->bda?&flags:NULL,.allocationSize=requirements.size,.memoryTypeIndex=type};
    if(pvkAllocateMemory(s_vk.device,&allocation,NULL,&buffer->mem)!=VK_SUCCESS || pvkBindBufferMemory(s_vk.device,buffer->buf,buffer->mem,0)!=VK_SUCCESS)goto fail;
    buffer->size=size;
    if(b->bda){VkBufferDeviceAddressInfo ai={.sType=VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,.buffer=buffer->buf};*address=b->address(s_vk.device,&ai);if(!*address)goto fail;}
    return 0;
fail:rsx_sb_buffer_destroy(buffer);return RSX_SCENE_BATCH_MEMORY;
}
static rsx_sb_state *rsx_sb_current(uint64_t token)
{
    rsx_sb_state *b=s_scene_batch;return b && b->device==s_vk.device && b->token==token?b:NULL;
}
int rsx_vulkan_scene_batch_begin(uint64_t generation,int bda,uint64_t *token)
{
    if(!token)return RSX_SCENE_BATCH_INVALID;
    if(!s_scene_batch){int result=rsx_sb_init(generation,bda);if(result)return result;}
    rsx_sb_state *b=s_scene_batch;
    if(b->device!=s_vk.device || b->generation!=generation || b->bda!=bda)return RSX_SCENE_BATCH_STALE;
    if(b->state!=RSX_SB_IDLE)return RSX_SCENE_BATCH_BUSY;
    if(s_scene_batch_serial==UINT64_MAX)return RSX_SCENE_BATCH_CAPACITY;
    b->token=++s_scene_batch_serial;b->state=RSX_SB_COLLECTING;b->error=0;*token=b->token;return 0;
}
int rsx_vulkan_scene_batch_append(uint64_t token,const rsx_scene_batch_input *input,uint32_t *base)
{
    rsx_sb_state *b=rsx_sb_current(token);if(!b)return RSX_SCENE_BATCH_STALE;
    if(b->state==RSX_SB_REJECTED)return b->error;
    if(b->state!=RSX_SB_COLLECTING)return RSX_SCENE_BATCH_BUSY;
    uint32_t cache=0;int result=rsx_sb_cpu_preflight(&b->cpu,input);
    if(!result)result=rsx_sb_pipeline_get(b,input,&cache);
    if(!result)result=rsx_sb_cpu_append(&b->cpu,input,cache,base);
    if(result){b->state=RSX_SB_REJECTED;b->error=result;}return result;
}
static VkDeviceSize rsx_sb_align(VkDeviceSize value,VkDeviceSize alignment)
{
    return (value+alignment-1)/alignment*alignment;
}
int rsx_vulkan_scene_batch_record(uint64_t token,VkCommandBuffer command,VkFence fence,rsx_scene_batch_output *output)
{
    rsx_sb_state *b=rsx_sb_current(token);if(!b)return RSX_SCENE_BATCH_STALE;
    if(b->state==RSX_SB_REJECTED)return b->error;
    if(b->state!=RSX_SB_COLLECTING)return RSX_SCENE_BATCH_BUSY;
    if(!command || !fence || !output || !b->cpu.draw_count)return RSX_SCENE_BATCH_INVALID;
    VkResult fence_state=b->fence_status(s_vk.device,fence);
    if(fence_state!=VK_NOT_READY)return fence_state==VK_SUCCESS?RSX_SCENE_BATCH_BUSY:RSX_SCENE_BATCH_GPU;
    VkDeviceSize alignment=b->properties.limits.minUniformBufferOffsetAlignment;if(alignment<16)alignment=16;
    VkDeviceSize param_offset=rsx_sb_align(RSX_SCENE_BATCH_CONSTANT_BYTES,alignment),stride=param_offset+alignment;
    VkDeviceSize constant_bytes=stride*b->cpu.draw_count,out_bytes=(VkDeviceSize)b->cpu.vertex_count*sizeof(rsx_scene_vertex_output);
    if(constant_bytes>UINT32_MAX || b->cpu.input_used>b->properties.limits.maxStorageBufferRange || out_bytes>b->properties.limits.maxStorageBufferRange)return RSX_SCENE_BATCH_CAPACITY;
    int result=rsx_sb_host_buffer(&b->input,b->cpu.input_used,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if(!result)result=rsx_sb_host_buffer(&b->constants,constant_bytes,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    if(!result)result=rsx_sb_device_buffer(b,&b->output,out_bytes,&b->output_address);
    if(!result)result=rsx_sb_device_buffer(b,&b->validity,4,&b->validity_address);
    if(result){b->state=RSX_SB_REJECTED;b->error=result;return result;}
    memcpy(b->input.ptr,b->cpu.input,b->cpu.input_used);memset(b->constants.ptr,0,(size_t)constant_bytes);
    for(uint32_t i=0;i<b->cpu.draw_count;i++) {
        unsigned char *target=(unsigned char*)b->constants.ptr+(size_t)stride*i;
        memcpy(target,b->cpu.constants+(size_t)i*RSX_SCENE_BATCH_CONSTANT_BYTES,RSX_SCENE_BATCH_CONSTANT_BYTES);
        uint32_t parameters[4]={b->cpu.draw[i].count,(uint32_t)(b->cpu.draw[i].input_offset/16),b->cpu.draw[i].base*14,0};
        memcpy(target+(size_t)param_offset,parameters,sizeof parameters);
    }
    VkDescriptorBufferInfo info[5]={{b->constants.buf,0,RSX_SCENE_BATCH_CONSTANT_BYTES},{b->constants.buf,0,16},
        {b->input.buf,0,b->cpu.input_used},{b->output.buf,0,out_bytes},{b->validity.buf,0,4}};
    const uint32_t bindings[5]={0,1,80,81,82};VkWriteDescriptorSet writes[5];
    for(unsigned i=0;i<5;i++)writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=b->set,.dstBinding=bindings[i],.descriptorCount=1,
        .descriptorType=i<2?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&info[i]};
    pvkUpdateDescriptorSets(s_vk.device,5,writes,0,NULL);
    VkMemoryBarrier upload={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,.dstAccessMask=VK_ACCESS_UNIFORM_READ_BIT|VK_ACCESS_SHADER_READ_BIT};
    pvkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&upload,0,NULL,0,NULL);
    b->fill(command,b->validity.buf,0,4,0);
    VkBufferMemoryBarrier zero={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=b->validity.buf,.offset=0,.size=4};
    pvkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,1,&zero,0,NULL);
    for(uint32_t i=0;i<b->cpu.draw_count;i++) {
        uint32_t offsets[2]={(uint32_t)(stride*i),(uint32_t)(stride*i+param_offset)};
        pvkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,b->cache[b->cpu.draw[i].cache].pipeline);
        pvkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,b->pipeline_layout,0,1,&b->set,2,offsets);
        b->dispatch(command,(b->cpu.draw[i].count+63)/64,1,1);
    }
    /* Separate dispatches write disjoint output intervals. Flag writes are
     * atomic OR operations; no between-dispatch dependence is required. */
    VkBufferMemoryBarrier barriers[2];
    for(unsigned i=0;i<2;i++)barriers[i]=(VkBufferMemoryBarrier){.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=i?b->validity.buf:b->output.buf,.offset=0,.size=i?4:out_bytes};
    pvkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,2,barriers,0,NULL);
    b->fence=fence;b->state=RSX_SB_RECORDED;
    *output=(rsx_scene_batch_output){token,b->generation,b->output.buf,out_bytes,b->validity.buf,b->output_address,b->validity_address,b->cpu.vertex_count,b->cpu.draw_count};return 0;
}
int rsx_vulkan_scene_batch_mark_submitted(uint64_t token)
{
    rsx_sb_state *b=rsx_sb_current(token);if(!b)return RSX_SCENE_BATCH_STALE;
    if(b->state!=RSX_SB_RECORDED)return RSX_SCENE_BATCH_BUSY;b->state=RSX_SB_INFLIGHT;return 0;
}
int rsx_vulkan_scene_batch_discard_recording(uint64_t token)
{
    rsx_sb_state *b=rsx_sb_current(token);if(!b)return RSX_SCENE_BATCH_STALE;
    if(b->state!=RSX_SB_RECORDED)return RSX_SCENE_BATCH_BUSY;b->state=RSX_SB_REJECTED;b->error=RSX_SCENE_BATCH_INVALID;b->fence=VK_NULL_HANDLE;return 0;
}
int rsx_vulkan_scene_batch_reset(uint64_t token)
{
    rsx_sb_state *b=rsx_sb_current(token);if(!b)return RSX_SCENE_BATCH_STALE;
    if(b->state==RSX_SB_RECORDED)return RSX_SCENE_BATCH_BUSY;
    if(b->state==RSX_SB_INFLIGHT){VkResult r=b->fence_status(s_vk.device,b->fence);if(r!=VK_SUCCESS)return r==VK_NOT_READY?RSX_SCENE_BATCH_BUSY:RSX_SCENE_BATCH_GPU;}
    b->state=RSX_SB_IDLE;b->fence=VK_NULL_HANDLE;b->error=0;b->cpu.draw_count=0;b->cpu.vertex_count=0;b->cpu.input_used=0;return 0;
}
int rsx_vulkan_scene_batch_shutdown(void)
{
    rsx_sb_state *b=s_scene_batch;if(!b)return 0;
    if(b->device!=s_vk.device)return RSX_SCENE_BATCH_STALE;
    if(b->state==RSX_SB_RECORDED)return RSX_SCENE_BATCH_BUSY;
    if(b->state==RSX_SB_INFLIGHT){int result=rsx_vulkan_scene_batch_reset(b->token);if(result)return result;}
    rsx_sb_destroy(b);s_scene_batch=NULL;return 0;
}
#endif /* !RSX_SCENE_BATCH_CPU_ONLY */
