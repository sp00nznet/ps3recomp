"""Exercise the production depth-copy shaders against a currently bound DSV."""
import os, re, subprocess, tempfile
from pathlib import Path
from msvc_env import environ
if os.name != 'nt':
    print('SKIP: WARP depth pixels require Windows; dependency-loop tests run separately')
    raise SystemExit(0)
root=Path(__file__).resolve().parents[1]
source=(root/'libs/video/rsx_d3d12_backend.c').read_text(encoding='utf-8')
source=source[source.index('static int dsconv_init(void)'):]
with tempfile.TemporaryDirectory(prefix='d3d12-depth-feedback-') as tmp:
    directory=Path(tmp)
    declarations=[]
    for name in ('vs','ps'):
        start=source.index('static const char '+name+'[]')
        tail=source[start:]
        chunks=re.match(r'static const char '+name+r'\[\] =\s*((?:"(?:\\.|[^"\\])*"\s*)+);',tail,re.S)
        assert chunks
        declarations.append('static const char depth_'+name+'[] = '+chunks.group(1)+';\n')
    (directory/'depth_shader_fixture.h').write_text(''.join(declarations),encoding='utf-8')
    binary=directory/'test.exe'
    subprocess.run([os.environ.get('CC','clang-cl'),'/nologo','/W0','/EHsc','/UNDEBUG',
        '/I'+str(directory),str(root/'libs/video/tests/test_d3d12_depth_feedback_gpu.cpp'),
        '/Fe:'+str(binary)],cwd=directory,env=environ(),check=True)
    subprocess.run([str(binary)],cwd=directory,check=True,timeout=30)
