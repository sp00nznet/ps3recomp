#!/usr/bin/env python3
"""Per-thread TLS blocks: every thread a title creates gets its own block.

Builds the real ``ppu_tls_new_block`` (pulled verbatim out of
runtime/ppu/ppu_loader.cpp) into a small host program and asks it for 100
thread blocks with a 0x240-byte TLS image, the size of a small title's. Checks
that every request is granted (0 means the thread silently shares the main
thread's TLS), that no block overlaps another block or the main block's image,
and that the .tdata image was copied into each block.
"""
import os
import re
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: fixture currently requires a POSIX C++ compiler')
    raise SystemExit(0)

root = Path(__file__).resolve().parents[1]
src = (root / 'runtime/ppu/ppu_loader.cpp').read_text()
m = re.search(r'static uint32_t ppu_tls_new_block\(void\)\n\{', src)
assert m, 'ppu_tls_new_block not found'
i, depth = m.end(), 1
while depth:
    depth += {'{': 1, '}': -1}.get(src[i], 0)
    i += 1
func = src[m.start():i]
tls_img = re.search(r'^#define PPU_TLS_IMG\s+(\S+)', src, re.M).group(1)
# #296 spells it VM_PPU_TLS_BASE from runtime/memory/vm.h; resolve that to the number.
if not tls_img[0].isdigit():
    vm_h = (root / 'runtime/memory/vm.h').read_text()
    tls_img = re.search(r'^#define ' + tls_img + r'\s+(\S+)', vm_h, re.M).group(1)

HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define PPU_TLS_IMG %s
static uint8_t* vm_base;
static uint32_t ppu_vm_size = 0x20000000u;
static uint32_t g_tls_vaddr = 0x1000, g_tls_filesz = 0x100, g_tls_memsz = 0x240;
%s
int main()
{
    vm_base = (uint8_t*)calloc(ppu_vm_size, 1);
    for (uint32_t i = 0; i < g_tls_filesz; i++) vm_base[g_tls_vaddr + i] = (uint8_t)(i + 1);
    enum { N = 100 };
    uint32_t blk[N];
    int granted = 0, overlaps = 0, bad_copies = 0;
    for (int i = 0; i < N; i++) {
        uint32_t tp = ppu_tls_new_block();
        blk[i] = tp ? tp - 0x7000u : 0;             /* TLS data starts at r13 - 0x7000 */
        if (!tp) continue;
        granted++;
        if (blk[i] < PPU_TLS_IMG + g_tls_memsz) overlaps++;   /* main block's image */
        for (int j = 0; j < i; j++)
            if (blk[j] && blk[i] < blk[j] + g_tls_memsz && blk[j] < blk[i] + g_tls_memsz) overlaps++;
        if (memcmp(vm_base + blk[i], vm_base + g_tls_vaddr, g_tls_filesz)) bad_copies++;
    }
    printf("granted %%d/%%d, overlaps %%d, bad copies %%d\n", granted, (int)N, overlaps, bad_copies);
    return granted == N && !overlaps && !bad_copies ? 0 : 1;
}
'''

with tempfile.TemporaryDirectory(prefix='ppu-tls-') as tmp:
    c = Path(tmp) / 'tls.cpp'
    exe = Path(tmp) / 'tls'
    c.write_text(HARNESS % (tls_img, func))
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', str(c), '-o', str(exe)], check=True)
    r = subprocess.run([str(exe)], timeout=10)
    print('PASS' if r.returncode == 0 else 'FAIL')
    raise SystemExit(r.returncode)
