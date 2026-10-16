#define PS3RECOMP_RSX_CACAO
#include "../libs/video/rsx_draw_engine.c"
static unsigned checks;
static unsigned snapshots;
static u32 snapshot(void* user,u32 handle,u32 w,u32 h)
{
    (void)user;(void)handle;(void)w;(void)h;return 100u+(++snapshots);
}
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main(void)
{
    g.vp_cb=(u8*)calloc(1,ENG_VP_CB_BYTES);CHECK(g.vp_cb);
    const float m[4][4]={{1,0,0,0},{0,1,0,0},{0,0,1.02f,-2.02f},{0,0,1,0}};
    const float xf[8]={1,1,.5f,0,0,0,.5f,0};
    memcpy(g.vp_cb+256*16,m,sizeof(m));
    eng_pipeline p={0};p.position_known=1;
    for(unsigned i=0;i<4;++i)p.position_rows[i]=(u16)(256+i);
    rsx_dsp_surface sf={0};sf.depth_format=2;sf.zeta_pitch=256;sf.clip_w=64;sf.clip_h=64;
    eng_zdepth* z=&g.zdepths[0];z->w=z->h=64;z->raster_w=z->raster_h=64;z->samples=1;
    rsx_draw_backend backend={0};backend.depth_snapshot=snapshot;g.be=&backend;
    z->handle=9;z->had_write=1;
    CHECK(eng_zdepth_snapshot(0)==101);CHECK(eng_zdepth_snapshot(0)==101 && snapshots==1);
    g.frames=7;
    CHECK(!rsx_ao_depth_usable(&z->ao,g.frames,z->depth_serial));
    rsx_ao_depth_clear(&z->ao,g.frames,1,z->depth_serial);
    CHECK(!rsx_ao_depth_usable(&z->ao,g.frames,z->depth_serial)); /* clear alone is not geometry */
    z->snapshot_valid=1;eng_zdepth_wrote(0,&sf);
    CHECK(!z->snapshot_valid && z->depth_serial==1);
    CHECK(eng_zdepth_snapshot(0)==102 && snapshots==2);
    CHECK(eng_zdepth_snapshot(0)==102 && snapshots==2);
    eng_ao_depth_draw(0,&p,xf,64,64,0,1);
    CHECK(rsx_ao_depth_usable(&z->ao,g.frames,z->depth_serial));
    CHECK(z->ao.accepted==1 && z->ao.width==64);
    CHECK(!rsx_ao_depth_usable(&z->ao,g.frames+1,z->depth_serial));
    CHECK(!rsx_ao_depth_usable(&z->ao,g.frames,z->depth_serial+1));
    eng_zdepth_wrote(0,&sf);eng_ao_depth_draw(0,&p,xf,64,64,0,1);
    CHECK(rsx_ao_depth_usable(&z->ao,g.frames,z->depth_serial));CHECK(z->ao.accepted==2);
    eng_zdepth_wrote(0,&sf);eng_ao_depth_draw(0,NULL,xf,64,64,0,1);
    CHECK(!rsx_ao_depth_usable(&z->ao,g.frames,z->depth_serial));CHECK(z->ao.unknown==1);
    eng_zdepth_wrote(0,&sf);eng_ao_depth_draw(0,&p,xf,64,64,0,1);
    CHECK(!rsx_ao_depth_usable(&z->ao,g.frames,z->depth_serial)); /* known draw cannot erase unknown */
    rsx_ao_depth_clear(&z->ao,g.frames,1,z->depth_serial);
    eng_zdepth_wrote(0,&sf);eng_ao_depth_draw(0,&p,xf,64,64,0,1);
    ((float*)g.vp_cb)[256*4]=2;((float*)g.vp_cb)[257*4+1]=2;
    eng_zdepth_wrote(0,&sf);eng_ao_depth_draw(0,&p,xf,64,64,0,1);
    CHECK(!rsx_ao_depth_usable(&z->ao,g.frames,z->depth_serial));CHECK(z->ao.conflicts==1);
    memcpy(g.vp_cb+256*16,m,sizeof(m));
    rsx_ao_depth_clear(&z->ao,g.frames,1,z->depth_serial);
    eng_zdepth_wrote(0,&sf);eng_ao_depth_draw(0,&p,xf,64,64,1,0);
    CHECK(!rsx_ao_depth_usable(&z->ao,g.frames,z->depth_serial));
    rsx_ao_depth_clear(&z->ao,g.frames,.5f,z->depth_serial);
    eng_zdepth_wrote(0,&sf);eng_ao_depth_draw(0,&p,xf,64,64,0,1);
    CHECK(!rsx_ao_depth_usable(&z->ao,g.frames,z->depth_serial));
    rsx_ao_depth_clear(&z->ao,g.frames,1,z->depth_serial);++g.frames;
    eng_zdepth_wrote(0,&sf);eng_ao_depth_draw(0,&p,xf,64,64,0,1);
    CHECK(!rsx_ao_depth_usable(&z->ao,g.frames,z->depth_serial));
    rsx_ao_depth_clear(&z->ao,g.frames,1,z->depth_serial);
    eng_zdepth_wrote(0,&sf);eng_ao_depth_draw(0,&p,xf,64,64,0,1);
    CHECK(rsx_ao_depth_usable(&z->ao,g.frames,z->depth_serial));
    rsx_ao_projection q=z->ao.projection;
    rsx_ao_depth_write(&z->ao,g.frames,++z->depth_serial,32,32,&q);
    CHECK(!rsx_ao_depth_usable(&z->ao,g.frames,z->depth_serial));CHECK(z->ao.conflicts==1);
    /* Different source constant base is supported by the actual hook. */
    memcpy(g.vp_cb,m,sizeof(m));for(unsigned i=0;i<4;++i)p.position_rows[i]=(u16)i;
    rsx_ao_depth_clear(&z->ao,g.frames,1,z->depth_serial);
    eng_zdepth_wrote(0,&sf);eng_ao_depth_draw(0,&p,xf,64,64,0,1);
    CHECK(rsx_ao_depth_usable(&z->ao,g.frames,z->depth_serial));
    /* Restricted host depth viewport cancels the shader normalization. */
    const float clipped[8]={1,1,10,0,0,0,-9,0};
    rsx_ao_depth_clear(&z->ao,g.frames,1,z->depth_serial);
    eng_zdepth_wrote(0,&sf);eng_ao_depth_draw(0,&p,clipped,64,64,.95f,1);
    CHECK(rsx_ao_depth_usable(&z->ao,g.frames,z->depth_serial));
    CHECK(fabs(z->ao.projection.depth_b-q.depth_b)<1e-5);
    /* Same camera through a uniform model scale remains compatible. */
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<3;++c)((float*)g.vp_cb)[r*4+c]*=3;
    eng_zdepth_wrote(0,&sf);eng_ao_depth_draw(0,&p,xf,64,64,0,1);
    CHECK(rsx_ao_depth_usable(&z->ao,g.frames,z->depth_serial));
    CHECK(z->ao.accepted==2 && z->ao.conflicts==0);
    /* Real fallback reads the active program and current constants each draw. */
    p.position_known=0;
    for(unsigned c=0;c<4;++c) {
        const u32 source=0x1b02,constant=0x1b03;
        g.rsx.vp[c*4]=(2u<<15)|(1u<<30);
        g.rsx.vp[c*4+1]=(7u<<22)|(c<<12)|(source>>9);
        g.rsx.vp[c*4+2]=((source&511u)<<23)|(constant<<6);
        g.rsx.vp[c*4+3]=(31u<<2)|(63u<<7)|(1u<<(16-c));
    }
    const u32 source=0x1b09; /* r2.xyzw */
    g.rsx.vp[16]=(63u<<15)|(1u<<30);
    g.rsx.vp[17]=(1u<<22)|(source>>9);
    g.rsx.vp[18]=(source&511u)<<23;
    g.rsx.vp[19]=(63u<<7)|(7u<<13); /* output yzw */
    g.rsx.vp[20]=(63u<<15)|(1u<<30);
    g.rsx.vp[21]=(4u<<22)|(400u<<12)|(1u<<8); /* MAD(c400.x,v1,r2.x) */
    g.rsx.vp[22]=(3u<<23)|(0x1b02u<<6);
    g.rsx.vp[23]=(9u<<21)|(63u<<7)|(1u<<16)|1u;
    rsx_ao_depth_clear(&z->ao,g.frames,1,z->depth_serial);
    eng_zdepth_wrote(0,&sf);eng_ao_depth_draw(0,&p,xf,64,64,0,1);
    CHECK(rsx_ao_depth_usable(&z->ao,g.frames,z->depth_serial));
    ((float*)g.vp_cb)[400*4]=1;
    eng_zdepth_wrote(0,&sf);eng_ao_depth_draw(0,&p,xf,64,64,0,1);
    CHECK(!rsx_ao_depth_usable(&z->ao,g.frames,z->depth_serial));
    CHECK(z->ao.unknown==1);
    ((float*)g.vp_cb)[400*4]=0;
    rsx_ao_depth_clear(&z->ao,g.frames,1,z->depth_serial);
    eng_zdepth_wrote(0,&sf);eng_ao_depth_draw(0,&p,xf,64,64,0,1);
    CHECK(rsx_ao_depth_usable(&z->ao,g.frames,z->depth_serial));
    free(g.vp_cb);g.vp_cb=NULL;
    printf("Actual engine AO depth lifetime: %u checks passed\n",checks);return 0;
}
