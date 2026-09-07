#!/usr/bin/env python3
"""
Write the synthetic title's image, for runtime/ppu/tests/smoke.

The PPU boot scaffold only ever runs behind a lifted game, which is why nothing
in this repository has ever executed it. The smoke title closes that: a
hand-written program in the lifter's ABI plus this image, so ppu_load_elf, the
entry OPD resolution and the TLS template run against real bytes rather than a
mock.

This is NOT a lifter. The recompiled code lives in smoke_recomp.c and is
compiled natively; what the image carries is everything the RUNTIME loader
reads out of a PS3 executable:

  - a big-endian ELF64 PowerPC ET_EXEC header whose e_entry is an OPD address,
  - the OPD table itself ({u32 code, u32 toc} pairs) -- the entry descriptor
    and the one sys_ppu_thread_create takes,
  - the marker strings the guest prints through sys_tty_write, so the text in
    the log is proof the PT_LOAD data reached guest RAM,
  - an image canary and a BSS tail (memsz > filesz), which the guest checks,
  - a PT_TLS template, which ppu_run copies into the main thread's TLS image.

It does not emit a proc_prx_param segment: the runtime loader
(runtime/ppu/ppu_loader.cpp) reads PT_LOAD and PT_TLS and nothing else. The
offline loader (tools/ppu_loader.py) is what parses proc_prx_param, and it is
not in this path -- the smoke title has no firmware import table to discover,
because its import stubs are written out by hand the way the lifter writes
them, with their NIDs already in the source.

--header writes the smoke title's ppu_recomp.h. The two outputs share one
address map, which is the reason they come from the same script: the OPD in the
image and the func_XXXXXXXX names the source defines have to name the same
guest addresses, and a script that owns both cannot get them out of step. The
header's preamble is imported from ppu_lifter.py rather than copied, the way
tools/check_ppu_scaffold.py does it, so it is the real lifted ABI and not a
lookalike that can drift from it.

  python tools/make_smoke_elf.py --out build/smoke/smoke.elf \
                                 --header build/smoke/ppu_recomp.h
"""

import argparse
import importlib.util
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ---------------------------------------------------------------------------
# The guest address map. Mirrored by runtime/ppu/tests/smoke/smoke_recomp.c,
# which documents each constant on its own side; the two must agree, and a
# disagreement is loud rather than subtle (an OPD that resolves to an
# unregistered code address makes ppu_run refuse to dispatch and say so).
# ---------------------------------------------------------------------------
TEXT_BASE = 0x00010000       # .text, R+X
TEXT_SIZE = 0x00001000
FUNC_STRIDE = 0x100          # one guest function per 256 bytes

DATA_BASE = 0x00020000       # .data + .rodata + .tbss, R+W
OPD_OFF = 0x0000             # OPD table, {u32 code, u32 toc} per entry
STRTAB_OFF = 0x0100          # marker strings, one per step
STR_STRIDE = 0x60
STR_COUNT = 32
TOC_OFF = 0x0D00             # r2 for every function in this image
THREADNAME_OFF = 0x0E00      # sys_ppu_thread_create's name argument
CANARY_OFF = 0x0FF0          # the guest checks this to prove PT_LOAD landed
TLS_OFF = 0x1000             # PT_TLS template
TLS_FILESZ = 0x10
TLS_MEMSZ = 0x40
DATA_FILESZ = TLS_OFF + TLS_FILESZ
DATA_MEMSZ = 0x3000          # the tail past DATA_FILESZ is BSS

CANARY = 0x50533352          # 'PS3R'
TLS_CANARY = 0x544C5330      # 'TLS0'
THREADNAME = b"smoke_worker\x00"

# One guest function per slot, in the order smoke_recomp.c defines them. The
# import stubs are the addresses a real .lib.stub trampoline would occupy; the
# lifter turns those into a single ps3_hle_call(nid), and so does the smoke.
FUNCTIONS = [
    "entry",                       # _start: the OPD in e_entry points here
    "thread_body",                 # sys_ppu_thread_create's entry
    "stub__cellGcmInitBody",
    "stub_cellGcmSetDisplayBuffer",
    "stub_cellGcmGetControlRegister",
    "stub_cellGcmSetFlipCommand",
    "stub_cellGcmGetFlipStatus",
    "stub_sys_process_exit",
    "vblank_handler",              # cellGcmSetVBlankHandler's OPD
    "stub_cellGcmSetVBlankHandler",
]

# The line each step prints through sys_tty_write. Indexed by the step id in
# smoke_recomp.c: the guest reads the string out of its own image, so an empty
# slot means this list and that enum have drifted, and the guest fails on it
# rather than printing nothing and passing.
MARKERS = [
    "[smoke] entry: the loader resolved the entry OPD and dispatched _start",
    "[smoke] image: PT_LOAD data, the BSS tail and the TLS template are live",
    "[smoke] hle: _cellGcmInitBody dispatched through the NID bridge",
    "[smoke] thread: sys_ppu_thread_create returned a thread id",
    "[smoke] thread: the guest thread body is running on its own host thread",
    "[smoke] thread: sys_ppu_thread_join handed back the thread's exit status",
    "[smoke] frame 1: cleared and flipped through the FIFO",
    "[smoke] frame 2: cleared and flipped through the FIFO",
    "[smoke] frame 3: cleared and flipped through the FIFO",
    "[smoke] vblank: the guest's handler ran on the guest thread",
    "[smoke] exit: calling sys_process_exit",
]

PPC_TRAP = 0x7FE00008        # `trap`: the .text is never executed, but it is real


def build_data() -> bytearray:
    """The .data segment's file image."""
    data = bytearray(DATA_FILESZ)

    toc = DATA_BASE + TOC_OFF
    for i in range(len(FUNCTIONS)):
        off = OPD_OFF + i * 8
        struct.pack_into(">II", data, off, TEXT_BASE + i * FUNC_STRIDE, toc)

    for i, text in enumerate(MARKERS):
        if i >= STR_COUNT:
            raise SystemExit(f"too many markers for the string table ({STR_COUNT})")
        line = (text + "\n").encode("ascii")
        if len(line) >= STR_STRIDE:
            raise SystemExit(f"marker {i} does not fit in {STR_STRIDE} bytes: {text}")
        off = STRTAB_OFF + i * STR_STRIDE
        data[off:off + len(line)] = line

    data[THREADNAME_OFF:THREADNAME_OFF + len(THREADNAME)] = THREADNAME
    struct.pack_into(">I", data, CANARY_OFF, CANARY)
    struct.pack_into(">I", data, TLS_OFF, TLS_CANARY)
    return data


def build_elf() -> bytes:
    ehdr_size, phdr_size, phnum = 64, 56, 3
    text_off = 0x1000
    data_off = 0x2000

    text = bytearray(TEXT_SIZE)
    for i in range(len(FUNCTIONS)):
        struct.pack_into(">I", text, i * FUNC_STRIDE, PPC_TRAP)

    data = build_data()

    out = bytearray(data_off + len(data))

    # ELF header: ELFCLASS64 / ELFDATA2MSB / ET_EXEC / EM_PPC64, the shape
    # ppu_load_elf checks before it will touch anything.
    out[0:16] = b"\x7fELF\x02\x02\x01\x00" + b"\x00" * 8
    struct.pack_into(">HHI", out, 16, 2, 21, 1)          # e_type, e_machine, e_version
    struct.pack_into(">QQQ", out, 24,
                     DATA_BASE + OPD_OFF,                # e_entry: an OPD address
                     ehdr_size,                          # e_phoff
                     0)                                  # e_shoff: stripped
    struct.pack_into(">IHHHHHH", out, 48,
                     0,                                  # e_flags
                     ehdr_size, phdr_size, phnum,
                     0, 0, 0)                            # e_shentsize/num/strndx

    def phdr(index, p_type, off, vaddr, filesz, memsz, flags, align):
        struct.pack_into(">IIQQQQQQ", out, ehdr_size + index * phdr_size,
                         p_type, flags, off, vaddr, vaddr, filesz, memsz, align)

    phdr(0, 1, text_off, TEXT_BASE, TEXT_SIZE, TEXT_SIZE, 5, 0x10000)   # PT_LOAD R+X
    phdr(1, 1, data_off, DATA_BASE, DATA_FILESZ, DATA_MEMSZ, 6, 0x10000)  # PT_LOAD R+W
    phdr(2, 7, data_off + TLS_OFF, DATA_BASE + TLS_OFF,
         TLS_FILESZ, TLS_MEMSZ, 4, 0x10)                                  # PT_TLS

    out[text_off:text_off + len(text)] = text
    out[data_off:data_off + len(data)] = data
    return bytes(out)


def build_header() -> str:
    """HEADER_PREAMBLE plus this image's forward declarations.

    Imported from the lifter rather than copied, exactly as
    tools/check_ppu_scaffold.py does: the smoke title compiles against the same
    ppu_context, PPU_THREAD_LOCAL and function-table declarations a real port
    gets, so a change to the lifted ABI reaches this title too.
    """
    spec = importlib.util.spec_from_file_location(
        "ppu_lifter", os.path.join(ROOT, "tools", "ppu_lifter.py"))
    lifter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(lifter)

    lines = [lifter.HEADER_PREAMBLE]
    lines.append("/* The smoke title's guest functions, named the way the lifter")
    lines.append(" * names them: func_<guest address>. Written by "
                 "tools/make_smoke_elf.py")
    lines.append(" * alongside the image whose OPD table points at these "
                 "addresses. */")
    for i, name in enumerate(FUNCTIONS):
        lines.append(f"void func_{TEXT_BASE + i * FUNC_STRIDE:08X}"
                     f"(ppu_context* ctx);  /* {name} */")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", help="write the ELF image here")
    ap.add_argument("--header", help="write the smoke title's ppu_recomp.h here")
    args = ap.parse_args()

    if not args.out and not args.header:
        ap.error("nothing to do: pass --out, --header or both")

    for path, blob in ((args.out, build_elf()),
                       (args.header, build_header())):
        if not path:
            continue
        d = os.path.dirname(os.path.abspath(path))
        if d:
            os.makedirs(d, exist_ok=True)
        mode = "wb" if isinstance(blob, bytes) else "w"
        with open(path, mode) as fh:
            fh.write(blob)
        print(f"[smoke] wrote {path} ({len(blob)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
