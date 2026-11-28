#!/usr/bin/env python3
"""Actual async dispatch, ELF loading and context construction retain SPURS ownership."""
import os
import subprocess
import tempfile
from pathlib import Path
from msvc_env import environ
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='spurs-dispatch-owner-') as tmp:
    temp = Path(tmp)
    binary = temp / ('test.exe' if os.name == 'nt' else 'test')
    source = root / 'runtime/spu/tests/test_spurs_dispatch_owner.c'
    if os.name == 'nt':
        cmd = [os.environ.get('CC', 'clang-cl'), '/nologo', '/W0', '/O1', '/UNDEBUG',
               '/D_CRT_SECURE_NO_WARNINGS', '/I' + str(root / 'include'),
               '/I' + str(root / 'runtime'), '/I' + str(root / 'runtime/spu'),
               str(source), '/Fe:' + str(binary)]
    else:
        cmd = [os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
               '-I' + str(root / 'include'), '-I' + str(root / 'runtime'),
               '-I' + str(root / 'runtime/spu'), str(source), '-pthread', '-o', str(binary)]
    subprocess.run(cmd, cwd=temp, env=environ(), check=True)
    subprocess.run([str(binary)], cwd=temp, env=environ(), check=True, timeout=20)
