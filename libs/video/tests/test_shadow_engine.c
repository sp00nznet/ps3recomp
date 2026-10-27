/* Synthetic production draw-engine shadow state/cache test; no title data. */
#define main old_engine_tests_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_test_programs.h"
static char shadow_ps[65536];
static u32 capture_pipeline(void* u,const char* vs,const char* ps,const rsx_be_render_state* rs,const rsx_vertex_layout_plan* l,u32 stride,rsx_be_format f,u32 n){snprintf(shadow_ps,sizeof shadow_ps,"%s",ps);return stub_pipeline_create(u,vs,ps,rs,l,stride,f,n);}
static void texture(u32 unit,u32 format,u32 dim,u32 mips,u32 func,u32 filter,u32 ctl){u32 b=M_TEX_UNIT+unit*32;m(b,0x60000+unit*0x1000);m(b+4,2|(dim<<4)|(format<<8)|(mips<<16));m(b+8,(func<<28)|0x30303);m(b+12,ctl);m(b+16,0xAAE4);m(b+20,filter);m(b+24,0x40004);}
int main(void){vm_base=calloc(1,GUEST_BYTES);if(!vm_base)return 2;ppu_vm_size=GUEST_BYTES;engine_up();rsx_draw_backend be=g_stub_backend;be.pipeline_create=capture_pipeline;rsx_draw_engine_set_backend(&be);
 u8 vp[16];rsx_test_vp_mov_out(vp,0,RSX_TEST_VP_SWZ_IDENT,0,1);m(0x1E9C,0);for(u32 i=0;i<4;i++){u32 w;memcpy(&w,vp+i*4,4);m(0xB80+i*4,w);}m(0x1EA0,0);
 rsx_test_fp_instr(vm_base+TEX_OFFSET,RSX_TEST_FP_OPC(0x18)|RSX_TEST_FP_MASK_XYZW|RSX_TEST_FP_INSRC_TC0|RSX_TEST_FP_END,RSX_TEST_FP_SRC_INPUT|RSX_TEST_FP_SWZ_IDENT);m(0x8E4,TEX_OFFSET|2);m(0x1D60,64);
 texture(0,0xB0,2,1,0,0x01010000,0x80000000);draw_triangle();u32 raw=stub.bound_pipeline;CHECK(stub.n_pipeline_create==1,"raw endpoint starts one shader");CHECK(!strstr(shadow_ps,"_rsx_shadow"),"endpoint leaves ordinary HLSL");
 u32 h[8]={raw};for(u32 f=1;f<7;f++){texture(0,0xB0,2,1,f,0x01010000,0x80000000);draw_triangle();h[f]=stub.bound_pipeline;CHECK(stub.n_pipeline_create==(int)f+1,"comparison %u creates distinct shader",f);CHECK(strstr(shadow_ps,"_rsx_shadow_cmp0")!=NULL,"comparison helper is emitted");CHECK(strstr(shadow_ps,".xyz /")!=NULL,"TXP divides reference as well as coordinates");CHECK(stub.bound_mask==1,"only source sampled unit is bound");draw_triangle();CHECK(stub.bound_pipeline==h[f]&&stub.n_pipeline_create==(int)f+1,"unchanged comparison reuses shader");}
 texture(0,0xB0,2,1,7,0x01010000,0x80000000);draw_triangle();CHECK(stub.bound_pipeline==raw&&stub.n_pipeline_create==7,"second raw endpoint reuses ordinary shader");texture(0,0xB0,2,1,3,0x01010000,0x80000000);draw_triangle();CHECK(stub.bound_pipeline==h[3]&&stub.n_pipeline_create==7,"switchback selects correct comparison shader");
 texture(1,0xB0,2,1,4,0x02020000,0x80000000);draw_triangle();CHECK(stub.n_pipeline_create==7&&stub.bound_mask==1,"unused shadow unit creates no helper or binding");
 texture(1,0xF0,2,1,4,0x02020000,0x80000000);draw_triangle();CHECK(stub.n_pipeline_create==7&&stub.bound_mask==1,"unused active UN unit leaves shader cache and bindings unchanged");
 texture(0,0xB0,2,1,3,0x02020000,0x80000000);draw_triangle();u32 linear=stub.bound_pipeline;CHECK(stub.n_pipeline_create==8&&linear!=h[3],"filter specialization participates in cache");CHECK(strstr(shadow_ps,"lerp(lerp(c0")!=NULL,"compared taps are filtered");
 texture(0,0xF0,2,1,3,0x02020000,0x80000000);draw_triangle();CHECK(stub.n_pipeline_create==9&&strstr(shadow_ps,"coordinate.xy / size"),"UN coordinates specialize and use actual texture dimensions");
 u32 un=stub.bound_pipeline;m(M_TEX_UNIT+24,0x80008);draw_triangle();CHECK(stub.n_pipeline_create==9&&stub.bound_pipeline==un,"shadow UN dimensions are runtime resource state, not shader identity");
 texture(0,0xB0,2,1,3,0x01010000,0x80000004);draw_triangle();CHECK(stub.bound_pipeline==raw&&stub.n_pipeline_create==9,"alpha-kill disables comparison without changing ordinary shader");
 int n=stub.n_draw;texture(0,0xB0,2,2,3,0x01010000,0x80000000);draw_triangle();CHECK(stub.n_draw==n&&stub.n_pipeline_create==9,"unsupported mip comparison rejects draw before backend emit");draw_triangle();CHECK(stub.n_draw==n,"cached failed shader cannot emit fallback");
 texture(0,0xB0,3,1,3,0x01010000,0x80000000);draw_triangle();CHECK(stub.n_draw==n,"unsupported 3D comparison cannot emit fallback");texture(0,0xB0,2,1,3,0x02010000,0x80000000);draw_triangle();CHECK(stub.n_draw==n,"mixed min/mag filtering cannot emit fallback");texture(0,0xB0,2,1,3,0x01010000,0x80000000);draw_triangle();CHECK(stub.n_draw==n+1&&stub.bound_pipeline==h[3],"valid cache switchback survives rejected draws");
 engine_down();free(vm_base);printf("shadow engine failures: %d\n",g_failures);return g_failures?1:0;}
