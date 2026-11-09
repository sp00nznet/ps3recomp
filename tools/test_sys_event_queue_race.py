#!/usr/bin/env python3
"""Host producers pushing into event queues the guest creates and destroys, on Windows and POSIX.

Builds runtime/syscalls/tests/test_sys_event_queue_race.c (which compiles in
runtime/syscalls/sys_event.c) and runs it: producer threads resolve a key and push
while the guest creates and destroys that queue through both create paths.
"""
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'runtime/syscalls/tests/test_sys_event_queue_race.c'
windows = os.name == 'nt'
compiler = os.environ.get('CC', 'clang-cl' if windows else 'cc')
msvc_style = Path(compiler).stem.lower() in ('clang-cl', 'cl')
if not shutil.which(compiler):
    raise SystemExit(f'compiler {compiler!r} not found')
includes = [ROOT / 'include', ROOT / 'runtime', ROOT / 'runtime/syscalls']

with tempfile.TemporaryDirectory(prefix='sys-event-race-') as tmp:
    binary = Path(tmp) / ('test.exe' if windows else 'test')
    if msvc_style:
        command = [compiler, '/nologo', '/std:c17', '/O2', '/D_CRT_SECURE_NO_WARNINGS',
                   *('/I' + str(i) for i in includes), str(SOURCE), '/Fe' + str(binary)]
    else:
        command = [compiler, '-std=gnu17', '-O2', '-UNDEBUG', *('-I' + str(i) for i in includes),
                   str(SOURCE), *([] if windows else ['-pthread']), '-o', str(binary)]
    subprocess.run(command, check=True, timeout=180, cwd=tmp)
    for attempt in range(3):
        run = subprocess.run([str(binary)], cwd=tmp, timeout=180, capture_output=True, text=True)
        print(run.stdout.strip(), flush=True)
        if run.returncode != 0:
            print(run.stderr[-2000:])
            raise SystemExit(f'test_sys_event_queue_race failed (exit {run.returncode:#x}) on run {attempt + 1}')
print('PASS')
