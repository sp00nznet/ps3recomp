#!/usr/bin/env python3
"""
SPU decode cross-check: tools/spu_disasm.py vs RPCS3's SPUDisAsm used as an
OUTPUT-ONLY oracle.

Clean-room posture: RPCS3 is GPLv2. The oracle is a tiny standalone dump
harness (see tools/spu_dump_tool.vcxproj, a thin main.cpp that links RPCS3's
own SPUDisAsm class from a local RPCS3 source checkout) that you build
against your own RPCS3 clone. No RPCS3 source is copied into this repo; this
script only invokes that already-built .exe and diffs its plain-text stdout
against our own decoder's output. See that harness's main.cpp for the build
rationale and its vcxproj for the link recipe.

Usage:
    py -3 tools\\spu_disasm_audit.py <image1.elf> [image2.elf ...]
        --oracle-exe PATH [--samples N]

--oracle-exe defaults to the SPU_DUMP_TOOL_EXE environment variable if set;
otherwise it is required.

Each <imageN.elf> is one of the lifted SPU images in scratch\\spu_imgs\\ (an
ELF-wrapped SPU binary produced by the lift pipeline). For each image:
  1. Parse the ELF program headers (tools/elf_parser.py), find the PT_LOAD
     segment with the executable flag (PF_X, bit 0x1) -- that is the SPU code
     segment; other PT_LOAD segments are data (e.g. the 0x6 = R+W segment)
     and are NOT code, so they are skipped.
  2. Decode every 4-byte-aligned word of that segment with our
     tools/spu_disasm.py (spu_decode) -> "ours" mnemonic per offset.
  3. Write the SAME raw bytes to a temp file and invoke the oracle harness
     (spu_dump_tool.exe <bytes> --base <vaddr>) -> "oracle" mnemonic per
     offset, parsed back out of its stdout.
  4. Normalize both to mnemonic-only (v1 scope; operand normalization is a
     stretch goal, matching disasm_audit.py's precedent) via a whitelist
     alias table (known made-up RPCS3 mnemonics: "mr" for ori/shlqbyi with a
     zero immediate -- verified by reading SPUDisAsm.h's SHLQBYI/ORI bodies,
     see the ALIAS_WHITELIST comment below -- plus the C-bit-conditional
     sync/syncc and hbr/hbrp pairs, which our decoder does not model).
  5. Diff per-offset; group residual (non-whitelisted) mismatches by
     (ours_mnemonic, oracle_mnemonic) pair, descending count, with sample
     addresses; report whitelisted pairs separately as INFO.

Exit code: 0 always (this is a report tool, like disasm_audit.py); the
CALLER (an acceptance run / a human) judges the residual count.
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from elf_parser import ELFFile, PT_LOAD  # noqa: E402
from spu_disasm import spu_decode  # noqa: E402

# ---------------------------------------------------------------------------
# Oracle harness location: never shipped in this repo (built against your own
# RPCS3 clone -- see tools/spu_dump_tool.vcxproj). Point --oracle-exe at your
# build, or set the SPU_DUMP_TOOL_EXE environment variable.
# ---------------------------------------------------------------------------
DEFAULT_ORACLE_EXE = os.environ.get("SPU_DUMP_TOOL_EXE")

PF_X = 0x1  # ELF program header "executable" flag bit

# ---------------------------------------------------------------------------
# Alias whitelist: (ours_mnemonic, oracle_mnemonic) pairs known to be the
# SAME instruction under a different display name, confirmed by reading the
# RPCS3 source (GPLv2, cited by file:line only -- no code copied):
#   - SPUDisAsm.h ORI():      "if (!op.si10) DisAsm(\"mr\", ...)"       (~line 897)
#   - SPUDisAsm.h SHLQBYI():  "if (!op.si7)  DisAsm(\"mr\", ...)"       (~line 553)
#     Both are RPCS3's own "made-up mnemonic: as MR on PPU" comment --
#     value-conditional (immediate == 0) aliasing our decoder does not do.
#     v1 whitelists the mnemonic PAIR unconditionally (does not re-check the
#     immediate); this is a deliberate v1 simplification, matching the
#     "mnemonic-only in v1" scope used elsewhere in this audit family.
#   - SPUDisAsm.h SYNC():     "DisAsm(op.c ? \"syncc\" : \"sync\")"      (~line 240)
#   - SPUDisAsm.h HBR():      "DisAsm(op.c ? \"hbrp\" : \"hbr\", ...)"   (~line 419)
#     Our decoder (tools/spu_disasm.py) always emits the C=0 name; these
#     pairs are a REAL, reportable gap (not a bug -- SYNC/HBR are 1-bit
#     variants we don't currently distinguish), whitelisted here so they
#     don't drown out true decode-table-swap mismatches in the top-N report.
# ---------------------------------------------------------------------------
ALIAS_WHITELIST = {
    ("ori", "mr"),
    ("shlqbyi", "mr"),
    ("sync", "syncc"),
    ("hbr", "hbrp"),
}


def find_oracle_exe(explicit: str | None) -> str:
    path = explicit or DEFAULT_ORACLE_EXE
    if not path:
        raise SystemExit(
            "error: no oracle harness path given.\n"
            "Pass --oracle-exe PATH or set the SPU_DUMP_TOOL_EXE environment "
            "variable. Build the harness first: the project is "
            "tools/spu_dump_tool.vcxproj, linked against your own local RPCS3 "
            "source checkout; build with msbuild against that .vcxproj "
            "(/p:SolutionDir=<path-to-your-rpcs3-checkout>\\)."
        )
    if not os.path.isfile(path):
        raise SystemExit(
            f"error: oracle harness not found at '{path}'.\n"
            "Build it first: the harness project is "
            "tools/spu_dump_tool.vcxproj, linked against your own local RPCS3 "
            "source checkout; build with msbuild against that .vcxproj "
            "(/p:SolutionDir=<path-to-your-rpcs3-checkout>\\)."
        )
    return path


def extract_code_segment(elf_path: str) -> tuple[bytes, int]:
    """Return (code_bytes, vaddr) for the executable PT_LOAD segment."""
    ef = ELFFile(elf_path)
    ef.load()

    code_segs = [
        ph for ph in ef.program_headers
        if ph.p_type == PT_LOAD and (ph.p_flags & PF_X) and ph.p_filesz > 0
    ]
    if not code_segs:
        raise SystemExit(f"error: no executable PT_LOAD segment found in '{elf_path}'")
    if len(code_segs) > 1:
        print(
            f"warning: '{elf_path}' has {len(code_segs)} executable PT_LOAD "
            "segments; using the first",
            file=sys.stderr,
        )

    ph = code_segs[0]
    data = ef.raw_data[ph.p_offset:ph.p_offset + ph.p_filesz]
    return data, ph.p_vaddr


def decode_ours(code: bytes, vaddr: int) -> dict[int, str]:
    """offset -> mnemonic, decoding every 4-byte-aligned word."""
    out = {}
    n = len(code) - (len(code) % 4)
    for off in range(0, n, 4):
        word = struct.unpack_from(">I", code, off)[0]
        insn = spu_decode(word, vaddr + off)
        out[vaddr + off] = insn.mnemonic
    return out


def decode_oracle(oracle_exe: str, code: bytes, vaddr: int) -> dict[int, str]:
    """offset -> mnemonic, by shelling out to the rpcs3clone harness."""
    # SPU local store is 256 KiB (SPU_LS_SIZE); the harness requires
    # base + len(code) <= 0x40000. Our lifted images' vaddrs (e.g. gs_task's
    # 0x3000) already satisfy this -- if a future image doesn't, mask to the
    # low 18 bits (LS-relative) the way real SPU addressing does.
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
    delta = vaddr - base  # map harness-local offsets back to the real vaddr
    for line in proc.stdout.splitlines():
        # "00003000: ila r2,0x15ba4" (or "<disasm-out-of-range>")
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
        mnemonic = rest.split(None, 1)[0]
        out[off + delta] = mnemonic
    return out


def audit_one(image_path: str, oracle_exe: str, samples: int) -> tuple[Counter, dict, int]:
    """Returns (mismatch_pair_counts, sample_addrs_by_pair, total_words)."""
    code, vaddr = extract_code_segment(image_path)
    ours = decode_ours(code, vaddr)
    oracle = decode_oracle(oracle_exe, code, vaddr)

    pair_counts: Counter = Counter()
    pair_samples: dict[tuple[str, str], list[int]] = defaultdict(list)

    total = 0
    for addr, our_mn in ours.items():
        total += 1
        oracle_mn = oracle.get(addr)
        if oracle_mn is None:
            continue  # oracle didn't cover this offset (shouldn't happen for aligned code)
        if our_mn == oracle_mn:
            continue
        pair = (our_mn, oracle_mn)
        pair_counts[pair] += 1
        if len(pair_samples[pair]) < samples:
            pair_samples[pair].append(addr)

    return pair_counts, pair_samples, total


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("images", nargs="+", help="lifted SPU image ELF(s), e.g. scratch\\spu_imgs\\spu_0003_at_0126A580.elf")
    ap.add_argument("--oracle-exe", default=None,
                     help="path to your built spu_dump_tool.exe "
                          "(default: $SPU_DUMP_TOOL_EXE if set)")
    ap.add_argument("--samples", type=int, default=3, help="sample addresses per mismatch pair (default 3)")
    args = ap.parse_args()

    oracle_exe = find_oracle_exe(args.oracle_exe)
    print(f"[spu_disasm_audit] oracle exe: {oracle_exe}")
    print(f"[spu_disasm_audit] images: {len(args.images)}")

    combined_counts: Counter = Counter()
    combined_samples: dict[tuple[str, str], list[int]] = defaultdict(list)
    grand_total = 0
    per_image_totals = []

    for img in args.images:
        pair_counts, pair_samples, total = audit_one(img, oracle_exe, args.samples)
        grand_total += total
        per_image_totals.append((img, total, sum(pair_counts.values())))
        combined_counts.update(pair_counts)
        for pair, addrs in pair_samples.items():
            for a in addrs:
                if len(combined_samples[pair]) < args.samples:
                    combined_samples[pair].append(a)

    print()
    print("=== Per-image summary ===")
    for img, total, mism in per_image_totals:
        print(f"  {os.path.basename(img)}: {total} words decoded, {mism} mismatches ({100.0 * mism / total if total else 0:.3f}%)")

    whitelisted_pairs = {p: c for p, c in combined_counts.items() if p in ALIAS_WHITELIST}
    residual_pairs = {p: c for p, c in combined_counts.items() if p not in ALIAS_WHITELIST}

    print()
    print(f"=== Total words decoded across {len(args.images)} image(s): {grand_total} ===")
    print(f"=== Total mismatch pairs (distinct): {len(combined_counts)}  (whitelisted: {len(whitelisted_pairs)}, residual: {len(residual_pairs)}) ===")

    print()
    print("--- Residual (non-whitelisted) mismatches, grouped by (ours, oracle), descending count ---")
    for pair, count in sorted(residual_pairs.items(), key=lambda kv: -kv[1]):
        samples_str = ", ".join(f"0x{a:x}" for a in combined_samples[pair])
        print(f"  ({pair[0]!r}, {pair[1]!r}): {count}  samples: {samples_str}")
    if not residual_pairs:
        print("  (none)")

    print()
    print("--- Whitelisted (known-alias) mismatches, INFO only ---")
    for pair, count in sorted(whitelisted_pairs.items(), key=lambda kv: -kv[1]):
        samples_str = ", ".join(f"0x{a:x}" for a in combined_samples[pair])
        print(f"  ({pair[0]!r}, {pair[1]!r}): {count}  samples: {samples_str}")
    if not whitelisted_pairs:
        print("  (none)")

    # Sanity gate: core ops must show zero NON-whitelisted mismatches.
    core_ops = {"il", "ai", "lqd", "brsl", "shufb"}
    core_residual = {p: c for p, c in residual_pairs.items() if p[0] in core_ops}
    print()
    if core_residual:
        print(f"SANITY CHECK FAILED: core ops have residual mismatches: {core_residual}")
    else:
        print("SANITY CHECK OK: core ops (il, ai, lqd, brsl, shufb) show zero non-whitelisted mismatches.")


if __name__ == "__main__":
    main()
