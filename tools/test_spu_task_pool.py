#!/usr/bin/env python3
"""The opt-in async SPU task thread pool (runtime/spu/spu_task_pool.h):
reuse, no task queued behind another, multi-submitter stress and idle
retirement. Synthetic: tests/test_spu_task_pool.c runs plain host tasks."""
import os
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: the task pool is POSIX only (Windows keeps one thread per task)')
    raise SystemExit(0)

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='ps3-spu-task-pool-') as tmp:
    binary = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-pthread', '-I' + str(root / 'runtime/spu'),
                    str(root / 'tests/test_spu_task_pool.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=120)
