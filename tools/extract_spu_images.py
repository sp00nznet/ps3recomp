#!/usr/bin/env python3
"""
Scan a PPU ELF / EBOOT for embedded SPU ELF images and extract each one.

PS3 games carry SPU programs as ELF blobs embedded in the PPU binary's data
section (often referenced by sys_spu_image_open via spu_image structs).  An
SPU ELF is identified by:

    bytes 0..3 : 7F 45 4C 46     ELF magic
    byte  5    : 02              EI_DATA = big-endian
    bytes 18-19: 00 17           e_machine = EM_SPU (23)

ELF32 headers, tables, file extents and local-store bounds are validated before
extraction. Image size matches the runtime fingerprint extent, including every
segment, section headers and non-NOBITS section content.

Usage:
    python extract_spu_images.py <ppu_elf> [--output DIR]
"""

from __future__ import annotations

import argparse
import os
import sys

ELF_MAGIC = b"\x7FELF"
EM_SPU    = 23


from spu_elf import InvalidSPUELF, scan_spu_candidates, validate_spu_elf


def parse_spu_elf_size(buf: bytes, off: int) -> int | None:
    """Return the bounded runtime-compatible extent, or None if invalid."""
    try:
        return validate_spu_elf(buf, off)["size"]
    except InvalidSPUELF:
        return None


def find_spu_images(buf: bytes):
    """Yield (offset, size) for every valid embedded SPU ELF occurrence."""
    for offset, info, error in scan_spu_candidates(buf):
        if info is not None:
            yield offset, info["size"]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="PPU ELF / EBOOT.elf to scan")
    ap.add_argument("--output", "-o", default="spu_images",
                    help="Output directory (default: ./spu_images)")
    args = ap.parse_args()

    with open(args.input, "rb") as f:
        buf = f.read()

    os.makedirs(args.output, exist_ok=True)
    count = 0
    total_bytes = 0
    print(f"Scanning {len(buf):,} bytes of {args.input}")
    for idx, (off, size) in enumerate(find_spu_images(buf)):
        out_path = os.path.join(args.output, f"spu_{idx:04d}_at_{off:08X}.elf")
        with open(out_path, "wb") as g:
            g.write(buf[off:off + size])
        print(f"  [{idx:3d}]  offset 0x{off:08X}  size {size:>8,} B  -> {out_path}")
        count += 1
        total_bytes += size

    if count == 0:
        print("No SPU images found.")
        return 1
    print(f"\nExtracted {count} SPU image(s), {total_bytes:,} bytes total.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
