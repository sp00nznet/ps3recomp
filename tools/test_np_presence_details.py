#!/usr/bin/env python3
from pathlib import Path
import subprocess,sys,tempfile
r=Path(__file__).resolve().parents[1]
if sys.platform=='win32':
 print('SKIP: presence fixture needs POSIX C toolchain');raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='np-presence-') as d:
 p=Path(d);gen=p/'nids.cpp'
 subprocess.run([sys.executable,str(r/'tools/gen_hle_nids.py'),'--out',str(gen),'sceNpBasic'],check=True)
 # Only this import is under test; unrelated Basic APIs use the full guest VM.
 # The generated NID table retains all functions, so dead-strip their unused
 # registrations by making the table's registration sink select the NID.
 source=gen.read_text();source='\n'.join(l for l in source.splitlines() if 'ps3_hle_register(0x' not in l or '0xBE81C71C' in l);gen.write_text(source)
 obj=p/'basic.o';test=p/'test.o'
 for s,o in [(r/'libs/network/sceNpBasic.c',obj),(r/'tests/test_np_presence_details.c',test)]:
  subprocess.run(['cc','-std=gnu17','-O1','-UNDEBUG','-ffunction-sections','-fdata-sections','-I'+str(r/'include'),'-c',str(s),'-o',str(o)],check=True)
 gc='-Wl,-dead_strip' if sys.platform=='darwin' else '-Wl,--gc-sections'
 exe=p/'fixture';subprocess.run(['c++',str(gen),str(obj),str(test),gc,'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=10)
