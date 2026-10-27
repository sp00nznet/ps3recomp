#!/usr/bin/env python3
"""Exercise extended module-state queries and their generated NID registration."""
import os
import subprocess
import tempfile
from pathlib import Path
from gen_hle_nids import scan

root = Path(__file__).resolve().parents[1]
assert (0x70E319B5, 'cellSysmoduleIsLoadedEx') in scan(root / 'libs/system/cellSysmodule.c'), 'Missing extended query registration'
if os.name == 'nt':
    print('SKIP: C runtime fixture requires the POSIX toolchain; registration checked')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='ps3-sysmodule-query-') as tmp:
    binary = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c17', '-UNDEBUG',
                    '-I' + str(root / 'include'), '-I' + str(root / 'libs/system'),
                    str(root / 'tests/test_sysmodule_is_loaded_ex.c'),
                    str(root / 'libs/system/cellSysmodule.c'), '-o', str(binary)], check=True, timeout=30)
    env = {key: value for key, value in os.environ.items()
           if key in ("PATH", "HOME", "TMPDIR", "SYSTEMROOT")}
    subprocess.run([str(binary)], check=True, timeout=10, env=env)
