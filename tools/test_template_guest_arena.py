#!/usr/bin/env python3
"""Compile and run tests/test_template_guest_arena.c against the project
template's POSIX guest arena (templates/project/guest_arena.h). Synthetic."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]


def main():
    if os.name == 'nt':
        print('SKIP: the template maps its POSIX arena with mmap; Windows uses VirtualAlloc')
        return 0
    with tempfile.TemporaryDirectory(prefix='template-arena-') as temporary:
        binary = Path(temporary) / 'fixture'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(root / 'templates/project'),
                        str(root / 'tests/test_template_guest_arena.c'), '-o', str(binary)],
                       check=True, timeout=60)
        return subprocess.run([str(binary)], timeout=30).returncode


if __name__ == '__main__':
    raise SystemExit(main())
