# Recompiler Tools Reference

Complete documentation for the Python-based recompilation pipeline tools in `tools/`.

---

## Table of Contents

1. [Pipeline Overview](#pipeline-overview)
2. [ppu_loader.py — image, OPD functions, imports](#ppu_loaderpy)
3. [elf_parser.py — ELF/SELF/PRX Analysis](#elf_parserpy)
4. [ppu_disasm.py — PowerPC Disassembler](#ppu_disasmpy)
5. [ppu_lifter.py — PPU → C Code Generator](#ppu_lifterpy)
6. [spu_disasm.py — SPU Disassembler](#spu_disasmpy)
7. [find_functions.py — Function Boundary Detection](#find_functionspy)
8. [nid_database.py — NID Resolver](#nid_databasepy)
9. [prx_analyzer.py — Module Dependency Analysis](#prx_analyzerpy)
10. [generate_stubs.py — HLE Stub Generator](#generate_stubspy)
11. [gen_imports.py / gen_hle_nids.py — firmware imports](#gen_importspy--gen_hle_nidspy)
12. [Dependencies](#dependencies)

---

## Pipeline Overview

The recompiler pipeline transforms a PS3 executable into native C++ source in four stages:

```
  EBOOT.BIN (SELF)  --unfself.py / RPCS3 dump-->  EBOOT.ELF
       │
       ▼
  ┌──────────────────┐
  │ ppu_loader.py    │ ← Stage 1: image, OPD function table, TOC, imports
  └──────┬───────────┘
         │  EBOOT.functions.json, EBOOT.imports.json, .image.json, .loader.json
         ▼
  ┌──────────────────┐
  │ ppu_lifter.py    │ ← Stage 2: lift to C++ (--functions, --hle-stubs)
  └──────┬───────────┘
         │  ppu_recomp_NNN.cpp + ppu_recomp.h
         ▼
  ┌──────────────────┐
  │ gen_hle_nids.py  │ ← Stage 3: NID → HLE handler table for this runtime
  └──────┬───────────┘
         │  ppu_hle_nids.cpp
         ▼
  ┌──────────────────┐
  │ CMake + compiler │ ← Stage 4: compile against ps3recomp_runtime
  └──────┬───────────┘
         ▼
       🎮 Run!
```

`ppu_disasm.py`, `find_functions.py`, `elf_parser.py` and `prx_analyzer.py` are
analysis tools around that spine, not steps in it: you reach for them when the
loader's output does not explain what a title is doing.

Each stage can be run independently, allowing incremental development and debugging.

---

## ppu_loader.py

Stage 1. Turns a decrypted PPU ELF (ET_EXEC EBOOT or ET_DYN PRX) into the data
the lifter and the runtime need, all from the binary's own tables rather than
from heuristics.

### What It Does

1. **Segment manifest** — which PT_LOAD segments go where, including zero-filled
   BSS (`memsz > filesz`)
2. **OPD function table** — PS3 function pointers are addresses of 8-byte
   `{code, toc}` descriptors, so the `.opd` section is an authoritative list of
   every address-taken function, and of the module TOC (`r2`)
3. **Entry point** — `e_entry` is itself an OPD address; resolved to (code, TOC)
4. **Firmware imports** — walks `sys_proc_prx_param` (an ET_EXEC EBOOT has no
   dynamic section, so `prx_analyzer` finds nothing there) for every `.lib.stub`
   trampoline address and its NID

### Usage

```bash
python tools/ppu_loader.py game/EBOOT.ELF -o out/
```

### Output Files

| File | Contents |
|------|----------|
| `<name>.functions.json` | `[{start,end,toc,opd}]` — feeds `ppu_lifter --functions` |
| `<name>.imports.json` | `[{library,nid,stub}]` — feeds `ppu_lifter --hle-stubs` |
| `<name>.image.json` | `[{vaddr,filesz,memsz,flags,file_offset}]` |
| `<name>.loader.json` | entry OPD → (code, TOC), module TOC, OPD extent, counts |

It also prints a summary — entry, TOC, OPD extent, function count, imports per
library — which is the first check on whether the input is what you think.

---


## elf_parser.py

**Size:** ~31 KB
**Purpose:** Parse PS3 ELF, SELF, and PRX binary files

### What It Does

1. **SELF decryption** — handles the Sony SELF container format (if keys are provided)
2. **ELF header parsing** — reads the ELF64 header, program headers, and section headers
3. **Segment extraction** — identifies `.text` (code), `.data`, `.rodata`, `.bss` segments
4. **Import/export table parsing** — extracts NID tables from PRX-style import/export stubs
5. **Relocation processing** — reads relocation entries for position-dependent code
6. **Symbol table extraction** — if present, extracts symbol names and addresses
7. **Metadata output** — produces a structured JSON report of the binary

### Usage

```bash
# Header, entry point, architecture (JSON on stdout — redirect it)
python tools/elf_parser.py path/to/EBOOT.ELF

# One report at a time, or everything
python tools/elf_parser.py path/to/EBOOT.ELF --imports  > analysis/imports.json
python tools/elf_parser.py path/to/EBOOT.ELF --exports  > analysis/exports.json
python tools/elf_parser.py path/to/EBOOT.ELF --all      > analysis/elf_info.json
```

### Report Sections

Everything goes to **stdout** as one JSON document; the names below are the
conventional files to redirect each report into.

| File | Contents |
|------|----------|
| `elf_info.json` | Header fields, entry point, architecture flags |
| `segments.json` | All program header entries with addresses and sizes |
| `sections.json` | All section header entries |
| `imports.json` | `[{module, nid, name (if known)}, ...]` |
| `exports.json` | `[{nid, name, address}, ...]` |
| `relocations.json` | `[{offset, type, symbol, addend}, ...]` |
| `symbols.json` | `[{name, address, size, type}, ...]` |

### Key Data Structures

**ELF Header fields parsed:**
- `e_type`: Executable (ET_EXEC=2) or PRX (special PS3 type)
- `e_machine`: PowerPC64 (EM_PPC64=21) or SPU (EM_SPU=23)
- `e_entry`: Entry point address
- `e_phnum`: Number of program headers (segments)
- `e_shnum`: Number of section headers

**PS3-specific extensions:**
- PRX-style import/export stubs use a custom ELF note format
- SELF containers add encryption and signature layers around the ELF
- SCE header contains metadata about required firmware version, flags, etc.

---

## ppu_disasm.py

**Size:** ~34 KB
**Purpose:** Disassemble PowerPC 64-bit machine code with PS3-specific extensions

### Instruction Set Coverage

| Category | Instructions | Notes |
|----------|-------------|-------|
| **Integer arithmetic** | `add`, `addi`, `addis`, `sub`, `subf`, `mullw`, `divw`, `neg` | With carry/overflow variants (`addc`, `addo`, etc.) |
| **Logical** | `and`, `or`, `xor`, `nand`, `nor`, `andc`, `orc` | With immediate variants (`andi.`, `ori`, `xori`) |
| **Shift/rotate** | `slw`, `srw`, `sraw`, `rlwinm`, `rlwimi`, `rlwnm` | 64-bit: `sld`, `srd`, `srad`, `rldimi`, etc. |
| **Compare** | `cmp`, `cmpi`, `cmpl`, `cmpli` | Word and doubleword, signed and unsigned |
| **Branch** | `b`, `bl`, `bc`, `bcl`, `blr`, `bctr`, `bctrl` | Conditional: `beq`, `bne`, `blt`, `bgt`, `ble`, `bge` |
| **Load** | `lbz`, `lhz`, `lha`, `lwz`, `lwa`, `ld` | Indexed: `lbzx`, `lhzx`, `lwzx`, `ldx` |
| **Store** | `stb`, `sth`, `stw`, `std` | Indexed: `stbx`, `sthx`, `stwx`, `stdx` |
| **Load/store update** | `lbzu`, `lhzu`, `lwzu`, `ldu`, `stbu`, `sthu`, `stwu`, `stdu` | Pre-increment addressing |
| **Load/store multiple** | `lmw`, `stmw` | Load/store r(n) through r31 |
| **String** | `lswi`, `lswx`, `stswi`, `stswx` | Byte string operations |
| **Floating-point** | `lfs`, `lfd`, `stfs`, `stfd`, `fadd`, `fsub`, `fmul`, `fdiv`, `fmadd`, `fmsub`, `fabs`, `fneg`, `fcmp` | Single and double precision |
| **VMX/AltiVec** | `lvx`, `stvx`, `vaddfp`, `vsubfp`, `vmaddfp`, `vcmpgtfp`, `vperm`, `vsel`, `vsldoi`, `vmrghw`, `vmrglw` | 128-bit SIMD vectors |
| **System** | `sc`, `mflr`, `mtlr`, `mfcr`, `mtcrf`, `mfspr`, `mtspr`, `eieio`, `isync`, `sync` | Privileged and supervisor |
| **Atomic** | `lwarx`, `stwcx.`, `ldarx`, `stdcx.` | Load-linked / store-conditional |
| **CR manipulation** | `crand`, `cror`, `crxor`, `crnand`, `crnor`, `creqv`, `crandc`, `mcrf` | Condition register logic |

### Usage

```bash
# Disassemble an ELF
python tools/ppu_disasm.py path/to/EBOOT.ELF --output disasm/

# Disassemble a specific address range
python tools/ppu_disasm.py path/to/EBOOT.ELF --start 0x10000 --end 0x20000

# Generate annotated listing
python tools/ppu_disasm.py path/to/EBOOT.ELF --annotate --output disasm/

# Output raw instruction list (for machine processing)
python tools/ppu_disasm.py path/to/EBOOT.ELF --format json --output disasm/
```

### Output Format

```
0x00010000: 7C0802A6   mflr    r0              ; save link register
0x00010004: F8010010   std     r0, 0x10(r1)    ; store to stack
0x00010008: FBE1FFF8   std     r31, -8(r1)     ; save r31
0x0001000C: F821FF71   stdu    r1, -0x90(r1)   ; allocate stack frame
0x00010010: 7C3F0B78   mr      r31, r1         ; frame pointer
0x00010014: 4BFFFFE5   bl      0x0000FFF8      ; call sub_FFF8
```

### Control Flow Analysis

The disassembler performs basic block analysis:
- Identifies function boundaries from `blr` returns and call graph
- Detects branch targets and labels
- Classifies branches as direct, conditional, or indirect
- Recognizes jump table patterns (`mtctr` + `bctr` with `r12` load)

---

## ppu_lifter.py

**Size:** ~30 KB
**Purpose:** Translate disassembled PowerPC instructions into C source code

### Translation Strategy

Each PowerPC instruction maps to one or more C statements. The lifter operates directly on the `ppu_context` struct — registers are accessed via `ctx->gpr[N]`, `ctx->lr`, `ctx->cr`, etc.:

```c
void func_00010000(ppu_context* ctx) {
    // mflr r0
    ctx->gpr[0] = ctx->lr;

    // std r0, 0x10(r1)
    vm_write64(ctx->gpr[1] + 0x10, ctx->gpr[0]);

    // stdu r1, -0x90(r1)
    vm_write64(ctx->gpr[1] + -0x90, ctx->gpr[1]);
    ctx->gpr[1] += -0x90;

    // bl func_00010200
    func_00010200(ctx);

    // blr
    return;
}
```

Functions are named `func_XXXXXXXX` where `XXXXXXXX` is the guest address in hex. Internal branches become `goto` labels; external branches become function calls.

### Important Lifter Behaviors

**Split-function trampoline:** When a function ends without `blr`/`b`, or when a branch targets a different split fragment, the lifter emits a trampoline pattern instead of a direct call:
```c
{ g_trampoline_fn = (void(*)(void*))func_NEXT; return; }
```
The preamble declares `g_trampoline_fn` as `extern "C" __declspec(thread)` to match the TLS definition in the game project's `indirect_dispatch.cpp`. The game project must define `g_trampoline_fn` as `__declspec(thread)` and add `DRAIN_TRAMPOLINE(ctx)` after every `bl` (function call) in the generated code. This converts recursive split-function chains into iterative loops, preventing both native and guest stack overflow from backward branches across fragment boundaries.

**Indirect calls (bctrl):** Emitted as `ps3_indirect_call(ctx)` which dispatches through a hash table mapping guest addresses to host function pointers. The game project must provide this function. It also handles OPD (Official Procedure Descriptor) resolution.

**TOC save:** The lifter does NOT emit `std r2, 40(r1)` before import stub calls (the PS3 linker normally inserts this). Game projects must save the TOC in their `nid_dispatch()` function.

**VMX/AltiVec:** Vector instructions operate on `ctx->vr[N]` (128-bit registers). Vector loads/stores access guest memory via `vm_base + addr` with 16-byte alignment. Float operations treat each register as 4×float32.

### Key Translations

| PowerPC | C Output | Notes |
|---------|----------|-------|
| `add r3, r4, r5` | `ctx->gpr[3] = ctx->gpr[4] + ctx->gpr[5];` | — |
| `addi r3, r4, 100` | `ctx->gpr[3] = (int64_t)(int32_t)(ctx->gpr[4] + 100);` | Sign-extended |
| `lwz r3, 0x10(r4)` | `ctx->gpr[3] = vm_read32(ctx->gpr[4] + 0x10);` | Big-endian load |
| `stw r3, 0x10(r4)` | `vm_write32(ctx->gpr[4] + 0x10, (uint32_t)ctx->gpr[3]);` | Big-endian store |
| `bl 0x10200` | `func_00010200(ctx);` | Direct function call |
| `bctrl` | `ps3_indirect_call(ctx);` | Indirect call via hash table |
| `blr` | `return;` | Function return |
| `cmpwi cr0, r3, 0` | `{ int64_t a = ...; int64_t b = ...; cr_val = ...; }` | Inline CR update |
| `beq target` | `if (((ctx->cr >> shift) & 2)) goto loc_XXXX;` | CR EQ bit check |
| `sc` | `lv2_syscall(ctx);` | System call dispatch |
| `lwarx r3, 0, r4` | `ctx->gpr[3] = vm_read32(ea); ctx->reserve_addr = ea;` | Atomic reservation |
| `stwcx. r3, 0, r4` | `if (reserve_addr == ea) { store; CR0=EQ; } else { CR0=0; }` | Conditional store |
| `lvx v3, r4, r5` | `memcpy(&ctx->vr[3], vm_base + (ea & ~0xF), 16);` | VMX vector load |
| `vmaddfp v3, v4, v5, v6` | `d[i] = a[i] * c[i] + b[i]; (4× float)` | VMX multiply-add |
| `vperm v3, v4, v5, v6` | Byte permutation across two source vectors | VMX shuffle |

### Instruction Coverage

The lifter handles **100+ instruction mnemonics** across these categories:

| Category | Instructions | Count |
|----------|------------|-------|
| Integer arithmetic | add, addi, addis, subf, neg, mulld, divd, adde, addze, ... | ~25 |
| Integer logical | and, or, xor, nand, nor, andc, orc, extsb, extsh, ... | ~15 |
| Loads/stores | lwz, lbz, lhz, ld, stw, stb, sth, std, lwa, lwbrx, ... | ~30 |
| Branches | b, bl, blr, bctrl, bctr, beq, bne, blt, bgt, ... | ~20 |
| Rotate/shift | rlwinm, rlwimi, rldicl, rldicr, rldic, rldimi, rldcl, rlwnm, ... | ~10 |
| Compare | cmpw, cmpd, cmplw, cmpld, cmpwi, cmplwi, ... | ~8 |
| CR logical | cror, crand, crnand, crxor, crnor, creqv, mcrf | 7 |
| FP arithmetic | fadd, fsub, fmul, fdiv, fmadd, fmsub, fneg, fabs, ... | ~20 |
| FP convert | fcfid, fctid, fctiw, frsp, fsel, fsqrt, fres, ... | ~10 |
| VMX load/store | lvx, stvx, lvebx, stvebx, lvsl, lvsr, ... | ~12 |
| VMX float | vmaddfp, vnmsubfp, vaddfp, vsubfp, vmaxfp, vrefp, vrsqrtefp | 7 |
| VMX integer | vaddubm-uwm, vsububm-uwm, vmax/vmin (all types), ... | ~30 |
| VMX logical | vand, vandc, vor, vxor, vnor, vsel, vperm, vsldoi | 8 |
| VMX compare | vcmpeqfp, vcmpgefp, vcmpgtfp, vcmpbfp, vcmpequb-w, ... | ~12 |
| VMX convert | vcfsx, vcfux, vctsxs, vctuxs | 4 |
| VMX misc | vspltw, vspltisb/h/w, vmrghb-w, vmrglb-w, vmsumshm | ~10 |
| Atomics | lwarx, stwcx., ldarx, stdcx. | 4 |
| System | sc, tw, mflr, mtlr, mfcr, mtcrf, mfctr, mtctr, mftb | ~10 |
| Cache/sync | dcbt, dcbf, sync, eieio, isync (all no-ops) | ~10 |

### Memory Access Macros

```c
vm_read8(addr)      // Load unsigned byte
vm_read16(addr)     // Load unsigned halfword (big-endian)
vm_read32(addr)     // Load unsigned word (big-endian)
vm_read64(addr)     // Load unsigned doubleword (big-endian)
vm_write8(addr, v)  // Store byte
vm_write16(addr, v) // Store halfword (big-endian)
vm_write32(addr, v) // Store word (big-endian)
vm_write64(addr, v) // Store doubleword (big-endian)
```

### VMX/AltiVec Translation

Vector instructions map to operations on `u128` union members:

```c
// vaddfp vr3, vr4, vr5  (vector add float, 4 lanes)
for (int i = 0; i < 4; i++)
    vr3._f32[i] = vr4._f32[i] + vr5._f32[i];

// vperm vr3, vr4, vr5, vr6  (vector permute)
// Uses the low 5 bits of each byte in vr6 to select bytes from vr4:vr5
```

### Branch and Control Flow

The lifter handles several control flow patterns:

1. **Direct branches** → `goto label;` within the function
2. **Direct calls** (`bl`) → `recomp_func_ADDR(ctx);`
3. **Conditional branches** → `if (cr_field & condition) goto label;`
4. **Indirect calls** (`bctrl`) → `recomp_dispatch(ctx, ctr);` (runtime dispatch)
5. **Switch tables** → `switch (index) { case 0: goto label0; ... }`
6. **Return** (`blr`) → `return;` with register writeback

### Usage

```bash
# Lift an ELF, with the loader's function table and import list
python tools/ppu_lifter.py game/EBOOT.ELF \
    --functions out/EBOOT.functions.json \
    --hle-stubs out/EBOOT.imports.json \
    --output src/recomp/

# One translation unit instead of chunks (small titles and tests only)
python tools/ppu_lifter.py game/EBOOT.ELF --functions out/EBOOT.functions.json \
    --single-file --output src/recomp/
```

### Output Files

| File | Contents |
|------|----------|
| `ppu_recomp_NNN.cpp` | Recompiled functions, batched across translation units |
| `ppu_recomp.h` | Declarations, VM access helpers, and `ppu_recomp_register()` |

---

## spu_disasm.py

**Size:** ~18 KB
**Purpose:** Disassemble SPU (Synergistic Processing Unit) machine code

### SPU ISA

The SPU has its own instruction set, completely different from PowerPC:

| Category | Example Instructions |
|----------|---------------------|
| **Integer arithmetic** | `a` (add word), `ah` (add halfword), `ai` (add immediate), `sf` (subtract), `mpya`, `mpyh` |
| **Logical** | `and`, `or`, `xor`, `nand`, `nor`, `andc`, `orc` |
| **Shift/rotate** | `shl`, `shlh`, `shlhi`, `rot`, `roth`, `rothi`, `rotqbi`, `shlqbi` |
| **Compare** | `ceq`, `ceqh`, `ceqb`, `cgt`, `cgth`, `cgtb`, `clgt`, `clgth` |
| **Branch** | `br`, `bra`, `brsl`, `brasl`, `bi`, `bisl`, `brnz`, `brz`, `brhz`, `brhnz` |
| **Load/store** | `lqd`, `lqx`, `lqa`, `lqr`, `stqd`, `stqx`, `stqa`, `stqr` (all 128-bit quadword) |
| **Select/shuffle** | `selb`, `shufb`, `cbd`, `chd`, `cwd`, `cdd` |
| **Floating-point** | `fa`, `fs`, `fm`, `fma`, `fms`, `fnms`, `fcgt`, `fceq` |
| **Channel** | `rdch`, `wrch`, `rchcnt` |
| **Hint** | `hbr`, `hbrr`, `hbra` (branch hint for instruction prefetch) |
| **Control** | `stop`, `stopd`, `nop`, `lnop`, `iret` |

### Key Differences from PPU

- **All registers are 128-bit** — there are no scalar registers
- **All loads/stores are 128-bit aligned** — no byte/halfword/word memory access (use shuffle to extract)
- **No condition register** — comparisons produce vector masks, branches read the preferred slot
- **Even/odd pipeline** — instructions are dual-issued in pairs
- **No cache** — local store is directly addressed, no cache misses

### Usage

```bash
# Disassemble SPU ELF segment
python tools/spu_disasm.py spu_program.elf --output spu_disasm/

# Disassemble raw SPU binary
python tools/spu_disasm.py spu_code.bin --raw --base 0x0 --output spu_disasm/
```

---

## find_functions.py

**Purpose:** Detect function boundaries in PowerPC code

### Detection Methods

1. **Symbol table** — if the ELF has symbols, each `STT_FUNC` entry defines a function
2. **`blr` return instructions** — mark function endings
3. **`bl` call targets** — the target of a branch-and-link is a function entry
4. **Prologue patterns** — detect common function prologues:
   ```
   mflr    r0          ; Save link register
   stw     r0, N(r1)   ; Store to stack
   stwu    r1, -M(r1)  ; Allocate stack frame
   ```
5. **Call graph analysis** — walk all `bl` targets transitively to find complete function set
6. **Alignment gaps** — functions are often aligned to 4 or 16 bytes; padding with `nop` between them

### Usage

```bash
python tools/find_functions.py path/to/EBOOT.ELF --output functions.json
```

### Output Format

```json
[
    {
        "address": "0x00010000",
        "end": "0x00010120",
        "size": 288,
        "name": "main",
        "source": "symbol_table",
        "calls": ["0x00010200", "0x00010400"],
        "called_by": []
    },
    ...
]
```

---

## nid_database.py

**Size:** ~22 KB
**Purpose:** NID lookup, computation, and database management

### Features

- **2000+ NID-to-name mappings** from RPCS3 and Sony SDK
- **Forward lookup**: name → NID (SHA-1 computation)
- **Reverse lookup**: NID → name (database search)
- **Module classification**: which module owns each NID
- **Batch processing**: resolve all NIDs from an ELF analysis

### Usage

```bash
# Look up a single NID
python tools/nid_database.py --lookup 0x718BF5F8
# → cellFsOpen (module: cellFs)

# Compute NID from function name
python tools/nid_database.py --compute cellFsOpen
# → 0x718BF5F8

# List all NIDs for a module
python tools/nid_database.py --module cellFs

# Resolve all imports from an ELF analysis
python tools/nid_database.py --resolve analysis/imports.json --output resolved.json

# Search for NIDs matching a pattern
python tools/nid_database.py --search "cellFs*"

# Export the full database
python tools/nid_database.py --export-all --format json > nid_db.json
```

### Database Format

The NID database is stored as a JSON file with entries organized by module:

```json
{
    "cellFs": {
        "0x718BF5F8": "cellFsOpen",
        "0x4D5FF8E2": "cellFsRead",
        "0x6D3BB15B": "cellFsWrite",
        "0x8ACAC8B6": "cellFsClose"
    },
    "cellPad": {
        "0x8B72CDA1": "cellPadInit",
        "0xBE5BE3BA": "cellPadGetData"
    }
}
```

---

## prx_analyzer.py

**Purpose:** Analyze PRX module dependencies and build dependency graphs

### Features

1. **Import analysis** — which modules a PRX imports from
2. **Export analysis** — which functions/variables a PRX exports
3. **Dependency graph** — build a directed graph of module dependencies
4. **Load order** — determine the correct order to load modules (topological sort)
5. **Coverage report** — show which imports have HLE implementations

### Usage

```bash
# Analyze a single PRX
python tools/prx_analyzer.py path/to/module.prx --info

# Build dependency graph for a game's modules
python tools/prx_analyzer.py game/modules/ --graph --output deps.dot

# Show import coverage against ps3recomp HLE
python tools/prx_analyzer.py game/EBOOT.ELF --coverage
```

### Output Example

```
Module: EBOOT.ELF
  Imports from cellFs: 12 functions (12/12 implemented = 100%)
  Imports from cellPad: 5 functions (5/5 implemented = 100%)
  Imports from cellGcmSys: 27 functions (27/27 implemented = 100%)
  Imports from cellSpurs: 15 functions (12/15 implemented = 80%)
  ---
  Total: 139/140 imports resolved (99.3%)
  Missing: 1 function from cellFoo (NID 0xABCDEF01)
```

---

## generate_stubs.py

**Purpose:** Auto-generate HLE stub implementations from RPCS3's module source code

### What It Does

1. Reads RPCS3's C++ HLE module files (e.g., `rpcs3/Emu/Cell/Modules/cellFs.cpp`)
2. Extracts function signatures, NID registrations, and error code patterns
3. Generates standalone C stubs compatible with ps3recomp's module framework
4. Produces a header file with all function declarations
5. Generates a registration block (`DECLARE_PS3_MODULE` + `REGISTER_FUNC`)

### Usage

```bash
# Generate stubs from an RPCS3 module file
python tools/generate_stubs.py \
    --input rpcs3/Emu/Cell/Modules/cellFs.cpp \
    --output libs/filesystem/cellFs.c

# Generate stubs for all modules in a directory
python tools/generate_stubs.py \
    --input-dir rpcs3/Emu/Cell/Modules/ \
    --output-dir libs/

# Generate only the header
python tools/generate_stubs.py \
    --input rpcs3/Emu/Cell/Modules/cellPad.cpp \
    --header-only \
    --output libs/input/cellPad.h
```

### Output Structure

For each module, the generator produces:

**Header (`.h`):**
```c
#ifndef CELLFS_H
#define CELLFS_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

int32_t cellFsOpen(/* params */);
int32_t cellFsRead(/* params */);
// ...

#endif
```

**Implementation (`.c`):**
```c
#include "cellFs.h"
#include "ps3emu/module.h"

int32_t cellFsOpen(/* params */)
{
    // TODO: implement
    return CELL_OK;
}

// ...

DECLARE_PS3_MODULE(cellFs, "cellFs")
{
    REGISTER_FUNC(cellFsOpen);
    REGISTER_FUNC(cellFsRead);
    // ...
}
```

---

## gen_imports.py / gen_hle_nids.py

The two halves of firmware imports. Omit either and the port builds, starts, and
dies on its first firmware call with `[ppu] unresolved indirect call ->
0x39800000` — which is not an address but the instruction `li r12,0`, the first
word of an import trampoline.

**`gen_imports.py`** writes the same import list `ppu_loader.py` already emits,
with NIDs resolved to names where `nid_database` knows them. Use it when you
want to read the list; the lifter is happy with either file.

```bash
python tools/gen_imports.py game/EBOOT.ELF -o out/imports_named.json
# 1 libraries, 12 imports, 12 named (100%) -> out/imports_named.json
```

Feeding that list to `ppu_lifter.py --hle-stubs` makes each import stub its own
function whose body is `ps3_hle_call(<nid>, ctx)`, instead of the trampoline's
literal instructions.

**`gen_hle_nids.py`** generates the other side: the table that maps those NIDs
to this runtime's HLE implementations.

```bash
python tools/gen_hle_nids.py --all --out src/gen/ppu_hle_nids.cpp   # every module
python tools/gen_hle_nids.py sysPrxForUser --out src/gen/ppu_hle_nids.cpp
```

It defines `ppu_hle_register_all()`, which `ppu_hle_init()` calls at startup.
`runtime/ppu/ppu_hle.cpp` carries a **weak** empty version, so a port that never
compiles this file still links — regenerate it after every toolkit update, since
the table names handlers from the revision it was generated against.

---


## Dependencies

### Python Requirements

Listed in `tools/requirements.txt`:

| Package | Purpose |
|---------|---------|
| `pyelftools` | ELF binary parsing |
| `capstone` | Disassembly engine (used alongside custom decoders) |
| `pycryptodome` | AES/SHA for SELF decryption |
| `toml` | TOML configuration file parsing |
| `networkx` | Dependency graph construction (optional) |

### Installation

```bash
pip install -r tools/requirements.txt
```

### Python Version

Python 3.9+ required (uses `match/case` in some tools, f-strings throughout).
