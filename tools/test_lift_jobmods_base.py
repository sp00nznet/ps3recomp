#!/usr/bin/env python3
"""lift_jobmods.py lifts each WWS job module at the link base its header names (+0x28).

Resistance: Fall of Man ships job modules linked at LS 0x7000 next to the usual
0x4000 ones; lifted at 0x4000, every function and branch target of those was
0x3000 short of where the job manager runs them. Synthetic modules only: a
header, a call and two returns.
"""
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import lift_jobmods  # noqa: E402

BASE = lift_jobmods.BASE

NOP, BI_R0 = 0x40200000, 0x35000000
BRSL_R0_4 = (0x066 << 23) | (4 << 7)        # brsl $0, +4 words: +0x30 -> +0x40


def module(base_word, magic=0xC0DEC0DE):
    hdr = struct.pack(">4I", NOP, NOP, NOP, NOP)              # the four ila slots
    hdr += struct.pack(">4I", 0x30, 0x60, 0, 0x40)          # entry offset, size
    hdr += struct.pack(">4I", magic, 4, base_word, 4)       # magic, ., link base, .
    code = struct.pack(">8I", BRSL_R0_4, BI_R0, NOP, NOP, NOP, BI_R0, 0, 0)
    return hdr + code


CASES = {                      # sha -> (header base word, magic, expected lift base)
    "aaaaaaaaaaaa": (0x7000, 0xC0DEC0DE, 0x7000),
    "bbbbbbbbbbbb": (0x4000, 0xC0DEC0DE, 0x4000),
    "cccccccccccc": (0, 0xC0DEC0DE, BASE),          # no base: the job code buffer
    "dddddddddddd": (0x7004, 0xC0DEC0DE, BASE),     # unaligned
    "eeeeeeeeeeee": (0x3FFF0, 0xC0DEC0DE, BASE),    # would run past local store
    "ffffffffffff": (0x7000, 0x12345678, BASE),     # not a WWS module header
}


def main():
    fails = 0
    module_base = getattr(lift_jobmods, "module_base", None)
    if module_base is None:
        print("FAIL lift_jobmods has no module_base")
        fails += 1
    for sha, (word, magic, want) in CASES.items() if module_base else ():
        got = module_base(module(word, magic))
        if got != want:
            print(f"FAIL module_base({sha}: 0x{word:X}) = 0x{got:X}, want 0x{want:X}")
            fails += 1
    with tempfile.TemporaryDirectory(prefix="lift-jobmods-base-") as tmp:
        mods, out = Path(tmp) / "mods", Path(tmp) / "out"
        mods.mkdir()
        for sha in ("aaaaaaaaaaaa", "bbbbbbbbbbbb"):
            (mods / f"jobmod_{sha}_e30.bin").write_bytes(module(CASES[sha][0]))
        r = subprocess.run([sys.executable, str(ROOT / "tools/lift_jobmods.py"),
                            "--jobmods", str(mods), "--out", str(out)],
                           capture_output=True, text=True)
        if r.returncode:
            print(r.stdout, r.stderr)
            print("FAIL lift_jobmods.py exited", r.returncode)
            return 1
        manifest = {m["name"]: m for m in json.loads((out / "jobmods_manifest.json").read_text())}
        for sha in ("aaaaaaaaaaaa", "bbbbbbbbbbbb"):
            name, base = f"jobmod_{sha}_e30", CASES[sha][2]
            m = manifest[name]
            if (m["base"], m["entry"]) != (base, base + 0x30):
                print(f"FAIL {name}: manifest base 0x{m['base']:X} entry 0x{m['entry']:X}, "
                      f"want 0x{base:X} / 0x{base + 0x30:X}")
                fails += 1
            src = (out / name / "spu_recomp.c").read_text()
            funcs = sorted(int(a, 16) for a in re.findall(rf"^void {name}_spu_func_([0-9A-Fa-f]{{8}})\(spu_context",
                                                          src, re.M))
            if funcs != [base + 0x30, base + 0x40]:
                print(f"FAIL {name}: functions at {[hex(a) for a in funcs]}, "
                      f"want {[hex(base + 0x30), hex(base + 0x40)]}")
                fails += 1
    if fails:
        print(f"{fails} check(s) failed")
        return 1
    print("PASS: job modules lift at their header's link base (0x7000 and 0x4000), "
          "and fall back to 0x4000 when the header names none")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
