#!/usr/bin/env python3
"""Verify that raw PRX metadata reaches jump-table discovery."""

import json
import os
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)

import ppu_lifter


def main():
    base = 0x02000000
    toc = 0x02008000
    blr = bytes.fromhex("4e800020")
    seen = {}

    def capture(insns, read_u32, toc_candidates, text_lo, text_hi):
        seen["toc"] = toc_candidates
        seen["word"] = read_u32(base)
        seen["bounds"] = (text_lo, text_hi)
        return {}

    with tempfile.TemporaryDirectory() as td:
        image = os.path.join(td, "module_image.bin")
        funcs = os.path.join(td, "module_functions.json")
        output = os.path.join(td, "lifted")
        with open(image, "wb") as f:
            f.write(blr)
        with open(funcs, "w") as f:
            json.dump([{"start": hex(base), "end": hex(base + 4)}], f)

        old_argv = sys.argv
        old_discover = ppu_lifter.discover_jump_tables
        try:
            ppu_lifter.discover_jump_tables = capture
            sys.argv = [
                "ppu_lifter.py", image,
                "--raw", "--base", hex(base), "--toc", hex(toc),
                "--functions", funcs, "--output", output, "--jobs", "1",
            ]
            ppu_lifter.main()
        finally:
            ppu_lifter.discover_jump_tables = old_discover
            sys.argv = old_argv

    expected = {
        "toc": [toc],
        "word": int.from_bytes(blr, "big"),
        "bounds": (base, base + 4),
    }
    if seen != expected:
        print(f"FAIL raw TOC wiring: got {seen!r}, want {expected!r}")
        return 1
    print("[raw-prx-toc] raw segment and module TOC reached jump-table discovery")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
