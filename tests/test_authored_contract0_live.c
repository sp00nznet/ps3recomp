/* Contract0 authored RGB end-to-end fixture. The caller supplies capture012725
 * draw077 FGM1v2 and the exact pipe71 VS/PS sources. Geometry, deformation,
 * source programs and sampler bindings are captured; camera and lighting are
 * controlled fixture values, not a replay of the recorded gameplay view.
 * The controller runs builds/GPU checks. */
#define main contract0_unused_live_main
#include "test_vulkan_ray_ao_live.c"
#undef main
#include "../libs/video/rsx_ao_projection.h"
#include "../libs/video/rsx_authored_lights.h"

static void *contract0_file(const char *path,size_t *size)
{
    FILE *f=fopen(path,"rb");if(!f)return NULL;
    if(fseek(f,0,SEEK_END)){fclose(f);return NULL;}
    long n=ftell(f);if(n<=0||n>16*1024*1024||fseek(f,0,SEEK_SET)){fclose(f);return NULL;}
    char *b=malloc((size_t)n+1);if(!b){fclose(f);return NULL;}
    int ok=fread(b,1,(size_t)n,f)==(size_t)n;fclose(f);b[n]=0;
    if(!ok){free(b);return NULL;}*size=(size_t)n;return b;
}

/* The light is directed toward -view-Z. The plane is behind the near plane,
 * so it can occlude source rays without writing raster depth or color. */
static const float contract0_backplane[6][4]={
    {-100,-100,-1,1},{100,-100,-1,1},{-100,100,-1,1},
    {100,-100,-1,1},{100,100,-1,1},{-100,100,-1,1}};

static int contract0_render(live_fixture *f,const void *vb,u32 nv,u32 stride,
    const u32 *ib,u32 ni,const void *vc,const void *fp,const u32 textures[16],
    const rsx_be_sampler_desc samplers[16],u32 blocker,int with_blocker)
{
    const float clear[4]={1,0,1,1};
    vk_eng_clear_color(NULL,f->color,clear);
    vk_eng_clear_depth_stencil(NULL,f->depth,RSX_BE_CLEAR_DEPTH,1,0);
    vk_eng_bind_targets(NULL,&f->color,1,f->depth);
    vk_eng_set_viewport(NULL,0,0,LIVE_W,LIVE_H,0,1);
    vk_eng_set_scissor(NULL,0,0,f->width,LIVE_H);
    vk_eng_bind_pipeline(NULL,f->pipe);vk_eng_bind_vs_constants(NULL,vc,8224);
    vk_eng_bind_ps_constants(NULL,fp,256);vk_eng_bind_textures(NULL,textures,samplers,7);
    vk_eng_draw(NULL,RSX_TOPOLOGY_TRIANGLES,vb,nv,stride,ib,ni);
    if(with_blocker){
        vk_eng_bind_pipeline(NULL,blocker);
        vk_eng_bind_vs_constants(NULL,f->constants,sizeof f->constants);
        vk_eng_bind_ps_constants(NULL,NULL,0);
        vk_eng_bind_textures(NULL,textures,samplers,0);
        vk_eng_draw(NULL,RSX_TOPOLOGY_TRIANGLES,contract0_backplane,6,16,NULL,0);
    }
    vk_flush_pending(0);REQUIRE(live_read(f,f->color,f->before));
    float *depth=malloc((size_t)f->width*LIVE_H*sizeof(float));REQUIRE(depth);
    REQUIRE(vk_eng_depth_readback(NULL,f->depth,0,0,f->width,LIVE_H,depth,f->width*4)==0);
    u32 covered=0;for(u32 i=0;i<f->width*LIVE_H;i++)if(depth[i]<1)covered++;
    free(depth);printf("contract0 raster samples=%u covered=%u\n",f->samples,covered);
    REQUIRE(covered>0);return 1;
}

static int contract0_suite(const char *geometry,const char *vs_path,const char *ps_path,u32 samples)
{
    size_t ng=0,nv=0,np=0;unsigned char *g=contract0_file(geometry,&ng);
    char *vs=contract0_file(vs_path,&nv),*ps=contract0_file(ps_path,&np);
    REQUIRE(g&&vs&&ps&&ng>=128);
    u32 h[32];memcpy(h,g,128);
    REQUIRE(h[0]==0x314d4746u && h[1]==2u && h[29]==71u &&
        h[30]==8224u && h[31]==256u && h[7]<=16u && h[5]==h[7]*16u);
    size_t vb=(size_t)h[4]*h[5],ib=(size_t)h[6]*4;
    REQUIRE(128+vb+ib+8224u+256u==ng);
    const void *vertices=g+128;const u32 *indices=(const u32*)(g+128+vb);
    float (*vc)[4]=(float(*)[4])(g+128+vb+ib);
    float (*fp)[4]=(float(*)[4])((unsigned char*)vc+8224);
    char vh[65],ph[65];REQUIRE(vk_rt_source_sha256(vs,vh) &&
        vk_rt_source_sha256(ps,ph) && rsx_authored__pair(vh,ph)==0);
    live_fixture f={0};REQUIRE(live_create(&f,samples));
    u32 blocker=f.pipe;f.pipe=0;
    rsx_vertex_layout_plan layout;memset(&layout,0,sizeof layout);
    memset(layout.slot_by_attr,-1,sizeof layout.slot_by_attr);
    layout.count=(u8)h[7];layout.stride=h[5];
    for(u32 a=0;a<h[7];a++){
        REQUIRE(h[8+a]<16u);layout.attrs[a]=(u8)h[8+a];
        layout.slot_by_attr[h[8+a]]=(s8)a;layout.mask|=1u<<h[8+a];
    }
    rsx_be_render_state state={0};state.color_mask=0x01010101;
    state.antialias_control=0xffff0001;state.depth_test=state.depth_write=1;
    state.depth_func=0x203;state.alpha_test_enable=1;state.alpha_func=0x204;
    state.alpha_ref_raw=0;state.alpha_ref_format=RSX_BE_FMT_R8G8B8A8;
    f.pipe=vk_eng_pipeline_create(NULL,vs,ps,&state,&layout,h[5],RSX_BE_FMT_R8G8B8A8,1);
    REQUIRE(f.pipe);
    float effective[8];rsx_ao_projection camera;
    REQUIRE(rsx_ao_viewport_transform((const float*)(vc+512),0,1,effective));
    REQUIRE(rsx_ao_projection_extract((const float(*)[4])vc,effective,effective+4,
        (float)LIVE_W/LIVE_H,&camera));
    memcpy(f.projection,camera.matrix,sizeof f.projection);
    rsx_scene_draw_input proof={0};proof.vertices=vertices;proof.vertex_count=h[4];proof.layout=layout;
    REQUIRE(vk_ray_unit_position(&proof,0)==RSX_AUTHORED_POSITION_UNIT_HOMOGENEOUS);
    rsx_scene_vertex_output *replay=calloc(h[4],sizeof(*replay));REQUIRE(replay);
    REQUIRE(rsx_vulkan_backend_replay_outputs(vs,&layout,vertices,h[4],vc,8224,replay,h[4])==0);
    float lo[3]={INFINITY,INFINITY,INFINITY},hi[3]={-INFINITY,-INFINITY,-INFINITY};
    for(u32 i=0;i<h[4];i++)for(u32 k=0;k<3;k++){
        float v=replay[i].pos[k==2?3:k]/(k==2?1:f.projection[k==0?0:5]);
        REQUIRE(isfinite(v));if(v<lo[k])lo[k]=v;if(v>hi[k])hi[k]=v;
    }
    float radius=fmaxf(hi[2]-lo[2],fmaxf((hi[0]-lo[0])*f.projection[0],
        (hi[1]-lo[1])*f.projection[5]));
    float shift[3]={-(lo[0]+hi[0])*.5f,-(lo[1]+hi[1])*.5f,
        fmaxf(4*camera.near_z,2*radius)-(lo[2]+hi[2])*.5f};
    vc[0][3]+=shift[0]*f.projection[0];vc[1][3]+=shift[1]*f.projection[5];
    vc[2][3]+=shift[2]*(2*f.projection[10]-1);vc[3][3]+=shift[2];
    /* Controlled test coefficients isolate source12 (FP c7). Source13 is off;
     * retained constant/vertex/fog terms are zeroed only in these copied FP
     * constants, so clipping cannot hide the direct source difference. */
    for(u32 k=0;k<3;k++){
        fp[0][k]=fp[1][k]=fp[6][k]=fp[8][k]=fp[10][k]=0;
        fp[7][k]=.5f;fp[12][k]=fp[13][k]=0;
    }
    fp[9][0]=0;fp[14][0]=.5f;fp[15][0]=0;
    /* The source direction is expressed in its original light coordinates:
     * transpose of the qualified light-to-view rotation maps (0,0,-300). */
    rsx_authored_lights lights;
    REQUIRE(rsx_authored_lights_recover(vh,ph,(const float(*)[4])vc,8224,fp,256,
        &f.projection[0],0,1,RSX_AUTHORED_POSITION_UNIT_HOMOGENEOUS,&lights));
    for(u32 k=0;k<3;k++)vc[12][k]=-300*lights.light_to_view[2][k];
    vc[12][3]=0;
    REQUIRE(rsx_authored_lights_recover(vh,ph,(const float(*)[4])vc,8224,fp,256,
        &f.projection[0],0,1,RSX_AUTHORED_POSITION_UNIT_HOMOGENEOUS,&lights));
    REQUIRE(lights.lights[1].status==RSX_AUTHORED_ACTIVE && lights.lights[1].view[2]<0);
    REQUIRE(rsx_vulkan_backend_replay_outputs(vs,&layout,vertices,h[4],vc,8224,replay,h[4])==0);
    u32 front=0;for(u32 i=0;i<h[4];i++)if(replay[i].pos[3]>0)front++;
    printf("contract0 fixture shift=%g,%g,%g front=%u/%u\n",
        shift[0],shift[1],shift[2],front,h[4]);REQUIRE(front==h[4]);free(replay);
    f.constants[0]=f.projection[0];f.constants[1]=f.projection[5];
    f.constants[2]=f.projection[10];f.constants[3]=f.projection[14];
    u32 textures[16]={0};rsx_be_sampler_desc samplers[16]={0};
    const u8 texel[3][4]={{102,153,204,255},{191,191,191,255},{255,255,255,255}};
    for(u32 u=0;u<3;u++){
        textures[u]=vk_eng_texture_create(NULL,RSX_BE_FMT_R8G8B8A8,1,1,1,1,0xAAE4,0x85);
        REQUIRE(textures[u]);vk_eng_texture_upload(NULL,textures[u],0,0,1,1,texel[u],4,1);
        samplers[u].wrap_s=samplers[u].wrap_t=samplers[u].wrap_r=3;
    }
#define CONTRACT0_RENDER(B) contract0_render(&f,vertices,h[4],h[5],indices,h[6],vc,fp,textures,samplers,blocker,(B))
    s_ray_ao.test_disable_authored_sources=1;s_ray_ao.test_disable_authored_bounce=1;
    REQUIRE(CONTRACT0_RENDER(1));
    REQUIRE(live_consume(&f)!=0 && s_ray_ao.gpu_stats[24]>0);
    REQUIRE(live_read(&f,s_ray_ao.color,f.output));
    REQUIRE(!memcmp(f.before,f.output,f.bytes));REQUIRE(live_original_preserved(&f));
    s_ray_ao.test_disable_authored_sources=0;
    REQUIRE(CONTRACT0_RENDER(0));
    REQUIRE(live_consume(&f)!=0 && s_ray_ao.gpu_stats[24]>0);
    REQUIRE(live_read(&f,s_ray_ao.color,f.output));
    unsigned char *unblocked=malloc(f.bytes),*raster=malloc(f.bytes);
    REQUIRE(unblocked&&raster);memcpy(unblocked,f.output,f.bytes);
    memcpy(raster,f.before,f.bytes);
    REQUIRE(CONTRACT0_RENDER(1));
    REQUIRE(!memcmp(raster,f.before,f.bytes));
    REQUIRE(live_consume(&f)!=0 && s_ray_ao.gpu_stats[24]>0 &&
        s_ray_ao.gpu_stats[25]>0 && s_ray_ao.gpu_stats[28]>0);
    REQUIRE(live_read(&f,s_ray_ao.color,f.output));
    REQUIRE(memcmp(unblocked,f.output,f.bytes)!=0);
    REQUIRE(memcmp(f.before,f.output,f.bytes)!=0);
    REQUIRE(live_original_preserved(&f));
    printf("contract0 samples=%u agreed=%u changed=%u shadowed=%u\n",
        samples,s_ray_ao.gpu_stats[24],s_ray_ao.gpu_stats[25],s_ray_ao.gpu_stats[28]);
    free(unblocked);free(raster);
#undef CONTRACT0_RENDER
    s_ray_ao.test_disable_authored_sources=0;s_ray_ao.test_disable_authored_bounce=0;
    for(u32 u=0;u<3;u++)vk_eng_release(NULL,textures[u]);
    vk_eng_pipeline_release(NULL,blocker);live_release(&f);
    free(g);free(vs);free(ps);return 1;
}

int main(int argc,char **argv)
{
    if(argc<4||argc>5){fprintf(stderr,"usage: test_authored_contract0_live geometry VS PS [--single|--msaa2]\n");return 2;}
    int first=1,last=2;
    if(argc==5){if(!strcmp(argv[4],"--single"))last=1;
        else if(!strcmp(argv[4],"--msaa2"))first=2;else return 2;}
    live_env("PS3RECOMP_VK_GUEST_PROGRAMS","1");
    live_env("PS3RECOMP_RSX_AO","rayquery");
    live_env("PS3RECOMP_RSX_AUTHORED_LIGHTS","1");
    live_env("PS3RECOMP_RENDER_SCALE","1");
    live_env("PS3RECOMP_VK_WINDOW","0");
    live_env("PS3RECOMP_ANTIALIASING","off");
    live_env("PS3RECOMP_UPSCALER","off");SDL_SetMainReady();
    if(rsx_vulkan_backend_init(LIVE_W,LIVE_H,"contract0 authored source fixture"))return 77;
    int result=1;if(!s_vk.rt_enabled){result=77;goto done;}
    for(int samples=first;samples<=last;samples++)
        if(!contract0_suite(argv[1],argv[2],argv[3],(u32)samples))goto done;
    printf("PASS %u contract0 authored source checks\n",checks);result=0;
done:rsx_vulkan_backend_shutdown();return result;
}
