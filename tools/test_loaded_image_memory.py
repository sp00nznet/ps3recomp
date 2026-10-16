#!/usr/bin/env python3
"""Loaded ELF data and BSS must survive user and mapper allocations."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from check_ppu_scaffold import stub_header

ROOT = Path(__file__).resolve().parents[1]


def image(loads):
    out = bytearray(0x1000)
    out[:16] = b'\x7fELF\x02\x02\x01' + bytes(9)
    struct.pack_into('>HHIQQQIHHHHHH', out, 16,
                     2, 21, 1, 0x10000, 64, 0, 0, 64, 56, len(loads), 0, 0, 0)
    for i, (addr, data, memsz) in enumerate(loads):
        struct.pack_into('>IIQQQQQQ', out, 64 + i * 56,
                         1, 6, len(out), addr, addr, len(data), memsz, 0x1000)
        out.extend(data)
    return out


def main():
    if os.name == 'nt':
        print('SKIP: standalone loader fixture currently needs POSIX mmap/toolchain')
        return
    with tempfile.TemporaryDirectory(prefix='loaded-image-memory-') as tmp:
        tmp = Path(tmp)
        (tmp / 'ppu_recomp.h').write_text(stub_header())
        elf = tmp / 'image.elf'
        elf.write_bytes(image([
            (0x10000000, b'QRST' * 64, 0x71000),
            (0x10080000, b'ABCD' * 64, 0x700000),
            (0x40000000, b'MAPS' * 64, 0x180001),
            (0x40200000, b'', 0x10000),
        ]))
        empty = tmp / 'empty.elf'
        empty.write_bytes(image([]))
        obj = tmp / 'memory.o'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1',
                        '-ffunction-sections', '-fdata-sections', '-c',
                        str(ROOT / 'runtime/syscalls/sys_memory.c'), '-o', str(obj)],
                       check=True, timeout=60)
        flags = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-ldl']
        binary = tmp / 'fixture'
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1',
                        '-ffunction-sections', '-fdata-sections',
                        '-I' + str(tmp), '-I' + str(ROOT / 'include'),
                        '-I' + str(ROOT / 'runtime/platform'),
                        str(ROOT / 'runtime/ppu/ppu_loader.cpp'),
                        str(ROOT / 'tests/test_loaded_image_memory.cpp'),
                        str(obj), *flags, '-o', str(binary)], check=True, timeout=120)
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(('PPU_', 'PS3_', 'PS3RECOMP_', 'SPU_'))}
        subprocess.run([str(binary), str(elf), str(empty)], env=env, check=True, timeout=20)


if __name__ == '__main__':
    main()
