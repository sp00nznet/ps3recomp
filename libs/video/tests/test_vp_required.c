/* Required instruction rejection, synthetic NV40 words only. */
#include "../rsx_vp_decompiler.h"
#include <stdio.h>

static void word(u8* out,u32 v)
{
    out[0]=(u8)v;out[1]=(u8)(v>>8);out[2]=(u8)(v>>16);out[3]=(u8)(v>>24);
}

int main(void)
{
    static char hlsl[131072];
    u8 code[16]={0};
    int failed=0;
    const u32 d1[]={0u,1u<<22,31u<<22,31u<<27,0x11u<<27};
    const u32 d3[]={1u,(1u<<16)|1u,(1u<<16)|1u,(1u<<20)|1u,1u};
    const int supported[]={1,1,0,0,0};
    for(unsigned i=0;i<5;i++) {
        word(code+4,d1[i]);word(code+12,d3[i]);
        const int result=rsx_vp_decompile(code,sizeof code,hlsl,sizeof hlsl);
        if((result>0)!=supported[i]) {
            fprintf(stderr,"case%u supported=%d result=%d\n",i,supported[i],result);failed++;
        }
    }
    printf("Required VP instruction checks: %u passed, %d failed\n",5-failed,failed);
    return failed ? 1 : 0;
}
