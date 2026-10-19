/* Exact generated alpha shader raster comparison with captured FTX1 resources.
 * Single sample coverage is compared in both directions. This is a fixture,
 * not an in-game AO pass or a claim about secondary-ray mip selection. */
#define RSX_CAPTURE_ALLOW_NONFINITE_ATTRIBUTES 1
#define main rsx_position_replay_fixture_main
#include "test_vulkan_position_replay.c"
#undef main
#include "../libs/video/rsx_texture_layout.h"

static rsx_be_format texformat(rsx_texfmt f) {
    switch(f) {
    case RSX_TEXFMT_R8:return RSX_BE_FMT_R8;case RSX_TEXFMT_R8G8:return RSX_BE_FMT_R8G8;
    case RSX_TEXFMT_R8G8B8A8:return RSX_BE_FMT_R8G8B8A8;case RSX_TEXFMT_BC1:return RSX_BE_FMT_BC1;
    case RSX_TEXFMT_BC2:return RSX_BE_FMT_BC2;case RSX_TEXFMT_BC3:return RSX_BE_FMT_BC3;
    case RSX_TEXFMT_R16:return RSX_BE_FMT_R16;case RSX_TEXFMT_R16G16:return RSX_BE_FMT_R16G16;
    case RSX_TEXFMT_R16G16F:return RSX_BE_FMT_R16G16F;case RSX_TEXFMT_R16G16B16A16F:return RSX_BE_FMT_R16G16B16A16F;
    case RSX_TEXFMT_R32F:return RSX_BE_FMT_R32F;case RSX_TEXFMT_R32G32B32A32F:return RSX_BE_FMT_R32G32B32A32F;
    default:return 0;
    }
}
static u64 payload_hash(const unsigned char* src,u32 span) {
    const u64 prime=1099511628211ull;u64 h[4]={1469598103934665603ull,1469598103934665602ull,1469598103934665601ull,1469598103934665600ull};u32 i=0;
    for(;i+32<=span;i+=32)for(u32 k=0;k<4;k++){u64 word;memcpy(&word,src+i+k*8,8);h[k]=(h[k]^word)*prime;}
    u64 hash=h[0];for(u32 k=1;k<4;k++)hash=(hash^h[k])*prime;
    for(;i+8<=span;i+=8){u64 word;memcpy(&word,src+i,8);hash=(hash^word)*prime;}
    for(;i<span;i++)hash=(hash^src[i])*prime;return hash;
}
static int texture(const char* path,u32* handles,rsx_be_sampler_desc* samplers,u32* mask,u32 pass,u32 draw_id,const capture* capture) {
    size_t n=0;unsigned char* bytes=load_file(path,16u*1024u*1024u+128u,&n);if(!bytes || n<128){free(bytes);return 0;}
    u32 h[32];memcpy(h,bytes,128);int ok=0;u32 handle=0;
    if(h[0]!=0x31585446u || h[1]!=1 || h[4]>=16 || h[8]!=2 || h[9] || h[14]>1 || h[21] || !h[20] || n!=128u+h[20] ||
       !h[11] || !h[12] || h[11]>4096 || h[12]>4096 || h[10]>RSX_MAX_TEXTURE_LEVELS)goto end;
    if(h[2]!=pass || h[3]!=draw_id || h[23]!=0x204u || (*mask&(1u<<h[4])) ||
       payload_hash(bytes+128,h[20])!=((u64)h[26]|((u64)h[27]<<32)) || capture->h[31]<16)goto end;
    float reference;memcpy(&reference,(const unsigned char*)capture->constants+capture->h[30]+capture->h[31]-16,4);
    if(reference!=rsx_fp_alpha_ref(h[24],h[25]))goto end;
    rsx_tex_level levels[RSX_MAX_TEXTURE_LEVELS];u32 count=rsx_texture_mip_chain(h[7],h[11],h[12],h[10],h[13],levels);if(!count)goto end;
    handle=vk_eng_texture_create(NULL,texformat(levels[0].tl.fmt),h[11],h[12],count,1,h[16]&0xffffu,h[7]);if(!handle)goto end;
    for(u32 i=0;i<count;i++) {
        rsx_tex_level* l=&levels[i];size_t size=(size_t)l->tl.dst_row_bytes*l->tl.rows;
        if((uint64_t)l->offset+l->tl.face_bytes>h[20] || size>64u*1024u*1024u)goto end;
        unsigned char* decoded=malloc(size);if(!decoded)goto end;
        rsx_texture_decode(decoded,l->tl.dst_row_bytes,bytes+128+l->offset,l->w,l->h,&l->tl,(int)h[22]);
        vk_eng_texture_upload(NULL,handle,0,i,l->w,l->h,decoded,l->tl.dst_row_bytes,l->tl.rows);free(decoded);
    }
    rsx_be_sampler_desc* d=&samplers[h[4]];u32 minf=(h[17]>>16)&7,magf=(h[17]>>24)&7;
    d->min_linear=minf==2 || minf==4 || minf==6;d->mag_linear=magf==2;d->mip_present=minf>=3;d->mip_linear=minf==5 || minf==6;
    d->wrap_s=h[15]&15;d->wrap_t=(h[15]>>8)&15;d->wrap_r=(h[15]>>16)&15;d->anisotropy_code=(h[18]>>4)&7;
    d->min_lod=(float)((h[18]>>19)&4095)/256;d->max_lod=d->mip_present?(float)((h[18]>>7)&4095)/256:0;
    if(d->max_lod<d->min_lod)d->max_lod=d->min_lod;
    handles[h[4]]=handle;*mask|=1u<<h[4];ok=1;
end:if(!ok && handle)vk_eng_release(NULL,handle);free(bytes);return ok;
}
static char* mask_shader(const char* original) {
    const char* entry="PSOutput main(PSInput input)";const char* at=strstr(original,entry);if(!at || strstr(at+strlen(entry),entry))return NULL;
    size_t len=strlen(original),prefix=(size_t)(at-original);char* out=malloc(len+256);if(!out)return NULL;
    memcpy(out,original,prefix);strcpy(out+prefix,"PSOutput rsx_alpha_original(PSInput input)");strcat(out,at+strlen(entry));
    strcat(out,"\nfloat4 main(PSInput input):SV_Target0 { rsx_alpha_original(input); return float4(0,1,0,1); }\n");return out;
}
static char* varying_shader(void) {
    char* out=calloc(1,4096);if(!out)return NULL;
    const char* names[]={"pos","col0","col1","fog","t0","t1","t2","t3","t4","t5","t6","t7","t8","t9"};
    const char* semantics[]={"SV_Position","COLOR0","COLOR1","FOG","TEXCOORD0","TEXCOORD1","TEXCOORD2","TEXCOORD3","TEXCOORD4","TEXCOORD5","TEXCOORD6","TEXCOORD7","TEXCOORD8","TEXCOORD9"};
    size_t n=(size_t)sprintf(out,"struct VSInput {");for(unsigned i=0;i<14;i++)n+=(size_t)sprintf(out+n,"float4 a%u:ATTR%u;",i,i);
    n+=(size_t)sprintf(out+n,"};struct VSOutput {");for(unsigned i=0;i<14;i++)n+=(size_t)sprintf(out+n,"float4 %s:%s;",names[i],semantics[i]);
    n+=(size_t)sprintf(out+n,"};VSOutput main(VSInput input){VSOutput o;");for(unsigned i=0;i<14;i++)n+=(size_t)sprintf(out+n,"o.%s=input.a%u;",names[i],i);
    strcpy(out+n,"return o;}");return out;
}
static int render(const capture* c,const char* vs,const char* ps,const u32* textures,const rsx_be_sampler_desc* samplers,u32 mask,u8* pixels) {
    u32 rt=vk_eng_color_target_create(NULL,RSX_BE_FMT_R8G8B8A8,1280,720,NULL,0),z=vk_eng_depth_target_create(NULL,1280,720),pipe=0;int ok=0;
    if(!rt || !z)goto end;rsx_be_render_state state={0};state.color_mask=0x01010101;state.antialias_control=0xffff0001;
    state.cull_enable=c->h[24];state.cull_face=c->h[25];state.front_face=c->h[26];
    pipe=vk_eng_pipeline_create(NULL,vs,ps,&state,&c->layout,c->h[5],RSX_BE_FMT_R8G8B8A8,1);if(!pipe)goto end;
    const float clear[4]={1,0,1,1};vk_eng_clear_color(NULL,rt,clear);vk_eng_clear_depth_stencil(NULL,z,RSX_BE_CLEAR_DEPTH,1,0);
    vk_eng_bind_targets(NULL,&rt,1,z);vk_eng_bind_pipeline(NULL,pipe);vk_eng_bind_vs_constants(NULL,c->constants,c->h[30]);
    vk_eng_bind_ps_constants(NULL,(const unsigned char*)c->constants+c->h[30],c->h[31]);vk_eng_bind_textures(NULL,textures,samplers,mask);
    vk_eng_set_viewport(NULL,0,0,1280,720,0,1);vk_eng_set_scissor(NULL,0,0,1280,720);vk_eng_draw(NULL,RSX_TOPOLOGY_TRIANGLES,c->vertices,c->h[4],c->h[5],c->indices,c->h[6]);
    memset(pixels,0x55,1280*720*4);vk_eng_readback(NULL,rt,0,0,1280,720,pixels,1280*4);ok=1;
end:if(pipe)vk_eng_pipeline_release(NULL,pipe);if(rt)vk_eng_release(NULL,rt);if(z)vk_eng_release(NULL,z);return ok;
}
int main(int argc,char** argv) {
    if(argc<7 || argc>22){fprintf(stderr,"geometry.bin vertex.hlsl fragment.hlsl output-prefix expected-alpha-texture-mask tex0.ftx [tex1.ftx ...]\n");return 2;}
    /* Caller must derive this mask from a hash-verified alpha dependency
     * contract. We compare its exact resource set, never accept missing units. */
    char* mask_end=NULL;unsigned long requested=strtoul(argv[5],&mask_end,0);
    if(!*argv[5] || *mask_end || !requested || requested>65535)return 2;
    const char* base=argv[1];for(const char* q=base;*q;q++)if(*q=='/' || *q=='\\')base=q+1;
    u32 pass=0,draw_id=0;
    if(sscanf(base,"pass%u_draw%u_geometry.bin",&pass,&draw_id)!=2){fprintf(stderr,"alpha geometry requires pass/draw filename provenance\n");return 2;}
    capture a={0},b={0};char* vs=NULL;char* ps=NULL;char* masked=NULL;char* replay=NULL;rsx_scene_vertex_output* vertices=NULL;u8 *first=NULL,*second=NULL;int result=1,init=0;
    u32 textures[16]={0},mask=0;rsx_be_sampler_desc samplers[16]={0};
    if(!capture_load(&a,argv[1]) || !(vs=shader_load(argv[2])) || !(ps=shader_load(argv[3])) || !(masked=mask_shader(ps)) || !(replay=varying_shader()))goto done;
#ifdef _WIN32
    _putenv_s("PS3RECOMP_VK_GUEST_PROGRAMS","1");
#else
    setenv("PS3RECOMP_VK_GUEST_PROGRAMS","1",1);
#endif
    SDL_SetMainReady();if(rsx_vulkan_backend_init(1280,720,"alpha varying qualification"))goto done;init=1;
    for(int i=6;i<argc;i++)if(!texture(argv[i],textures,samplers,&mask,pass,draw_id,&a))goto done;
    if(mask!=(u32)requested){fprintf(stderr,"alpha texture mask mismatch\n");goto done;}
    vertices=malloc((size_t)a.h[4]*sizeof *vertices);first=malloc(1280*720*4);second=malloc(1280*720*4);if(!vertices || !first || !second)goto done;
    if(rsx_vulkan_backend_replay_outputs(vs,&a.layout,a.vertices,a.h[4],a.constants,a.h[30],vertices,a.h[4]))goto done;
    b=a;b.bytes=NULL;b.vertices=(const float*)vertices;b.h[5]=sizeof *vertices;rsx_vertex_layout_plan_init(&b.layout,0x3fff);
    if(!render(&a,vs,masked,textures,samplers,mask,first) || !render(&b,replay,masked,textures,samplers,mask,second))goto done;
    unsigned original=0,replayed=0,different=0,invalid=0,opaque=0;
    for(size_t i=0;i<1280*720;i++) {
        const u8* x=first+i*4;const u8* y=second+i*4;int xg=x[0]==0&&x[1]==255&&x[2]==0&&x[3]==255,yg=y[0]==0&&y[1]==255&&y[2]==0&&y[3]==255;
        int xm=x[0]==255&&x[1]==0&&x[2]==255&&x[3]==255,ym=y[0]==255&&y[1]==0&&y[2]==255&&y[3]==255;
        original+=xg;replayed+=yg;different+=memcmp(x,y,4)!=0;invalid+=(!xg&&!xm)||(!yg&&!ym);
    }
    printf("{\"scope\":\"single-sample original guest alpha shader\",\"original\":%u,\"replay\":%u,\"pixel_mismatch\":%u,\"invalid\":%u}\n",original,replayed,different,invalid);
    if(strcmp(argv[4],"-") && (!write_ppm(argv[4],"original-alpha",first,1280,720) || !write_ppm(argv[4],"replay-alpha",second,1280,720)))goto done;
    if(!render(&a,vs,"float4 main():SV_Target0{return float4(0,1,0,1);}",textures,samplers,mask,first))goto done;
    for(size_t i=0;i<1280*720;i++)if(first[i*4]==0 && first[i*4+1]==255 && first[i*4+2]==0 && first[i*4+3]==255)++opaque;
    printf("{\"opaque_coverage\":%u,\"alpha_coverage\":%u,\"qualification\":\"%s\"}\n",opaque,original,
        original?"measured":opaque?"all_fragments_rejected":"no_visible_geometry");
    result=different || invalid?1:original?0:77;
done:if(init)rsx_vulkan_backend_shutdown();free(a.bytes);free(vs);free(ps);free(masked);free(replay);free(vertices);free(first);free(second);return result;
}
