"""Execute the exact depth-dependency loop; reject four broken variants."""
import os, subprocess, tempfile
from pathlib import Path
from msvc_env import environ
root=Path(__file__).resolve().parents[1]
source=(root/'libs/video/rsx_d3d12_backend.c').read_text(encoding='utf-8')
a=source.index('                for (int _u = 0; _u < 16; _u++) {', source.index('int cur_z = -1, cur_nc = 0, force_rebind = 0;'))
b=source.index('                /* The reduced-viewport passes',a)
loop=source[a:b]
assert loop.count('ds < 0 ||')==1 and loop.count('force_rebind = 1;')==1
variants={'positive':loop,
          'current_dsv_skipped':loop.replace('ds < 0 ||','ds < 0 || ds == cur_z ||'),
          'unread_units_copied':loop.replace('!(dr->fp_sampled_units & (1u << _u)) || ',''),
          'rebind_omitted':loop.replace('force_rebind = 1;','force_rebind = 0;'),
          'generations_ignored':loop.replace('s_d3d.off_ds[ds].conv_gen == s_d3d.off_ds[ds].gen','0')}
for name,text in variants.items():
    with tempfile.TemporaryDirectory(prefix='depth-dependencies-') as tmp:
        d=Path(tmp);(d/'depth_dependencies_fixture.h').write_text(text);exe=d/('test.exe' if os.name=='nt' else 'test')
        fixture=str(root/'libs/video/tests/test_d3d12_depth_dependencies.c')
        cmd=([os.environ.get('CC','clang-cl'),'/nologo','/W0','/UNDEBUG','/I'+tmp,fixture,'/Fe:'+str(exe)] if os.name=='nt' else [os.environ.get('CC','cc'),'-std=c11','-UNDEBUG','-I'+tmp,fixture,'-o',str(exe)])
        subprocess.run(cmd,cwd=d,env=environ(),check=True)
        run=subprocess.run([str(exe)],cwd=d,capture_output=True,text=True,timeout=15)
        if name=='positive':
            print(run.stdout,end='');print(run.stderr,end='');assert run.returncode==0
        else:
            assert run.returncode==1 and 'FAIL depth dependency' in run.stderr,(name,run.returncode,run.stdout,run.stderr)
            print('PASS: negative control rejected:',name)
