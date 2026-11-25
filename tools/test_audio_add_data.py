#!/usr/bin/env python3
"""cellAudioAdd*Data and the notify-queue key (libs/audio/tests/test_audio_add_data.c).

Builds the test against libs/audio/cellAudio.c: with SDL2 (sdl2-config) off
Windows, where cellAudio selects its SDL2 backend, and with clang-cl and the
WASAPI backend on Windows. No audio device is opened.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
source = root / 'libs/audio/tests/test_audio_add_data.c'

with tempfile.TemporaryDirectory(prefix='audio-add-data-') as tmp:
    binary = Path(tmp) / 'test'
    if sys.platform == 'win32' and not os.environ.get('CC'):
        cl = shutil.which('clang-cl')
        if not cl:
            print('SKIP: no clang-cl on PATH')
            raise SystemExit(0)
        binary = Path(str(binary) + '.exe')
        cmd = [cl, '/nologo', '/std:c17', '-D_CRT_SECURE_NO_WARNINGS', '-I' + str(root / 'include'),
               str(source), '/Fe:' + str(binary), '/Fo:' + tmp + '\\',
               '/link', 'ole32.lib', 'avrt.lib', 'winmm.lib', 'user32.lib']
    else:
        cfg = shutil.which('sdl2-config')
        if not cfg:
            if os.environ.get('CI'):
                print('FAIL: sdl2-config not found')
                raise SystemExit(1)
            print('SKIP: no sdl2-config (cellAudio uses SDL2 off Windows)')
            raise SystemExit(0)
        cflags = subprocess.run([cfg, '--cflags'], capture_output=True, text=True, check=True).stdout.split()
        libs = subprocess.run([cfg, '--libs'], capture_output=True, text=True, check=True).stdout.split()
        cmd = [os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG', '-I' + str(root / 'include')] \
            + cflags + [str(source), '-o', str(binary)] + libs + ['-lm', '-pthread']
    subprocess.run(cmd, check=True, timeout=120)
    subprocess.run([str(binary)], check=True, timeout=10)
