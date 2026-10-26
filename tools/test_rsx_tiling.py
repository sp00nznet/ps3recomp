#!/usr/bin/env python3
"""RSX tiled main memory: addressing and range copies (libs/video/tests/test_rsx_tiling.c, rsx_tiling.h/.c)."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='rsx-tiling-') as tmp:
    binary = Path(tmp) / ('test.exe' if os.name == 'nt' else 'test')
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-I' + str(root / 'include'), '-I' + str(root / 'libs/video'),
                    str(root / 'libs/video/tests/test_rsx_tiling.c'), str(root / 'libs/video/rsx_tiling.c'), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], timeout=30, capture_output=True, text=True)
    print(result.stdout.strip())
    if result.returncode:
        print(result.stderr[-2000:])
        raise SystemExit('test_rsx_tiling failed')
