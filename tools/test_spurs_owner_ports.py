#!/usr/bin/env python3
"""Exercise the production SPURS port table without linking unrelated HLE modules."""
import os
from pathlib import Path
import subprocess
import tempfile
from msvc_env import environ

root = Path(__file__).resolve().parents[1]
source = (root / 'libs/spurs/cellSpurs.c').read_text(encoding='utf-8')
start = source.index('/* The lv2 event queues')
end = source.index('/* Validate guest backing', start)
with tempfile.TemporaryDirectory(prefix='spurs-owner-ports-') as directory:
    temp = Path(directory)
    (temp / 'spurs_ports_under_test.inc').write_text(source[start:end], encoding='utf-8')
    test = root / 'libs/spurs/tests/test_owner_ports.c'
    binary = temp / ('test.exe' if os.name == 'nt' else 'test')
    env = environ()
    if os.name == 'nt':
        command = [os.environ.get('CC', 'clang-cl'), '/nologo', '/W0', '/UNDEBUG',
                   '/D_CRT_SECURE_NO_WARNINGS', '/I' + str(root / 'include'),
                   '/I' + str(temp), str(test), '/Fe:' + str(binary)]
    else:
        command = [os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                   '-I' + str(root / 'include'), '-I' + str(temp), str(test),
                   '-pthread', '-o', str(binary)]
    subprocess.run(command, cwd=temp, env=env, check=True)
    subprocess.run([str(binary)], cwd=temp, check=True, timeout=15)
