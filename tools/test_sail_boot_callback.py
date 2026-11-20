#!/usr/bin/env python3
"""Compile/run real Sail and generated NID registration through the PPU HLE bridge."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from gen_hle_nids import scan

root=Path(__file__).resolve().parents[1]

def main():
    compiler = shutil.which(os.environ.get('CC', 'cc')) or shutil.which('clang')
    cxx = shutil.which(os.environ.get('CXX', 'c++')) or shutil.which('clang++')
    if not compiler or not cxx:
        print('No C/C++ compiler found')
        return 1 if os.environ.get('CI') else 0
    msvc = Path(compiler).stem.lower() in ('clang-cl', 'cl')
    handlers=dict(scan(root/'libs/codec/cellSail.c'))
    assert handlers.get(0xBDF21B0F)=='cellSailPlayerBoot','Boot is not registered'
    with tempfile.TemporaryDirectory(prefix='ps3-sail-boot-') as tmp:
        tmp=Path(tmp)
        (tmp/'ppu_recomp.h').write_text('#include "ppu_context.h"\n')
        subprocess.run([sys.executable,str(root/'tools/gen_hle_nids.py'),'cellSail','--out',str(tmp/'nids.cpp')],check=True,timeout=15)
        paths=(root/'include',root/'runtime/ppu',tmp)
        binary=tmp/('fixture.exe' if os.name=='nt' else 'fixture')
        if msvc:
            flags=['/nologo','/O1','/Gy','/Gw','/MT','/W0','/D_CRT_SECURE_NO_WARNINGS']
            flags += ['/I'+str(p) for p in paths]
            subprocess.run([compiler,*flags,'/std:c17','/c',str(root/'libs/codec/cellSail.c'),'/Fo'+str(tmp/'sail.obj')],check=True,timeout=30)
            subprocess.run([cxx,*flags,'/std:c++17','/EHsc',str(root/'tests/test_sail_boot_callback.cpp'),str(root/'runtime/ppu/ppu_hle.cpp'),str(tmp/'nids.cpp'),str(tmp/'sail.obj'),'/Fo'+str(tmp)+'\\','/Fe'+str(binary),'/link','/OPT:REF'],check=True,timeout=45)
        else:
            includes=['-I'+str(p) for p in paths]
            subprocess.run([compiler,'-std=gnu17','-O1','-UNDEBUG',*includes,'-c',str(root/'libs/codec/cellSail.c'),'-o',str(tmp/'sail.o')],check=True,timeout=30)
            subprocess.run([cxx,'-std=c++17','-O1','-UNDEBUG',*includes,str(root/'tests/test_sail_boot_callback.cpp'),str(root/'runtime/ppu/ppu_hle.cpp'),str(tmp/'nids.cpp'),str(tmp/'sail.o'),'-o',str(binary)],check=True,timeout=45)
        env={k:v for k,v in os.environ.items() if k in ('PATH','HOME','TMPDIR','SYSTEMROOT')}
        subprocess.run([str(binary)],env=env,check=True,timeout=10)
    return 0
if __name__=='__main__':
    raise SystemExit(main())
