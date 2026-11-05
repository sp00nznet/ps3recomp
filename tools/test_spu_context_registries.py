#!/usr/bin/env python3
"""Run SPU MFC ownership and reservation regressions with >8 live contexts."""
import os
import signal
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: SPU registry fixtures require POSIX pthreads')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
spu = root / 'runtime/spu'
common = ['spu_drain.c', 'spu_lockstep.c', 'spu_coherency.c', 'spu_workload.c',
          'spurs_policy.c', 'spurs_job.c', 'spu_tsp_weak.c', 'spu_vm_pagemap.c',
          'spurs_policy_blob_weak.c', 'spu_interp.c']
failed = []
with tempfile.TemporaryDirectory(prefix='spu-context-registry-') as tmp:
    for name in ['test_spu_mfc_slots', 'test_spu_mfc_shared_lookup', 'test_spu_coherency', 'test_spu_lockline_peer', 'test_spu_registry_allocation', 'test_spu_mfc_env_once']:
        binary = Path(tmp) / name
        command = [os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                   '-I' + str(root / 'include'), '-I' + str(spu),
                   '-I' + str(root / 'runtime/platform'),
                   str(spu / 'tests' / (name + '.c'))]
        if name != 'test_spu_coherency':
            command += [str(spu / c) for c in common if not
                        (name == 'test_spu_registry_allocation' and c == 'spu_coherency.c')]
            command += [str(root / 'runtime/platform/win32_compat.c'), '-pthread', '-lm']
        subprocess.run(command + ['-o', str(binary)], check=True)
        if name == 'test_spu_registry_allocation':
            for mode, diagnostic in [('mfc', 'cannot allocate private MFC engine'),
                                     ('coh', 'cannot register reservation holder')]:
                result = subprocess.run([str(binary), mode], timeout=10,
                                        capture_output=True, text=True)
                assert result.returncode == -signal.SIGABRT, result
                assert diagnostic in result.stderr, result.stderr
                print(name + '/' + mode + ': PASS (visible allocation failure)')
            continue
        result = subprocess.run([str(binary)], timeout=30, capture_output=True, text=True)
        print(name + ': ' + ('PASS' if result.returncode == 0 else 'FAIL'))
        print(result.stdout.strip())
        if result.returncode:
            print(result.stderr[-2500:])
        if result.returncode: failed.append(name)
if failed:
    raise SystemExit('Failed: ' + ', '.join(failed))
