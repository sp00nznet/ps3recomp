#!/usr/bin/env python3
"""Synthetic real-GPU immediate-vertex check on Windows (VS environment).

Supply --runtime-library <build>/ps3recomp_runtime.lib built with /MT.
Tests legacy, hoisted and compact vertex paths. No game files are used.
"""
import argparse,os,shutil,subprocess,tempfile
from pathlib import Path
def main():
    if os.name!='nt':print('SKIP: Windows D3D12 fixture');return 0
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime-library',type=Path,required=True);args=parser.parse_args()
    library=args.runtime_library.resolve();root=Path(__file__).resolve().parents[1]
    compiler=shutil.which(os.environ.get('CC','clang-cl'))
    if not compiler or not library.is_file():parser.error('C compiler and runtime library required')
    with tempfile.TemporaryDirectory(prefix='live-immediate-') as temp:
        work=Path(temp);objects=[]
        for index,source in enumerate((root/'libs/video/tests/test_live_draw_immediate.c',root/'runtime/host/host_stubs.c')):
            obj=work/(str(index)+'.obj')
            subprocess.run([compiler,'/nologo','/std:c17','/O1','/Gy','/Gw','/W0','/MT','/D_CRT_SECURE_NO_WARNINGS',
                '/I'+str(root/'include'),'/I'+str(root/'libs/video'),'/I'+str(root/'runtime'),'/c',str(source),'/Fo'+str(obj)],check=True,timeout=90)
            objects.append(str(obj))
        binary=work/'fixture.exe';libs=['d3d12','dxgi','d3dcompiler','dxguid','gdi32','user32','ole32','ws2_32','xinput','bcrypt','winmm','shlwapi','synchronization','psapi','dbghelp']
        subprocess.run([compiler,'/nologo',*objects,str(library),'/Fe'+str(binary),'/link','/OPT:REF',*(name+'.lib' for name in libs)],check=True,timeout=90)
        for mode in ('L','H','M','C'):
            environment=dict(os.environ,RSX_VERTEX_MODE=mode)
            subprocess.run([str(binary)],cwd=work,env=environment,check=True,timeout=60)
    return 0
if __name__=='__main__':raise SystemExit(main())
