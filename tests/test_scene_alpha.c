/* CPU-only scalar contract qualification. No Vulkan dependency or device. */
#include "../libs/video/rsx_scene_alpha.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); return 0; } } while(0)
typedef struct fixture_contract {
    const char* hash;
    unsigned constants,kind,scale,decode,q,q_component,power_base,power_exponent;
    unsigned attenuation,mix,inner,inner_component,base,lower,upper;
} fixture_contract;
/* Independent fixture setup identifies the exact components that must matter.
 * Expected scalar results below are fixed analytic binary fractions, not a
 * second copy of the implementation's table-driven expression evaluator. */
static const fixture_contract cases[]={
    {"84d877ddc6083545452948630259c21ca796e7985fcd5e7b5a005666e4a0d634",15,0,11,0,0,0,0,0,0,0,0,0,0,0,0},
    {"adc0a1a4e6f5204131f3e40be1ec23ed10284e7100ceb80705fb0c917689a78d",12,0,9,0,0,0,0,0,0,0,0,0,0,0,0},
    {"bc1839eec9a6aebf2e0b5b860740e7368411aca1429e6a730c4a044685d9f0b6",43,1,42,0,12,0,30,33,34,35,37,3,39,40,41},
    {"4d7f89f750d67445d700f6edd5715c8ab59996a12618e645a9b86c42a3f48c63",41,1,40,2,16,0,30,32,33,34,35,0,37,38,39},
    {"65fbf5c251ee842a32bc38e185405f37c6f4c0ae001b7bbba3a8a832c6a16c38",35,1,34,0,2,1,21,25,27,28,29,0,31,32,33},
    {"bf9b5d7c6a2e73f4d9639259a0322518da9cc051a6928df898bb4ef65fa720ac",38,1,37,0,6,0,27,28,30,31,32,0,33,35,36},
    {"fa93e0db862af8fbd755b7270649e6f870c9ff17c4e6f627f4ede946cf550e36",37,1,36,0,8,0,26,27,29,30,32,0,33,34,35},
    {"4cf8870c89518ec8a342062570bd75a11149bbfd69a48d6b13df4ebe90b0442a",23,2,22,0,0,0,0,0,0,0,0,0,0,20,21}
};
typedef struct fixture {
    const fixture_contract* desc;
    const rsx_scene_alpha_contract* contract;
    rsx_scene_vertex_output v;
    float fp[44][4],lod[16],samples[2][4];
    unsigned calls,units[4];float seen_uv[4][2],seen_lod[4];int fail_unit;
} fixture;
static int sample(void* opaque,unsigned unit,float u,float v,float lod,float rgba[4])
{
    fixture* f=(fixture*)opaque;
    if(unit>1 || f->calls>=4)return 0;
    unsigned call=f->calls++;f->units[call]=unit;
    f->seen_uv[call][0]=u;f->seen_uv[call][1]=v;f->seen_lod[call]=lod;
    if((int)unit==f->fail_unit)return 0;
    memcpy(rgba,f->samples[unit],sizeof f->samples[unit]);return 1;
}
static void setup(fixture* f,const fixture_contract* d)
{
    memset(f,0,sizeof *f);f->desc=d;f->contract=rsx_scene_alpha_contract_for_sha256(d->hash);f->fail_unit=-1;
    /* All unused channels/constants deliberately remain NaN. */
    memset(&f->v,0xff,sizeof f->v);
    for(unsigned i=0;i<44;++i)for(unsigned k=0;k<4;++k)f->fp[i][k]=NAN;
    for(unsigned i=0;i<16;++i)f->lod[i]=NAN;
    for(unsigned i=0;i<2;++i)for(unsigned k=0;k<4;++k)f->samples[i][k]=NAN;
    f->v.col0[3]=.5f;f->v.t0[0]=.125f;f->v.t0[1]=.375f;
    f->fp[d->constants][0]=0;f->fp[d->scale][0]=d->kind?.5f:2;
    if(!d->kind){f->samples[0][3]=.25f;f->lod[0]=-.25f;return;}
    f->v.t0[2]=.625f;f->v.t0[3]=.875f;
    f->v.t4[0]=f->v.t4[1]=0;f->v.t4[2]=1;
    f->v.t3[0]=f->v.t3[1]=0;f->v.t3[2]=1;
    f->samples[0][0]=f->samples[0][1]=.5f;f->samples[0][2]=1;
    f->samples[1][3]=.25f;f->lod[0]=-.25f;f->lod[1]=1.75f;
    f->fp[d->decode][0]=2;f->fp[d->decode][1]=-1;
    if(d->kind==2){
        f->fp[3][3]=.25f;f->fp[4][0]=.5f;f->fp[6][2]=.25f;
        f->fp[19][3]=.5f;f->fp[20][0]=.125f;f->fp[21][0]=2;return;
    }
    f->fp[d->q][d->q_component]=1.25f;
    f->fp[d->power_base][0]=.375f;f->fp[d->power_base][1]=.5f;
    f->fp[d->power_exponent][2]=2;
    f->fp[d->attenuation][3]=.25f;f->fp[d->mix][3]=2;
    f->fp[d->inner][d->inner_component]=.4375f;
    f->fp[d->base][3]=.5f;f->fp[d->lower][0]=.125f;f->fp[d->upper][0]=.75f;
}
static rsx_scene_alpha_status evaluate(fixture* f,rsx_scene_alpha_result* result)
{
    f->calls=0;
    return rsx_scene_alpha_evaluate(f->contract,&f->v,f->fp,(size_t)(f->desc->constants+1)*16,
                                   f->lod,sample,f,result);
}
static int expected(fixture* f,float alpha,int keep)
{
    rsx_scene_alpha_result result;
    CHECK(evaluate(f,&result)==RSX_SCENE_ALPHA_OK);
    CHECK(fabsf(result.alpha-alpha)<.000001f && result.keep==keep);
    CHECK(result.reference==f->fp[f->desc->constants][0]);
    unsigned count=f->desc->kind?2:1;CHECK(f->calls==count);
    for(unsigned i=0;i<count;++i){
        unsigned unit=f->units[i];
        CHECK(f->seen_lod[i]==f->lod[unit]);
        unsigned channel=f->desc->kind && unit==0?2:0;
        CHECK(f->seen_uv[i][0]==f->v.t0[channel] && f->seen_uv[i][1]==f->v.t0[channel+1]);
    }
    return 1;
}
static int failed_unchanged(fixture* f,rsx_scene_alpha_status status)
{
    rsx_scene_alpha_result result,saved;memset(&saved,0x5a,sizeof saved);memcpy(&result,&saved,sizeof result);
    CHECK(evaluate(f,&result)==status);CHECK(!memcmp(&result,&saved,sizeof result));return 1;
}
static int lookup_checks(void)
{
    CHECK(!rsx_scene_alpha_contract_for_sha256(NULL));
    CHECK(!rsx_scene_alpha_contract_for_sha256(""));
    CHECK(!rsx_scene_alpha_contract_for_sha256("84d877dd"));
    CHECK(!rsx_scene_alpha_contract_for_sha256("0000000000000000000000000000000000000000000000000000000000000000"));
    char hash[66];memcpy(hash,cases[0].hash,65);hash[12]='g';CHECK(!rsx_scene_alpha_contract_for_sha256(hash));
    memcpy(hash,cases[0].hash,65);hash[64]='0';hash[65]=0;CHECK(!rsx_scene_alpha_contract_for_sha256(hash));
    for(size_t i=0;i<sizeof cases/sizeof cases[0];++i){
        const rsx_scene_alpha_contract* c=rsx_scene_alpha_contract_for_sha256(cases[i].hash);CHECK(c);
        CHECK(!strcmp(rsx_scene_alpha_contract_sha256(c),cases[i].hash));
        CHECK(rsx_scene_alpha_required_fp_bytes(c)==(size_t)(cases[i].constants+1)*16);
        CHECK(rsx_scene_alpha_texture_mask(c)==(cases[i].kind?3u:1u));
        memcpy(hash,cases[i].hash,65);
        for(unsigned k=0;k<64;++k)if(hash[k]>='a' && hash[k]<='f')hash[k]=(char)(hash[k]-('a'-'A'));
        CHECK(rsx_scene_alpha_contract_for_sha256(hash)==c);
    }
    const rsx_scene_alpha_contract* fake=(const rsx_scene_alpha_contract*)(uintptr_t)1;
    CHECK(!rsx_scene_alpha_contract_sha256(fake));CHECK(!rsx_scene_alpha_required_fp_bytes(fake));
    CHECK(!rsx_scene_alpha_texture_mask(fake));CHECK(!rsx_scene_alpha_contract_sha256(NULL));
    CHECK(rsx_scene_alpha_evaluate(fake,NULL,NULL,0,NULL,NULL,NULL,NULL)==RSX_SCENE_ALPHA_UNSUPPORTED);
    return 1;
}
static int simple_checks(void)
{
    for(unsigned i=0;i<2;++i){
        fixture f;setup(&f,&cases[i]);CHECK(expected(&f,.5f,1));
        f.fp[f.desc->constants][0]=.5f;CHECK(expected(&f,.5f,0));
        f.fp[f.desc->constants][0]=nextafterf(.5f,-INFINITY);CHECK(expected(&f,.5f,1));
        f.fp[f.desc->constants][0]=nextafterf(.5f,INFINITY);CHECK(expected(&f,.5f,0));
        f.fp[f.desc->constants][0]=NAN;rsx_scene_alpha_result r;
        CHECK(evaluate(&f,&r)==RSX_SCENE_ALPHA_OK && r.alpha==.5f && !r.keep && isnan(r.reference));
        f.fp[f.desc->constants][0]=0;f.samples[0][3]=NAN;
        CHECK(evaluate(&f,&r)==RSX_SCENE_ALPHA_OK && isnan(r.raw_alpha) && r.alpha==0 && !r.keep);
        f.samples[0][3]=.25f;f.v.col0[3]=NAN;
        CHECK(evaluate(&f,&r)==RSX_SCENE_ALPHA_OK && isnan(r.raw_alpha) && r.alpha==0 && !r.keep);
        f.v.col0[3]=.5f;f.fp[f.desc->scale][0]=NAN;
        CHECK(evaluate(&f,&r)==RSX_SCENE_ALPHA_OK && isnan(r.raw_alpha) && r.alpha==0 && !r.keep);
        setup(&f,&cases[i]);f.samples[0][3]=INFINITY;
        CHECK(evaluate(&f,&r)==RSX_SCENE_ALPHA_OK && isinf(r.alpha) && r.alpha>0 && r.keep);
        f.samples[0][3]=-INFINITY;
        CHECK(evaluate(&f,&r)==RSX_SCENE_ALPHA_OK && isinf(r.alpha) && r.alpha<0 && !r.keep);
        f.samples[0][3]=INFINITY;f.v.col0[3]=0;
        CHECK(evaluate(&f,&r)==RSX_SCENE_ALPHA_OK && isnan(r.raw_alpha) && r.alpha==0 && !r.keep);
        setup(&f,&cases[i]);f.fp[f.desc->scale][0]=-1;CHECK(expected(&f,-.25f,0));
        f.fail_unit=0;CHECK(failed_unchanged(&f,RSX_SCENE_ALPHA_SAMPLE_UNAVAILABLE));
    }
    return 1;
}
static int fresnel_checks(void)
{
    for(unsigned i=2;i<7;++i){
        fixture f;const fixture_contract* d=&cases[i];setup(&f,d);
        /* A=.25, F=.25, product=.0625, sum=.5, attenuated=.375,
         * mixed=.5, inner=.4375, base added=.5625, final alpha=.28125. */
        CHECK(expected(&f,.28125f,1));
        f.fp[d->constants][0]=.28125f;CHECK(expected(&f,.28125f,0));
        f.fp[d->constants][0]=nextafterf(.28125f,-INFINITY);CHECK(expected(&f,.28125f,1));
        f.fp[d->constants][0]=nextafterf(.28125f,INFINITY);CHECK(expected(&f,.28125f,0));
        f.fp[d->constants][0]=0;
        f.fp[d->lower][0]=.625f;CHECK(expected(&f,.3125f,1));
        f.fp[d->lower][0]=.125f;f.fp[d->upper][0]=.5f;CHECK(expected(&f,.25f,1));
        f.fp[d->upper][0]=.75f;f.fp[d->inner][d->inner_component]=1;CHECK(expected(&f,.3125f,1));
        /* Remove the clamps that would otherwise mask the normal dependence. */
        f.fp[d->inner][d->inner_component]=10;f.fp[d->base][3]=0;
        f.fp[d->lower][0]=-100;f.fp[d->upper][0]=100;
        f.fp[d->scale][0]=1;f.fp[d->attenuation][3]=0;f.fp[d->mix][3]=1;
        CHECK(expected(&f,.5625f,1));
        f.v.t4[1]=1;f.v.t4[2]=0;CHECK(expected(&f,1.20703125f,1));
        f.v.t4[1]=0;f.v.t4[2]=1;
        f.samples[0][0]=1;f.samples[0][2]=.5f;CHECK(expected(&f,1.20703125f,1));
        /* Saturation below zero: F0=.375, F=.140625. */
        setup(&f,d);f.fp[d->inner][d->inner_component]=10;f.fp[d->base][3]=0;
        f.fp[d->lower][0]=-100;f.fp[d->upper][0]=100;f.fp[d->scale][0]=1;
        f.fp[d->attenuation][3]=0;f.fp[d->mix][3]=1;f.fp[d->q][d->q_component]=.5f;
        CHECK(expected(&f,.42578125f,1));
        /* All unused channels are already NaN; a final scale NaN is sanitized. */
        setup(&f,d);f.fp[d->scale][0]=NAN;rsx_scene_alpha_result r;
        CHECK(evaluate(&f,&r)==RSX_SCENE_ALPHA_OK && isnan(r.raw_alpha) && r.alpha==0 && !r.keep);
        setup(&f,d);f.fp[d->scale][0]=INFINITY;
        CHECK(evaluate(&f,&r)==RSX_SCENE_ALPHA_OK && isinf(r.alpha) && r.keep);
        setup(&f,d);f.fp[d->constants][0]=NAN;
        CHECK(evaluate(&f,&r)==RSX_SCENE_ALPHA_OK && r.alpha==.28125f && !r.keep);
        setup(&f,d);f.fp[d->power_base][0]=-1;f.fp[d->power_base][1]=0;
        CHECK(failed_unchanged(&f,RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED));
        f.fp[d->power_base][0]=0;CHECK(failed_unchanged(&f,RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED));
        setup(&f,d);f.fp[d->power_exponent][2]=NAN;CHECK(failed_unchanged(&f,RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED));
        setup(&f,d);f.fp[d->inner][d->inner_component]=NAN;CHECK(failed_unchanged(&f,RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED));
        setup(&f,d);f.samples[0][2]=.5f;CHECK(failed_unchanged(&f,RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED));
        setup(&f,d);f.v.t4[2]=0;CHECK(failed_unchanged(&f,RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED));
        setup(&f,d);f.samples[1][3]=NAN;CHECK(failed_unchanged(&f,RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED));
        setup(&f,d);f.fail_unit=0;CHECK(failed_unchanged(&f,RSX_SCENE_ALPHA_SAMPLE_UNAVAILABLE));
        f.fail_unit=1;CHECK(failed_unchanged(&f,RSX_SCENE_ALPHA_SAMPLE_UNAVAILABLE));
    }
    return 1;
}
static int light_checks(void)
{
    fixture f;setup(&f,&cases[7]);
    /* N=V=L=(0,0,1), A=.25, K=2.5, R=.75, F=1.875,
     * raw=((.25*(-1.875)+.25)+1.875+.25*.5)*.5=.890625. */
    CHECK(expected(&f,.890625f,1));
    f.fp[23][0]=.890625f;CHECK(expected(&f,.890625f,0));
    f.fp[23][0]=nextafterf(.890625f,-INFINITY);CHECK(expected(&f,.890625f,1));
    f.fp[23][0]=0;f.v.t3[2]=-1;CHECK(expected(&f,.0625f,1));
    f.v.t3[2]=1;f.samples[0][0]=1;f.samples[0][2]=.5f;CHECK(expected(&f,.28125f,1));
    setup(&f,&cases[7]);f.fp[21][0]=1;CHECK(expected(&f,.5f,1));
    f.fp[20][0]=2;f.fp[21][0]=3;CHECK(expected(&f,1,1));
    f.fp[21][0]=1;CHECK(expected(&f,.5f,1)); /* Ordered max then min even if bounds invert. */
    setup(&f,&cases[7]);f.fp[22][0]=NAN;rsx_scene_alpha_result r;
    CHECK(evaluate(&f,&r)==RSX_SCENE_ALPHA_OK && isnan(r.raw_alpha) && r.alpha==0 && !r.keep);
    setup(&f,&cases[7]);f.v.t3[2]=0;CHECK(failed_unchanged(&f,RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED));
    setup(&f,&cases[7]);f.fp[3][3]=NAN;CHECK(failed_unchanged(&f,RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED));
    setup(&f,&cases[7]);f.samples[1][3]=NAN;CHECK(failed_unchanged(&f,RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED));
    return 1;
}
static int interface_checks(void)
{
    fixture f;setup(&f,&cases[2]);rsx_scene_alpha_result r,saved;
    memset(&saved,0x5a,sizeof saved);memcpy(&r,&saved,sizeof r);
    size_t bytes=rsx_scene_alpha_required_fp_bytes(f.contract);
    CHECK(rsx_scene_alpha_evaluate(f.contract,NULL,f.fp,bytes,f.lod,sample,&f,&r)==RSX_SCENE_ALPHA_INVALID);
    CHECK(rsx_scene_alpha_evaluate(f.contract,&f.v,NULL,bytes,f.lod,sample,&f,&r)==RSX_SCENE_ALPHA_INVALID);
    CHECK(rsx_scene_alpha_evaluate(f.contract,&f.v,f.fp,bytes-16,f.lod,sample,&f,&r)==RSX_SCENE_ALPHA_INVALID);
    CHECK(rsx_scene_alpha_evaluate(f.contract,&f.v,f.fp,bytes+16,f.lod,sample,&f,&r)==RSX_SCENE_ALPHA_INVALID);
    CHECK(rsx_scene_alpha_evaluate(f.contract,&f.v,f.fp,SIZE_MAX,f.lod,sample,&f,&r)==RSX_SCENE_ALPHA_INVALID);
    CHECK(rsx_scene_alpha_evaluate(f.contract,&f.v,f.fp,bytes,NULL,sample,&f,&r)==RSX_SCENE_ALPHA_INVALID);
    CHECK(rsx_scene_alpha_evaluate(f.contract,&f.v,f.fp,bytes,f.lod,NULL,&f,&r)==RSX_SCENE_ALPHA_INVALID);
    CHECK(rsx_scene_alpha_evaluate(f.contract,&f.v,f.fp,bytes,f.lod,sample,&f,NULL)==RSX_SCENE_ALPHA_INVALID);
    CHECK(!memcmp(&r,&saved,sizeof r));
    unsigned char unaligned[sizeof f.fp+1];memcpy(unaligned+1,f.fp,bytes);
    CHECK(rsx_scene_alpha_evaluate(f.contract,&f.v,unaligned+1,bytes,f.lod,sample,&f,&r)==RSX_SCENE_ALPHA_OK && r.alpha==.28125f);
    f.v.t0[2]=NAN;CHECK(failed_unchanged(&f,RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED));
    setup(&f,&cases[2]);f.lod[0]=INFINITY;CHECK(failed_unchanged(&f,RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED));
    setup(&f,&cases[0]);f.v.t0[0]=NAN;CHECK(failed_unchanged(&f,RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED));
    setup(&f,&cases[0]);f.lod[0]=NAN;CHECK(failed_unchanged(&f,RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED));
    return 1;
}
int main(void)
{
    if(!lookup_checks() || !simple_checks() || !fresnel_checks() || !light_checks() || !interface_checks())return 1;
    printf("PASS %u CPU alpha contract checks; 8 exact PS hashes cover 10 captured pipelines; no GPU\n",checks);
    return 0;
}
