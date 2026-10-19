/* Position/depth diagnostic using the production Vulkan compiler and draws.
 * Captures stay private. Solid fragment masks do not establish game parity.
 * Controller implementation after managed Claude job-hoZrIG timed out with no
 * accepted artifact. No worker source was applied. */
#define SDL_MAIN_HANDLED
#include "../libs/video/rsx_vulkan_backend.c"
#include <math.h>
#include <stdlib.h>
uint8_t* vm_base;

typedef struct {
    unsigned char* bytes;
    u32 h[32];
    const float* vertices;
    const u32* indices;
    const void* constants;
    rsx_vertex_layout_plan layout;
} capture;

static void* load_file(const char* path,size_t limit,size_t* length)
{
    FILE* f=fopen(path,"rb");
    if(!f)return NULL;
    if(fseek(f,0,SEEK_END)){fclose(f);return NULL;}
    long n=ftell(f);
    if(n<=0 || (unsigned long)n>limit || fseek(f,0,SEEK_SET)){fclose(f);return NULL;}
    unsigned char* b=(unsigned char*)malloc((size_t)n+1);
    if(!b){fclose(f);return NULL;}
    const int ok=fread(b,1,(size_t)n,f)==(size_t)n;
    const int closed=fclose(f);
    if(!ok || closed){free(b);return NULL;}
    b[n]=0;*length=(size_t)n;return b;
}

static int capture_load(capture* c,const char* path)
{
    size_t n=0;memset(c,0,sizeof *c);
    c->bytes=(unsigned char*)load_file(path,4u*1024u*1024u,&n);
    if(!c->bytes || n<128)goto bad;
    memcpy(c->h,c->bytes,128);const u32* h=c->h;
    if(h[0]!=0x314d4746u || h[1]!=2 || h[3]!=RSX_TOPOLOGY_TRIANGLES ||
       !h[4] || h[7]<1 || h[7]>16 || h[5]!=h[7]*16 || h[30]!=514*16 ||
       h[24]>1 || (h[25]!=0x404 && h[25]!=0x405 && h[25]!=0x408) ||
       (h[26]!=0x900 && h[26]!=0x901) || (h[6]?h[6]:h[4])%3)goto bad;
    const uint64_t vb=(uint64_t)h[4]*h[5],ib=(uint64_t)h[6]*4;
    if(128+vb+ib+h[30]+h[31]!=n)goto bad;
    c->layout.count=(u8)h[7];c->layout.stride=h[5];
    memset(c->layout.slot_by_attr,-1,sizeof c->layout.slot_by_attr);
    for(u32 i=0;i<h[7];i++) {
        u32 a=h[8+i];if(a>=16 || (c->layout.mask&(1u<<a)))goto bad;
        c->layout.mask|=1u<<a;c->layout.attrs[i]=(u8)a;c->layout.slot_by_attr[a]=(s8)i;
    }
    if(!(c->layout.mask&1))goto bad;
    c->vertices=(const float*)(c->bytes+128);
    c->indices=h[6]?(const u32*)(c->bytes+128+(size_t)vb):NULL;
    c->constants=c->bytes+128+(size_t)vb+(size_t)ib;
#ifndef RSX_CAPTURE_ALLOW_NONFINITE_ATTRIBUTES
    for(size_t i=0;i<vb/4;i++)if(!isfinite(c->vertices[i]))goto bad;
#endif
    for(u32 i=0;i<h[6];i++)if(c->indices[i]>=h[4])goto bad;
    for(u32 i=0;i<h[30]/4;i++)if(!isfinite(((const float*)c->constants)[i]))goto bad;
    return 1;
bad:
    fprintf(stderr,"Invalid FGM1v2 capture: %s\n",path);
    free(c->bytes);memset(c,0,sizeof *c);return 0;
}

static char* shader_load(const char* path)
{
    size_t n=0;char* s=(char*)load_file(path,1024*1024,&n);
    if(s && memchr(s,0,n)){free(s);s=NULL;}
    if(!s)fprintf(stderr,"Invalid shader input: %s\n",path);
    return s;
}

static u32 pipeline(const capture* c,const char* vs,int pre,u32 compare)
{
    rsx_be_render_state rs={0};
    rs.color_mask=pre?0:0x01010101u;rs.antialias_control=0xffff0001u;
    rs.depth_test=compare!=0;rs.depth_write=pre;rs.depth_func=compare;
    rs.cull_enable=c->h[24];rs.cull_face=c->h[25];rs.front_face=c->h[26];
    return vk_eng_pipeline_create(NULL,vs,
        "float4 main():SV_Target0{return float4(0,1,0,1);}",
        &rs,&c->layout,c->h[5],RSX_BE_FMT_R8G8B8A8,1);
}

static void draw(const capture* c,u32 pipe)
{
    vk_eng_bind_pipeline(NULL,pipe);
    vk_eng_bind_vs_constants(NULL,c->constants,c->h[30]);
    vk_eng_draw(NULL,RSX_TOPOLOGY_TRIANGLES,c->vertices,c->h[4],c->h[5],c->indices,c->h[6]);
}

static int write_ppm(const char* prefix,const char* name,const u8* pixels,u32 w,u32 h)
{
    char path[1024];int n=snprintf(path,sizeof path,"%s-%s.ppm",prefix,name);
    if(n<0 || n>=(int)sizeof path)return 0;
    FILE* f=fopen(path,"wb");if(!f)return 0;
    int ok=fprintf(f,"P6\n%u %u\n255\n",w,h)>0;
    for(size_t i=0;ok && i<(size_t)w*h;i++)ok=fwrite(pixels+i*4,1,3,f)==3;
    return fclose(f)==0 && ok;
}

/* Every run starts from a new clear. compare0 establishes color coverage;
 * EQ/LEQUAL measure how the same color draw survives a separate depth pass. */
static int measure(const capture* depth,const capture* color,const char* dvs,
                   const char* cvs,const char* prefix,u32 w,u32 h,int expected)
{
    u32 rt=0,z=0,pipes[4]={0};u8* pixels=NULL;u8* baseline=NULL;int result=0;
    size_t count=(size_t)w*h;unsigned covered[3]={0},missing[3]={0},extra[3]={0};
    const u32 funcs[3]={0,0x202,0x203};
    const char* names[3]={"baseline","equal","lequal"};
    const float clear[4]={1,0,1,1};
    rt=vk_eng_color_target_create(NULL,RSX_BE_FMT_R8G8B8A8,w,h,NULL,0);
    z=vk_eng_depth_target_create(NULL,w,h);
    if(!rt || !z)goto end;
    pipes[0]=pipeline(depth,dvs,1,0x201);
    for(unsigned i=0;i<3;i++)pipes[i+1]=pipeline(color,cvs,0,funcs[i]);
    for(unsigned i=0;i<4;i++)if(!pipes[i])goto end;
    pixels=(u8*)malloc(count*4);baseline=(u8*)malloc(count);
    if(!pixels || !baseline)goto end;
    for(unsigned mode=0;mode<3;mode++) {
        vk_eng_clear_color(NULL,rt,clear);
        vk_eng_clear_depth_stencil(NULL,z,RSX_BE_CLEAR_DEPTH|RSX_BE_CLEAR_STENCIL,1,0);
        vk_eng_bind_targets(NULL,&rt,1,z);
        vk_eng_set_viewport(NULL,0,0,(float)w,(float)h,0,1);
        vk_eng_set_scissor(NULL,0,0,w,h);
        if(mode)draw(depth,pipes[0]);
        draw(color,pipes[mode+1]);
        memset(pixels,0x55,count*4);vk_eng_readback(NULL,rt,0,0,w,h,pixels,w*4);
        for(size_t p=0;p<count;p++) {
            const u8* v=pixels+p*4;
            int green=v[0]==0 && v[1]==255 && v[2]==0 && v[3]==255;
            int magenta=v[0]==255 && v[1]==0 && v[2]==255 && v[3]==255;
            if(!green && !magenta){fprintf(stderr,"Unexpected pixel at %zu mode%u\n",p,mode);goto end;}
            covered[mode]+=green;
            if(!mode)baseline[p]=(u8)green;
            else {missing[mode]+=baseline[p]&&!green;extra[mode]+=!baseline[p]&&green;}
        }
        if(prefix && !write_ppm(prefix,names[mode],pixels,w,h))goto end;
    }
    printf("{\"scope\":\"position-only replay, single sample, solid fragment\",\"baseline\":%u,\"equal\":%u,\"lequal\":%u,\"equal_missing\":%u,\"lequal_missing\":%u,\"equal_extra\":%u,\"lequal_extra\":%u}\n",
           covered[0],covered[1],covered[2],missing[1],missing[2],extra[1],extra[2]);
    result=expected==3 || covered[0]>0;
    if(expected==1 || expected==3)result=result && !missing[1] && !missing[2] && !extra[1] && !extra[2];
    if(expected==2)result=result && !covered[1] && !covered[2] && missing[1]==covered[0] && missing[2]==covered[0];
end:
    for(unsigned i=0;i<4;i++)if(pipes[i])vk_eng_pipeline_release(NULL,pipes[i]);
    if(rt)vk_eng_release(NULL,rt);
    if(z)vk_eng_release(NULL,z);
    free(pixels);free(baseline);return result;
}

/* Analytic fullscreen triangle, both index orders and both front-face modes.
 * The production HLSL compiler flips Y; forward input therefore has positive
 * Vulkan signed area (CCW). Check rendered pixels, not just enum mapping. */
static int cull_self_test(void)
{
    static const float vertices[12]={-1,-1,.25f,1,3,-1,.25f,1,-1,3,.25f,1};
    static const float constants[514*4]={0};
    static const char plain[]="float4 main(float4 p:ATTR0):SV_Position{return p;}";
    const float clear[4]={1,0,1,1};
    const u32 modes[4]={0,0x404,0x405,0x408};
    u32 rt=vk_eng_color_target_create(NULL,RSX_BE_FMT_R8G8B8A8,64,64,NULL,0);
    u32 z=vk_eng_depth_target_create(NULL,64,64);
    u8 pixels[64*64*4];unsigned checks=0;
    if(!rt || !z)return 0;
    for(unsigned reverse=0;reverse<2;reverse++)
    for(unsigned cw=0;cw<2;cw++)
    for(unsigned m=0;m<4;m++) {
        u32 indices[3]={0,reverse?2:1,reverse?1:2};
        capture c={0};c.h[4]=3;c.h[5]=16;c.h[6]=3;
        c.h[24]=m!=0;c.h[25]=m?modes[m]:0x405;c.h[26]=cw?0x900:0x901;
        c.h[30]=sizeof constants;c.vertices=vertices;c.indices=indices;c.constants=constants;
        rsx_vertex_layout_plan_init(&c.layout,1);
        u32 pipe=pipeline(&c,plain,0,0);if(!pipe)return 0;
        vk_eng_clear_color(NULL,rt,clear);
        vk_eng_bind_targets(NULL,&rt,1,z);
        vk_eng_set_viewport(NULL,0,0,64,64,0,1);vk_eng_set_scissor(NULL,0,0,64,64);
        draw(&c,pipe);memset(pixels,0x55,sizeof pixels);
        vk_eng_readback(NULL,rt,0,0,64,64,pixels,64*4);
        const int front=reverse==cw;
        const int visible=m==0 || (m==1 && !front) || (m==2 && front);
        unsigned covered=0,bad=0;
        for(unsigned p=0;p<64*64;p++) {
            const u8* v=pixels+p*4;
            int green=v[0]==0 && v[1]==255 && v[2]==0 && v[3]==255;
            int magenta=v[0]==255 && v[1]==0 && v[2]==255 && v[3]==255;
            covered+=green;bad+=!green&&!magenta;
        }
        vk_eng_pipeline_release(NULL,pipe);
        if(bad || covered!=(visible?4096u:0u)) {
            fprintf(stderr,"Cull failure reverse=%u cw=%u mode=%x covered=%u expected=%u unexpected=%u\n",
                    reverse,cw,modes[m],covered,visible?4096:0,bad);return 0;
        }
        checks++;
    }
    printf("Cull self-test: %u rendered orientation/mode cases passed\n",checks);return 1;
}

int main(int argc,char** argv)
{
    capture a={0},b={0};char* avs=NULL;char* bvs=NULL;int result=1,inited=0;
    if(argc==3 && !strcmp(argv[1],"--validate")) {
        int ok=capture_load(&a,argv[2]);free(a.bytes);
        if(ok)puts("FGM1v2 validated; no Vulkan initialized");return ok?0:2;
    }
    int self=argc==2 && !strcmp(argv[1],"--self-test");
    int cull=argc==2 && !strcmp(argv[1],"--cull-self-test");
    if(!self && !cull && (argc!=7 || strcmp(argv[1],"--replay"))) {
        fprintf(stderr,"Use --validate capture | --self-test | --cull-self-test | --replay depth.bin color.bin depth.hlsl color.hlsl output-prefix\n");return 2;
    }
    if(!self && !cull) {
        if(strlen(argv[6])>900 || !capture_load(&a,argv[2]) || !capture_load(&b,argv[3]))goto end;
        avs=shader_load(argv[4]);bvs=shader_load(argv[5]);if(!avs || !bvs)goto end;
    }
    SDL_SetMainReady();
    if(rsx_vulkan_backend_init((self||cull)?64:1280,(self||cull)?64:720,"Position replay diagnostic"))goto end;
    inited=1;
    if(cull)result=cull_self_test()?0:1;
    else if(self) {
        static const float vertices[12]={-1,-1,.25f,1,3,-1,.25f,1,-1,3,.25f,1};
        static const float constants[514*4]={0};
        static const char plain[]="float4 main(float4 p:ATTR0):SV_Position{return p;}";
        static const char farther_vs[]="float4 main(float4 p:ATTR0):SV_Position{return p+float4(0,0,.125,0);}";
        a.h[4]=3;a.h[5]=16;a.h[25]=0x405;a.h[26]=0x901;a.h[30]=sizeof constants;
        a.vertices=vertices;a.constants=constants;rsx_vertex_layout_plan_init(&a.layout,1);
        result=measure(&a,&a,plain,plain,NULL,64,64,1) && measure(&a,&a,plain,farther_vs,NULL,64,64,2)?0:1;
        printf("Position replay self-test: %s\n",result?"FAILED":"identical-depth and deliberate-mismatch passed");
    } else result=measure(&a,&b,avs,bvs,argv[6],1280,720,0)?0:1;
end:
    if(inited)rsx_vulkan_backend_shutdown();
    free(avs);free(bvs);free(a.bytes);free(b.bytes);return result;
}
