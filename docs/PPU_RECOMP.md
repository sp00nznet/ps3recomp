# PPU Static Recompilation — loader, ISA, HLE, boot

The PPU half of the recompiler. Where the RSX/SPU work is the **runtime** that a
recompiled game links against, this is the path that turns a game's PowerPC code
into a native host executable: disassemble → lift to C → load → link with the
HLE runtime → run.

`ps3recomp` is a **static recompiler** (à la the N64Recomp family), *not* an
emulator: the game's PPC machine code is translated to C ahead of time and
compiled into a native binary; there is no runtime CPU interpretation. The work
here is game-agnostic — Uncharted: Drake's Fortune (BCUS98103) is only the test
target (we have its decrypted `EBOOT.elf` + an RSX shader corpus).

---

## Pipeline

```
EBOOT.elf (PPC64 ELF, decrypted)
   │
   ├─ ppu_loader.py ──► image manifest + OPD function table + TOC + imports
   │                      (.functions.json / .image.json / .loader.json / .imports.json)
   │
   ├─ ppu_disasm.py ──► PPC64 disassembly
   ├─ ppu_lifter.py ──► ppu_recomp.{c,h}   (one C function per game function)
   │     --functions  <OPD table>          (authoritative function boundaries)
   │     --imports     <imports.json>       (firmware stubs → ps3_hle_call)
   │     --names       <ghidra names>       (optional, for readable comments)
   │
   └─ build: ppu_recomp.c + runtime (ppu_loader.cpp, ppu_hle.cpp + generated
             NID table) + HLE libs (libs/*) ──► native executable
```

---

## Loader

PS3 retail EBOOTs are `ET_EXEC` (fixed load address → **no relocations**;
relocations only matter for `ET_DYN` PRX/.sprx, a later stage). The loader has
two parts:

**Offline (`tools/ppu_loader.py`)** — parses the ELF and emits:
- **Image manifest**: the `PT_LOAD` segments (vaddr / filesz / memsz / flags).
  BSS is `memsz > filesz`.
- **OPD function table**: PS3 function descriptors are 8-byte `{u32 func, u32 toc}`
  pairs (32-bit ABI). A function *pointer* in the binary is an OPD address, so
  indirect calls must resolve OPD → code entry. The `.opd` section (located as
  the section containing `e_entry` — section names are usually stripped) gives
  the **authoritative list of function entry points** (far better than heuristic
  scanning) and the module **TOC** (`r2`). Uncharted: **14,104 functions**,
  entry code `0x10230`, TOC `0x7d3190`.
- **Firmware imports**: via the `proc_prx_param` segment (PT type `0x60000002`,
  magic `0x1b434cec`) → `libstub` table → `(library, NID, stub_addr)` per import.
  Uncharted: **172 imports across 12 libraries** (cellGcmSys, cellSpurs,
  cellSysutil, sysPrxForUser, sys_net, sys_io, sys_fs, …).

**Runtime (`runtime/ppu/ppu_loader.cpp`)** — provides what the lifted code needs:
- big-endian guest memory accessors (`vm_read8/16/32/64`, `vm_write*`),
- `ppu_load_elf(path)` — copies `PT_LOAD` into `vm_base + vaddr`, zeroes BSS,
- the address → function registry (populated by `ppu_recomp_register()`),
- `ps3_indirect_call(ctx)` — dispatches `ctx->ctr` (an already-OPD-resolved code
  address) through the registry,
- `g_trampoline_fn` (TLS) + `ppu_unlifted_stub` (subset/incremental builds),
- `lv2_syscall(ctx)` stub,
- `ppu_run(entry_opd, stack_top)` — resolves the entry OPD, sets `r1`/`r2`,
  dispatches the entry function.

---

## Lifter ABI

Each game function becomes `void func_XXXXXXXX(ppu_context* ctx)` (`ppu_context`
holds `gpr[32]`, `fpr[32]`, `vr[32]` VMX, `cr/lr/ctr/xer/...`). Output is C++
(`extern "C"`, `__declspec(thread)`).

- Direct call (`bl`) → `func_TARGET(ctx); DRAIN_TRAMPOLINE(ctx);`
- Indirect (`bctrl`/`bctr`) → `ps3_indirect_call(ctx)` (target in `ctx->ctr`).
- `bi $r0`-style returns, cross-fragment tail calls via the `g_trampoline_fn`
  TLS mechanism (iterative, avoids deep host recursion).
- `sc` → `lv2_syscall(ctx)`.
- Import stubs (with `--imports`) → `func_STUB(ctx) { ps3_hle_call(NID, ctx); }`.
- Call targets not in this lift → `ppu_unlifted_stub` (a full-image lift has none).
- `ppu_recomp_register()` populates the runtime address→function map.

**ISA coverage**: ~99.8% on real Uncharted code (measured by counting
`/* TODO */` no-ops). Remaining gaps are a few VMX ops the *disassembler*
doesn't decode (`op31_x983`, `vmx_x1098`), not the lifter.

---

## NID → HLE bridge

PS3 links firmware imports by **NID** (a 32-bit hash of the function name), not
by symbol. The bridge connects a game's import call to our HLE C libraries.

**NID algorithm** (`tools/nid_database.py`, `include/ps3emu/nid.h`): NID =
first 4 bytes of `SHA-1(name + suffix)` read **little-endian**, where the suffix
is the 16-byte secret
`67 59 65 99 04 25 04 90 56 64 27 49 94 89 74 1A`
(verified against RPCS3 `ppu_generate_id` and real EBOOT imports). *The previous
Python suffix/byte-order was wrong; `nid.h` was already correct.*

**Runtime (`runtime/ppu/ppu_hle.cpp`)**:
- a flat NID → handler table (`ps3_hle_register`, `ps3_nid_table` from nid.h),
- `ps3_hle_call(nid, ctx)` — resolves the NID and marshals the **PPC integer/
  pointer ABI**: args `r3..r10` → C args, return → `r3`. Covers the majority of
  `cellXxx` APIs. *Caveat:* functions that take/return **pointers** need
  host↔guest address translation (per-function wrappers); the generic path
  passes the raw value through.

**Table generation (`tools/gen_hle_nids.py`)**: scans an HLE lib's non-static
function definitions, **computes** each NID, and emits the `ps3_hle_register`
calls (no manual `/* NID */` annotations needed — and some hand-annotations were
found to be wrong).

**Coverage** against Uncharted's 172 imports: **69%** (118/172). cellGcmSys is
100% (26/26). Remaining gaps are concentrated in **networking (sys_net), input
(sys_io), and SPU tasks (cellSpurs)** — none render-critical — plus the
sysPrxForUser CRT.

---

## Boot

`runtime/ppu/tests/boot_main.cpp` links the whole PPU half (lifted code + loader
+ HLE bridge + HLE libs) and dispatches the entry. The integrated build works;
running it executes **real Uncharted startup code** until it hits a function
outside the lifted subset, an unimplemented import, or a syscall — each logged,
so the boot is a live to-do list. First boot stops at **`sys_initialize_tls`**
(NID `0x744680A2`), the textbook first CRT call.

**Full-image build** needs the lifter to **split output into multiple TUs**: a
14,104-function lift is ~88 MB of C in one file, impractical for a single g++
invocation. That output-splitting is the main scaling task.

---

## File map

| File | Role |
|---|---|
| `tools/ppu_disasm.py` | PPC64 disassembler |
| `tools/ppu_lifter.py` | PPC → C lifter (`--functions/--imports/--names`) |
| `tools/ppu_loader.py` | offline loader: image + OPD + TOC + imports |
| `tools/elf_parser.py` | ELF/SELF/PRX parsing primitives |
| `tools/nid_database.py` | NID computation (fixed) + DB tooling |
| `tools/gen_hle_nids.py` | generate NID→handler table from HLE sources |
| `runtime/ppu/ppu_loader.cpp` | runtime loader: image load, mem, registry, dispatch |
| `runtime/ppu/ppu_hle.cpp` | NID→HLE dispatch + PPC ABI adapter |
| `runtime/ppu/ppu_context.h` | `ppu_context` (also emitted into `ppu_recomp.h`) |
| `runtime/ppu/tests/test_loader.cpp` | loader smoke test (real EBOOT) |
| `runtime/ppu/tests/test_hle*.cpp` | HLE adapter + real-library integration tests |
| `runtime/ppu/tests/boot_main.cpp` | integrated first-boot harness |

## Status

| Piece | State |
|---|---|
| Offline loader (image/OPD/TOC/imports) | done, validated on real EBOOT |
| Runtime loader + dispatch | done, executes real lifted code |
| Lifter ISA coverage | ~99.8% on real code |
| NID→HLE bridge + correct NID algorithm | done, 69% import coverage |
| Integrated build + first boot | done; stops at `sys_initialize_tls` |
| Full-image lift (output splitting) | TODO (scaling) |
| HLE completeness (net/input/spurs/CRT) | incremental, mapped by NID |
| Pointer host↔guest wrappers | TODO |
