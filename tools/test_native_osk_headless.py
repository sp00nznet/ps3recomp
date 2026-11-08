#!/usr/bin/env python3
"""Headless native OSK host fixture (POSIX without a toolkit dialog). No game files."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    if sys.platform in ('win32', 'darwin'):
        print('SKIP: the headless host is built only where no toolkit dialog exists')
        return 0
    with tempfile.TemporaryDirectory(prefix='native-osk-headless-') as temp:
        work = Path(temp)
        out = work / 'fixture'
        sources = [ROOT / 'tests/test_native_osk_headless.c', ROOT / 'libs/misc/cellOskDialog.c',
                   ROOT / 'libs/misc/osk_native.c', ROOT / 'libs/misc/osk_headless.c',
                   ROOT / 'libs/system/cellSysutil.c']
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                        '-ffunction-sections', '-fdata-sections', '-DPS3RECOMP_NATIVE_OSK_HEADLESS=1',
                        '-I' + str(ROOT / 'include'), *map(str, sources), '-o', str(out),
                        '-pthread', '-Wl,--gc-sections'], check=True)
        env = os.environ.copy()
        env['OSK_NATIVE_CONTROL_FILE'] = str(work / 'control')
        env['PS3RECOMP_OSK_INITIAL_TEXT'] = 'PlayerHost'
        subprocess.run([str(out)], check=True, timeout=10, env=env)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
