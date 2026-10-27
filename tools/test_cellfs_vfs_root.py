#!/usr/bin/env python3
"""Exercise actual CellFs path translation, shared fallback, and host file I/O."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]

def main():
    if os.name == 'nt':
        print('SKIP: fixture requires a POSIX C compiler and dead-section linker')
        return 0
    with tempfile.TemporaryDirectory(prefix='cellfs-vfs-') as temporary:
        tmp=Path(temporary)
        binary=tmp/'fixture'
        gc='-Wl,-dead_strip' if sys.platform=='darwin' else '-Wl,--gc-sections'
        subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-D_GNU_SOURCE','-O1','-ffunction-sections','-fdata-sections',
            '-I'+str(root/'include'),str(root/'tests/test_cellfs_vfs_root.c'),
            str(root/'libs/filesystem/cellFs.c'),str(root/'runtime/syscalls/sys_fs.c'),
            gc,'-o',str(binary)],check=True,timeout=45)
        env={k:v for k,v in os.environ.items() if k in ('PATH','HOME','TMPDIR','SYSTEMROOT')}
        relative='PS3_GAME/USRDIR/movie.pam'
        for folder in ['gamedata/dev_bdvd','disc','explicit','root/assets']:
            file=tmp/folder/relative;file.parent.mkdir(parents=True);file.write_bytes(b'disc-data')
        cases=[('default','unused','./gamedata/dev_bdvd/'+relative,None),
               ('env',str(tmp/'disc'),str(tmp/'disc'/relative),str(tmp/'disc')),
               ('trailing',str(tmp/'disc'),str(tmp/'disc'/relative),str(tmp/'disc')+'/'),
               ('absolute',str(tmp/'explicit'),str(tmp/'explicit'/relative),str(tmp/'disc')),
               ('relative',str(tmp/'root'),str(tmp/'root/assets'/relative),str(tmp/'disc'))]
        failures=0
        for mode,mapping,expected,vfs in cases:
            run_env=dict(env)
            if vfs is not None: run_env['PS3_VFS_ROOT']=vfs
            r=subprocess.run([str(binary),mode,mapping,expected],cwd=tmp,env=run_env,timeout=10)
            failures+=bool(r.returncode)
        return 1 if failures else 0

if __name__=='__main__':
    raise SystemExit(main())
