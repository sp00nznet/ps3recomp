#!/usr/bin/env python3
"""Real AppKit native OSK host fixture (macOS graphical session only).

Builds tests/test_native_osk_macos.m against the real osk_macos.m,
osk_native.c, cellOskDialog.c and cellSysutil.c (PS3RECOMP_NATIVE_OSK_MACOS),
wraps it in an app bundle and launches it twice with `open`, so activation
and keyboard focus are real: once plain, once driven by the control mailbox.
A second bundle of the same binary plays "another application" taking focus.
The fixture takes keyboard focus for a few seconds. No game files.
"""
from pathlib import Path
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def bundle(path, executable, identifier, name):
    (path / 'Contents/MacOS').mkdir(parents=True)
    (path / 'Contents/Info.plist').write_bytes(plistlib.dumps({
        'CFBundleExecutable': 'fixture', 'CFBundleIdentifier': identifier, 'CFBundleName': name,
        'CFBundlePackageType': 'APPL', 'CFBundleVersion': '1'}))
    shutil.copy2(executable, path / 'Contents/MacOS/fixture')


def main():
    if sys.platform != 'darwin':
        print('SKIP: the AppKit OSK host fixture needs macOS')
        return 0
    if os.environ.get('CI'):
        print('SKIP: the AppKit OSK host fixture needs an interactive graphical session')
        return 0
    with tempfile.TemporaryDirectory(prefix='native-osk-macos-') as temp:
        work = Path(temp)
        objects = []
        for index, source in enumerate(('libs/misc/cellOskDialog.c', 'libs/misc/osk_native.c',
                                        'libs/system/cellSysutil.c')):
            obj = work / f'{index}.o'
            subprocess.run(['cc', '-std=gnu17', '-O1', '-UNDEBUG', '-ffunction-sections', '-fdata-sections',
                            '-DPS3RECOMP_NATIVE_OSK_MACOS=1',
                            '-I' + str(ROOT / 'include'), '-c', str(ROOT / source), '-o', str(obj)], check=True)
            objects.append(str(obj))
        binary = work / 'fixture'
        subprocess.run(['clang', '-fobjc-arc', '-O1', '-UNDEBUG', '-DPS3RECOMP_NATIVE_OSK_MACOS=1',
                        '-I' + str(ROOT / 'include'), str(ROOT / 'tests/test_native_osk_macos.m'),
                        str(ROOT / 'libs/misc/osk_macos.m'), *objects, '-framework', 'AppKit',
                        '-pthread', '-Wl,-dead_strip', '-o', str(binary)], check=True)
        app = work / 'OskFixture.app'
        other = work / 'OtherFixture.app'
        bundle(app, binary, 'org.ps3recomp.tests.nativeosk', 'Native OSK fixture')
        bundle(other, binary, 'org.ps3recomp.tests.nativeoskother', 'Other focus fixture')
        env = os.environ.copy()
        env.pop('PS3RECOMP_OSK_INITIAL_TEXT', None)
        for controlled in (False, True):
            label = 'mailbox' if controlled else 'default'
            receipt = work / ('passed-' + label)
            out = work / (label + '.out')
            err = work / (label + '.err')
            # LaunchServices does not pass the caller's environment through, so
            # every variable the fixture reads is given explicitly.
            control = str(work / 'control') if controlled else ''
            subprocess.run(['/usr/bin/open', '-W', '-n',
                            '--env', 'OSK_FIXTURE_RECEIPT=' + str(receipt),
                            '--env', 'OSK_FIXTURE_OTHER_APP=' + str(other),
                            '--env', 'OSK_NATIVE_CONTROL_FILE=' + control,
                            '--stdout', str(out), '--stderr', str(err), str(app)],
                           check=True, timeout=60, env=env)
            print(f'--- {label} run')
            for log in (out, err):
                if log.exists() and log.read_text().strip():
                    print(log.read_text().rstrip())
            if not (receipt.exists() and receipt.read_text() == 'passed\n'):
                print(f'FAIL: the {label} AppKit fixture did not pass')
                return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
