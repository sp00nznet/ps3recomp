#!/usr/bin/env python3
"""Build/run the PPU directory-entry regression on POSIX."""
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    if os.name == 'nt':
        print('SKIP: PPU fixture requires the POSIX build toolchain')
        return
    with tempfile.TemporaryDirectory(prefix='ps3-dir-') as tmp:
        out=Path(tmp); fixture=out/'fixture'; fixture.mkdir()
        (fixture/'payload').write_bytes(b'1234567')
        (fixture/'.hidden').write_bytes(b'')
        (fixture/'folder').mkdir()
        subprocess.run([sys.executable,str(ROOT/'tools/make_smoke_elf.py'),
                        '--out',str(out/'smoke.elf'),'--header',str(out/'ppu_recomp.h')],check=True)
        linker='-Wl,-dead_strip' if platform.system()=='Darwin' else '-Wl,--gc-sections'
        subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-O1','-UNDEBUG',
                        '-ffunction-sections','-fdata-sections',linker,
                        '-I'+str(out),'-I'+str(ROOT/'include'),
                        str(ROOT/'runtime/ppu/tests/test_ppu_directory.cpp'),
                        '-o',str(out/'test')],check=True)
        subprocess.run([str(out/'test'),str(fixture)],check=True,timeout=15)

if __name__=='__main__':
    main()
