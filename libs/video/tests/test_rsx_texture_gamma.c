/* Source/state tests use synthetic programs and the production engine. */
#define main existing_engine_test_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_test_programs.h"
#include "rsx_fp_decompiler.h"
#include "rsx_texture_layout.h"
static char captured[65536];
static u32 capture_pipeline(void* u,const char* vs,const char* ps,const rsx_be_render_state* rs,const rsx_vertex_layout_plan* l,u32 stride,rsx_be_format f,u32 n)
{ snprintf(captured,sizeof captured,"%s",ps);return stub_pipeline_create(u,vs,ps,rs,l,stride,f,n); }
static void make_fp(u8* p,u32 opcode,u32 unit,u32 expanded)
{ rsx_test_fp_instr(p,RSX_TEST_FP_OPC(opcode)|RSX_TEST_FP_MASK_XYZW|RSX_TEST_FP_INSRC_TC0|RSX_TEST_FP_TEXU(unit)|RSX_TEST_FP_END|(expanded?1u<<21:0),RSX_TEST_FP_SRC_INPUT|RSX_TEST_FP_SWZ_IDENT); }
int main(int argc,char** argv)
{
 vm_base=calloc(1,GUEST_BYTES);ppu_vm_size=GUEST_BYTES;engine_up();rsx_draw_backend be=g_stub_backend;be.pipeline_create=capture_pipeline;rsx_draw_engine_set_backend(&be);
 u8 vp[32];rsx_test_vp_mov_out(vp,0,RSX_TEST_VP_SWZ_IDENT,0,0);rsx_test_vp_mov_out(vp+16,8,RSX_TEST_VP_SWZ_IDENT,7,1);m(0x1E9C,0);
 for(u32 i=0;i<8;i++){u32 x;memcpy(&x,vp+i*4,4);m(0xB80+i*4,x);}m(0x1EA0,0);make_fp(vm_base+0x60000,0x17,0,0);m(0x8E4,0x60002);m(0x1D60,0x40);
 bind_texture_unit(RSX_LOCATION_MAIN,TEX_OFFSET);m(M_TEX_UNIT+8,0);draw_triangle();const u32 raw=stub.bound_pipeline;char original[65536];memcpy(original,captured,sizeof original);const int uploads=stub.n_texture_upload;
 m(M_TEX_UNIT+8,7u<<20);draw_triangle();const u32 gamma=stub.bound_pipeline;
 CHECK(stub.n_pipeline_create==2&&gamma!=raw,"gamma mask selects distinct shader");CHECK(strstr(captured,"_rsx_gamma_input")!=NULL,"gamma shader decodes samples");CHECK(stub.n_texture_upload==uploads,"state transition does not mutate/upload texture bytes");CHECK(stub.bound_mask==1,"only referenced unit is bound");
 m(M_TEX_UNIT+0x20+8,15u<<20);draw_triangle();CHECK(stub.bound_pipeline==gamma&&stub.n_pipeline_create==2,"unused unit does not create helper/key/binding");
 m(M_TEX_UNIT+8,0);draw_triangle();CHECK(stub.bound_pipeline==raw&&stub.n_pipeline_create==2,"gamma disable switchback reuses raw shader");
 make_fp(vm_base+0x60000,0x17,0,1);m(M_TEX_UNIT+8,0);draw_triangle();const u32 expanded=stub.bound_pipeline;const int count=stub.n_pipeline_create;m(M_TEX_UNIT+8,7u<<20);draw_triangle();CHECK(stub.bound_pipeline==expanded&&stub.n_pipeline_create==count,"EXP_TEX overrides gamma without extra cache identity");
 const u32 formats[]={0x81,0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x8B,0x8D,0x8E,0x8F,0x97,0x9D,0x9E};
 for(u32 f=0;f<sizeof formats/sizeof formats[0];++f)for(u32 mask=0;mask<16;++mask)CHECK(rsx_texture_gamma_mask(formats[f],0xAAE4,0,mask<<20)==mask,"supported normalized formats retain final lane mask");
 const u32 bypass[]={0x90,0x91,0x92,0x93,0x94,0x95,0x98,0x99,0x9A,0x9B,0x9C,0x9F,0x80};
 for(u32 f=0;f<sizeof bypass/sizeof bypass[0];++f)CHECK(!rsx_texture_gamma_mask(bypass[f],0xAAE4,0,15u<<20),"float/depth/non-gamma format bypasses");
 CHECK(rsx_texture_gamma_mask(0x85,0xAAE4,1u<<29,15u<<20)==14,"signed source R wins over gamma R");
 CHECK(rsx_texture_gamma_mask(0x85,0xAAE4,1u<<28,15u<<20)==7,"signed source A wins over gamma A");
 CHECK(rsx_texture_gamma_mask(0x85,0xAA1B,1u<<28,15u<<20)==11,"signed source follows remap while gamma names final B");
 CHECK(rsx_texture_gamma_mask(0x85,0,0,7u<<20)==7,"unset crossbar retains default identity");
 u8 fp[32];char hlsl[65536],legacy[65536];u8 masks[16]={0};u32 constants=0;
 for(u32 unit=0;unit<16;unit+=15)for(u32 op=0;op<4;++op)for(u32 mask=0;mask<16;++mask){const u32 ops[]={0x17,0x18,0x31,0x2F};make_fp(fp,ops[op],unit,0);memset(masks,0,sizeof masks);masks[unit]=(u8)mask;
 CHECK(rsx_fp_decompile_buffered_color_ex(fp,16,0x40,0,masks,hlsl,sizeof hlsl,&constants)>0,"supported texture opcode decompiles with channel mask");
 CHECK((strstr(hlsl,"float4 _rsx_gamma_input")!=NULL)==(mask!=0),"helper emitted only for active fetch");
 if(mask==0){CHECK(rsx_fp_decompile_buffered_ex(fp,16,0x40,0,legacy,sizeof legacy,&constants)>0&&!strcmp(legacy,hlsl),"zero metadata preserves original source byte for byte");}
 if(argc==2){char path[1024];snprintf(path,sizeof path,"%s/u%u-op%u-mask%u.hlsl",argv[1],unit,op,mask);FILE* f=fopen(path,"wb");if(f){fputs(hlsl,f);fclose(f);}}
 }
 masks[0]=7;const u32 unsupported[]={0x19,0x33,0x34};for(u32 i=0;i<3;++i){make_fp(fp,unsupported[i],0,0);CHECK(rsx_fp_decompile_buffered_color_ex(fp,16,0x40,0,masks,hlsl,sizeof hlsl,&constants)<0,"active unsupported fetch fails explicitly");}
 make_fp(fp,0x17,0,0);rsx_test_fp_put_word(fp,(RSX_TEST_FP_OPC(0x17)|RSX_TEST_FP_MASK_XYZW|RSX_TEST_FP_INSRC_TC0));make_fp(fp+16,0x17,0,1);
 CHECK(rsx_fp_decompile_buffered_color_ex(fp,32,0x40,0,masks,hlsl,sizeof hlsl,&constants)>0,"mixed ordinary/expanded same-unit fetch decompiles");
 CHECK(rsx_fp_texture_gamma_units(fp,32,masks)==1,"ordinary fetch retains gamma when same unit also expands");
 engine_down();free(vm_base);printf("texture gamma: %d failures\n",g_failures);return g_failures?1:0;
}
