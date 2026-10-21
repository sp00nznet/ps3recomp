#!/usr/bin/env python3
"""PS3_PRIO_START through the real sys_ppu_thread.c: run once with the option and once without."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
root=Path(__file__).resolve().parents[1]
if os.name=='nt':
    print('SKIP: fixture currently uses POSIX linking')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='ppu-prio-start-') as tmp:
    binary=Path(tmp)/'test'
    linker=['-Wl,-dead_strip'] if sys.platform=='darwin' else ['-Wl,--gc-sections','-pthread','-lm']
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-D_GNU_SOURCE','-O1','-UNDEBUG',
        '-ffunction-sections','-fdata-sections',
        '-I'+str(root/'include'),'-I'+str(root/'runtime'),'-I'+str(root/'runtime/syscalls'),
        str(root/'tests/test_ppu_prio_start.c'),*linker,'-o',str(binary)],check=True)
    quiet=subprocess.DEVNULL if os.environ.get('CI') else None
    for on in (False, True):
        env={k:v for k,v in os.environ.items() if k!='PS3_PRIO_START'}
        if on: env['PS3_PRIO_START']='1'
        subprocess.run([str(binary)],check=True,timeout=60,env=env,stderr=quiet)
