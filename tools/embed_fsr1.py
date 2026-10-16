"""Compile pinned AMD FSR1 wrappers and validate their Vulkan 1.0 interface."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler',required=True)
p.add_argument('--validator',required=True)
p.add_argument('--disassembler',required=True)
p.add_argument('--work-dir',type=Path,required=True)
args=p.parse_args()
root=Path(__file__).resolve().parents[1]
stage=args.work_dir.resolve();stage.mkdir(parents=True,exist_ok=True)
vendor=root/'third_party/fsr1'
provenance=json.loads((vendor/'source.json').read_text(encoding='utf-8-sig'))
for f in provenance['files']:
    if hashlib.sha256((vendor/f['name']).read_bytes()).hexdigest()!=f['sha256']:
        raise SystemExit('Pinned vendor hash mismatch: '+f['name'])

def run(command,log):
    result=subprocess.run(command,capture_output=True,text=True)
    (stage/log).write_text(result.stdout+result.stderr)
    if result.returncode:
        raise SystemExit('Command failed: '+str(command)+'\n'+(result.stdout+result.stderr)[-2000:])
    return result.stdout

records=[];arrays=[]
for name,symbol in [('fsr1.vert','rsx_fsr1_vs_spv'),('fsr1_easu.frag','rsx_fsr1_easu_spv'),('fsr1_rcas.frag','rsx_fsr1_rcas_spv')]:
    shader=root/'libs/video/shaders'/name
    output=stage/(name+'.spv')
    command=[args.compiler,'-V','--target-env','vulkan1.0','-I'+str(vendor),str(shader),'-o',str(output)]
    run(command,name+'.compile.log')
    run([args.validator,'--target-env','vulkan1.0',str(output)],name+'.validate.log')
    assembly=run([args.disassembler,str(output)],name+'.spvasm')
    capabilities=re.findall(r'OpCapability (\w+)',assembly)
    # ImageQuery is a core Shader capability used by textureSize for RCAS bounds.
    assert 'Shader' in capabilities and set(capabilities)<={'Shader','ImageQuery'},(name,capabilities)
    assert 'OpImageWrite' not in assembly
    if name.endswith('.frag'):
        assert re.findall(r'OpDecorate %source_texture DescriptorSet (\d+)',assembly)==['0']
        assert re.findall(r'OpDecorate %source_texture Binding (\d+)',assembly)==['0']
        assert re.findall(r'OpDecorate %out_color Location (\d+)',assembly)==['0']
        block='EasuConstants' if 'easu' in name else 'RcasConstants'
        offsets=[int(x) for x in re.findall(r'OpMemberDecorate %'+block+r' \d+ Offset (\d+)',assembly)]
        assert offsets==([0,16,32,48] if 'easu' in name else [0]),(name,offsets)
        assert len(re.findall(r'OpVariable %\w+ PushConstant',assembly))==1
        assert len(re.findall(r'OpVariable %\w+ UniformConstant',assembly))==1
    else:
        assert 'PushConstant' not in assembly and 'UniformConstant' not in assembly
    data=output.read_bytes()
    assert len(data)%4==0 and data[:4]==b'\x03\x02\x23\x07'
    words=struct.unpack('<'+'I'*(len(data)//4),data)
    arrays.append('static const uint32_t '+symbol+'[] = {\n'+''.join('    '+', '.join(f'0x{x:08x}u' for x in words[i:i+8])+',\n' for i in range(0,len(words),8))+'};\n')
    records.append({'shader':name,'source_sha256':hashlib.sha256(shader.read_bytes()).hexdigest(),
        'spirv_sha256':hashlib.sha256(data).hexdigest(),'words':len(words),'capabilities':capabilities,'command':command})
header=root/'libs/video/rsx_fsr1_spv.h'
header.write_text('// Generated AMD FSR1 bytecode. MIT license: third_party/fsr1/LICENSE.txt.\n#pragma once\n#include <stdint.h>\n'+'\n'.join(arrays))
receipt={'passed':True,'scope':'Compilation, SPIR-V validation and interface checks only; no GPU or game rendering proof.',
    'upstream':provenance,'shaders':records,'embedded_sha256':hashlib.sha256(header.read_bytes()).hexdigest(),
    'tools':{name:run([exe,'--version'],name+'-version.log').strip() for name,exe in [('glslang',args.compiler),('validator',args.validator),('disassembler',args.disassembler)]}}
(stage/'shader-receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
print(json.dumps({'passed':True,'shaders':[{k:r[k] for k in ['shader','words','capabilities']} for r in records]}))
