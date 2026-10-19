/* Renderer-level, source-qualified single-bounce fixture. Caller supplies the
 * captured contract0 draw077 FGM1v2 and exact pipe71 VS/PS. Captured geometry
 * and programs are unchanged; camera, FP terms, and 1x1 textures are synthetic
 * controlled fixture values. No claim of recovered albedo or physical BRDF. */
#define main bounce_unused_live_main
#include "test_vulkan_ray_ao_live.c"
#undef main
#include "../libs/video/rsx_ao_projection.h"
#include "../libs/video/rsx_authored_lights.h"

static void *bounce_file(const char *path,size_t *size)
{
    FILE *f=fopen(path,"rb");if(!f)return NULL;
    if(fseek(f,0,SEEK_END)){fclose(f);return NULL;}
    long n=ftell(f);if(n<=0||n>16*1024*1024||fseek(f,0,SEEK_SET)){fclose(f);return NULL;}
    char *b=malloc((size_t)n+1);if(!b){fclose(f);return NULL;}
    int ok=fread(b,1,(size_t)n,f)==(size_t)n;fclose(f);b[n]=0;
    if(!ok){free(b);return NULL;}*size=(size_t)n;return b;
}

static int bounce_render(live_fixture *f,const void *vertices,u32 count,u32 stride,
    const u32 *indices,u32 index_count,const float receiver_vc[514][4],
    const float secondary_vc[514][4],const float fp[16][4],
    const u32 receiver_tex[16],const u32 secondary_tex[16],
    const rsx_be_sampler_desc samplers[16])
{
    const float clear[4]={1,0,1,1};
    vk_eng_clear_color(NULL,f->color,clear);
    vk_eng_clear_depth_stencil(NULL,f->depth,RSX_BE_CLEAR_DEPTH,1,0);
    vk_eng_bind_targets(NULL,&f->color,1,f->depth);
    vk_eng_set_viewport(NULL,0,0,LIVE_W,LIVE_H,0,1);
    vk_eng_set_scissor(NULL,0,0,f->width,LIVE_H);
    vk_eng_bind_pipeline(NULL,f->pipe);
    vk_eng_bind_ps_constants(NULL,fp,256);
    vk_eng_bind_vs_constants(NULL,receiver_vc,8224);
    vk_eng_bind_textures(NULL,receiver_tex,samplers,7);
    vk_eng_draw(NULL,RSX_TOPOLOGY_TRIANGLES,vertices,count,stride,indices,index_count);
    /* The same qualified surface is translated wholly behind the camera.
     * It is a ray-visible source surface, but contributes no raster pixels. */
    vk_eng_bind_vs_constants(NULL,secondary_vc,8224);
    vk_eng_bind_textures(NULL,secondary_tex,samplers,7);
    vk_eng_draw(NULL,RSX_TOPOLOGY_TRIANGLES,vertices,count,stride,indices,index_count);
    vk_flush_pending(0);
    REQUIRE(live_read(f,f->color,f->before));return 1;
}

static int bounce_pass(live_fixture *f,const void *vertices,u32 count,u32 stride,
    const u32 *indices,u32 index_count,const float receiver_vc[514][4],
    const float secondary_vc[514][4],const float fp[16][4],
    const u32 receiver_tex[16],const u32 secondary_tex[16],
    const rsx_be_sampler_desc samplers[16],int disable_bounce,
    unsigned char *raster,unsigned char *output,u32 *bounced)
{
    s_ray_ao.test_disable_authored_sources=0;
    s_ray_ao.test_disable_authored_bounce=disable_bounce;
    REQUIRE(bounce_render(f,vertices,count,stride,indices,index_count,
        receiver_vc,secondary_vc,fp,receiver_tex,secondary_tex,samplers));
    memcpy(raster,f->before,f->bytes);
    REQUIRE(live_consume(f)!=0 && s_ray_ao.gpu_stats[24]>0);
    *bounced=s_ray_ao.gpu_stats[26];
    REQUIRE(live_read(f,s_ray_ao.color,output));
    REQUIRE(live_original_preserved(f));
    return 1;
}

static int bounce_suite(const char *geometry,const char *vs_path,const char *ps_path,u32 samples)
{
    size_t ng=0,nv=0,np=0;unsigned char *g=bounce_file(geometry,&ng);
    char *vs=bounce_file(vs_path,&nv),*ps=bounce_file(ps_path,&np);
    REQUIRE(g&&vs&&ps&&ng>=128);
    u32 h[32];memcpy(h,g,128);
    REQUIRE(h[0]==0x314d4746u && h[1]==2u && h[29]==71u &&
        h[30]==8224u && h[31]==256u && h[7]==4u && h[5]==64u);
    size_t vb=(size_t)h[4]*h[5],ib=(size_t)h[6]*4;
    REQUIRE(128+vb+ib+8224u+256u==ng);
    const void *vertices=g+128;const u32 *indices=(const u32*)(g+128+vb);
    float (*receiver_vc)[4]=(float(*)[4])(g+128+vb+ib);
    float (*fp)[4]=(float(*)[4])((unsigned char*)receiver_vc+8224);
    float (*secondary_vc)[4]=malloc(8224);REQUIRE(secondary_vc);
    char vh[65],ph[65];REQUIRE(vk_rt_source_sha256(vs,vh) &&
        vk_rt_source_sha256(ps,ph) && rsx_authored__pair(vh,ph)==0);
    live_fixture f={0};REQUIRE(live_create(&f,samples));
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
    vk_eng_pipeline_release(NULL,f.pipe);
    f.pipe=vk_eng_pipeline_create(NULL,vs,ps,&state,&layout,h[5],RSX_BE_FMT_R8G8B8A8,1);
    REQUIRE(f.pipe);
    float effective[8];rsx_ao_projection camera;
    REQUIRE(rsx_ao_viewport_transform((const float*)(receiver_vc+512),0,1,effective));
    REQUIRE(rsx_ao_projection_extract((const float(*)[4])receiver_vc,effective,effective+4,
        (float)LIVE_W/LIVE_H,&camera));
    memcpy(f.projection,camera.matrix,sizeof f.projection);
    rsx_scene_draw_input proof={0};proof.vertices=vertices;proof.vertex_count=h[4];proof.layout=layout;
    REQUIRE(vk_ray_unit_position(&proof,0)==RSX_AUTHORED_POSITION_UNIT_HOMOGENEOUS);
    rsx_scene_vertex_output *replay=calloc(h[4],sizeof(*replay));REQUIRE(replay);
    REQUIRE(rsx_vulkan_backend_replay_outputs(vs,&layout,vertices,h[4],receiver_vc,8224,replay,h[4])==0);
    float lo[3]={INFINITY,INFINITY,INFINITY},hi[3]={-INFINITY,-INFINITY,-INFINITY};
    for(u32 i=0;i<h[4];i++)for(u32 k=0;k<3;k++){
        float v=replay[i].pos[k==2?3:k]/(k==2?1:f.projection[k==0?0:5]);
        REQUIRE(isfinite(v));if(v<lo[k])lo[k]=v;if(v>hi[k])hi[k]=v;
    }
    float radius=fmaxf(hi[2]-lo[2],fmaxf((hi[0]-lo[0])*f.projection[0],
        (hi[1]-lo[1])*f.projection[5]));
    float shift[3]={-(lo[0]+hi[0])*.5f,-(lo[1]+hi[1])*.5f,
        fmaxf(4*camera.near_z,2*radius)-(lo[2]+hi[2])*.5f};
    receiver_vc[0][3]+=shift[0]*f.projection[0];
    receiver_vc[1][3]+=shift[1]*f.projection[5];
    receiver_vc[2][3]+=shift[2]*(2*f.projection[10]-1);
    receiver_vc[3][3]+=shift[2];
    /* Preserve original source programs and mesh. Controlled terms avoid
     * clipping, so source-owned diffuse hue remains visible at the receiver. */
    for(u32 k=0;k<3;k++){
        fp[0][k]=fp[1][k]=fp[6][k]=fp[8][k]=fp[10][k]=0;
        fp[7][k]=.35f;fp[12][k]=fp[13][k]=0;
    }
    fp[9][0]=0;fp[14][0]=.5f;fp[15][0]=0;
    rsx_authored_lights lights;
    REQUIRE(rsx_authored_lights_recover(vh,ph,(const float(*)[4])receiver_vc,8224,
        fp,256,f.projection,0,1,RSX_AUTHORED_POSITION_UNIT_HOMOGENEOUS,&lights));
    /* Illuminate the secondary from its receiver-facing side at an angle,
     * rather than putting the receiver directly between source and secondary. */
    for(u32 k=0;k<3;k++)receiver_vc[12][k]=300*(lights.light_to_view[0][k]+lights.light_to_view[2][k]);
    receiver_vc[12][3]=0;
    REQUIRE(rsx_authored_lights_recover(vh,ph,(const float(*)[4])receiver_vc,8224,
        fp,256,f.projection,0,1,RSX_AUTHORED_POSITION_UNIT_HOMOGENEOUS,&lights));
    REQUIRE(lights.lights[1].status==RSX_AUTHORED_ACTIVE && lights.lights[1].view[2]>0);
    REQUIRE(rsx_vulkan_backend_replay_outputs(vs,&layout,vertices,h[4],receiver_vc,8224,replay,h[4])==0);
    for(u32 i=0;i<h[4];i++)REQUIRE(replay[i].pos[3]>0);
    memcpy(secondary_vc,receiver_vc,8224);
    /* A rigid view-Z translation keeps light-space recovery orthogonal. All
     * secondary vertices must lie behind the eye, with no raster contribution. */
    float behind=hi[2]+shift[2]+fmaxf(camera.near_z,radius)+1;
    secondary_vc[2][3]-=behind*(2*f.projection[10]-1);
    secondary_vc[3][3]-=behind;
    REQUIRE(rsx_authored_lights_recover(vh,ph,(const float(*)[4])secondary_vc,8224,
        fp,256,f.projection,0,1,RSX_AUTHORED_POSITION_UNIT_HOMOGENEOUS,&lights));
    REQUIRE(rsx_vulkan_backend_replay_outputs(vs,&layout,vertices,h[4],secondary_vc,8224,replay,h[4])==0);
    for(u32 i=0;i<h[4];i++)REQUIRE(replay[i].pos[3]<0);
    float color_max=0;for(u32 i=0;i<h[4];i++)for(u32 k=0;k<3;k++)color_max=fmaxf(color_max,replay[i].col0[k]);
    printf("bounce source maximum vertex color=%g\n",color_max);free(replay);
    u32 receiver_tex[16]={0},secondary_tex[16]={0};rsx_be_sampler_desc samplers[16]={0};
    /* Keep 2*vertexColor*baseTexture within the qualified [0,1] proxy range. */
    const u8 receiver_base[4]={64,64,64,255},red[4]={112,8,8,255};
    const u8 blue[4]={8,8,112,255},lut[4]={191,191,191,255},overlay[4]={255,255,255,255};
    for(u32 u=0;u<3;u++){
        receiver_tex[u]=vk_eng_texture_create(NULL,RSX_BE_FMT_R8G8B8A8,1,1,1,1,0xAAE4,0x85);
        secondary_tex[u]=vk_eng_texture_create(NULL,RSX_BE_FMT_R8G8B8A8,1,1,1,1,0xAAE4,0x85);
        REQUIRE(receiver_tex[u]&&secondary_tex[u]);
        const u8 *r=u==0?receiver_base:u==1?lut:overlay;
        const u8 *s=u==0?red:u==1?lut:overlay;
        vk_eng_texture_upload(NULL,receiver_tex[u],0,0,1,1,r,4,1);
        vk_eng_texture_upload(NULL,secondary_tex[u],0,0,1,1,s,4,1);
        samplers[u].wrap_s=samplers[u].wrap_t=samplers[u].wrap_r=3;
    }
    unsigned char *base_raster=malloc(f.bytes),*blue_raster=malloc(f.bytes);
    unsigned char *base_direct=malloc(f.bytes),*blue_direct=malloc(f.bytes);
    unsigned char *red_bounce=malloc(f.bytes),*blue_bounce=malloc(f.bytes);
    REQUIRE(base_raster&&blue_raster&&base_direct&&blue_direct&&red_bounce&&blue_bounce);
    u32 n=0;
#define PASS(D,R,O,N) REQUIRE(bounce_pass(&f,vertices,h[4],h[5],indices,h[6], \
    (const float(*)[4])receiver_vc,(const float(*)[4])secondary_vc, \
    (const float(*)[4])fp,receiver_tex,secondary_tex,samplers,(D),(R),(O),(N)))
    PASS(1,base_raster,base_direct,&n);REQUIRE(n==0);
    PASS(0,base_raster,red_bounce,&n);REQUIRE(n>0);
    u32 red_hits=n;
    vk_eng_texture_upload(NULL,secondary_tex[0],0,0,1,1,blue,4,1);
    PASS(1,blue_raster,blue_direct,&n);REQUIRE(n==0);
    REQUIRE(!memcmp(base_raster,blue_raster,f.bytes));
    REQUIRE(!memcmp(base_direct,blue_direct,f.bytes));
    PASS(0,blue_raster,blue_bounce,&n);REQUIRE(n>0);
    REQUIRE(!memcmp(base_raster,blue_raster,f.bytes));
    u32 colored=0,red_advantage=0,blue_advantage=0;
    for(size_t i=0;i<f.bytes;i+=4){
        if(!memcmp(base_raster+i,(u8[]){255,0,255,255},4))continue;
        REQUIRE(base_raster[i+3]==red_bounce[i+3] && base_raster[i+3]==blue_bounce[i+3]);
        if(memcmp(red_bounce+i,blue_bounce+i,3)){
            colored++;
            if(red_bounce[i]>blue_bounce[i])red_advantage++;
            if(blue_bounce[i+2]>red_bounce[i+2])blue_advantage++;
        }
    }
    REQUIRE(colored>0 && red_advantage>0 && blue_advantage>0);
    printf("contract0 bounce samples=%u red_hits=%u blue_hits=%u hue_pixels=%u red_adv=%u blue_adv=%u\n",
        samples,red_hits,n,colored,red_advantage,blue_advantage);
#undef PASS
    s_ray_ao.test_disable_authored_sources=0;s_ray_ao.test_disable_authored_bounce=0;
    free(base_raster);free(blue_raster);free(base_direct);free(blue_direct);
    free(red_bounce);free(blue_bounce);
    for(u32 u=0;u<3;u++){vk_eng_release(NULL,receiver_tex[u]);vk_eng_release(NULL,secondary_tex[u]);}
    live_release(&f);free(secondary_vc);free(g);free(vs);free(ps);return 1;
}

int main(int argc,char **argv)
{
    if(argc<4||argc>5){fprintf(stderr,"usage: test_authored_bounce_live geometry VS PS [--single|--msaa2]\n");return 2;}
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
    if(rsx_vulkan_backend_init(LIVE_W,LIVE_H,"contract0 authored bounce fixture"))return 77;
    int result=1;if(!s_vk.rt_enabled){result=77;goto done;}
    for(int samples=first;samples<=last;samples++)
        if(!bounce_suite(argv[1],argv[2],argv[3],(u32)samples))goto done;
    printf("PASS %u contract0 authored bounce checks\n",checks);result=0;
done:rsx_vulkan_backend_shutdown();return result;
}
