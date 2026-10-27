"""objdump_audit.py - cross-check our PPU/SPU decoders against the GNU objdump oracle.

The PS3 SDK ships the authoritative GNU disassemblers (ppu-lv2-objdump / spu-lv2-objdump).
This runs one over a binary and diffs its mnemonic-per-address against ours (ppu_disasm.decode
/ spu_disasm.spu_decode), so any decode-table bug shows up as a mismatch.

Clean-room posture (matches spu_disasm_audit.py): the SDK is proprietary and NOT part of
this repo. This script only *invokes* an objdump binary you already have and diffs its
plain-text stdout -- no SDK code, headers, or output are copied in. Point --objdump at your
toolchain's disassembler (or set OBJDUMP_PPU / OBJDUMP_SPU).

    python objdump_audit.py t.spu.elf --objdump .../spu-lv2-objdump.exe
    python objdump_audit.py EBOOT.elf --objdump .../ppu-lv2-objdump.exe --show 40

objdump gives (address, raw bytes, mnemonic) per line; we re-decode its bytes with our own
decoder and compare, so no ELF parsing or section-finding is needed -- objdump is the single
source of truth for what is code.
"""
from __future__ import annotations

import argparse
import os
import re
import struct
import subprocess
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from elf_parser import ELFFile
from ppu_disasm import decode as ppu_decode
from spu_disasm import spu_decode

EM_PPC64 = 21
EM_SPU = 23

# objdump -d line: "   c0:\t04 00 01 d0 \tori\t$80,$3,0". The mnemonic starts with a
# letter (skips ".word"/".long" data directives) and may carry a trailing "." (the PPC
# record forms: addic., or., ...), which must be captured or every dot-form mis-diffs.
_LINE = re.compile(r"^\s*([0-9a-fA-F]+):\s+((?:[0-9a-fA-F]{2} )+)\s+([a-zA-Z][a-zA-Z0-9_]*\.?)")

# (ours, objdump) mnemonic pairs that decode to the SAME instruction but display under a
# different name. These are the PowerPC "simplified/extended mnemonics" (Power ISA Book I
# Appendix C): objdump prints the friendly alias, our decoder prints the architected base
# form the lifter operates on. Confirmed against this corpus + the ISA manual; not decode
# bugs. Keep evidence-based -- an unconfirmed entry here could mask a real table swap.
ALIAS = {
    ("or", "mr"),          # mr rA,rS      = or rA,rS,rS
    ("nor", "not"),        # not rA,rS     = nor rA,rS,rS
    ("tw", "trap"),        # trap          = tw 31,r0,r0
    ("rldicl", "clrldi"),  # clrldi rA,rS,n = rldicl rA,rS,0,n
    ("rldicl", "rotldi"),  # rotldi rA,rS,n = rldicl rA,rS,n,0
    ("rlwinm", "clrlwi"),  # clrlwi rA,rS,n = rlwinm rA,rS,0,n,31
    ("rlwinm", "rotlwi"),  # rotlwi rA,rS,n = rlwinm rA,rS,n,0,31
    ("rlwnm", "rotlw"),    # rotlw  rA,rS,rB= rlwnm rA,rS,rB,0,31
    ("mtcrf", "mtocrf"),   # one-CR-field form (bit 11 set); same effect for lifting
    ("addi", "li"), ("addi", "la"), ("addis", "lis"),  # li/la/lis: addi/addis with rA=0
    ("ori", "nop"),        # nop           = ori r0,r0,0
}


def machine(elf_path: str) -> int:
    ef = ELFFile(elf_path); ef.load()
    return ef.elf_header.e_machine


def parse_objdump_text(text: str) -> list[tuple[int, int, str]]:
    """(address, word, objdump_mnemonic) for every 4-byte instruction line."""
    rows = []
    for line in text.splitlines():
        m = _LINE.match(line)
        if not m:
            continue
        raw = bytes(int(b, 16) for b in m.group(2).split())
        if len(raw) != 4:                    # PPU/SPU are fixed 4-byte; skip data lines
            continue
        word = struct.unpack(">I", raw)[0]   # both ISAs are big-endian
        rows.append((int(m.group(1), 16), word, m.group(3).lower()))
    return rows


def run_objdump(objdump: str, elf_path: str) -> list[tuple[int, int, str]]:
    out = subprocess.run([objdump, "-d", elf_path], capture_output=True, text=True).stdout
    return parse_objdump_text(out)


def audit(objdump: str, elf_path: str, is_spu: bool):
    rows = run_objdump(objdump, elf_path)
    decode = (lambda w, a: spu_decode(w, a).mnemonic) if is_spu \
        else (lambda w, a: ppu_decode(w, a).mnemonic)

    total = matched = 0
    mismatches = Counter()
    samples: dict[tuple[str, str], int] = {}
    for addr, word, oracle in rows:
        ours = decode(word, addr).lower()
        total += 1
        if ours == oracle or (ours, oracle) in ALIAS:
            matched += 1
        else:
            key = (ours, oracle)
            mismatches[key] += 1
            samples.setdefault(key, addr)
    return total, matched, mismatches, samples


def main() -> int:
    ap = argparse.ArgumentParser(description="Diff our PPU/SPU decoder vs the GNU objdump oracle")
    ap.add_argument("inputs", nargs="+", help="ELF(s) to audit (SPU or PPU, auto-detected)")
    ap.add_argument("--objdump", help="objdump binary (else OBJDUMP_PPU / OBJDUMP_SPU env)")
    ap.add_argument("--show", type=int, default=20, help="How many mismatch classes to list")
    args = ap.parse_args()

    grand_total = grand_match = 0
    all_mism = Counter()
    all_samples = {}
    for path in args.inputs:
        mach = machine(path)
        is_spu = mach == EM_SPU
        if mach not in (EM_PPC64, EM_SPU):
            print(f"{path}: machine {mach} is neither PPU nor SPU, skipping", file=sys.stderr)
            continue
        objdump = args.objdump or os.environ.get("OBJDUMP_SPU" if is_spu else "OBJDUMP_PPU")
        if not objdump:
            raise SystemExit("error: pass --objdump or set OBJDUMP_%s"
                             % ("SPU" if is_spu else "PPU"))
        total, matched, mism, samples = audit(objdump, path, is_spu)
        arch = "SPU" if is_spu else "PPU"
        pct = 100 * matched / total if total else 0
        print(f"{os.path.basename(path)} [{arch}]: {matched}/{total} mnemonics match ({pct:.2f}%)")
        grand_total += total; grand_match += matched
        for k, v in mism.items():
            all_mism[k] += v
            all_samples.setdefault(k, samples[k])

    if grand_total:
        print(f"\nTOTAL: {grand_match}/{grand_total} "
              f"({100*grand_match/grand_total:.2f}%)  mismatch classes: {len(all_mism)}")
    if all_mism:
        print(f"\ntop mismatches (ours -> objdump):")
        for (ours, oracle), n in all_mism.most_common(args.show):
            print(f"  {n:5}x  {ours:12} -> {oracle:12}  e.g. 0x{all_samples[(ours, oracle)]:X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
