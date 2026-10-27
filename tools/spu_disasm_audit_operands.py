#!/usr/bin/env python3
"""
SPU OPERAND-level decode cross-check: tools/spu_disasm.py vs RPCS3's SPUDisAsm
oracle, restricted to words where the MNEMONIC already matches (v1's job,
tools/spu_disasm_audit.py, covers mnemonic mismatches).

This closes the blind spot where the historically most expensive SPU bug
lived: the `il` negative-immediate double-sign-extension (see
tools/spu_disasm.py's IL comment at ~line 387-394) decoded the right
MNEMONIC but the wrong OPERAND VALUE. A mnemonic-only diff (v1) cannot catch
that class at all -- this script can.

Method
------
For each SPU image:
  1. Reuse v1's ELF-segment extraction (extract_code_segment) to get the
     same code bytes + vaddr both sides will decode.
  2. Decode every 4-byte-aligned word with our spu_decode() -> (mnemonic,
     operands string).
  3. Shell out to the SAME oracle exe (spu_dump_tool.exe) used by v1 to get
     its "offset: mnemonic operands" text per word.
  4. Where mnemonics differ, skip (v1's job).
  5. Where mnemonics match, CANONICALIZE both operand strings into an
     ordered list of typed atoms (register-index int, immediate int, or a
     literal string for channel/spr names) and compare atom-by-atom.
  6. Divergences are grouped by mnemonic; each group carries sample
     (addr, ours_raw, oracle_raw, ours_canon, oracle_canon) tuples.

Canonicalization rules (derived from reading RPCS3's SPUDisAsm.h -- GPLv2,
cited by file:line, NO code copied; only the black-box TEXT contract is
reverse-derived):
  - Registers: both sides name registers positionally; oracle uses
    "lr"/"sp"/"r2".."r127" (SPUDisAsm.h spu_reg_name[128], line ~9-27) while
    ours always uses "$rN". Canonical form = the integer index (lr->0,
    sp->1, rN->N). This makes the "lr"/"sp" aliasing purely cosmetic.
  - Immediates: oracle's SignedHex() helper (Emu/CPU/CPUDisAsm.h ~line 115)
    prints abs(value) < 10 as plain decimal, otherwise as "0x.."/"-0x..".
    Canonical form = the signed Python int the text denotes, regardless of
    which base/sign notation was used. This is the crux of the "il" class:
    a double-sign-extension bug changes which INTEGER is denoted, so this
    canonicalization catches it even though both sides "look like hex".
  - Displacement operands ("disp(reg)" for lqd/stqd/stqa/lqa/lqr/stqr):
    split into (immediate, register) atoms in that order; the a-form/r-form
    ops encode an absolute/pc-relative LOAD-STORE ADDRESS (not a signed
    displacement) on the oracle side (DisAsmBranchTarget) -- canonicalize
    both as the resolved absolute address integer.
  - Branch targets (br/bra/brsl/brasl/brz/brnz/brhz/brhnz/hbra/hbrr): both
    sides resolve to an absolute LS address; canonicalize as that integer,
    NOT the raw immediate encoding.
  - Channel operands (rdch/wrch/rchcnt): oracle prints a symbolic name
    (SPUDisAsm.h spu_ch_name[128], line ~49-70); ours prints its own
    CHANNEL_NAMES table (tools/spu_disasm.py line ~283) which is missing at
    least channel 12 ("MFC_RdTagMask" on the oracle side, falls back to a
    numeric "ch12" on ours per current CHANNEL_NAMES gaps). Canonical form
    = the CHANNEL INDEX, not the name string, recovered by inverting each
    side's own name table; a numeric fallback name ("ch12") is parsed back
    to 12 directly. This avoids reporting a channel NAME gap as an operand
    VALUE bug.
  - Comment suffixes ("#GETL", "#0", "#i32[3]", "#0x1h", "#00" etc, emitted
    by comment_constant()/spu_stop_syscall on the oracle side only) are
    stripped before comparison -- ours never emits them and they are not
    semantic operands.

Whitelist: tools/spu_disasm_audit_operands_whitelist.txt, one
"mnemonic:reason" per line, for KNOWN-benign canonicalization gaps (e.g. a
channel name-table gap already understood and not a value bug). Never
whitelist a raw register-index or immediate-VALUE mismatch.

Usage:
    py -3 tools\\spu_disasm_audit_operands.py <image1.elf> [image2.elf ...]
        --oracle-exe PATH [--samples N] [--whitelist PATH]

--oracle-exe defaults to the SPU_DUMP_TOOL_EXE environment variable if set;
otherwise it is required. See tools/spu_disasm_audit.py's module docstring
for how to build the oracle harness.
"""

import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from elf_parser import ELFFile, PT_LOAD  # noqa: E402
from spu_disasm import spu_decode  # noqa: E402

# Never shipped in this repo -- see tools/spu_disasm_audit.py's module
# docstring for the build recipe. Point --oracle-exe at your build, or set
# the SPU_DUMP_TOOL_EXE environment variable.
DEFAULT_ORACLE_EXE = os.environ.get("SPU_DUMP_TOOL_EXE")
DEFAULT_WHITELIST = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "spu_disasm_audit_operands_whitelist.txt",
)

PF_X = 0x1

# ---------------------------------------------------------------------------
# Oracle's own register/channel name tables (SPUDisAsm.h, GPLv2 -- table
# CONTENTS reproduced here as data only, not code/logic, purely so the
# Python side can invert "lr"/"sp"/"r2"/"MFC_Cmd"/etc back to an index for
# canonicalization; see SPUDisAsm.h line ~9 spu_reg_name[128] and line ~49
# spu_ch_name[128]).
# ---------------------------------------------------------------------------
ORACLE_REG_NAMES = ["lr", "sp"] + [f"r{i}" for i in range(2, 128)]
ORACLE_REG_INDEX = {name: i for i, name in enumerate(ORACLE_REG_NAMES)}

ORACLE_CH_NAMES = [
    "SPU_RdEventStat", "SPU_WrEventMask", "SPU_WrEventAck", "SPU_RdSigNotify1",
    "SPU_RdSigNotify2", "ch5", "ch6", "SPU_WrDec", "SPU_RdDec",
    "MFC_WrMSSyncReq", "ch10", "SPU_RdEventMask", "MFC_RdTagMask", "SPU_RdMachStat",
    "SPU_WrSRR0", "SPU_RdSRR0", "MFC_LSA", "MFC_EAH", "MFC_EAL", "MFC_Size",
    "MFC_TagID", "MFC_Cmd", "MFC_WrTagMask", "MFC_WrTagUpdate", "MFC_RdTagStat",
    "MFC_RdListStallStat", "MFC_WrListStallAck", "MFC_RdAtomicStat",
    "SPU_WrOutMbox", "SPU_RdInMbox", "SPU_WrOutIntrMbox", "ch31",
]
ORACLE_CH_INDEX = {name: i for i, name in enumerate(ORACLE_CH_NAMES)}

# Ours: tools/spu_disasm.py CHANNEL_NAMES (addr -> name); invert it.
from spu_disasm import CHANNEL_NAMES as OUR_CHANNEL_NAMES  # noqa: E402
OUR_CH_INDEX = {name: i for i, name in OUR_CHANNEL_NAMES.items()}


def canon_reg_token(tok: str):
    """A register-like token -> integer index, or None if not a register."""
    tok = tok.strip()
    if tok.startswith("$r"):
        try:
            return int(tok[2:])
        except ValueError:
            return None
    if tok in ORACLE_REG_INDEX:
        return ORACLE_REG_INDEX[tok]
    m = re.fullmatch(r"r(\d+)", tok)
    if m:
        return int(m.group(1))
    return None


def canon_channel_token(tok: str, ours: bool):
    """A channel-name-or-fallback token -> channel index, or None."""
    tok = tok.strip()
    m = re.fullmatch(r"ch(\d+)", tok)
    if m:
        return int(m.group(1))
    table = OUR_CH_INDEX if ours else ORACLE_CH_INDEX
    if tok in table:
        return table[tok]
    return None


def canon_spreg_token(tok: str):
    tok = tok.strip()
    m = re.fullmatch(r"spr(\d+)", tok)
    if m:
        return int(m.group(1))
    return None


def parse_int_token(tok: str):
    """A hex/decimal/negative-immediate token -> Python int, or None."""
    tok = tok.strip()
    neg = False
    if tok.startswith("-"):
        neg = True
        tok = tok[1:]
    try:
        if tok.lower().startswith("0x"):
            v = int(tok, 16)
        else:
            v = int(tok, 10)
    except ValueError:
        return None
    return -v if neg else v


def strip_comment(text: str) -> str:
    """Drop a trailing ' #...' comment (comment_constant()/stop-syscall
    annotation on the oracle side; ours never emits one)."""
    idx = text.find("#")
    if idx < 0:
        return text.strip()
    return text[:idx].strip()


def split_operands(mnemonic: str, operands: str, ours: bool):
    """Return a list of canonical atoms for one side's operand string.

    Atom = ("reg", idx) | ("imm", value) | ("spr", idx) | ("ch", idx) |
           ("sym", raw_lowercased_string)   -- last resort, compared as text.

    A disp(reg) operand (e.g. "0x10(sp)" or "-0x50(sp)") is split into two
    atoms: ("imm", disp) then ("reg", idx), matching the register-then-
    immediate-then-register token order used by both decoders for
    lqd/stqd/lqa/stqa/lqr/stqr (ours already prints "rt, disp(ra)"; the
    oracle prints "rt,disp(ra)" -- same order once tokenized).
    """
    operands = strip_comment(operands)
    if not operands:
        return []

    atoms = []
    # First split top-level commas (but not commas inside "(...)").
    parts = []
    depth = 0
    cur = ""
    for c in operands:
        if c == "(":
            depth += 1
            cur += c
        elif c == ")":
            depth -= 1
            cur += c
        elif c == "," and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += c
    parts.append(cur)

    disp_re = re.compile(r"^\s*(-?(?:0x[0-9a-fA-F]+|\d+))\((\S+)\)\s*$")

    for part in parts:
        part = part.strip()
        if not part:
            continue

        m = disp_re.match(part)
        if m:
            imm_val = parse_int_token(m.group(1))
            atoms.append(("imm", imm_val))
            reg_idx = canon_reg_token(m.group(2))
            if reg_idx is not None:
                atoms.append(("reg", reg_idx))
            else:
                atoms.append(("sym", m.group(2).lower()))
            continue

        reg_idx = canon_reg_token(part)
        if reg_idx is not None and (part.startswith("$r") or part in ORACLE_REG_INDEX
                                     or re.fullmatch(r"r\d+", part)):
            atoms.append(("reg", reg_idx))
            continue

        spr_idx = canon_spreg_token(part)
        if spr_idx is not None:
            atoms.append(("spr", spr_idx))
            continue

        if mnemonic in ("rdch", "wrch", "rchcnt"):
            ch_idx = canon_channel_token(part, ours)
            if ch_idx is not None:
                atoms.append(("ch", ch_idx))
                continue

        int_val = parse_int_token(part)
        if int_val is not None:
            atoms.append(("imm", int_val))
            continue

        atoms.append(("sym", part.lower()))

    return atoms


# ---------------------------------------------------------------------------
# Whitelist: (mnemonic) -> reason, for KNOWN-benign canonicalization gaps.
# Format: one "mnemonic:reason text" per line in the whitelist file, '#'
# comments and blank lines ignored. A whitelisted mnemonic's residual
# mismatches are still counted and reported (as INFO), just excluded from
# the "residual real divergences" bucket that gates the acceptance check.
# ---------------------------------------------------------------------------
def load_whitelist(path: str) -> dict:
    wl = {}
    if not path or not os.path.isfile(path):
        return wl
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if ":" in line:
                mn, reason = line.split(":", 1)
            else:
                mn, reason = line, ""
            wl[mn.strip()] = reason.strip()
    return wl


def find_oracle_exe(explicit):
    path = explicit or DEFAULT_ORACLE_EXE
    if not path:
        raise SystemExit(
            "error: no oracle harness path given. STOP -- cannot fake this.\n"
            "Pass --oracle-exe PATH or set the SPU_DUMP_TOOL_EXE environment "
            "variable (see tools/spu_disasm_audit.py's find_oracle_exe for the "
            "build recipe)."
        )
    if not os.path.isfile(path):
        raise SystemExit(
            f"error: oracle harness not found at '{path}'. STOP -- cannot fake this."
        )
    return path


def extract_code_segment(elf_path: str):
    ef = ELFFile(elf_path)
    ef.load()
    code_segs = [
        ph for ph in ef.program_headers
        if ph.p_type == PT_LOAD and (ph.p_flags & PF_X) and ph.p_filesz > 0
    ]
    if not code_segs:
        raise SystemExit(f"error: no executable PT_LOAD segment found in '{elf_path}'")
    ph = code_segs[0]
    data = ef.raw_data[ph.p_offset:ph.p_offset + ph.p_filesz]
    return data, ph.p_vaddr


def decode_ours(code: bytes, vaddr: int):
    out = {}
    n = len(code) - (len(code) % 4)
    for off in range(0, n, 4):
        word = struct.unpack_from(">I", code, off)[0]
        insn = spu_decode(word, vaddr + off)
        out[vaddr + off] = (insn.mnemonic, insn.operands, word)
    return out


def decode_oracle(oracle_exe: str, code: bytes, vaddr: int):
    base = vaddr
    if base + len(code) > 0x40000:
        base = vaddr & 0x3FFFF
        if base + len(code) > 0x40000:
            raise SystemExit(
                f"error: code segment (vaddr=0x{vaddr:x} len=0x{len(code):x}) "
                "does not fit in a 256 KiB SPU local store even after LS-masking"
            )

    with tempfile.NamedTemporaryFile(suffix=".spu.bin", delete=False) as tf:
        tf.write(code)
        tmp_path = tf.name

    try:
        proc = subprocess.run(
            [oracle_exe, tmp_path, "--base", hex(base)],
            capture_output=True, text=True, timeout=120,
        )
    finally:
        os.unlink(tmp_path)

    if proc.returncode != 0:
        raise SystemExit(
            f"error: oracle harness exited {proc.returncode}\n"
            f"stdout: {proc.stdout[-2000:]}\nstderr: {proc.stderr[-2000:]}"
        )

    out = {}
    delta = vaddr - base
    for line in proc.stdout.splitlines():
        colon = line.find(":")
        if colon < 0:
            continue
        try:
            off = int(line[:colon], 16)
        except ValueError:
            continue
        rest = line[colon + 1:].strip()
        if not rest or rest.startswith("<"):
            continue
        sp = rest.split(None, 1)
        mnemonic = sp[0]
        operands = sp[1] if len(sp) > 1 else ""
        out[off + delta] = (mnemonic, operands)
    return out


def audit_one(image_path: str, oracle_exe: str, samples: int, whitelist: dict):
    code, vaddr = extract_code_segment(image_path)
    ours = decode_ours(code, vaddr)
    oracle = decode_oracle(oracle_exe, code, vaddr)

    mnemonic_matched = 0
    mismatch_counts = Counter()
    mismatch_samples = defaultdict(list)
    whitelisted_counts = Counter()

    for addr, (our_mn, our_ops, word) in ours.items():
        oracle_entry = oracle.get(addr)
        if oracle_entry is None:
            continue
        oracle_mn, oracle_ops = oracle_entry
        if our_mn != oracle_mn:
            continue  # v1's job
        mnemonic_matched += 1

        our_atoms = split_operands(our_mn, our_ops, ours=True)
        oracle_atoms = split_operands(oracle_mn, oracle_ops, ours=False)

        if our_atoms == oracle_atoms:
            continue

        key = our_mn
        entry = (addr, word, our_ops.strip(), oracle_ops.strip(),
                 our_atoms, oracle_atoms)
        if key in whitelist:
            whitelisted_counts[key] += 1
        else:
            mismatch_counts[key] += 1
            if len(mismatch_samples[key]) < samples:
                mismatch_samples[key].append(entry)

    return mnemonic_matched, mismatch_counts, mismatch_samples, whitelisted_counts, len(ours)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("images", nargs="+")
    ap.add_argument("--oracle-exe", default=None,
                     help="path to your built spu_dump_tool.exe "
                          "(default: $SPU_DUMP_TOOL_EXE if set)")
    ap.add_argument("--samples", type=int, default=5)
    ap.add_argument("--whitelist", default=DEFAULT_WHITELIST)
    args = ap.parse_args()

    oracle_exe = find_oracle_exe(args.oracle_exe)
    whitelist = load_whitelist(args.whitelist)
    print(f"[spu_operand_audit] oracle exe: {oracle_exe}")
    print(f"[spu_operand_audit] whitelist: {args.whitelist} ({len(whitelist)} entries)")
    print(f"[spu_operand_audit] images: {len(args.images)}")

    grand_words = 0
    grand_matched = 0
    combined_counts = Counter()
    combined_samples = defaultdict(list)
    combined_whitelisted = Counter()
    per_image = []

    for img in args.images:
        matched, counts, samples_d, wl_counts, total_words = audit_one(
            img, oracle_exe, args.samples, whitelist)
        grand_words += total_words
        grand_matched += matched
        per_image.append((img, total_words, matched, sum(counts.values())))
        combined_counts.update(counts)
        combined_whitelisted.update(wl_counts)
        for k, v in samples_d.items():
            for e in v:
                if len(combined_samples[k]) < args.samples:
                    combined_samples[k].append(e)

    print()
    print("=== Per-image summary ===")
    for img, total, matched, mism in per_image:
        print(f"  {os.path.basename(img)}: {total} words, {matched} mnemonic-matched, {mism} operand divergences")

    print()
    print(f"=== Totals across {len(args.images)} image(s): {grand_words} words decoded, "
          f"{grand_matched} mnemonic-matched (population for THIS audit) ===")
    total_residual = sum(combined_counts.values())
    total_whitelisted = sum(combined_whitelisted.values())
    print(f"=== Residual (non-whitelisted) operand divergences: {total_residual}  "
          f"(whitelisted: {total_whitelisted}) ===")

    print()
    print("--- Residual operand divergences, grouped by mnemonic, descending count (top 30) ---")
    for mn, count in sorted(combined_counts.items(), key=lambda kv: -kv[1])[:30]:
        print(f"  {mn}: {count} divergent word(s)")
        for (addr, word, our_ops, oracle_ops, our_atoms, oracle_atoms) in combined_samples[mn]:
            print(f"      0x{addr:08x}  word=0x{word:08x}  ours='{our_ops}' ({our_atoms})  "
                  f"oracle='{oracle_ops}' ({oracle_atoms})")
    if not combined_counts:
        print("  (none)")

    print()
    print("--- Whitelisted mnemonics, INFO only ---")
    for mn, count in sorted(combined_whitelisted.items(), key=lambda kv: -kv[1]):
        print(f"  {mn}: {count}  reason: {whitelist.get(mn, '')}")
    if not combined_whitelisted:
        print("  (none)")

    core_ops = {"il", "ai", "lqd", "ori", "shufb", "brsl"}
    core_residual = {mn: c for mn, c in combined_counts.items() if mn in core_ops}
    print()
    if core_residual:
        print(f"SANITY CHECK FAILED: core ops have residual OPERAND mismatches: {core_residual}")
    else:
        print("SANITY CHECK OK: core ops (il, ai, lqd, ori, shufb, brsl) show zero non-whitelisted operand divergences.")


if __name__ == "__main__":
    main()
