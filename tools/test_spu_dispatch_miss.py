#!/usr/bin/env python3
"""A dispatch MISS through the real spu_workload.c: one ERROR per dropped program, SPU_MISS_FATAL=1 aborts."""
import os
from pathlib import Path
import platform
import re
import signal
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
if os.name=='nt':
    print('SKIP: fixture currently uses POSIX linking')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='spu-dispatch-miss-') as tmp:
    out=Path(tmp)/'test'
    units=['spu_channels','spu_drain','spu_lockstep','spu_coherency','spu_workload',
           'spurs_policy','spurs_job','spu_tsp_weak','spu_vm_pagemap',
           'spurs_policy_blob_weak','spu_interp']
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-D_GNU_SOURCE','-O1','-UNDEBUG',
        '-ffunction-sections','-fdata-sections',
        '-Wl,-dead_strip' if platform.system()=='Darwin' else '-Wl,--gc-sections',
        '-I'+str(root/'include'),'-I'+str(root/'runtime/spu'),'-I'+str(root/'runtime/platform'),
        str(root/'tests/test_spu_dispatch_miss.c'),
        *[str(root/'runtime/spu'/f'{u}.c') for u in units],
        str(root/'runtime/platform/win32_compat.c'),'-pthread','-lm','-o',str(out)],check=True)

    env={k:v for k,v in os.environ.items() if k not in ('SPU_MISS_FATAL','SPU_DUMP_MISS')}
    p=subprocess.run([str(out)],env=env,capture_output=True,text=True,timeout=15)
    assert p.returncode==0, (p.returncode, p.stderr)
    assert 'dispatch-miss: done r=0' in p.stderr, p.stderr
    errors=[l for l in p.stderr.splitlines() if '[spu_workload] ERROR:' in l]
    assert len(errors)==2, 'expected one ERROR per dropped program (2), got:\n'+'\n'.join(errors)
    fps={re.search(r'fp=0x([0-9A-F]{16})',l).group(1) for l in errors}
    assert len(fps)==2, errors
    # The first report names the first path that dropped the program and its owner.
    assert 'sync dispatch' in errors[0] and 'owner=0x00001000' in errors[0], errors[0]
    assert 'async task dispatch' in errors[1] and 'owner=0x00004000' in errors[1], errors[1]
    assert all('DROPPED' in l for l in errors), errors

    p=subprocess.run([str(out)],env={**env,'SPU_MISS_FATAL':'1'},capture_output=True,text=True,timeout=15)
    assert p.returncode==-signal.SIGABRT, (p.returncode, p.stderr)
    assert 'dispatch-miss: done' not in p.stderr, p.stderr
    errors=[l for l in p.stderr.splitlines() if '[spu_workload] ERROR:' in l]
    assert len(errors)==1 and 'sync dispatch' in errors[0], p.stderr
print('PASS')
