#!/usr/bin/env python3
"""SPURS policy-module hops through the real scheduler (tests/bench_spurs_pm_hop.c).

Every hop must run, with and without PS3RECOMP_SPURS_WORKER_WAKE=1, at one and
six lanes. With per-worker wake on, no worker may wake with nothing to run.
Timings are printed, not asserted. `--hops N` and `--runs R` for a benchmark run.
"""
import argparse
import os
import platform
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: POSIX fixture')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
ap = argparse.ArgumentParser()
ap.add_argument('--hops', type=int, default=2000)
ap.add_argument('--runs', type=int, default=1)
args = ap.parse_args()
units = ['spu_channels', 'spu_drain', 'spu_lockstep', 'spu_coherency', 'spu_workload',
         'spurs_policy', 'spurs_job', 'spu_tsp_weak', 'spu_vm_pagemap',
         'spurs_policy_blob_weak', 'spu_interp']
with tempfile.TemporaryDirectory(prefix='spurs-pm-hop-') as tmp:
    out = Path(tmp) / 'bench'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-D_GNU_SOURCE', '-O2', '-UNDEBUG',
                    '-ffunction-sections', '-fdata-sections',
                    '-Wl,-dead_strip' if platform.system() == 'Darwin' else '-Wl,--gc-sections',
                    '-I' + str(root / 'include'), '-I' + str(root / 'runtime/spu'), '-I' + str(root / 'runtime/platform'),
                    str(root / 'tests/bench_spurs_pm_hop.c'),
                    *[str(root / 'runtime/spu' / f'{u}.c') for u in units],
                    str(root / 'runtime/platform/win32_compat.c'), '-pthread', '-lm', '-o', str(out)], check=True)
    base = {k: v for k, v in os.environ.items() if not k.startswith(('PS3RECOMP_', 'PS3_', 'SPU_', 'SPURS_'))}
    failed = []
    for lanes in (1, 6):
        for wake in ('0', '1'):
            for _ in range(args.runs):
                env = dict(base, PS3RECOMP_SPURS_WORKER_WAKE=wake)
                r = subprocess.run([str(out), str(args.hops), str(lanes), 'spu'], env=env,
                                   capture_output=True, text=True, timeout=120)
                line = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ''
                print(line or r.stderr[-500:])
                if r.returncode:
                    failed.append(f'lanes={lanes} wake={wake}: exit {r.returncode}')
                    continue
                fields = dict(f.split('=', 1) for f in line.split() if '=' in f)
                if wake == '1' and fields.get('idle_worker_wakes') != '0':
                    failed.append(f'lanes={lanes}: a worker woke with nothing to run under per-worker wake')
    if failed:
        raise SystemExit('FAIL: ' + '; '.join(failed))
    print('PASS')
