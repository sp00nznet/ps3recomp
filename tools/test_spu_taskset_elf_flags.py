#!/usr/bin/env python3
"""Exercise production taskset requeue and ELF parsing with synthetic tagged ELFs."""
import os
import re
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: fixture currently requires a POSIX C compiler')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
source = (root / 'runtime/spu/spu_workload.c').read_text()

def function(name):
    match = re.search(r'^.*\b' + re.escape(name) + r'\([^;\n]*\)\s*\{', source, re.M)
    if not match:
        raise AssertionError('Missing production function ' + name)
    brace = source.index('{', match.start())
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]

functions = '\n'.join(function(name) for name in
                      ['rd_be16', 'rd_be32', 'spu_elf_image_size', 'ts_running',
                       'ts_lock', 'ts_unlock', 'spu_taskset_task_exited'])
tables = source[source.index('#define TS_TRACK '):
                source.index('static uint8_t* ts_running(')]
fixture = (root / 'runtime/spu/tests/test_spu_taskset_elf_flags.c').read_text()
fixture = fixture.replace('/* PRODUCTION_FUNCTIONS */', tables + functions)
env = {k: v for k, v in os.environ.items()
       if not k.startswith(('SPU_', 'SPURS_', 'YDKJ_', 'LBP_', 'GH3_'))}
with tempfile.TemporaryDirectory(prefix='spu-taskset-elf-flags-') as tmp:
    c = Path(tmp) / 'test.c'
    binary = Path(tmp) / 'test'
    c.write_text(fixture)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-Wall', '-Wextra', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=5, env=env, cwd=tmp)
