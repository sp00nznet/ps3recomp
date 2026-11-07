#!/usr/bin/env python3
"""The raw sys_fs_* calls must be able to use an fd opened with cellFs.

On hardware cellFsOpen and the sys_fs syscalls share one kernel fd space.
MLB The Show 16's Bink SDK opens the .BIK with cellFsOpen and then reads it
with sys_fs_lseek/sys_fs_read. Our runtime kept two tables, so the read hit an
empty sys_fs slot, returned EBADF silently, and BinkOpen reported "Not a Bink
file." with no file read at all.

This fixture compiles the real runtime/syscalls/sys_fs.c and supplies a
ppu_cellfs_file_for_fd() backed by a temp file, exactly as ppu_fs.cpp does.
No game run.
"""
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: standalone fixture requires the POSIX C toolchain')
    raise SystemExit(0)

FIXTURE = r'''
#include "runtime/syscalls/sys_fs.h"
#include "runtime/memory/vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
const char* cellGame_get_hdd_title_id(void) { return NULL; }

static FILE* g_cellfs_file;
FILE* ppu_cellfs_file_for_fd(int fd) { return fd == 12 ? g_cellfs_file : NULL; }

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static uint32_t be32(uint32_t ea) {
    const uint8_t* p = vm_base + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t be64(uint32_t ea) {
    return ((uint64_t)be32(ea) << 32) | be32(ea + 4);
}

int main(void) {
    ppu_vm_size = 0x200000;
    vm_base = (uint8_t*)mmap(NULL, ppu_vm_size, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (vm_base == MAP_FAILED) return 2;

    const char payload[] = "BIKi-block-header-0123456789abcdef";
    char host_path[] = "/tmp/sysfs-bridge-XXXXXX";
    int hfd = mkstemp(host_path);
    if (hfd < 0) return 2;
    FILE* w = fdopen(hfd, "wb");
    fwrite(payload, 1, sizeof payload - 1, w);
    fclose(w);
    g_cellfs_file = fopen(host_path, "r+b");
    if (!g_cellfs_file) return 2;

    ppu_context ctx;
    memset(&ctx, 0, sizeof ctx);

    /* sys_fs_read(12, guest 0x100, 0x2c, &nread@0x80) on a cellFs fd. */
    ctx.gpr[3] = 12; ctx.gpr[4] = 0x100; ctx.gpr[5] = 0x20; ctx.gpr[6] = 0x80;
    CHECK(sys_fs_read(&ctx) == CELL_OK);
    CHECK(be64(0x80) == 0x20);
    CHECK(memcmp(vm_base + 0x100, payload, 0x20) == 0);

    /* lseek on the same fd: rewind, then 4 bytes. */
    ctx.gpr[3] = 12; ctx.gpr[4] = 0; ctx.gpr[5] = CELL_FS_SEEK_SET; ctx.gpr[6] = 0x88;
    CHECK(sys_fs_lseek(&ctx) == CELL_OK);
    CHECK(be64(0x88) == 0);
    ctx.gpr[3] = 12; ctx.gpr[4] = 4; ctx.gpr[5] = CELL_FS_SEEK_SET; ctx.gpr[6] = 0x90;
    CHECK(sys_fs_lseek(&ctx) == CELL_OK);
    CHECK(be64(0x90) == 4);

    /* And a read from the new offset sees the bridged stream position. */
    memset(vm_base + 0x200, 0, 8);
    ctx.gpr[3] = 12; ctx.gpr[4] = 0x200; ctx.gpr[5] = 4; ctx.gpr[6] = 0x98;
    CHECK(sys_fs_read(&ctx) == CELL_OK);
    CHECK(be64(0x98) == 4);
    CHECK(memcmp(vm_base + 0x200, payload + 4, 4) == 0);

    /* fstat on the bridged fd reports the real size at +0x24. */
    ctx.gpr[3] = 12; ctx.gpr[4] = 0x300;
    CHECK(sys_fs_fstat(&ctx) == CELL_OK);
    CHECK(be64(0x300 + 0x24) == (uint64_t)(sizeof payload - 1));

    /* sys_fs_write on a cellFs fd reaches the file, not the "pretend it
     * worked" path for a bad fd. */
    ctx.gpr[3] = 12; ctx.gpr[4] = 0; ctx.gpr[5] = CELL_FS_SEEK_SET; ctx.gpr[6] = 0x88;
    CHECK(sys_fs_lseek(&ctx) == CELL_OK);
    memcpy(vm_base + 0x400, "XXXX", 4);
    ctx.gpr[3] = 12; ctx.gpr[4] = 0x400; ctx.gpr[5] = 4; ctx.gpr[6] = 0xA0;
    CHECK(sys_fs_write(&ctx) == CELL_OK);
    CHECK(be64(0xA0) == 4);
    fclose(g_cellfs_file);
    char back[4] = {0};
    FILE* r = fopen(host_path, "rb");
    CHECK(r && fread(back, 1, 4, r) == 4 && memcmp(back, "XXXX", 4) == 0);
    fclose(r);
    unlink(host_path);
    printf("PASS: %u sys_fs/cellFs fd bridge checks\n", checks);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='sysfs-bridge-') as name:
    tmp = Path(name)
    fixture = tmp / 'fixture.c'
    fixture.write_text(FIXTURE)
    obj = tmp / 'sys_fs.o'
    binary = tmp / 'fixture'
    common = ['-O1', '-UNDEBUG', '-ffunction-sections', '-fdata-sections',
              '-I' + str(ROOT), '-I' + str(ROOT / 'include')]
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', *common, '-c',
                    str(ROOT / 'runtime/syscalls/sys_fs.c'), '-o', str(obj)],
                   check=True)
    linker = (['-Wl,-dead_strip'] if platform.system() == 'Darwin'
              else ['-Wl,--gc-sections'])
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', *common,
                    str(fixture), str(obj), *linker, '-o', str(binary)],
                   check=True)
    subprocess.run([str(binary)], check=True, timeout=10, cwd=tmp)
