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
    unsigned decode_scale_component,decode_bias_component;
} fixture_contract;
/* Independent fixture setup identifies the exact components that must matter.
 * Expected scalar results below are fixed analytic binary fractions, not a
 * second copy of the implementation's table-driven expression evaluator. */
static const fixture_contract cases[]={
    {"84d877ddc6083545452948630259c21ca796e7985fcd5e7b5a005666e4a0d634",15,0,11,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {"adc0a1a4e6f5204131f3e40be1ec23ed10284e7100ceb80705fb0c917689a78d",12,0,9,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {"bc1839eec9a6aebf2e0b5b860740e7368411aca1429e6a730c4a044685d9f0b6",43,1,42,0,12,0,30,33,34,35,37,3,39,40,41,0,1},
    {"4d7f89f750d67445d700f6edd5715c8ab59996a12618e645a9b86c42a3f48c63",41,1,40,2,16,0,30,32,33,34,35,0,37,38,39,0,1},
    {"65fbf5c251ee842a32bc38e185405f37c6f4c0ae001b7bbba3a8a832c6a16c38",35,1,34,0,2,1,21,25,27,28,29,0,31,32,33,0,1},
    {"bf9b5d7c6a2e73f4d9639259a0322518da9cc051a6928df898bb4ef65fa720ac",38,1,37,0,6,0,27,28,30,31,32,0,33,35,36,0,1},
    {"fa93e0db862af8fbd755b7270649e6f870c9ff17c4e6f627f4ede946cf550e36",37,1,36,0,8,0,26,27,29,30,32,0,33,34,35,0,1},
    {"4cf8870c89518ec8a342062570bd75a11149bbfd69a48d6b13df4ebe90b0442a",23,2,22,0,0,0,0,0,0,0,0,0,0,20,21,0,1},
    {"7d1cae7dbfc3397410bf569d864441df3b24d8501d65b0b026a200b9fb3e9ebb",12,0,10,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {"beb09f4079b28ab71fb27cb1404a5ad55355d8a3a98e6bc03f319a1ba22f4d77",15,0,11,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {"8cb0c015c36b64e89febba4fd65b21c4cc2a7178e2772c2e374ac1b7c9ded6e7",12,0,9,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    /* Full captured pipe107 PS: normal decode uses fp[0].y/.z; x is unused. */
    {"1f522df4363a3b10c071afc967bee6c1f4c08583cb9e5b0e969112dd3395fed0",35,1,34,0,4,1,22,24,26,28,29,0,31,32,33,1,2},
    /* Pipe110 alpha uses tex0.a * col0.a * 2 * fp[8].x; no discard. */
    {"a932910e5f10a1c6ff2837127c4b982a2451598a773084733b55bc94f094d738",10,0,8,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    /* Pipe117: strict threshold, alpha scale fp[10].x, other resources RGB only. */
    {"7bcbd31dd59e0fa8f6f39d0bf270b7206853f817f9cdd557cdd96c02cc497bd7",12,0,10,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {"d4c3e8c882ef3d96befd2c57386ab9697c52f1eb5b9d08b06d715557b7fe16c8",34,1,33,0,6,0,23,25,26,27,28,0,29,31,32,0,1}
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
    f->fp[d->decode][d->decode_scale_component]=2;
    f->fp[d->decode][d->decode_bias_component]=-1;
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
    CHECK(result.thresholded==1 && result.coverage==fminf(fmaxf(alpha,0),1));
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
    const char* repaired[2]={
        "c0dfeb27d8b4564c76a0c4597a2803e2207e45f087352ac278d7435ab6c0a50c",
        "f8edbea31132b75c337c8af49016e831fe10930deaffd151a9317f40216acdc8"};
    const char* original[2]={
        "816722266a9edefb0d0a70c97b469c5c1d2d65658d8c06ffb296ecc2601ef579",
        "b2bb97384f7f2e7c066f4560886b0aa2c039ba982cbdb8ae2b824fd289a3b043"};
    for(unsigned i=0;i<2;i++){
        const rsx_scene_alpha_contract* c=rsx_scene_alpha_contract_for_sha256(repaired[i]);
        CHECK(c && c==rsx_scene_alpha_contract_for_sha256(original[i]));
        char unknown[65];memcpy(unknown,repaired[i],65);
        unknown[0]=unknown[0]=='0'?'1':'0';
        CHECK(!rsx_scene_alpha_contract_for_sha256(unknown));
    }
    CHECK(!rsx_scene_alpha_contract_for_sha256(NULL));
    CHECK(!rsx_scene_alpha_contract_for_sha256(""));
    CHECK(!rsx_scene_alpha_contract_for_sha256("84d877dd"));
    CHECK(!rsx_scene_alpha_contract_for_sha256("0000000000000000000000000000000000000000000000000000000000000000"));
    /* The other unsupported blend shader is not admitted by pipe107 work. */
    CHECK(!rsx_scene_alpha_contract_for_sha256("36ab2319ba5aaf66d08a5c35c0ec2b45a1968780bd6cb3e6cf7862e3bf054b65"));
    char hash[66];memcpy(hash,cases[0].hash,65);hash[12]='g';CHECK(!rsx_scene_alpha_contract_for_sha256(hash));
    memcpy(hash,cases[0].hash,65);hash[64]='0';hash[65]=0;CHECK(!rsx_scene_alpha_contract_for_sha256(hash));
    for(size_t i=0;i<sizeof cases/sizeof cases[0];++i){
        const rsx_scene_alpha_contract* c=rsx_scene_alpha_contract_for_sha256(cases[i].hash);CHECK(c);
        CHECK(!strcmp(rsx_scene_alpha_contract_sha256(c),cases[i].hash));
        CHECK(rsx_scene_alpha_required_fp_bytes(c)==(size_t)(cases[i].constants+1)*16);
        CHECK(rsx_scene_alpha_texture_mask(c)==(cases[i].kind?3u:1u));
        CHECK(rsx_scene_alpha_contract_id(c)==(int)i);
        CHECK(rsx_scene_alpha_contract_has_threshold(c)==(i<8 || i==11 || i>=13));
        CHECK(rsx_scene_alpha_contract_for_id((unsigned)i)==c);
        memcpy(hash,cases[i].hash,65);
        for(unsigned k=0;k<64;++k)if(hash[k]>='a' && hash[k]<='f')hash[k]=(char)(hash[k]-('a'-'A'));
        CHECK(rsx_scene_alpha_contract_for_sha256(hash)==c);
    }
    const rsx_scene_alpha_contract* fake=(const rsx_scene_alpha_contract*)(uintptr_t)1;
    CHECK(!rsx_scene_alpha_contract_sha256(fake));CHECK(!rsx_scene_alpha_required_fp_bytes(fake));
    CHECK(!rsx_scene_alpha_texture_mask(fake));CHECK(!rsx_scene_alpha_contract_sha256(NULL));
    CHECK(rsx_scene_alpha_contract_has_threshold(fake)==-1);
    CHECK(rsx_scene_alpha_contract_has_threshold(NULL)==-1);
    CHECK(!rsx_scene_alpha_contract_for_id(15));
    CHECK(!rsx_scene_alpha_contract_for_id(UINT32_MAX));
    CHECK(rsx_scene_alpha_evaluate(fake,NULL,NULL,0,NULL,NULL,NULL,NULL)==RSX_SCENE_ALPHA_UNSUPPORTED);
    return 1;
}
static int simple_checks(void)
{
    const unsigned simple_ids[]={0,1,13};
    for(unsigned n=0;n<sizeof simple_ids/sizeof simple_ids[0];++n){
        unsigned i=simple_ids[n];
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
/* Independent scalar translation of the captured register writes and final
 * NaN sanitization. No production contract descriptor drives this oracle. */
static rsx_scene_alpha_result pipe117_original(const fixture* f)
{
    float h1_w=(f->v.col0[3]*f->samples[0][3])*2.0f;
    float r0_w=h1_w*f->fp[10][0];
    float out_w=r0_w==r0_w?r0_w:0.0f;
    rsx_scene_alpha_result r={0};r.raw_alpha=r0_w;r.alpha=out_w;
    r.reference=f->fp[12][0];r.keep=out_w>r.reference;
    r.coverage=fminf(fmaxf(out_w,0.0f),1.0f);r.thresholded=1;return r;
}
static int pipe117_original_checks(void)
{
    const float alphas[]={.25f,NAN,INFINITY,-INFINITY,0.0f,-.25f};
    const float scales[]={2.0f,NAN,INFINITY,0.0f,-1.0f};
    const float refs[]={0.0f,.5f,NAN,nextafterf(.5f,-INFINITY),nextafterf(.5f,INFINITY)};
    fixture f;rsx_scene_alpha_result a,b;
    CHECK(rsx_scene_alpha_contract_for_sha256("8fccec9d863b934fe89adfc52624f681cb99934885f68e83bdafcb670ff74799")==rsx_scene_alpha_contract_for_id(13));
    for(unsigned i=0;i<sizeof alphas/sizeof alphas[0];++i)
    for(unsigned j=0;j<sizeof scales/sizeof scales[0];++j)
    for(unsigned k=0;k<sizeof refs/sizeof refs[0];++k){
        setup(&f,&cases[13]);f.samples[0][3]=alphas[i];f.fp[10][0]=scales[j];f.fp[12][0]=refs[k];
        a=pipe117_original(&f);CHECK(evaluate(&f,&b)==RSX_SCENE_ALPHA_OK);
        CHECK((isnan(a.raw_alpha)&&isnan(b.raw_alpha)) || a.raw_alpha==b.raw_alpha);
        CHECK(a.alpha==b.alpha && a.coverage==b.coverage && a.keep==b.keep && b.thresholded);
        /* Changing every otherwise NaN RGB input cannot alter scalar alpha. */
        for(unsigned ch=0;ch<3;++ch){f.v.col0[ch]=100.0f+ch;f.samples[0][ch]=-100.0f-ch;}
        for(unsigned ch=0;ch<4;++ch)f.v.col1[ch]=200.0f+ch;
        for(unsigned slot=0;slot<12;++slot)if(slot!=10)
            for(unsigned ch=0;ch<4;++ch)f.fp[slot][ch]=300.0f+slot+ch;
        CHECK(evaluate(&f,&b)==RSX_SCENE_ALPHA_OK);
        CHECK(a.alpha==b.alpha && a.coverage==b.coverage && a.keep==b.keep);
        CHECK(f.calls==1 && f.units[0]==0);
    }
    return 1;
}
static int a2c_checks(void)
{
    const unsigned a2c_ids[]={8,9,10,12};
    for(unsigned n=0;n<sizeof a2c_ids/sizeof a2c_ids[0];++n){
        unsigned i=a2c_ids[n];
        fixture f;rsx_scene_alpha_result r;setup(&f,&cases[i]);
        /* A=.5*.25*2=.25. The scale of 2 gives .5 coverage. The declared
         * fp_alpha exists in the buffer, but the shader has no comparison. */
        CHECK(evaluate(&f,&r)==RSX_SCENE_ALPHA_OK);
        CHECK(r.raw_alpha==.5f && r.alpha==.5f && r.coverage==.5f);
        CHECK(!r.thresholded && r.keep && isnan(r.reference));
        CHECK(f.calls==1 && f.units[0]==0 && f.seen_lod[0]==-.25f);
        f.fp[f.desc->constants][0]=INFINITY;
        CHECK(evaluate(&f,&r)==RSX_SCENE_ALPHA_OK && r.keep && r.coverage==.5f);
        f.fp[f.desc->scale][0]=0;
        CHECK(evaluate(&f,&r)==RSX_SCENE_ALPHA_OK && r.alpha==0 && r.coverage==0 && r.keep);
        f.fp[f.desc->scale][0]=4;
        CHECK(evaluate(&f,&r)==RSX_SCENE_ALPHA_OK && r.alpha==1 && r.coverage==1 && r.keep);
        f.fp[f.desc->scale][0]=8;
        CHECK(evaluate(&f,&r)==RSX_SCENE_ALPHA_OK && r.alpha==2 && r.coverage==1 && r.keep);
        f.fp[f.desc->scale][0]=-4;
        CHECK(evaluate(&f,&r)==RSX_SCENE_ALPHA_OK && r.alpha==-1 && r.coverage==0 && r.keep);
        f.fp[f.desc->scale][0]=NAN;
        CHECK(evaluate(&f,&r)==RSX_SCENE_ALPHA_OK && isnan(r.raw_alpha) && r.alpha==0 && r.coverage==0 && r.keep);
        setup(&f,&cases[i]);f.samples[0][3]=INFINITY;
        CHECK(evaluate(&f,&r)==RSX_SCENE_ALPHA_OK && isinf(r.alpha) && r.coverage==1 && r.keep);
        f.samples[0][3]=-INFINITY;
        CHECK(evaluate(&f,&r)==RSX_SCENE_ALPHA_OK && isinf(r.alpha) && r.coverage==0 && r.keep);
        setup(&f,&cases[i]);f.v.t0[0]=INFINITY;
        CHECK(failed_unchanged(&f,RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED));
        setup(&f,&cases[i]);f.lod[0]=NAN;
        CHECK(failed_unchanged(&f,RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED));
        setup(&f,&cases[i]);f.fail_unit=0;
        CHECK(failed_unchanged(&f,RSX_SCENE_ALPHA_SAMPLE_UNAVAILABLE));
        setup(&f,&cases[i]);
        memset(&r,0x5a,sizeof r);rsx_scene_alpha_result saved=r;
        CHECK(rsx_scene_alpha_evaluate(f.contract,&f.v,f.fp,rsx_scene_alpha_required_fp_bytes(f.contract)-16,
                                       f.lod,sample,&f,&r)==RSX_SCENE_ALPHA_INVALID);
        CHECK(!memcmp(&r,&saved,sizeof r));
    }
    return 1;
}
static int fresnel_checks(void)
{
    const unsigned fresnel_ids[]={2,3,4,5,6,11,14};
    for(unsigned n=0;n<sizeof fresnel_ids/sizeof fresnel_ids[0];++n){
        unsigned i=fresnel_ids[n];
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
        /* The captured contract-6 shader uses base=(0,0), exponent=2:
         * exp2(log2(0)*2) is zero. A=.25 then gives alpha=.15625 with
         * these fixture attenuation and clamp constants. All Fresnel
         * contracts share the same scalar zero-base rule. */
        setup(&f,d);f.fp[d->power_base][0]=0;f.fp[d->power_base][1]=0;
        CHECK(expected(&f,.15625f,1));
        f.fp[d->power_exponent][2]=0;
        CHECK(failed_unchanged(&f,RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED));
        f.fp[d->power_exponent][2]=-2;
        CHECK(failed_unchanged(&f,RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED));
        setup(&f,d);f.fp[d->power_base][0]=0;f.fp[d->power_base][1]=0;
        f.fp[d->attenuation][3]=0;f.fp[d->mix][3]=0;
        f.fp[d->inner][d->inner_component]=1;f.fp[d->base][3]=0;
        f.fp[d->lower][0]=0;f.fp[d->upper][0]=1;f.fp[d->scale][0]=1;
        CHECK(expected(&f,.25f,1)); /* Captured draw 99/100 coefficient shape. */
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
static int pipe107_decode_checks(void)
{
    fixture f;setup(&f,&cases[11]);
    CHECK(isnan(f.fp[0][0])); /* Captured fp[0].x is unused by this shader. */
    CHECK(expected(&f,.28125f,1));
    f.fp[0][1]=NAN;CHECK(failed_unchanged(&f,RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED));
    setup(&f,&cases[11]);f.fp[0][2]=NAN;
    CHECK(failed_unchanged(&f,RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED));
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
    if(!lookup_checks() || !simple_checks() || !pipe117_original_checks() || !a2c_checks() || !fresnel_checks() ||
       !pipe107_decode_checks() || !light_checks() || !interface_checks())return 1;
    printf("PASS %u CPU alpha contract checks; 15 exact PS hashes; no GPU\n",checks);
    return 0;
}
