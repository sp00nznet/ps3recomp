'''terrain-source-parity: provenance gate for libs/video/rsx_terrain_rgb.glsl.

Usage: python tests/test_terrain_rgb_source.py SOURCE.hlsl [SHADER.glsl]
The pipe90 source is pinned by SHA256 of its LF-normalized text. Its ordered register statements
are mechanically translated to GLSL and must appear verbatim, in order, inside the shader's
SOURCE REPLAY block. The only other lines allowed there are '// term' captures and the explicit
'// relight' insertions listed in PLAN. Under pytest the embedded canonical source is used.
'''
import hashlib
import re
import sys
from pathlib import Path

DEFAULT_GLSL = Path(__file__).resolve().parents[1] / 'libs' / 'video' / 'rsx_terrain_rgb.glsl'

CANONICAL = r'''struct PSInput {
    float4 position : SV_POSITION; float4 col0 : COLOR0; float4 col1 : COLOR1;
    float4 fog : FOG;
    float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3;
    float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;
};
Texture2D    rsx_tex[16] : register(t0);
SamplerState rsx_samp[16] : register(s0);
cbuffer PSConstants : register(b1) {
    float4 fp_constants[22];
    float4 fp_alpha;
};
struct PSOutput {
    float4 t0 : SV_TARGET0;
    float4 t1 : SV_TARGET1;
    float4 t2 : SV_TARGET2;
};
PSOutput main(PSInput input) {
    /* RSX shader window */
    input.position.xy = input.position.xy * float2(1.0, -1.0) + float2(0.0, 720.0);

    float4 r[48]; float4 h[48];
    [unroll] for (int _i = 0; _i < 48; _i++) { r[_i] = (float4)0; h[_i] = (float4)0; }
    float4 cc0 = (float4)0; float4 cc1 = (float4)0;
    { float4 _v = (float4)(rsx_tex[0].Sample(rsx_samp[0], ((input.tc0).zwzz).xy)); r[0].xyz = _v.xyz; }
    { float4 _v = (float4)(((r[0]).xyzw) * ((fp_constants[0]).xxxx) + ((fp_constants[0]).yyyy)); h[3].xyz = _v.xyz; }
    { float4 _v = (float4)((input.col1).xyzw); h[2].xyzw = _v.xyzw; }
    { float4 _v = (float4)(((h[2]).wwww) * ((fp_constants[1]).xyzw)); h[0].xyz = _v.xyz; }
    { float4 _v = (float4)(float4(normalize(((input.tc4).xyzw).xyz), 1.0)); h[4].xyz = _v.xyz; }
    { float4 _v = (float4)(((h[2]).zzzz) * ((fp_constants[2]).xyzw) + ((h[0]).xyzw)); h[1].xyz = _v.xyz; }
    { float4 _v = (float4)(float4(normalize(((h[3]).xyzw).xyz), 1.0)); h[3].xyz = _v.xyz; }
    { float4 _v = (float4)((-((h[2]).yyyy)) * ((fp_constants[3]).xyzw) + ((fp_constants[3]).xyzw)); h[0].xyz = _v.xyz; }
    { float4 _v = (float4)(dot(((h[3]).xyzw).xyz, ((h[4]).xyzw).xyz)); r[1].y = _v.y; }
    { float4 _v = (float4)((-(abs((r[1]).yyyy))) + ((fp_constants[4]).xxxx)); _v = saturate(_v); r[3].z = _v.z; }
    { float4 _v = (float4)(float4(normalize(((input.tc3).xyzw).xyz), 1.0)); h[4].xyz = _v.xyz; }
    { float4 _v = (float4)(dot(((h[3]).xyzw).xyz, ((h[4]).xyzw).xyz)) / 2.0; r[1].y = _v.y; }
    { float4 _v = (float4)(((r[3]).zwzz) + ((fp_constants[5]).xyxx)); r[3].xy = _v.xy; }
    { float4 _v = (float4)(((r[1]).yyyy) + ((fp_constants[6]).xyzw)); r[3].z = _v.z; }
    { float4 _v = (float4)((fp_constants[7]).xxxx); r[3].w = _v.w; }
    { float4 _v = (float4)(rsx_tex[1].Sample(rsx_samp[1], ((input.tc0).xyzw).xy)); h[4].xyzw = _v.xyzw; }
    { float4 _v = (float4)(float4(normalize(((input.tc7).xyzw).xyz), 1.0)); h[5].xyz = _v.xyz; }
    { float4 _v = (float4)(dot(((h[3]).xyzw).xyz, ((h[5]).xyzw).xyz)) / 2.0; r[1].y = _v.y; }
    { float4 _v = (float4)((fp_constants[8]).xxxx); r[2].w = _v.w; }
    { float4 _v = (float4)(((r[1]).yyyy) + ((fp_constants[9]).xxxx)); r[2].z = _v.z; }
    { float4 _v = (float4)(rsx_tex[4].Sample(rsx_samp[4], ((r[3]).xyzw).xy)); h[3].xyz = _v.xyz; }
    { float4 _v = (float4)(((h[3]).xyzw) * ((fp_constants[10]).zzzz)); h[3].xyz = _v.xyz; }
    { float4 _v = (float4)(rsx_tex[2].Sample(rsx_samp[2], ((r[2]).zwzz).xy)); h[5].x = _v.x; }
    { float4 _v = (float4)(((input.col0).xyzw) * ((h[4]).xyzw)) * 2.0; h[4].xyzw = _v.xyzw; }
    { float4 _v = (float4)(((h[4]).xyzw) * ((h[5]).xxxx)); h[5].xyz = _v.xyz; }
    { float4 _v = (float4)(((h[5]).xyzw) * ((fp_constants[11]).xyzw)); h[6].xyz = _v.xyz; }
    { float4 _v = (float4)(rsx_tex[2].Sample(rsx_samp[2], ((r[3]).zwzz).xy)); h[5].x = _v.x; }
    { float4 _v = (float4)(((h[4]).xyzw) * ((h[5]).xxxx)); h[7].xyz = _v.xyz; }
    { float4 _v = (float4)((-((h[2]).xxxx)) * ((fp_constants[12]).xyzw) + ((fp_constants[12]).xyzw)) / 2.0; h[5].xyz = _v.xyz; }
    { float4 _v = (float4)(((h[7]).xyzw) * ((fp_constants[13]).xyzw) + ((h[6]).xyzw)); h[6].xyz = _v.xyz; }
    { float4 _v = (float4)(((h[4]).xyzw) * ((fp_constants[14]).xyzw) + ((h[6]).xyzw)); h[6].xyz = _v.xyz; }
    { float4 _v = (float4)(((h[4]).xyzw) * ((fp_constants[15]).xyzw) + ((h[4]).xyzw)); h[0].w = _v.w; }
    { float4 _v = (float4)(((h[4]).xyzw) * ((h[1]).xyzw) + ((h[6]).xyzw)); h[1].xyz = _v.xyz; }
    { float4 _v = (float4)(max(((h[1]).xyzw), ((fp_constants[16]).xxxx))); h[1].xyz = _v.xyz; }
    { float4 _v = (float4)(((h[1]).xyzw) + ((fp_constants[17]).xyzw)); h[4].xyz = _v.xyz; }
    { float4 _v = (float4)(rsx_tex[3].Sample(rsx_samp[3], ((input.tc1).zwzz).xy)); h[1].xyz = _v.xyz; }
    { float4 _v = (float4)(((h[1]).xyzw) * ((h[4]).xyzw)); h[4].xyz = _v.xyz; }
    { float4 _v = (float4)(((h[4]).xyzw) * ((h[2]).xxxx) + ((h[5]).xyzw)) * 2.0; h[1].xyz = _v.xyz; }
    { float4 _v = (float4)(((h[1]).xyzw) * ((h[2]).yyyy) + ((h[0]).xyzw)); h[0].xyz = _v.xyz; }
    { float4 _v = (float4)(((h[0]).xyzw) * ((fp_constants[18]).xxxx) + ((h[3]).xyzw)); h[0].xyz = _v.xyz; }
    { float4 _v = (float4)(max(((h[0]).xyzw), ((fp_constants[19]).xxxx))); h[0].xyzw = _v.xyzw; }
    { float4 _v = (float4)(min(((h[0]).wwww), ((fp_constants[20]).xxxx))); h[1].x = _v.x; }
    { float4 _v = (float4)(((h[1]).xxxx) * ((fp_constants[21]).xxxx)); r[0].w = _v.w; }
    { float4 _v = (float4)((h[0]).xyzw); r[0].xyz = _v.xyz; }
    PSOutput _out;
    float4 _o0 = r[0];
    _out.t0 = (_o0 == _o0) ? _o0 : (float4)0;
    float4 _o1 = r[2];
    _out.t1 = (_o1 == _o1) ? _o1 : (float4)0;
    float4 _o2 = r[3];
    _out.t2 = (_o2 == _o2) ? _o2 : (float4)0;
    return _out;
}
'''


def normalized(text):
    return text.lstrip('﻿').replace('\r\n', '\n').replace('\r', '\n').rstrip('\n') + '\n'


PINNED_SHA256 = hashlib.sha256(normalized(CANONICAL).encode('utf-8')).hexdigest()
STATEMENT_COUNT = 44
RULES = [
    (r'rsx_tex\[(\d+)\]\.Sample\(rsx_samp\[\1\], (\(\([^()]*\)\.[xyzw]+\)\.xy)\)', r'rsx_terrain_rgb_tex(material, \1u, \2, lod, ok)'),
    (r'input\.tc(\d)\b', r'v.t\1'),
    (r'input\.(col[01])\b', r'v.\1'),
    (r'fp_constants\[(\d+)\]', r'c[\1]'),
    (r'float4 _v', 'precise vec4 _v'),
    (r'\(float4\)\(', 'vec4('),
    (r'\bfloat4\(', 'vec4('),
    (r'saturate\(_v\)', 'clamp(_v, 0.0, 1.0)'),
]
VIS = re.compile(r'if \(modified && vis\.([xyzw]) != 1\.0\) (h\[\d\]\.[xyzw]+) = \2 \* vis\.\1;')
IND = re.compile(r'if \(modified && any\(notEqual\(ind, vec3\(0\.0\)\)\)\) (h\[\d\]\.xyz) = \1 \+ ind;')
TERM = re.compile(r't\.\w+(?:\[\d\])? = [^;]+;')
# (source statements replayed before the insertion, visibility lane or None for indirect, target, adjacent anchor)
PLAN = [(4, 'w', 'h[0].xyz', 'fp_constants[1]'), (5, 'z', 'h[2].z', 'fp_constants[2]'),
        (26, 'x', 'h[6].xyz', 'fp_constants[11]'), (29, 'y', 'h[7].xyz', 'fp_constants[13]'),
        (33, None, 'h[1].xyz', 'fp_constants[16]')]


def source_statements(src):
    lines = [l.strip() for l in src.split('\n')]
    try:
        start = next(i for i, l in enumerate(lines) if l.startswith('float4 cc0')) + 1
        end = lines.index('PSOutput _out;')
    except (StopIteration, ValueError):
        raise ValueError('source register window not found')
    stmts = [l for l in lines[start:end] if l]
    bad = [s for s in stmts if not re.fullmatch(r'\{ float4 _v = .*\}', s)]
    if bad:
        raise ValueError(f'unparsed source statement: {bad[0]}')
    return stmts


def translate(stmt):
    for pat, rep in RULES:
        stmt = re.sub(pat, rep, stmt)
    if re.search(r'float4|input\.|fp_|rsx_tex|rsx_samp|saturate', stmt):
        raise ValueError(f'untranslatable source statement: {stmt}')
    return ' '.join(stmt.split())


def replay_block(glsl):
    m = re.search(r'// BEGIN SOURCE REPLAY\n(.*?)\n[ \t]*// END SOURCE REPLAY', glsl, re.S)
    if not m:
        raise ValueError('GLSL SOURCE REPLAY block not found')
    return [' '.join(l.split()) for l in m.group(1).split('\n')]


def compare(src, glsl):
    glsl = normalized(glsl)
    stmts = source_statements(src)
    expected = [translate(s) for s in stmts]
    errs = [] if len(stmts) == STATEMENT_COUNT else [f'source has {len(stmts)} register statements, expected {STATEMENT_COUNT}']
    i, seen = 0, []
    for line in replay_block(glsl):
        if not line or line.startswith('//'):
            continue
        if line.endswith('// term'):
            if not TERM.fullmatch(line[:-len('// term')].rstrip()):
                errs.append(f'term capture may only assign source terms: {line}')
        elif line.endswith('// relight'):
            body = line[:-len('// relight')].rstrip()
            m, n = VIS.fullmatch(body), IND.fullmatch(body)
            if m:
                seen.append((i, m.group(1), m.group(2)))
            elif n:
                seen.append((i, None, n.group(1)))
            else:
                errs.append(f'relight insertion not of an allowed form: {line}')
        elif i < len(expected) and line == expected[i]:
            i += 1
        else:
            errs.append(f'GLSL line does not replay source statement {i + 1}: {line}')
    if i < len(expected):
        errs.append(f'missing source statement {i + 1}: {expected[i]}')
    plan = [p[:3] for p in PLAN]
    if seen != plan:
        errs.append(f'relight insertions {seen} != plan {plan}')
    if len(stmts) == STATEMENT_COUNT:
        for pos, _, target, anchor in PLAN:
            near = stmts[pos - 1] + stmts[pos]
            if anchor not in near or target.split('.')[0] not in near:
                errs.append(f'relight anchor {anchor}/{target} not adjacent to source statement {pos}')
    n = re.search(r'float4 fp_constants\[(\d+)\];\s*float4 fp_alpha;', src)
    if not n or f'#define RSX_TERRAIN_RGB_CONSTANT_COUNT {int(n.group(1)) + 1}u' not in glsl:
        errs.append('constant count does not match source cbuffer (constants plus alpha)')
    units = {int(u) for u in re.findall(r'rsx_tex\[(\d+)\]\.Sample', src)}
    if f'float lod[{max(units, default=-1) + 1}]' not in glsl:
        errs.append('LOD array does not cover every sampled unit')
    outs = re.findall(r'float4 _o(\d) = (r\[\d+\]);', src)
    if len(outs) != src.count('SV_TARGET'):
        errs.append('source output registers not parsed')
    for k, reg in outs:
        if f'_out.t{k} = (_o{k} == _o{k}) ? _o{k} : (float4)0;' not in src or f'o{k} = rsx_terrain_rgb_nan0({reg});' not in glsl:
            errs.append(f'MRT{k} must be the per-channel NaN-sanitized {reg}')
    if glsl.count('normalize(') != sum(s.count('normalize(') for s in stmts):
        errs.append('normalize use differs from source (no direction repair allowed)')
    for needle in ('result.mrt[3] = vec4(0.0);', 'terms.reflectionValid = false;'):
        if needle not in glsl:
            errs.append(f'missing {needle}')
    return errs


def check(source_text, glsl_text):
    src = normalized(source_text)
    digest = hashlib.sha256(src.encode('utf-8')).hexdigest()
    errs = [] if digest == PINNED_SHA256 else [f'source sha256 {digest} does not match pinned {PINNED_SHA256}']
    try:
        return errs + compare(src, glsl_text)
    except ValueError as e:
        return errs + [str(e)]


def _glsl():
    return DEFAULT_GLSL.read_text(encoding='utf-8')


def test_terrain_source_parity():
    assert check(CANONICAL, _glsl()) == []


def test_rejects_source_mutation():
    mutated = CANONICAL.replace('((h[7]).xyzw) * ((fp_constants[13])', '((h[7]).xyzw) * ((fp_constants[14])')
    assert mutated != CANONICAL
    errs = check(mutated, _glsl())
    assert any('sha256' in e for e in errs) and any('statement 30' in e for e in errs)


def test_rejects_untagged_or_regrouped_glsl():
    glsl = _glsl()
    for bad in (glsl.replace('h[1].xyz + ind; // relight', 'h[1].xyz + ind;'),
                glsl.replace('h[6].xyz = h[6].xyz * vis.x; // relight', 'h[6].xyz = h[5].xyz * c[11].xyz * vis.x; // relight'),
                glsl.replace('((c[13]).xyzw) + ((h[6]).xyzw))', '((c[13]).xyzw * vis.y) + ((h[6]).xyzw))')):
        assert bad != glsl and check(CANONICAL, bad)


def main(argv):
    if not 1 <= len(argv) <= 2:
        print('usage: test_terrain_rgb_source.py SOURCE.hlsl [SHADER.glsl]', file=sys.stderr)
        return 2
    glsl = Path(argv[1]) if len(argv) > 1 else DEFAULT_GLSL
    errs = check(Path(argv[0]).read_text(encoding='utf-8'), glsl.read_text(encoding='utf-8'))
    for e in errs:
        print(f'terrain-source-parity: {e}', file=sys.stderr)
    if not errs:
        print(f'terrain-source-parity: ok sha256={PINNED_SHA256}')
    return 1 if errs else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
