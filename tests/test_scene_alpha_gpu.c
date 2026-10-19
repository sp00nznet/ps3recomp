/* GPU expression parity for all exact Folklore alpha contracts. Synthetic
 * samples isolate scalar shader math from sampler equivalence. Emit GLSL with
 * --emit-glsl PATH, compile with glslangValidator -V -Ilibs/video, then pass
 * the SPIR-V path. Exit 77 means Vulkan compute unavailable. */
#define main rsx_scene_alpha_cpu_fixture_main
#include "test_scene_alpha.c"
#undef main
#include <vulkan/vulkan.h>
#include <stdlib.h>

typedef struct gpu_case {
    rsx_scene_vertex_output vertex;
    float lod[4];uint32_t config[4];float tex[2][4];
} gpu_case;
typedef struct gpu_answer {float value[4];int32_t meta[4];float oracle[4];} gpu_answer;
_Static_assert(sizeof(gpu_case)==288,"std430 case stride");
_Static_assert(offsetof(gpu_case,lod)==224,"std430 LOD offset");
_Static_assert(offsetof(gpu_case,config)==240,"std430 metadata offset");
_Static_assert(offsetof(gpu_case,tex)==256,"std430 sample offset");
_Static_assert(sizeof(gpu_answer)==48,"std430 answer stride");

enum {MAX_CASES=384};
static gpu_case gpu_cases[MAX_CASES];
static float gpu_constants[MAX_CASES][44][4];
static gpu_answer expected_answers[MAX_CASES];
static char labels[MAX_CASES][72];
static uint32_t test_count;
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
enum {NEW_VARIANTS=24};
static const char* const new_lf_hashes[3]={
    "a84392db694aab4b5435524b9e6018f7b271088e60d5ebbb06bca3082f7ba797",
    "369d37e28775a313f54bd620cef1b9d65412d85d201d739e3c8f798a2b3cc7d3",
    "47535a9dfef9d40b6edf11e473ca2fa717a7057e44579112093002c322607b08"
};
static const char* const new_variants[NEW_VARIANTS]={
    "nominal","reference_equal","reference_nextbelow","reference_nextabove",
    "sample_alpha_nan","scale_nan","scale_infinity","sample_infinity",
    "negative_alpha","zero_alpha","over_one","nonfinite_uv",
    "nonfinite_lod","alpha_sample_missing","normal_sample_missing",
    "wrong_fp_count","invalid_normal_or_log","unknown_id",
    "zero_base_positive_exponent","zero_base_zero_exponent","zero_base_negative_exponent","reference_nan","vertex_alpha_nan","rgb_changed"
};
static int prepare_cases(void)
{
    test_count=0;
    for(unsigned id=0;id<15;++id){
        const rsx_scene_alpha_contract* c=rsx_scene_alpha_contract_for_sha256(cases[id].hash);
        CHECK(c && rsx_scene_alpha_contract_id(c)==(int)id);
        CHECK(rsx_scene_alpha_contract_has_threshold(c)==(id<8 || id==11 || id>=13));
        const char* lf=id<8?lf_hashes[id]:id<11?new_lf_hashes[id-8]:id==11?
            "298e00a919a7a779a09da135b48d8ba8513992d2af6f50e960edcefeb8e559aa":
            id==12?"ca7f1fe7be0b0d38d514582b023782e233f9abe08e25f4545d89cb03feacc8ed":
            id==13?"8fccec9d863b934fe89adfc52624f681cb99934885f68e83bdafcb670ff74799":
            "8d45d68ed584e0d7b66b718498b7f30344fd6cdc456a684bfcae809f57df5456";
        CHECK(rsx_scene_alpha_contract_for_sha256(lf)==c);
        for(unsigned variant=0;variant<NEW_VARIANTS;++variant){
            CHECK(test_count<MAX_CASES);
            fixture f;setup(&f,&cases[id]);
            const fixture_contract* d=f.desc;
            unsigned alpha_unit=d->kind?1u:0u;
            float nominal=d->kind==0?.5f:d->kind==1?.28125f:.890625f;
            uint32_t contract_id=id,fp_count=d->constants+1;
            switch(variant){
            case 1:f.fp[d->constants][0]=nominal;break;
            case 2:f.fp[d->constants][0]=nextafterf(nominal,-INFINITY);break;
            case 3:f.fp[d->constants][0]=nextafterf(nominal,INFINITY);break;
            case 4:f.samples[alpha_unit][3]=NAN;break;
            case 5:f.fp[d->scale][0]=NAN;break;
            case 6:f.fp[d->scale][0]=INFINITY;break;
            case 7:f.samples[alpha_unit][3]=INFINITY;break;
            case 8:f.samples[alpha_unit][3]=-.25f;break;
            case 9:f.fp[d->scale][0]=0;break;
            case 10:f.fp[d->scale][0]=d->kind?4.0f:8.0f;break;
            case 11:f.v.t0[0]=INFINITY;break;
            case 12:f.lod[alpha_unit]=NAN;break;
            case 13:f.fail_unit=(int)alpha_unit;break;
            case 14:f.fail_unit=d->kind?0:1;break;
            case 15:fp_count--;break;
            case 16:
                if(d->kind==1){f.fp[d->power_base][0]=-1;f.fp[d->power_base][1]=0;}
                else if(d->kind==2)f.v.t4[2]=0;
                else f.fp[0][0]=NAN; /* Unused constant must not poison simple alpha. */
                break;
            case 17:contract_id=15;break;
            case 18:case 19:case 20:
                if(d->kind==1){
                    f.fp[d->power_base][0]=0;f.fp[d->power_base][1]=0;
                    f.fp[d->power_exponent][2]=variant==18?2.0f:variant==19?0.0f:-2.0f;
                }
                break;
            case 21:f.fp[d->constants][0]=NAN;break;
            case 22:f.v.col0[3]=NAN;break;
            case 23:
                for(unsigned k=0;k<3;++k){f.v.col0[k]=100.0f+k;f.v.col1[k]=-200.0f-k;}
                if(!d->kind)for(unsigned k=0;k<3;++k)f.samples[0][k]=300.0f+k;
                break;
            default:break;
            }
            uint32_t i=test_count++;
            gpu_case* g=&gpu_cases[i];memset(g,0,sizeof *g);
            g->vertex=f.v;memcpy(g->lod,f.lod,2*sizeof(float));
            if(d->kind==1){
                /* Independent original-expression GPU oracle reads these
                 * operands from the case SSBO, never shader literals. */
                g->lod[2]=f.fp[d->power_base][0];
                g->lod[3]=f.fp[d->power_exponent][2];
            }
            g->config[0]=contract_id;g->config[1]=i*44;
            g->config[2]=fp_count;g->config[3]=f.fail_unit>=0?1u<<(unsigned)f.fail_unit:0;
            memcpy(g->tex,f.samples,sizeof g->tex);
            memcpy(gpu_constants[i],f.fp,sizeof f.fp);
            rsx_scene_alpha_result r={123,456,789,37,234,57};
            rsx_scene_alpha_status status=rsx_scene_alpha_evaluate(contract_id<15?c:NULL,
                &f.v,f.fp,(size_t)fp_count*16,f.lod,sample,&f,&r);
            expected_answers[i]=(gpu_answer){{r.raw_alpha,r.alpha,r.reference,r.coverage},
                {r.keep,(int32_t)status,status==RSX_SCENE_ALPHA_OK?0:1,r.thresholded},{0}};
            snprintf(labels[i],sizeof labels[i],"contract%u/%s",id,new_variants[variant]);
            if(variant==0){
                CHECK(status==RSX_SCENE_ALPHA_OK && r.thresholded==(id<8 || id==11 || id>=13));
                CHECK(r.keep==1 && r.coverage>=0 && r.coverage<=1);
            }
            if(variant==1)CHECK(status==RSX_SCENE_ALPHA_OK && r.keep==(id>=8 && id!=11 && id<13));
            if(variant==15)CHECK(status==RSX_SCENE_ALPHA_INVALID);
            if(variant==17)CHECK(status==RSX_SCENE_ALPHA_UNSUPPORTED);
            if(d->kind==1 && variant==18)
                CHECK(status==RSX_SCENE_ALPHA_OK && r.raw_alpha==.15625f &&
                      r.alpha==.15625f && r.keep && r.coverage==.15625f);
            if(d->kind==1 && (variant==19 || variant==20))
                CHECK(status==RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED);
        }
    }
    CHECK(test_count==15*NEW_VARIANTS);return 1;
}
static int compare_answers(const gpu_answer* actual)
{
    unsigned failures=0;float max_error=0;
    for(uint32_t i=0;i<test_count;++i){
        const gpu_answer* e=&expected_answers[i];const gpu_answer* a=&actual[i];int ok=1;
        for(unsigned k=0;k<4;++k)if(!close_numeric(a->value[k],e->value[k],&max_error))ok=0;
        for(unsigned k=0;k<4;++k)if(a->meta[k]!=e->meta[k])ok=0;
        if(i/NEW_VARIANTS>=2 && i/NEW_VARIANTS<7 && i%NEW_VARIANTS==18 &&
           (!isfinite(a->oracle[0]) || a->oracle[0]!=0 ||
            a->oracle[1]!=0 || a->oracle[2]!=2))ok=0;
        /* Independently translated original register expression, executed on GPU. */
        if((i/NEW_VARIANTS==13 || i/NEW_VARIANTS==14) && e->meta[1]==RSX_SCENE_ALPHA_OK){
            if(!close_numeric(a->oracle[0],e->value[0],&max_error) ||
               !close_numeric(a->oracle[1],e->value[1],&max_error) ||
               a->oracle[2]!=(float)e->meta[0] ||
               !close_numeric(a->oracle[3],e->value[3],&max_error))ok=0;
        }
        if(!ok){
            if(failures<16)fprintf(stderr,"FAIL %s: GPU alpha=%g coverage=%g keep=%d status=%d threshold=%d; CPU alpha=%g coverage=%g keep=%d status=%d threshold=%d\n",
                labels[i],(double)a->value[1],(double)a->value[3],a->meta[0],a->meta[1],a->meta[3],
                (double)e->value[1],(double)e->value[3],e->meta[0],e->meta[1],e->meta[3]);
            ++failures;
        }
    }
    printf("%s GPU/CPU alpha expressions: %u cases, %u mismatches, max finite error=%g; coverage/threshold/status checked\n",
        failures?"FAIL":"PASS",test_count,failures,(double)max_error);
    return failures==0;
}

static const char shader_source[]=
"#version 460\n"
"#extension GL_GOOGLE_include_directive : require\n"
"#include \"rsx_scene_alpha.glsl\"\n"
"layout(local_size_x=64) in;\n"
"struct AlphaCase {RsxSceneAlphaVertexOutput vertex;vec4 lod;uvec4 config;vec4 tex0;vec4 tex1;};\n"
"struct AlphaAnswer {vec4 value;ivec4 meta;vec4 oracle;};\n"
"layout(std430,set=0,binding=0) readonly buffer Cases {AlphaCase value[];} cases;\n"
"layout(std430,set=0,binding=1) readonly buffer Constants {vec4 value[];} fp;\n"
"layout(std430,set=0,binding=2) writeonly buffer Answers {AlphaAnswer value[];} answers;\n"
"layout(push_constant) uniform Parameters {uint count;uint fp_count;} parameters;\n"
"uint invalid_epoch=0;\n"
"bool rsx_scene_alpha_load_constant(uint index,out vec4 value){if(index>=parameters.fp_count)return false;value=fp.value[index];return true;}\n"
"bool rsx_scene_alpha_sample(uint material_id,uint unit,vec2 uv,float lod,out vec4 rgba){\n"
" if(material_id>=parameters.count || unit>1)return false;AlphaCase c=cases.value[material_id];\n"
" if((c.config.w&(1u<<unit))!=0u)return false;\n"
" vec2 expected=(c.config.x<2u || (c.config.x>=8u && c.config.x<11u) || c.config.x==12u || c.config.x==13u || unit==1u)?c.vertex.t0.xy:c.vertex.t0.zw;\n"
" if(any(notEqual(uv,expected)) || lod!=c.lod[unit])return false;rgba=unit==0u?c.tex0:c.tex1;return true;}\n"
"void rsx_scene_alpha_invalidate_epoch(uint material_id,int status){if(material_id==gl_GlobalInvocationID.x && status<0)invalid_epoch=1u;}\n"
"void main(){uint id=gl_GlobalInvocationID.x;if(id>=parameters.count)return;AlphaCase c=cases.value[id];\n"
" RsxSceneAlphaResult r=RsxSceneAlphaResult(123.0,456.0,789.0,37,234.0,57);\n"
" int status=rsx_scene_alpha_evaluate(c.config.x,id,c.config.y,c.config.z,c.vertex,c.lod.xy,r);\n"
" answers.value[id].value=vec4(r.raw_alpha,r.alpha,r.reference,r.coverage);\n"
" answers.value[id].meta=ivec4(r.keep,status,int(invalid_epoch),r.thresholded);\n"
" float base=c.lod.z,exponent=c.lod.w;\n"
" if(c.config.x==13u){\n"
"   precise float h1_w=(c.vertex.col0.w*c.tex0.w)*2.0;\n"
"   precise float r0_w=h1_w*fp.value[c.config.y+10u].x;\n"
"   float out_w=r0_w==r0_w?r0_w:0.0;\n"
"   bool survives=out_w>fp.value[c.config.y+12u].x;\n"
"   answers.value[id].oracle=vec4(r0_w,out_w,survives?1.0:0.0,clamp(out_w,0.0,1.0));\n"
" }else if(c.config.x==14u){\n"
"   uint b=c.config.y; precise vec3 n=normalize(c.tex0.xyz*fp.value[b].x+fp.value[b].y);\n"
"   precise float d=dot(n,normalize(c.vertex.t4.xyz));\n"
"   precise float q=clamp(-abs(d)+fp.value[b+6u].x,0.0,1.0);\n"
"   precise float f=exp2(log2(q*fp.value[b+23u].y+fp.value[b+23u].x)*fp.value[b+25u].z);\n"
"   precise float a=(c.vertex.col0.w*c.tex1.w)*2.0;\n"
"   precise float product=a*f;precise float sum=a+f;\n"
"   precise float attenuated=sum*(-fp.value[b+26u].w)+sum;\n"
"   precise float mixed=product*fp.value[b+27u].w+attenuated;\n"
"   precise float inner=min(mixed,fp.value[b+28u].x);\n"
"   precise float added=a*fp.value[b+29u].w+inner;\n"
"   precise float raw=min(max(added,fp.value[b+31u].x),fp.value[b+32u].x)*fp.value[b+33u].x;\n"
"   float sanitized=raw==raw?raw:0.0;\n"
"   answers.value[id].oracle=vec4(raw,sanitized,sanitized>fp.value[b+34u].x?1.0:0.0,clamp(sanitized,0.0,1.0));\n"
" }else answers.value[id].oracle=vec4(exp2(log2(base)*exponent),base,exponent,0.0);}\n";

static int emit_shader(const char* path)
{
    FILE* out=fopen(path,"wb");if(!out){perror(path);return 0;}
    size_t size=sizeof shader_source-1;
    int ok=fwrite(shader_source,1,size,out)==size;
    if(fclose(out)!=0)ok=0;
    if(!ok)fprintf(stderr,"Failed writing GLSL fixture: %s\n",path);
    return ok;
}

int main(int argc,char** argv)
{
    if(argc==3 && !strcmp(argv[1],"--emit-glsl"))return emit_shader(argv[2])?0:1;
    if(argc!=2){fprintf(stderr,"Usage: --emit-glsl PATH | --cpu-only | FIXTURE.spv\n");return 2;}
    if(rsx_scene_alpha_cpu_fixture_main() || !prepare_cases())return 1;
    if(!strcmp(argv[1],"--cpu-only")){
        printf("PASS CPU preparation: %u scalar GPU cases; all 15 CRLF/LF identities; no Vulkan calls\n",test_count);return 0;
    }
    size_t shader_bytes=0;uint32_t* words=read_shader(argv[1],&shader_bytes);
    if(!words){fprintf(stderr,"invalid SPIR-V input\n");return 2;}
    int result=1;alpha_buffer buffers[3]={{0}};VkShaderModule shader=VK_NULL_HANDLE;
    VkDescriptorSetLayout layout=VK_NULL_HANDLE;VkPipelineLayout pipeline_layout=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;
    VkDescriptorPool descriptors=VK_NULL_HANDLE;VkCommandPool pool=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;
    if(!initialize()){fprintf(stderr,"UNAVAILABLE: Vulkan 1.1 compute device\n");result=77;goto done;}
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
    VkComputePipelineCreateInfo pc={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.module=shader,.pName="main"},.layout=pipeline_layout};
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
    if(!vk_check(vkWaitForFences(device,1,&fence,VK_TRUE,30000000000ull),"wait")){vkDeviceWaitIdle(device);goto done;}
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
