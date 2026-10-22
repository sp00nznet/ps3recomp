#!/usr/bin/env python3
"""sys_ppu_thread_get_stack_information through the real sys_ppu_thread.c: the main thread is told a real stack."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
root=Path(__file__).resolve().parents[1]
if os.name=='nt':
    print('SKIP: fixture currently uses POSIX linking')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='ppu-stack-info-') as tmp:
    binary=Path(tmp)/'test'
    linker=['-Wl,-dead_strip'] if sys.platform=='darwin' else ['-Wl,--gc-sections','-pthread','-lm']
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-D_GNU_SOURCE','-O1','-UNDEBUG',
        '-ffunction-sections','-fdata-sections',
        '-I'+str(root/'include'),'-I'+str(root/'runtime'),'-I'+str(root/'runtime/syscalls'),
        str(root/'tests/test_ppu_stack_info.c'),*linker,'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=60,stderr=subprocess.DEVNULL if os.environ.get('CI') else None)
