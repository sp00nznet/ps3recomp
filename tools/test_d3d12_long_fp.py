"""Long FP bounds, tail hash/constants; Windows also validates the production scan and HLSL."""
import os, subprocess, tempfile
from pathlib import Path
from msvc_env import environ
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='long-fp-') as tmp:
    binary = Path(tmp)/('test.exe' if os.name == 'nt' else 'test')
    sources = [str(root/'libs/video/tests/test_d3d12_long_fp.c'), str(root/'libs/video/rsx_fp_decompiler.c')]
    if os.name == 'nt':
        cmd = [os.environ.get('CC', 'clang-cl'), '/nologo', '/W0', '/UNDEBUG', '/D_CRT_SECURE_NO_WARNINGS', '/I'+str(root/'include'), '/I'+str(root/'libs/video'), *sources, '/Fe:'+str(binary)]
    else:
        cmd = [os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG', '-I'+str(root/'include'), '-I'+str(root/'libs/video'), *sources, '-lm', '-o', str(binary)]
    subprocess.run(cmd, cwd=tmp, env=environ(), check=True)
    subprocess.run([str(binary)], cwd=tmp, check=True, timeout=30)
