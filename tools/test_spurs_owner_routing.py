#!/usr/bin/env python3
"""SPURS owner routing through production mailboxes and real LV2 queues.

Extract only the SPURS binding/delivery functions to avoid linking unrelated
HLE modules. Non-SPURS group and blocking-receive dependencies abort if called.
The queue create/push/tryreceive/destroy/cancel implementation is linked whole.
"""
import os
import platform
import subprocess
import tempfile
from pathlib import Path
from msvc_env import environ

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='spurs-owner-routing-') as directory:
    temp = Path(directory)
    source = (root / 'libs/spurs/cellSpurs.c').read_text(encoding='utf-8')
    start = source.index('/* The lv2 event queues')
    end = source.index('/* Validate guest backing', start)
    (temp / 'spurs_ports_under_test.inc').write_text(source[start:end], encoding='utf-8')
    source = (root / 'runtime/syscalls/lv2_register.c').read_text(encoding='utf-8')
    start = source.index('/* User-event ports are independent')
    end = source.index('static int64_t sys_spu_thread_group_disconnect_event_handler', start)
    source = source[start:end]
    start = source.index('static int64_t sys_spu_thread_group_connect_event_all_threads_handler')
    end = source.index('/* The interrupt mailbox', start)
    (temp / 'spurs_delivery_under_test.inc').write_text(source[:start] + source[end:], encoding='utf-8')
    env = environ()
    test = root / 'libs/spurs/tests/test_owner_routing.c'
    event = root / 'runtime/syscalls/sys_event.c'
    obj = temp / 'event.obj'
    binary = temp / ('test.exe' if os.name == 'nt' else 'test')
    if os.name == 'nt':
        common = [os.environ.get('CC', 'clang-cl'), '/nologo', '/W0', '/O1', '/Gy', '/Gw', '/UNDEBUG',
                  '/D_CRT_SECURE_NO_WARNINGS', '/I' + str(root), '/I' + str(root / 'include'),
                  '/I' + str(root / 'runtime'), '/I' + str(temp)]
        commands = [common + ['/c', str(event), '/Dsys_event_flag_set=audit_unused_flag_set', '/Fo:' + str(obj)],
                    common + [str(test), str(obj), '/Fe:' + str(binary), '/link', '/OPT:REF']]
    else:
        common = [os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG', '-ffunction-sections',
                  '-fdata-sections', '-I' + str(root), '-I' + str(root / 'include'),
                  '-I' + str(root / 'runtime'), '-I' + str(temp), '-pthread']
        commands = [common + ['-c', str(event), '-Dsys_event_flag_set=audit_unused_flag_set', '-o', str(obj)],
                    common + [str(test), str(obj), '-Wl,-dead_strip' if platform.system() == 'Darwin' else '-Wl,--gc-sections', '-o', str(binary)]]
    for command in commands:
        subprocess.run(command, cwd=temp, env=env, check=True)
    cases = ['single', 'distinct-ports', 'same-static', 'same-dynamic', 'reverse-order', 'throw-collision',
             'duplicate-no-mutation', 'detach', 'finalize-helper', 'finalized-sender', 'reuse-with-detach',
             'shared-queue-finalize', 'capacity', 'unknown-owner', 'full-queue', 'malformed-mailbox', 'concurrent', 'unowned-raw']
    for case in cases:
        subprocess.run([str(binary), case], cwd=temp, env=env, check=True, timeout=15)
    print('PASS: all', len(cases), 'SPURS routing cases')
