#!/usr/bin/env python3
"""The FIFO walker hands rsx_state only the methods that matter once the draw
engine runs without a vtable backend (libs/video/tests/test_gcm_legacy_state_filter.c,
which compiles cellGcmSys.c in).
Run twice: as shipped, and with RSX_TRACE (argument "trace"), which keeps every method."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: the GCM get fixture uses POSIX linking')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='gcm-legacy-state-filter-') as tmp:
    binary = Path(tmp) / 'test'
    linker = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-lm']
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-ffunction-sections', '-fdata-sections',
                    '-I' + str(root / 'include'), '-I' + str(root / 'libs/video'),
                    str(root / 'libs/video/tests/test_gcm_legacy_state_filter.c'), *linker,
                    '-o', str(binary)], check=True)
    for args in ([], ['trace']):
        result = subprocess.run([str(binary), *args], timeout=30, capture_output=True, text=True)
        print(result.stdout.strip())
        if result.returncode:
            print(result.stderr[-2000:])
            raise SystemExit('test_gcm_legacy_state_filter failed')
