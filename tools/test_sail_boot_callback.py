#!/usr/bin/env python3
"""Compile/run real Sail and generated NID registration through the PPU HLE bridge."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from gen_hle_nids import scan

root=Path(__file__).resolve().parents[1]

def main():
    if os.name=='nt':
        print('SKIP: standalone HLE bridge regression needs a POSIX toolchain')
        return 0
    handlers=dict(scan(root/'libs/codec/cellSail.c'))
    assert handlers.get(0xBDF21B0F)=='cellSailPlayerBoot','Boot is not registered'
    with tempfile.TemporaryDirectory(prefix='ps3-sail-boot-') as tmp:
        tmp=Path(tmp)
        (tmp/'ppu_recomp.h').write_text('#include "ppu_context.h"\n')
        subprocess.run([sys.executable,str(root/'tools/gen_hle_nids.py'),'cellSail','--out',str(tmp/'nids.cpp')],check=True,timeout=15)
        includes=['-I'+str(p) for p in (root/'include',root/'runtime/ppu',tmp)]
        subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-O1','-UNDEBUG',*includes,'-c',str(root/'libs/codec/cellSail.c'),'-o',str(tmp/'sail.o')],check=True,timeout=30)
        subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-O1','-UNDEBUG',*includes,str(root/'tests/test_sail_boot_callback.cpp'),str(root/'runtime/ppu/ppu_hle.cpp'),str(tmp/'nids.cpp'),str(tmp/'sail.o'),'-o',str(tmp/'fixture')],check=True,timeout=45)
        env={k:v for k,v in os.environ.items() if k in ('PATH','HOME','TMPDIR','SYSTEMROOT')}
        subprocess.run([str(tmp/'fixture')],env=env,check=True,timeout=10)
    return 0
if __name__=='__main__':
    raise SystemExit(main())
