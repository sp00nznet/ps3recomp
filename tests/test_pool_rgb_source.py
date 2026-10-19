"""Exact source-coverage gate and deterministic pool91 GLSL emitter. No GPU calls."""
from pathlib import Path
import argparse, hashlib, re
ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT.parent/'capture-20261019T034900Z/shaders/pipe91.ps.hlsl'
TARGET=ROOT/'libs/video/rsx_pool_rgb.glsl'
RAW='46e66b9feb7f93cff81ce5e9ccd84ef64c9eb35f0e78b44d73b25aedd69d4285'
LF='6e59f9da9d300213eb50ab8124ca5d5d7e4b6cf177ffc7dbb96c0c544db3d2f6'
HEADER='''/* Exact pool91 full PS raw SHA256:
 * 46e66b9feb7f93cff81ce5e9ccd84ef64c9eb35f0e78b44d73b25aedd69d4285
 * LF alias:6e59f9da9d300213eb50ab8124ca5d5d7e4b6cf177ffc7dbb96c0c544db3d2f6
 * Generated/checked by tests/test_pool_rgb_source.py. No resource bindings.
 * Include rsx_scene_alpha.glsl first for RsxSceneAlphaVertexOutput.
 * Host must qualify the exact full source, constants and original sampler
 * semantics. Callbacks must be deterministic, including repeated samples.
 * Explicit LOD is caller-supplied; raster derivative equivalence is not claimed.
 * normalize(0), NaNs, scalar splats and final per-channel sanitization retain
 * original source behavior. No guessed normal repair or early NaN rejection.
 */
#ifndef RSX_POOL_RGB_GLSL
#define RSX_POOL_RGB_GLSL
bool rsx_pool_rgb_load_constant(uint absolute_index,out vec4 value);
bool rsx_pool_rgb_sample2d(uint material,uint unit,vec2 uv,float lod,out vec4 rgba);
bool rsx_pool_rgb_sample_cube(uint material,uint unit,vec3 direction,float lod,out vec4 rgba);
struct RsxPoolRgbTerms {
    vec3 direct[4]; // order:13,12,20,22; branch products, not a regrouped composer
    vec3 reflectionDirection; // exact source-space cube lookup direction, PS:83
    vec3 diffuseProxy;vec3 diffuseDirect[4]; // authored base*(1-cube weight)
    bool transportValid,reflectionValid; // independent diffuse/direction gates
    vec3 prefloor;vec3 floored;vec3 additive;vec3 overlay;
    vec2 fog_weights;vec3 fog_colors[2];vec3 postfog_additive;
};
struct RsxPoolRgbOutput {
    vec4 originalRGBA;vec4 composedRGBA;vec4 mrt[4];RsxPoolRgbTerms source;
};
vec4 rsx_pool_rgb_sanitize(vec4 x){return mix(x,vec4(0),isnan(x));}
bool rsx_pool_rgb_transport_finite3(vec3 x){return !any(isnan(x))&&!any(isinf(x));}
// Internal exact register replay. Scales are inserted only at direct branches;
// unity scales skip multiplication so the original instruction order survives.
int rsx_pool_rgb_replay(uint material,RsxSceneAlphaVertexOutput v,
    vec4 fp_constants[43],float lod[7],vec4 visibility,vec3 indirect,
    out vec4 mrt[4],out RsxPoolRgbTerms terms){
    precise vec4 r[48];precise vec4 h[48];
    for(int i=0;i<48;++i){r[i]=vec4(0);h[i]=vec4(0);}
    precise vec4 cc0=vec4(0),cc1=vec4(0);
    terms.transportValid=false;terms.reflectionValid=false;
'''
FOOTER='''    mrt[0]=rsx_pool_rgb_sanitize(r[0]);mrt[1]=rsx_pool_rgb_sanitize(r[2]);
    mrt[2]=rsx_pool_rgb_sanitize(r[3]);mrt[3]=rsx_pool_rgb_sanitize(r[4]);return 0;
}
// Failure leaves result unchanged. fp_count includes the declared alpha vec4.
// Original source uses43 constants and no discard; alpha reference is unused.
int rsx_pool_rgb_evaluate(bool qualifiedSource,bool extendedCoordinates,uint material,uint fp_base,uint fp_count,
    RsxSceneAlphaVertexOutput v,float lod[7],vec4 visibility,vec3 indirect,
    inout RsxPoolRgbOutput result){
    if(!qualifiedSource)return -1;
    if(fp_count!=44u || fp_base>0xffffffffu-43u)return -2;
    if(any(isnan(visibility))||any(isinf(visibility))||any(lessThan(visibility,vec4(0)))||
       any(greaterThan(visibility,vec4(1)))||any(isnan(indirect))||any(isinf(indirect)))return -4;
    for(int i=0;i<7;++i)if(isnan(lod[i])||isinf(lod[i]))return -4;
    vec4 constants[43];for(uint i=0u;i<43u;++i)if(!rsx_pool_rgb_load_constant(fp_base+i,constants[i]))return -2;
    if(!extendedCoordinates){v.t8=vec4(0);v.t9=vec4(0);}
    RsxPoolRgbOutput answer;int status=rsx_pool_rgb_replay(material,v,constants,lod,vec4(1),vec3(0),answer.mrt,answer.source);
    if(status!=0)return status;
    answer.originalRGBA=answer.mrt[0];answer.composedRGBA=answer.originalRGBA;
    if(any(notEqual(visibility,vec4(1)))||any(notEqual(indirect,vec3(0)))){
        vec4 modified[4];RsxPoolRgbTerms discarded;
        status=rsx_pool_rgb_replay(material,v,constants,lod,visibility,indirect,modified,discarded);
        if(status!=0)return status;answer.composedRGBA=modified[0];
    }
    result=answer;return 0;
}
#endif
'''
def translate(expr):
    # Replace each balanced HLSL Sample call with a checked, explicit-LOD hook.
    samples=[]
    while True:
        m=re.search(r'rsx_tex(\d+)\.Sample\(rsx_samp\[(\d+)\], ',expr)
        if not m: break
        assert m[1]==m[2]
        pos=m.end();depth=1;end=pos
        while depth:
            if expr[end]=='(':depth+=1
            elif expr[end]==')':depth-=1
            end+=1
        coords=expr[pos:end-1];unit=int(m[1]);assert 0<=unit<=6
        samples.append((unit,coords));expr=expr[:m.start()]+'sampled'+expr[end:]
    assert len(samples)<=1
    expr=expr.replace('(float4)','vec4').replace('float4(', 'vec4(')
    expr=expr.replace('input.tc','v.t').replace('input.col','v.col')
    return expr,samples

def generate(source):
    result=[HEADER];count=0;units=[]
    for line_no,line in enumerate(source.splitlines(),1):
        if '{ float4 _v =' not in line:continue
        m=re.fullmatch(r'\s*\{ float4 _v = (.*?);( _v = saturate\(_v\);)? (.*?) \}',line)
        assert m,(line_no,line)
        expression,samples=translate(m[1]);dest=m[3];count+=1
        if line_no==61:
            assert 'normalize(((vec4(0,0,0,0)).xyzw).xyz)' in expression
            # Captured raw FP instruction21 consumes INPUT0xC=TC8. Legacy
            # sources explicitly zero this appended varying at the call boundary.
            expression=expression.replace('vec4(0,0,0,0)','v.t8')
        anchors={98:'h[3].xyz',102:'h[0].xyz',105:'h[5].xyz',106:'h[5].xyz',127:'h[5].xyz',130:'h[0].xyz',132:'h[6].xyz',138:'h[1].xyz'}
        if line_no in anchors:assert dest.startswith(anchors[line_no]),(line_no,dest)
        result.append(f'    // source line {line_no}, register statement {count}\n')
        # Capture and modify only the original direct-light branch operands.
        if line_no==98:
            result.append('    terms.direct[1]=(h[1].xyz*h[0].xxx)*fp_constants[17].xyz;\n    terms.diffuseDirect[1]=(terms.diffuseProxy*h[0].xxx)*fp_constants[17].xyz;\n    if(visibility.y!=1.0)h[0].x=h[0].x*visibility.y;\n')
        if line_no==102:
            result.append('    terms.direct[0]=(h[1].xyz*h[0].xxx)*fp_constants[16].xyz;\n    terms.diffuseDirect[0]=(terms.diffuseProxy*h[0].xxx)*fp_constants[16].xyz;\n    if(visibility.x!=1.0)h[0].x=h[0].x*visibility.x;\n')
        if line_no==105:result.append('    terms.direct[3]=h[1].xyz*(h[2].www*fp_constants[18].xyz);\n    terms.diffuseDirect[3]=terms.diffuseProxy*(h[2].www*fp_constants[18].xyz);\n')
        if line_no==106:
            result.append('    terms.direct[2]=h[1].xyz*(h[2].zzz*fp_constants[19].xyz);\n    terms.diffuseDirect[2]=terms.diffuseProxy*(h[2].zzz*fp_constants[19].xyz);\n    if(visibility.z!=1.0)h[2].z=h[2].z*visibility.z;\n')
        if line_no==130:result.append('    terms.prefloor=h[0].xyz;\n    if(any(notEqual(indirect,vec3(0))))h[0].xyz=h[0].xyz+indirect;\n')
        result.append('    { ')
        for unit,coords in samples:
            coords=coords.replace('input.tc','v.t').replace('input.col','v.col')
            hook='rsx_pool_rgb_sample_cube' if unit==3 else 'rsx_pool_rgb_sample2d'
            result.append(f'vec4 sampled;if(!{hook}(material,{unit}u,{coords},lod[{unit}],sampled))return -3;')
            units.append(unit)
        result.append('precise vec4 _v='+expression+';')
        if m[2]:result.append('_v=clamp(_v,vec4(0),vec4(1));')
        if line_no==105:result.append('if(visibility.w!=1.0)_v.xyz=_v.xyz*visibility.w;')
        result.append(dest+' }\n')
        if line_no==83:result.append('    terms.reflectionDirection=h[5].xyz;\n    terms.reflectionValid=rsx_pool_rgb_transport_finite3(terms.reflectionDirection)&&dot(terms.reflectionDirection,terms.reflectionDirection)>0.0;\n')
        if line_no==84:result.append('    terms.diffuseProxy=h[3].xyz;\n    terms.transportValid=rsx_pool_rgb_transport_finite3(h[4].xyz)&&rsx_pool_rgb_transport_finite3(terms.diffuseProxy)&&!isnan(h[1].w)&&!isinf(h[1].w)&&h[1].w>=0.0&&h[1].w<=1.0&&all(greaterThanEqual(terms.diffuseProxy,vec3(0)))&&all(lessThanEqual(terms.diffuseProxy,vec3(1)));\n')
        if line_no==106:result.append('    for(int i=0;i<4;++i)terms.transportValid=terms.transportValid&&rsx_pool_rgb_transport_finite3(terms.diffuseDirect[i])&&all(greaterThanEqual(terms.diffuseDirect[i],vec3(0)));\n')
        if line_no==130:result.append('    terms.floored=h[0].xyz;\n')
        if line_no==132:result.append('    terms.additive=h[6].xyz;\n')
        if line_no==138:result.append('    terms.overlay=h[1].xyz;terms.fog_weights=v.col1.xy;terms.fog_colors[0]=fp_constants[25].xyz;terms.fog_colors[1]=fp_constants[36].xyz;\n')
        if line_no==127:result.append('    terms.postfog_additive=h[5].xyz;\n')
    assert count==source.count('{ float4 _v ='),count
    assert set(units)==set(range(7)),units
    result.append(FOOTER)
    return ''.join(result),count,units

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--write',action='store_true');args=parser.parse_args()
    raw=SOURCE.read_bytes();normalized=raw.replace(b'\r\n',b'\n')
    assert hashlib.sha256(raw).hexdigest()==RAW
    assert hashlib.sha256(normalized).hexdigest()==LF
    generated,count,units=generate(normalized.decode())
    if args.write:TARGET.write_text(generated,encoding='utf-8',newline='\n')
    assert TARGET.read_text(encoding='utf-8-sig')==generated
    assert 'normalize(((v.t8).xyzw).xyz)' in generated
    assert 'fp_count!=44u' in generated and 'if(!qualifiedSource)return -1' in generated
    assert generated.count('// source line ')==count
    assert 'answer.composedRGBA=answer.originalRGBA;' in generated
    assert 'terms.direct[0]' in generated and 'terms.direct[3]' in generated
    assert all(f'terms.diffuseDirect[{i}]=' in generated for i in range(4))
    assert 'terms.reflectionDirection=h[5].xyz;' in generated and 'terms.diffuseProxy=h[3].xyz;' in generated
    print(f'PASS pool91 full-source hashes; {count} of {count} register statements; {len(units)} samples, units0..6 including cube3; deterministic translation and unity bypass')
if __name__=='__main__':main()
