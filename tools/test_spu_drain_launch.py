#!/usr/bin/env python3
"""Real launch/IRQ/guard preservation, with optional temporary bug controls."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SPU = ROOT / 'runtime/spu'
UNITS = ['spu_channels', 'spu_lockstep', 'spu_coherency', 'spu_workload',
         'spurs_policy', 'spurs_job', 'spu_tsp_weak', 'spu_vm_pagemap',
         'spurs_policy_blob_weak', 'spu_interp']
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutants', action='store_true')
args = parser.parse_args()
env = {k: v for k, v in os.environ.items()
       if not k.startswith(('SPU_', 'SPURS_', 'YDKJ_', 'LBP_', 'GH3_'))}
source = (SPU / 'spu_drain.c').read_text()
# CI has already configured its compiler matrix before discovering this suite.
# Honor explicit overrides, then use that configured compiler rather than `cc`
# (which would silently run GCC in both Linux matrix entries).
cc = os.environ.get('CC')
cache = ROOT / 'build/CMakeCache.txt'
if not cc and cache.is_file():
    match = re.search(r'^CMAKE_C_COMPILER:[^=]+=([^\r\n]+)$', cache.read_text(), re.M)
    if match:
        configured = match[1]
        # clang-cl uses MSVC driver flags; clang shares its backend/target and
        # accepts the portable flags used by this standalone fixture.
        if Path(configured).stem.lower() == 'clang-cl':
            configured = str(Path(configured).with_name('clang' + Path(configured).suffix))
        cc = configured
if not cc:
    cc = shutil.which('clang') if os.name == 'nt' else 'cc'
assert cc, 'a C compiler is required'
print('drain preservation compiler:', cc, flush=True)


def depth_history(result):
    found = re.search(r'\[spu-depth\] recent drain sites[^:]*:([^\n]*)', result.stderr)
    assert found, result.stdout + result.stderr
    return [int(pc, 16) for pc in found[1].split()]


with tempfile.TemporaryDirectory(prefix='spu-drain-launch-') as td:
    tmp = Path(td)

    def run(name, drain, *, sanitize=False, diagnostics=False, debug=False,
            lockstep=False, depth_only=False):
        path = tmp / (name + '.c')
        path.write_text(drain)
        exe = tmp / (name + ('.exe' if os.name == 'nt' else ''))
        flags = ['-std=gnu17', '-O0' if debug else '-O2', '-UNDEBUG',
                 '-I' + str(ROOT / 'include'), '-I' + str(SPU),
                 '-I' + str(ROOT / 'runtime/platform')]
        if sanitize:
            flags += ['-fsanitize=undefined', '-fno-sanitize-recover=all']
        command = [cc, *flags, str(SPU / 'tests/test_spu_drain_launch.c'), str(path),
                   *[str(SPU / (unit + '.c')) for unit in UNITS],
                   str(ROOT / 'runtime/platform/win32_compat.c'),
                   *([] if os.name == 'nt' else ['-pthread', '-lm']), '-o', str(exe)]
        built = subprocess.run(command, capture_output=True, text=True, timeout=60)
        assert built.returncode == 0, built.stdout + built.stderr
        testenv = dict(env)
        if diagnostics:
            testenv.update(SPU_STEPTRACE='906', SPU_EXITTRACE='906')
        if lockstep:
            testenv.update(SPU_LOCKSTEP='1', SPU_LOCKSTEP_QUANTUM='8')
        mode = ['lockstep'] if lockstep else []
        if depth_only:
            mode = ['depth']
            testenv['SPU_HOST_DEPTH_MAX'] = '8'
        result = subprocess.run([str(exe), *mode], env=testenv, capture_output=True,
                                text=True, timeout=15)
        print(name, result.returncode, result.stdout.strip(), flush=True)
        if name == 'o2':
            cases = [('8', 'depth', [0xA010, 0xA014, 0xA01C, 0xA020]),
                     ('0x8', 'depth', [0xA010, 0xA014, 0xA01C, 0xA020]),
                     ('0', 'depth-small', [0xA000]),
                     ('1', 'depth-small', [0xA000, 0xA004]),
                     (None, 'depth-default', [0xAFA0, 0xAFA4, 0xBF3C, 0xBF40])]
            for limit, argument, want in cases:
                depthenv = dict(env)
                if limit is not None:
                    depthenv['SPU_HOST_DEPTH_MAX'] = limit
                probe = subprocess.run([str(exe), argument], env=depthenv,
                                       capture_output=True, text=True, timeout=15)
                assert probe.returncode == 0, probe.stdout + probe.stderr
                assert depth_history(probe) == want, probe.stderr
                print('PASS depth ring, limit=' + str(limit), flush=True)
        return result

    modes = [('o2', {}), ('debug', {'debug': True}),
             ('diagnostics', {'diagnostics': True}), ('lockstep', {'lockstep': True})]
    if os.name != 'nt':
        modes.append(('ubsan', {'sanitize': True}))
    for name, options in modes:
        result = run(name, source, **options)
        assert result.returncode == 0, result.stdout + result.stderr
        if name == 'diagnostics':
            assert '[step]' in result.stderr and '[spu-exit]' in result.stderr, result.stderr
    if args.mutants:
        history_line = next(line for line in source.splitlines()
                            if ('.pc[' in line or '->pc[' in line)
                            and 'SPU_LS_MASK' in line and '++' in line)
        # Each temporary edit removes a real side effect; expected values are
        # literal history/steps/register assertions in the compiled fixture.
        controls = {
            'history': (history_line, ''),
            'steps': ('ctx->steps++;', '/* missing hot step increment */'),
            'reset': ('spu_check_stack_reset(ctx, (void (*)(spu_context*))fn);', '(void)fn;'),
            'irq': ('if (ctx->int_enable && (ctx->event_status & ctx->event_mask)) {', 'if (0) {'),
            'flow': ('g_pm_flow_buf[g_pm_flow_n++] = ctx->pc;', '(void)ctx;'),
            'lockstep': ('yz_lockstep_tick(ctx);', '(void)ctx;'),
        }
        for name, (old, new) in controls.items():
            assert source.count(old) == 1, (name, source.count(old))
            result = run('mutant-' + name, source.replace(old, new))
            assert result.returncode != 0 and 'FAIL:' in result.stdout, result.stdout + result.stderr
            print('PASS rejected bug control:', name, flush=True)
        old = 'if (ctx->host_depth < s_max / 2) return;'
        assert source.count(old) == 1
        result = run('mutant-half-ring', source.replace(old, 'if (ctx->host_depth < s_max) return;'),
                     depth_only=True)
        assert result.returncode != 0 or depth_history(result) != [0xA010, 0xA014, 0xA01C, 0xA020]
        print('PASS rejected bug control: half-ring', flush=True)
    print('PASS real drain launch preservation', flush=True)
