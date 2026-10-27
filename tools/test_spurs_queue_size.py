#!/usr/bin/env python3
"""cellSpursQueueSize through the real cellSpurs.c: registered export and occupancy from the ring indices."""
import os
from pathlib import Path
import platform
import subprocess
import tempfile
from gen_hle_nids import scan
from nid_database import compute_nid
root=Path(__file__).resolve().parents[1]
assert (compute_nid('cellSpursQueueSize'),'cellSpursQueueSize') in scan(root/'libs/spurs/cellSpurs.c'), \
    'Missing cellSpursQueueSize export (an unregistered import fakes CELL_OK without writing *size)'
if os.name=='nt':
    print('SKIP: fixture currently uses POSIX linking; registration checked')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='spurs-queue-size-') as tmp:
    out=Path(tmp)/'test'
    units=['spu_channels','spu_drain','spu_lockstep','spu_coherency','spu_workload',
           'spurs_policy','spurs_job','spu_tsp_weak','spu_vm_pagemap',
           'spurs_policy_blob_weak','spu_interp']
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-D_GNU_SOURCE','-O1','-UNDEBUG',
        '-ffunction-sections','-fdata-sections',
        '-Wl,-dead_strip' if platform.system()=='Darwin' else '-Wl,--gc-sections',
        '-I'+str(root/'include'),'-I'+str(root/'runtime/spu'),'-I'+str(root/'runtime/platform'),
        str(root/'tests/test_spurs_queue_size.c'),
        *[str(root/'runtime/spu'/f'{u}.c') for u in units],
        str(root/'runtime/platform/win32_compat.c'),'-pthread','-lm','-o',str(out)],check=True)
    subprocess.run([str(out)],check=True,timeout=15)
