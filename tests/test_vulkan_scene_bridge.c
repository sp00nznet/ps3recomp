/* Reuse the existing bounded FGM1v2 reader and independent raster depth
 * comparison fixture. Its CLI entry is renamed, not called by this test. */
/* Captured unused attributes may contain NaNs. Preserve their bits and
 * require finite evaluated positions at the compute API boundary. */
#define RSX_CAPTURE_ALLOW_NONFINITE_ATTRIBUTES 1
#define main rsx_position_replay_fixture_main
#include "test_vulkan_position_replay.c"
#undef main
#include "../libs/video/rsx_ao_projection.h"
#include "../libs/video/rsx_ao_position_dataflow.h"

static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); return 0; } } while(0)
static const char simple[]=
    "struct VSInput {float4 a0:ATTR0;}; struct VSOutput {float4 pos:SV_Position;};\n"
    "cbuffer VPConst:register(b0){float4 c[514];};\n"
    "VSOutput main(VSInput input){VSOutput o;o.pos=input.a0*c[0]+c[1];return o;}\n";

static int cpu_test(void)
{
    rsx_vertex_layout_plan l;rsx_vertex_layout_plan_init(&l,1);
    char* source=rsx_scene_compute_source(simple,&l,3);
    CHECK(source && strstr(source,"rsx_scene_vertex") && strstr(source,"numthreads"));free(source);
    CHECK(!rsx_scene_compute_source("Texture2D t;",&l,3));
    CHECK(!rsx_scene_compute_source(simple,&l,0));
    CHECK(!rsx_scene_compute_source(simple,&l,262145));
    l.attrs[0]=16;CHECK(!rsx_scene_compute_source(simple,&l,3));
    float p[16]={2,0,0,0,0,3,0,0,0,0,1.1f,1,0,0,-1.1f,0};
    float clip[4]={4,9,9.9f,10},xyz[3]={0};
    CHECK(rsx_scene_clip_to_view(clip,p,xyz));
    CHECK(fabsf(xyz[0]-2)<0.0001f && fabsf(xyz[1]-3)<0.0001f && fabsf(xyz[2]-10)<0.0001f);
    float behind[4]={-4,-9,-12.1f,-10};
    CHECK(rsx_scene_clip_to_view(behind,p,xyz) && fabsf(xyz[2]+10)<0.0001f);
    clip[3]=0;xyz[0]=123;CHECK(!rsx_scene_clip_to_view(clip,p,xyz) && xyz[0]==123);
    clip[3]=10;p[14]=0;xyz[0]=123;CHECK(!rsx_scene_clip_to_view(clip,p,xyz) && xyz[0]==123);
    p[14]=-1.1f;p[1]=1;CHECK(!rsx_scene_clip_to_view(clip,p,xyz));
    return 1;
}

static int synthetic(void)
{
    rsx_vertex_layout_plan l;rsx_vertex_layout_plan_init(&l,1);
    float v[12]={-1,-1,.25f,1,3,-1,.25f,1,-1,3,.25f,1};
    float constants[514*4]={1,1,1,1},out[12];
    CHECK(!rsx_vulkan_backend_replay_positions(simple,&l,v,3,constants,sizeof constants,out,12));
    for(unsigned i=0;i<12;++i)CHECK(out[i]==v[i]);
    /* Skin/animation-like transformation is actually evaluated, not input copied. */
    constants[0]=2;constants[5]=.25f;
    CHECK(!rsx_vulkan_backend_replay_positions(simple,&l,v,3,constants,sizeof constants,out,12));
    for(unsigned i=0;i<3;++i)CHECK(out[i*4]==v[i*4]*2 && out[i*4+1]==v[i*4+1]+.25f);
    out[0]=123;CHECK(rsx_vulkan_backend_replay_positions(simple,&l,v,3,constants,16,out,12)==-1 && out[0]==123);
    return 1;
}

static int real_capture(const char* geometry,const char* shader_path,const char* output,int compare)
{
    capture a={0},b={0};char* shader=NULL;float* pos=NULL;int ok=0;
    if(!capture_load(&a,geometry) || !(shader=shader_load(shader_path)))goto end;
    pos=(float*)malloc((size_t)a.h[4]*16);if(!pos)goto end;
    if(rsx_vulkan_backend_replay_positions(shader,&a.layout,a.vertices,a.h[4],a.constants,a.h[30],pos,(size_t)a.h[4]*4))goto end;
    b=a;b.bytes=NULL;b.vertices=pos;b.h[5]=16;rsx_vertex_layout_plan_init(&b.layout,1);
    const char* plain="float4 main(float4 p:ATTR0):SV_Position{return p;}";
    ok=1;
    if(compare) {
        char reverse[1024];snprintf(reverse,sizeof reverse,"%s-reverse",output);
        ok=measure(&a,&b,shader,plain,compare==2?NULL:output,1280,720,compare==2?3:1) &&
           measure(&b,&a,plain,shader,compare==2?NULL:reverse,1280,720,compare==2?3:1);
    }
    char file[1024];snprintf(file,sizeof file,"%s-clip.f32le",output);
    FILE* f=fopen(file,"wb");if(!f){ok=0;goto end;}
    if(fwrite(pos,16,a.h[4],f)!=a.h[4])ok=0;
    if(fclose(f))ok=0;
    printf("{\"vertices\":%u,\"indices\":%u,\"pipeline\":%u,\"raster_compared\":%s,\"success\":%s}\n",a.h[4],a.h[6],a.h[29],compare?"true":"false",ok?"true":"false");
end:
    free(a.bytes);free(shader);free(pos);return ok;
}

int main(int argc,char** argv)
{
    if(!cpu_test())return 1;
    if(argc==2 && !strcmp(argv[1],"--cpu-only")){printf("PASS %u scene CPU checks\n",checks);return 0;}
    int batch_compare=argc==3 && !strcmp(argv[1],"--batch-compare");
    int batch=argc==3 && (!strcmp(argv[1],"--batch") || batch_compare);
    if(argc!=1 && argc!=4 && !batch){fprintf(stderr,"Usage: [geometry.bin vertex.hlsl output-prefix] or --cpu-only or --batch manifest\n");return 2;}
    SDL_SetMainReady();
#ifdef _WIN32
    _putenv_s("PS3RECOMP_VK_GUEST_PROGRAMS","1");
#else
    setenv("PS3RECOMP_VK_GUEST_PROGRAMS","1",1);
#endif
    const char* guest=getenv("PS3RECOMP_VK_GUEST_PROGRAMS");
    if(!guest || strcmp(guest,"1")){fprintf(stderr,"guest program environment setup failed\n");return 1;}
    if(rsx_vulkan_backend_init(1280,720,"scene bridge qualification"))return 1;
    int ok=synthetic();
    if(ok && argc==4)ok=real_capture(argv[1],argv[2],argv[3],1);
    if(ok && batch) {
        FILE* f=fopen(argv[2],"r");char line[3072];unsigned n=0;
        if(!f)ok=0;
        while(ok && fgets(line,sizeof line,f)) {
            if(++n>512){ok=0;break;}
            line[strcspn(line,"\r\n")]=0;
            char* a=strchr(line,'|');char* b=a?strchr(a+1,'|'):NULL;
            if(!a || !b){ok=0;break;}*a++=0;*b++=0;
            ok=real_capture(line,a,b,batch_compare?2:0);
        }
        if(f){if(ferror(f))ok=0;fclose(f);}
        printf("Batch processed %u captures\n",n);
    }
    rsx_vulkan_backend_shutdown();
    printf("%s %u scene bridge checks\n",ok?"PASS":"FAIL",checks);return ok?0:1;
}
