#include "../libs/video/rsx_commands.h"
#include "../libs/video/rsx_fp_decompiler.h"
#include "../libs/video/rsx_shader_window.h"
#include "folklore_wpos_program.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
uint32_t ppu_hle_inject_base;
void vm_write32(uint32_t a,uint32_t v) { (void)a;(void)v; }
void ps3_ms(const char* key) { (void)key; }
static u32 observed;
static void shader(void* user,const rsx_state* state) { (void)user;observed=state->shader_window; }
int main(int argc,char** argv)
{
    rsx_state state;rsx_state_init(&state);
    if (argc==2 && !strcmp(argv[1],"bad-register")) {
        rsx_process_method(&state,NV4097_SET_SHADER_WINDOW,0x2000u);return 99;
    }
    static const struct {u32 raw;float x,y;const char* expression;} cases[]={
      {0x2D0,0.5f,0.5f,NULL},
      {0x12D0,0.5f,719.5f,"float2(1.0, -1.0) + float2(0.0, 720.0)"},
      {0x102D0,0.0f,0.0f,"float2(1.0, 1.0) + float2(-0.5, -0.5)"},
      {0x112D0,0.0f,719.0f,"float2(1.0, -1.0) + float2(-0.5, 719.5)"},
      {0x12CF,0.5f,718.5f,"float2(1.0, -1.0) + float2(0.0, 719.0)"},
      {0x1FFF,0.5f,4094.5f,"float2(1.0, -1.0) + float2(0.0, 4095.0)"}};
    u8 program[48]={0};folklore_wpos_program(program);
    rsx_backend backend={0};backend.set_shader=shader;rsx_set_backend(&backend);
    for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        rsx_shader_window w;assert(rsx_shader_window_decode(cases[i].raw,&w)==0);
        assert(0.5f+w.x_bias==cases[i].x);
        assert(0.5f*w.y_scale+w.y_bias==cases[i].y);
        assert(rsx_process_method(&state,NV4097_SET_SHADER_WINDOW,cases[i].raw)==0);
        assert(state.shader_dirty && state.shader_window==cases[i].raw);
        assert(rsx_process_method(&state,NV4097_SET_BEGIN_END,5)==0);
        assert(observed==cases[i].raw && !state.shader_dirty);
        char hlsl[32768];assert(rsx_fp_decompile(program,sizeof(program),0x40,hlsl,sizeof(hlsl))==2);
        assert(strstr(hlsl,"input.position") && strstr(hlsl,"frac("));
        int status=rsx_fp_apply_shader_window(hlsl,sizeof(hlsl),cases[i].raw);
        assert(status==(cases[i].expression?1:0));
        if (cases[i].expression) {
            assert(strstr(hlsl,cases[i].expression));
            char original[32768];memcpy(original,hlsl,sizeof(hlsl));
            assert(rsx_fp_apply_shader_window(hlsl,sizeof(hlsl),cases[i].raw)==-1);
            assert(memcmp(original,hlsl,sizeof(hlsl))==0);
        }
        if (argc==2 && !strcmp(argv[1],"dump")) {
            char name[64];snprintf(name,sizeof name,"window-%08X.hlsl",cases[i].raw);
            FILE* f=fopen(name,"wb");assert(f);assert(fwrite(hlsl,1,strlen(hlsl),f)==strlen(hlsl));fclose(f);
        }
    }
    rsx_shader_window w={3,4,5};
    for (u32 raw=0x2000;raw!=0;raw<<=1) {
        if (raw==0x10000)continue;
        assert(rsx_shader_window_decode(raw,&w)==-1 && w.y_scale==3 && w.x_bias==4 && w.y_bias==5);
    }
    assert(rsx_shader_window_decode(0,NULL)==-1);
    char original[]="float4 main(PSInput input) : SV_TARGET { return input.position; }";
    char copy[256];strcpy(copy,original);
    assert(rsx_fp_apply_shader_window(copy,(u32)strlen(copy)+1,0x12D0)==-1 && !strcmp(copy,original));
    assert(rsx_fp_apply_shader_window(copy,sizeof copy,0x2000)==-1 && !strcmp(copy,original));
    assert(rsx_fp_apply_shader_window(NULL,0,0)==-1);
    char unterminated[8]={'a','a','a','a','a','a','a','a'};
    assert(rsx_fp_apply_shader_window(unterminated,sizeof unterminated,0)==-1);
    char no_main[]="float4 other() {return 0;}";
    assert(rsx_fp_apply_shader_window(no_main,sizeof no_main,0x12D0)==-1);
    puts("Shader-window register, coordinates and HLSL boundary tests passed");return 0;
}
