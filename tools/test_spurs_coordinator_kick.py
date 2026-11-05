#!/usr/bin/env python3
"""An SPU commit to the SPURS instance's request line wakes the coordinator."""
import os
from pathlib import Path
import platform
import subprocess
import tempfile
if os.name == 'nt':
    print('SKIP: POSIX fixture'); raise SystemExit(0)
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='spurs-coordinator-kick-') as tmp:
    out=Path(tmp)/'test'
    units=['spu_channels','spu_drain','spu_lockstep','spu_coherency','spu_workload',
           'spurs_policy','spurs_job','spu_tsp_weak','spu_vm_pagemap',
           'spurs_policy_blob_weak','spu_interp']
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-D_GNU_SOURCE','-O1','-UNDEBUG',
        '-ffunction-sections','-fdata-sections',
        '-Wl,-dead_strip' if platform.system()=='Darwin' else '-Wl,--gc-sections',
        '-I'+str(root/'include'),'-I'+str(root/'runtime/spu'),'-I'+str(root/'runtime/platform'),
        str(root/'tests/test_spurs_coordinator_kick.c'),
        *[str(root/'runtime/spu'/f'{u}.c') for u in units],
        str(root/'runtime/platform/win32_compat.c'),'-pthread','-lm','-o',str(out)],check=True)
    subprocess.run([str(out)],check=True,timeout=15)
