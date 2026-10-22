#!/usr/bin/env python3
"""The bounded EA->IO lookup behind the SPU PUT hook agrees with a full-table walk."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
root=Path(__file__).resolve().parents[1]
if os.name=='nt':
    print('SKIP: fixture currently uses POSIX linking')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='gcm-ea2io-range-') as tmp:
    binary=Path(tmp)/'test'
    linker=['-Wl,-dead_strip'] if sys.platform=='darwin' else ['-Wl,--gc-sections','-pthread','-lm']
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-D_GNU_SOURCE','-O1','-UNDEBUG','-w',
        '-ffunction-sections','-fdata-sections','-I'+str(root/'include'),
        str(root/'tests/test_gcm_ea2io_range.c'),*linker,'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=60,
        env={k:v for k,v in os.environ.items() if not k.startswith('A2O_')})
