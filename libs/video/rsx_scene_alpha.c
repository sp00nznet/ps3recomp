#include "rsx_scene_alpha.h"
#include <math.h>
#include <string.h>

#ifdef __FAST_MATH__
#error "rsx_scene_alpha requires IEEE NaN handling; compile without fast-math"
#endif
#if defined(__clang__)
#pragma clang fp contract(off)
#pragma clang fp reassociate(off)
#endif

enum alpha_kind { SIMPLE, FRESNEL, LIGHT };
struct rsx_scene_alpha_contract {
    const char* hash;
    const char* lf_hash;
    enum alpha_kind kind;
    unsigned constants,scale,normal_decode;
    unsigned q,q_component,power_base,power_exponent;
    unsigned attenuation,mix,inner_limit,inner_component,base,lower,upper;
};
/* Scalar component contracts from work-bvh/alpha-contract.json. Identity is
 * the complete captured PS HLSL SHA256, never a guessed draw/pipeline number. */
static const rsx_scene_alpha_contract contracts[]={
    {.hash="84d877ddc6083545452948630259c21ca796e7985fcd5e7b5a005666e4a0d634",.lf_hash="4b43ead21c9dce22ed005919077251929e3c5efb9890013745ece3afb0cfd4b7",.kind=SIMPLE,.constants=15,.scale=11},
    {.hash="adc0a1a4e6f5204131f3e40be1ec23ed10284e7100ceb80705fb0c917689a78d",.lf_hash="529231634350bbd2f296b7caa78ddd5346a9502701f071931cb086902855671c",.kind=SIMPLE,.constants=12,.scale=9},
    {.hash="bc1839eec9a6aebf2e0b5b860740e7368411aca1429e6a730c4a044685d9f0b6",.lf_hash="816722266a9edefb0d0a70c97b469c5c1d2d65658d8c06ffb296ecc2601ef579",.kind=FRESNEL,.constants=43,.scale=42,.normal_decode=0,
     .q=12,.q_component=0,.power_base=30,.power_exponent=33,.attenuation=34,.mix=35,.inner_limit=37,.inner_component=3,.base=39,.lower=40,.upper=41},
    {.hash="4d7f89f750d67445d700f6edd5715c8ab59996a12618e645a9b86c42a3f48c63",.lf_hash="b2bb97384f7f2e7c066f4560886b0aa2c039ba982cbdb8ae2b824fd289a3b043",.kind=FRESNEL,.constants=41,.scale=40,.normal_decode=2,
     .q=16,.q_component=0,.power_base=30,.power_exponent=32,.attenuation=33,.mix=34,.inner_limit=35,.inner_component=0,.base=37,.lower=38,.upper=39},
    {.hash="65fbf5c251ee842a32bc38e185405f37c6f4c0ae001b7bbba3a8a832c6a16c38",.lf_hash="baf7a956ae2e3b649c393fefd672697904fe26e66810eb3a8f8cf896c8934f2e",.kind=FRESNEL,.constants=35,.scale=34,.normal_decode=0,
     .q=2,.q_component=1,.power_base=21,.power_exponent=25,.attenuation=27,.mix=28,.inner_limit=29,.inner_component=0,.base=31,.lower=32,.upper=33},
    {.hash="bf9b5d7c6a2e73f4d9639259a0322518da9cc051a6928df898bb4ef65fa720ac",.lf_hash="a55ff59d9090562cca5ccb23eaba30b9886bfe2005bb3f61b51329f1d7b8d315",.kind=FRESNEL,.constants=38,.scale=37,.normal_decode=0,
     .q=6,.q_component=0,.power_base=27,.power_exponent=28,.attenuation=30,.mix=31,.inner_limit=32,.inner_component=0,.base=33,.lower=35,.upper=36},
    {.hash="fa93e0db862af8fbd755b7270649e6f870c9ff17c4e6f627f4ede946cf550e36",.lf_hash="c0a70bf855304927e3b74100d52669cf363e6c1904545ffeb58892d82178de4d",.kind=FRESNEL,.constants=37,.scale=36,.normal_decode=0,
     .q=8,.q_component=0,.power_base=26,.power_exponent=27,.attenuation=29,.mix=30,.inner_limit=32,.inner_component=0,.base=33,.lower=34,.upper=35},
    {.hash="4cf8870c89518ec8a342062570bd75a11149bbfd69a48d6b13df4ebe90b0442a",.lf_hash="9020e3a755063901005979eb5dc881e2afba13f4cfe01ec1220d1b214da0dce2",.kind=LIGHT,.constants=23,.scale=22,.normal_decode=0}
};
#define CONTRACT_COUNT (sizeof contracts/sizeof contracts[0])

static int known(const rsx_scene_alpha_contract* c)
{
    for(size_t i=0;i<CONTRACT_COUNT;++i)if(c==&contracts[i])return 1;
    return 0;
}
const rsx_scene_alpha_contract* rsx_scene_alpha_contract_for_sha256(const char* hash)
{
    if(!hash)return NULL;
    char normalized[65];
    for(unsigned i=0;i<64;++i){
        char v=hash[i];
        if(v>='A' && v<='F')v=(char)(v+('a'-'A'));
        if(!((v>='0' && v<='9') || (v>='a' && v<='f')))return NULL;
        normalized[i]=v;
    }
    if(hash[64])return NULL;
    normalized[64]=0;
    for(size_t i=0;i<CONTRACT_COUNT;++i)
        if(!strcmp(normalized,contracts[i].hash) || !strcmp(normalized,contracts[i].lf_hash))return &contracts[i];
    return NULL;
}
const char* rsx_scene_alpha_contract_sha256(const rsx_scene_alpha_contract* c)
{
    return known(c)?c->hash:NULL;
}
size_t rsx_scene_alpha_required_fp_bytes(const rsx_scene_alpha_contract* c)
{
    return known(c)?(size_t)(c->constants+1)*16u:0;
}
unsigned rsx_scene_alpha_texture_mask(const rsx_scene_alpha_contract* c)
{
    return known(c)?(c->kind==SIMPLE?1u:3u):0;
}
int rsx_scene_alpha_contract_id(const rsx_scene_alpha_contract* c)
{
    for(size_t i=0;i<CONTRACT_COUNT;++i)if(c==&contracts[i])return (int)i;
    return -1;
}

static float dot3(const float a[3],const float b[3])
{
    return (a[0]*b[0]+a[1]*b[1])+a[2]*b[2];
}
static int normalize3(const float v[3],float out[3])
{
    for(unsigned k=0;k<3;++k)if(!isfinite(v[k]))return 0;
    float squared=dot3(v,v);
    if(!isfinite(squared) || squared<=0)return 0;
    float length=sqrtf(squared);
    for(unsigned k=0;k<3;++k){out[k]=v[k]/length;if(!isfinite(out[k]))return 0;}
    return 1;
}
/* Refuse NaN/infinite operands before a clamp: the source hashes identify
 * scalar dependencies, but do not establish GPU intrinsic NaN selection. */
static int clamp_operands(float a,float b)
{
    return isfinite(a) && isfinite(b);
}
static rsx_scene_alpha_status normal_terms(const rsx_scene_alpha_contract* c,
    const rsx_scene_vertex_output* v,const float fp[44][4],const float texture_normal[4],
    float n[3],float* d)
{
    float decoded[3],view[3];
    for(unsigned k=0;k<3;++k)
        decoded[k]=texture_normal[k]*fp[c->normal_decode][0]+fp[c->normal_decode][1];
    if(!normalize3(decoded,n) || !normalize3(v->t4,view))return RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED;
    *d=dot3(n,view);
    return isfinite(*d)?RSX_SCENE_ALPHA_OK:RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED;
}
static rsx_scene_alpha_status fresnel_alpha(const rsx_scene_alpha_contract* c,
    const float fp[44][4],float a,float d,float* raw)
{
    float q=(-fabsf(d))+fp[c->q][c->q_component];
    if(!isfinite(q))return RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED;
    q=fminf(fmaxf(q,0),1);
    float f0=q*fp[c->power_base][1]+fp[c->power_base][0];
    if(!isfinite(f0) || f0<=0 || !isfinite(fp[c->power_exponent][2]))
        return RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED;
    float f1=log2f(f0);
    float f2=f1*fp[c->power_exponent][2];
    float f=exp2f(f2);
    if(!isfinite(a) || !isfinite(f1) || !isfinite(f2) || !isfinite(f))
        return RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED;
    float product=a*f;
    float sum=a+f;
    float b=sum*(-fp[c->attenuation][3])+sum;
    float w0=product*fp[c->mix][3]+b;
    float inner=fp[c->inner_limit][c->inner_component];
    if(!clamp_operands(w0,inner))return RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED;
    float w1=fminf(w0,inner);
    float w2=a*fp[c->base][3]+w1;
    if(!clamp_operands(w2,fp[c->lower][0]))return RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED;
    float w3=fmaxf(w2,fp[c->lower][0]);
    if(!clamp_operands(w3,fp[c->upper][0]))return RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED;
    float w4=fminf(w3,fp[c->upper][0]);
    *raw=w4*fp[c->scale][0];
    return RSX_SCENE_ALPHA_OK;
}
static rsx_scene_alpha_status light_alpha(const rsx_scene_vertex_output* v,
    const float fp[44][4],float a,const float n[3],float d,float* raw)
{
    float k0=d+fp[3][3];
    if(!clamp_operands(k0,fp[4][0]))return RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED;
    float k=fmaxf(k0,fp[4][0])*2.0f,light[3];
    if(!normalize3(v->t3,light))return RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED;
    float r0=dot3(n,light)/2.0f;
    float r=r0+fp[6][2];
    float f=k*r;
    float w0=a*(-f)+a;
    float w1=w0+f;
    float w2=a*fp[19][3]+w1;
    if(!clamp_operands(w2,fp[20][0]))return RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED;
    float w3=fmaxf(w2,fp[20][0]);
    if(!clamp_operands(w3,fp[21][0]))return RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED;
    float w4=fminf(w3,fp[21][0]);
    *raw=w4*fp[22][0];
    return RSX_SCENE_ALPHA_OK;
}
rsx_scene_alpha_status rsx_scene_alpha_evaluate(const rsx_scene_alpha_contract* c,
    const rsx_scene_vertex_output* v,const void* constants,size_t bytes,const float lod[16],
    rsx_scene_alpha_sample_fn sample,void* user,rsx_scene_alpha_result* result)
{
    if(!known(c))return RSX_SCENE_ALPHA_UNSUPPORTED;
    if(!v || !constants || !lod || !sample || !result || bytes!=rsx_scene_alpha_required_fp_bytes(c))
        return RSX_SCENE_ALPHA_INVALID;
    unsigned alpha_unit=c->kind==SIMPLE?0u:1u;
    if(!isfinite(v->t0[0]) || !isfinite(v->t0[1]) || !isfinite(lod[alpha_unit]) ||
       (c->kind!=SIMPLE && (!isfinite(v->t0[2]) || !isfinite(v->t0[3]) || !isfinite(lod[0]))))
        return RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED;
    /* memcpy supports unaligned captured constant payloads without aliasing. */
    float fp[44][4];memcpy(fp,constants,bytes);
    float rgba[4]={NAN,NAN,NAN,NAN};
    if(!sample(user,alpha_unit,v->t0[0],v->t0[1],lod[alpha_unit],rgba))
        return RSX_SCENE_ALPHA_SAMPLE_UNAVAILABLE;
    float a=(v->col0[3]*rgba[3])*2.0f,raw;
    if(c->kind==SIMPLE)raw=a*fp[c->scale][0];
    else {
        float texture_normal[4]={NAN,NAN,NAN,NAN},n[3],d;
        if(!sample(user,0,v->t0[2],v->t0[3],lod[0],texture_normal))
            return RSX_SCENE_ALPHA_SAMPLE_UNAVAILABLE;
        rsx_scene_alpha_status status=normal_terms(c,v,fp,texture_normal,n,&d);
        if(status!=RSX_SCENE_ALPHA_OK)return status;
        status=c->kind==FRESNEL?fresnel_alpha(c,fp,a,d,&raw):light_alpha(v,fp,a,n,d,&raw);
        if(status!=RSX_SCENE_ALPHA_OK)return status;
    }
    rsx_scene_alpha_result answer={raw,isnan(raw)?0.0f:raw,fp[c->constants][0],0};
    answer.keep=answer.alpha>answer.reference;
    *result=answer;
    return RSX_SCENE_ALPHA_OK;
}
