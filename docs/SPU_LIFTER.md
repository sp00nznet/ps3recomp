# SPU → C Lifter — Status & TODO

Brief for `tools/spu_lifter.py` (added to close the "no SPU lifting" gap noted
in the README's critical gaps). The PPU side had `ppu_lifter.py`; the SPU side
only had `spu_disasm.py`. This document tracks what the lifter does today and
what still needs doing to actually execute SPU code.

## What exists now

`tools/spu_lifter.py` mirrors `ppu_lifter.py`:

- Emits each SPU function as `void spu_func_XXXXXXXX(spu_context* ctx)`.
- Instruction semantics live in `static inline u128 spu_*()` helpers in the
  generated source preamble (readable + unit-testable); the translator just
  wires `ctx->gpr[N]` to them.
- Output: `spu_recomp.h` / `spu_recomp.c`, plus a `spu_function_table[]`.
- CLI prints a coverage summary including a per-mnemonic list of any
  unsupported instructions encountered.

Targets the ABI in `runtime/spu/spu_context.h` (`u128 gpr[128]`, 256 KB
local store, channels, MFC staging) and the `u128` union in
`include/ps3emu/ps3types.h`.

## Relationship to the PPU fallback approach (read `SPU_FALLBACK.md` first)

The project's **current** SPU strategy is *not* execution — it is the
PPU-side fallback registry (`SPU_FALLBACK.md`): per game, you hand-write a C
function that reproduces each SPU job's output, keyed on the SPU image entry
point. That is pragmatic and gets specific games running fast, but it is
**manual and per-job** — every game re-pays the cost, and jobs whose logic is
non-trivial (decompressors, mixers, physics) are hard to reimplement by hand.

This lifter is the **general, automated** path: translate the SPU ISA once and
run the real job. The two are complementary, and they can coexist:

- Use a **fallback** when the job is simple to reproduce or when the lifted
  output isn't trusted yet.
- Use the **lifted SPU function** when the job is complex or unknown.

A clean integration: have `spu_indirect_branch` / the group-start dispatcher
first consult `spu_lookup_ppu_fallback(entry_point)`, and only fall through to
the lifted `spu_func_*` (via `spu_function_table[]`) when no fallback is
registered. That makes lifting an opt-in upgrade over the existing shim
mechanism rather than a replacement.

### Implemented instructions (Phase 1)

- **Immediates:** il, ila, ilh, ilhu, iohl
- **Integer:** a, sf, ah, sfh (+ ai/ahi/sfi/sfhi); mpy/mpyu/mpyi
- **Logic:** and/or/xor/nand/nor/andc/orc (+ andi/ori/xori)
- **Compare:** ceq/ceqh/ceqb, cgt/cgth/cgtb, clgt/clgth/clgtb (+ ceqi/cgti/clgti)
- **clz, cntb**
- **Select / shuffle:** selb, shufb (faithful, incl. the 0x80/0xC0/0xE0
  selector special cases)
- **Shift/rotate immediate:** shli, shlhi, roti, rothi, rotmi, rotmai, rotmhi,
  shlqbyi, rotqbyi
- **Float (single):** fa, fs, fm, fma, fms, fnms, fceq, fcgt
- **Memory:** lqd, stqd, lqa, stqa, lqx, stqx
- **Branches:** br, bra, brsl, brasl (link reg recovered from raw encoding),
  brz/brnz/brhz/brhnz (internal → `goto`, cross-function → trampoline call),
  bi, bisl, biz/binz/bihz/bihnz (→ `spu_indirect_branch`)
- **stop, nop, lnop, sync, dsync, hbr/hbra/hbrr** (hints dropped)

## TODO — to actually run SPU code

### 1. Runtime glue — **done** (`runtime/spu/spu_channels.c`)

Implements the externs the lifter declares:
- `spu_wrch` / `spu_rdch` / `spu_rchcnt` route MFC channels (`MFC_LSA`,
  `MFC_EAL/EAH`, `MFC_Size`, `MFC_TagID`, `MFC_Cmd`, `MFC_WrTagMask`,
  `MFC_WrTagUpdate`, `MFC_RdTagStat`, `MFC_RdAtomicStat`, ...) through
  `mfc_channel_write`/`mfc_channel_read` from `spu_dma.h`, and mailbox /
  signal / event / decrementer / SRR0 channels through the
  `spu_channel_*` helpers + `spu_context` fields.
- A small `spu_context* → mfc_engine` registry holds per-SPU DMA state
  (the engine isn't embedded in `spu_context`).
- `spu_indirect_branch` resolves `ctx->pc` via a flat address-keyed
  registry of `spu_func_*`. The lifter now emits a `spu_recomp_register()`
  that populates the registry from `spu_function_table[]` — call it once
  at program init.
- Tiny ABI shortcut for the standard SPU calling convention: the lifter
  translates `bi $r0` (return through the link register) directly to a
  host `return;`, so SPU call/return nests as normal C calls.

#### Validation
End-to-end test (two functions; A calls B via `brsl`, B writes outbound
mailbox via `wrch`, returns via `bi $r0`; test main pre-fills inbound
mailbox then reads it via `rdch`):

```
gcc -std=c11 -O2 spu_recomp.c runtime/spu/spu_channels.c test_main.c
```

All seven checks pass (link saved correctly, `wrch` populates
`ctx.ch_out_mbox` with the right value+count, `rdch` consumes
`ctx.ch_in_mbox`, function return restores execution to A).

### 2. Missing instructions (extend `_translate` + add a helper)

Real-image coverage measured against an SPU ELF extracted from Uncharted:
Drake's Fortune (BCUS98103, EBOOT.elf @ 0x845000, 36,608 bytes of code,
~9,150 instructions). **After the Phase 2 pass below: 0 lifter gaps remain
for instructions the disassembler decodes.** The only remaining "unsupported"
entries (683) are `.word` — i.e., bytes the disassembler couldn't decode at
all. That is now §2b's problem, not the lifter's.

Closed in Phase 2:

| Group | Count closed | How |
|---|---|---|
| Variable-count shifts/rotates (`shl`, `shlh`, `rot`, `roth`, `shlqbi`, `rotqbi`, `shlqby`, `rotqby`, `shlqbybi`, `rotqbybi`) | ~590 | Register-variable mirrors of the immediate forms. |
| `rotmahi` | 203 | 16-bit twin of `rotmai`. |
| Immediate compares (`ceqbi`, `ceqhi`, `clgtbi`, `clgthi`) | 69 | Byte/halfword imm twins of the word forms. |
| FP status / estimates (`fscrrd`, `fcmeq`, `fcmgt`, `frsqest`) | 41 | `fscrrd` stubs to 0 (no FPSCR model); the rest implemented IEEE-style. |
| Misc (`gb`, `gbh`, `cg`, `addx`, `mpyh`, `mpyhh`, `mpys`, `mpyui`) | ~30 | One-offs. |
| `shlqbii`, `rotqbii` (quad bit shift/rotate, immediate) | 69 | Found during the build pass — disassembler maps these as 3-reg RR, lifter strips the spurious `$r` prefix to recover the i7 value. |

The "RI7 emitted as RR" wart is a disassembler quirk (§2b territory) handled
defensively in the lifter for now.

Phase 3 — **done**. All mnemonics that the disassembler emits now have
helpers. Verified on the Uncharted SPU image:

- d-form constant generators (`cbd`/`chd`/`cwd`/`cdd`) → 16-byte insertion
  shuffle pattern via `spu_c{b,h,w,d}d_pos()` helpers.
- Indexed constant generators (`cbx`/`chx`/`cwx`/`cdx`) → same patterns,
  position derived from `(ra+rb) & 0xF`.
- Form-select-mask (`fsmbi`/`fsm`/`fsmh`/`fsmb`) → bit→lane expansion.
- Rotate-and-mask family (`rotm`/`rotma`/`rothm`/`rothma`/`rothmi`/`rotqmbi`/
  `rotqmby`/`rotqmbybi`/`rotqmbii`/`rotqmbyi`) → right-shift fills.
- Sign-extension (`xsbh`/`xshw`/`xswd`).
- Halfword/byte immediate logic (`andhi`/`andbi`/`orhi`/`orbi`/`xorhi`).
- OR-across (`orx`).
- `bgx` (borrow-generate extended) — reads `rt` as carry-in like `addx`.
- Halt-conditionals (`hgti`/`hlgti`/`heqi`/`hgt`/`hlgt`/`heq`) → emitted as
  no-ops (recompiled code does not honor SPU halt semantics).
- `stopd` → same as `stop`.
- `lqr`/`stqr` → LS load/store at PC-relative target (same as `lqa`/`stqa`).
- `mfspr` → stub (returns 0; SPU SPRs are not modeled).

Result on the Uncharted SPU image after Phase 3:
- **0 lifter gaps remain** for any instruction the disassembler decodes.
- The 1,092 `.word` entries are bytes in the executable segment that are
  not SPU instructions (data tables, padding placed in `.text` by the
  linker). Eliminating them is §4 work — split text from data using the
  ELF section table and function-boundary detection.

Future — not yet hit by any image we've measured:
- Register-variable shifts/rotates: shl, shlh, rot, roth, shlqbi(i),
  rotqbi(i), shlqby(bi), rotqby(bi), rotqbybi
- Float: fi (interpolate), frest/frsqest (+ the Newton-Raphson step pairs),
  fcmeq/fcmgt, fscrrd/fscrwr, double-precision (dfa/dfs/dfm/dfma…)
- Integer extras: addx/sfx/cg/bg (extended add/sub with carry/borrow),
  mpyh/mpys/mpyhh*, mpyhha, cntb edge cases, gb/gbh/gbb (gather bits), fsmb/fsm
- Conversions: cuflt/csflt/cflts/cfltu (int↔float), fesd/frds (sp↔dp)
- form-select/extend: fsmbi, cbd/chd/cwd/cdd (generate insertion masks for
  scalar stores — needed for the standard "load-quad / insert / store-quad"
  unaligned scalar access idiom)

### 2b. Disassembler — **rewritten against the RPCS3 authoritative table**
The original `spu_disasm.py` had systemic errors: `IL`/`ILH`/`ILHU` listed as
RI18 instead of RI16; `BR`/`BRSL`/`BRA`/`BRASL` likewise; the entire
shift/rotate immediate group (`shli`, `shlhi`, `roti`, `rothi`, `rotmi`,
`rotmai`, `rotmhi`, `rotmahi`, `shlqbii`, `rotqbii`, `shlqbyi`, `rotqbyi`)
sitting at RI10 opcodes that are actually `brz/brnz` prefixes; `lqa`/`stqa`
at the wrong RI16 opcodes; `hbra` occupying `iohl`'s position; etc.
Result on real SPU code: real instructions silently mis-decoded as something
else (`rot` → `shl`, `stqa` → `shlhi`), and the lifter ran *wrong* but
*compilable* C.

Fixed by replacing `RI18_TABLE`, `RI16_TABLE`, `RI10_TABLE`, and `SPU_RR`
with values verified against the SPU decoder table in RPCS3's
`Emu/Cell/SPUOpcodes.h` (`{magn, value, handler}` entries). All RI7
shift/rotate forms now live in `SPU_RR` at their real 11-bit opcodes
(0x078–0x07F and 0x1F8–0x1FF). Operand formatters updated.

After the fix, on the Uncharted SPU image:
- Call targets detected: **13 → 47** — the call graph is now visible.
- 36 new real-SPU mnemonics appear (`cwd`, `fsmbi`, `xsbh`, `rotqmby`,
  `fsm`, `cbd`/`chd`/`cwd`/`cdd`, `hlgti`, `heqi`, `rothmi`, `xorhi`,
  `andhi`, `orhi`, `mfspr`, `xshw`/`xsbh`/`xswd`, etc.). Previously these
  bytes were mis-decoded as something the lifter handled, hiding the gap.
- `.word` count rose (683 → 1092) because the new decoder honestly reports
  bytes it can't identify (data, jump tables embedded in the executable
  segment) instead of matching them to bogus tables.

The original tables are kept as `tools/spu_disasm.py.bak` for reference.

### 3. Correctness items — partially **done**

Refactor + unit tests (`runtime/spu/spu_helpers.h`,
`runtime/spu/tests/test_spu_helpers.c`):

- All semantics helpers extracted from `spu_lifter.py` into the shared
  header — single source of truth for the lifter, the runtime, and tests.
- 63-case unit suite covering: immediate loaders, SIMD arithmetic, multiply
  family, compares, shift/rotate boundaries (sh=0 and sh≥width), shufb
  (incl. the 0x80/0xC0/0xE0 special selectors), selb, constant generators
  (cwd/cbd/chd/cdd/_pos), fsmbi, sign-extension, fused FP multiply.
- Build + run: `gcc -std=c11 -O2 -Wall -Wextra test_spu_helpers.c`; exit
  code = 0 if all pass. Currently **63 passed / 0 failed**.

**Endianness bug found and fixed**: `mpy`/`mpyh`/`mpyhh`/`mpyu`/`mpys`/`mpyi`/
`mpyui` and `xsbh`/`xshw`/`xswd` were accessing sub-lanes via `_s16[2i+1]` /
`_u8[2i+1]` etc., assuming big-endian element order inside `u128`. On the
LE host the recompiler targets this picks the *opposite* sub-lane.
Symptom: silent wrong results for any code using these mnemonics (~16
appearances in the Uncharted SPU image). Fix: invert the sub-lane index on
LE host (`_s16[2i]` etc.). Documented in `spu_helpers.h` with comments
explaining the host-endianness assumption.

Still open:
- `shlqby` direction and the preferred-halfword choice for `brhz`/`brhnz`
  (which `_u16[]` lane is the "halfword preferred slot" in BE semantics
  reinterpreted on LE storage). Both are exercisable by adding test
  vectors; the suite framework now makes it cheap.

### 4. Pipeline integration
- **SPU program discovery — done** (`tools/extract_spu_images.py`). Scans a
  PPU ELF / EBOOT for embedded ELFs with `EM_SPU=23`, computes each image's
  byte-size from its program/section headers, and writes them out. Verified
  on Uncharted: Drake's Fortune (1 image extracted, 39,608 B, code segment
  at va 0x3000).
- **Function-boundary detection for SPU images — done**
  (`tools/find_spu_functions.py`). Seeds from ELF entry point + STT_FUNC
  symbol-table entries + every `brsl`/`brasl` target the disassembler
  resolves. Walks each start forward until `stop`/`stopd`, `bi $r0` (SPU
  ABI return), an unconditional branch with no fallthrough, or the next
  seed. Symbol entries with non-zero `st_size` give exact boundaries
  directly. Emits the `--functions` JSON the lifter already consumes; or
  the lifter can take an ELF directly via `--auto-functions <elf>` and
  run the detector in-process.

  Verified on the Uncharted SPU image (no symbol table — fully stripped):
  **461 functions** detected from 47 `brsl`/`brasl` + other branch targets
  + entry. (Seeding only from calls was the first cut and gave 44; running
  an exploratory harness against the lifted output revealed orphan symbols
  for branch destinations the detector had not promoted to functions —
  `collect_branch_targets` was widened to include `br`/`bra`/`brz`/`brnz`/
  `brhz`/`brhnz` and the count jumped 10×.) Re-lifting with these
  boundaries: 461 functions emitted, only 4 `.word` remaining (the actual
  data tail). Build clean, and the entry function `spu_func_000030F0`
  executes to a natural return in the harness — zero indirect-to-unknown
  branches, zero crashes, zero channel I/O (the bare entry happens to be
  pure setup with no PPU dependency).

  Lifter follow-up: the lifter emits direct `spu_func_X(ctx)` calls for
  every branch target the disassembler resolves, including cross-function
  branches. emit_source now generates a trivial stub for any target the
  detector did NOT promote to a real function — the stub routes through
  `spu_indirect_branch`, which finds the function if registered or halts
  with a clean diagnostic. Without the stubs, those calls were unresolved
  externs and the link failed.

  Harness follow-up (`Extracoes/spu_uncharted1/uncharted_run/`): halts
  from `spu_indirect_branch` were not actually terminal — the C call
  returned into its caller's fallthrough, which often re-entered the
  same bad indirect (timeout loop). The harness now `setjmp`s in `main`
  and `longjmp`s from inside the halt path, so a HALT really stops the
  SPU instead of unwinding a stack frame at a time.
- Wire `spu_lifter.py` into the build alongside `ppu_lifter.py` (CMake).

## Tracing (for §3 diff vs RPCS3)

The lifter now accepts `--trace`. When set, every emitted instruction is
wrapped:

```c
spu_trace_pc(ctx, 0x<PC>);
<lifted instruction>
spu_trace_rt(ctx, <rt>);   // only for instructions that write rt
```

Runtime helpers live in `runtime/spu/spu_channels.c`:
- `spu_trace_init(path)` — `NULL`/empty → stderr; otherwise write to file.
- `spu_trace_pc(ctx, pc)` — one line per executed instruction: `<PC-5hex>`.
- `spu_trace_rt(ctx, rt)` — destination post-state: `  r<rt> <hi64> <lo64>`.

On the RPCS3 side, the equivalent capture is:
- `Core.SPU Decoder = Interpreter (static)` (the recompilers don't emit
  per-instruction trace).
- `Log.SPU = Trace`.
- Boot the SPU ELF directly via CLI: `rpcs3.exe <spu>.elf`. RPCS3 recognises
  standalone SPU ELFs and creates a single SPU thread for the image, avoiding
  the noise of booting a full title.

The two traces line up by PC; a tiny converter (still to be written) can
normalise RPCS3's `SPU.log` lines into the same `<PC>` / `  r<rt> ...` shape
this runtime emits, so plain `diff` finds divergences.

## How to validate

Status: **compile- and run-verified** with GCC 16.1.0 (MinGW-w64, UCRT).
A 7-instruction arithmetic function (il/a/ai/sf/and/cgt) was lifted, compiled
(`gcc -std=c11 -O2 spu_recomp.c test_main.c`) and executed — all register
results matched expected values (incl. `sf` = -2 and `cgt` = 0). Generate
output into a dir two levels under the repo root (e.g. `gen/spu/`) so the
header's relative `#include "../../runtime/spu/spu_context.h"` resolves, then:

```
gcc -std=c11 -Wall -c gen/spu/spu_recomp.c      # compile check
```

Still to do:
1. Per-helper unit tests (`spu_shufb`, compares, rotates) against expected
   vectors from the Cell BE ISA manual — especially the byte-shuffle special
   cases and the shift-direction questions in §3.
2. Diff execution against RPCS3 SPU traces for a real program (e.g. a SPURS
   job from the target game) once SPU images are extracted from the EBOOT.

### Integration tests (runtime/spu/tests/)
End-to-end tests that exercise the **whole pipeline** (encoder → wrap_spu_elf
→ find_spu_functions → spu_lifter --auto-functions → gcc → harness) on
hand-written SPU programs with known input/output. Per-test layout:
`gen_test_<name>.py` (encoder + ELF wrap) + `test_<name>_main.c` (harness
with channel-overriding stubs and asserts). `run_tests.sh` drives the
suite.

Current state (4 tests):

| Test | What it exercises | Result |
|---|---|---|
| `sum` | loop with `rdch` × N, accumulator, `brnz`, `wrch` | PASS |
| `shufb` | shufb special selectors (`0xC0`/`0xE0` magic bytes) | PASS |
| `brsl_return` | function call via `brsl r0`, return via `bi $r0` | PASS |
| `dma` | full MFC DMA round-trip (LSA/EAH/EAL/Size/TagID/Cmd channels, GET into LS, `lqd`, `wrch` out) | PASS |

The original DMA failure was the §3 endianness gap surfacing: on an LE
host the helpers store register lanes in native order so that `_u32[i]`
gives lane i directly, but `spu_ls_read128`/`write128` were doing raw
`memcpy` of LS bytes (which are big-endian, native to the SPU). Loading
a quadword from LS therefore produced byte-swapped lanes.

Fixed by making the two quadword LS helpers byte-swap per 4-byte lane,
mirroring what `spu_ls_read32` already does. The same approach keeps the
helper library untouched (every helper continued to work without
modification) and only adds work at the LS-to-register boundary — i.e.
the same line as the existing `spu_ls_read32` convention.

The alternative — extracting "preferred slot" via explicit BE byte
assembly in every channel consumer — would have required touching
`spu_rdch`, `spu_wrch`, and every harness, plus rewriting `shufb` to
swap indices on LS-loaded data. The per-lane swap in LS access is
strictly less invasive and is the model the helper authors had already
committed to.
