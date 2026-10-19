/* Renderer-level authored cube-reflection fixture. Args: pool91 FGM1v2,
 * pool91 VS/PS, contract0 FGM1v2, contract0 VS/PS, [--single|--msaa2].
 * The pool91 capture is the visible receiver. A source-qualified contract0
 * plane behind the eye is a ray-only red/blue color target. The comparison
 * proves that a hit, rather than the game's cube RGB, drives the receiver. */
#define AUTHORED_RAY_LIVE_NO_MAIN
#include "test_authored_ray_live.c"
#undef AUTHORED_RAY_LIVE_NO_MAIN
#include "../libs/video/rsx_authored_lights.h"

static int reflection_save(const char *name,const unsigned char *rgba,u32 samples)
{
    const char *directory=getenv("FOLKLORE_REFLECTION_FIXTURE_OUTPUT");
    if(!directory)return 1;
    char path[2048];int n=snprintf(path,sizeof path,"%s/%s-%u.ppm",directory,name,samples);
    if(n<=0||(size_t)n>=sizeof path)return 0;
    FILE *file=fopen(path,"wb");if(!file)return 0;
    int ok=fprintf(file,"P6\n%u %u\n255\n",LIVE_W,LIVE_H)>0;
    for(u32 y=0;y<LIVE_H&&ok;y++)for(u32 x=0;x<LIVE_W&&ok;x++){
        unsigned char rgb[3];for(u32 c=0;c<3;c++){
            unsigned value=0;for(u32 s=0;s<samples;s++)
                value+=rgba[((size_t)y*LIVE_W*samples+x*samples+s)*4+c];
            rgb[c]=(unsigned char)((value+samples/2)/samples);
        }
        ok=fwrite(rgb,1,3,file)==3;
    }
    if(fclose(file))ok=0;return ok;
}

static void reflection_draw(live_fixture *f,const void *receiver,u32 receiver_count,
    u32 receiver_stride,const u32 *receiver_indices,u32 receiver_index_count,
    const float receiver_vc[514][4],const float receiver_fp[44][4],
    const u32 receiver_tex[16],const float target[6][16],
    u32 target_pipe,const float target_vc[514][4],const float target_fp[16][4],
    const u32 target_tex[16],const rsx_be_sampler_desc samplers[16])
{
    const float clear[4]={1,0,1,1};
    vk_eng_clear_color(NULL,f->color,clear);
    vk_eng_clear_depth_stencil(NULL,f->depth,RSX_BE_CLEAR_DEPTH,1,0);
    vk_eng_bind_targets(NULL,&f->color,1,f->depth);
    vk_eng_set_viewport(NULL,0,0,LIVE_W,LIVE_H,0,1);
    vk_eng_set_scissor(NULL,0,0,f->width,LIVE_H);
    auth_actor_draw(f->pipe,receiver,receiver_count,receiver_stride,
        receiver_indices,receiver_index_count,receiver_vc,receiver_fp,
        receiver_tex,samplers);
    vk_eng_bind_pipeline(NULL,target_pipe);
    vk_eng_bind_vs_constants(NULL,target_vc,8224);
    vk_eng_bind_ps_constants(NULL,target_fp,256);
    vk_eng_bind_textures(NULL,target_tex,samplers,7);
    vk_eng_draw(NULL,RSX_TOPOLOGY_TRIANGLES,target,6,64,NULL,0);
    vk_flush_pending(0);
}

static int reflection_pass(live_fixture *f,const void *receiver,u32 receiver_count,
    u32 receiver_stride,const u32 *receiver_indices,u32 receiver_index_count,
    const float receiver_vc[514][4],const float receiver_fp[44][4],
    const u32 receiver_tex[16],const float target[6][16],
    u32 target_pipe,const float target_vc[514][4],const float target_fp[16][4],
    const u32 target_tex[16],const rsx_be_sampler_desc samplers[16],
    int disable_reflection,unsigned char *raster,unsigned char *output,
    u32 *valid_directions,u32 *hits)
{
    s_ray_ao.test_disable_authored_sources=0;
    s_ray_ao.test_disable_authored_bounce=1;
    s_ray_ao.test_disable_authored_reflections=disable_reflection;
    reflection_draw(f,receiver,receiver_count,receiver_stride,receiver_indices,
        receiver_index_count,receiver_vc,receiver_fp,receiver_tex,target,
        target_pipe,target_vc,target_fp,target_tex,samplers);
    REQUIRE(live_read(f,f->color,f->before));
    memcpy(raster,f->before,f->bytes);
    float *depth=malloc((size_t)f->width*LIVE_H*sizeof(float));REQUIRE(depth);
    REQUIRE(vk_eng_depth_readback(NULL,f->depth,0,0,f->width,LIVE_H,
        depth,f->width*4)==0);
    u32 covered=0;for(size_t i=0;i<(size_t)f->width*LIVE_H;i++)covered+=depth[i]<1;
    free(depth);REQUIRE(covered>0);
    rsx_scene_epoch *epoch=vk_rt_epoch(f->depth);
    REQUIRE(epoch && epoch->draw_count==2 && epoch->reason==RSX_SCENE_EPOCH_OK);
    REQUIRE(live_consume(f)!=0 && s_ray_ao.gpu_stats[24]>0);
    *valid_directions=s_ray_ao.gpu_stats[29];*hits=s_ray_ao.gpu_stats[30];
    REQUIRE(live_read(f,s_ray_ao.color,output));
    REQUIRE(live_original_preserved(f));
    return 1;
}

static int reflection_suite(const char *receiver_file,const char *receiver_vs_file,
    const char *receiver_ps_file,const char *target_file,const char *target_vs_file,
    const char *target_ps_file,u32 samples)
{
    size_t rn=0,rvn=0,rpn=0,tn=0,tvn=0,tpn=0;
    unsigned char *rg=auth_file(receiver_file,&rn),*tg=auth_file(target_file,&tn);
    char *rvs=auth_file(receiver_vs_file,&rvn),*rps=auth_file(receiver_ps_file,&rpn);
    char *tvs=auth_file(target_vs_file,&tvn),*tps=auth_file(target_ps_file,&tpn);
    REQUIRE(rg&&tg&&rvs&&rps&&tvs&&tps&&rn>=128&&tn>=128);
    u32 rh[32],th[32];memcpy(rh,rg,128);memcpy(th,tg,128);
    REQUIRE(rh[0]==0x314d4746u&&rh[1]==2&&rh[29]==91&&rh[30]==8224&&rh[31]==704);
    REQUIRE(th[0]==0x314d4746u&&th[1]==2&&th[29]==71&&th[30]==8224&&th[31]==256);
    size_t rvb=(size_t)rh[4]*rh[5],rib=(size_t)rh[6]*4;
    size_t tvb=(size_t)th[4]*th[5],tib=(size_t)th[6]*4;
    REQUIRE(rn==128+rvb+rib+8224+704&&tn==128+tvb+tib+8224+256);
    const void *receiver=rg+128;const u32 *receiver_indices=(const u32*)(rg+128+rvb);
    float (*rvc)[4]=(float(*)[4])(rg+128+rvb+rib);
    float (*rfp)[4]=(float(*)[4])((unsigned char*)rvc+8224);
    float (*tvc)[4]=(float(*)[4])(tg+128+tvb+tib);
    float (*tfp)[4]=(float(*)[4])((unsigned char*)tvc+8224);
    char vh[65],ph[65];REQUIRE(vk_rt_source_sha256(rvs,vh)&&
        vk_rt_source_sha256(rps,ph)&&rsx_authored__pair(vh,ph)==1);
    REQUIRE(vk_rt_source_sha256(tvs,vh)&&vk_rt_source_sha256(tps,ph)&&
        rsx_authored__pair(vh,ph)==0);
    live_fixture f={0};REQUIRE(live_create(&f,samples));
    rsx_vertex_layout_plan rlayout={0},tlayout={0};
    memset(rlayout.slot_by_attr,-1,sizeof rlayout.slot_by_attr);
    memset(tlayout.slot_by_attr,-1,sizeof tlayout.slot_by_attr);
    rlayout.count=(u8)rh[7];rlayout.stride=rh[5];
    tlayout.count=(u8)th[7];tlayout.stride=64;
    REQUIRE(th[7]==4&&th[5]==64&&th[8]==0&&th[9]==2&&th[10]==3&&th[11]==8);
    for(u32 a=0;a<rh[7];a++){REQUIRE(rh[8+a]<16);
        rlayout.attrs[a]=(u8)rh[8+a];rlayout.slot_by_attr[rh[8+a]]=(s8)a;
        rlayout.mask|=1u<<rh[8+a];}
    const u8 target_attrs[4]={0,2,3,8};
    for(u32 a=0;a<4;a++){tlayout.attrs[a]=target_attrs[a];
        tlayout.slot_by_attr[target_attrs[a]]=(s8)a;
        tlayout.mask|=1u<<target_attrs[a];}
    rsx_be_render_state state={0};state.color_mask=0x01010101;
    state.antialias_control=0xffff0001;state.depth_test=state.depth_write=1;
    state.depth_func=0x203;
    vk_eng_pipeline_release(NULL,f.pipe);
    f.pipe=vk_eng_pipeline_create(NULL,rvs,rps,&state,&rlayout,rh[5],
        RSX_BE_FMT_R8G8B8A8,1);REQUIRE(f.pipe);
    state.alpha_test_enable=1;state.alpha_func=0x204;
    state.alpha_ref_raw=0;state.alpha_ref_format=RSX_BE_FMT_R8G8B8A8;
    u32 target_pipe=vk_eng_pipeline_create(NULL,tvs,tps,&state,&tlayout,64,
        RSX_BE_FMT_R8G8B8A8,1);REQUIRE(target_pipe);
    float effective[8];rsx_ao_projection camera;
    REQUIRE(rsx_ao_viewport_transform((const float*)(rvc+512),0,1,effective));
    REQUIRE(rsx_ao_projection_extract((const float(*)[4])rvc,effective,effective+4,
        (float)LIVE_W/LIVE_H,&camera));
    memcpy(f.projection,camera.matrix,sizeof f.projection);
    rsx_scene_vertex_output *replay=calloc(rh[4],sizeof(*replay));REQUIRE(replay);
    REQUIRE(rsx_vulkan_backend_replay_outputs(rvs,&rlayout,receiver,rh[4],
        rvc,8224,replay,rh[4])==0);
    float lo[3]={INFINITY,INFINITY,INFINITY},hi[3]={-INFINITY,-INFINITY,-INFINITY};
    for(u32 i=0;i<rh[4];i++)for(u32 k=0;k<3;k++){
        float v=replay[i].pos[k==2?3:k]/(k==2?1:f.projection[k==0?0:5]);
        REQUIRE(isfinite(v));lo[k]=fminf(lo[k],v);hi[k]=fmaxf(hi[k],v);
    }
    float radius=fmaxf(hi[2]-lo[2],fmaxf((hi[0]-lo[0])*f.projection[0],
        (hi[1]-lo[1])*f.projection[5]));
    float shift[3]={-(lo[0]+hi[0])*.5f,-(lo[1]+hi[1])*.5f,
        fmaxf(4*camera.near_z,2*radius)-(lo[2]+hi[2])*.5f};
    rvc[0][3]+=shift[0]*f.projection[0];rvc[1][3]+=shift[1]*f.projection[5];
    rvc[2][3]+=shift[2]*(2*f.projection[10]-1);rvc[3][3]+=shift[2];
    REQUIRE(rsx_vulkan_backend_replay_outputs(rvs,&rlayout,receiver,rh[4],
        rvc,8224,replay,rh[4])==0);
    for(u32 i=0;i<rh[4];i++)REQUIRE(replay[i].pos[3]>0);
    free(replay);
    for(u32 k=0;k<3;k++){rfp[16][k]=rfp[18][k]=rfp[19][k]=0;
        rfp[17][k]=.5f;rfp[21][k]=rfp[24][k]=0;}
    rfp[22][0]=0;
    /* Keep recovered contract0 shading in range and illuminate its +Z face. */
    tvc[467][2]=0;tvc[467][3]=1;
    memset(tvc,0,4*4*sizeof(float));
    tvc[0][0]=f.projection[0];tvc[1][1]=f.projection[5];
    tvc[2][2]=f.projection[10];tvc[2][3]=f.projection[14];tvc[3][2]=1;
    tvc[512][0]=tvc[512][1]=tvc[512][2]=tvc[512][3]=1;
    memset(tvc[513],0,16);
    for(u32 row=256;row<=262;row++)memset(tvc[row],0,16);
    tvc[256][0]=tvc[257][1]=tvc[258][2]=1;
    tvc[260][0]=tvc[261][1]=tvc[262][2]=1;
    tvc[12][0]=tvc[13][0]=0;tvc[12][1]=tvc[13][1]=0;
    tvc[12][2]=tvc[13][2]=300;tvc[12][3]=tvc[13][3]=0;
    for(u32 k=0;k<3;k++){tfp[0][k]=tfp[1][k]=tfp[6][k]=tfp[8][k]=tfp[10][k]=0;
        tfp[7][k]=.35f;tfp[12][k]=tfp[13][k]=0;}
    tfp[9][0]=0;tfp[14][0]=.5f;tfp[15][0]=0;
    float target[6][16]={0};const float xy[6][2]={{-1000,-1000},{1000,-1000},
        {-1000,1000},{1000,-1000},{1000,1000},{-1000,1000}};
    for(u32 i=0;i<6;i++){
        float *v=target[i];v[0]=xy[i][0];v[1]=xy[i][1];v[2]=-1;v[3]=1;
        v[4]=v[5]=0;v[6]=1;v[7]=0;
        v[8]=v[9]=v[10]=v[11]=.5f;
        v[12]=v[13]=.5f;v[14]=0;v[15]=1;
    }
    rsx_scene_vertex_output target_replay[6]={0};
    REQUIRE(rsx_vulkan_backend_replay_outputs(tvs,&tlayout,target,6,tvc,8224,
        target_replay,6)==0);
    for(u32 i=0;i<6;i++){REQUIRE(target_replay[i].pos[3]<0);
        REQUIRE(isfinite(target_replay[i].t2[0])&&isfinite(target_replay[i].t3[0])&&
            isfinite(target_replay[i].t7[0]));}
    u32 rtex[16]={0},ttex[16]={0};rsx_be_sampler_desc samplers[16]={0};
    const u8 rtexels[7][4]={{128,128,255,255},{102,153,204,255},
        {128,64,32,255},{64,128,192,255},{192,192,192,255},
        {255,192,128,255},{32,64,128,255}};
    const u8 gray[4]={64,64,64,255},red[4]={112,8,8,255};
    const u8 blue[4]={8,8,112,255},lut[4]={191,191,191,255};
    const u8 white[4]={255,255,255,255};
    for(u32 u=0;u<7;u++){
        rtex[u]=vk_eng_texture_create(NULL,RSX_BE_FMT_R8G8B8A8,1,1,1,
            u==3?6:1,0xAAE4,0x85);REQUIRE(rtex[u]);
        for(u32 face=0;face<(u==3?6u:1u);face++)
            vk_eng_texture_upload(NULL,rtex[u],face,0,1,1,rtexels[u],4,1);
        samplers[u].wrap_s=samplers[u].wrap_t=samplers[u].wrap_r=3;
        if(u<3){ttex[u]=vk_eng_texture_create(NULL,RSX_BE_FMT_R8G8B8A8,
            1,1,1,1,0xAAE4,0x85);REQUIRE(ttex[u]);
            const u8 *texel=u==0?gray:u==1?lut:white;
            vk_eng_texture_upload(NULL,ttex[u],0,0,1,1,texel,4,1);}
    }
    unsigned char *raster_red=malloc(f.bytes),*raster_blue=malloc(f.bytes);
    unsigned char *off_red=malloc(f.bytes),*off_blue=malloc(f.bytes);
    unsigned char *on_red=malloc(f.bytes),*on_blue=malloc(f.bytes);
    REQUIRE(raster_red&&raster_blue&&off_red&&off_blue&&on_red&&on_blue);
    u32 dirs=0,hits=0,red_hits=0;
#define PASS(D,R,O) REQUIRE(reflection_pass(&f,receiver,rh[4],rh[5], \
    receiver_indices,rh[6],(const float(*)[4])rvc,(const float(*)[4])rfp, \
    rtex,(const float(*)[16])target,target_pipe,(const float(*)[4])tvc, \
    (const float(*)[4])tfp,ttex,samplers,(D),(R),(O),&dirs,&hits))
    vk_eng_texture_upload(NULL,ttex[0],0,0,1,1,red,4,1);
    PASS(1,raster_red,off_red);REQUIRE(dirs>0&&hits==0);
    PASS(0,raster_red,on_red);REQUIRE(dirs>0&&hits>0);red_hits=hits;
    vk_eng_texture_upload(NULL,ttex[0],0,0,1,1,blue,4,1);
    PASS(1,raster_blue,off_blue);REQUIRE(dirs>0&&hits==0);
    REQUIRE(!memcmp(raster_red,raster_blue,f.bytes));
    REQUIRE(!memcmp(off_red,off_blue,f.bytes));
    PASS(0,raster_blue,on_blue);REQUIRE(dirs>0&&hits>0);
    REQUIRE(!memcmp(raster_red,raster_blue,f.bytes));
    u32 changed=0,red_adv=0,blue_adv=0;
    for(size_t i=0;i<f.bytes;i+=4){
        if(!memcmp(raster_red+i,(u8[]){255,0,255,255},4))continue;
        REQUIRE(raster_red[i+3]==on_red[i+3]&&raster_red[i+3]==on_blue[i+3]);
        if(memcmp(on_red+i,on_blue+i,3)){
            changed++;red_adv+=on_red[i]>on_blue[i];
            blue_adv+=on_blue[i+2]>on_red[i+2];}
    }
    REQUIRE(changed>0&&red_adv>0&&blue_adv>0);
    REQUIRE(reflection_save("raster",raster_red,samples));
    REQUIRE(reflection_save("reflections-off",off_red,samples));
    REQUIRE(reflection_save("reflected-red",on_red,samples));
    REQUIRE(reflection_save("reflected-blue",on_blue,samples));
    printf("authored reflection samples=%u directions=%u red_hits=%u blue_hits=%u hue_pixels=%u red_adv=%u blue_adv=%u\n",
        samples,dirs,red_hits,hits,changed,red_adv,blue_adv);
#undef PASS
    s_ray_ao.test_disable_authored_reflections=0;
    s_ray_ao.test_disable_authored_bounce=0;
    free(raster_red);free(raster_blue);free(off_red);free(off_blue);
    free(on_red);free(on_blue);
    for(u32 u=0;u<7;u++)if(rtex[u])vk_eng_release(NULL,rtex[u]);
    for(u32 u=0;u<3;u++)if(ttex[u])vk_eng_release(NULL,ttex[u]);
    vk_eng_pipeline_release(NULL,target_pipe);live_release(&f);
    free(rg);free(tg);free(rvs);free(rps);free(tvs);free(tps);return 1;
}

int main(int argc,char **argv)
{
    if(argc<7||argc>8){fprintf(stderr,"usage: test_authored_reflection_live pool91.bin pool91.vs.hlsl pool91.ps.hlsl contract0.bin contract0.vs.hlsl contract0.ps.hlsl [--single|--msaa2]\n");return 2;}
    int first=1,last=2;
    if(argc==8){if(!strcmp(argv[7],"--single"))last=1;
        else if(!strcmp(argv[7],"--msaa2"))first=2;else return 2;}
    live_env("PS3RECOMP_VK_GUEST_PROGRAMS","1");
    live_env("PS3RECOMP_RSX_AO","rayquery");
    live_env("PS3RECOMP_RSX_AUTHORED_LIGHTS","1");
    live_env("PS3RECOMP_RENDER_SCALE","1");
    live_env("PS3RECOMP_VK_WINDOW","0");
    live_env("PS3RECOMP_ANTIALIASING","off");
    live_env("PS3RECOMP_UPSCALER","off");SDL_SetMainReady();
    if(rsx_vulkan_backend_init(LIVE_W,LIVE_H,"authored reflection fixture"))return 77;
    int result=1;if(!s_vk.rt_enabled){result=77;goto done;}
    for(int samples=first;samples<=last;samples++)
        if(!reflection_suite(argv[1],argv[2],argv[3],argv[4],argv[5],argv[6],
            (u32)samples))goto done;
    printf("PASS %u authored reflection checks\n",checks);result=0;
done:rsx_vulkan_backend_shutdown();return result;
}
