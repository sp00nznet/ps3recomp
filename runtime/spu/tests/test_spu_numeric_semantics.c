/* clang -std=gnu17 -I runtime/spu -I include \
 * runtime/spu/tests/test_spu_numeric_semantics.c -lm -o /tmp/spu-numeric && /tmp/spu-numeric
 * Covers SPU operations used by Yakuza's geometry and audio programs. */
#include "spu_helpers.h"
#include <math.h>
#include <stdio.h>
static int passed, failed;
#define CHECK(c) do { if(c) ++passed; else { ++failed; printf("FAIL line %d: %s\n",__LINE__,#c); } } while(0)
int main(void)
{
    u128 a = spu_zero(), r;
    a._f32[0]=1.0f; a._f32[1]=-2.75f; a._f32[2]=42.5f; a._f32[3]=1024.0f;
    r=spu_cflts(a,173);
    CHECK(r._s32[0]==1 && r._s32[1]==-2 && r._s32[2]==42 && r._s32[3]==1024);
    r=spu_cfltu(a,173);
    CHECK(r._u32[0]==1 && r._u32[1]==0 && r._u32[2]==42 && r._u32[3]==1024);
    r=spu_cflts(a,172); CHECK(r._s32[0]==2 && r._s32[1]==-5);
    a=spu_splat_u32(8); r=spu_csflt(a,155); CHECK(r._f32[0]==8.0f);
    r=spu_cuflt(a,156); CHECK(r._f32[0]==16.0f);
    r=spu_csflt(a,154); CHECK(r._f32[0]==4.0f);
    a._u32[0]=0x12345678; a._u32[1]=0x80000000;
    a._u32[2]=0xffffffff; a._u32[3]=7;
    r=spu_xswd(a);
    CHECK(r._u32[0]==0xffffffff && r._u32[1]==0x80000000 && r._u32[2]==0 && r._u32[3]==7);
    a._f32[0]=1.0f; a._f32[1]=99.0f; a._f32[2]=2.0f; a._f32[3]=-99.0f;
    r=spu_fesd(a); CHECK(spu__dget(r,0)==1.0 && spu__dget(r,1)==2.0);
    r=spu_frds(r); CHECK(r._f32[0]==1.0f && r._u32[1]==0 && r._f32[2]==2.0f && r._u32[3]==0);
    /* The estimate/FI pair feeds one Newton refinement in the guest code. */
    const float values[]={1.0f,2.0f,5.3311235f,-76.91874f};
    u128 one=spu_zero(); for(int i=0;i<4;++i) one._f32[i]=1.0f;
    for(int i=0;i<4;++i) a._f32[i]=values[i];
    r=spu_fi(a,spu_frest(a)); r=spu_fma(spu_fnms(a,r,one),r,r);
    for(int i=0;i<4;++i) CHECK(fabsf(r._f32[i]*values[i]-1.0f)<2e-6f);
    for(int i=0;i<4;++i) a._f32[i]=fabsf(values[i]);
    u128 half=one; for(int i=0;i<4;++i) half._f32[i]=0.5f;
    r=spu_fi(a,spu_frsqest(a));
    r=spu_fma(spu_fnms(spu_fm(a,r),r,one),spu_fm(r,half),r);
    for(int i=0;i<4;++i) CHECK(fabsf(r._f32[i]*sqrtf(a._f32[i])-1.0f)<2e-6f);
    printf("SPU numeric semantics: %d passed, %d failed\n",passed,failed);
    return failed?1:0;
}
