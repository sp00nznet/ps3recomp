/* Controller-serialized GPU fixture for the ACTUAL live ray AO consumer.
 * No saved geometry, CPU position replay, fabricated depth, or offline tracer.
 * Default runs single sample and expanded MSAA2; --single/--msaa2 isolate one.
 * Build/link exactly as test_scene_batch_gpu, including generated scene header.
 * Synthetic opaque geometry only: this does not qualify gameplay or alpha LOD.
 */
#define SDL_MAIN_HANDLED
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "../libs/video/rsx_vulkan_backend.c"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

uint8_t *vm_base;
static unsigned checks;
#define REQUIRE(x) do { ++checks; if(!(x)) { \
    fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 0; } } while(0)
enum { LIVE_W=640, LIVE_H=360 };
typedef struct live_fixture {
    u32 color,depth,pipe,kill_pipe,samples,width;
    float constants[2056],projection[16];
    unsigned char *before,*after,*output;
    size_t bytes;
} live_fixture;

static const char live_vs[]=
    "struct VSInput {float4 a0:ATTR0;};\n"
    "struct VSOutput {float4 pos:SV_Position;float4 col0:COLOR0;float4 col1:COLOR1;float4 fog:FOG;"
    "float4 t0:TEXCOORD0;float4 t1:TEXCOORD1;float4 t2:TEXCOORD2;float4 t3:TEXCOORD3;"
    "float4 t4:TEXCOORD4;float4 t5:TEXCOORD5;float4 t6:TEXCOORD6;float4 t7:TEXCOORD7;};\n"
    "cbuffer VPConst:register(b0){float4 c[514];};\n"
    "VSOutput main(VSInput input){VSOutput o=(VSOutput)0;float z=input.a0.z+c[1].x;"
    "o.pos=float4((input.a0.x+c[1].y)*c[0].x,input.a0.y*c[0].y,z*c[0].z+c[0].w,z);"
    "o.col0=float4(1,1,1,1);return o;}\n";
static const char live_ps[]="float4 main():SV_Target0{return float4(.2,.4,.6,1);}";
static const char live_kill_ps[]="float4 main():SV_Target0{discard;return float4(0,1,0,1);}";
static const float live_plane[6][4]={
    {-12,-12,3,1},{12,-12,3,1},{-12,12,3,1},
    {12,-12,3,1},{12,12,3,1},{-12,12,3,1}};
static const float live_wall[6][4]={
    {.5f,-12,1,1},{.5f,12,1,1},{.5f,-12,5,1},
    {.5f,12,1,1},{.5f,12,5,1},{.5f,-12,5,1}};

static void live_env(const char *name,const char *value)
{
#ifdef _WIN32
    _putenv_s(name,value);
#else
    setenv(name,value,1);
#endif
}
static int live_read(live_fixture *f,u32 color,unsigned char *pixels)
{
    return vk_eng_readback_rows_raw(color,0,0,f->width,LIVE_H,pixels,f->width*4)==0;
}
static void live_release(live_fixture *f)
{
    vk_eng_bind_targets(NULL,NULL,0,0);vk_eng_bind_pipeline(NULL,0);
    if(f->pipe)vk_eng_pipeline_release(NULL,f->pipe);
    if(f->kill_pipe)vk_eng_pipeline_release(NULL,f->kill_pipe);
    if(f->color)vk_eng_release(NULL,f->color);
    if(f->depth)vk_eng_release(NULL,f->depth);
    free(f->before);free(f->after);free(f->output);memset(f,0,sizeof *f);
}
static int live_create(live_fixture *f,u32 samples)
{
    rsx_vertex_layout_plan layout;rsx_vertex_layout_plan_init(&layout,1);
    rsx_be_render_state state={0};
    state.color_mask=0x01010101;state.antialias_control=0xffff0001;
    state.depth_test=1;state.depth_write=1;state.depth_func=0x203;
    f->samples=samples;f->width=LIVE_W*(samples==2?2u:1u);
    f->bytes=(size_t)f->width*LIVE_H*4;
    if(samples==2) {
        f->color=vk_eng_color_multisample(NULL,RSX_BE_FMT_R8G8B8A8,LIVE_W,LIVE_H,2);
        f->depth=vk_eng_depth_multisample(NULL,LIVE_W,LIVE_H,2);
    } else {
        f->color=vk_eng_color_target_create(NULL,RSX_BE_FMT_R8G8B8A8,LIVE_W,LIVE_H,NULL,0);
        f->depth=vk_eng_depth_target_create(NULL,LIVE_W,LIVE_H);
    }
    REQUIRE(f->color && f->depth);
    f->pipe=vk_eng_pipeline_create(NULL,live_vs,live_ps,&state,&layout,16,RSX_BE_FMT_R8G8B8A8,1);
    f->kill_pipe=vk_eng_pipeline_create(NULL,live_vs,live_kill_ps,&state,&layout,16,RSX_BE_FMT_R8G8B8A8,1);
    REQUIRE(f->pipe && f->kill_pipe);
    f->before=malloc(f->bytes);f->after=malloc(f->bytes);f->output=malloc(f->bytes);
    REQUIRE(f->before && f->after && f->output);
    f->constants[0]=f->constants[1]=1;
    f->constants[2]=100.0f/99.9f;f->constants[3]=-10.0f/99.9f;
    f->projection[0]=f->projection[5]=1;f->projection[10]=f->constants[2];
    f->projection[11]=1;f->projection[14]=f->constants[3];return 1;
}
/* Depth is produced by the normal graphics pipeline. Capture is invoked by
 * the real clear/draw callbacks, never by manually appending fake snapshots. */
static int live_render(live_fixture *f,int wall,int kill,int offscreen)
{
    const float clear[4]={1,0,1,1};
    u32 textures[16]={0};rsx_be_sampler_desc samplers[16]={0};
    f->constants[4]=0;f->constants[5]=offscreen?100.0f:0;
    vk_eng_clear_color(NULL,f->color,clear);
    vk_eng_clear_depth_stencil(NULL,f->depth,RSX_BE_CLEAR_DEPTH,1,0);
    vk_eng_bind_targets(NULL,&f->color,1,f->depth);
    vk_eng_bind_pipeline(NULL,kill?f->kill_pipe:f->pipe);
    vk_eng_bind_vs_constants(NULL,f->constants,sizeof f->constants);
    /* These opaque shaders have no constants; exercise the empty descriptor
     * case through the real binding/capture path. */
    vk_eng_bind_ps_constants(NULL,NULL,0);
    vk_eng_bind_textures(NULL,textures,samplers,0);
    vk_eng_set_viewport(NULL,0,0,LIVE_W,LIVE_H,0,1);
    vk_eng_set_scissor(NULL,0,0,LIVE_W,LIVE_H);
    vk_eng_draw(NULL,RSX_TOPOLOGY_TRIANGLES,live_plane,6,16,NULL,0);
    if(wall)vk_eng_draw(NULL,RSX_TOPOLOGY_TRIANGLES,live_wall,6,16,NULL,0);
    vk_flush_pending(0);
    REQUIRE(live_read(f,f->color,f->before));return 1;
}
static u32 live_consume(live_fixture *f)
{
    /* RSX identity remap and A8R8G8B8 interpretation, as the world callback. */
    return vk_eng_ray_world(NULL,f->color,f->depth,LIVE_W,LIVE_H,
        f->projection,0xAAE4u,0x85u,1.5f,.8f);
}
static int live_original_preserved(live_fixture *f)
{
    REQUIRE(live_read(f,f->color,f->after));
    REQUIRE(memcmp(f->before,f->after,f->bytes)==0);return 1;
}
static int live_valid(live_fixture *f,int occluded)
{
    u64 traced=s_ray_ao.traced,valid=s_ray_ao.completed;
    REQUIRE(live_consume(f)!=0);
    REQUIRE(s_ray_ao.traced==traced+1 && s_ray_ao.completed==valid+1);
    REQUIRE((s_ray_ao.last_flags&~2u)==0);
    REQUIRE(s_ray_ao.gpu_stats[0]>0 && s_ray_ao.gpu_stats[1]>0);
    REQUIRE(s_ray_ao.gpu_stats[2]==s_ray_ao.gpu_stats[1]*4u);
    REQUIRE(live_read(f,s_ray_ao.color,f->output));
    size_t darkened=0;
    for(size_t i=0;i<f->bytes;i+=4) {
        REQUIRE(f->output[i+3]==f->before[i+3]);
        if(f->output[i]<f->before[i] || f->output[i+1]<f->before[i+1] || f->output[i+2]<f->before[i+2])++darkened;
    }
    if(occluded)REQUIRE(darkened>0);
    else REQUIRE(memcmp(f->before,f->output,f->bytes)==0);
    REQUIRE(live_original_preserved(f));
    printf("live-ray samples=%u control=%s primary=%u matched=%u secondary=%u darkened=%zu\n",
        f->samples,occluded?"corner":"plane",s_ray_ao.gpu_stats[0],s_ray_ao.gpu_stats[1],s_ray_ao.gpu_stats[2],darkened);
    return 1;
}
static int live_suite(live_fixture *f)
{
    REQUIRE(live_render(f,0,0,0));
    rsx_scene_epoch *e=vk_rt_epoch(f->depth);
    REQUIRE(e && e->armed && e->reason==RSX_SCENE_EPOCH_OK && e->draw_count==1);
    /* Snapshot existence plus independent real depth readback. The existing
     * helper reads original single-sample depth or MSAA-expanded depth using
     * images with TRANSFER_SRC usage, not the sampled snapshot image. */
    u32 snapshot=vk_eng_depth_snapshot(NULL,f->depth,f->width,LIVE_H);
    REQUIRE(snapshot!=0);
    float raster_depth=0;
    REQUIRE(vk_eng_depth_readback(NULL,f->depth,LIVE_W/2,LIVE_H/2,1,1,&raster_depth,4)==0);
    REQUIRE(isfinite(raster_depth) && fabsf(raster_depth-(f->constants[2]+f->constants[3]/3))<.00001f);
    REQUIRE(f->before[((size_t)(LIVE_H/2)*f->width+f->width/2)*4]!=255);
    REQUIRE(live_valid(f,0));
    u64 traced=s_ray_ao.traced,valid=s_ray_ao.completed;
    REQUIRE(live_consume(f)==0); /* sealed same-epoch reuse */
    REQUIRE(s_ray_ao.traced==traced && s_ray_ao.completed==valid);

    REQUIRE(live_render(f,1,0,0));REQUIRE(live_valid(f,1));
    REQUIRE(live_render(f,1,0,0));e=vk_rt_epoch(f->depth);
    REQUIRE(e && e->draw_count==2 && e->draws[1].draw.vs_bytes==8224);
    /* Intentional corruption of the owned captured constants ONLY. The
     * independently rasterized depth/color remain untouched. Move replayed
     * wall x=.5 to x=.75, creating a primary-depth mismatch next to real AO. */
    float shifted=.25f;memcpy(e->bytes+e->draws[1].vs_offset+20,&shifted,4);
    traced=s_ray_ao.traced;valid=s_ray_ao.completed;
    REQUIRE(live_consume(f)==0);
    REQUIRE(s_ray_ao.traced==traced+1 && s_ray_ao.completed==valid);
    REQUIRE(s_ray_ao.last_flags&8u);
    REQUIRE(live_read(f,s_ray_ao.color,f->output));
    REQUIRE(memcmp(f->before,f->output,f->bytes)==0); /* EVERY pixel fallback */
    REQUIRE(live_original_preserved(f));

    REQUIRE(live_render(f,0,0,1));traced=s_ray_ao.traced;valid=s_ray_ao.completed;
    u64 empty=s_ray_ao.empty_ao;
    REQUIRE(live_consume(f)==0);
    REQUIRE(s_ray_ao.traced==traced+1 && s_ray_ao.completed==valid && s_ray_ao.empty_ao==empty+1);
    REQUIRE(s_ray_ao.gpu_stats[0]==0 && s_ray_ao.gpu_stats[1]==0 && s_ray_ao.gpu_stats[2]==0);
    REQUIRE(live_original_preserved(f));

    REQUIRE(live_render(f,0,0,0));s_rt_capture.overflow=1;
    traced=s_ray_ao.traced;valid=s_ray_ao.completed;
    REQUIRE(live_consume(f)==0 && s_ray_ao.traced==traced && s_ray_ao.completed==valid);
    REQUIRE(live_original_preserved(f));s_rt_capture.overflow=0;
    REQUIRE(live_render(f,0,1,0));e=vk_rt_epoch(f->depth);
    REQUIRE(e && e->reason==RSX_SCENE_EPOCH_UNSUPPORTED);
    REQUIRE(live_consume(f)==0 && s_ray_ao.traced==traced && s_ray_ao.completed==valid);

    REQUIRE(live_render(f,0,0,0));
    vk_eng_clear_depth_stencil(NULL,f->depth,RSX_BE_CLEAR_DEPTH,1,0);
    e=vk_rt_epoch(f->depth);REQUIRE(e && e->armed && e->draw_count==0);
    REQUIRE(live_consume(f)==0 && s_ray_ao.traced==traced);
    REQUIRE(live_render(f,0,0,0));u64 frame=s_rt_capture.frame;
    vk_eng_present(NULL,f->color);
    REQUIRE(s_rt_capture.frame==frame+1);
    REQUIRE(live_consume(f)==0 && s_ray_ao.traced==traced);
    REQUIRE(live_render(f,0,0,0));REQUIRE(live_valid(f,0));
    printf("live-ray samples=%u mismatch/empty/overflow/KIL/clear/present checks passed\n",f->samples);
    return 1;
}
int main(int argc,char **argv)
{
    int first=1,last=2,result=1,initialized=0;live_fixture f={0};
    if(argc==2 && strcmp(argv[1],"--single")==0)last=1;
    else if(argc==2 && strcmp(argv[1],"--msaa2")==0)first=2;
    else if(argc!=1){fprintf(stderr,"usage: test_vulkan_ray_ao_live [--single|--msaa2]\n");return 2;}
    live_env("PS3RECOMP_VK_GUEST_PROGRAMS","1");live_env("PS3RECOMP_RSX_AO","rayquery");
    live_env("PS3RECOMP_RENDER_SCALE","1");live_env("PS3RECOMP_VK_WINDOW","0");
    live_env("PS3RECOMP_ANTIALIASING","off");live_env("PS3RECOMP_UPSCALER","off");
    SDL_SetMainReady();
    if(rsx_vulkan_backend_init(LIVE_W,LIVE_H,"actual live ray AO fixture"))goto done;
    initialized=1;
    if(!s_vk.rt_enabled){fprintf(stderr,"UNAVAILABLE: Vulkan BDA/AS/ray query not enabled\n");result=77;goto done;}
    for(int samples=first;samples<=last;++samples) {
        if(!live_create(&f,(u32)samples) || !live_suite(&f))goto done;
        live_release(&f);
    }
    vk_rt_ao_shutdown();
    if(s_ray_ao.ready || s_ray_ao.color || s_scene_batch){fprintf(stderr,"FAIL: AO shutdown retained live state\n");goto done;}
    vk_rt_ao_shutdown(); /* idempotent, backend will invoke it again */
    printf("PASS actual live ray AO fixture: %u checks; synthetic opaque geometry only\n",checks);result=0;
done:
    if(initialized){live_release(&f);rsx_vulkan_backend_shutdown();}
    return result;
}
