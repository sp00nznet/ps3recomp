#!/usr/bin/env python3
"""Use the production mapper and exact HLE heap bodies in a synthetic VM."""
import os
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
def main():
    if os.name == 'nt':
        print('SKIP: standalone mapper/HLE-heap fixture needs a POSIX toolchain')
        return 0
    source = (ROOT / 'libs/system/sysPrxForUser.c').read_text()
    # Two layout-independent slices: the heap table decls (wherever they sit)
    # and the heap-management section, so the fixture also builds on master
    # for the fail-before runs.
    t_begin = source.index('#define MAX_HEAPS')
    t_mark = 'static u32 s_next_heap_id = 1;'
    t_end = source.index(t_mark) + len(t_mark)
    h_mark = source.index('Heap management (wraps malloc)')
    h_begin = source.rindex('/*', 0, h_mark)   # the banner comment opener, not its text
    h_end = source.index('/* The plain libc allocator sysPrxForUser exports', h_begin)
    body = source[t_begin:t_end] + '\n' + source[h_begin:h_end]
    with tempfile.TemporaryDirectory(prefix='ps3-mmapper-hle-heap-') as tmp:
        temp = Path(tmp)
        (temp / 'heap-under-test.inc').write_text(body)
        executable = temp / 'fixture'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                        '-I', str(temp), '-I', str(ROOT / 'include'), str(ROOT / 'tests/test_mmapper_hle_heap.c'),
                        str(ROOT / 'runtime/syscalls/sys_memory.c'),
                        str(ROOT / 'runtime/syscalls/sys_vm.c'), '-pthread',
                        '-o', str(executable)], check=True, timeout=30)
        subprocess.run([str(executable)], check=True, timeout=15)
    return 0
if __name__ == '__main__':
    raise SystemExit(main())
