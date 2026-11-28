"""Regression for empty slots after a successful preview read."""
import os, subprocess, tempfile
from pathlib import Path
from msvc_env import environ
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='savedata-missing-') as tmp:
    directory = Path(tmp)
    binary = directory/('test.exe' if os.name == 'nt' else 'test')
    env = environ()
    env.pop('PS3_GAMEDATA_ROOT', None)
    source = root/'libs/system/tests/test_savedata_autoload_missing.c'
    if os.name == 'nt':
        command = [os.environ.get('CC', 'clang-cl'), '/nologo', '/W0', '/UNDEBUG',
            '/D_CRT_SECURE_NO_WARNINGS', '/I'+str(root/'include'),
            str(source), '/Fe:'+str(binary)]
    else:
        command = [os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
            '-I'+str(root/'include'), str(source), '-pthread', '-o', str(binary)]
    subprocess.run(command, cwd=directory, env=env, check=True)
    saves = directory/'saves'
    saves.mkdir()
    subprocess.run([str(binary), str(saves)], cwd=directory, env=env, check=True, timeout=30)
