#!/usr/bin/env python3
"""CPU-only register-run and actual FIFO equivalence checks. --bench records
raw timings without thresholds; --baseline-source selects an original walker
for the FIFO benchmark (e.g. git show upstream/master:libs/video/cellGcmSys.c).
No GPU, windows, guest assets or graphics library is used.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--bench', action='store_true')
p.add_argument('--baseline-source', type=Path)
a = p.parse_args()
if os.name == 'nt':
    print('SKIP: FIFO scaffold requires POSIX linking; dispatcher CMake test is portable')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='rsx-plain-run-') as tmp:
    linker = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-lm']
    cc = [os.environ.get('CC', 'cc'), '-std=gnu17', '-O2', '-UNDEBUG',
          '-ffunction-sections', '-fdata-sections', '-I'+str(root/'include'), '-I'+str(root/'libs/video')]
    for name in ('rsx_plain_run', 'gcm_plain_runs'):
        binary = Path(tmp)/name
        sources = [str(root/'libs/video/tests'/('test_'+name+'.c'))]
        extra = []
        sources += [str(root/'libs/video/rsx_dispatch.c')]
        if name == 'gcm_plain_runs' and a.baseline_source:
            extra = ['-DGCM_TEST_SOURCE="'+str(a.baseline_source.resolve())+'"']
        subprocess.run([*cc, *extra, *sources, *linker, '-o', str(binary)], check=True)
        if a.bench:
            if name == 'gcm_plain_runs':
                subprocess.run([str(binary), 'bench'], check=True, timeout=60)
            continue
        subprocess.run([str(binary)], check=True, timeout=30)
        if name == 'rsx_plain_run':
            # Prove that equivalence detects a corrupted admitted write.
            r = subprocess.run([str(binary), 'mutate'], capture_output=True, text=True, timeout=30)
            if r.returncode == 0:
                raise SystemExit('mutation witness unexpectedly passed')
            print('PASS mutation witness rejects corrupted admitted register')
        else:
            for diagnostic in ('RSX_TRACE', 'GCM_WORD_STATS', 'GCM_METHOD_HIST'):
                subprocess.run([str(binary), diagnostic], check=True, timeout=30)
