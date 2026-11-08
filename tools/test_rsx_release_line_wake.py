#!/usr/bin/env python3
"""RSX main-memory semaphore releases and PS3RECOMP_RSX_RELEASE_LINE_WAKE
(libs/video/tests/test_rsx_release_line_wake.c, built against rsx_commands.c):
one run with the variable unset and one with it set."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: builds with a POSIX cc')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='rsx-release-line-wake-') as tmp:
    binary = Path(tmp) / 'test'
    extra = [] if sys.platform == 'darwin' else ['-lm']
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-I' + str(root / 'include'), '-I' + str(root / 'libs/video'),
                    str(root / 'libs/video/tests/test_rsx_release_line_wake.c'),
                    str(root / 'libs/video/rsx_commands.c'), *extra,
                    '-o', str(binary)], check=True)
    for mode in ('off', 'on'):
        result = subprocess.run([str(binary), mode], timeout=60, capture_output=True, text=True)
        print('\n'.join(l for l in result.stdout.splitlines() if l.startswith('[') and not l.startswith('[RSX')))
        if result.returncode:
            print(result.stdout[-2000:])
            raise SystemExit('test_rsx_release_line_wake failed: ' + mode)
