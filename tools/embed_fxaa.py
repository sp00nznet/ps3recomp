from pathlib import Path
import argparse,hashlib,json,subprocess,struct
parser=argparse.ArgumentParser(description='Regenerate embedded FXAA shader bytecode from pinned vendor source.')
parser.add_argument('--compiler',required=True)
parser.add_argument('--work-dir',type=Path,required=True)
args=parser.parse_args()
root=Path(__file__).resolve().parents[1]
stage=args.work_dir.resolve(); stage.mkdir(parents=True,exist_ok=True)
vendor=root/'third_party/fxaa/Fxaa3_11.h'
provenance=json.loads((vendor.parent/'source.json').read_text(encoding='utf-8-sig'))
assert hashlib.sha256(vendor.read_bytes()).hexdigest()==provenance['sha256']
source=vendor.read_text()
changes=[('FxaaFloat FxaaLuma(FxaaFloat4 rgba) { return rgba.w; }',
          'FxaaFloat FxaaLuma(FxaaFloat4 rgba) { return dot(rgba.rgb, FxaaFloat3(0.299, 0.587, 0.114)); }',1),
         ('#define lumaM rgbyM.w','#define lumaM FxaaLuma(rgbyM)',2)]
for old,new,count in changes:
    assert source.count(old)==count,(old,source.count(old))
    source=source.replace(old,new)
vertex='''#version 450
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
'''
prefix='''#version 450
// FXAA3.11 quality29, full RGB luma; original vendor header remains untouched.
#define FXAA_PC 1
#define FXAA_GLSL_130 1
#define FXAA_QUALITY__PRESET 29
#define FXAA_GATHER4_ALPHA 0
#define FXAA_GREEN_AS_LUMA 0
#define FXAA_LUMA_SEPARATE_R8 0
#define FXAA_DISCARD 0
'''
suffix='''
layout(set=0,binding=0) uniform sampler2D frame_texture;
layout(push_constant) uniform Frame { vec2 reciprocal_size; } frame;
layout(location=0) out vec4 out_color;
void main() {
    vec2 uv = gl_FragCoord.xy * frame.reciprocal_size;
    vec4 filtered = FxaaPixelShader(uv, vec4(0), frame_texture, frame_texture,
        frame_texture, frame_texture, frame.reciprocal_size, vec4(0), vec4(0), vec4(0),
        0.75, 0.125, 0.0312, 0.0, 0.0, 0.0, vec4(0));
    // The algorithm temporarily uses alpha for luma. Preserve guest alpha exactly.
    out_color = vec4(filtered.rgb, texelFetch(frame_texture, ivec2(gl_FragCoord.xy), 0).a);
}
'''
(stage/'fxaa.vert').write_text(vertex)
(stage/'fxaa.frag').write_text(prefix+source+suffix)
compiler=args.compiler
records=[]; arrays=[]
for extension,symbol in [('vert','rsx_fxaa_vs_spv'),('frag','rsx_fxaa_fs_spv')]:
    shader=stage/('fxaa.'+extension); output=shader.with_suffix(shader.suffix+'.spv')
    command=[compiler,'-V','--target-env','vulkan1.0','-S',extension,str(shader),'-o',str(output)]
    result=subprocess.run(command,capture_output=True,text=True)
    (stage/('compile-'+extension+'.log')).write_text(result.stdout+result.stderr)
    print(extension,result.returncode,(result.stdout+result.stderr)[-1200:])
    assert result.returncode==0
    data=output.read_bytes();assert len(data)%4==0 and data[:4]==b'\x03\x02\x23\x07'
    words=struct.unpack('<'+'I'*(len(data)//4),data)
    arrays.append('static const uint32_t '+symbol+'[] = {\n'+''.join('    '+', '.join(f'0x{x:08x}u' for x in words[i:i+8])+',\n' for i in range(0,len(words),8))+'};\n')
    records.append({'stage':extension,'source_sha256':hashlib.sha256(shader.read_bytes()).hexdigest(),'spirv_sha256':hashlib.sha256(data).hexdigest(),'words':len(words),'command':command})
notice='// Generated FXAA3.11 bytecode. NVIDIA copyright and license: third_party/fxaa/Fxaa3_11.h.\n#pragma once\n#include <stdint.h>\n'
(root/'libs/video/rsx_fxaa_spv.h').write_text(notice+'\n'.join(arrays))
(stage/'shader-receipt.json').write_text(json.dumps({'vendor_sha256':hashlib.sha256(vendor.read_bytes()).hexdigest(),'changes':changes,'shaders':records},indent=2)+'\n')
