#!/usr/bin/env python3
"""SPU decrementer: counts down from the host clock at 79.8 MHz, at fine resolution.

Builds runtime/spu/tests/test_spu_decrementer.c against the SPU channel layer
(the build line of test_spu_taskset_wake.c) and runs it.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

if os.name == 'nt':
    print('SKIP: fixture currently requires a POSIX C compiler')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
sources = ['runtime/spu/tests/test_spu_decrementer.c',
           'runtime/spu/spu_drain.c', 'runtime/spu/spu_lockstep.c',
           'runtime/spu/spu_coherency.c', 'runtime/spu/spu_workload.c',
           'runtime/spu/spurs_policy.c', 'runtime/spu/spurs_job.c',
           'runtime/spu/spu_tsp_weak.c', 'runtime/spu/spu_vm_pagemap.c',
           'runtime/spu/spurs_policy_blob_weak.c', 'runtime/spu/spu_interp.c',
           'runtime/platform/win32_compat.c']
with tempfile.TemporaryDirectory(prefix='spu-decrementer-') as tmp:
    binary = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-w',
                    '-I', str(root / 'include'), '-I', str(root / 'runtime/spu'),
                    '-I', str(root / 'runtime/platform'),
                    *[str(root / s) for s in sources],
                    '-o', str(binary), '-lpthread', '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=60)
