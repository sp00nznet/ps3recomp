# ps3recomp Harness

Batch-runs the ps3recomp analysis pipeline across a whole title library to turn
one-off observations into a **compatibility matrix** and a **catalog of recurring
failure modes** — the data that actually hardens the toolkit (and tells us which
NID stubs to ship next). It's the PS3 sibling of the 360 side's
`recomp_harness.py`.

PS3 content reaches us at two very different stages, so the harness has two
front doors:

| Command | Input | What it does |
|---|---|---|
| `catalog` | `--archive-root` (a directory of packaged titles) | Catalogs titles cheaply from their filenames, which encode title-id and region. Packaged titles are NPDRM-encrypted and frequently multi-GB, so nothing is decrypted wholesale at this tier. With `--probe N` it additionally extracts the nested PKG of the *N smallest* titles far enough to read the **plaintext** PKG header (content-id, item count, size). |
| `analyze` | `--elf-root` (dirs of decrypted binaries) | The real recomp triage over already-decrypted `EBOOT.elf` / `*.elf` / `*.self`. `*.self` inputs are decrypted to ELF via `ps3sce` first. |

## Tiers (cost grows steeply; triage is cheap and scales)

| Tier | Stage | Cost | Notes |
|---|---|---|---|
| 1 | catalog | instant | filename → title-id/region/class; optional PKG-header probe |
| 2 | decrypt | seconds | `*.self` → `*.elf` via `ps3sce` (only for encrypted inputs) |
| 3 | profile | seconds | `elf_parser.py` → machine, entry, **image base**, segments, memsz |
| 4 | functions | seconds | `find_functions.py` → function count, `.opd`-seeded starts, verify |
| 5 | lift | minutes–GBs | `ppu_lifter.py -j N` → generated C chunks, giant-function warnings (**opt-in**) |
| 6 | crosscheck | ~10–60 s/binary | `--oracle ghidra\|ida\|both`: runs Ghidra (`ghidra_analyze.py`) and/or IDA (`ida_analyze.py`) headless and compares `find_functions` to their function set. Reports **recall** (functions the oracle finds that we miss) + `extra`; with `both`, a **high-confidence** recall vs the set Ghidra and IDA agree on. The gap is recoverable via `find_functions --seed-json`. (**opt-in**, needs Ghidra at `c:\tools\ghidra` / IDA via Python 3.11 idalib) |

Each title is isolated, timed and resumable; one title's failure never aborts the
batch. Per-title results land in `<out>/results/*.json`; `report` aggregates them
into `REPORT.md`.

## Usage

```bash
# Catalog a directory of packaged titles by filename (no extraction), + probe 10 headers:
python ps3_recomp_harness.py catalog --archive-root /path/to/your/archives --probe 10

# Triage every decrypted binary under a tree (profile + function detection):
python ps3_recomp_harness.py analyze --elf-root /path/to/decrypted-elfs --max-tier 4

# Push a few all the way through the lifter (opt-in, slow, big output):
python ps3_recomp_harness.py analyze --elf-root /path/to/decrypted-elfs/one-title --max-tier 5

# Cross-check our function detection against Ghidra (opt-in, needs Ghidra):
python ps3_recomp_harness.py analyze --elf-root /path/to/decrypted-elfs/one-title --max-tier 6

# Re-aggregate the report from existing per-title results:
python ps3_recomp_harness.py report
```

Key flags: `--max-tier {2,3,4,5}`, `--limit N`, `--force` (re-run recorded
titles), `--elf-root` (repeatable), `--probe N`, `--jobs N` (lifter parallelism),
`--keep-lifted`, `--out <results root>`, `--ps3sce <path to ps3sce>`.

## What the report tells you

- **Catalog** — region distribution (SCEA/SCEE/SCEJ/…), content-class
  distribution (full game / demo-minis / PS1-minis / app), total footprint. A bird's-eye view of
  whatever set of titles you point it at.
- **PKG-header probes** — verified content-id / item-count / size for the sampled
  titles.
- **Decrypted-ELF recomp triage** — a pipeline funnel, a per-binary profile table
  (machine, image base, segments, function count, `.opd`-seeded starts, lift
  result), an **image-base distribution** (flags anything that isn't the standard
  `0x00010000`), and **giant-function / blocked-binary** catalogs that point at
  boundary mis-detection and decryption gaps.

## Dependencies

- **`ps3sce`** (a `scetool` clone) for `*.self` → `*.elf` decryption. NPDRM titles
  need the licence key for **your own** copy in its `raps/` dir or `klics.txt`. Point at it
  with `--ps3sce`.
- **7-Zip** (`--sevenzip`) for the nested-rar PKG-header probe.
- The repo's own `tools/elf_parser.py`, `find_functions.py`, `ppu_lifter.py`.

Catalog + profile + function tiers need no keys and scale to large sets;
decrypt (for NPDRM) and lift are where cost and key requirements kick in.
