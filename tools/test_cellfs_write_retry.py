#!/usr/bin/env python3
"""A failed cellFsWrite must not make a later write fail too
(libs/filesystem/cellFs.c).

ferror() is sticky. The fixture limits the file size, writes past it, raises
the limit and writes again; without clearerr() the second write returns EIO
even though it wrote every byte. A small write past the limit must fail too,
which takes checking the flush. POSIX only (RLIMIT_FSIZE).
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]


def main():
    if os.name == 'nt':
        print('SKIP: the fixture uses RLIMIT_FSIZE')
        return 0
    with tempfile.TemporaryDirectory(prefix='cellfs-write-retry-') as tmp:
        binary = Path(tmp) / 'fixture'
        gc = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
        subprocess.run([
            os.environ.get('CC', 'cc'), '-std=gnu17', '-D_GNU_SOURCE', '-O1',
            '-ffunction-sections', '-fdata-sections',
            '-I' + str(root / 'include'), '-I' + str(root / 'runtime'),
            '-I' + str(root / 'libs/filesystem'), '-I' + str(root),
            str(root / 'tests/test_cellfs_write_retry.c'),
            str(root / 'libs/filesystem/cellFs.c'),
            gc, '-o', str(binary)], check=True, timeout=45)
        result = subprocess.run([str(binary), tmp], timeout=20)
        return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
