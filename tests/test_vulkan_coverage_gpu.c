/* Alpha-to-coverage, alpha-to-one and sample mask on actual Vulkan
 * multisample attachments, verified through expanded per-sample readbacks.
 * Synthetic geometry only; no game/firmware bytes. */
#define SDL_MAIN_HANDLED
#include "../libs/video/rsx_vulkan_backend.c"
#include <math.h>
#include <stdarg.h>
uint8_t* vm_base;

/* Raw NV4097 0x1D7C (antialias control) fields. */
#define AA_MSAA 0x1u
#define AA_A2C 0x10u
#define AA_A2O 0x100u
#define AA_MASK(m) ((u32)(m)<<16)
#define AA_ORDINARY (AA_MASK(0xFFFFu)|AA_MSAA)

enum { ALPHA_ZERO, ALPHA_QUARTER, ALPHA_HALF, ALPHA_ONE, ALPHA_COUNT };
enum { WRITE_ALL, WRITE_NONE, WRITE_PARTIAL, WRITE_ONE };

typedef struct {
    char name[96];
    u32 control;
    unsigned alpha;   /* fragment shader index */
    int expect;
    unsigned sample;  /* WRITE_ONE only */
    int alpha8;       /* expected stored alpha of written samples */
} aa_case;

static unsigned checks, failures, case_failures;
static unsigned g_samples;
static const char* g_case="setup";

static void expect(int ok,int line,const char* fmt,...)
{
    ++checks;
    if(ok) return;
    ++failures;
    if(++case_failures>12) {
        if(case_failures==13) fprintf(stderr,"  ... further failures in this case suppressed\n");
        return;
    }
    fprintf(stderr,"FAIL line %d [%u samples, %s]: ",line,g_samples,g_case);
    va_list ap;va_start(ap,fmt);vfprintf(stderr,fmt,ap);va_end(ap);
    fputc('\n',stderr);
}
#define EXPECT(x,...) expect(!!(x),__LINE__,__VA_ARGS__)
#define REQUIRE(x,...) do { int ok_=!!(x);expect(ok_,__LINE__,__VA_ARGS__);if(!ok_) goto fatal; } while(0)

static void add_case(aa_case* out,unsigned* n,const char* name,u32 control,
                     unsigned alpha,int kind,unsigned sample,int alpha8)
{
    aa_case* c=&out[(*n)++];
    snprintf(c->name,sizeof c->name,"%s",name);
    c->control=control;c->alpha=alpha;c->expect=kind;c->sample=sample;c->alpha8=alpha8;
}

/* Neighbouring cases deliberately share a shader while differing only in
 * flags or mask, so a pipeline cache ignoring antialias_control fails. */
static unsigned build_cases(aa_case* out,unsigned count)
{
    unsigned n=0;
    add_case(out,&n,"coverage off, alpha 0 writes red alpha 0",AA_ORDINARY,ALPHA_ZERO,WRITE_ALL,0,0);
    add_case(out,&n,"alpha-to-coverage, alpha 0 writes nothing",AA_ORDINARY|AA_A2C,ALPHA_ZERO,WRITE_NONE,0,0);
    add_case(out,&n,"alpha-to-coverage, alpha 1 writes all",AA_ORDINARY|AA_A2C,ALPHA_ONE,WRITE_ALL,0,255);
    add_case(out,&n,"alpha-to-coverage, alpha 0.5 writes some",AA_ORDINARY|AA_A2C,ALPHA_HALF,WRITE_PARTIAL,0,128);
    add_case(out,&n,"alpha-to-one alone, alpha 0.25 stores 255",AA_ORDINARY|AA_A2O,ALPHA_QUARTER,WRITE_ALL,0,255);
    add_case(out,&n,"flags off again, alpha 0.25 stores 64",AA_ORDINARY,ALPHA_QUARTER,WRITE_ALL,0,64);
    add_case(out,&n,"coverage+one, alpha 0 writes nothing",AA_ORDINARY|AA_A2C|AA_A2O,ALPHA_ZERO,WRITE_NONE,0,255);
    add_case(out,&n,"coverage+one, alpha 0.5 partial alpha 255",AA_ORDINARY|AA_A2C|AA_A2O,ALPHA_HALF,WRITE_PARTIAL,0,255);
    add_case(out,&n,"coverage only again, alpha 0.5 keeps alpha 128",AA_ORDINARY|AA_A2C,ALPHA_HALF,WRITE_PARTIAL,0,128);
    add_case(out,&n,"sample mask 0 preserves clear",AA_MASK(0)|AA_MSAA,ALPHA_ONE,WRITE_NONE,0,255);
    for(unsigned s=0;s<count;++s) {
        char name[96];snprintf(name,sizeof name,"sample mask 0x%X writes only sample %u",1u<<s,s);
        add_case(out,&n,name,AA_MASK(1u<<s)|AA_MSAA,ALPHA_ONE,WRITE_ONE,s,255);
    }
    add_case(out,&n,"sample mask 0xFFFF writes all",AA_ORDINARY,ALPHA_ONE,WRITE_ALL,0,255);
    add_case(out,&n,"sample mask 0 again preserves clear",AA_MASK(0)|AA_MSAA,ALPHA_ONE,WRITE_NONE,0,255);
    return n;
}

static void draw_case(u32 pipeline,u32 color,u32 depth)
{
    static const float vertices[6][4]={{-1,-1,.25f,1},{1,-1,.25f,1},{1,1,.25f,1},
                                      {-1,-1,.25f,1},{1,1,.25f,1},{-1,1,.25f,1}};
    const float blue[4]={0,0,1,1};
    vk_eng_clear_color(NULL,color,blue);
    vk_eng_clear_depth_stencil(NULL,depth,RSX_BE_CLEAR_DEPTH,1.f,0);
    vk_eng_bind_targets(NULL,&color,1,depth);
    vk_eng_bind_pipeline(NULL,pipeline);
    vk_eng_set_viewport(NULL,0,0,8,4,0,1);
    vk_eng_set_scissor(NULL,0,0,8,4);
    vk_eng_draw(NULL,RSX_TOPOLOGY_TRIANGLES,vertices,6,16,NULL,0);
}

/* Returns nonzero when the depth readback itself failed. */
static int verify_case(const aa_case* c,u32 color,u32 depth,unsigned count)
{
    const unsigned sx=count>1?2:1,sy=count>2?2:1,w=8*sx,h=4*sy;
    uint8_t pixels[16*8*4];float depths[16*8];
    memset(pixels,0x55,sizeof pixels);
    for(unsigned i=0;i<16*8;++i) depths[i]=-7.f;
    vk_eng_readback(NULL,color,0,0,w,h,pixels,w*4);
    const int rc=vk_eng_depth_readback(NULL,depth,0,0,w,h,depths,w*4);
    EXPECT(rc==0,"depth readback failed (rc=%d)",rc);
    if(rc!=0) return 1;
    const int tol=(c->alpha8==0||c->alpha8==255)?0:1;
    unsigned written=0,total=0;
    /* Interior pixels only: edges are not needed and avoid raster ambiguity. */
    for(unsigned y=1;y<3;++y) for(unsigned x=1;x<7;++x)
    for(unsigned dy=0;dy<sy;++dy) for(unsigned dx=0;dx<sx;++dx) {
        const unsigned s=dx+dy*2,i=(y*sy+dy)*w+x*sx+dx;
        const uint8_t* p=pixels+i*4;const float d=depths[i];
        const int red=p[0]==255&&p[1]==0&&p[2]==0;
        const int blue=p[0]==0&&p[1]==0&&p[2]==255&&p[3]==255;
        ++total;
        EXPECT(red||blue,"pixel (%u,%u) sample %u is neither red nor clear blue: rgba=%u,%u,%u,%u depth=%f",
               x,y,s,p[0],p[1],p[2],p[3],d);
        if(red) {
            ++written;
            EXPECT(abs((int)p[3]-c->alpha8)<=tol,"pixel (%u,%u) sample %u written alpha %u, expected %d",
                   x,y,s,p[3],c->alpha8);
            EXPECT(fabsf(d-.25f)<1e-5f,"pixel (%u,%u) sample %u written color but depth %f, expected 0.25",x,y,s,d);
        } else if(blue) {
            EXPECT(fabsf(d-1.f)<1e-5f,"pixel (%u,%u) sample %u unwritten color but depth %f, expected clear 1.0",x,y,s,d);
        }
        switch(c->expect) {
        case WRITE_ALL:
            EXPECT(red,"pixel (%u,%u) sample %u should be written red but is rgba=%u,%u,%u,%u",
                   x,y,s,p[0],p[1],p[2],p[3]);
            break;
        case WRITE_NONE:
            EXPECT(blue,"pixel (%u,%u) sample %u should keep clear blue but is rgba=%u,%u,%u,%u",
                   x,y,s,p[0],p[1],p[2],p[3]);
            break;
        case WRITE_ONE:
            if(s==c->sample) EXPECT(red,"pixel (%u,%u) masked-in sample %u not written: rgba=%u,%u,%u,%u",
                                    x,y,s,p[0],p[1],p[2],p[3]);
            else EXPECT(blue,"pixel (%u,%u) masked-out sample %u was modified: rgba=%u,%u,%u,%u",
                        x,y,s,p[0],p[1],p[2],p[3]);
            break;
        default: break;
        }
    }
    if(c->expect==WRITE_PARTIAL)
        EXPECT(written>0&&written<total,"alpha 0.5 coverage wrote %u of %u interior samples; expected some but not all",
               written,total);
    return 0;
}

int main(void)
{
    SDL_SetMainReady();
    int inited=0;
    REQUIRE(rsx_vulkan_backend_init(8,4,"MSAA coverage regression")==0,
            "backend init failed (an alphaToOne-capable device is required)");
    inited=1;
    static const char vs[]="float4 main(float4 p:ATTR0):SV_Position{return p;}";
    static const char* const fs[ALPHA_COUNT]={
        "float4 main():SV_Target0{return float4(1,0,0,0);}",
        "float4 main():SV_Target0{return float4(1,0,0,0.25);}",
        "float4 main():SV_Target0{return float4(1,0,0,0.5);}",
        "float4 main():SV_Target0{return float4(1,0,0,1);}",
    };
    rsx_vertex_layout_plan layout={0};layout.count=1;layout.stride=16;layout.attrs[0]=0;
    const u32 counts[]={2,4};
    for(unsigned round=0;round<2;++round) {
        const unsigned count=counts[round];
        g_samples=count;g_case="target creation";case_failures=0;
        u32 color=vk_eng_color_multisample(NULL,RSX_BE_FMT_R8G8B8A8,8,4,count);
        u32 depth=vk_eng_depth_multisample(NULL,8,4,count);
        REQUIRE(color&&depth,"multisample targets not created (color=%u depth=%u)",color,depth);
        aa_case cases[32];u32 pipelines[32]={0};
        const unsigned ncases=build_cases(cases,count);
        for(unsigned k=0;k<ncases;++k) {
            g_case=cases[k].name;case_failures=0;
            rsx_be_render_state state={0};state.color_mask=0x01010101u;
            state.depth_test=1;state.depth_write=1;state.depth_func=0x0207;
            state.antialias_control=cases[k].control;
            pipelines[k]=vk_eng_pipeline_create(NULL,vs,fs[cases[k].alpha],&state,&layout,16,RSX_BE_FMT_R8G8B8A8,1);
            REQUIRE(pipelines[k]!=0,"pipeline creation failed for control 0x%08X%s",cases[k].control,
                    (cases[k].control&AA_A2O)?" (alpha-to-one requires the alphaToOne device feature)":"");
        }
        unsigned before=failures;
        for(unsigned k=0;k<ncases;++k) {
            g_case=cases[k].name;case_failures=0;
            draw_case(pipelines[k],color,depth);
            REQUIRE(verify_case(&cases[k],color,depth,count)==0,"readback failed, cannot continue");
        }
        for(unsigned k=0;k<ncases;++k) vk_eng_pipeline_release(NULL,pipelines[k]);
        vk_eng_release(NULL,color);vk_eng_release(NULL,depth);
        printf("MSAA %u-sample coverage round: %s (%u cases)\n",count,
               failures==before?"passed":"FAILED",ncases);
    }
    rsx_vulkan_backend_shutdown();
    if(failures) {
        fprintf(stderr,"MSAA coverage GPU: %u of %u checks failed\n",failures,checks);
        return 1;
    }
    printf("MSAA coverage GPU: %u checks passed\n",checks);
    return 0;
fatal:
    fprintf(stderr,"MSAA coverage GPU: fatal failure after %u checks (%u failed)\n",checks,failures);
    if(inited) rsx_vulkan_backend_shutdown();
    return 1;
}
