#!/usr/bin/env python3
"""SPURS policy-module hops through the real scheduler (tests/bench_spurs_pm_hop.c).

Every hop must run, with and without PS3RECOMP_SPURS_WORKER_WAKE=1, at one and
six lanes. With per-worker wake on, no worker may wake with nothing to run.

PS3RECOMP_SPURS_STORE_WAKE=1: with a PPU stand-in storing each request while
the workers sleep, the scheduler's request-line watch must be registered (and
absent with the switch off), every store must reach the line's watches, and
the store-to-start p50 must be under 500 us (up to three attempts on a busy
host). Other timings are printed, not asserted. `--hops N` and `--runs R` for a
benchmark run.
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
    def ppu_case(store_wake, hops, lanes, mode):
        env = dict(base, PS3RECOMP_SPURS_STORE_WAKE=store_wake)
        r = subprocess.run([str(out), str(hops), str(lanes), mode], env=env,
                           capture_output=True, text=True, timeout=120)
        line = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ''
        print(f'store_wake={store_wake} ' + (line or r.stderr[-500:]))
        if r.returncode:
            failed.append(f'store_wake={store_wake} {mode}: exit {r.returncode}')
            return None
        return dict(f.split('=', 1) for f in line.split() if '=' in f)
    for store_wake in ('0', '1'):
        ppu_case(store_wake, args.hops, 2, 'ppu')
    for attempt in range(3):
        on = ppu_case('1', 300, 2, 'ppu-idle=200~1000')
        off = ppu_case('0', 300, 2, 'ppu-idle=200~1000')
        if on is None or off is None:
            break
        if on['request_watched'] != '1' or off['request_watched'] != '0':
            failed.append('the request-line watch is not registered exactly when the store wake is on')
            break
        if int(on['ppu_stores']) < 1 or on['line_writes'] != on['ppu_stores']:
            failed.append(f"{on['ppu_stores']} PPU stores reached the request line's watches {on['line_writes']} times")
            break
        if float(on['p50_us']) < 500:
            print(f"store wake p50 {on['p50_us']} us vs {off['p50_us']} us with the timed pass")
            break
    else:
        failed.append('the store wake p50 was not under 500 us in any attempt')
    if failed:
        raise SystemExit('FAIL: ' + '; '.join(failed))
    print('PASS')
