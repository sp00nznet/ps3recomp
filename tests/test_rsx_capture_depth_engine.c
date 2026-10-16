#define _POSIX_C_SOURCE 200809L
#include "../libs/video/rsx_draw_engine.c"

/* Exercise the actual pass writer with a deterministic backend readback.
 * Link with section garbage collection; no GPU or guest memory is used. */
static unsigned checks, reads, submits;
#define CHECK(v) do { ++checks; if (!(v)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#v); return 1; } } while(0)
static void submit(void* user,u32 reason)
{ (void)user; if(reason==RSX_BE_FLUSH_GUEST_REFERENCE)++submits; }
static int read_depth(void* user,u32 depth,u32 x,u32 y,u32 w,u32 h,float* out,u32 pitch)
{
    (void)user;
    if(depth!=1 || x || y || w!=2 || h!=1 || pitch!=8)return -1;
    out[0]=0.25f;out[1]=0.75f;++reads;return 0;
}
int main(int argc,char** argv)
{
    CHECK(argc==2);
    rsx_draw_backend backend={0};backend.submit_and_wait=submit;backend.depth_readback=read_depth;
    g.be=&backend;g.zdepths[0].handle=1;g.zdepths[0].w=2;g.zdepths[0].h=1;
    s_pd.dir=argv[1];
    char pgm[1024],raw[1024],index_path[1024];
    snprintf(pgm,sizeof(pgm),"%s/depth.pgm",argv[1]);
    snprintf(raw,sizeof(raw),"%s/pass000_z.f32le",argv[1]);
    snprintf(index_path,sizeof(index_path),"%s/index.txt",argv[1]);
    s_pd.index=fopen(index_path,"w+");CHECK(s_pd.index);
    float lo,hi;
    CHECK(unsetenv("RSX_ENGINE_PASS_DUMP_DEPTH_RAW")==0);
    pd_write_depth(0,pgm,&lo,&hi);
    CHECK(reads==1 && submits==1 && lo==0.25f && hi==0.75f);
    CHECK(s_pd.depth_bytes==0 && !fopen(raw,"rb"));
    CHECK(setenv("RSX_ENGINE_PASS_DUMP_DEPTH_RAW","1",1)==0);
    pd_write_depth(0,pgm,&lo,&hi);
    CHECK(reads==2 && submits==2 && s_pd.depth_bytes==8);
    FILE* f=fopen(raw,"rb");CHECK(f);
    const unsigned char expected[]={0,0,128,62,0,0,64,63};
    for(unsigned i=0;i<sizeof(expected);++i)CHECK(fgetc(f)==expected[i]);
    CHECK(fgetc(f)==EOF);CHECK(fclose(f)==0);
    s_pd.pass=1;s_pd.depth_bytes=256u*1024u*1024u-4u;
    pd_write_depth(0,pgm,&lo,&hi);
    snprintf(raw,sizeof(raw),"%s/pass001_z.f32le",argv[1]);
    CHECK(!fopen(raw,"rb"));CHECK(s_pd.depth_bytes==256u*1024u*1024u-4u);
    CHECK(reads==3 && submits==3 && lo==0.25f && hi==0.75f);
    CHECK(fflush(s_pd.index)==0);rewind(s_pd.index);
    char metadata[1024]={0};CHECK(fread(metadata,1,sizeof(metadata)-1,s_pd.index)>0);
    CHECK(strstr(metadata,"rawdepth written pass000_z.f32le f32le image-y0-first 2x1"));
    CHECK(strstr(metadata,"rawdepth skipped-budget"));CHECK(fclose(s_pd.index)==0);
    printf("actual engine raw depth: %u checks passed\n",checks);return 0;
}
