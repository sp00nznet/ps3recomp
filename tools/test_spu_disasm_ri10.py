#!/usr/bin/env python3
"""SPU RI10 decode: every 8-bit-opcode immediate form, and the interpreter's
tables built from it.

RI10 is opcd(8) i10(10) ra(7) rt(7). A form missing from RI10_TABLE decodes as
`.word`; the lifter then emits spu_unsupported() in its place and the job runs
on with a wrong register, which is how Twisted Metal's 0x29F5F1C692D71C34 job
lost four `orbi`s. The lifter already maps orbi/xorhi/xorbi to their helpers;
only the decoder lacked them.

The pure interpreter's opcode tables (runtime/spu/spu_interp_tables.inc) are
generated from the same decoder by gen_spu_interp.py, so the second check
regenerates them and fails if the committed copy is stale.

Usage:
    python3 tools/test_spu_disasm_ri10.py
"""

import os
import subprocess
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import spu_disasm  # noqa: E402

# mnemonic -> 8-bit opcode, from the Cell BE SPU ISA's RI10 encodings.
RI10_FORMS = {
    "ori": 0x04, "orhi": 0x05, "orbi": 0x06,
    "sfi": 0x0C, "sfhi": 0x0D,
    "andi": 0x14, "andhi": 0x15, "andbi": 0x16,
    "ai": 0x1C, "ahi": 0x1D,
    "stqd": 0x24, "lqd": 0x34,
    "xori": 0x44, "xorhi": 0x45, "xorbi": 0x46,
    "cgti": 0x4C, "cgthi": 0x4D, "cgtbi": 0x4E, "hgti": 0x4F,
    "clgti": 0x5C, "clgthi": 0x5D, "clgtbi": 0x5E, "hlgti": 0x5F,
    "mpyi": 0x74, "mpyui": 0x75,
    "ceqi": 0x7C, "ceqhi": 0x7D, "ceqbi": 0x7E, "heqi": 0x7F,
}


def ri10_word(op8, i10, ra, rt):
    return (op8 << 24) | ((i10 & 0x3FF) << 14) | ((ra & 0x7F) << 7) | (rt & 0x7F)


def check_decode():
    failures = 0
    for mnem, op8 in sorted(RI10_FORMS.items(), key=lambda kv: kv[1]):
        word = ri10_word(op8, 5, 24, 17)
        text = spu_disasm.spu_decode(word, 0).mnemonic
        if text != mnem:
            print(f"[FAIL] op8 0x{op8:02X} decodes as {text!r}, want {mnem!r}")
            failures += 1
    # The four words Twisted Metal's job carries, with their fields.
    for word, want in ((0x06004C11, "orbi $r17, $r24, 1"),
                       (0x06020B0D, "orbi $r13, $r22, 8"),
                       (0x06010B10, "orbi $r16, $r22, 4"),
                       (0x06030B2A, "orbi $r42, $r22, 12")):
        insn = spu_disasm.spu_decode(word, 0)
        got = f"{insn.mnemonic} {insn.operands}".strip()
        if " ".join(got.split()) != want:
            print(f"[FAIL] 0x{word:08X} decodes as {got!r}, want {want!r}")
            failures += 1
    return failures


def check_interp_tables():
    gen = os.path.join(TOOLS, "gen_spu_interp.py")
    committed = os.path.join(ROOT, "runtime", "spu", "spu_interp_tables.inc")
    fresh = subprocess.run([sys.executable, gen], capture_output=True, text=True,
                           check=True).stdout
    with open(committed, encoding="utf-8") as f:
        if f.read() != fresh:
            print("[FAIL] runtime/spu/spu_interp_tables.inc is stale: "
                  "python3 tools/gen_spu_interp.py > runtime/spu/spu_interp_tables.inc")
            return 1
    return 0


def main():
    failures = check_decode() + check_interp_tables()
    if failures:
        print(f"{failures} check(s) FAILED")
        return 1
    print(f"all {len(RI10_FORMS)} RI10 forms decode, and the interpreter tables are current")
    return 0


if __name__ == "__main__":
    sys.exit(main())
