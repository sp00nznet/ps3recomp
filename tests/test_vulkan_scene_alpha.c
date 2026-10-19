/* Standalone synthetic GPU/CPU scalar alpha comparison. No emulator/backend,
 * captured scene, textures, presentation or ray-tracing feature is required.
 * Run --cpu-only without initializing Vulkan. The controller serializes GPU
 * execution of the shader.spv mode. Exit77 means required device unavailable. */
#define main rsx_scene_alpha_cpu_fixture_main
#include "test_scene_alpha.c"
#undef main
#include <vulkan/vulkan.h>
#include <stdlib.h>

typedef struct gpu_case {
    rsx_scene_vertex_output vertex;
    float lod[4];uint32_t config[4];float tex[2][4];
} gpu_case;
typedef struct gpu_answer {float value[4];int32_t meta[4];} gpu_answer;
_Static_assert(sizeof(gpu_case)==256,"std430 case stride");
_Static_assert(offsetof(gpu_case,lod)==192,"std430 LOD offset");
_Static_assert(offsetof(gpu_case,config)==208,"std430 metadata offset");
_Static_assert(offsetof(gpu_case,tex)==224,"std430 sample offset");
_Static_assert(sizeof(gpu_answer)==32,"std430 answer stride");

enum {MAX_CASES=256,VARIANT_COUNT=22};
static gpu_case gpu_cases[MAX_CASES];
static float gpu_constants[MAX_CASES][44][4];
static gpu_answer expected_answers[MAX_CASES];
static char labels[MAX_CASES][72];
static uint32_t test_count;
static const char* const variant_names[VARIANT_COUNT]={
    "nominal","reference_equal","reference_nextbelow","reference_nextabove",
    "final_alpha_nan","reference_nan","sample_alpha_nan","zero_view_normal",
    "unknown_contract","wrong_fp_count","alpha_sample_unavailable","normal_sample_unavailable",
    "nonfinite_uv","nonfinite_lod","final_alpha_infinity","nontrivial_view_curve",
    "sampled_normal_changes","invalid_log_or_normal","nan_clamp_operand","sanitized_zero_above_negative_ref",
    "zero_alpha_equal_ref","negative_sample_alpha"
};
static const char* const lf_hashes[8]={
    "4b43ead21c9dce22ed005919077251929e3c5efb9890013745ece3afb0cfd4b7",
    "529231634350bbd2f296b7caa78ddd5346a9502701f071931cb086902855671c",
    "816722266a9edefb0d0a70c97b469c5c1d2d65658d8c06ffb296ecc2601ef579",
    "b2bb97384f7f2e7c066f4560886b0aa2c039ba982cbdb8ae2b824fd289a3b043",
    "baf7a956ae2e3b649c393fefd672697904fe26e66810eb3a8f8cf896c8934f2e",
    "a55ff59d9090562cca5ccb23eaba30b9886bfe2005bb3f61b51329f1d7b8d315",
    "c0a70bf855304927e3b74100d52669cf363e6c1904545ffeb58892d82178de4d",
    "9020e3a755063901005979eb5dc881e2afba13f4cfe01ec1220d1b214da0dce2"
};
static int prepare_cases(void)
{
    test_count=0;
    for(unsigned id=0;id<8;++id){
        const rsx_scene_alpha_contract* canonical=rsx_scene_alpha_contract_for_sha256(cases[id].hash);
        CHECK(canonical && rsx_scene_alpha_contract_id(canonical)==(int)id);
        CHECK(rsx_scene_alpha_contract_for_sha256(lf_hashes[id])==canonical);
        CHECK(rsx_scene_alpha_contract_id(rsx_scene_alpha_contract_for_sha256(lf_hashes[id]))==(int)id);
        char mismatch[65];memcpy(mismatch,lf_hashes[id],65);mismatch[0]=mismatch[0]=='a'?'b':'a';
        CHECK(!rsx_scene_alpha_contract_for_sha256(mismatch));
        for(unsigned variant=0;variant<VARIANT_COUNT;++variant){
            CHECK(test_count<MAX_CASES);fixture f;setup(&f,&cases[id]);
            const fixture_contract* d=f.desc;unsigned alpha_unit=d->kind?1u:0u;
            uint32_t contract_id=id,fp_count=d->constants+1;
            float nominal=d->kind==0?.5f:d->kind==1?.28125f:.890625f;
            switch(variant){
            case 1:f.fp[d->constants][0]=nominal;break;
            case 2:f.fp[d->constants][0]=nextafterf(nominal,-INFINITY);break;
            case 3:f.fp[d->constants][0]=nextafterf(nominal,INFINITY);break;
            case 4:f.fp[d->scale][0]=NAN;break;
            case 5:f.fp[d->constants][0]=NAN;break;
            case 6:f.samples[alpha_unit][3]=NAN;break;
            case 7:f.v.t4[0]=f.v.t4[1]=f.v.t4[2]=0;break;
            case 8:contract_id=8;break;
            case 9:fp_count--;break;
            case 10:f.fail_unit=(int)alpha_unit;break;
            case 11:f.fail_unit=d->kind?0:1;break;
            case 12:f.v.t0[0]=NAN;break;
            case 13:f.lod[alpha_unit]=NAN;break;
            case 14:f.fp[d->scale][0]=INFINITY;break;
            case 15:
                f.v.t4[0]=.3f;f.v.t4[1]=.4f;f.v.t4[2]=.5f;
                f.fp[d->constants][0]=.1f;
                if(d->kind==1){
                    f.fp[d->inner][d->inner_component]=10;f.fp[d->base][3]=0;
                    f.fp[d->lower][0]=-100;f.fp[d->upper][0]=100;
                    f.fp[d->scale][0]=1;f.fp[d->attenuation][3]=0;f.fp[d->mix][3]=1;
                }
                break;
            case 16:f.samples[0][0]=1;f.samples[0][2]=.5f;break;
            case 17:
                if(d->kind==1){f.fp[d->power_base][0]=-1;f.fp[d->power_base][1]=0;}
                else if(d->kind==2)f.v.t4[2]=0;
                else f.fp[0][0]=NAN;
                break;
            case 18:
                if(d->kind==1)f.fp[d->inner][d->inner_component]=NAN;
                else if(d->kind==2)f.fp[20][0]=NAN;
                else f.fp[0][0]=NAN;
                break;
            case 19:f.fp[d->scale][0]=NAN;f.fp[d->constants][0]=-.25f;break;
            case 20:f.fp[d->scale][0]=0;f.fp[d->constants][0]=0;break;
            case 21:f.samples[alpha_unit][3]=-.25f;break;
            default:break;
            }
            uint32_t i=test_count++;gpu_case* g=&gpu_cases[i];
            memset(g,0,sizeof *g);g->vertex=f.v;memcpy(g->lod,f.lod,2*sizeof(float));
            g->config[0]=contract_id;g->config[1]=i*44;g->config[2]=fp_count;
            g->config[3]=f.fail_unit>=0?1u<<(unsigned)f.fail_unit:0;
            memcpy(g->tex,f.samples,sizeof g->tex);memcpy(gpu_constants[i],f.fp,sizeof f.fp);
            rsx_scene_alpha_result result={123,456,789,37};
            rsx_scene_alpha_status status=rsx_scene_alpha_evaluate(contract_id<8?f.contract:NULL,
                &f.v,f.fp,(size_t)fp_count*16,f.lod,sample,&f,&result);
            expected_answers[i]=(gpu_answer){{result.raw_alpha,result.alpha,result.reference,0},
                {result.keep,(int32_t)status,status==RSX_SCENE_ALPHA_OK?0:1,0}};
            snprintf(labels[i],sizeof labels[i],"contract%u/%s",id,variant_names[variant]);
            if(variant==8)CHECK(status==RSX_SCENE_ALPHA_UNSUPPORTED);
            if(variant==9)CHECK(status==RSX_SCENE_ALPHA_INVALID);
            if(variant==1)CHECK(status==RSX_SCENE_ALPHA_OK && !result.keep);
            if(variant==2)CHECK(status==RSX_SCENE_ALPHA_OK && result.keep);
            if(variant==3)CHECK(status==RSX_SCENE_ALPHA_OK && !result.keep);
        }
    }
    CHECK(rsx_scene_alpha_contract_id(NULL)==-1);return 1;
}

typedef struct alpha_buffer {VkBuffer handle;VkDeviceMemory memory;void* map;} alpha_buffer;
static VkInstance instance;
static VkPhysicalDevice physical;
static VkDevice device;
static VkPhysicalDeviceMemoryProperties memory_properties;
static VkQueue queue;
static uint32_t family;
static int vk_check(VkResult result,const char* operation)
{
    if(result==VK_SUCCESS)return 1;
    fprintf(stderr,"%s: VkResult%d\n",operation,(int)result);return 0;
}
static int initialize(void)
{
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="scalar alpha comparison",.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ci={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    if(!vk_check(vkCreateInstance(&ci,NULL,&instance),"create instance"))return 0;
    uint32_t count=0;
    if(vkEnumeratePhysicalDevices(instance,&count,NULL)!=VK_SUCCESS || !count || count>256)return 0;
    VkPhysicalDevice devices[256];if(vkEnumeratePhysicalDevices(instance,&count,devices)!=VK_SUCCESS)return 0;
    for(uint32_t i=0;i<count && !physical;++i){
        VkPhysicalDeviceProperties properties;vkGetPhysicalDeviceProperties(devices[i],&properties);
        if(properties.apiVersion<VK_API_VERSION_1_1)continue;
        uint32_t n=0;vkGetPhysicalDeviceQueueFamilyProperties(devices[i],&n,NULL);if(!n || n>1024)continue;
        VkQueueFamilyProperties families[1024];vkGetPhysicalDeviceQueueFamilyProperties(devices[i],&n,families);
        for(uint32_t j=0;j<n;++j)if(families[j].queueCount && (families[j].queueFlags&VK_QUEUE_COMPUTE_BIT)){
            physical=devices[i];family=j;fprintf(stderr,"Selected %s; scalar fixture validation follows\n",properties.deviceName);break;
        }
    }
    if(!physical)return 0;
    float priority=1;
    VkDeviceQueueCreateInfo q={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo dc={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&q};
    if(!vk_check(vkCreateDevice(physical,&dc,NULL,&device),"create device"))return 0;
    vkGetDeviceQueue(device,family,0,&queue);vkGetPhysicalDeviceMemoryProperties(physical,&memory_properties);return 1;
}
static int make_buffer(alpha_buffer* b,VkDeviceSize bytes)
{
    VkBufferCreateInfo bc={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    if(!vk_check(vkCreateBuffer(device,&bc,NULL,&b->handle),"create storage buffer"))return 0;
    VkMemoryRequirements req;vkGetBufferMemoryRequirements(device,b->handle,&req);uint32_t type=UINT32_MAX;
    for(uint32_t i=0;i<memory_properties.memoryTypeCount;++i)
        if((req.memoryTypeBits&(1u<<i)) && (memory_properties.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){type=i;break;}
    if(type==UINT32_MAX)return 0;
    VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};
    return vk_check(vkAllocateMemory(device,&ma,NULL,&b->memory),"allocate memory") &&
        vk_check(vkBindBufferMemory(device,b->handle,b->memory,0),"bind memory") &&
        vk_check(vkMapMemory(device,b->memory,0,VK_WHOLE_SIZE,0,&b->map),"map memory");
}
static void free_buffer(alpha_buffer* b)
{
    if(b->map)vkUnmapMemory(device,b->memory);if(b->handle)vkDestroyBuffer(device,b->handle,NULL);
    if(b->memory)vkFreeMemory(device,b->memory,NULL);
}
static int close_numeric(float actual,float expected,float* max_error)
{
    if(isnan(expected))return isnan(actual);
    if(isinf(expected))return actual==expected;
    if(!isfinite(actual))return 0;
    float error=fabsf(actual-expected);if(error>*max_error)*max_error=error;
    return error<=.00002f+.00002f*fmaxf(fabsf(actual),fabsf(expected));
}
static int compare_answers(const gpu_answer* actual)
{
    unsigned failures=0;float max_error=0;
    for(uint32_t i=0;i<test_count;++i){
        const gpu_answer* e=&expected_answers[i];const gpu_answer* a=&actual[i];int ok=1;
        for(unsigned k=0;k<3;++k)if(!close_numeric(a->value[k],e->value[k],&max_error))ok=0;
        for(unsigned k=0;k<3;++k)if(a->meta[k]!=e->meta[k])ok=0;
        if(!ok){
            if(failures<16)fprintf(stderr,"FAIL %s: gpu(alpha=%g keep=%d status=%d epoch=%d) cpu(alpha=%g keep=%d status=%d epoch=%d)\n",
                labels[i],(double)a->value[1],a->meta[0],a->meta[1],a->meta[2],(double)e->value[1],e->meta[0],e->meta[1],e->meta[2]);
            ++failures;
        }
    }
    printf("%s GPU/CPU alpha fixture: %u cases, %u mismatches, max_finite_abs_error=%g; tolerance abs2e-5+rel2e-5; keep/status/epoch exact\n",
        failures?"FAIL":"PASS",test_count,failures,(double)max_error);
    return failures==0;
}
static uint32_t* read_shader(const char* path,size_t* bytes)
{
    FILE* file=fopen(path,"rb");if(!file)return NULL;
    if(fseek(file,0,SEEK_END)){fclose(file);return NULL;}long n=ftell(file);
    if(n<20 || n>4*1024*1024 || n%4 || fseek(file,0,SEEK_SET)){fclose(file);return NULL;}
    uint32_t* data=(uint32_t*)malloc((size_t)n);if(!data){fclose(file);return NULL;}
    int read=fread(data,1,(size_t)n,file)==(size_t)n;int closed=fclose(file);
    if(!read || closed || data[0]!=0x07230203u){free(data);return NULL;}
    *bytes=(size_t)n;return data;
}
int main(int argc,char** argv)
{
    if(argc!=2){fprintf(stderr,"Usage: --cpu-only | scene_alpha_fixture.spv\n");return 2;}
    if(rsx_scene_alpha_cpu_fixture_main() || !prepare_cases())return 1;
    if(!strcmp(argv[1],"--cpu-only")){
        printf("PASS CPU preparation: %u GPU comparison cases; CRLF/LF aliases map to8 stable IDs; no Vulkan calls\n",test_count);return 0;
    }
    size_t shader_bytes=0;uint32_t* words=read_shader(argv[1],&shader_bytes);
    if(!words){fprintf(stderr,"invalid SPIR-V input\n");return 2;}
    int result=1;alpha_buffer buffers[3]={{0}};VkShaderModule shader=VK_NULL_HANDLE;
    VkDescriptorSetLayout layout=VK_NULL_HANDLE;VkPipelineLayout pipeline_layout=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;
    VkDescriptorPool descriptors=VK_NULL_HANDLE;VkCommandPool pool=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;
    if(!initialize()){fprintf(stderr,"UNAVAILABLE: Vulkan1.1 compute device\n");result=77;goto done;}
    VkPhysicalDeviceProperties properties;vkGetPhysicalDeviceProperties(physical,&properties);
    size_t sizes[3]={(size_t)test_count*sizeof(gpu_case),(size_t)test_count*sizeof gpu_constants[0],(size_t)test_count*sizeof(gpu_answer)};
    for(unsigned i=0;i<3;++i)if(sizes[i]>properties.limits.maxStorageBufferRange || !make_buffer(&buffers[i],sizes[i]))goto done;
    memcpy(buffers[0].map,gpu_cases,sizes[0]);memcpy(buffers[1].map,gpu_constants,sizes[1]);memset(buffers[2].map,0xcd,sizes[2]);
    VkShaderModuleCreateInfo sm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=shader_bytes,.pCode=words};
    if(!vk_check(vkCreateShaderModule(device,&sm,NULL,&shader),"create shader module"))goto done;
    VkDescriptorSetLayoutBinding bindings[3];
    for(unsigned i=0;i<3;++i)bindings[i]=(VkDescriptorSetLayoutBinding){.binding=i,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo sl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=3,.pBindings=bindings};
    if(!vk_check(vkCreateDescriptorSetLayout(device,&sl,NULL,&layout),"descriptor layout"))goto done;
    VkPushConstantRange push={.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT,.size=8};
    VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&layout,.pushConstantRangeCount=1,.pPushConstantRanges=&push};
    if(!vk_check(vkCreatePipelineLayout(device,&pl,NULL,&pipeline_layout),"pipeline layout"))goto done;
    VkComputePipelineCreateInfo pc={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=shader,.pName="main"},.layout=pipeline_layout};
    if(!vk_check(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pc,NULL,&pipeline),"compute pipeline"))goto done;
    VkDescriptorPoolSize ds={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,3};
    VkDescriptorPoolCreateInfo dp={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&ds};
    if(!vk_check(vkCreateDescriptorPool(device,&dp,NULL,&descriptors),"descriptor pool"))goto done;
    VkDescriptorSet set;VkDescriptorSetAllocateInfo da={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=descriptors,.descriptorSetCount=1,.pSetLayouts=&layout};
    if(!vk_check(vkAllocateDescriptorSets(device,&da,&set),"descriptor allocation"))goto done;
    VkDescriptorBufferInfo info[3];VkWriteDescriptorSet writes[3];
    for(unsigned i=0;i<3;++i){
        info[i]=(VkDescriptorBufferInfo){buffers[i].handle,0,sizes[i]};
        writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=i,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&info[i]};
    }
    vkUpdateDescriptorSets(device,3,writes,0,NULL);
    VkCommandPoolCreateInfo cp={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};
    if(!vk_check(vkCreateCommandPool(device,&cp,NULL,&pool),"command pool"))goto done;
    VkCommandBuffer command;VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    if(!vk_check(vkAllocateCommandBuffers(device,&ca,&command),"command allocation"))goto done;
    VkCommandBufferBeginInfo cb={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if(!vk_check(vkBeginCommandBuffer(command,&cb),"begin commands"))goto done;
    VkMemoryBarrier upload={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&upload,0,NULL,0,NULL);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&set,0,NULL);
    uint32_t parameters[2]={test_count,test_count*44};vkCmdPushConstants(command,pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof parameters,parameters);
    vkCmdDispatch(command,(test_count+63)/64,1,1);
    VkMemoryBarrier readback={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&readback,0,NULL,0,NULL);
    if(!vk_check(vkEndCommandBuffer(command),"end commands"))goto done;
    VkFenceCreateInfo fc={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};if(!vk_check(vkCreateFence(device,&fc,NULL,&fence),"fence"))goto done;
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    if(!vk_check(vkQueueSubmit(queue,1,&submit,fence),"submit"))goto done;
    if(!vk_check(vkWaitForFences(device,1,&fence,VK_TRUE,30000000000ull),"wait")){
        /* Device teardown requires submitted work to have stopped. */
        vkDeviceWaitIdle(device);goto done;
    }
    if(compare_answers((const gpu_answer*)buffers[2].map))result=0;
done:
    if(device){
        if(fence)vkDestroyFence(device,fence,NULL);if(pool)vkDestroyCommandPool(device,pool,NULL);
        if(descriptors)vkDestroyDescriptorPool(device,descriptors,NULL);if(pipeline)vkDestroyPipeline(device,pipeline,NULL);
        if(pipeline_layout)vkDestroyPipelineLayout(device,pipeline_layout,NULL);if(layout)vkDestroyDescriptorSetLayout(device,layout,NULL);
        if(shader)vkDestroyShaderModule(device,shader,NULL);for(unsigned i=0;i<3;++i)free_buffer(&buffers[i]);
        vkDestroyDevice(device,NULL);
    }
    if(instance)vkDestroyInstance(instance,NULL);free(words);return result;
}
