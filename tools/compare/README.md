# Cross-platform comparison harness (`tools/compare/`)

Compare the **Xbox 360** build and the **PS3** build of the *same game*,
function-by-function, to:

1. **Validate** ps3recomp's PS3 lift against a second, independent PowerPC
   implementation of the same source code.
2. **Mine** the 360 side (recompiled with `rexglue` / `XenonRecomp`) for ISA
   handling and codegen idioms worth porting into ps3recomp.

Why this works: both consoles run **64-bit big-endian PowerPC** (Xenon vs Cell
PPU), and cross-platform games are built from a largely **shared C++ codebase**.
String literals and large constants survive compilation almost identically, so
they make excellent platform-independent anchors for matching functions across
the two binaries.

```
   default.xex (360)                         EBOOT.ELF / .SELF (PS3)
        │                                          │
   Ghidra XEX loader / IDA                  ppu_disasm + find_functions
   + ExportCompareUnits.java                (ingest_native.py)   OR Ghidra
   or ida_export_units.py                          │
        │                                          │
        ▼                                          ▼
   x360.units.json  ◄──── unified schema ────►  ps3.units.json
                         (schema.py)
                              │
                     match_functions.py   (string/const/name anchors
                              │            + call-graph propagation)
                              ▼
                     compare_report.py  ──►  report.md + findings.json
```

## Files

| File | Role |
|------|------|
| `schema.py` | Unified `Unit`/`Module` record + JSON I/O. The contract every tool agrees on. |
| `ghidra/ExportCompareUnits.java` | Ghidra headless post-script → `units.json`. Works for **both** Xenon and Cell (same processor module). |
| `ida_export_units.py` | IDAPython script → `units.json`. IDA/Ghidra are interchangeable. |
| `ingest_native.py` | Build a PS3 `units.json` from ps3recomp's own `ppu_disasm` + `find_functions` JSON (no Ghidra needed). |
| `xex_parser.py` | XEX2 metadata (title id, entry, image base, imports, packed/encrypted check). 360-side analogue of `elf_parser.py`. |
| `match_functions.py` | The matcher. Library (`match`) + CLI. |
| `compare_report.py` | `findings.json` + human-readable `report.md`. |
| `run_compare.py` | One-command driver (units.json × 2 → reports). |
| `selftest.py` | Pure-Python smoke test (no external tools/games). |

## The unified `Unit` schema

One record per discovered function:

```jsonc
{
  "addr": "0x82010000",     // entry (effective address)
  "size": 256,               // bytes
  "name": "World::update",   // symbol if known, else null
  "insn_count": 64,
  "is_leaf": false,          // makes no calls
  "stack_size": 144,
  "calls":       ["0x82010100", ...],   // direct call targets
  "imports":     ["xboxkrnl.exe:ord.41", ...],
  "string_refs": ["Hedgehog::World::update tick", ...],  // KEY anchor
  "const_refs":  ["0x00abf000", ...],
  "mnemonic_hist": {"lwz": 8, "std": 4, "bl": 2, ...}
}
```

## How matching works (`match_functions.py`)

1. **Name anchors** — identical (normalized) symbols, when present.
2. **String anchors** — a string referenced by exactly one function on *each*
   side is a near-certain pair; longer/rarer strings score higher.
3. **Const anchors** — same idea for distinctive immediates / data addresses.
4. **Call-graph propagation** — from an anchored pair, pair unmatched callees
   (gated by structural similarity), iterating to a fixpoint. This reaches
   functions that have no strings of their own.
5. **Structural similarity** — mnemonic-histogram cosine + size/insn ratios +
   leaf/call-count agreement. Confirms pairs and surfaces **diverging pairs**
   (confidently the same function, but very different code → mining targets).

The report also computes **mnemonic-corpus deltas** — opcodes frequent on one
side but absent/rare on the other. This is where Xenon **VMX128** and other
360-only idioms show up, telling you what the 360 lifter handled that ps3recomp
may need to.

## End-to-end usage

```bash
# 0. (360) identify the dump / check whether the basefile is packed
python xex_parser.py default.xex

# 1a. (360) export units via Ghidra (its XEX loader unpacks the basefile)
analyzeHeadless proj P -import default.xex \
    -postScript ExportCompareUnits.java x360 ppc64-xenon x360.units.json
#    ...or via IDA:
ida64 -A -S"ida_export_units.py x360 ppc64-xenon x360.units.json" default.xex

# 1b. (PS3) export units via ps3recomp's own tools
python ../ppu_disasm.py EBOOT.ELF --json > instructions.json
python ../find_functions.py EBOOT.ELF --json > functions.json
python ingest_native.py --functions functions.json \
    --instructions instructions.json --binary EBOOT.ELF -o ps3.units.json
#    ...or via Ghidra (same script, ps3 args):
analyzeHeadless proj P -import EBOOT.ELF -processor PowerPC:BE:64:64-32addr \
    -postScript ExportCompareUnits.java ps3 ppc64-cell ps3.units.json

# 2. compare  (A = PS3, B = X360)
python run_compare.py ps3.units.json x360.units.json --outdir out/
#    -> out/report.md, out/findings.json, out/matches.json
```

## Notes / limitations

- `xex_parser.py` reads **metadata only**; it does not decrypt or LZX-decompress
  the basefile. For disassembly, let Ghidra/IDA (or `xextool -d`) unpack it.
- Xenon uses **VMX128** (different vector encoding from PS3's standard
  VMX/AltiVec). Ghidra/IDA decode it; our schema just records the mnemonics, and
  the report flags them as 360-only so they're easy to spot.
- Best results come when at least one front-end provides **strings**. With the
  pure-`ingest_native` path, pass a Ghidra `strings.json` via `--strings` to fill
  in `string_refs` (the strongest anchor).
- Validate the core anytime with `python selftest.py`.
```
