"""Self-check for objdump_audit's line parser + decoder agreement.

No SDK/objdump needed: feed a synthetic objdump snippet (real Cell instruction words) and
assert the parser recovers address/word/mnemonic -- including a record-form ".": the trailing
dot is exactly what a naive \\w+ regex drops, silently mis-diffing every dot-form. Then check
our own decoder agrees on the same words (directly or via a known simplified-mnemonic alias).

Run: python tools/test_objdump_audit.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import objdump_audit as oa
from ppu_disasm import decode as ppu_decode

# A few real PPU instruction lines in ppu-lv2-objdump's format (addr: bytes  mnemonic ops).
SNIPPET = """\
Disassembly of section .text:

00010324 <foo>:
   10324:\t7c 60 1b 78 \tmr      r0,r3
   10328:\t37 ff ff ff \taddic.  r31,r31,-1
   1032c:\t60 00 00 00 \tnop
   10330:\t4e 80 00 20 \tblr
   10334:\t00 00 00 00 \t.long 0x0
"""


def main() -> int:
    rows = oa.parse_objdump_text(SNIPPET)
    got = {addr: (word, mn) for addr, word, mn in rows}

    # .long data line is dropped; the four instructions are parsed.
    assert 0x10334 not in got, "data (.long) line should be skipped"
    assert set(got) == {0x10324, 0x10328, 0x1032c, 0x10330}, sorted(hex(a) for a in got)

    # The record-form trailing "." must survive parsing (the bug this guards).
    assert got[0x10328][1] == "addic.", got[0x10328]

    # Words decoded big-endian: 0x7c601b78 = "mr r0,r3" alias of "or r0,r3,r3".
    assert got[0x10324][0] == 0x7C601B78, hex(got[0x10324][0])

    # Our decoder agrees with objdump on each (directly, or via the alias table).
    for addr, (word, oracle) in got.items():
        ours = ppu_decode(word, addr).mnemonic.lower()
        assert ours == oracle or (ours, oracle) in oa.ALIAS, \
            f"0x{addr:X}: ours {ours!r} vs objdump {oracle!r}"

    print("ok: objdump line parser (incl. record-form '.') and decoder agreement pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
