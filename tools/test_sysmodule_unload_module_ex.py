#!/usr/bin/env python3
"""Exercise the one-argument extended unload through production generated NID dispatch."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root=Path(__file__).resolve().parents[1]

def main():
    if os.name=='nt':
        print('SKIP: standalone HLE bridge regression needs a POSIX toolchain')
        return 0
    with tempfile.TemporaryDirectory(prefix='ps3-sysmodule-unload-ex-') as tmp:
        tmp=Path(tmp)
        (tmp/'ppu_recomp.h').write_text('#include "ppu_context.h"\n')
        subprocess.run([sys.executable,str(root/'tools/gen_hle_nids.py'),'cellSysmodule','--out',str(tmp/'nids.cpp')],check=True,timeout=15)
        includes=['-I'+str(p) for p in (root/'include',root/'runtime/ppu',root/'runtime/syscalls',root/'libs/system',tmp)]
        subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-O1','-UNDEBUG',*includes,'-c',str(root/'libs/system/cellSysmodule.c'),'-o',str(tmp/'module.o')],check=True,timeout=30)
        subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-O1','-UNDEBUG',*includes,str(root/'tests/test_sysmodule_unload_module_ex.cpp'),str(root/'runtime/ppu/ppu_hle.cpp'),str(tmp/'nids.cpp'),str(tmp/'module.o'),'-pthread','-o',str(tmp/'fixture')],check=True,timeout=45)
        env={k:v for k,v in os.environ.items() if k in ('PATH','HOME','TMPDIR','SYSTEMROOT')}
        subprocess.run([str(tmp/'fixture')],env=env,check=True,timeout=10)
    return 0
if __name__=='__main__':
    raise SystemExit(main())
