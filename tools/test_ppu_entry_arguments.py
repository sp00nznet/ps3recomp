#!/usr/bin/env python3
"""Run the real PPU ELF loader/entry path against an overlapping synthetic image."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from check_ppu_scaffold import stub_header

ROOT = Path(__file__).resolve().parents[1]

def image(overlap_stack=False):
    # Entry OPD + a PT_LOAD sentinel exactly where the old argv scratch lived.
    loads = [(0x10000, struct.pack('>II', 0x10000, 0x10000), 0x1000),
             (0xB00000, b'QRST' * 64, 0x1000)]
    if overlap_stack:
        # A sparse image with a BSS-only segment covering the entry stack top.
        loads.append((0xD00F0000, b'', 0x10000))
    out = bytearray(0x1000)
    out[:16] = b'\x7fELF\x02\x02\x01' + bytes(9)
    struct.pack_into('>HHIQQQIHHHHHH', out, 16, 2, 21, 1, 0x10000, 64, 0, 0, 64, 56, len(loads), 0, 0, 0)
    for i, (addr, data, memsz) in enumerate(loads):
        offset = len(out)
        struct.pack_into('>IIQQQQQQ', out, 64+i*56, 1, 6, offset, addr, addr, len(data), memsz, 0x1000)
        out.extend(data)
    return out

def main():
    if os.name == 'nt':
        print('SKIP: standalone fixture currently uses the POSIX mmap/toolchain')
        return
    with tempfile.TemporaryDirectory(prefix='ppu-entry-arguments-') as tmp:
        tmp = Path(tmp)
        (tmp/'ppu_recomp.h').write_text(stub_header())
        binary = tmp/'test'
        memory = tmp/'memory.o'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-c',
                        str(ROOT/'runtime/syscalls/sys_memory.c'), '-o', str(memory)], check=True)
        flags = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-ldl']
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1',
                        '-ffunction-sections', '-fdata-sections',
                        '-I'+str(tmp), '-I'+str(ROOT/'include'), '-I'+str(ROOT/'runtime/platform'),
                        str(ROOT/'runtime/ppu/ppu_loader.cpp'),
                        str(ROOT/'runtime/ppu/tests/test_ppu_entry_arguments.cpp'),
                        str(memory), *flags, '-o', str(binary)], check=True)
        env = {k: v for k,v in os.environ.items() if not k.startswith(('PPU_', 'PS3_', 'PS3RECOMP_', 'SPU_'))}
        env['PS3_BOOT_PATH'] = '/dev_bdvd/PS3_GAME/USRDIR/bin/test.self'
        for label, extras, bad_stack in [('single', False, False), ('multiple', True, False), ('image-stack-overlap', False, True)]:
            elf = tmp/(label+'.elf'); elf.write_bytes(image(bad_stack))
            run_env = dict(env)
            if extras: run_env['PS3_ARGV'] = 'alpha;beta'
            subprocess.run([str(binary), str(elf), 'reject' if bad_stack else 'run'], env=run_env, check=True, timeout=15)
        elf = tmp/'single.elf'
        for mode in ['low', 'outside']:
            subprocess.run([str(binary), str(elf), mode], env=env, check=True, timeout=15)
        large_env = dict(env, PS3_BOOT_PATH='x'*65536)
        subprocess.run([str(binary), str(elf), 'reject'], env=large_env, check=True, timeout=15)
        print('PASS: image/heap preserved, argv/env ABI and frame lifetime, invalid storage rejected')

if __name__ == '__main__':
    main()
