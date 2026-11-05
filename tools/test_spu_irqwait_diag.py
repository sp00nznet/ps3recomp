#!/usr/bin/env python3
"""SPU_IRQWAIT_DIAG through the real spu_drain.c: a report every 2^22 interrupt-wait dispatches, off by default."""
import os
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: fixture requires the POSIX compiler/linker')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
spu = root / 'runtime/spu'
sources = ['spu_channels.c', 'spu_drain.c', 'spu_lockstep.c', 'spu_coherency.c', 'spu_workload.c',
           'spurs_policy.c', 'spurs_job.c', 'spu_tsp_weak.c', 'spu_vm_pagemap.c',
           'spurs_policy_blob_weak.c', 'spu_interp.c']
env = {k: v for k, v in os.environ.items()
       if not k.startswith(('SPU_', 'SPURS_', 'YDKJ_', 'LBP_', 'GH3_'))}
with tempfile.TemporaryDirectory(prefix='spu-irqwait-diag-') as tmp:
    binary = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-I' + str(root / 'include'), '-I' + str(spu), '-I' + str(root / 'runtime/platform'),
                    str(spu / 'tests/test_spu_irqwait_diag.c'),
                    *[str(spu / name) for name in sources],
                    str(root / 'runtime/platform/win32_compat.c'), '-pthread', '-lm',
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary), 'off'], check=True, timeout=60, cwd=tmp, env=env)
    subprocess.run([str(binary), 'on', str(Path(tmp) / 'log')], check=True, timeout=60, cwd=tmp,
                   env={**env, 'SPU_IRQWAIT_DIAG': '1', 'SPU_IRQWAIT_LS': '12A0'})
