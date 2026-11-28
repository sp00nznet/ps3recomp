#ifdef NDEBUG
#error "Point sprite regressions require assertions enabled"
#endif
/* Authored HLSL through the production translator; CPU only, no game data. */
#include "../rsx_shader_spirv.h"
#include "../rsx_vp_decompiler.h"
#include "../rsx_point_sprite.h"
#include <assert.h>
#include <stdlib.h>
static u32 words[65536];
static int builtin(u32 n,u32 which) {
    for(u32 i=5;i<n;) {u32 len=words[i]>>16,op=words[i]&65535u;if(!len||i+len>n)return 0;
        if(op==71 && len==4 && words[i+2]==11 && words[i+3]==which)return 1;i+=len;}
    return 0;
}
static int input_location(u32 n,const char* name) {
    u32 id=0;
    for(u32 i=5;i<n;) {u32 len=words[i]>>16;
        if((words[i]&65535u)==5 && len>=3 && strcmp((const char*)&words[i+2],name)==0)id=words[i+1];i+=len;}
    if(!id)return -1;
    for(u32 i=5;i<n;) {u32 len=words[i]>>16;
        if((words[i]&65535u)==71 && len==4 && words[i+1]==id && words[i+2]==30)return (int)words[i+3];i+=len;}
    return -1;
}
static void compile(const char*s,int stage,const char*name,int need,u32 which) {
    u32 n=0;char log[2048]="";int rc=rsx_hlsl_to_spirv(s,stage,words,65536,&n,log,sizeof log);
    if(rc)fprintf(stderr,"%s: %s\n",name,log);assert(rc==0 && n>5);
    assert(builtin(n,which)==need);
    if(stage==RSX_SHADER_STAGE_FRAGMENT)assert(input_location(n,"input.tc0")==0);
    char path[256];snprintf(path,sizeof path,"%s.spv",name);FILE*f=fopen(path,"wb");assert(f);fwrite(words,4,n,f);fclose(f);
    printf("PASS: %s translates with expected built-in\n",name);
}
int main(void) {
    const char*vs="struct VSOutput {float4 pos:SV_Position;\n" RSX_VP_HLSL_PSIZE_MEMBER "};\n"
        "cbuffer VPConst:register(b0){float4 vp_c[512];float4 vp_posscale;float4 vp_posoffset;\n" RSX_VP_HLSL_PSIZE_ROW "};\n"
        RSX_VP_HLSL_PSIZE_FUNC
        "VSOutput main(float4 p:ATTR0){VSOutput Out;Out.pos=p;\n" RSX_VP_HLSL_PSIZE_STORE("vp_c[0].x") "return Out;}\n";
    const char*ps="struct PSInput {float4 position:SV_POSITION;float4 tc0:TEXCOORD0;float4 tc4:TEXCOORD4;};\n"
        "cbuffer PSConstants:register(b1){float4 fp_constants[1];float4 fp_alpha;};\n"
        "float4 main(PSInput input):SV_TARGET{return float4(input.tc4.xy,input.tc0.x,1);}\n";
    char h[8192];strcpy(h,ps);
    compile(vs,RSX_SHADER_STAGE_VERTEX,"point-size",1,1);
    compile(h,RSX_SHADER_STAGE_FRAGMENT,"ordinary-fragment",0,16);
    assert(rsx_point_sprite_patch(h,sizeof h,rsx_point_sprite_key(1,0x1001))==1);
    compile(h,RSX_SHADER_STAGE_FRAGMENT,"point-coordinate",1,16);
    puts("PASS: PointSize, PointCoord and ordinary fragment isolation");return 0;
}
