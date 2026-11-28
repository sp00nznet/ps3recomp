#!/usr/bin/env python3
"""Controlled wait-loop contracts plus real-thread priority-start integration.

--negative-controls compiles deliberately broken runtime copies and requires the
corresponding correctness assertions to reject each one. Compilation errors,
crashes and watchdog timeouts never count as a successful negative control.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--negative-controls', action='store_true')
parser.add_argument('--host-delay-ms', type=int, default=0)
args = parser.parse_args()
if not 0 <= args.host_delay_ms <= 1000:
    parser.error('host delay must be between 0 and 1000 ms')
if os.name == 'nt':
    print('SKIP: fixture currently uses POSIX linking')
    raise SystemExit(0)
linker = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-lm']
env = {k: v for k, v in os.environ.items() if k not in ('PS3_PRIO_START', 'PS3_PRIO_TEST_HOST_DELAY_MS')}
env['PS3_PRIO_TEST_HOST_DELAY_MS'] = str(args.host_delay_ms)


def compile_fixture(tmp, fixture, name, runtime=None, source=None):
    binary = tmp / name
    defines = ['-DPPU_THREAD_IMPLEMENTATION="' + str(runtime) + '"'] if runtime else []
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-D_GNU_SOURCE', '-O1', '-UNDEBUG',
                    '-ffunction-sections', '-fdata-sections',
                    '-I' + str(root / 'include'), '-I' + str(root / 'runtime'),
                    '-I' + str(root / 'runtime/syscalls'), *defines,
                    str(source or root / 'tests' / fixture), *linker, '-o', str(binary)], check=True)
    return binary


def run(binary, on=False, reject=None):
    settings = dict(env)
    if on:
        settings['PS3_PRIO_START'] = '1'
    result = subprocess.run([str(binary)], timeout=60, env=settings,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if reject:
        if result.returncode != 1 or reject not in result.stderr:
            sys.stdout.write(result.stdout)
            sys.stderr.write(result.stderr)
            raise AssertionError(f'{binary.name}: expected a correctness assertion, got exit {result.returncode}')
        print(f'PASS negative control: {binary.name} rejected by {reject}')
    else:
        sys.stdout.write(result.stdout)
        if result.returncode:
            sys.stderr.write(result.stderr)
        result.check_returncode()


with tempfile.TemporaryDirectory(prefix='ppu-prio-start-') as temporary:
    tmp = Path(temporary)
    wait = compile_fixture(tmp, 'test_ppu_prio_wait.c', 'wait-contracts')
    run(wait)
    integration = compile_fixture(tmp, 'test_ppu_prio_start.c', 'thread-integration')
    run(integration)
    run(integration, on=True)
    if args.negative_controls:
        original = (root / 'runtime/syscalls/sys_ppu_thread.c').read_text()
        terminal = 'if (state == PPU_THREAD_STATE_FINISHED || state == PPU_THREAD_STATE_FREE) return;'
        mutations = [
            ('ignore-blocked', 'wait',
             'if (__atomic_load_n(&t->blocked, __ATOMIC_ACQUIRE)) return;',
             'if (0 && __atomic_load_n(&t->blocked, __ATOMIC_ACQUIRE)) return;'),
            ('ignore-finished', 'wait', terminal, 'if (state == PPU_THREAD_STATE_FREE) return;'),
            ('ignore-free', 'wait', terminal, 'if (state == PPU_THREAD_STATE_FINISHED) return;'),
            ('detached-is-finished', 'wait', terminal,
             'if (state == PPU_THREAD_STATE_FINISHED || state == PPU_THREAD_STATE_FREE || state == PPU_THREAD_STATE_DETACHED) return;'),
            ('early-cap', 'wait', 'waited >= PRIO_START_TIMEOUT_MS', 'waited >= PRIO_START_TIMEOUT_MS - 1'),
            ('late-cap', 'wait', 'waited >= PRIO_START_TIMEOUT_MS', 'waited > PRIO_START_TIMEOUT_MS'),
            ('missing-cap', 'wait', 'waited >= PRIO_START_TIMEOUT_MS', '0 && waited >= PRIO_START_TIMEOUT_MS'),
            ('skip-priority-wait', 'integration', 'if (preempts) wait_first_block(t, thread_id);',
             'if (0 && preempts) wait_first_block(t, thread_id);'),
            ('equal-priority-preempts', 'integration', 'priority < creator_priority(ctx->thread_id)',
             'priority <= creator_priority(ctx->thread_id)'),
            ('zero-priority-fallback', 'integration', 'return c->priority;',
             'return c->priority ? c->priority : PPU_DEFAULT_PRIORITY;'),
        ]
        for name, kind, before, after in mutations:
            if original.count(before) != 1:
                raise AssertionError(f'{name}: runtime mutation anchor changed')
            runtime = tmp / (name + '.c')
            runtime.write_text(original.replace(before, after))
            fixture = 'test_ppu_prio_wait.c' if kind == 'wait' else 'test_ppu_prio_start.c'
            binary = compile_fixture(tmp, fixture, name, runtime)
            run(binary, on=True, reject='FAIL wait' if kind == 'wait' else 'FAIL integration')
        # Restore the old out-of-range guest stack offsets. The protection
        # ownership check must reject this before mprotect can harm host pages.
        fixture = (root / 'tests/test_ppu_prio_start.c').read_text()
        before = 'g_vm_stack_alloc.next_addr = 0x10000u; g_vm_stack_alloc.region_end = ppu_vm_size;'
        after = 'g_vm_stack_alloc.next_addr = 0x10000000u; g_vm_stack_alloc.region_end = 0xA0000000u;'
        if fixture.count(before) != 1:
            raise AssertionError('guard-outside-vm: fixture mutation anchor changed')
        source = tmp / 'guard-outside-vm.c'
        source.write_text(fixture.replace(before, after).replace('../runtime/', (root / 'runtime').as_posix() + '/'))
        binary = compile_fixture(tmp, '', 'guard-outside-vm', source=source)
        run(binary, reject='FAIL integration: guard outside fixture VM')
        print('PASS: all 11 PPU priority-start negative controls rejected')
