#ifdef NDEBUG
#error "Point sprite regressions require assertions enabled"
#endif
#include "../rsx_point_sprite.h"
#include <assert.h>
int main(void) {
    const char* base = "struct PSInput { float4 tc0:TEXCOORD0; float4 tc4:TEXCOORD4; };\ncbuffer PSConstants:register(b1){float4 fp_constants[1]; float4 fp_alpha;};\nfloat4 main(PSInput input):SV_TARGET{return input.tc4+input.tc0;}\n";
    char h[4096], old[4096];
    assert(rsx_point_sprite_key(1,0x1001)==0x40000010u);
    assert(rsx_point_sprite_key(6,0x1001)==0);
    assert(rsx_point_sprite_key(1,0x1000)==0x40000000u);
    strcpy(h,base); assert(rsx_point_sprite_patch(h,sizeof h,0x40000010)==1);
    assert(strstr(h,"input.tc4 = float4(_rsx_sprite_uv"));
    assert(strstr(h,"tc4:TEXCOORD4") < strstr(h,"_rsx_sprite_point"));
    assert(!strstr(h,"input.tc0 = float4(_rsx_sprite_uv"));
    strcpy(old,h);assert(rsx_point_sprite_patch(h,sizeof h,0x40000010)==0 && !strcmp(h,old));
    strcpy(h,base);strcpy(old,h);assert(rsx_point_sprite_patch(h,strlen(h)+1,0x40000010)==-1 && !strcmp(h,old));
    assert(rsx_point_sprite_patch(h,sizeof h,0)==0 && !strcmp(h,old));
    float xy[2];assert(rsx_point_sprite_grid(2,1,xy)==2 && xy[0]==1 && xy[1]==0.5f);
    assert(rsx_point_sprite_grid(1,2,xy)==2 && xy[0]==0.5f && xy[1]==1);
    assert(rsx_point_sprite_grid(0,0,xy)==1 && xy[0]==1 && xy[1]==1);
    puts("PASS: POINTS identity, selected coordinate replacement, AA grid, disabled path, atomic capacity and idempotence");
    return 0;
}
