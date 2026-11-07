#!/usr/bin/env python3
"""GCM_HANDLER_THREAD=1: the guest handlers run on an interrupt thread with a guest PPU thread's host stack."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
root=Path(__file__).resolve().parents[1]
if os.name=='nt':
    print('SKIP: fixture currently uses POSIX linking')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='gcm-handler-thread-') as tmp:
    binary=Path(tmp)/'test'
    linker=['-Wl,-dead_strip'] if sys.platform=='darwin' else ['-Wl,--gc-sections','-pthread','-lm']
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-D_GNU_SOURCE','-O1','-UNDEBUG',
        '-ffunction-sections','-fdata-sections','-I'+str(root/'include'),
        str(root/'tests/test_gcm_handler_thread.c'),*linker,'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10,
        env={k:v for k,v in os.environ.items() if not k.startswith('A2O_')})
