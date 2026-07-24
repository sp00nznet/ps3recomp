# Contributors

ps3recomp exists because people who love the PS3 keep showing up. Thank you to
everyone who has contributed code, fixes, testing, or a hard-won debugging
insight. This file is the canonical record of who did what; the README changelog
tells the story version-by-version, but credit lives here.

If you've contributed and aren't listed (or a line is wrong), open a PR against
this file — we want every name right.

---

## Maintainer

### Ned Heller — [@sp00nznet](https://github.com/sp00nznet)
Project creator and maintainer. Core toolchain (PPU/SPU disassemblers and
lifters, ELF/SELF/PRX parsing, NID system, runtime libraries and system-call
layer), the recompilation pipeline, and the batch-triage harness.

---

## Contributors

### Caner Saka — [@canersaka](https://github.com/canersaka)
Found and fixed a remarkable run of correctness bugs while stress-testing the
toolkit against a 22 MB / ~45,000-function **Yakuza: Dead Souls** port — which
turned out to be an excellent fuzzer for everything the pipeline got subtly
wrong. Landed in **v0.6.1 "Many Hands"**:

*Decode & lift correctness*
- **NID computation fix** — the suffix constant was truncated/corrupted and the
  digest was read big-endian; corrected to the authoritative 16-byte suffix +
  little-endian. Took NID resolution on a real game from 0/354 to 343/354.
- **VMX/AltiVec decode tables** — cross-referenced against the Power ISA manuals;
  fixed dozens of mnemonics mapped to the wrong opcodes that were decoding
  silently and lifting to valid C for the *wrong* operation.
- **VMX lifter handlers** — `addc`/`subfc` with carry-out into XER[CA], the
  unaligned vector loads `lvlx`/`lvrx`/`lvlxl`, `ldbrx`, `vsrw`, `vsububs`,
  `vsum2sws`, `vupkhsh`, `vrfim`, and a missing `vand` dispatch entry.
- **`sradi` decode for shifts ≥ 32** — the 6-bit shift's top bit lives in
  instruction bit 30, so the XO field reads 827 for shifts of 32+.
- **Function detection seeded from the `.opd` table** — seeds starts from every
  address-taken function descriptor (located by shape, since section names are
  often stripped), bounds them at the first `blr`, and filters phantom branch
  targets. A large improvement to coverage of pointer-only/virtual functions.

*Lifter throughput*
- **Parallel lifting** (`--jobs N`) across a process pool, chunked C output, and
  record-form CR0 handling — turned a multi-hour single-core lift into minutes.

*Runtime & libraries*
- **`sys_vm` syscall family (300–313)** implemented from scratch.
- **`sys_memory`** allocations from a committed-on-demand window matching
  real-hardware addresses, plus thread-safe bump allocation.
- **`sys_lwmutex_destroy`** lv2-correct ESRCH/EBUSY semantics + slot locking.
- **TTY syscall numbers** corrected to 402/403.
- **`cellVideoOut`** big-endian guest structs + corrected `CellGcmContextData`
  layout.

*Build & tooling*
- Excluded runtime test harnesses from the library build (clean MSVC build).
- **`tools/show_func.py`** — dump a single lifted function's C and/or its
  original PowerPC disassembly from the chunked output.

*Also incorporated (**v0.6.4** "Carry the One")* — the PPU **XER[CA] carry/borrow
bug class** (shift-algebraic `sraw`/`srad` forms + `mtcrf` mask, #21;
`subfe`/`subfme`/`addme`, #26), `cntlzw(0) = 32` (#35), the **PPU lifter
conformance suite** (#37), and the `vcmpgt*` handlers (#39); the SPU
**computed-`bi $r0` tail-jump classifier** (#36), self-referential branch
mislift fix (#30), full SPU ISA coverage (#31), byte-correct quadword helpers
(#32), `il` double sign-extension (#33), and preferred-slot link register +
`rchcnt` (#34); plus `mftb`/`mftbu` reading a real timebase (#38).

*Also incorporated (**v0.6.5**)* — cellFs big-endian out-params + `CellFsStat`
PS3 packing (#22), cellGame title id read from `PARAM.SFO` (#24), `sys_rwlock`
`EDEADLK`/`EPERM` lv2 semantics (#25), the `crnor`/`crnand` opcode-33/225
disassembler fix (#40), and **static firmware LLE** (`tools/lift_prx.py`) —
relocating a decrypted PRX to lift a real firmware module (e.g. the libsre
SPURS kernel) instead of HLE-ing it, under a bring-your-own-firmware model
that ships nothing derived from firmware (#53).

*Also incorporated (**v0.6.6** "Two Ports, One Toolkit")* — `fsqrt`/`fsqrts`
source-register decode + `vspltis` signed-immediate printing (#46), the
`addeo`/`subfeo`/`mulhwo`/`mulhwuo` overflow forms + `vupklsb`/`vupkhsb`/`vupklsh`
unpacks (#52), sub-millisecond `sys_timer` usleep (#44), `cellNetCtl` big-endian
out-params (#45), and `cellAudio` period-event delivery to notify queues (#54).

*Also incorporated (**v0.6.7** "Fine Print")* — NV40 VP/FP shader-decompiler
hardening (#47), `spu_disasm` channel-width/hbr/`bisl`-link/`ri7` fixes (#49),
sub-millisecond timeouts for event-flags/semaphores/mutexes (#50), corrected HLE
ABI signatures across cellPamf/Sail/Http/Ssl/Net/sysNet/sceNpTrophy/cellGameData
(#55), and a lifter/HLE audit toolkit + lv2 sync stress test (#56).

*Also incorporated (unreleased)* — a PPU/SPU correctness batch, several of which
were silent-miscompile classes rather than crashes:
- **VMX element-wise ops read big-endian lanes** — values were byte-reversed on
  x86, so every float lane op quietly computed on the wrong data (#74).
- **D-form loads/stores treat `rA=0` as a literal zero base**, not `GPR[0]` (#72).
- **Update forms write back `rA`** — indexed stores (#71) and FP loads/stores
  (#73); dropping the write-back leaves `rA` stale and corrupts every subsequent
  `(rA)`-relative access.
- **`lmw`/`stmw`** load/store multiple word (#68), **`mulldo`** overflow form
  (#70), and **`stvlx`/`stvrx`/`stvlxl`/`stvrxl`** Cell unaligned vector stores
  (#61).
- **Value-verified CAS** for `lwarx`/`stwcx` and `ldarx`/`stdcx` (#62).
- **FP correctness batch** — `fcmpu` NaN ordering, `fcti` saturation, single
  rounding, `vctsxs`/`vctuxs` (#60); plus the objdump decoder audit that found 5
  missing VMX float ops.
- **SPU**: `brsl` target parse broken by the disasm link-register fix — every call
  lifted as a silent no-op (#58); `bisl`/`bisled` target register is the last
  operand; single-round `fma`/`fms`/`fnms`, `fesd`/`frds` from the left word slot,
  and `fi`/`frest`/`frsqest` base+step interpolation (#51).
- **lv2**: `sys_mutex_trylock` recursive fast path pairs the host critical section
  (#67); `sys_spu_thread_group_create` reads r5 as priority and the name from the
  attr struct (#66).

### Jonathan Del Corpo — [@JonathanDC64](https://github.com/JonathanDC64)
Correctness and robustness fixes distilled from a **Demon's Souls** port that
stress-tested the toolkit against a ~106k-function title. The title-agnostic wins
incorporated (**v0.6.6**):
- **SPU cross-function tail calls forced with `musttail`** — a guest loop whose
  back-edge crosses a lifted-function boundary was nesting one host C frame per
  iteration and silently overflowing the stack (a stack-overflow SE runs no
  unhandled filter, so the process died with no crash log and exit code 0); now an
  O(1)-stack jump under clang, with a call+return fallback elsewhere.
- **Mid-function / gap lifting sliced O(n²)→O(span)** — turned a ~40-minute
  no-output hang on 96k+ function titles into bounded work.
- **`sys_event_queue_receive` returns the event in r4–r7** (lv2 ABI) — callers
  that read the registers instead of the `sys_event_t*` buffer saw stale values.
- **`sys_memory_get_page_attribute`** renumbered 358→351 (0x15F) and implemented.
- **`CellFsStat` runtime-side layout** corrected to the 52-byte / 4-byte-aligned
  ABI, fixing the `ppu_fs.cpp` / `sys_fs.c` stat writers the v0.6.5 libs-side fix
  didn't cover.

Further title-agnostic fixes incorporated (**v0.6.7**):
- **`g_active_ctx` restored after guest callbacks** — it was left pointing at a
  stack-local scratch ctx, so after any callback the current-thread ctx pointer
  dangled and corrupted the crash handler / diagnostics; applied to both
  `ppu_guest_call` and `ppu_guest_call_ct`.
- **SPU `stop` signal code preserved** to a new `ctx->stop_code` — SPURS leaf
  tasks invoke kernel syscalls via `stop <code>` (EXIT/YIELD/WAIT/POLL/…), which
  the lifter was discarding.
- **`cellGameDataCheckCreate2`** (NID `0xC9645C41`) implemented — marshals
  StatGet/StatSet and fires the guest `funcStat` callback.

*(The port's remaining fork-specific work — SPU/PPU jump-table discovery tuned to
a different lifter architecture, and cellKb/Mouse/Rtc guest-pointer translation
that needs a big-endian rewrite — is tracked for a follow-up with a re-lift/test
loop rather than a blind port.)*

### sagemono — [@sagemono](https://github.com/sagemono)
Real-controller correctness surfaced by a DualShock-as-XInput bring-up
(**v0.6.5**):
- **cellPad DIGITAL2 packing** — `hs->buttons` carries DIGITAL1 in its low byte
  and DIGITAL2 (the face buttons cross/circle/triangle/square + L1/L2/R1/R2) in
  the high byte; the whole value was being written into DIGITAL1 with DIGITAL2
  forced to 0, so every face button was dead. Split correctly (#42).
- **analog-Y centering** — reflect the inverted Y about 128 (`256 - x`) instead
  of `255 - x`, which turned a centered stick into 127 (`0x7F`, aliasing
  SELECT+START self-exit + TRIANGLE).

The **RSX draw engine** and the toolchain work behind it, developed across a
homebrew TUI (cellmark), vkcube, wave, and a PSL1GHT/Tiny3D bring-up
(*unreleased*):

*Rendering*
- **2D/dbgfont pipeline + vertex-program capture**, then **executing the title's
  real NV4097 vertex program** on D3D12 for pixel-perfect text (#43) — grown from
  there into guest fragment-program pipelines, per-draw textures and VP constants,
  indexed draws, render-to-texture, MRT, multi-unit textures, FP predication and
  branches, float RTs, and a VS cache keyed by ucode hash.
- **Present correctness** — present only on guest flip (partial-frame flicker),
  present at the clear boundary (ring-wrap blink), flips ordered against the
  drain, honest flip status, synced non-blocking flips.
- **FIFO ring recycle on wrap** — the command ring never recycled (the callback
  OPD was null), wedging ~200 frames in; a real command-buffer-full callback now
  drains and resets it.
- **`cellGcmSetFlipCommand` context argument**, `MapMainMemory` never handing out
  IO offset 0 (FIFO collision), back-end label byte-swap, and discrete-GPU
  selection by default.

*Lifter & toolchain*
- **`blrl` dispatched as an indirect call** — it was lumped with `blr` and emitted
  a bare `return;`, dropping the call *and* skipping the frame epilogue, so `r1`
  leaked the frame size on every function-descriptor call (#42).
- **EBOOT (`ET_EXEC`) import parsing** — the lib-stub table was only reachable via
  the PRX module_info path, so every retail main executable reported 0 imports;
  now located via `PT_PROC_PRX_PARAM`. Minecraft: 0 → 345 NIDs across 30 modules
  (#41).
- **`lift_function` indexed by address** — an O(refs·N) mid-function pass hung the
  lifter indefinitely after "100% lifted" on large titles (~11.6k refs × 3M
  instructions); now bisect over a cached sorted index (#41).
- **Rotate-by-0 undefined behaviour** in the rotate helpers — `v >> (64 - n)` at
  n=0 is UB, and clang miscompiled the `clrldi` truncation idiom that every 32-bit
  address/index cast lowers to (#41).
- **Function-registry sizing** for >64k-function titles, jump-table switches as
  in-function computed gotos, multi-TOC jump-table discovery, and callee-save
  detection that rejects a reused save slot / scratch spill.
- **Lifter torture suite** — 3793 KATs against an independent PPC model (#64),
  plus the fused-fmadd / PPC NaN / `mfspr`/`mtspr` semantics it surfaced.
- **`sys_spu_image_import`** implemented (SPU ELF → entry point + segment table)
  and the SPU image syscall numbers corrected (#57).

A **guest-ABI HLE correctness batch**, mostly surfaced by LBP and DeferredShading
bring-ups (folded via `integrate/fold-2026-07-24`):
- **Guest-EA / big-endian out-param marshalling** across the HLE surface — a run
  of handlers were writing host pointers or native-endian values into what are
  actually guest VM addresses: `cellUserInfo` (overflow), trophy u64s,
  `cellGameGetParamInt` DISC type, open PSID, `cellRtc`, save-data user paths,
  `cellJpgDecDecodeData`, `cellPngDec` (first live use, DeferredShading), and the
  `cellAudio` read index as a BE u64.
- **`cellFsSdataOpen` with real SDATA/EDAT (NPD) decryption**, `cellFsMkdir` + the
  game-data dir created where the VFS actually looks, and a `/dev_hdd0` overlay
  onto the installed-update tree (`PS3_HDD0_ROOT`).
- **Honest state, not silently-faked CELL_OK** — a module name that matches no
  file now fails; net reports a real offline state with real SDK NP error codes;
  `sys_time_get_system_time` returns real microseconds instead of a call counter.
- **Raw-`sys_fs` path parity** — strip mount prefixes so raw `sys_fs` opens hit
  the same host tree as the cellFs layer; `cellHttpCreateClient` guest-EA
  translation (fixed a boot crash); HLSL-safe NaN/Inf fragment-program constants;
  `sys_spu_image_import` segment source EA at the correct offset.

The **SPU-lifter "faithful adoption"** (branch `spu/faithful-adopt-caner`, folded
via `integrate/faithful-adopt-caner`) — a from-scratch re-implementation of the
SPU decode/lift + runtime, consolidating the interpreter and function registry
into `spu_channels.c`, plus LBP SPU/SPURS bring-up and RSX texture support
(G8B8/DXT, deswizzle). **This work re-adopts the original SPU decoder/lifter and
runtime that [@sp00nznet](https://github.com/sp00nznet) and
[@canersaka](https://github.com/canersaka) authored** — their commit history for
that work stands on its own (the `caner/ppu-*`, `c6*/c7*`, and the core SPU
subsystem commits); the faithful-adoption branch is a parallel re-derivation, and
credit for the underlying design belongs to them.

### Paulo Adriano Alves — [@pauloadrianoalves](https://github.com/pauloadrianoalves)
Initial **PPU boot path** and supporting tooling (PR #3, partially incorporated
in **v0.6.2** — the SPU portions were superseded by the v0.6.0 SPU subsystem and
not taken):
- **PPU boot scaffold** — `runtime/ppu/` (loader, HLE dispatch, sysprx, fs) +
  `runtime/host/host_main.c`: the per-game path that loads a lifted PPU image,
  links it with the HLE runtime, and boots it. Compiled per-game against the
  lifter-generated `ppu_recomp.h`, so it lives outside the game-agnostic runtime
  library build.
- **RSX shader decompilers** — `libs/video/rsx_fp_decompiler.*` and
  `rsx_vp_decompiler.*`: NV40 fragment/vertex program → host shader translation,
  with a validation-test corpus.
- **Tooling** — `tools/ppu_loader.py` (image manifest / OPD table / TOC / imports
  extraction) and `tools/gen_hle_nids.py`.
- **Docs** — `docs/PPU_RECOMP.md`, `docs/RSX_FRAGMENT_PROGRAM.md`.

### gnome41 — [@gnome41](https://github.com/gnome41)
While porting *DBZ Budokai HD*, [gnome41's fork](https://github.com/gnome41/ps3recomp-dbz-budokai-hd)
surfaced and fixed real bugs in the shared SDK that have been incorporated
upstream:
- the AltiVec VX-form **11-bit XO extraction** fix (~3,800 instructions were
  mis-decoded);
- **19 additional VMX integer lifter handlers** + `crorc` / `crandc`;
- **SPU decoder corrections** (RI16 branch forms, channel opcodes per the IBM
  Cell BE manual);
- a `find_functions.py` walrus-syntax fix.

### Lucas Picoli — [@LucasPicoli](https://github.com/LucasPicoli)
- **Linux/GCC build fixes** — glibc's `st_atime`/`st_mtime`/`st_ctime` macros
  were shadowing identically named `CellFsStat` / `CellSaveData` members; five
  translation units now compile clean so downstream game projects get a Linux
  runtime to link against. (v0.6.1)

---

## A note on AI-assisted contributions

Parts of this project — and some contributions to it — were developed with the
help of AI coding tools. That's welcome here: what matters is that every change
is understood, reviewed, and verified by a human before it lands. If you used an
assistant, just say so in your PR (several contributors have) and make sure
you can stand behind the result.
