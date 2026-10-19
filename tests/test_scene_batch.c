/* CPU-only tests; never initializes Vulkan or invokes a GPU function.
 * Define RSX_SCENE_BATCH_COMPILE_BACKEND for a syntax-only inclusion check of
 * the entire backend integration. Normal mode links rsx_scene_bridge.c only.
 */
#ifdef RSX_SCENE_BATCH_COMPILE_BACKEND
#define SDL_MAIN_HANDLED
#include "../libs/video/rsx_vulkan_backend.c"
#else
#define RSX_SCENE_BATCH_CPU_ONLY
#include "../libs/video/rsx_vulkan_scene_batch.inc"

static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;}}while(0)
static const char fixture[]=
    "struct VSInput{float4 a0:ATTR0;};\n"
    "struct VSOutput{float4 pos:SV_Position;float4 col0:COLOR0;float4 col1:COLOR1;float4 fog:FOG;"
    "float4 t0:TEXCOORD0;float4 t1:TEXCOORD1;float4 t2:TEXCOORD2;float4 t3:TEXCOORD3;"
    "float4 t4:TEXCOORD4;float4 t5:TEXCOORD5;float4 t6:TEXCOORD6;float4 t7:TEXCOORD7;};\n"
    "cbuffer VPConst:register(b0){float4 c[514];};\n"
    "VSOutput main(VSInput input){VSOutput o=(VSOutput)0;o.pos=input.a0*c[0]+c[1];"
    "o.col0=c[2];o.t7=asfloat(asuint(c[42]));return o;}\n";
int main(int argc,char **argv)
{
    _Static_assert(sizeof(rsx_scene_vertex_output)==192,"full output contract");
    rsx_vertex_layout_plan layout={0};layout.mask=1;layout.stride=16;layout.count=1;
    char *source=rsx_scene_batch_compute_source(fixture,&layout);CHECK(source);
    CHECK(strstr(source,"VSOutput rsx_scene_vertex(VSInput input)"));
    CHECK(strstr(source,"if(id.x>=rsx_scene_count) return;"));
    CHECK(!strstr(source,"if(id.x>=1)"));
    CHECK(strstr(source,"input.a0=rsx_scene_in[rsx_scene_input_base+id.x*1+0];"));
    CHECK(strstr(source,"rsx_scene_out[rsx_scene_output_base+id.x*12+11]=result.t7;"));
    CHECK(strstr(source,"result.pos=float4(0,0,0,0)"));
    CHECK(!strstr(source,"result.t7=") && !strstr(source,"result.col0="));
    CHECK(strstr(source,"InterlockedOr(rsx_scene_validity[0],1u"));
    CHECK(strstr(source,"InterlockedOr(rsx_scene_validity[0],2u"));
    CHECK(strstr(source,"register(b1)") && strstr(source,"register(u82)"));
    CHECK(!rsx_scene_batch_compute_source(NULL,&layout));
    CHECK(!rsx_scene_batch_compute_source(fixture,NULL));
    CHECK(!rsx_scene_batch_compute_source("Texture2D t;",&layout));
    CHECK(!rsx_scene_batch_compute_source("VSOutput main(VSInput input){} rsx_scene_collision",&layout));
    rsx_vertex_layout_plan bad=layout;bad.mask=2;CHECK(!rsx_scene_batch_compute_source(fixture,&bad));
    bad=layout;bad.count=2;bad.stride=32;bad.attrs[1]=0;CHECK(!rsx_scene_batch_compute_source(fixture,&bad));
    char *huge=(char*)malloc(RSX_SCENE_BATCH_MAX_SOURCE_BYTES+2);CHECK(huge);
    memset(huge,' ',RSX_SCENE_BATCH_MAX_SOURCE_BYTES+1);huge[RSX_SCENE_BATCH_MAX_SOURCE_BYTES+1]=0;
    CHECK(!rsx_scene_batch_compute_source(huge,&layout));free(huge);
    rsx_sb_cpu cpu={0};float vertices[12]={1,2,3,1,4,5,6,1,7,8,9,1};
    unsigned char constants[RSX_SCENE_BATCH_CONSTANT_BYTES];memset(constants,0x5a,sizeof constants);
    rsx_scene_batch_input in={fixture,layout,vertices,sizeof vertices,3,constants,sizeof constants};uint32_t base=UINT32_MAX;
    CHECK(!rsx_sb_cpu_append(&cpu,&in,7,&base) && base==0 && cpu.vertex_count==3);
    CHECK(cpu.draw[0].cache==7 && cpu.draw[0].input_offset==0);
    float saved=vertices[0];vertices[0]=99;constants[0]=0;
    CHECK(((float*)cpu.input)[0]==saved && cpu.constants[0]==0x5a);
    CHECK(!rsx_sb_cpu_append(&cpu,&in,8,&base) && base==3 && cpu.draw[1].input_offset==sizeof vertices);
    CHECK(((float*)(cpu.input+cpu.draw[1].input_offset))[0]==99 && cpu.constants[RSX_SCENE_BATCH_CONSTANT_BYTES]==0);
    in.constant_bytes--;CHECK(rsx_sb_cpu_append(&cpu,&in,0,&base)==RSX_SCENE_BATCH_INVALID && cpu.draw_count==2);in.constant_bytes++;
    in.vertex_bytes--;CHECK(rsx_sb_cpu_append(&cpu,&in,0,&base)==RSX_SCENE_BATCH_INVALID && cpu.draw_count==2);in.vertex_bytes++;
    rsx_sb_cpu_dispose(&cpu);in.vertex_count=1;in.vertex_bytes=16;
    for(unsigned i=0;i<RSX_SCENE_BATCH_MAX_DRAWS;i++)CHECK(!rsx_sb_cpu_append(&cpu,&in,0,&base) && base==i);
    CHECK(rsx_sb_cpu_append(&cpu,&in,0,&base)==RSX_SCENE_BATCH_CAPACITY && cpu.draw_count==RSX_SCENE_BATCH_MAX_DRAWS);
    rsx_sb_cpu_dispose(&cpu);
    size_t bytes=(size_t)RSX_SCENE_BATCH_MAX_VERTICES*16;void *many=calloc(1,bytes);CHECK(many);
    in.vertices=many;in.vertex_count=RSX_SCENE_BATCH_MAX_VERTICES;in.vertex_bytes=bytes;
    CHECK(!rsx_sb_cpu_append(&cpu,&in,0,&base) && cpu.vertex_count==RSX_SCENE_BATCH_MAX_VERTICES);
    in.vertices=vertices;in.vertex_count=1;in.vertex_bytes=16;
    CHECK(rsx_sb_cpu_append(&cpu,&in,0,&base)==RSX_SCENE_BATCH_CAPACITY && cpu.draw_count==1);
    rsx_sb_cpu_dispose(&cpu);free(many);
    if(argc==2){FILE *f=fopen(argv[1],"wb");CHECK(f);CHECK(fwrite(source,1,strlen(source),f)==strlen(source));CHECK(!fclose(f));}
    else CHECK(argc==1);
    free(source);printf("PASS %u scene-batch CPU checks (no Vulkan calls)\n",checks);return 0;
}
#endif
