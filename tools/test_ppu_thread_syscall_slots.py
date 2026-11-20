#!/usr/bin/env python3
"""lv2 syscall slot 41 is _sys_ppu_thread_exit and 42 is ENOSYS, through the real sys_ppu_thread.c and its table."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
root=Path(__file__).resolve().parents[1]
if os.name=='nt':
    print('SKIP: fixture currently uses POSIX linking')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='ppu-thread-syscall-slots-') as tmp:
    binary=Path(tmp)/'test'
    linker=['-Wl,-dead_strip'] if sys.platform=='darwin' else ['-Wl,--gc-sections','-pthread','-lm']
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-D_GNU_SOURCE','-O1','-UNDEBUG',
        '-ffunction-sections','-fdata-sections',
        '-I'+str(root/'include'),'-I'+str(root/'runtime'),'-I'+str(root/'runtime/syscalls'),
        str(root/'tests/test_ppu_thread_syscall_slots.c'),
        str(root/'runtime/syscalls/lv2_register.c'),   # the real lv2_try_syscall and its sentinel
        *linker,'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=60,stderr=subprocess.DEVNULL if os.environ.get('CI') else None)
