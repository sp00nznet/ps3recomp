#define main old_engine_tests_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_test_programs.h"
int main(void){
    vm_base=calloc(1,GUEST_BYTES);if(!vm_base)return 2;ppu_vm_size=GUEST_BYTES;
    for(u32 i=0;i<12;i++){u8*p=vm_base+VTX_OFFSET+i*4;p[0]=0x3F;p[1]=0x80;}
    engine_up();u8 vp[16];rsx_test_vp_mov_out(vp,0,RSX_TEST_VP_SWZ_IDENT,0,1);
    m(0x1E9C,0);for(u32 i=0;i<4;i++){u32 w;memcpy(&w,vp+i*4,4);m(0xB80+i*4,w);}m(0x1EA0,0);
    rsx_test_fp_mov_r0_col0(vm_base+TEX_OFFSET,RSX_TEST_FP_SWZ_IDENT);m(0x8E4,TEX_OFFSET|2u);
    const u32 ctrl[4]={0,14,64,78};u32 handle[4];
    for(int i=0;i<4;i++){
        m(0x1D60,ctrl[i]);draw_triangle();handle[i]=stub.bound_pipeline;
        CHECK(stub.n_pipeline_create==i+1,"width/depth control %X has distinct actual pipeline (%d)",ctrl[i],stub.n_pipeline_create);
        draw_triangle();CHECK(stub.n_pipeline_create==i+1,"unchanged export control reuses actual pipeline");
    }
    CHECK(handle[0]!=handle[1] && handle[2]!=handle[3],"depth-export toggles distinct pipeline within same colour width");
    m(0x1D60,0);draw_triangle();CHECK(stub.n_pipeline_create==4 && stub.bound_pipeline==handle[0],"switch back reuses correct disabled-depth pipeline");
    CHECK(stub.n_draw==9,"all synthetic guest-program draws emitted");
    engine_down();free(vm_base);printf("export cache failures: %d\n",g_failures);return g_failures?1:0;
}
