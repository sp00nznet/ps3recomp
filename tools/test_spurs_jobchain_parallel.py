#!/usr/bin/env python3
"""Job chains through the real cellSpurs.c: serial by default, overlapping up to
maxContention under SPURS_JC_PARALLEL=1 with SYNC honoured, and no lost job
guard notify."""
import os
from pathlib import Path
import platform
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
if os.name=='nt':
    print('SKIP: fixture currently uses POSIX linking')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='spurs-jobchain-parallel-') as tmp:
    out=Path(tmp)/'test'
    units=['spu_channels','spu_drain','spu_lockstep','spu_coherency','spu_workload',
           'spurs_policy','spurs_job','spu_tsp_weak','spu_vm_pagemap',
           'spurs_policy_blob_weak','spu_interp']
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-D_GNU_SOURCE','-O1','-UNDEBUG',
        '-ffunction-sections','-fdata-sections',
        '-Wl,-dead_strip' if platform.system()=='Darwin' else '-Wl,--gc-sections',
        '-I'+str(root/'include'),'-I'+str(root/'runtime/spu'),'-I'+str(root/'runtime/platform'),
        str(root/'tests/test_spurs_jobchain_parallel.c'),
        *[str(root/'runtime/spu'/f'{u}.c') for u in units],
        str(root/'runtime/platform/win32_compat.c'),'-pthread','-lm','-o',str(out)],check=True)
    env=dict(os.environ); env.pop('SPURS_JC_PARALLEL',None)
    subprocess.run([str(out),'serial'],check=True,timeout=60,env=env)
    env['SPURS_JC_PARALLEL']='1'
    subprocess.run([str(out),'parallel'],check=True,timeout=60,env=env)
