/* Captured Folklore scalar alpha helper. No resources or scene data are baked
 * here. Contract IDs MUST come from CPU exact PS SHA256 matching; see
 * rsx_scene_alpha_contract_id. This is not a generic guest PS interpreter.
 *
 * Required caller hooks (definitions may follow this include):
 * - load_constant bounds-checks the complete FP buffer and returns shader-visible
 *   float4 data without rejecting unused NaN channels;
 * - sample applies captured texture/sampler/remap/filter/mip semantics at the
 *   explicitly supplied LOD. It must not depend on call order;
 * - invalidate_epoch marks the entire scene epoch unusable. Every failure,
 *   including an unknown contract, invokes it. A failed evaluation is neither
 *   opaque acceptance nor alpha rejection; ray traversal must abandon/fallback.
 *
 * No implicit raster derivatives or secondary-ray mip equivalence is claimed.
 * `precise` preserves ordered scalar arithmetic and inhibits contraction;
 * GPU normalize/log2/exp2 still need independent numeric qualification. */
#ifndef RSX_SCENE_ALPHA_GLSL
#define RSX_SCENE_ALPHA_GLSL

bool rsx_scene_alpha_load_constant(uint absolute_index,out vec4 value);
bool rsx_scene_alpha_sample(uint material_id,uint texture_unit,vec2 uv,float explicit_lod,out vec4 rgba);
void rsx_scene_alpha_invalidate_epoch(uint material_id,int status);

const int RSX_SCENE_ALPHA_OK=0;
const int RSX_SCENE_ALPHA_UNSUPPORTED=-1;
const int RSX_SCENE_ALPHA_INVALID=-2;
const int RSX_SCENE_ALPHA_SAMPLE_UNAVAILABLE=-3;
const int RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED=-4;

struct RsxSceneAlphaVertexOutput {
    vec4 pos;vec4 col0;vec4 col1;vec4 fog;
    vec4 t0;vec4 t1;vec4 t2;vec4 t3;vec4 t4;vec4 t5;vec4 t6;vec4 t7;
};
struct RsxSceneAlphaResult {float raw_alpha;float alpha;float reference;int keep;};
struct RsxSceneAlphaContract {
    uint kind;uint constants;uint scale;uint decode;
    uint q;uint q_component;uint power_base;uint power_exponent;
    uint attenuation;uint mix_weight;uint inner_limit;uint inner_component;
    uint base_weight;uint lower;uint upper;
};
const RsxSceneAlphaContract rsx_scene_alpha_contracts[8]=RsxSceneAlphaContract[8](
    RsxSceneAlphaContract(0,15,11,0, 0,0,0,0, 0,0,0,0, 0,0,0),
    RsxSceneAlphaContract(0,12,9,0, 0,0,0,0, 0,0,0,0, 0,0,0),
    RsxSceneAlphaContract(1,43,42,0, 12,0,30,33, 34,35,37,3, 39,40,41),
    RsxSceneAlphaContract(1,41,40,2, 16,0,30,32, 33,34,35,0, 37,38,39),
    RsxSceneAlphaContract(1,35,34,0, 2,1,21,25, 27,28,29,0, 31,32,33),
    RsxSceneAlphaContract(1,38,37,0, 6,0,27,28, 30,31,32,0, 33,35,36),
    RsxSceneAlphaContract(1,37,36,0, 8,0,26,27, 29,30,32,0, 33,34,35),
    RsxSceneAlphaContract(2,23,22,0, 0,0,0,0, 0,0,0,0, 0,20,21)
);
bool rsx_scene_alpha_finite(float x){return !isnan(x) && !isinf(x);}
bool rsx_scene_alpha_finite3(vec3 x){return !any(isnan(x)) && !any(isinf(x));}
float rsx_scene_alpha_dot3(vec3 a,vec3 b){precise float d=(a.x*b.x+a.y*b.y)+a.z*b.z;return d;}
bool rsx_scene_alpha_normalize3(vec3 v,out vec3 result){
    if(!rsx_scene_alpha_finite3(v))return false;
    precise float squared=rsx_scene_alpha_dot3(v,v);
    if(!rsx_scene_alpha_finite(squared) || squared<=0)return false;
    precise float length=sqrt(squared);
    precise vec3 normalized=v/length;
    result=normalized;return rsx_scene_alpha_finite3(result);
}
bool rsx_scene_alpha_component(uint base,uint index,uint component,out float value){
    vec4 v;if(!rsx_scene_alpha_load_constant(base+index,v))return false;
    value=v[component];return true;
}
bool rsx_scene_alpha_clamp_operands(float a,float b){return rsx_scene_alpha_finite(a) && rsx_scene_alpha_finite(b);}

int rsx_scene_alpha_evaluate_impl(uint id,uint material_id,uint fp_base,uint fp_count,
    RsxSceneAlphaVertexOutput v,vec2 lod,inout RsxSceneAlphaResult result)
{
    if(id>=8)return RSX_SCENE_ALPHA_UNSUPPORTED;
    RsxSceneAlphaContract c=rsx_scene_alpha_contracts[id];
    if(fp_count!=c.constants+1 || fp_base>0xffffffffu-c.constants)return RSX_SCENE_ALPHA_INVALID;
    uint alpha_unit=c.kind==0?0u:1u;
    if(any(isnan(v.t0.xy)) || any(isinf(v.t0.xy)) || !rsx_scene_alpha_finite(lod[alpha_unit]) ||
       (c.kind!=0 && (any(isnan(v.t0.zw)) || any(isinf(v.t0.zw)) || !rsx_scene_alpha_finite(lod.x))))
        return RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED;
    vec4 alpha_sample;
    if(!rsx_scene_alpha_sample(material_id,alpha_unit,v.t0.xy,lod[alpha_unit],alpha_sample))
        return RSX_SCENE_ALPHA_SAMPLE_UNAVAILABLE;
    precise float a=(v.col0.w*alpha_sample.w)*2.0;
    float scale,reference;
    if(!rsx_scene_alpha_component(fp_base,c.scale,0,scale) ||
       !rsx_scene_alpha_component(fp_base,c.constants,0,reference))return RSX_SCENE_ALPHA_INVALID;
    precise float raw;
    if(c.kind==0)raw=a*scale;
    else {
        vec4 texture_normal,decode;
        if(!rsx_scene_alpha_sample(material_id,0,v.t0.zw,lod.x,texture_normal))return RSX_SCENE_ALPHA_SAMPLE_UNAVAILABLE;
        if(!rsx_scene_alpha_load_constant(fp_base+c.decode,decode))return RSX_SCENE_ALPHA_INVALID;
        precise vec3 decoded=texture_normal.xyz*decode.x+decode.y;
        vec3 n,view;
        if(!rsx_scene_alpha_normalize3(decoded,n) || !rsx_scene_alpha_normalize3(v.t4.xyz,view))
            return RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED;
        precise float d=rsx_scene_alpha_dot3(n,view);
        if(!rsx_scene_alpha_finite(d))return RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED;
        if(c.kind==1){
            float q_offset,exponent,attenuation,mix_weight,inner_limit,base_weight,lower,upper;
            vec4 power_base;
            if(!rsx_scene_alpha_component(fp_base,c.q,c.q_component,q_offset) ||
               !rsx_scene_alpha_load_constant(fp_base+c.power_base,power_base) ||
               !rsx_scene_alpha_component(fp_base,c.power_exponent,2,exponent) ||
               !rsx_scene_alpha_component(fp_base,c.attenuation,3,attenuation) ||
               !rsx_scene_alpha_component(fp_base,c.mix_weight,3,mix_weight) ||
               !rsx_scene_alpha_component(fp_base,c.inner_limit,c.inner_component,inner_limit) ||
               !rsx_scene_alpha_component(fp_base,c.base_weight,3,base_weight) ||
               !rsx_scene_alpha_component(fp_base,c.lower,0,lower) ||
               !rsx_scene_alpha_component(fp_base,c.upper,0,upper))return RSX_SCENE_ALPHA_INVALID;
            precise float q=(-abs(d))+q_offset;
            if(!rsx_scene_alpha_finite(q))return RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED;
            q=min(max(q,0.0),1.0);
            precise float f0=q*power_base.y+power_base.x;
            if(!rsx_scene_alpha_finite(f0) || f0<=0 || !rsx_scene_alpha_finite(exponent))return RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED;
            precise float f1=log2(f0);
            precise float f2=f1*exponent;
            precise float f=exp2(f2);
            if(!rsx_scene_alpha_finite(a) || !rsx_scene_alpha_finite(f1) ||
               !rsx_scene_alpha_finite(f2) || !rsx_scene_alpha_finite(f))return RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED;
            precise float product=a*f;
            precise float sum=a+f;
            precise float b=sum*(-attenuation)+sum;
            precise float w0=product*mix_weight+b;
            if(!rsx_scene_alpha_clamp_operands(w0,inner_limit))return RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED;
            precise float w1=min(w0,inner_limit);
            precise float w2=a*base_weight+w1;
            if(!rsx_scene_alpha_clamp_operands(w2,lower))return RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED;
            precise float w3=max(w2,lower);
            if(!rsx_scene_alpha_clamp_operands(w3,upper))return RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED;
            precise float w4=min(w3,upper);
            raw=w4*scale;
        }else {
            float k_bias,k_min,r_bias,base_weight,lower,upper;
            if(!rsx_scene_alpha_component(fp_base,3,3,k_bias) || !rsx_scene_alpha_component(fp_base,4,0,k_min) ||
               !rsx_scene_alpha_component(fp_base,6,2,r_bias) || !rsx_scene_alpha_component(fp_base,19,3,base_weight) ||
               !rsx_scene_alpha_component(fp_base,20,0,lower) || !rsx_scene_alpha_component(fp_base,21,0,upper))
                return RSX_SCENE_ALPHA_INVALID;
            precise float k0=d+k_bias;
            if(!rsx_scene_alpha_clamp_operands(k0,k_min))return RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED;
            precise float k=max(k0,k_min)*2.0;vec3 light;
            if(!rsx_scene_alpha_normalize3(v.t3.xyz,light))return RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED;
            precise float r0=rsx_scene_alpha_dot3(n,light)/2.0;
            precise float r=r0+r_bias;
            precise float f=k*r;
            precise float w0=a*(-f)+a;
            precise float w1=w0+f;
            precise float w2=a*base_weight+w1;
            if(!rsx_scene_alpha_clamp_operands(w2,lower))return RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED;
            precise float w3=max(w2,lower);
            if(!rsx_scene_alpha_clamp_operands(w3,upper))return RSX_SCENE_ALPHA_NUMERIC_UNSUPPORTED;
            precise float w4=min(w3,upper);
            raw=w4*scale;
        }
    }
    RsxSceneAlphaResult answer;answer.raw_alpha=raw;answer.alpha=isnan(raw)?0.0:raw;
    answer.reference=reference;answer.keep=answer.alpha>reference?1:0;
    result=answer;return RSX_SCENE_ALPHA_OK;
}
int rsx_scene_alpha_evaluate(uint contract_id,uint material_id,uint fp_base,uint fp_count,
    RsxSceneAlphaVertexOutput interpolated,vec2 explicit_lod,inout RsxSceneAlphaResult result)
{
    int status=rsx_scene_alpha_evaluate_impl(contract_id,material_id,fp_base,fp_count,interpolated,explicit_lod,result);
    if(status!=RSX_SCENE_ALPHA_OK)rsx_scene_alpha_invalidate_epoch(material_id,status);
    return status;
}
#endif
