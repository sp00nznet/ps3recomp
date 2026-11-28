"""D3D12 point expansion: real HLSL compiler and WARP pixel regressions."""
import os, subprocess, tempfile
from pathlib import Path
from msvc_env import environ
if os.name != 'nt':
    print('SKIP: D3D12 point expansion requires Windows')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
for name in ('test_d3d12_point_quad.c', 'test_d3d12_point_quad_gpu.cpp'):
    with tempfile.TemporaryDirectory(prefix='d3d12-point-') as tmp:
        exe = Path(tmp)/'test.exe'
        subprocess.run([os.environ.get('CC','clang-cl'), '/nologo', '/W0', '/UNDEBUG', '/EHsc',
            '/D_CRT_SECURE_NO_WARNINGS', '/I'+str(root/'libs/video'),
            str(root/'libs/video/tests'/name), '/Fe:'+str(exe)], cwd=tmp, env=environ(), check=True)
        subprocess.run([str(exe)], cwd=tmp, check=True, timeout=60)

# Restore the original incorrect StartVertexLocation subtraction in a private
# header. It must fail the pixel comparison, not merely fail to compile.
with tempfile.TemporaryDirectory(prefix='d3d12-point-negative-') as tmp:
    d=Path(tmp);header=(root/'libs/video/rsx_d3d12_point_quad.h').read_text()
    old='_corner[_d3d_point_vertex%%6u]'
    assert header.count(old)==1
    header=header.replace(old,'_corner[(_d3d_point_vertex-(uint)d3d_point_view.z)%%6u]')
    (d/'rsx_d3d12_point_quad.h').write_text(header)
    exe=d/'negative.exe'
    subprocess.run([os.environ.get('CC','clang-cl'),'/nologo','/W0','/UNDEBUG','/EHsc',
        '/I'+tmp, str(root/'libs/video/tests/test_d3d12_point_quad_gpu.cpp'),'/Fe:'+str(exe)],
        cwd=d,env=environ(),check=True)
    result=subprocess.run([str(exe)],cwd=d,capture_output=True,text=True,timeout=60)
    assert result.returncode==1 and 'FAIL: point quad changed with StartVertexLocation=' in result.stderr,(result.returncode,result.stderr)
    print('PASS: negative control rejected: subtracting StartVertexLocation changes point pixels')
