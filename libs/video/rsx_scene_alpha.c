#include "rsx_scene_alpha.h"
#include "rsx_scene_alpha_tc8_aliases.h"
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
    unsigned thresholded;
    unsigned constants,scale,normal_decode,decode_scale_component,decode_bias_component;
    unsigned q,q_component,power_base,power_exponent;
    unsigned attenuation,mix,inner_limit,inner_component,base,lower,upper;
};
/* Scalar component contracts from work-bvh/alpha-contract.json. Identity is
 * the complete captured PS HLSL SHA256, never a guessed draw/pipeline number. */
static const rsx_scene_alpha_contract contracts[]={
    {.hash="84d877ddc6083545452948630259c21ca796e7985fcd5e7b5a005666e4a0d634",.lf_hash="4b43ead21c9dce22ed005919077251929e3c5efb9890013745ece3afb0cfd4b7",.kind=SIMPLE,.thresholded=1,.constants=15,.scale=11},
    {.hash="adc0a1a4e6f5204131f3e40be1ec23ed10284e7100ceb80705fb0c917689a78d",.lf_hash="529231634350bbd2f296b7caa78ddd5346a9502701f071931cb086902855671c",.kind=SIMPLE,.thresholded=1,.constants=12,.scale=9},
    {.hash="bc1839eec9a6aebf2e0b5b860740e7368411aca1429e6a730c4a044685d9f0b6",.lf_hash="816722266a9edefb0d0a70c97b469c5c1d2d65658d8c06ffb296ecc2601ef579",.kind=FRESNEL,.thresholded=1,.constants=43,.scale=42,.normal_decode=0,.decode_bias_component=1,
     .q=12,.q_component=0,.power_base=30,.power_exponent=33,.attenuation=34,.mix=35,.inner_limit=37,.inner_component=3,.base=39,.lower=40,.upper=41},
    {.hash="4d7f89f750d67445d700f6edd5715c8ab59996a12618e645a9b86c42a3f48c63",.lf_hash="b2bb97384f7f2e7c066f4560886b0aa2c039ba982cbdb8ae2b824fd289a3b043",.kind=FRESNEL,.thresholded=1,.constants=41,.scale=40,.normal_decode=2,.decode_bias_component=1,
     .q=16,.q_component=0,.power_base=30,.power_exponent=32,.attenuation=33,.mix=34,.inner_limit=35,.inner_component=0,.base=37,.lower=38,.upper=39},
    {.hash="65fbf5c251ee842a32bc38e185405f37c6f4c0ae001b7bbba3a8a832c6a16c38",.lf_hash="baf7a956ae2e3b649c393fefd672697904fe26e66810eb3a8f8cf896c8934f2e",.kind=FRESNEL,.thresholded=1,.constants=35,.scale=34,.normal_decode=0,.decode_bias_component=1,
     .q=2,.q_component=1,.power_base=21,.power_exponent=25,.attenuation=27,.mix=28,.inner_limit=29,.inner_component=0,.base=31,.lower=32,.upper=33},
    {.hash="bf9b5d7c6a2e73f4d9639259a0322518da9cc051a6928df898bb4ef65fa720ac",.lf_hash="a55ff59d9090562cca5ccb23eaba30b9886bfe2005bb3f61b51329f1d7b8d315",.kind=FRESNEL,.thresholded=1,.constants=38,.scale=37,.normal_decode=0,.decode_bias_component=1,
     .q=6,.q_component=0,.power_base=27,.power_exponent=28,.attenuation=30,.mix=31,.inner_limit=32,.inner_component=0,.base=33,.lower=35,.upper=36},
    {.hash="fa93e0db862af8fbd755b7270649e6f870c9ff17c4e6f627f4ede946cf550e36",.lf_hash="c0a70bf855304927e3b74100d52669cf363e6c1904545ffeb58892d82178de4d",.kind=FRESNEL,.thresholded=1,.constants=37,.scale=36,.normal_decode=0,.decode_bias_component=1,
     .q=8,.q_component=0,.power_base=26,.power_exponent=27,.attenuation=29,.mix=30,.inner_limit=32,.inner_component=0,.base=33,.lower=34,.upper=35},
    {.hash="4cf8870c89518ec8a342062570bd75a11149bbfd69a48d6b13df4ebe90b0442a",.lf_hash="9020e3a755063901005979eb5dc881e2afba13f4cfe01ec1220d1b214da0dce2",.kind=LIGHT,.thresholded=1,.constants=23,.scale=22,.normal_decode=0,.decode_bias_component=1},
    /* Captured A2C shaders retain the scalar alpha output but contain no discard. */
    {.hash="7d1cae7dbfc3397410bf569d864441df3b24d8501d65b0b026a200b9fb3e9ebb",.lf_hash="a84392db694aab4b5435524b9e6018f7b271088e60d5ebbb06bca3082f7ba797",.kind=SIMPLE,.constants=12,.scale=10},
    {.hash="beb09f4079b28ab71fb27cb1404a5ad55355d8a3a98e6bc03f319a1ba22f4d77",.lf_hash="369d37e28775a313f54bd620cef1b9d65412d85d201d739e3c8f798a2b3cc7d3",.kind=SIMPLE,.constants=15,.scale=11},
    {.hash="8cb0c015c36b64e89febba4fd65b21c4cc2a7178e2772c2e374ac1b7c9ded6e7",.lf_hash="47535a9dfef9d40b6edf11e473ca2fa717a7057e44579112093002c322607b08",.kind=SIMPLE,.constants=12,.scale=9},
    /* capture-20261019T030813Z/shaders/pipe107.ps.hlsl, also prior pipe104:
     * alpha uses normal tex0 RGB * fp[0].y + fp[0].z, tex1.a, and fp[4,22,24,26,28,29,31,32,33,34].
     * Other sampled textures affect RGB only. Keep the exact full-PS identities. */
    {.hash="1f522df4363a3b10c071afc967bee6c1f4c08583cb9e5b0e969112dd3395fed0",.lf_hash="298e00a919a7a779a09da135b48d8ba8513992d2af6f50e960edcefeb8e559aa",.kind=FRESNEL,.thresholded=1,.constants=35,.scale=34,.normal_decode=0,.decode_scale_component=1,.decode_bias_component=2,
     .q=4,.q_component=1,.power_base=22,.power_exponent=24,.attenuation=26,.mix=28,.inner_limit=29,.inner_component=0,.base=31,.lower=32,.upper=33},
    /* capture-20261019T032510Z/shaders/pipe110.ps.hlsl:
     * no discard; output alpha is (2 * col0.a * tex0.a) * fp[8].x.
     * Other texture samples and fp constants affect RGB only. */
    {.hash="a932910e5f10a1c6ff2837127c4b982a2451598a773084733b55bc94f094d738",.lf_hash="ca7f1fe7be0b0d38d514582b023782e233f9abe08e25f4545d89cb03feacc8ed",.kind=SIMPLE,.constants=10,.scale=8},
    /* capture-20261019T033220Z/shaders/pipe117.ps.hlsl: strict alpha discard;
     * alpha=((col0.a * tex0.a) * 2) * fp[10].x; RGB has no alpha dependency. */
    {.hash="7bcbd31dd59e0fa8f6f39d0bf270b7206853f817f9cdd557cdd96c02cc497bd7",.lf_hash="8fccec9d863b934fe89adfc52624f681cb99934885f68e83bdafcb670ff74799",.kind=SIMPLE,.thresholded=1,.constants=12,.scale=10},
    /* capture-20261019T040116Z pipe119 and129 are byte-identical.
     * Normal tex0 uses fp0.x/y; base alpha uses tex1.a. The alpha-only
     * chain is q6.x, Fresnel23.x/y +25.z, mix26..29, floor31, cap32, scale33. */
    {.hash="d4c3e8c882ef3d96befd2c57386ab9697c52f1eb5b9d08b06d715557b7fe16c8",.lf_hash="8d45d68ed584e0d7b66b718498b7f30344fd6cdc456a684bfcae809f57df5456",.kind=FRESNEL,.thresholded=1,.constants=34,.scale=33,.normal_decode=0,.decode_bias_component=1,
     .q=6,.q_component=0,.power_base=23,.power_exponent=25,.attenuation=26,.mix=27,.inner_limit=28,.inner_component=0,.base=29,.lower=31,.upper=32}
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
    const char* identity=rsx_scene_alpha_tc8_original(normalized);
    for(size_t i=0;i<CONTRACT_COUNT;++i)
        if(!strcmp(identity,contracts[i].hash) || !strcmp(identity,contracts[i].lf_hash))return &contracts[i];
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
const rsx_scene_alpha_contract* rsx_scene_alpha_contract_for_id(unsigned id)
{
    return id<CONTRACT_COUNT?&contracts[id]:NULL;
}
int rsx_scene_alpha_contract_has_threshold(const rsx_scene_alpha_contract* c)
{
    return known(c)?(int)c->thresholded:-1;
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
        decoded[k]=texture_normal[k]*fp[c->normal_decode][c->decode_scale_component]+fp[c->normal_decode][c->decode_bias_component];
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
    const float exponent=fp[c->power_exponent][2];
    if(!isfinite(f0) || f0<0 || !isfinite(exponent))return RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED;
    float f=0;
    if(f0==0){
        /* Captured Fresnel materials use zero base/slope and exponent two.
         * Original GPU log2/exp2 yields zero. Keep 0^0/0^negative unsupported;
         * the independent GPU expression fixture qualifies the narrow case. */
        if(exponent<=0)return RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED;
    }else{
        float f1=log2f(f0),f2=f1*exponent;
        if(!isfinite(f1) || !isfinite(f2))return RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED;
        f=exp2f(f2);
    }
    if(!isfinite(a) || !isfinite(f))
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
    rsx_scene_alpha_result answer={0};
    answer.raw_alpha=raw;
    answer.alpha=isnan(raw)?0.0f:raw;
    answer.coverage=fminf(fmaxf(answer.alpha,0.0f),1.0f);
    answer.thresholded=(int)c->thresholded;
    answer.reference=c->thresholded?fp[c->constants][0]:NAN;
    answer.keep=c->thresholded?(answer.alpha>answer.reference):1;
    *result=answer;
    return RSX_SCENE_ALPHA_OK;
}
