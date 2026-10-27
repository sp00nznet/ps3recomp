#!/usr/bin/env python3
"""PPU lift-structure census + relift regression diff.

Purpose: the structure bug class (map gaps, stubbed jump-table dispatchers,
fall-throughs, unresolved indirect targets -- boot blockers #6/#8/#14/#19b
were THIS class) has no static sweep. This tool runs three census passes
over the CURRENT relift output and the static function map, then diffs the
totals against a saved baseline so a NEW structure anomaly is flagged the
day a relift creates it.

NOTE (spec correction, verified on disk 2026-07-05): a single
`recomp/ppu_recomp.c` was assumed. That file does not exist -- the lifter
emits 20 chunk files `recomp/ppu_recomp_000.cpp .. ppu_recomp_019.cpp` (see
ppu_lifter.py's write_source_files / the stale-.c cleanup at its main(),
~line 3037-3048). This tool globs `recomp/ppu_recomp_*.cpp` instead. recomp/
is READ-ONLY here -- never edited.

Three census passes:

1. Marker census -- counts `/* TODO: <mn> ... */` emissions (grouped by
   mnemonic) and `/* unresolved target 0x... */` stub emissions, by reading
   the generated chunk files directly (this is what actually shipped, not
   what the lifter *would* emit -- catches drift between the two).
   Marker strings are emitted at exactly two call sites in ppu_lifter.py:
     - line 2176: `return f"/* TODO: {mn} {insn.operands} */;"`
       (the catch-all default for any mnemonic with no dedicated handler;
       `.word` -- ppu_disasm.py's own "unknown opcode" fallback, line 1077 --
       reaches this same site since the lifter has no special-case for it,
       so ".word" TODOs and "real" unhandled-instruction TODOs are
       distinguished here only by mnemonic grouping, matching the lifter's
       own word_todos/insn_todos split at ppu_lifter.py ~line 3050-3066).
     - line 2568: `f"/* unresolved target 0x{target:08X} */"` inside
       _stub_and_table_lines() (no-op stub bodies for call/branch targets
       that resolved to no lifted function and aren't in --extern-funcs).

2. Fall-through census -- absorbs scratch/fallthrough_sweep.py's logic
   in-place (same terminator test, same map-gap definition) rather than
   shelling out, so this tool has zero external-script dependencies.

3. Target coverage -- decodes every executable word of game/EBOOT.elf via
   the SAME PT_LOAD/PF_X segment walk ppu_lifter.py's main() uses (reusing
   tools/elf_parser.py + tools/ppu_disasm.py, never re-parsing the ELF by
   hand), collects all unconditional-transfer targets (b/bl/ba/bla -- PPC
   opcode 18, see ppu_disasm.py decode() ~line 151), and buckets each target
   as:
     - COVERED    -- falls inside some functions.json [start, end) entry
     - GAP        -- outside every entry (a "gap target")
     - MID-FUNC   -- inside an entry but not equal to its start (a split
                     candidate: the lifter's mid-function-entry discovery
                     pass, ppu_lifter.py ~line 3011-3029, is expected to
                     absorb most of these -- this pass reports them as an
                     independent cross-check of that mechanism, not a
                     duplicate of it).

CLI:
    py -3 tools\\lift_audit.py [--elf game\\EBOOT.elf]
                                [--functions functions.json]
                                [--recomp-dir recomp]
                                [--baseline scratch\\lift_audit_baseline.txt]
                                [--update-baseline]

--elf/--functions/--recomp-dir default to the repo-root game/EBOOT.elf,
functions.json, and recomp/ paths respectively, but are overridable so this
tool works against any relift target, not just this repo's checked-in game.

Default mode prints the full census, then diffs summary counts against the
baseline file. A NEW anomaly (a count that INCREASED, or a newly-appeared
group key with nonzero count) vs baseline is a FAIL (nonzero exit). Counts
that only decreased, or are unchanged, are fine. --update-baseline writes the
current census as the new baseline and exits 0 without diffing.

Evidence discipline: this is a pure static census over files that exist on
disk at run time (recomp/*.cpp, functions.json, game/EBOOT.elf) -- every
number printed is MEASURED from those files in this run, not inferred.
"""
import argparse
import glob
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))

sys.path.insert(0, HERE)
try:
    from ppu_disasm import decode, disassemble_bytes  # noqa: E402
    from elf_parser import ELFFile, PT_LOAD  # noqa: E402
except ImportError as exc:
    print(f"Error: could not import ppu_disasm/elf_parser from {HERE}: {exc}",
          file=sys.stderr)
    sys.exit(1)

DEFAULT_BASELINE = os.path.join(ROOT, "scratch", "lift_audit_baseline.txt")

TODO_RE = re.compile(r"/\* TODO: (\S+)")
UNRESOLVED_RE = re.compile(r"/\* unresolved target 0x([0-9A-Fa-f]+) \*/")


# ---------------------------------------------------------------------------
# Pass 1: marker census (reads the generated chunk .cpp files as-shipped)
# ---------------------------------------------------------------------------

def marker_census(recomp_dir: str):
    """Scan <recomp_dir>/ppu_recomp_*.cpp for the two marker emit sites.

    Grouping: /* TODO: <mn> ... */ by mnemonic (mn is the first whitespace
    token after "TODO: ", matching ppu_lifter.py line 2176's f-string
    exactly: "{mn} {insn.operands}"). /* unresolved target 0x... */ has no
    sub-grouping in the source (ppu_lifter.py line 2568 emits one per
    address) -- reported as a single reason bucket "unresolved-call-or-
    branch-target" with the total count and sample addresses.
    """
    chunk_paths = sorted(glob.glob(os.path.join(recomp_dir, "ppu_recomp_*.cpp")))
    if not chunk_paths:
        print(f"ERROR: no {recomp_dir}/ppu_recomp_*.cpp chunk files found -- "
              "has the project ever been relifted? (the recomp output dir is "
              "generated, never hand-edited; run tools/ppu_lifter.py first)",
              file=sys.stderr)
        sys.exit(2)

    todo_by_mnemonic: dict[str, int] = {}
    unresolved_addrs: list[int] = []

    for path in chunk_paths:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if "/* TODO:" in line:
                    m = TODO_RE.search(line)
                    if m:
                        mn = m.group(1)
                        todo_by_mnemonic[mn] = todo_by_mnemonic.get(mn, 0) + 1
                elif "/* unresolved target" in line:
                    m = UNRESOLVED_RE.search(line)
                    if m:
                        unresolved_addrs.append(int(m.group(1), 16))

    return chunk_paths, todo_by_mnemonic, unresolved_addrs


# ---------------------------------------------------------------------------
# Pass 2: fall-through census (absorbs scratch/fallthrough_sweep.py in-place)
# ---------------------------------------------------------------------------

def _load_functions_json(functions_path: str):
    import json
    with open(functions_path) as f:
        funcs = json.load(f)
    return sorted((int(str(e["start"]), 0), int(str(e["end"]), 0)) for e in funcs)


def _is_terminator_word(w: int) -> bool:
    """Same terminator test as scratch/fallthrough_sweep.py's is_terminator():
    unconditional b/ba (opcode 18, LK=0) or unconditional blr/bctr (opcode 19,
    XO 16/528, LK=0, BO unconditional bits bo&0x14==0x14). sc (opcode 17)
    returns control to the caller via the syscall path, so it is NOT treated
    as a terminator (matches the sweep script: falls through)."""
    op = w >> 26
    if op == 18 and not (w & 1):
        return True
    if op == 19:
        xo = (w >> 1) & 0x3FF
        bo = (w >> 21) & 0x1F
        if xo in (16, 528) and not (w & 1) and (bo & 0x14) == 0x14:
            return True
    return False


def fallthrough_census(elf_data: bytes, functions_path: str):
    entries = _load_functions_json(functions_path)

    def word(addr):
        off = addr - 0x10000
        return struct.unpack(">I", elf_data[off:off + 4])[0]

    hazards = []
    gap_followed = 0
    for i, (start, end) in enumerate(entries):
        nxt = entries[i + 1][0] if i + 1 < len(entries) else None
        if nxt is None or end == nxt:
            continue
        gap_followed += 1
        last = word(end - 4)
        if _is_terminator_word(last):
            continue
        hazards.append((start, end, nxt, last))

    return entries, gap_followed, hazards


# ---------------------------------------------------------------------------
# Pass 3: target coverage (decode all executable words, bucket b/bl targets)
# ---------------------------------------------------------------------------

def _executable_instructions(elf_path: str):
    """Same PT_LOAD/PF_X segment walk as ppu_lifter.py main() (~line 2840-
    2857): decode every executable segment's words via disassemble_bytes.
    Falls back to the first non-empty PT_LOAD segment if none are marked
    executable (mirrors the lifter's own fallback), matching its behavior
    exactly rather than reimplementing a heuristic."""
    elf = ELFFile(elf_path)
    elf.load()
    big_endian = elf.elf_header.big_endian
    all_insns = []
    for idx, ph in enumerate(elf.program_headers):
        if ph.p_type == PT_LOAD and (ph.p_flags & 1):
            seg_data = elf.get_segment_data(idx)
            all_insns.extend(disassemble_bytes(seg_data, ph.p_vaddr, big_endian))
    if not all_insns:
        for idx, ph in enumerate(elf.program_headers):
            if ph.p_type == PT_LOAD and ph.p_filesz > 0:
                seg_data = elf.get_segment_data(idx)
                all_insns.extend(disassemble_bytes(seg_data, ph.p_vaddr, big_endian))
                break
    return all_insns


def target_coverage_census(elf_path: str, entries: list[tuple[int, int]]):
    """Decode all executable words; collect unconditional-transfer (b/bl/
    ba/bla) targets; bucket vs functions.json coverage.

    Unconditional transfer = PPC opcode 18 forms (mnemonic starts with 'b',
    produced only by the opcd==18 branch in ppu_disasm.py decode() ~line
    151: mnemonics "b"/"bl"/"ba"/"bla"). Conditional branches (opcode 16) and
    CTR/LR-indirect forms are excluded -- their targets are either runtime-
    resolved (not statically knowable) or already covered as conditional
    in-function gotos by the lifter.
    """
    starts = sorted(s for s, _ in entries)
    import bisect

    def bucket(target: int) -> str:
        i = bisect.bisect_right(starts, target) - 1
        if i < 0:
            return "GAP"
        s, e = entries_by_start[starts[i]]
        if s <= target < e:
            return "COVERED" if target == s else "MID-FUNC"
        return "GAP"

    entries_by_start = {}
    for s, e in entries:
        # keep the widest/last end if duplicate starts exist (shouldn't, but
        # be defensive rather than silently dropping data)
        if s not in entries_by_start or e > entries_by_start[s][1]:
            entries_by_start[s] = (s, e)
    starts = sorted(entries_by_start.keys())

    insns = _executable_instructions(elf_path)
    total_words = len(insns)

    targets: set[int] = set()
    for insn in insns:
        mn = insn.mnemonic
        if mn in ("b", "bl", "ba", "bla"):
            try:
                targets.add(int(insn.operands, 16) & 0xFFFFFFFF)
            except ValueError:
                continue

    gap_targets = []
    midfunc_targets = []
    for t in sorted(targets):
        b = bucket(t)
        if b == "GAP":
            gap_targets.append(t)
        elif b == "MID-FUNC":
            midfunc_targets.append(t)

    return total_words, len(targets), gap_targets, midfunc_targets


# ---------------------------------------------------------------------------
# Report formatting + baseline diff
# ---------------------------------------------------------------------------

def build_report(chunk_paths, todo_by_mnemonic, unresolved_addrs,
                  fn_entries, gap_followed, ft_hazards,
                  total_words, total_targets, gap_targets, midfunc_targets):
    lines = []
    lines.append("=== Pass 1: marker census (recomp/ppu_recomp_*.cpp, %d chunks) ==="
                  % len(chunk_paths))
    total_todo = sum(todo_by_mnemonic.values())
    lines.append(f"TOTAL /* TODO: ... */ markers: {total_todo}")
    word_todos = todo_by_mnemonic.get(".word", 0)
    insn_todos = total_todo - word_todos
    lines.append(f"  .word (data-in-text, KNOWN/harmless): {word_todos}")
    lines.append(f"  other unhandled-instruction TODOs:    {insn_todos}")
    lines.append("  by mnemonic (desc count):")
    for mn, cnt in sorted(todo_by_mnemonic.items(), key=lambda kv: (-kv[1], kv[0])):
        lines.append(f"    {mn:<12s} {cnt}")
    lines.append(f"TOTAL /* unresolved target 0x... */ markers: {len(unresolved_addrs)}")
    if unresolved_addrs:
        sample = ", ".join(f"0x{a:08X}" for a in sorted(unresolved_addrs)[:10])
        lines.append(f"  sample addresses: {sample}"
                      + (" ..." if len(unresolved_addrs) > 10 else ""))
    lines.append("")

    lines.append("=== Pass 2: fall-through census (functions.json vs game/EBOOT.elf) ===")
    lines.append(f"map entries: {len(fn_entries)}")
    lines.append(f"entries followed by a gap: {gap_followed}")
    lines.append(f"FALL-THROUGH-INTO-GAP hazards: {len(ft_hazards)}")
    limit = 40
    for s, e, n, w in ft_hazards[:limit]:
        lines.append(f"  func {s:#010x}  end {e:#010x}  next-mapped {n:#010x}  "
                      f"gap {n - e:#x}  last-insn {w:08X}")
    if len(ft_hazards) > limit:
        lines.append(f"  ... {len(ft_hazards) - limit} more")
    lines.append("")

    lines.append("=== Pass 3: target coverage (b/bl/ba/bla targets vs functions.json) ===")
    lines.append(f"executable words decoded: {total_words}")
    lines.append(f"unique unconditional-transfer targets: {total_targets}")
    lines.append(f"GAP targets (outside any functions.json entry): {len(gap_targets)}")
    for t in gap_targets[:40]:
        lines.append(f"    0x{t:08X}")
    if len(gap_targets) > 40:
        lines.append(f"    ... {len(gap_targets) - 40} more")
    lines.append(f"MID-FUNC targets (split candidates, inside an entry but != start): "
                  f"{len(midfunc_targets)}")
    for t in midfunc_targets[:40]:
        lines.append(f"    0x{t:08X}")
    if len(midfunc_targets) > 40:
        lines.append(f"    ... {len(midfunc_targets) - 40} more")
    lines.append("")

    return "\n".join(lines)


def build_baseline_dict(todo_by_mnemonic, unresolved_addrs, ft_hazards,
                         gap_targets, midfunc_targets):
    """Flat key->count dict used for baseline persistence + diffing. Only
    SUMMARY counts are diffed (not full address lists) -- new counts higher
    than baseline are FAIL; the full lists are for human triage in the
    printed report, not baseline-tracked line by line (address sets shift
    harmlessly with unrelated relifts; count regressions are the signal)."""
    d = {}
    for mn, cnt in todo_by_mnemonic.items():
        d[f"todo_mnemonic:{mn}"] = cnt
    d["unresolved_targets_total"] = len(unresolved_addrs)
    d["fallthrough_hazards_total"] = len(ft_hazards)
    d["gap_targets_total"] = len(gap_targets)
    d["midfunc_targets_total"] = len(midfunc_targets)
    return d


def write_baseline(path, d):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# lift_audit.py baseline -- generated file, do not hand-edit.\n")
        f.write("# key<TAB>count, one per line, sorted by key.\n")
        for k in sorted(d.keys()):
            f.write(f"{k}\t{d[k]}\n")


def read_baseline(path):
    if not os.path.exists(path):
        return None
    d = {}
    # utf-8-sig: tolerate a BOM if the file was ever touched by a BOM-adding
    # editor/tool (this tool's own writer never adds one, but baseline files
    # are plain text and easy to corrupt by hand on Windows).
    with open(path, "r", encoding="utf-8-sig") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if not line or line.startswith("#"):
                continue
            k, _, v = line.partition("\t")
            v = v.strip()
            if not v:
                continue
            d[k] = int(v)
    return d


def diff_against_baseline(current: dict, baseline: dict) -> tuple[list[str], bool]:
    """Returns (report_lines, has_new_anomaly). A NEW anomaly = a key whose
    count increased vs baseline, or a key with nonzero count that is absent
    from baseline entirely. Decreases and unchanged counts are fine."""
    lines = []
    fail = False
    all_keys = sorted(set(current) | set(baseline))
    any_diff = False
    for k in all_keys:
        cur = current.get(k, 0)
        base = baseline.get(k, 0)
        if cur == base:
            continue
        any_diff = True
        if k not in baseline and cur > 0:
            lines.append(f"  NEW key {k}: {cur} (not in baseline) -- FAIL")
            fail = True
        elif cur > base:
            lines.append(f"  INCREASE {k}: {base} -> {cur} (+{cur - base}) -- FAIL")
            fail = True
        else:
            lines.append(f"  decrease {k}: {base} -> {cur} ({cur - base}) -- ok")
    if not any_diff:
        lines.append("  (no diff vs baseline)")
    return lines, fail


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--elf", default=os.path.join(ROOT, "game", "EBOOT.elf"),
                     help="target ELF to census (default: repo-root game/EBOOT.elf)")
    ap.add_argument("--functions", default=os.path.join(ROOT, "functions.json"),
                     help="function boundary table (default: repo-root functions.json)")
    ap.add_argument("--recomp-dir", default=os.path.join(ROOT, "recomp"),
                     help="generated recomp output dir to scan (default: repo-root recomp/)")
    ap.add_argument("--baseline", default=DEFAULT_BASELINE,
                     help=f"baseline file path (default: {DEFAULT_BASELINE})")
    ap.add_argument("--update-baseline", action="store_true",
                     help="write current census as the new baseline and exit "
                          "(no diff performed)")
    args = ap.parse_args()

    elf_path = args.elf
    if not os.path.exists(elf_path):
        print(f"ERROR: {elf_path} not found", file=sys.stderr)
        sys.exit(2)
    with open(elf_path, "rb") as f:
        elf_data = f.read()

    chunk_paths, todo_by_mnemonic, unresolved_addrs = marker_census(args.recomp_dir)
    fn_entries, gap_followed, ft_hazards = fallthrough_census(elf_data, args.functions)
    total_words, total_targets, gap_targets, midfunc_targets = \
        target_coverage_census(elf_path, fn_entries)

    report = build_report(chunk_paths, todo_by_mnemonic, unresolved_addrs,
                           fn_entries, gap_followed, ft_hazards,
                           total_words, total_targets, gap_targets, midfunc_targets)
    print(report)

    current = build_baseline_dict(todo_by_mnemonic, unresolved_addrs, ft_hazards,
                                   gap_targets, midfunc_targets)

    if args.update_baseline:
        write_baseline(args.baseline, current)
        print(f"=== Baseline written: {args.baseline} ({len(current)} keys) ===")
        sys.exit(0)

    baseline = read_baseline(args.baseline)
    if baseline is None:
        print(f"=== No baseline at {args.baseline} -- run with --update-baseline "
              f"to seed one. Skipping diff. ===")
        sys.exit(0)

    print(f"=== Diff vs baseline ({args.baseline}) ===")
    diff_lines, fail = diff_against_baseline(current, baseline)
    for ln in diff_lines:
        print(ln)

    if fail:
        print("=== FAIL: new structure anomaly vs baseline ===")
        sys.exit(1)
    print("=== PASS: no new anomaly vs baseline ===")
    sys.exit(0)


if __name__ == "__main__":
    main()
