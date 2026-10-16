/* Software depth comparison (shadow sampling) from R32_FLOAT depth textures
 * on actual Vulkan, verified through colour readbacks of a fullscreen quad.
 * Synthetic depth values only; no game/firmware bytes. */
#define SDL_MAIN_HANDLED
#include "../libs/video/rsx_vulkan_backend.c"
#include "../libs/video/rsx_shadow_sampling.h"
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
uint8_t* vm_base;

/* Backend entry points this test relies on beyond the coverage test's set;
 * overridable so integration can adapt them without touching the cases. */
#ifndef SHADOW_FMT_R32F
#define SHADOW_FMT_R32F RSX_BE_FMT_R32F
#endif
#ifndef SHADOW_TEX_REMAP
#define SHADOW_TEX_REMAP 0xAAE4u   /* RSX identity remap */
#endif
#ifndef SHADOW_TEX_RSXFMT
#define SHADOW_TEX_RSXFMT 0x9Cu    /* CELL_GCM_TEXTURE_X32_FLOAT */
#endif
#ifndef SHADOW_TEX_UPLOAD
#define SHADOW_TEX_UPLOAD(tex,w,h,data) vk_eng_texture_upload(NULL,(tex),0,0,(w),(h),(data),(w)*4u,(h))
#endif
#ifndef SHADOW_BIND_TEXTURES
#define SHADOW_BIND_TEXTURES(texs,samps,n) vk_eng_bind_textures(NULL,(texs),(samps),(n))
#endif
#ifndef SHADOW_COLOR_CREATE
#define SHADOW_COLOR_CREATE(w,h) vk_eng_color_multisample(NULL,RSX_BE_FMT_R8G8B8A8,(w),(h),1)
#endif

/* Guest wrap codes (SET_TEXTURE_ADDRESS). */
#define WRAP_REPEAT 1u
#define WRAP_CLAMP_EDGE 3u

enum { TEX_CONST, TEX_PATTERN2, TEX_CHECKER4, TEX_FRESH, TEX_COUNT };
enum { S_NEAREST_CLAMP, S_NEAREST_REPEAT, S_LINEAR_CLAMP, S_LINEAR_REPEAT, S_COUNT };

typedef struct {
    char name[112];
    char coord[96];
    unsigned func, proj, unnorm, minl, magl;
    unsigned tex, samp;
    int raw;          /* plain SampleLevel depth read instead of the helper */
    float e0, e1;     /* expected value at pixel centre x: e0 + e1*x */
} shadow_case;

static unsigned checks, failures, case_failures;
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
    fprintf(stderr,"FAIL line %d [%s]: ",line,g_case);
    va_list ap;va_start(ap,fmt);vfprintf(stderr,fmt,ap);va_end(ap);
    fputc('\n',stderr);
}
#define EXPECT(x,...) expect(!!(x),__LINE__,__VA_ARGS__)
#define REQUIRE(x,...) do { int ok_=!!(x);expect(ok_,__LINE__,__VA_ARGS__);if(!ok_) goto fatal; } while(0)

static rsx_be_sampler_desc make_sampler(int linear,u8 wrap)
{
    rsx_be_sampler_desc s;
    memset(&s,0,sizeof s);
    s.min_linear=(u8)linear;s.mag_linear=(u8)linear;s.mip_linear=0;s.mip_present=0;
    s.wrap_s=wrap;s.wrap_t=wrap;s.wrap_r=wrap;
    s.min_lod=0.f;s.max_lod=0.f;
    return s;
}

static void add_case(shadow_case* out,unsigned* n,const char* name,const char* coord,
                     unsigned func,unsigned proj,unsigned unnorm,unsigned minl,unsigned magl,
                     unsigned tex,unsigned samp,float e0,float e1)
{
    shadow_case* c=&out[(*n)++];
    memset(c,0,sizeof *c);
    snprintf(c->name,sizeof c->name,"%s",name);
    snprintf(c->coord,sizeof c->coord,"%s",coord);
    c->func=func;c->proj=proj;c->unnorm=unnorm;c->minl=minl;c->magl=magl;
    c->tex=tex;c->samp=samp;c->e0=e0;c->e1=e1;
}

/* Depth contents:
 *   TEX_CONST    4x4, all 0.5
 *   TEX_PATTERN2 2x2, (0,0)=0.1 (1,0)=0.9 (0,1)=0.6 (1,1)=0.3
 *   TEX_CHECKER4 4x4, 0.8 where x+y is odd, else 0.2
 * Reference 0.5 with LESS therefore yields 1 exactly on depth > 0.5. */
static unsigned build_cases(shadow_case* out)
{
    static const char* const fname[8]={"NEVER","LESS","EQUAL","LEQUAL","GREATER","NOTEQUAL","GEQUAL","ALWAYS"};
    static const char* const refs[3]={"0.25","0.5","0.75"};
    /* ref below / equal / above the constant depth 0.5 */
    static const float want[8][3]={{0,0,0},{1,0,0},{0,1,0},{1,1,0},{0,0,1},{1,0,1},{0,1,1},{1,1,1}};
    unsigned n=0;
    char name[112],coord[96];
    for(unsigned lin=0;lin<2;++lin)
    for(unsigned f=0;f<8;++f) for(unsigned r=0;r<3;++r) {
        snprintf(name,sizeof name,"%s %s ref %s vs depth 0.5",lin?"linear":"nearest",fname[f],refs[r]);
        snprintf(coord,sizeof coord,"float4(0.375,0.625,%s,1.0)",refs[r]);
        add_case(out,&n,name,coord,f,0,0,lin,lin,TEX_CONST,lin?S_LINEAR_CLAMP:S_NEAREST_CLAMP,want[f][r],0);
    }

    /* Nearest on the 2x2 pattern, at and off texel centres. */
    add_case(out,&n,"nearest 2x2 texel (0,0) 0.1","float4(0.25,0.25,0.5,1.0)",1,0,0,0,0,TEX_PATTERN2,S_NEAREST_CLAMP,0,0);
    add_case(out,&n,"nearest 2x2 texel (1,0) 0.9","float4(0.75,0.25,0.5,1.0)",1,0,0,0,0,TEX_PATTERN2,S_NEAREST_CLAMP,1,0);
    add_case(out,&n,"nearest 2x2 texel (0,1) 0.6","float4(0.25,0.75,0.5,1.0)",1,0,0,0,0,TEX_PATTERN2,S_NEAREST_CLAMP,1,0);
    add_case(out,&n,"nearest 2x2 texel (1,1) 0.3","float4(0.75,0.75,0.5,1.0)",1,0,0,0,0,TEX_PATTERN2,S_NEAREST_CLAMP,0,0);
    add_case(out,&n,"nearest 2x2 texel (1,1) GREATER","float4(0.75,0.75,0.5,1.0)",4,0,0,0,0,TEX_PATTERN2,S_NEAREST_CLAMP,1,0);
    add_case(out,&n,"nearest 2x2 off-centre floors to (0,0)","float4(0.45,0.05,0.5,1.0)",1,0,0,0,0,TEX_PATTERN2,S_NEAREST_CLAMP,0,0);
    add_case(out,&n,"nearest 2x2 off-centre floors to (1,0)","float4(0.55,0.45,0.5,1.0)",1,0,0,0,0,TEX_PATTERN2,S_NEAREST_CLAMP,1,0);
    add_case(out,&n,"nearest 2x2 off-centre floors to (0,1)","float4(0.05,0.95,0.5,1.0)",1,0,0,0,0,TEX_PATTERN2,S_NEAREST_CLAMP,1,0);
    add_case(out,&n,"nearest flags, linear sampler, (0,0)","float4(0.45,0.45,0.5,1.0)",1,0,0,0,0,TEX_PATTERN2,S_LINEAR_CLAMP,0,0);
    add_case(out,&n,"nearest flags, linear sampler, (1,0)","float4(0.55,0.45,0.5,1.0)",1,0,0,0,0,TEX_PATTERN2,S_LINEAR_CLAMP,1,0);

    /* Linear: compare first, then blend. The averaged depth at these points
     * is 0.5, which would give 0 (LESS) or 1 (LEQUAL) instead of 0.5. */
    add_case(out,&n,"raw filtered depth read stays unchanged","",0,0,0,0,0,TEX_CHECKER4,S_LINEAR_CLAMP,.5f,0);
    out[n-1].raw=1;
    add_case(out,&n,"linear LESS half between 0.2/0.8","float4(0.5,0.375,0.5,1.0)",1,0,0,1,1,TEX_CHECKER4,S_LINEAR_CLAMP,.5f,0);
    add_case(out,&n,"linear LEQUAL half between 0.2/0.8","float4(0.5,0.375,0.5,1.0)",3,0,0,1,1,TEX_CHECKER4,S_LINEAR_CLAMP,.5f,0);
    add_case(out,&n,"linear LESS 2D quad of checker","float4(0.5,0.5,0.5,1.0)",1,0,0,1,1,TEX_CHECKER4,S_LINEAR_CLAMP,.5f,0);
    add_case(out,&n,"linear LESS x weight 0.25","float4(0.4375,0.375,0.5,1.0)",1,0,0,1,1,TEX_CHECKER4,S_LINEAR_CLAMP,.25f,0);
    add_case(out,&n,"linear LESS y weight 0.25","float4(0.375,0.4375,0.5,1.0)",1,0,0,1,1,TEX_CHECKER4,S_LINEAR_CLAMP,.25f,0);
    add_case(out,&n,"linear LESS xy weights 0.25","float4(0.4375,0.4375,0.5,1.0)",1,0,0,1,1,TEX_CHECKER4,S_LINEAR_CLAMP,.375f,0);
    add_case(out,&n,"linear at texel centre equals nearest LESS","float4(0.375,0.375,0.5,1.0)",1,0,0,1,1,TEX_CHECKER4,S_LINEAR_CLAMP,0,0);
    add_case(out,&n,"linear at texel centre equals nearest GREATER","float4(0.375,0.375,0.5,1.0)",4,0,0,1,1,TEX_CHECKER4,S_LINEAR_CLAMP,1,0);
    add_case(out,&n,"nearest at same centre LESS","float4(0.375,0.375,0.5,1.0)",1,0,0,0,0,TEX_CHECKER4,S_LINEAR_CLAMP,0,0);

    /* Projection divides xyz by w. */
    add_case(out,&n,"projected matches linear half case","float4(1.0,0.75,1.0,2.0)",1,1,0,1,1,TEX_CHECKER4,S_LINEAR_CLAMP,.5f,0);
    add_case(out,&n,"projected ref 0.8/2 LESS 0.5","float4(0.25,0.25,0.8,2.0)",1,1,0,0,0,TEX_CONST,S_NEAREST_CLAMP,1,0);
    add_case(out,&n,"same coord unprojected ref 0.8","float4(0.25,0.25,0.8,2.0)",1,0,0,0,0,TEX_CONST,S_NEAREST_CLAMP,0,0);
    add_case(out,&n,"projected w=4 picks 2x2 texel (0,1)","float4(1.0,3.0,1.6,4.0)",1,1,0,0,0,TEX_PATTERN2,S_NEAREST_CLAMP,1,0);
    add_case(out,&n,"projected w=4 picks 2x2 texel (1,1)","float4(3.0,3.0,1.6,4.0)",1,1,0,0,0,TEX_PATTERN2,S_NEAREST_CLAMP,0,0);

    /* Unnormalized xy are texel space; z stays as given. */
    add_case(out,&n,"unnormalized linear half, z unchanged","float4(2.0,1.5,0.5,1.0)",1,0,1,1,1,TEX_CHECKER4,S_LINEAR_CLAMP,.5f,0);
    add_case(out,&n,"unnormalized nearest 2x2 (1,0)","float4(1.5,0.5,0.5,1.0)",1,0,1,0,0,TEX_PATTERN2,S_NEAREST_CLAMP,1,0);
    add_case(out,&n,"unnormalized nearest 2x2 (0,0)","float4(0.5,0.5,0.5,1.0)",1,0,1,0,0,TEX_PATTERN2,S_NEAREST_CLAMP,0,0);
    add_case(out,&n,"unnormalized+projected linear half","float4(4.0,3.0,1.0,2.0)",1,1,1,1,1,TEX_CHECKER4,S_LINEAR_CLAMP,.5f,0);
    add_case(out,&n,"unnormalized z 0.75 GREATER 0.5","float4(1.0,1.0,0.75,1.0)",4,0,1,0,0,TEX_CONST,S_NEAREST_CLAMP,1,0);

    /* Wrapping through the sampler for out-of-range coordinates. */
    add_case(out,&n,"nearest repeat u=1.25 -> (0,0)","float4(1.25,0.25,0.5,1.0)",1,0,0,0,0,TEX_PATTERN2,S_NEAREST_REPEAT,0,0);
    add_case(out,&n,"nearest clamp u=1.25 -> (1,0)","float4(1.25,0.25,0.5,1.0)",1,0,0,0,0,TEX_PATTERN2,S_NEAREST_CLAMP,1,0);
    add_case(out,&n,"nearest repeat u=-0.25 -> (1,1)","float4(-0.25,0.75,0.5,1.0)",1,0,0,0,0,TEX_PATTERN2,S_NEAREST_REPEAT,0,0);
    add_case(out,&n,"nearest clamp u=-0.25 -> (0,1)","float4(-0.25,0.75,0.5,1.0)",1,0,0,0,0,TEX_PATTERN2,S_NEAREST_CLAMP,1,0);
    add_case(out,&n,"linear repeat at u=0 blends texel 3 and 0","float4(0.0,0.375,0.5,1.0)",1,0,0,1,1,TEX_CHECKER4,S_LINEAR_REPEAT,.5f,0);
    add_case(out,&n,"linear clamp at u=0 stays on texel 0","float4(0.0,0.375,0.5,1.0)",1,0,0,1,1,TEX_CHECKER4,S_LINEAR_CLAMP,1,0);
    add_case(out,&n,"linear repeat at u=1 blends texel 3 and 0","float4(1.0,0.375,0.5,1.0)",1,0,0,1,1,TEX_CHECKER4,S_LINEAR_REPEAT,.5f,0);
    add_case(out,&n,"linear clamp at u=1 stays on texel 3","float4(1.0,0.375,0.5,1.0)",1,0,0,1,1,TEX_CHECKER4,S_LINEAR_CLAMP,0,0);

    /* Min/mag selection from a varying coordinate. 2 texels per pixel
     * minifies: nearest gives 0, linear 0.5. 1/16 texel per pixel magnifies:
     * nearest stays on texel (1,1) = 0, linear gives 0.5 - x/16. */
    add_case(out,&n,"minified picks min nearest","float4(pos.x*0.5,0.375,0.5,1.0)",1,0,0,0,1,TEX_CHECKER4,S_LINEAR_REPEAT,0,0);
    add_case(out,&n,"minified picks min linear","float4(pos.x*0.5,0.375,0.5,1.0)",1,0,0,1,0,TEX_CHECKER4,S_LINEAR_REPEAT,.5f,0);
    add_case(out,&n,"magnified picks mag linear","float4(pos.x*0.015625+0.25,0.375,0.5,1.0)",1,0,0,0,1,TEX_CHECKER4,S_LINEAR_REPEAT,.5f,-.0625f);
    add_case(out,&n,"magnified picks mag nearest","float4(pos.x*0.015625+0.25,0.375,0.5,1.0)",1,0,0,1,0,TEX_CHECKER4,S_LINEAR_REPEAT,0,0);
    return n;
}

static int compose_fs(char* out,size_t cap,const shadow_case* c)
{
    char body[256];
    if(c->raw)
        snprintf(body,sizeof body,"return rsx_tex[0].SampleLevel(rsx_samp[0],float2(0.5,0.375),0.0).rrrr;");
    else
        snprintf(body,sizeof body,"return rsx_shadow_sample(rsx_tex[0],rsx_samp[0],%s,%uu,%uu,%uu,%uu,%uu);",
                 c->coord,c->func,c->proj,c->unnorm,c->minl,c->magl);
    const int n=snprintf(out,cap,
        "Texture2D rsx_tex[16]:register(t0);\n"
        "SamplerState rsx_samp[16]:register(s0);\n"
        "%s\n"
        "float4 main(float4 pos:SV_Position):SV_Target0{%s}\n",rsx_shadow_hlsl,body);
    return n>0&&(size_t)n<cap;
}

static u32 make_pipeline(const shadow_case* c)
{
    static const char vs[]="float4 main(float4 p:ATTR0):SV_Position{return p;}";
    static char fs[16384];
    if(!compose_fs(fs,sizeof fs,c)) return 0;
    rsx_vertex_layout_plan layout={0};layout.count=1;layout.stride=16;layout.attrs[0]=0;
    rsx_be_render_state state={0};
    state.color_mask=0x01010101u;
    state.antialias_control=0xFFFF0001u;
    return vk_eng_pipeline_create(NULL,vs,fs,&state,&layout,16,RSX_BE_FMT_R8G8B8A8,1);
}

static void draw_quad(u32 pipeline,u32 color,u32 tex,const rsx_be_sampler_desc* samp)
{
    static const float vertices[6][4]={{-1,-1,.5f,1},{1,-1,.5f,1},{1,1,.5f,1},
                                      {-1,-1,.5f,1},{1,1,.5f,1},{-1,1,.5f,1}};
    /* Channels differ, so an unwritten pixel never looks like a replicated result. */
    const float clear[4]={0,.5f,1,.25f};
    u32 texs[1];texs[0]=tex;
    vk_eng_clear_color(NULL,color,clear);
    vk_eng_bind_targets(NULL,&color,1,0);
    vk_eng_bind_pipeline(NULL,pipeline);
    SHADOW_BIND_TEXTURES(texs,samp,1u);
    vk_eng_set_viewport(NULL,0,0,8,4,0,1);
    vk_eng_set_scissor(NULL,0,0,8,4);
    vk_eng_draw(NULL,RSX_TOPOLOGY_TRIANGLES,vertices,6,16,NULL,0);
}

static void verify(u32 color,float e0,float e1)
{
    uint8_t pixels[8*4*4];
    memset(pixels,0x55,sizeof pixels);
    vk_eng_readback(NULL,color,0,0,8,4,pixels,8*4);
    /* Interior pixels only: edges are not needed and avoid raster ambiguity. */
    for(unsigned y=1;y<3;++y) for(unsigned x=1;x<7;++x) {
        float e=e0+e1*((float)x+.5f);
        if(e<0.f) e=0.f;
        if(e>1.f) e=1.f;
        const int e8=(int)lroundf(e*255.f);
        const int tol=(e8==0||e8==255)?0:1;
        const uint8_t* p=pixels+(y*8+x)*4;
        for(unsigned ch=0;ch<4;++ch)
            EXPECT(abs((int)p[ch]-e8)<=tol,"pixel (%u,%u) channel %u = %u, expected %d (%.4f); rgba=%u,%u,%u,%u",
                   x,y,ch,p[ch],e8,e,p[0],p[1],p[2],p[3]);
    }
}

static void fill(float* d,unsigned count,float v)
{
    for(unsigned i=0;i<count;++i) d[i]=v;
}

static u32 make_depth_texture(u32 w,u32 h,const float* data)
{
    const u32 t=vk_eng_texture_create(NULL,SHADOW_FMT_R32F,w,h,1,1,SHADOW_TEX_REMAP,SHADOW_TEX_RSXFMT);
    if(t) SHADOW_TEX_UPLOAD(t,w,h,data);
    return t;
}

int main(void)
{
    SDL_SetMainReady();
    int inited=0;
    u32 color=0,textures[TEX_COUNT]={0},fresh_pipe=0,wrap_pipe=0;
    static shadow_case cases[128];
    rsx_be_sampler_desc samplers[S_COUNT];
    samplers[S_NEAREST_CLAMP]=make_sampler(0,WRAP_CLAMP_EDGE);
    samplers[S_NEAREST_REPEAT]=make_sampler(0,WRAP_REPEAT);
    samplers[S_LINEAR_CLAMP]=make_sampler(1,WRAP_CLAMP_EDGE);
    samplers[S_LINEAR_REPEAT]=make_sampler(1,WRAP_REPEAT);

    REQUIRE(rsx_vulkan_backend_init(8,4,"Shadow comparison regression")==0,"backend init failed");
    inited=1;
    color=SHADOW_COLOR_CREATE(8,4);
    REQUIRE(color!=0,"colour target not created");

    float data[16];
    fill(data,16,.5f);
    textures[TEX_CONST]=make_depth_texture(4,4,data);
    {
        const float p2[4]={.1f,.9f,.6f,.3f};
        textures[TEX_PATTERN2]=make_depth_texture(2,2,p2);
    }
    for(unsigned y=0;y<4;++y) for(unsigned x=0;x<4;++x) data[y*4+x]=((x+y)&1u)?.8f:.2f;
    textures[TEX_CHECKER4]=make_depth_texture(4,4,data);
    fill(data,16,.25f);
    textures[TEX_FRESH]=make_depth_texture(4,4,data);
    for(unsigned t=0;t<TEX_COUNT;++t)
        REQUIRE(textures[t]!=0,"R32_FLOAT depth texture %u not created",t);

    const unsigned ncases=build_cases(cases);
    for(unsigned k=0;k<ncases;++k) {
        const shadow_case* c=&cases[k];
        g_case=c->name;case_failures=0;
        const u32 pipe=make_pipeline(c);
        EXPECT(pipe!=0,"pipeline creation failed");
        if(!pipe) continue;
        draw_quad(pipe,color,textures[c->tex],&samplers[c->samp]);
        verify(color,c->e0,c->e1);
        vk_eng_pipeline_release(NULL,pipe);
    }

    /* One pipeline across texture uploads and rebinds: every draw must see the
     * currently bound contents. Reference 0.4 LESS. */
    {
        shadow_case c;memset(&c,0,sizeof c);
        snprintf(c.coord,sizeof c.coord,"float4(0.375,0.375,0.4,1.0)");
        c.func=1;
        g_case="fresh texture pipeline";case_failures=0;
        fresh_pipe=make_pipeline(&c);
        REQUIRE(fresh_pipe!=0,"pipeline creation failed");
        const rsx_be_sampler_desc* s=&samplers[S_NEAREST_CLAMP];
        g_case="fresh: constant 0.5 texture";case_failures=0;
        draw_quad(fresh_pipe,color,textures[TEX_CONST],s);verify(color,1,0);
        g_case="fresh: rebind to 0.25 texture";case_failures=0;
        draw_quad(fresh_pipe,color,textures[TEX_FRESH],s);verify(color,0,0);
        g_case="fresh: reupload 0.75";case_failures=0;
        fill(data,16,.75f);SHADOW_TEX_UPLOAD(textures[TEX_FRESH],4,4,data);
        draw_quad(fresh_pipe,color,textures[TEX_FRESH],s);verify(color,1,0);
        g_case="fresh: reupload 0.125";case_failures=0;
        fill(data,16,.125f);SHADOW_TEX_UPLOAD(textures[TEX_FRESH],4,4,data);
        draw_quad(fresh_pipe,color,textures[TEX_FRESH],s);verify(color,0,0);
        g_case="fresh: back to constant 0.5";case_failures=0;
        draw_quad(fresh_pipe,color,textures[TEX_CONST],s);verify(color,1,0);
    }

    /* One pipeline across sampler switches: wrap must follow the bound sampler. */
    {
        shadow_case c;memset(&c,0,sizeof c);
        snprintf(c.coord,sizeof c.coord,"float4(1.25,0.25,0.5,1.0)");
        c.func=1;
        g_case="sampler switch pipeline";case_failures=0;
        wrap_pipe=make_pipeline(&c);
        REQUIRE(wrap_pipe!=0,"pipeline creation failed");
        static const unsigned seq[4]={S_NEAREST_REPEAT,S_NEAREST_CLAMP,S_NEAREST_REPEAT,S_NEAREST_CLAMP};
        for(unsigned i=0;i<4;++i) {
            g_case=seq[i]==S_NEAREST_REPEAT?"sampler switch: repeat":"sampler switch: clamp";
            case_failures=0;
            draw_quad(wrap_pipe,color,textures[TEX_PATTERN2],&samplers[seq[i]]);
            verify(color,seq[i]==S_NEAREST_REPEAT?0.f:1.f,0);
        }
    }

    vk_eng_pipeline_release(NULL,fresh_pipe);
    vk_eng_pipeline_release(NULL,wrap_pipe);
    for(unsigned t=0;t<TEX_COUNT;++t) vk_eng_release(NULL,textures[t]);
    vk_eng_release(NULL,color);
    rsx_vulkan_backend_shutdown();
    if(failures) {
        fprintf(stderr,"Shadow sampling GPU: %u of %u checks failed (%u cases)\n",failures,checks,ncases);
        return 1;
    }
    printf("Shadow sampling GPU: %u checks passed (%u cases)\n",checks,ncases);
    return 0;
fatal:
    fprintf(stderr,"Shadow sampling GPU: fatal failure after %u checks (%u failed)\n",checks,failures);
    if(fresh_pipe) vk_eng_pipeline_release(NULL,fresh_pipe);
    if(wrap_pipe) vk_eng_pipeline_release(NULL,wrap_pipe);
    for(unsigned t=0;t<TEX_COUNT;++t) if(textures[t]) vk_eng_release(NULL,textures[t]);
    if(color) vk_eng_release(NULL,color);
    if(inited) rsx_vulkan_backend_shutdown();
    return 1;
}
