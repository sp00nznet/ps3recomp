"""Run compiled test_mesh_pilot.c against valid and malformed FMP1 files.
Usage: python run_mesh_pilot_fixtures.py TEST_EXE EXISTING_SCRATCH_DIRECTORY
"""
import pathlib
import struct
import subprocess
import sys

def main():
    exe=pathlib.Path(sys.argv[1]).resolve()
    root=pathlib.Path(sys.argv[2]).resolve()
    if not root.is_dir():raise SystemExit('Scratch directory must exist')
    v=struct.pack('<12f',0,0,0,1,1,0,0,1,0,1,0,1)
    ix=struct.pack('<3I',0,1,2)
    h=struct.pack('<24I',3,3,16,1,3,3,4,0,*([0]*16))
    p=b'FMP1'+struct.pack('<2I',1,1)+h+v+ix+v+ix
    cases={'valid':p,'truncated':p[:-1],'trailing':p+b'x',
           'nan':p[:108]+struct.pack('<I',0x7fc00000)+p[112:],
           'bad_index':p[:156]+struct.pack('<I',3)+p[160:],
           'oversize':p[:12]+struct.pack('<I',0xffffffff)+p[16:]}
    for name,data in cases.items():
        path=root/f'pilot-fixture-{name}.bin'
        path.write_bytes(data)
        subprocess.run([str(exe),str(path),'accept' if name=='valid' else 'reject'],check=True)
    print('6 fixture cases passed')
if __name__=='__main__':main()
