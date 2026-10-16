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
    begin = source.index('#define YZ_HEAP_BASE')
    end = source.index('/* _sys_heap_create_heap', begin)
    body = source[begin:end]
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
