#!/usr/bin/env python3
"""sys_memory containers take their memory out of the default pool (the real sys_memory.c, a manufactured VM)."""
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
def main():
 if os.name=='nt':
  print('SKIP: standalone memory-container regression needs a POSIX toolchain');return 0
 with tempfile.TemporaryDirectory(prefix='ps3-memory-container-') as tmp:
  exe=Path(tmp)/'fixture'
  subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-O1','-UNDEBUG',str(root/'tests/test_sys_memory_container.c'),str(root/'runtime/syscalls/sys_memory.c'),'-pthread','-o',str(exe)],check=True,timeout=30)
  subprocess.run([str(exe)],check=True,timeout=15)
 return 0
if __name__=='__main__':raise SystemExit(main())
