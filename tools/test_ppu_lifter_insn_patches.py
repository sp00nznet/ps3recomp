#!/usr/bin/env python3
"""--insn-patches: a patched instruction is lifted in both forms behind its guard.

1. Each patch becomes `if (guard) { <new> } else { <original> }`, the guard is
   declared before the function, and the original's branch label survives.
2. A patch whose `orig` is not the word in the input stops the lift (serial
   and parallel: a worker must not hang the pool).
3. A patch at an address no lifted function covers stops the lift.

Run: python tools/test_ppu_lifter_insn_patches.py
"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

CODE = bytes.fromhex(
    "2f830002"   # 0x10000 cmpwi cr7, r3, 2
    "419d0008"   # 0x10004 bgt   cr7, 0x1000C
    "38600002"   # 0x10008 li    r3, 2
    "4e800020")  # 0x1000C blr


def lift(root, patches, jobs="1"):
    (root / "patches.json").write_text(json.dumps({"patches": patches}))
    out = root / ("out" + jobs)
    cmd = [sys.executable, str(Path(__file__).with_name("ppu_lifter.py")),
           str(root / "code.bin"), "--raw", "--base", "0x10000",
           "--functions", str(root / "functions.json"), "--jobs", jobs,
           "--insn-patches", str(root / "patches.json"), "--output", str(out)]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       text=True, timeout=120)
    text = "".join(p.read_text() for p in sorted(out.glob("ppu_recomp_*.cpp"))) \
        if r.returncode == 0 else ""
    return r, text


def main():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "code.bin").write_bytes(CODE)
        (root / "functions.json").write_text(
            json.dumps([{"start": "0x10000", "end": "0x10010"}]))
        good = [
            {"addr": "0x10004", "orig": "0x419D0008", "new": "0x60000000", "guard": "g_a"},
            {"addr": "0x10008", "orig": "0x38600002", "new": "0x38600001", "guard": "g_b"},
        ]
        r, text = lift(root, good)
        assert r.returncode == 0, r.stderr
        assert 'extern "C" int g_a;' in text and 'extern "C" int g_b;' in text, text
        assert "if (g_a) { /* nop */; } else { if (" in text, text
        assert "goto loc_0001000C; }  /* insn patch 0x00010004: 419D0008 -> 60000000 */" in text, text
        assert ("if (g_b) { ctx->gpr[3] = (int64_t)(int32_t)(1); } else "
                "{ ctx->gpr[3] = (int64_t)(int32_t)(2); }") in text, text
        assert "loc_0001000C:" in text, text

        bad = [dict(good[0], orig="0x419D0010")]
        for jobs in ("1", "2"):
            r, _ = lift(root, bad, jobs)
            assert r.returncode != 0 and "the patch expects 0x419D0010" in r.stderr, \
                (jobs, r.stderr[-400:])

        r, _ = lift(root, [dict(good[0], addr="0x20000")])
        assert r.returncode != 0 and "0x00020000" in r.stderr, r.stderr[-400:]
    print("PASS: insn patches lift both forms behind their guard and refuse a mismatch")


if __name__ == "__main__":
    main()
