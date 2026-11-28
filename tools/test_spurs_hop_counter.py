#!/usr/bin/env python3
"""Force the benchmark's next hop to run before the current kick returns."""
import os
import subprocess
import tempfile
from pathlib import Path
from msvc_env import environ

root = Path(__file__).resolve().parents[1]
source = (root / 'tests/bench_spurs_pm_hop.c').read_text(encoding='utf-8')
start = source.index('static void pm_run(')
end = source.index('static void pm_a(', start)
with tempfile.TemporaryDirectory(prefix='spurs-hop-counter-') as directory:
    temp = Path(directory)
    (temp / 'spurs_hop_under_test.inc').write_text(source[start:end], encoding='utf-8')
    test = root / 'tests/test_spurs_hop_counter.c'
    binary = temp / ('test.exe' if os.name == 'nt' else 'test')
    if os.name == 'nt':
        command = [os.environ.get('CC', 'clang-cl'), '/nologo', '/O1', '/UNDEBUG',
                   '/I' + str(temp), str(test), '/Fe:' + str(binary)]
    else:
        command = [os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                   '-I' + str(temp), str(test), '-o', str(binary)]
    subprocess.run(command, cwd=temp, env=environ(), check=True)
    subprocess.run([str(binary)], cwd=temp, check=True, timeout=10)
