#!/usr/bin/env python3
"""Windows native OSK host fixture, run on a private desktop.

Supply a /MT ps3recomp_runtime.lib built with -DPS3RECOMP_NATIVE_OSK_WINDOWS=ON.
The fixture never switches the user's desktop and sends no real input.
No game files.
"""
import argparse,ctypes,os,shutil,subprocess,sys,tempfile
from ctypes import wintypes as W
from pathlib import Path

def main():
    if os.name!='nt':print('SKIP: Windows OSK fixture');return 0
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime-library',required=True,type=Path);args=parser.parse_args()
    root=Path(__file__).resolve().parents[1];compiler=shutil.which(os.environ.get('CC','clang-cl'))
    if not compiler or not args.runtime_library.is_file():parser.error('Windows compiler/runtime required')
    with tempfile.TemporaryDirectory(prefix='windows-osk-') as temp:
        work=Path(temp);objects=[]
        (work/'ppu_recomp.h').write_text('#include "ppu_context.h"\n')
        subprocess.run([sys.executable,str(root/'tools/gen_hle_nids.py'),'cellOskDialog','cellSysutil','--out',str(work/'nids.cpp')],check=True,timeout=30)
        for i,source in enumerate((root/'tests/test_native_osk_windows.c',root/'runtime/host/host_stubs.c')):
            obj=work/(str(i)+'.obj');objects.append(str(obj))
            host_flags=['/Dps3_hle_register_ctx=fixture_unused_hle_register_ctx'] if i==1 else ['/DPS3RECOMP_NATIVE_OSK_WINDOWS=1']
            subprocess.run([compiler,'/nologo','/std:c17','/O1','/MT','/Gy','/Gw','/W0','/UNDEBUG','/D_CRT_SECURE_NO_WARNINGS',*host_flags,
                '/I'+str(root/'include'),'/I'+str(root/'runtime/ppu'),'/c',str(source),'/Fo'+str(obj)],check=True,timeout=60)
        binary=work/'fixture.exe';libraries=['d3d12','dxgi','d3dcompiler','dxguid','gdi32','user32','ole32','ws2_32','xinput','bcrypt','winmm','shlwapi','synchronization','psapi','dbghelp']
        subprocess.run([compiler,'/nologo','/std:c++17','/EHsc','/MT','/Gy','/Gw','/W0','/D_CRT_SECURE_NO_WARNINGS','/I'+str(root/'include'),'/I'+str(root/'runtime/ppu'),'/I'+str(work),*objects,str(work/'nids.cpp'),str(root/'runtime/ppu/ppu_hle.cpp'),str(root/'tests/native_osk_bridge.cpp'),str(args.runtime_library),'/Fo'+str(work)+'\\','/Fe'+str(binary),'/link','/OPT:REF',*(x+'.lib' for x in libraries)],check=True,timeout=90)
        user=ctypes.WinDLL('user32',use_last_error=True)
        user.CreateDesktopW.argtypes=[W.LPCWSTR,W.LPCWSTR,ctypes.c_void_p,W.DWORD,W.DWORD,ctypes.c_void_p];user.CreateDesktopW.restype=W.HANDLE
        user.CloseDesktop.argtypes=[W.HANDLE];user.CloseDesktop.restype=W.BOOL
        desktop=user.CreateDesktopW(work.name,None,None,0,0x10000000,None)
        if not desktop:raise ctypes.WinError(ctypes.get_last_error())
        try:
            startup=subprocess.STARTUPINFO();startup.lpDesktop=work.name
            environment={k:v for k,v in os.environ.items() if not k.startswith(('PAD_','PS3RECOMP_OSK_','OSK_NATIVE_'))}
            for mode in ('normal','prefill','unicode','control','legacy-pump'):
                run_environment=environment.copy()
                if mode=='prefill':run_environment['PS3RECOMP_OSK_INITIAL_TEXT']='PlayerHost'
                if mode=='unicode':run_environment['PS3RECOMP_OSK_INITIAL_TEXT']='AΩ🚀'
                control=work/(mode+'.control')
                if mode=='control':run_environment['OSK_NATIVE_CONTROL_FILE']=str(control)
                subprocess.run([str(binary),mode,str(control)],env=run_environment,cwd=work,startupinfo=startup,check=True,timeout=30)
        finally:user.CloseDesktop(desktop)
    return 0
if __name__=='__main__':raise SystemExit(main())
