"""Exercise the production D3D12 sampler descriptor without a GPU."""
import os, subprocess, tempfile
from pathlib import Path
from msvc_env import environ
if os.name != 'nt':
    print('SKIP: D3D12 sampler descriptors require Windows SDK headers')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='sampler-lod-') as tmp:
    binary = Path(tmp)/'test.exe'
    subprocess.run([os.environ.get('CC', 'clang-cl'), '/nologo', '/W4', '/UNDEBUG',
        '/I'+str(root/'libs/video'), str(root/'libs/video/tests/test_d3d12_sampler.c'),
        '/Fe:'+str(binary)], cwd=tmp, env=environ(), check=True)
    subprocess.run([str(binary)], cwd=tmp, check=True, timeout=30)
    print('PASS base-only, full-chain, fractional, non-mip and inverted LOD ranges')
