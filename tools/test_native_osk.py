#!/usr/bin/env python3
"""Native OSK lifecycle fixture (POSIX). No game files.

Builds tests/test_native_osk.c against the real cellOskDialog.c, osk_native.c
and cellSysutil.c with a simulated host boundary (PS3RECOMP_TEST_NATIVE_OSK),
then runs the lifecycle, PS3RECOMP_OSK_INITIAL_TEXT and control-mailbox cases.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    if sys.platform == 'win32':
        print('SKIP: native OSK fixture requires a POSIX C toolchain')
        return 0
    gc = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
    with tempfile.TemporaryDirectory(prefix='native-osk-') as temp:
        work = Path(temp)
        out = work / 'fixture'
        sources = [ROOT / 'tests/test_native_osk.c', ROOT / 'libs/misc/cellOskDialog.c',
                   ROOT / 'libs/misc/osk_native.c', ROOT / 'libs/system/cellSysutil.c']
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                        '-ffunction-sections', '-fdata-sections', '-DPS3RECOMP_TEST_NATIVE_OSK',
                        '-I' + str(ROOT / 'include'), *map(str, sources), '-o', str(out),
                        '-pthread', gc], check=True)
        env = os.environ.copy()
        env.pop('PS3RECOMP_OSK_INITIAL_TEXT', None)
        env.pop('OSK_NATIVE_CONTROL_FILE', None)
        subprocess.run([str(out)], check=True, timeout=10, env=env)

        cases = [('ascii', 'PlayerHost'), ('unicode', 'A¢Ω€🚀'), ('pair', 'A🚀B'),
                 ('split', 'A🚀B'), ('zero', 'A🚀B'), ('empty', ''),
                 ('maximum', 'a' * 510 + '🚀B'), ('unchanged', None)]
        # Invalid UTF-8 leaves the whole guest default intact, also when the
        # invalid suffix lies past the field's limit: stray continuation,
        # overlong forms, a surrogate, out of range, truncation.
        invalid = [b'\x80', b'\xc0\xaf', b'\xe0\x80\xaf', b'\xf0\x80\x80\xaf',
                   b'\xed\xa0\x80', b'\xf4\x90\x80\x80', b'\xf5\x80\x80\x80',
                   b'\xe2\x82', b'\xf0\x9f\x9a', b'A' * 40 + b'\xff', b'\xc2A']
        cases.extend(('unchanged', os.fsdecode(value)) for value in invalid)
        for mode, value in cases:
            case_env = env.copy()
            if value is not None:
                case_env['PS3RECOMP_OSK_INITIAL_TEXT'] = value
            subprocess.run([str(out), mode], check=True, timeout=10, env=case_env)

        for enabled in (False, True):
            control_env = env.copy()
            control_env['TEST_CONTROL_FILE'] = str(work / 'control')
            if enabled:
                control_env['OSK_NATIVE_CONTROL_FILE'] = control_env['TEST_CONTROL_FILE']
            subprocess.run([str(out), 'control' if enabled else 'control-off'],
                           check=True, timeout=10, env=control_env)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
