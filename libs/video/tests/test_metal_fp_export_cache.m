#include "rsx_metal_backend.m"
#include "rsx_test_programs.h"
#include <stdio.h>
u8* vm_base;
u32 cellGcmResolveLocated(int local,u32 off){(void)local;return off;}
static int failures,checks;
#define CHECK(x,n) do{checks++;if(!(x)){failures++;fprintf(stderr,"FAIL %s\n",n);}}while(0)
int main(void){@autoreleasepool{
    s_dev=MTLCreateSystemDefaultDevice();CHECK(s_dev!=nil,"Metal device");if(!s_dev)return 2;
    vm_base=calloc(1,4096);if(!vm_base)return 2;
    rsx_test_fp_mov_r0_col0(vm_base+256,RSX_TEST_FP_SWZ_IDENT);
    rsx_state st;memset(&st,0,sizeof st);st.shader_program=257;
    const u8*uc=NULL;const u32 ctrl[4]={0,14,64,78};int slot[4];
    for(int i=0;i<4;i++){
        st.shader_control=ctrl[i];slot[i]=fp_slot_for(&st,&uc,0);
        CHECK(slot[i]>=0,"actual Metal guest fragment compiles");
        CHECK(s_fp_count==(u32)i+1,"depth/width controls select distinct actual Metal cache slots");
        int again=fp_slot_for(&st,&uc,0);CHECK(again==slot[i] && s_fp_count==(u32)i+1,"unchanged shader control reuses cached Metal function");
    }
    CHECK(slot[0]!=slot[1] && slot[2]!=slot[3],"depth export separates both colour widths");
    st.shader_control=0;CHECK(fp_slot_for(&st,&uc,0)==slot[0] && s_fp_count==4,"switch back uses original no-export function");
    for(u32 i=0;i<s_fp_count;i++)s_fp_cache[i].fn=nil;s_fp_count=0;free(vm_base);vm_base=NULL;s_dev=nil;
    printf("Metal export cache: %d checks, %d failures\n",checks,failures);return failures?1:0;
}}
