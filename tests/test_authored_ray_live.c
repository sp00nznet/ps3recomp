/* Controller-serialized end-to-end authored preview fixture. Root runs GPU.
 * Args: draw044 FGM1v2, exact pool91 VS, exact pool91 PS, [--single|--msaa2].
 * Actual captured skinned geometry/constants, deterministic1x1 texture resources.
 * A behind-camera opaque plane casts a source shadow but never owns raster RGB.
 * This is source-subset preview validation, not gameplay/material coverage. */
#define main authored_unused_live_main
#include "test_vulkan_ray_ao_live.c"
#undef main
#include "../libs/video/rsx_ao_projection.h"
static void *auth_file(const char *path,size_t *size)
{FILE *f=fopen(path,"rb");if(!f)return NULL;if(fseek(f,0,SEEK_END)){fclose(f);return NULL;}long n=ftell(f);if(n<=0||n>16*1024*1024||fseek(f,0,SEEK_SET)){fclose(f);return NULL;}char *b=malloc((size_t)n+1);if(!b){fclose(f);return NULL;}int ok=fread(b,1,(size_t)n,f)==(size_t)n;fclose(f);b[n]=0;*size=(size_t)n;if(!ok){free(b);return NULL;}return b;}
static const float auth_backplane[6][4]={{-100,-100,-1,1},{100,-100,-1,1},{-100,100,-1,1},{100,-100,-1,1},{100,100,-1,1},{-100,100,-1,1}};
static void auth_actor_draw(u32 pipe,const void *vb,u32 nv,u32 stride,const u32 *ib,u32 ni,const void *vc,const void *fp,const u32 *textures,const rsx_be_sampler_desc *samplers)
{
    vk_eng_bind_pipeline(NULL,pipe);vk_eng_bind_vs_constants(NULL,vc,8224);vk_eng_bind_ps_constants(NULL,fp,704);
    vk_eng_bind_textures(NULL,textures,samplers,127);vk_eng_draw(NULL,RSX_TOPOLOGY_TRIANGLES,vb,nv,stride,ib,ni);
}
/* duplicate_first controls coplanar draw order. A zero duplicate is the normal
 * single actor; the blocker is optional so source shadow has its own control. */
static int auth_render(live_fixture *f,const void *vb,u32 nv,u32 stride,const u32 *ib,u32 ni,const void *vc,const void *fp,const u32 *textures,const rsx_be_sampler_desc *samplers,u32 blocker,int actor,u32 duplicate,int duplicate_first)
{
    const float clear[4]={1,0,1,1};vk_eng_clear_color(NULL,f->color,clear);vk_eng_clear_depth_stencil(NULL,f->depth,RSX_BE_CLEAR_DEPTH,1,0);
    vk_eng_bind_targets(NULL,&f->color,1,f->depth);vk_eng_set_viewport(NULL,0,0,LIVE_W,LIVE_H,0,1);vk_eng_set_scissor(NULL,0,0,f->width,LIVE_H);
    if(actor){if(duplicate && duplicate_first)auth_actor_draw(duplicate,vb,nv,stride,ib,ni,vc,fp,textures,samplers);
        auth_actor_draw(f->pipe,vb,nv,stride,ib,ni,vc,fp,textures,samplers);
        if(duplicate && !duplicate_first)auth_actor_draw(duplicate,vb,nv,stride,ib,ni,vc,fp,textures,samplers);}
    if(blocker){vk_eng_bind_pipeline(NULL,blocker);vk_eng_bind_vs_constants(NULL,f->constants,sizeof f->constants);vk_eng_bind_ps_constants(NULL,NULL,0);vk_eng_bind_textures(NULL,textures,samplers,0);vk_eng_draw(NULL,RSX_TOPOLOGY_TRIANGLES,auth_backplane,6,16,NULL,0);}
    vk_flush_pending(0);REQUIRE(live_read(f,f->color,f->before));
    float *depth=malloc((size_t)f->width*LIVE_H*sizeof(float));REQUIRE(depth);
    REQUIRE(vk_eng_depth_readback(NULL,f->depth,0,0,f->width,LIVE_H,depth,f->width*4)==0);
    u32 covered=0;float minimum=1;for(u32 i=0;i<f->width*LIVE_H;i++)if(depth[i]<1){covered++;if(depth[i]<minimum)minimum=depth[i];}
    printf("authored fixture raster samples=%u covered=%u minimumDepth=%g\n",f->samples,covered,minimum);free(depth);REQUIRE(actor ? covered>0 : covered==0);return 1;
}
static int auth_suite(const char *geometry,const char *vs_path,const char *ps_path,u32 samples)
{
    size_t ng=0,nv=0,np=0;unsigned char *g=auth_file(geometry,&ng);char *vs=auth_file(vs_path,&nv),*ps=auth_file(ps_path,&np);REQUIRE(g&&vs&&ps&&ng>=128);
    u32 h[32];memcpy(h,g,128);REQUIRE(h[0]==0x314d4746 && h[1]==2 && h[29]==91 && h[30]==8224 && h[31]==704 && h[7]<=16 && h[5]==h[7]*16);
    size_t vb=(size_t)h[4]*h[5],ib=(size_t)h[6]*4;REQUIRE(128+vb+ib+h[30]+h[31]==ng);
    const void *vertices=g+128,*vc=g+128+vb+ib,*fp=(const unsigned char*)vc+8224;const u32 *indices=(const u32*)(g+128+vb);
    char vh[65],ph[65];REQUIRE(vk_rt_source_sha256(vs,vh)&&vk_rt_source_sha256(ps,ph)&&rsx_authored__pair(vh,ph)==1);
    live_fixture f={0};REQUIRE(live_create(&f,samples));u32 blocker=f.pipe;f.pipe=0;
    rsx_vertex_layout_plan layout;memset(&layout,0,sizeof layout);memset(layout.slot_by_attr,-1,sizeof layout.slot_by_attr);layout.count=(u8)h[7];layout.stride=h[5];
    for(u32 a=0;a<h[7];a++){REQUIRE(h[8+a]<16);layout.attrs[a]=(u8)h[8+a];layout.slot_by_attr[h[8+a]]=(s8)a;layout.mask|=1u<<h[8+a];}
    rsx_be_render_state state={0};state.color_mask=0x01010101;state.antialias_control=0xffff0001;state.depth_test=state.depth_write=1;state.depth_func=0x203;
    f.pipe=vk_eng_pipeline_create(NULL,vs,ps,&state,&layout,h[5],RSX_BE_FMT_R8G8B8A8,1);REQUIRE(f.pipe);
    /* A byte-different shader with identical instructions creates a genuinely
     * unknown color writer, including the hard case of equal raster RGB. */
    char *unknown_ps=malloc(np+64);REQUIRE(unknown_ps);
    memcpy(unknown_ps,ps,np);strcpy(unknown_ps+np,"\n// unqualified coplanar color owner\n");
    u32 unknown=vk_eng_pipeline_create(NULL,vs,unknown_ps,&state,&layout,h[5],RSX_BE_FMT_R8G8B8A8,1);REQUIRE(unknown);
    state.color_mask=0;
    u32 depth_only=vk_eng_pipeline_create(NULL,vs,ps,&state,&layout,h[5],RSX_BE_FMT_R8G8B8A8,1);REQUIRE(depth_only);
    const float (*constants)[4]=(const float(*)[4])vc;float effective[8];rsx_ao_projection projection;
    REQUIRE(rsx_ao_viewport_transform((const float*)(constants+512),0,1,effective));
    REQUIRE(rsx_ao_projection_extract(constants,effective,effective+4,(float)LIVE_W/LIVE_H,&projection));memcpy(f.projection,projection.matrix,sizeof f.projection);
    rsx_scene_draw_input proof={0};proof.vertices=vertices;proof.vertex_count=h[4];proof.layout=layout;
    REQUIRE(vk_ray_unit_position(&proof,1)==RSX_AUTHORED_POSITION_UNIT_HOMOGENEOUS);
    rsx_scene_vertex_output *replay=calloc(h[4],sizeof(*replay));REQUIRE(replay);
    REQUIRE(rsx_vulkan_backend_replay_outputs(vs,&layout,vertices,h[4],vc,h[30],replay,h[4])==0);
    /* The saved camera-turn capture includes this mesh BEHIND the camera.
     * Reframe only this fixture's copied camera translation. The original
     * source, skinning, material and world lights remain intact. This is a
     * controlled fixture, not a reproduction of the recorded gameplay view. */
    float lo[3]={INFINITY,INFINITY,INFINITY},hi[3]={-INFINITY,-INFINITY,-INFINITY};
    for(u32 i=0;i<h[4];i++)for(u32 k=0;k<3;k++){
        float v=replay[i].pos[k==2?3:k]/(k==2?1:f.projection[k==0?0:5]);REQUIRE(isfinite(v));
        if(v<lo[k])lo[k]=v;if(v>hi[k])hi[k]=v;
    }
    float radius=fmaxf(hi[2]-lo[2],fmaxf((hi[0]-lo[0])*f.projection[0],(hi[1]-lo[1])*f.projection[5]));
    float shift[3]={-(lo[0]+hi[0])*.5f,-(lo[1]+hi[1])*.5f,fmaxf(4*projection.near_z,2*radius)-(lo[2]+hi[2])*.5f};
    float (*fixture_vc)[4]=(float(*)[4])(void*)vc;
    fixture_vc[0][3]+=shift[0]*f.projection[0];fixture_vc[1][3]+=shift[1]*f.projection[5];
    fixture_vc[2][3]+=shift[2]*(2*f.projection[10]-1);fixture_vc[3][3]+=shift[2];
    /* A controlled directional source toward the camera makes the external
     * behind-camera blocker an unambiguous direct-shadow test. Recover this
     * direction through the same runtime affine transform as gameplay. */
    for(u32 k=0;k<3;k++)fixture_vc[12][k]=-300*fixture_vc[3][k];fixture_vc[12][3]=0;
    float (*fixture_fp)[4]=(float(*)[4])(void*)fp;
    for(u32 k=0;k<3;k++){fixture_fp[16][k]=fixture_fp[18][k]=fixture_fp[19][k]=0;fixture_fp[17][k]=.5f;}
    /* Remove the captured unshadowed additive boosts in this isolated source
     * test so output clamping cannot hide a real direct-shadow difference. */
    fixture_fp[22][0]=0;
    for(u32 k=0;k<3;k++)fixture_fp[21][k]=fixture_fp[24][k]=0;
    REQUIRE(rsx_vulkan_backend_replay_outputs(vs,&layout,vertices,h[4],vc,h[30],replay,h[4])==0);
    u32 front=0;for(u32 i=0;i<h[4];i++)if(replay[i].pos[3]>0)front++;
    printf("fixture varyings col0=%g,%g,%g,%g col1=%g,%g,%g,%g\n",replay[0].col0[0],replay[0].col0[1],replay[0].col0[2],replay[0].col0[3],replay[0].col1[0],replay[0].col1[1],replay[0].col1[2],replay[0].col1[3]);
    printf("authored fixture camera shift=%g,%g,%g front=%u/%u\n",shift[0],shift[1],shift[2],front,h[4]);REQUIRE(front==h[4]);free(replay);
    f.constants[0]=f.projection[0];f.constants[1]=f.projection[5];f.constants[2]=f.projection[10];f.constants[3]=f.projection[14];
    u32 textures[16]={0};rsx_be_sampler_desc samplers[16]={0};const u8 texel[7][4]={{128,128,255,255},{102,153,204,255},{128,64,32,255},{64,128,192,255},{192,192,192,255},{255,192,128,255},{32,64,128,255}};
    for(u32 u=0;u<7;u++){textures[u]=vk_eng_texture_create(NULL,RSX_BE_FMT_R8G8B8A8,1,1,1,u==3?6:1,0xAAE4,0x85);REQUIRE(textures[u]);for(u32 face=0;face<(u==3?6u:1u);face++)vk_eng_texture_upload(NULL,textures[u],face,0,1,1,texel[u],4,1);samplers[u].wrap_s=samplers[u].wrap_t=samplers[u].wrap_r=3;}
#define AUTH_RENDER(B,A,D,FIRST) auth_render(&f,vertices,h[4],h[5],indices,h[6],vc,fp,textures,samplers,(B),(A),(D),(FIRST))
    /* Disable recovered source records only in this owned synthetic control;
     * original PS constants/texture resources remain identical. */
    s_ray_ao.test_disable_authored_sources=1;
    s_ray_ao.test_disable_authored_bounce=1;
    s_ray_ao.test_disable_authored_reflections=1;
    REQUIRE(AUTH_RENDER(blocker,1,0,0));
    REQUIRE(live_consume(&f)!=0 && s_ray_ao.gpu_stats[24]>0);REQUIRE(live_read(&f,s_ray_ao.color,f.output));REQUIRE(!memcmp(f.before,f.output,f.bytes));
    s_ray_ao.test_disable_authored_sources=0;
    /* Baseline and blocker must both have eligible source pixels. The GPU
     * source-shadow counter separates direct visibility from bounce changes. */
    REQUIRE(AUTH_RENDER(0,1,0,0));
    REQUIRE(live_consume(&f)!=0 && s_ray_ao.gpu_stats[24]>0);REQUIRE(live_read(&f,s_ray_ao.color,f.output));
    unsigned char *unblocked=malloc(f.bytes);REQUIRE(unblocked);memcpy(unblocked,f.output,f.bytes);
    REQUIRE(AUTH_RENDER(blocker,1,0,0));
    REQUIRE(live_consume(&f)!=0 && s_ray_ao.gpu_stats[24]>0 && s_ray_ao.gpu_stats[25]>0 && s_ray_ao.gpu_stats[28]>0);
    REQUIRE(live_read(&f,s_ray_ao.color,f.output));REQUIRE(memcmp(f.before,f.output,f.bytes)!=0);
    REQUIRE(memcmp(unblocked,f.output,f.bytes)!=0);free(unblocked);
    REQUIRE(live_original_preserved(&f));printf("authored samples=%u rasterAgreed=%u changed=%u\n",samples,s_ray_ao.gpu_stats[24],s_ray_ao.gpu_stats[25]);
    /* The depth-only copy is ignored as a color owner. Equal-depth unknown
     * color writers are ambiguous in both primitive/draw orders, even when
     * their shader produces exactly the same RGB as the qualified source. */
    REQUIRE(AUTH_RENDER(blocker,1,depth_only,1));
    REQUIRE(live_consume(&f)!=0 && s_ray_ao.gpu_stats[24]>0 && s_ray_ao.gpu_stats[25]>0);
    REQUIRE(live_read(&f,s_ray_ao.color,f.output));REQUIRE(memcmp(f.before,f.output,f.bytes)!=0);REQUIRE(live_original_preserved(&f));
    for(int first=0;first<2;first++){
        REQUIRE(AUTH_RENDER(blocker,1,unknown,first));
        (void)live_consume(&f);REQUIRE(s_ray_ao.gpu_stats[27]>0);
        REQUIRE(live_read(&f,s_ray_ao.color,f.output));REQUIRE(!memcmp(f.before,f.output,f.bytes));REQUIRE(live_original_preserved(&f));
    }
    /* Resource mutation is an epoch invalidation, not a stale color sample. */
    REQUIRE(AUTH_RENDER(blocker,1,0,0));
    vk_eng_texture_upload(NULL,textures[1],0,0,1,1,texel[1],4,1);REQUIRE(vk_rt_epoch(f.depth)->reason==RSX_SCENE_EPOCH_RESOURCE_CHANGED);REQUIRE(!live_consume(&f));REQUIRE(live_original_preserved(&f));
    /* Missing only RGB cube disables material relighting; geometry remains. */
    REQUIRE(AUTH_RENDER(blocker,1,0,0));
    rsx_scene_epoch *epoch=vk_rt_epoch(f.depth);REQUIRE(epoch && epoch->draw_count==2);epoch->draws[0].draw.texture_mask&=~8u;epoch->draws[0].draw.textures[3]=0;
    REQUIRE(!live_consume(&f));REQUIRE(live_original_preserved(&f));
    /* Unknown primary materials preserve RGB, including behind-camera geometry. */
    REQUIRE(AUTH_RENDER(blocker,0,0,0));REQUIRE(!live_consume(&f));REQUIRE(live_original_preserved(&f));
#undef AUTH_RENDER
    s_ray_ao.test_disable_authored_sources=0;
    s_ray_ao.test_disable_authored_bounce=0;
    s_ray_ao.test_disable_authored_reflections=0;
    for(u32 u=0;u<7;u++)vk_eng_release(NULL,textures[u]);vk_eng_pipeline_release(NULL,unknown);vk_eng_pipeline_release(NULL,depth_only);vk_eng_pipeline_release(NULL,blocker);live_release(&f);free(unknown_ps);free(g);free(vs);free(ps);return 1;
}
#ifndef AUTHORED_RAY_LIVE_NO_MAIN
int main(int argc,char **argv)
{
    if(argc<4||argc>5){fprintf(stderr,"usage: test_authored_ray_live geometry VS PS [--single|--msaa2]\n");return 2;}
    int first=1,last=2;if(argc==5){if(!strcmp(argv[4],"--single"))last=1;else if(!strcmp(argv[4],"--msaa2"))first=2;else return 2;}
    live_env("PS3RECOMP_VK_GUEST_PROGRAMS","1");live_env("PS3RECOMP_RSX_AO","rayquery");live_env("PS3RECOMP_RSX_AUTHORED_LIGHTS","1");live_env("PS3RECOMP_RENDER_SCALE","1");live_env("PS3RECOMP_VK_WINDOW","0");live_env("PS3RECOMP_ANTIALIASING","off");live_env("PS3RECOMP_UPSCALER","off");SDL_SetMainReady();
    if(rsx_vulkan_backend_init(LIVE_W,LIVE_H,"authored source end-to-end fixture"))return 77;int result=1;if(!s_vk.rt_enabled){result=77;goto done;}
    for(int samples=first;samples<=last;samples++)if(!auth_suite(argv[1],argv[2],argv[3],(u32)samples))goto done;
    live_env("PS3RECOMP_RSX_AUTHORED_LIGHTS","0");live_env("PS3RECOMP_RSX_PATH_PREVIEW","1");
    for(int samples=first;samples<=last;samples++){live_fixture f={0};if(!live_create(&f,(u32)samples)||!live_suite(&f)){live_release(&f);goto done;}live_release(&f);}
    printf("PASS %u authored renderer and existing path-preview regression checks\n",checks);result=0;
done:rsx_vulkan_backend_shutdown();return result;
}
#endif
