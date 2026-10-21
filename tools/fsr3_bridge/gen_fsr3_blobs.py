"""Generate FidelityFX SDK 1.1.4 FSR3-upscaler permutation headers without FidelityFX_SC.

gen_fsr3_blobs.py SDK_ROOT OUTPUT_DIR --tools DIR [--jobs N]   (DIR holds glslangValidator, spirv-val, spirv-cross, spirv-dis)

Same inputs as the SDK's own GLSL build (sdk/src/backends/vk/CMakeShadersFSR3Upscaler.txt
and gpu/fsr3upscaler/CMakeCompileFSR3UpscalerShaders.txt): every pass in
sdk/src/backends/vk/shaders/fsr3upscaler, all 64 option permutations, FFX_HALF 0 and 1,
glslang -Os for Vulkan 1.2. For GLSL the SDK compiles its wave64 variants with the
wave32 arguments, so those headers alias the same SPIR-V. Each blob passes spirv-val.
Bindings come from spirv-cross reflection; resource names are the SPIR-V variable
names (the FSR3 component binds its constant buffer by instance name, cbFSR3Upscaler).
Writes *_permutations.h headers plus generation-manifest.json (tool versions, SPIR-V hashes).
"""
import argparse
import concurrent.futures
import hashlib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

TOOLS = None  # set from --tools
BASE_DEFINES = ['FFX_GPU=1', 'FFX_GLSL=1',
                'FFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0',
                'FFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0',
                'FFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1',
                'FFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0',
                'FFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2']
OPTIONS = ['FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE',
           'FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT',
           'FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS',
           'FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS',
           'FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH',
           'FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING']
# FfxShaderBlob categories in POPULATE_SHADER_BLOB_FFX order.
CATEGORIES = [('constantBuffer', 'numConstantBuffers'), ('srvTexture', 'numSRVTextures'),
              ('uavTexture', 'numUAVTextures'), ('srvBuffer', 'numSRVBuffers'),
              ('uavBuffer', 'numUAVBuffers'), ('sampler', 'numSamplers'),
              ('rtAccelerationStructure', 'numRTAccelerationStructures')]


def exe(name):
    return name + '.exe' if sys.platform == 'win32' else name


def run(args, **kw):
    return subprocess.run([str(a) for a in args], capture_output=True, text=True, **kw)


def variable_names(spv):
    """(set, binding) -> SPIR-V variable name, from the disassembly."""
    text = run([TOOLS / exe('spirv-dis'), '--raw-id', spv]).stdout
    names = dict(re.findall(r'OpName %(\d+) "([^"]*)"', text))
    sets = dict(re.findall(r'OpDecorate %(\d+) DescriptorSet (\d+)', text))
    binds = dict(re.findall(r'OpDecorate %(\d+) Binding (\d+)', text))
    return {(int(sets.get(i, 0)), int(b)): names.get(i, '') for i, b in binds.items()}


def reflect(spv):
    j = json.loads(run([TOOLS / exe('spirv-cross'), spv, '--reflect'], check=True).stdout)
    names = variable_names(spv)
    out = {c: [] for c, _ in CATEGORIES}

    def add(cat, r):
        count = 1
        for n in r.get('array', []) or []:
            count *= n
        key = (r.get('set', 0), r['binding'])
        out[cat].append((r['binding'], names.get(key) or r['name'], count, r.get('set', 0)))
    for r in j.get('ubos', []):
        add('constantBuffer', r)
    for r in j.get('separate_images', []):
        if 'Buffer' in r['type']:
            raise SystemExit(f'unsupported texel buffer {r}')
        add('srvTexture', r)
    for r in j.get('images', []):
        if 'Buffer' in r['type']:
            raise SystemExit(f'unsupported storage texel buffer {r}')
        add('uavTexture', r)
    for r in j.get('ssbos', []):
        add('srvBuffer' if r.get('readonly') else 'uavBuffer', r)
    for r in j.get('separate_samplers', []):
        add('sampler', r)
    for r in j.get('acceleration_structures', []):
        add('rtAccelerationStructure', r)
    for k in ('textures', 'push_constants', 'subpass_inputs'):
        if j.get(k):
            raise SystemExit(f'unsupported resource class {k}: {j[k]}')
    for c in out:
        out[c].sort()
    return out


def compile_one(sdk, shader, perm, half, tmp):
    gpu = sdk / 'sdk/include/FidelityFX/gpu'
    defines = BASE_DEFINES + [f'{o}={(perm >> i) & 1}' for i, o in enumerate(OPTIONS)] + [f'FFX_HALF={half}']
    spv = tmp / f'{shader.stem}_{half}_{perm}.spv'
    r = run([TOOLS / exe('glslangValidator'), '-V', '--target-env', 'vulkan1.2', '-S', 'comp', '-Os',
             *[f'-D{d}' for d in defines], f'-I{gpu}', f'-I{gpu / "fsr3upscaler"}', shader, '-o', spv])
    if r.returncode:
        raise SystemExit(f'glslang failed: {shader.name} half={half} perm={perm}\n{r.stdout}{r.stderr}')
    v = run([TOOLS / exe('spirv-val'), '--target-env', 'vulkan1.2', spv])
    if v.returncode:
        raise SystemExit(f'spirv-val failed: {spv}\n{v.stdout}{v.stderr}')
    data = spv.read_bytes()
    return perm, data


def c_array(name, values, ctype='uint32_t'):
    if not values:
        return None
    return f'static const {ctype} {name}[] = {{ {", ".join(values)} }};'


def emit(base, variant, blobs, table):
    lines = [f'// Generated by gen_fsr3_blobs.py for {variant}: FidelityFX SDK 1.1.4 (c6efa6bf),',
             '// local glslang. Not produced by FidelityFX_SC. Do not edit.', '#pragma once', '#include <stdint.h>', '',
             f'#ifndef FFX_GEN_{base.upper()}_PERMUTATION_KEY',
             f'#define FFX_GEN_{base.upper()}_PERMUTATION_KEY',
             f'typedef union {base}_PermutationKey {{', '    struct {']
    lines += [f'        uint32_t {o} : 1;' for o in OPTIONS]
    lines += ['    };', '    uint32_t index;', f'}} {base}_PermutationKey;', '#endif', '']
    fields = ['    const uint32_t blobSize;', '    const unsigned char* blobData;']
    for cat, num in CATEGORIES:
        fields += [f'    const uint32_t {num};', f'    const char** {cat}Names;', f'    const uint32_t* {cat}Bindings;',
                   f'    const uint32_t* {cat}Counts;', f'    const uint32_t* {cat}Spaces;']
    lines += [f'typedef struct {variant}_PermutationInfo {{', *fields, f'}} {variant}_PermutationInfo;', '']
    infos = []
    for n, (data, res) in enumerate(blobs):
        tag = f'g_{variant}_{n}'
        words = [f'0x{b:02x}' for b in data]
        lines.append(f'static const unsigned char {tag}_Data[] = {{')
        lines += ['    ' + ','.join(words[i:i + 24]) + ',' for i in range(0, len(words), 24)]
        lines.append('};')
        entry = [str(len(data)), f'{tag}_Data']
        for cat, _ in CATEGORIES:
            items = res[cat]
            if items:
                lines.append(f'static const char* {tag}_{cat}Names[] = {{ {", ".join(json.dumps(i[1]) for i in items)} }};')
                lines.append(c_array(f'{tag}_{cat}Bindings', [str(i[0]) for i in items]))
                lines.append(c_array(f'{tag}_{cat}Counts', [str(i[2]) for i in items]))
                lines.append(c_array(f'{tag}_{cat}Spaces', [str(i[3]) for i in items]))
                entry += [str(len(items)), f'{tag}_{cat}Names', f'{tag}_{cat}Bindings', f'{tag}_{cat}Counts', f'{tag}_{cat}Spaces']
            else:
                entry += ['0', '0', '0', '0', '0']
        infos.append('    { ' + ', '.join(entry) + ' },')
        lines.append('')
    lines += [f'static const {variant}_PermutationInfo g_{variant}_PermutationInfo[] = {{', *infos, '};', '',
              f'static const int32_t g_{variant}_IndirectionTable[] = {{ {", ".join(str(t) for t in table)} }};', '']
    return '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('sdk', type=Path)
    ap.add_argument('out', type=Path)
    ap.add_argument('--tools', type=Path, required=True)
    ap.add_argument('--jobs', type=int, default=12)
    a = ap.parse_args()
    global TOOLS
    TOOLS = a.tools.resolve()
    sdk, out = a.sdk.resolve(), a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    shaders = sorted((sdk / 'sdk/src/backends/vk/shaders/fsr3upscaler').glob('*.glsl'))
    manifest = dict(tool='gen_fsr3_blobs.py', sdk_commit='c6efa6bf7f2027b3ec94f28578bb5965eabb9e55',
                    glslang=run([TOOLS / exe('glslangValidator'), '--version']).stdout.splitlines()[0],
                    spirv_cross_reflect=True, base_defines=BASE_DEFINES, options=OPTIONS,
                    shader_sources={s.name: hashlib.sha256(s.read_bytes()).hexdigest() for s in shaders}, headers={})
    with tempfile.TemporaryDirectory(prefix='ffx-fsr3-', dir=out.parent) as t, \
            concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        tmp = Path(t)
        for shader in shaders:
            base = shader.stem
            for half in (0, 1):
                results = dict(pool.map(lambda p: compile_one(sdk, shader, p, half, tmp), range(64)))
                uniq, table, blobs = {}, [], []
                for perm in range(64):
                    data = results[perm]
                    h = hashlib.sha256(data).hexdigest()
                    if h not in uniq:
                        spv = tmp / f'{shader.stem}_{half}_{perm}.spv'
                        uniq[h] = len(blobs)
                        blobs.append((data, reflect(spv)))
                    table.append(uniq[h])
                own, alias = (base, base + '_wave64') if half == 0 else (base + '_16bit', base + '_wave64_16bit')
                for variant in (own, alias):
                    path = out / f'{variant}_permutations.h'
                    if variant == own:
                        text = emit(base, variant, blobs, table)
                    else:
                        # ffx_fsr3upscaler_shaderblobs.cpp includes every wave32 header before its
                        # wave64 twin in the same translation unit, so alias rather than duplicate.
                        text = '\n'.join([f'// Generated by gen_fsr3_blobs.py: GLSL wave64 = wave32 build of {own}.',
                                          '#pragma once', f'#include "{own}_permutations.h"',
                                          f'#define g_{variant}_PermutationInfo g_{own}_PermutationInfo',
                                          f'#define g_{variant}_IndirectionTable g_{own}_IndirectionTable', ''])
                    path.write_text(text, encoding='utf-8')
                    manifest['headers'][path.name] = dict(sha256=hashlib.sha256(text.encode()).hexdigest(),
                                                          unique_blobs=len(blobs),
                                                          blob_sha256=sorted(uniq))
                print(json.dumps(dict(shader=shader.name, half=half, unique_blobs=len(blobs))), flush=True)
    (out / 'generation-manifest.json').write_text(json.dumps(manifest, indent=1), encoding='utf-8')


if __name__ == '__main__':
    main()
