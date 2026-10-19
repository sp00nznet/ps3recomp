/* Explicit generated VS output replay qualification. --cpu-only never creates
 * a Vulkan device. The default synthetic GPU tests need the controller's
 * serialized validation window; this is not an automatic per-frame path. */
#define SDL_MAIN_HANDLED
#include "../libs/video/rsx_vulkan_backend.c"
#include <stddef.h>

uint8_t* vm_base;
static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); return 0; } } while(0)
_Static_assert(sizeof(rsx_scene_vertex_output)==192,"complete output size");
_Static_assert(offsetof(rsx_scene_vertex_output,pos)==0,"position offset");
_Static_assert(offsetof(rsx_scene_vertex_output,col0)==16,"color0 offset");
_Static_assert(offsetof(rsx_scene_vertex_output,col1)==32,"color1 offset");
_Static_assert(offsetof(rsx_scene_vertex_output,fog)==48,"fog offset");
_Static_assert(offsetof(rsx_scene_vertex_output,t0)==64,"texcoord0 offset");
_Static_assert(offsetof(rsx_scene_vertex_output,t1)==80,"texcoord1 offset");
_Static_assert(offsetof(rsx_scene_vertex_output,t2)==96,"texcoord2 offset");
_Static_assert(offsetof(rsx_scene_vertex_output,t3)==112,"texcoord3 offset");
_Static_assert(offsetof(rsx_scene_vertex_output,t4)==128,"texcoord4 offset");
_Static_assert(offsetof(rsx_scene_vertex_output,t5)==144,"texcoord5 offset");
_Static_assert(offsetof(rsx_scene_vertex_output,t6)==160,"texcoord6 offset");
_Static_assert(offsetof(rsx_scene_vertex_output,t7)==176,"texcoord7 offset");

static const char synthetic_shader[]=
    "struct VSInput {float4 a0:ATTR0;float4 a8:ATTR8;};\n"
    "struct VSOutput {float4 pos:SV_Position;float4 col0:COLOR0;float4 col1:COLOR1;float4 fog:FOG;\n"
    "float4 t0:TEXCOORD0;float4 t1:TEXCOORD1;float4 t2:TEXCOORD2;float4 t3:TEXCOORD3;\n"
    "float4 t4:TEXCOORD4;float4 t5:TEXCOORD5;float4 t6:TEXCOORD6;float4 t7:TEXCOORD7;};\n"
    "cbuffer VPConst:register(b0){float4 c[514];};\n"
    "VSOutput main(VSInput input){VSOutput o;\n"
    "o.pos=input.a0*c[0]+c[1];\n"
    "o.col0=input.a8*c[2]+c[32];o.col1=input.a8*c[3]+c[33];\n"
    "o.fog=input.a8*c[4]+c[34];\n"
    "o.t0=input.a8*c[5]+c[35];o.t1=input.a8*c[6]+c[36];\n"
    "o.t2=input.a8*c[7]+c[37];o.t3=input.a8*c[8]+c[38];\n"
    "o.t4=input.a8*c[9]+c[39];o.t5=input.a8*c[10]+c[40];\n"
    "o.t6=input.a8*c[11]+c[41];o.t7=asfloat(asuint(c[42]));return o;}\n";

static int cpu_checks(void)
{
    static const char* const fields[]={"pos","col0","col1","fog","t0","t1","t2","t3","t4","t5","t6","t7"};
    rsx_vertex_layout_plan l;rsx_vertex_layout_plan_init(&l,(1u<<0)|(1u<<8));
    char* source=rsx_scene_compute_outputs_source(synthetic_shader,&l,3);
    CHECK(source && strlen(source)<strlen(synthetic_shader)+4096);
    CHECK(strstr(source,"VSOutput rsx_scene_vertex(VSInput input)"));
    CHECK(strstr(source,"input.a0=rsx_scene_in[id.x*2+0];"));
    CHECK(strstr(source,"input.a8=rsx_scene_in[id.x*2+1];"));
    CHECK(strstr(source,"if(id.x>=3) return;"));
    for(unsigned i=0;i<12;++i){
        char store[100];snprintf(store,sizeof store,"rsx_scene_out[id.x*12+%u]=result.%s;",i,fields[i]);
        CHECK(strstr(source,store));
    }
    free(source);
    source=rsx_scene_compute_source(synthetic_shader,&l,3);
    CHECK(source && strstr(source,"rsx_scene_out[id.x]=result.pos;") && !strstr(source,"id.x*12"));free(source);
    source=rsx_scene_compute_outputs_source(synthetic_shader,&l,262144);CHECK(source);free(source);
    CHECK(!rsx_scene_compute_outputs_source(synthetic_shader,&l,262145));
    CHECK(!rsx_scene_compute_outputs_source(synthetic_shader,&l,0));
    CHECK(!rsx_scene_compute_outputs_source(NULL,&l,3));
    CHECK(!rsx_scene_compute_outputs_source(synthetic_shader,NULL,3));
    CHECK(!rsx_scene_compute_outputs_source("Texture2D t;",&l,3));
    CHECK(!rsx_scene_compute_outputs_source("SamplerState s;",&l,3));
    CHECK(!rsx_scene_compute_outputs_source("VSOutput main(VSInput input){} rsx_scene_collision",&l,3));
    CHECK(!rsx_scene_compute_outputs_source("VSOutput main(VSInput input){} VSOutput main(VSInput input){}",&l,3));
    CHECK(!rsx_scene_compute_outputs_source("no generated entry",&l,3));
    rsx_vertex_layout_plan bad=l;bad.stride++;CHECK(!rsx_scene_compute_outputs_source(synthetic_shader,&bad,3));
    bad=l;bad.mask^=1u<<2;CHECK(!rsx_scene_compute_outputs_source(synthetic_shader,&bad,3));
    bad=l;bad.attrs[1]=bad.attrs[0];CHECK(!rsx_scene_compute_outputs_source(synthetic_shader,&bad,3));
    bad=l;bad.attrs[0]=16;CHECK(!rsx_scene_compute_outputs_source(synthetic_shader,&bad,3));
    bad=l;bad.count=0;CHECK(!rsx_scene_compute_outputs_source(synthetic_shader,&bad,3));
    bad=l;bad.count=17;CHECK(!rsx_scene_compute_outputs_source(synthetic_shader,&bad,3));
    char* huge=(char*)malloc(1024u*1024u+2);CHECK(huge);
    memset(huge,' ',1024u*1024u+1);huge[1024u*1024u+1]=0;
    CHECK(!rsx_scene_compute_outputs_source(huge,&l,3));free(huge);
    /* Both destination APIs must fail without changing caller data when the
     * backend is unavailable, including absurd capacity values. */
    float vertices[8]={0},constants[514*4]={0},positions[4];
    rsx_scene_vertex_output output,saved;
    memset(&saved,0x5a,sizeof saved);memcpy(&output,&saved,sizeof output);
    memset(positions,0x5a,sizeof positions);unsigned char saved_positions[sizeof positions];
    memcpy(saved_positions,positions,sizeof positions);
    CHECK(rsx_vulkan_backend_replay_outputs(synthetic_shader,&l,vertices,1,constants,sizeof constants,&output,1)==-1);
    CHECK(!memcmp(&output,&saved,sizeof output));
    CHECK(rsx_vulkan_backend_replay_positions(synthetic_shader,&l,vertices,1,constants,sizeof constants,positions,4)==-1);
    CHECK(!memcmp(positions,saved_positions,sizeof positions));
    CHECK(rsx_vulkan_backend_replay_outputs(synthetic_shader,&l,vertices,1,constants,sizeof constants,&output,SIZE_MAX)==-1);
    CHECK(rsx_vulkan_backend_replay_positions(synthetic_shader,&l,vertices,1,constants,sizeof constants,positions,SIZE_MAX)==-1);
    CHECK(!memcmp(&output,&saved,sizeof output) && !memcmp(positions,saved_positions,sizeof positions));
    return 1;
}

static int synthetic_gpu_checks(void)
{
    rsx_vertex_layout_plan l;rsx_vertex_layout_plan_init(&l,(1u<<0)|(1u<<8));
    const float vertices[3][8]={{-1,-2,3,1,1,2,3,4},{2,3,4,1,5,6,7,8},{3,-1,5,1,9,10,11,12}};
    float constants[514][4]={{0}};
    for(unsigned k=0;k<4;++k){
        constants[0][k]=(float)(k+1);constants[1][k]=(float)k;
        for(unsigned i=0;i<10;++i){constants[i+2][k]=(float)(i+k+1);constants[i+32][k]=(float)(i*4+k+20);}
        constants[42][k]=(float)(90+k);
    }
    rsx_scene_vertex_output output[3];float positions[12];
    CHECK(!rsx_vulkan_backend_replay_outputs(synthetic_shader,&l,vertices,3,constants,sizeof constants,output,3));
    CHECK(!rsx_vulkan_backend_replay_positions(synthetic_shader,&l,vertices,3,constants,sizeof constants,positions,12));
    for(unsigned v=0;v<3;++v){
        float fields[48];memcpy(fields,&output[v],sizeof fields);
        for(unsigned k=0;k<4;++k){
            float expected=vertices[v][k]*constants[0][k]+constants[1][k];
            CHECK(fields[k]==expected && positions[v*4+k]==expected);
            for(unsigned i=0;i<10;++i)
                CHECK(fields[(i+1)*4+k]==vertices[v][k+4]*constants[i+2][k]+constants[i+32][k]);
            CHECK(fields[44+k]==constants[42][k]);
        }
    }
    /* Unused varyings may contain NaNs/infinities/signed zero. The replay
     * boundary preserves the shader result bits, rather than rejecting them. */
    const uint32_t varying_bits[4]={0x7fc12345u,0x80000000u,0x7f800000u,0xff800000u};
    memcpy(constants[42],varying_bits,sizeof varying_bits);
    CHECK(!rsx_vulkan_backend_replay_outputs(synthetic_shader,&l,vertices,3,constants,sizeof constants,output,3));
    for(unsigned v=0;v<3;++v)CHECK(!memcmp(output[v].t7,varying_bits,sizeof varying_bits));
    CHECK(!rsx_vulkan_backend_replay_positions(synthetic_shader,&l,vertices,3,constants,sizeof constants,positions,12));
    /* Invalid evaluated positions reject the whole result transactionally. */
    rsx_scene_vertex_output saved[3];unsigned char saved_positions[sizeof positions];
    memset(saved,0x5a,sizeof saved);memcpy(output,saved,sizeof output);
    memset(positions,0x5a,sizeof positions);memcpy(saved_positions,positions,sizeof positions);
    constants[0][0]=NAN;
    CHECK(rsx_vulkan_backend_replay_outputs(synthetic_shader,&l,vertices,3,constants,sizeof constants,output,3)==-1);
    CHECK(!memcmp(output,saved,sizeof output));
    CHECK(rsx_vulkan_backend_replay_positions(synthetic_shader,&l,vertices,3,constants,sizeof constants,positions,12)==-1);
    CHECK(!memcmp(positions,saved_positions,sizeof positions));
    constants[0][0]=1;
    CHECK(rsx_vulkan_backend_replay_outputs(synthetic_shader,&l,vertices,3,constants,sizeof constants,output,2)==-1);
    CHECK(rsx_vulkan_backend_replay_outputs(synthetic_shader,&l,vertices,3,constants,16,output,3)==-1);
    CHECK(!memcmp(output,saved,sizeof output));
    return 1;
}

int main(int argc,char** argv)
{
    if(argc!=1 && !(argc==2 && !strcmp(argv[1],"--cpu-only"))){fprintf(stderr,"Usage: [--cpu-only]\n");return 2;}
    if(!cpu_checks())return 1;
    if(argc==2){printf("PASS %u scene vertex output CPU checks; no GPU initialized\n",checks);return 0;}
    SDL_SetMainReady();
#ifdef _WIN32
    _putenv_s("PS3RECOMP_VK_GUEST_PROGRAMS","1");
#else
    setenv("PS3RECOMP_VK_GUEST_PROGRAMS","1",1);
#endif
    if(rsx_vulkan_backend_init(8,8,"scene vertex output qualification"))return 1;
    int ok=synthetic_gpu_checks();rsx_vulkan_backend_shutdown();
    printf("%s %u scene vertex output checks\n",ok?"PASS":"FAIL",checks);
    return ok?0:1;
}
