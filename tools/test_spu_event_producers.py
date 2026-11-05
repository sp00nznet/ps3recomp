#!/usr/bin/env python3
"""Run real SPU channel event producers against concurrent reservation writes."""
import os, subprocess, tempfile
from pathlib import Path
r=Path(__file__).resolve().parents[1]
s=r/'runtime/spu'
with tempfile.TemporaryDirectory(prefix='spu-event-producers-') as tmp:
 b=Path(tmp)/'events'
 units=['spu_drain','spu_lockstep','spu_coherency','spu_workload','spurs_policy','spurs_job','spu_tsp_weak','spu_vm_pagemap','spurs_policy_blob_weak','spu_interp']
 subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-O1','-UNDEBUG','-I'+str(r/'include'),'-I'+str(s),'-I'+str(r/'runtime/platform'),str(s/'tests/test_spu_lockline_peer.c'),*[str(s/(u+'.c')) for u in units],str(r/'runtime/platform/win32_compat.c'),'-pthread','-lm','-o',str(b)],check=True)
 subprocess.run([str(b)],check=True,timeout=30)
