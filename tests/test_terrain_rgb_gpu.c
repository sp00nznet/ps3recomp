/* Standalone original-HLSL vs pool GLSL float32 compute parity. No main build.
 * --emit PS ALPHA_GLSL POOL_GLSL PREFIX writes original.comp.hlsl/helper.comp.
 * --run PS ORIGINAL.spv HELPER.spv DRAW037_GEOMETRY runs bounded GPU cases.
 * Original Sample calls become SampleLevel(...,0); actual 1x1 images/cube and
 * samplers are bound on both paths. No raster derivatives/gameplay claim. */
#define RSX_CAPTURE_ALLOW_NONFINITE_ATTRIBUTES 1
#define main pool_unused_position_main
#include "test_vulkan_position_replay.c"
#undef main
#include <bcrypt.h>
static unsigned pool_checks;
static int pool_extended;
#define PCHECK(x) do{++pool_checks;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 0;}}while(0)
typedef struct pool_case {rsx_scene_vertex_output vertex;float visibility[4],indirect[4];} pool_case;
_Static_assert(sizeof(pool_case)==256,"shader case stride");
static int pool_hash(const char *source)
{
    BCRYPT_ALG_HANDLE a=NULL;BCRYPT_HASH_HANDLE h=NULL;unsigned char digest[32];char hex[65];int ok=0;
    if(BCryptOpenAlgorithmProvider(&a,BCRYPT_SHA256_ALGORITHM,NULL,0)<0)return 0;
    if(BCryptCreateHash(a,&h,NULL,0,NULL,0,0)>=0 && BCryptHashData(h,(PUCHAR)source,(ULONG)strlen(source),0)>=0 && BCryptFinishHash(h,digest,32,0)>=0){
        for(unsigned i=0;i<32;++i)sprintf(hex+2*i,"%02x",digest[i]);
        ok=!strcmp(hex,"bc13117035015a01e6b17427f57e095392286b57b4e51e8b4e923d3f16bbc098")||!strcmp(hex,"e31604d3bdfe7359e53f9dea97daa5a5308ba8be5c068bf8c2741fed45812a2e");}
    if(h)BCryptDestroyHash(h);BCryptCloseAlgorithmProvider(a,0);return ok;
}
static char *pool_replace(char *owned,const char *a,const char *b)
{
    char *at=strstr(owned,a);if(!at || strstr(at+strlen(a),a)){free(owned);return NULL;}
    size_t prefix=(size_t)(at-owned);char *out=malloc(strlen(owned)+strlen(b)+1);
    if(out){memcpy(out,owned,prefix);strcpy(out+prefix,b);strcat(out,at+strlen(a));}free(owned);return out;
}
static char *pool_lod0(const char *source)
{
    char *out=calloc(1,strlen(source)+512);if(!out)return NULL;const char *at=source;size_t used=0;
    while(*at){const char *sample=strstr(at,".Sample(");if(!sample){strcpy(out+used,at);break;}
        size_t n=(size_t)(sample-at);memcpy(out+used,at,n);used+=n;strcpy(out+used,".SampleLevel(");used+=13;
        const char *args=sample+8,*end=args;int depth=1;while(*end&&depth){if(*end=='(')++depth;else if(*end==')')--depth;if(depth)++end;}
        if(depth){free(out);return NULL;}n=(size_t)(end-args);memcpy(out+used,args,n);used+=n;strcpy(out+used,",0.0)");used+=5;at=end+1;}
    return out;
}
static int pool_write(const char *path,const char *text)
{FILE *f=fopen(path,"wb");if(!f)return 0;size_t n=strlen(text);int ok=fwrite(text,1,n,f)==n;return fclose(f)==0&&ok;}
static int pool_emit(const char *ps,const char *alpha_path,const char *helper_path,const char *prefix)
{
    char *source=pool_lod0(ps),*alpha=shader_load(alpha_path),*helper=shader_load(helper_path);PCHECK(source&&alpha&&helper);
    if(!strstr(source,"TEXCOORD8"))source=pool_replace(source,"float4 tc7:TEXCOORD7;","float4 tc7:TEXCOORD7;float4 tc8:TEXCOORD8;float4 tc9:TEXCOORD9;");PCHECK(source);
    source=pool_replace(source,"PSOutput main(PSInput input)","static float4 poolVis;static float3 poolIndirect;\nPSOutput poolOriginal(PSInput input)");PCHECK(source);
    source=pool_replace(source,"{ float4 _v = (float4)(((h[2]).wwww) * ((fp_constants[1]).xyzw)); h[0].xyz = _v.xyz; }","{ float4 _v = (float4)(((h[2]).wwww) * ((fp_constants[1]).xyzw)); h[0].xyz = _v.xyz; }\nif(poolVis.w!=1.0)h[0].xyz=h[0].xyz*poolVis.w;");PCHECK(source);
    source=pool_replace(source,"{ float4 _v = (float4)(((h[2]).zzzz) * ((fp_constants[2]).xyzw) + ((h[0]).xyzw)); h[1].xyz = _v.xyz; }","if(poolVis.z!=1.0)h[2].z=h[2].z*poolVis.z;\n{ float4 _v = (float4)(((h[2]).zzzz) * ((fp_constants[2]).xyzw) + ((h[0]).xyzw)); h[1].xyz = _v.xyz; }");PCHECK(source);
    source=pool_replace(source,"{ float4 _v = (float4)(((h[5]).xyzw) * ((fp_constants[11]).xyzw)); h[6].xyz = _v.xyz; }","{ float4 _v = (float4)(((h[5]).xyzw) * ((fp_constants[11]).xyzw)); h[6].xyz = _v.xyz; }\nif(poolVis.x!=1.0)h[6].xyz=h[6].xyz*poolVis.x;");PCHECK(source);
    source=pool_replace(source,"{ float4 _v = (float4)(((h[7]).xyzw) * ((fp_constants[13]).xyzw) + ((h[6]).xyzw)); h[6].xyz = _v.xyz; }","if(poolVis.y!=1.0)h[7].xyz=h[7].xyz*poolVis.y;\n{ float4 _v = (float4)(((h[7]).xyzw) * ((fp_constants[13]).xyzw) + ((h[6]).xyzw)); h[6].xyz = _v.xyz; }");PCHECK(source);
    source=pool_replace(source,"{ float4 _v = (float4)(max(((h[1]).xyzw), ((fp_constants[16]).xxxx))); h[1].xyz = _v.xyz; }","if(any(poolIndirect!=0.0))h[1].xyz=h[1].xyz+poolIndirect;\n{ float4 _v = (float4)(max(((h[1]).xyzw), ((fp_constants[16]).xxxx))); h[1].xyz = _v.xyz; }");PCHECK(source);
    source=pool_replace(source,"Texture2D    rsx_tex[16] : register(t0);","[[vk::binding(3)]] Texture2D rsx_tex0;\n[[vk::binding(4)]] Texture2D rsx_tex1;\n[[vk::binding(5)]] Texture2D rsx_tex2;\n[[vk::binding(6)]] Texture2D rsx_tex3;\n[[vk::binding(7)]] Texture2D rsx_tex4;\n[[vk::binding(8)]] Texture2D rsx_tex5;\n[[vk::binding(9)]] Texture2D rsx_tex6;\n[[vk::binding(10)]] Texture2D rsx_tex7;\n[[vk::binding(11)]] Texture2D rsx_tex8;\n[[vk::binding(12)]] Texture2D rsx_tex9;\n[[vk::binding(13)]] Texture2D rsx_tex10;\n[[vk::binding(14)]] Texture2D rsx_tex11;\n[[vk::binding(15)]] Texture2D rsx_tex12;\n[[vk::binding(16)]] Texture2D rsx_tex13;\n[[vk::binding(17)]] Texture2D rsx_tex14;\n[[vk::binding(18)]] Texture2D rsx_tex15;");PCHECK(source);
    source=pool_replace(source,"rsx_tex[0].SampleLevel","rsx_tex0.SampleLevel");PCHECK(source);
    source=pool_replace(source,"rsx_tex[1].SampleLevel","rsx_tex1.SampleLevel");PCHECK(source);
    {char *q;while((q=strstr(source,"rsx_tex[2]"))){memmove(q+8,q+10,strlen(q+10)+1);memcpy(q,"rsx_tex2",8);}}
    source=pool_replace(source,"rsx_tex[3].SampleLevel","rsx_tex3.SampleLevel");PCHECK(source);
    source=pool_replace(source,"rsx_tex[4].SampleLevel","rsx_tex4.SampleLevel");PCHECK(source);
    source=pool_replace(source,"SamplerState rsx_samp[16] : register(s0);","[[vk::binding(19)]] SamplerState rsx_samp[16];");PCHECK(source);
    source=pool_replace(source,"cbuffer PSConstants : register(b1)","[[vk::binding(1)]] cbuffer PSConstants");PCHECK(source);
    const char *hlsl_tail="\nstruct PoolCase {PSInput vertex;float4 visibility;float4 indirect;};\n[[vk::binding(0)]] StructuredBuffer<PoolCase> cases;\n[[vk::binding(2)]] RWStructuredBuffer<float4> answers;\n[numthreads(1,1,1)] void main(uint3 id:SV_DispatchThreadID){PoolCase c=cases[0];poolVis=1.0;poolIndirect=0.0;PSOutput o=poolOriginal(c.vertex);answers[0]=o.t0;poolVis=c.visibility;poolIndirect=c.indirect.xyz;PSOutput m=poolOriginal(c.vertex);answers[1]=m.t0;}\n";
    char path[2048];char *hlsl=malloc(strlen(source)+strlen(hlsl_tail)+1);PCHECK(hlsl);strcpy(hlsl,source);strcat(hlsl,hlsl_tail);
    snprintf(path,sizeof path,"%s-original.comp.hlsl",prefix);PCHECK(pool_write(path,hlsl));
    char *glsl=calloc(1,strlen(alpha)+strlen(helper)+10000);PCHECK(glsl);strcpy(glsl,"#version 460\n");strcat(glsl,pool_extended?"#define POOL_EXTENDED true\n":"#define POOL_EXTENDED false\n");strcat(glsl,alpha);strcat(glsl,helper);
    strcat(glsl,"\nlayout(local_size_x=1) in;struct PoolCase{RsxSceneAlphaVertexOutput vertex;vec4 visibility;vec4 indirect;};layout(std430,binding=0) readonly buffer Cases{PoolCase value[];} cases;layout(std140,binding=1) uniform Constants{vec4 value[44];} fp;layout(std430,binding=2) writeonly buffer Answers{vec4 value[];} answers;\n");
    for(unsigned u=0;u<16;++u){char line[128];snprintf(line,sizeof line,"layout(binding=%u) uniform texture%s poolTex%u;\n",3+u,"2D",u);strcat(glsl,line);}
    strcat(glsl,"layout(binding=19) uniform sampler poolSamplers[16];\nbool rsx_scene_alpha_load_constant(uint i,out vec4 v){return false;}bool rsx_scene_alpha_sample(uint a,uint b,vec2 c,float d,out vec4 v){return false;}void rsx_scene_alpha_invalidate_epoch(uint a,int b){}\nbool rsx_pool_rgb_load_constant(uint i,out vec4 v){if(i>=44)return false;v=fp.value[i];return true;}\nbool rsx_pool_rgb_sample2d(uint material,uint unit,vec2 uv,float lod,out vec4 value){switch(unit){");
    for(unsigned u=0;u<7;++u){char line[180];snprintf(line,sizeof line,"case %uu:value=textureLod(sampler2D(poolTex%u,poolSamplers[%u]),uv,lod);return true;\n",u,u,u);strcat(glsl,line);}
    strcat(glsl,"default:return false;}}bool rsx_pool_rgb_sample_cube(uint material,uint unit,vec3 direction,float lod,out vec4 value){if(unit!=3u)return false;value=textureLod(sampler2D(poolTex3,poolSamplers[3]),direction.xy,lod);return true;}\nvoid main(){float lod[5];for(int i=0;i<5;++i)lod[i]=0;RsxPoolRgbOutput r;int status=rsx_terrain_rgb_evaluate(true,0,0,23,cases.value[0].vertex,lod,cases.value[0].visibility,cases.value[0].indirect.xyz,r);answers.value[0]=status==0?r.originalRGBA:vec4(-999);answers.value[1]=status==0?r.composedRGBA:vec4(-999);if(status!=0)return;answers.value[2]=vec4(r.source.reflectionDirection,r.source.transportValid?1.0:0.0);answers.value[3]=vec4(r.source.diffuseProxy,r.source.reflectionValid?1.0:0.0);for(int i=0;i<4;++i){answers.value[4+i]=vec4(r.source.direct[i],0);answers.value[8+i]=vec4(r.source.diffuseDirect[i],0);}}\n");
    snprintf(path,sizeof path,"%s-helper.comp",prefix);PCHECK(pool_write(path,glsl));free(source);free(alpha);free(helper);free(hlsl);free(glsl);return 1;
}
static VkPipeline pool_pipeline(const char *path,VkPipelineLayout layout)
{
    size_t n=0;void *bytes=load_file(path,1024*1024,&n);if(!bytes || n<20 || n%4){free(bytes);return VK_NULL_HANDLE;}
    VkShaderModule module=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;
    VkShaderModuleCreateInfo sm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=n,.pCode=bytes};
    if(pvkCreateShaderModule(s_vk.device,&sm,NULL,&module)!=VK_SUCCESS){free(bytes);return VK_NULL_HANDLE;}free(bytes);
    PFN_vkCreateComputePipelines create=(PFN_vkCreateComputePipelines)pvkGetDeviceProcAddr(s_vk.device,"vkCreateComputePipelines");
    VkComputePipelineCreateInfo pc={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"},.layout=layout};
    if(create && create(s_vk.device,VK_NULL_HANDLE,1,&pc,NULL,&pipeline)!=VK_SUCCESS)pipeline=VK_NULL_HANDLE;
    pvkDestroyShaderModule(s_vk.device,module,NULL);return pipeline;
}
static void pool_case_setup(pool_case *c,float constants[44][4],const float captured[44][4],unsigned variant)
{
    memset(c,0,sizeof *c);memcpy(constants,captured,704);for(unsigned i=0;i<4;++i)c->visibility[i]=1;
    c->vertex.col0[0]=.5f;c->vertex.col0[1]=.75f;c->vertex.col0[2]=.25f;c->vertex.col0[3]=.5f;
    c->vertex.col1[0]=.8f;c->vertex.col1[1]=.7f;c->vertex.col1[2]=.5f;c->vertex.col1[3]=.25f;
    c->vertex.t0[0]=c->vertex.t0[1]=c->vertex.t0[2]=c->vertex.t0[3]=.5f;
    c->vertex.t1[2]=c->vertex.t1[3]=.5f;c->vertex.t2[2]=1;c->vertex.t3[2]=1;c->vertex.t4[2]=1;c->vertex.t5[0]=1;c->vertex.t7[1]=1;
    if(pool_extended)c->vertex.t8[1]=1;
    if(!variant)return; /* Actual draw37 FP constants, synthetic constant varyings. */
    for(unsigned k=0;k<3;++k){constants[1][k]=.25f;constants[2][k]=.5f;constants[11][k]=.25f;constants[13][k]=.5f;constants[14][k]=.05f;constants[17][k]=.01f;constants[3][k]=.02f;constants[12][k]=.02f;}
    constants[0][0]=2;constants[0][1]=-1;constants[18][0]=1;constants[19][0]=0;
    if(variant==2){c->vertex.col0[0]=.9f;c->vertex.col1[0]=.3f;}
    if(variant==3)c->vertex.t4[2]=0;
    if(variant==4)c->vertex.t3[2]=0;
    if(variant==5){constants[0][0]=0;constants[0][1]=0;}
    if(variant==6)constants[18][0]=NAN;
    if(variant>=7 && variant<=10)c->visibility[variant-7]=.25f;
    if(variant==11){c->indirect[0]=.03f;c->indirect[1]=.06f;c->indirect[2]=.09f;}

}
static int pool_run(const char *original_spv,const char *helper_spv,const char *geometry)
{
    capture cap={0};PCHECK(capture_load(&cap,geometry));PCHECK(cap.h[29]==90 && cap.h[31]==368);float captured[44][4]={{0}};memcpy(captured,(const unsigned char*)cap.constants+cap.h[30],368);free(cap.bytes);
    VkDescriptorSetLayout dl=VK_NULL_HANDLE;VkPipelineLayout pl=VK_NULL_HANDLE;VkDescriptorPool dp=VK_NULL_HANDLE;VkPipeline pipelines[2]={0};vk_buffer buffers[3]={{0}};u32 textures[7]={0};int ok=0;
    VkDescriptorSetLayoutBinding bindings[20];memset(bindings,0,sizeof bindings);
    for(unsigned i=0;i<20;++i){bindings[i].binding=i;bindings[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;bindings[i].descriptorCount=i==19?16:1;bindings[i].descriptorType=i==1?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:i<3?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:i==19?VK_DESCRIPTOR_TYPE_SAMPLER:VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;}
    VkDescriptorSetLayoutCreateInfo lc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=20,.pBindings=bindings};
    if(pvkCreateDescriptorSetLayout(s_vk.device,&lc,NULL,&dl)!=VK_SUCCESS)goto done;
    VkPipelineLayoutCreateInfo pc={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&dl};if(pvkCreatePipelineLayout(s_vk.device,&pc,NULL,&pl)!=VK_SUCCESS)goto done;
    pipelines[0]=pool_pipeline(original_spv,pl);pipelines[1]=pool_pipeline(helper_spv,pl);if(!pipelines[0]||!pipelines[1])goto done;
    VkDeviceSize sizes[3]={sizeof(pool_case),704,192};for(unsigned i=0;i<3;++i)if(vk_create_host_buffer(&buffers[i],sizes[i],i==1?VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT:VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)||!buffers[i].coherent)goto done;
    const u8 texel[7][4]={{128,128,255,255},{102,153,204,255},{128,64,32,255},{64,128,192,255},{192,192,192,255},{255,192,128,255},{32,64,128,255}};
    for(unsigned u=0;u<7;++u){textures[u]=vk_eng_texture_create(NULL,RSX_BE_FMT_R8G8B8A8,1,1,1,1,0xAAE4u,0x85u);if(!textures[u])goto done;
        for(unsigned f=0;f<1u;++f)vk_eng_texture_upload(NULL,textures[u],f,0,1,1,texel[u],4,1);}
    rsx_be_sampler_desc desc={0};desc.wrap_s=desc.wrap_t=desc.wrap_r=3;VkSampler sampler=vk_eng_sampler(&desc);if(!sampler)goto done;
    VkDescriptorPoolSize counts[4]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,16},{VK_DESCRIPTOR_TYPE_SAMPLER,16}};
    VkDescriptorPoolCreateInfo dc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=4,.pPoolSizes=counts};if(pvkCreateDescriptorPool(s_vk.device,&dc,NULL,&dp)!=VK_SUCCESS)goto done;
    VkDescriptorSet set;VkDescriptorSetAllocateInfo da={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=dp,.descriptorSetCount=1,.pSetLayouts=&dl};if(pvkAllocateDescriptorSets(s_vk.device,&da,&set)!=VK_SUCCESS)goto done;
    VkDescriptorBufferInfo bi[3];VkDescriptorImageInfo images[16],samplers[16];VkWriteDescriptorSet writes[20];memset(writes,0,sizeof writes);
    for(unsigned i=0;i<20;++i){writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=set;writes[i].dstBinding=i;writes[i].descriptorCount=bindings[i].descriptorCount;writes[i].descriptorType=bindings[i].descriptorType;
        if(i<3){bi[i]=(VkDescriptorBufferInfo){buffers[i].buf,0,sizes[i]};writes[i].pBufferInfo=&bi[i];}
        else if(i<19){unsigned u=i-3;vk_eng_obj *t=vk_eng_get(textures[u<7?u:0],VK_ENG_TEXTURE);images[u]=(VkDescriptorImageInfo){VK_NULL_HANDLE,t->im.view,VK_IMAGE_LAYOUT_GENERAL};writes[i].pImageInfo=&images[u];}
        else {for(unsigned u=0;u<16;++u)samplers[u]=(VkDescriptorImageInfo){sampler,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_UNDEFINED};writes[i].pImageInfo=samplers;}}
    pvkUpdateDescriptorSets(s_vk.device,20,writes,0,NULL);
    PFN_vkCmdDispatch dispatch=(PFN_vkCmdDispatch)pvkGetDeviceProcAddr(s_vk.device,"vkCmdDispatch");if(!dispatch)goto done;
    unsigned mismatches=0,nonzero=0,changed=0,bitwise=0,transport=0;float max_error=0,proxy_red[2]={0};
    for(unsigned variant=0;variant<12;++variant){pool_case c;float constants[44][4],answers[2][48]={{0}};pool_case_setup(&c,constants,captured,variant);memcpy(buffers[0].ptr,&c,sizeof c);memcpy(buffers[1].ptr,constants,704);
        for(unsigned path=0;path<2;++path){memset(buffers[2].ptr,0x5a,192);if(vk_begin())goto done;
            for(unsigned u=0;u<7;++u)vk_barrier_image(vk_eng_get(textures[u],VK_ENG_TEXTURE)->im.img,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL);
            VkMemoryBarrier upload={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT};pvkCmdPipelineBarrier(s_vk.cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&upload,0,NULL,0,NULL);
            pvkCmdBindPipeline(s_vk.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines[path]);pvkCmdBindDescriptorSets(s_vk.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pl,0,1,&set,0,NULL);dispatch(s_vk.cmd,1,1,1);
            VkMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};pvkCmdPipelineBarrier(s_vk.cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,NULL,0,NULL);
            if(vk_submit_and_wait())goto done;vk_invalidate(&buffers[2]);memcpy(answers[path],buffers[2].ptr,path?192:32);}
        int same=!memcmp(answers[0],answers[1],32);bitwise+=same;int bad=0;
        for(unsigned k=0;k<8;++k){float a=answers[0][k],b=answers[1][k];float error=fabsf(a-b);if(isfinite(error)&&error>max_error)max_error=error;
            if(!(a==b || (isfinite(a)&&isfinite(b)&&error<=.00002f+.00002f*fmaxf(fabsf(a),fabsf(b)))))bad=1;}
        if(variant<7 && memcmp(answers[1],answers[1]+4,16))bad=1; /* Exact identity path. */
        if(variant==1 || variant==2){
            float expected=(variant==1?.5f:.9f)*2*.4f;
            float proxy=answers[1][12];proxy_red[variant-1]=proxy;
            if(answers[1][15]!=0 || answers[1][11]!=1 || !isfinite(proxy) || fabsf(proxy-expected)>.00003f)bad=1;
            unsigned positive=0;
            for(unsigned branch=0;branch<4;++branch)for(unsigned channel=0;channel<3;++channel){
                float mixed=answers[1][(4+branch)*4+channel],diffuse=answers[1][(8+branch)*4+channel];
                if(!isfinite(mixed)||!isfinite(diffuse)||diffuse<0||diffuse>mixed+.00003f)bad=1;
                if(diffuse>.00001f)positive++;
            }
            if(positive<4)bad=1;transport++;

        }
        if(answers[0][0]!=0 || answers[0][1]!=0 || answers[0][2]!=0)++nonzero;
        if(variant>=7 && memcmp(answers[0],answers[0]+4,12))++changed;
        mismatches+=bad;printf("pool case=%u bitwise=%d mismatch=%d original=(%g,%g,%g) modified=(%g,%g,%g)\n",variant,same,bad,answers[0][0],answers[0][1],answers[0][2],answers[0][4],answers[0][5],answers[0][6]);}
    if(!(proxy_red[1]>proxy_red[0]+.2f))mismatches++;
    printf("terrain parity:12 cases mismatches=%u bitwise=%u max_error=%g nonzero=%u changed=%u transport=%u proxyRed=%g,%g; original HLSL explicitLOD0, no raster/gameplay claim\n",mismatches,bitwise,max_error,nonzero,changed,transport,proxy_red[0],proxy_red[1]);
    ok=!mismatches&&nonzero>=3&&changed==5&&transport==2; /* Existing original identity and changed branches plus transport taps. */
done:if(s_vk.device){vk_flush_pending(0);if(dp)pvkDestroyDescriptorPool(s_vk.device,dp,NULL);for(unsigned i=0;i<2;++i)if(pipelines[i])pvkDestroyPipeline(s_vk.device,pipelines[i],NULL);if(pl)pvkDestroyPipelineLayout(s_vk.device,pl,NULL);if(dl)pvkDestroyDescriptorSetLayout(s_vk.device,dl,NULL);for(unsigned i=0;i<3;++i)vk_destroy_buffer(&buffers[i]);for(unsigned i=0;i<7;++i)if(textures[i])vk_eng_release(NULL,textures[i]);}return ok;
}
int main(int argc,char **argv)
{
    if(argc!=6 || (strcmp(argv[1],"--emit")&&strcmp(argv[1],"--run"))){fprintf(stderr,"Usage: --emit PS ALPHA_GLSL POOL_GLSL PREFIX | --run PS ORIGINAL.spv HELPER.spv DRAW037_GEOMETRY\n");return 2;}
    char *ps=shader_load(argv[2]);if(!ps||!pool_hash(ps)){free(ps);fprintf(stderr,"Exact original terrain90 PS SHA256 required\n");return 1;}
    pool_extended=strstr(ps,"input.tc8")!=NULL;
    if(!strcmp(argv[1],"--emit")){int ok=pool_emit(ps,argv[3],argv[4],argv[5]);free(ps);printf("pool source emission checks=%u result=%d\n",pool_checks,ok);return ok?0:1;}free(ps);
    _putenv_s("PS3RECOMP_VK_GUEST_PROGRAMS","1");_putenv_s("PS3RECOMP_VK_WINDOW","0");_putenv_s("PS3RECOMP_RENDER_SCALE","1");_putenv_s("PS3RECOMP_UPSCALER","off");SDL_SetMainReady();
    if(rsx_vulkan_backend_init(8,8,"terrain90 isolated shader parity"))return 77;
    int ok=pool_run(argv[3],argv[4],argv[5]);rsx_vulkan_backend_shutdown();return ok?0:1;
}
