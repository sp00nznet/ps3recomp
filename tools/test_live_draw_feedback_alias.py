#!/usr/bin/env python3
"""Actual Windows GPU current-target feedback with stale size aliases."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

def main():
    if os.name != 'nt':
        print('SKIP: Windows D3D12 fixture'); return 0
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime-library', type=Path, required=True)
    parser.add_argument('--scenario', choices=('all', 'large-first', 'small-first', 'no-alias', 'half', 'other-size', 'other-pitch', 'other-format', 'other-remap', 'newer-covering'), default='all')
    parser.add_argument('--renderer-source', type=Path, help='Optional unchanged renderer source for red/green comparison')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    compiler = shutil.which(os.environ.get('CC', 'clang-cl'))
    if not compiler or not args.runtime_library.is_file(): parser.error('Native compiler and /MT runtime library required')
    with tempfile.TemporaryDirectory(prefix='live-feedback-alias-') as temp:
        work = Path(temp); objects = []
        for index, source in enumerate((root / 'libs/video/tests/test_live_draw_feedback_alias.c', root / 'runtime/host/host_stubs.c')):
            obj = work / (str(index) + '.obj')
            definitions = []
            if index == 0 and args.renderer_source:
                definitions.append('/DFEEDBACK_RENDERER_SOURCE="' + args.renderer_source.resolve().as_posix() + '"')
            subprocess.run([compiler, '/nologo', '/std:c17', '/O1', '/Gy', '/Gw', '/W0', '/MT',
                '/D_CRT_SECURE_NO_WARNINGS', *definitions, '/I' + str(root / 'include'), '/I' + str(root / 'libs/video'),
                '/I' + str(root / 'runtime'), '/c', str(source), '/Fo' + str(obj)], check=True, timeout=90)
            objects.append(str(obj))
        binary = work / 'fixture.exe'
        libs = ('d3d12', 'dxgi', 'd3dcompiler', 'dxguid', 'gdi32', 'user32', 'ole32', 'ws2_32', 'xinput', 'bcrypt', 'winmm', 'shlwapi', 'synchronization', 'psapi', 'dbghelp')
        subprocess.run([compiler, '/nologo', *objects, str(args.runtime_library.resolve()), '/Fe' + str(binary),
            '/link', '/OPT:REF', *(name + '.lib' for name in libs)], check=True, timeout=90)
        scenarios = ('large-first', 'small-first', 'no-alias', 'half', 'other-size', 'other-pitch', 'other-format', 'other-remap', 'newer-covering') if args.scenario == 'all' else (args.scenario,)
        for mode in ('buffered', 'literal'):
            for scenario in scenarios:
                print('MODE', mode, 'SCENARIO', scenario, flush=True)
                subprocess.run([str(binary), scenario], cwd=work, env=dict(os.environ, RSX_FP_CONSTANT_MODE=mode), check=True, timeout=60)
    return 0

if __name__ == '__main__': raise SystemExit(main())
