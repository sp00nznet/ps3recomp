# Getting Started with ps3recomp

Step-by-step guide to recompiling your first PS3 game.

## Prerequisites

### Required Software

- **CMake** 3.20 or later: [cmake.org](https://cmake.org/download/)
- **C/C++ compiler** with C17 and C++20 support:
  - Windows: Visual Studio 2022 with clang-cl. Plain `cl` builds the runtime library (see BUILDING.md), but a lifted game needs clang-cl (Step 6)
  - Linux: GCC or Clang
  - macOS: Xcode or the Xcode Command Line Tools (Apple Clang)
  - CI builds with the compilers on GitHub's `windows-latest`, `ubuntu-latest` and `macos-14` runner images; no older compiler version is tested.
- **Python** 3.10+ (CI uses 3.12): Required for the recompiler tools
- **Ninja**: Faster than Make/MSBuild and what the commands in this guide use
- **SDL2** on Linux and macOS: Required for the input and audio backends

On macOS, Homebrew supplies every command-line dependency not included with
Xcode:

```bash
brew install cmake ninja python@3.12 sdl2
```

Prefer not to build from source? Each [release](https://github.com/sp00nznet/ps3recomp/releases/latest) includes a prebuilt SDK bundle per platform (see the README's Downloads paragraph).

### Required Game Files

- A **decrypted PS3 EBOOT.BIN** (ELF format). You need:
  - A legally obtained copy of the game
  - The decryption keys (from your own console or PS3 firmware)
  - RPCS3's decrypted-ELF dump, or `tools/unfself.py` for an unencrypted
    debug/prototype fSELF (see Step 1)

## Installation

### 1. Clone the Repository

```bash
git clone https://github.com/sp00nznet/ps3recomp.git
cd ps3recomp
```

### 2. Create a Python Environment

macOS:

```bash
"$(brew --prefix python@3.12)/bin/python3.12" -m venv .venv
source .venv/bin/activate
python -m pip install -r requirements.txt
```

Linux:

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r requirements.txt
```

Windows PowerShell:

```powershell
py -3 -m venv .venv
.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt
```

Keep this environment active while running the `python` commands below. It
also avoids relying on a `python` command that a clean macOS installation does
not provide.

### 3. Build the Runtime Library

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

This builds `libps3recomp_runtime.a` (or `ps3recomp_runtime.lib` on Windows), which contains the memory manager, thread manager, and all HLE library stubs.

### 4. Verify the Build

```bash
# The runtime library should exist:
ls build/libps3recomp_runtime.a    # Linux/macOS
dir build\ps3recomp_runtime.lib    # Windows
```

## First Recompilation Walkthrough

The whole pipeline is four commands. `tests/run_torture.py` runs exactly this
chain end to end on every change, so it is the in-tree reference if a command
here ever stops matching the tools:

```bash
python tools/ppu_loader.py  game/EBOOT.ELF -o out/          # 1. image, OPD functions, imports
python tools/ppu_lifter.py  game/EBOOT.ELF --functions out/EBOOT.functions.json \
                            --hle-stubs out/EBOOT.imports.json -o recompiled/   # 2. lift
python tools/gen_hle_nids.py --all --out recompiled/ppu_hle_nids.cpp   # 3. HLE handler table
cmake -B build -G Ninja && cmake --build build                      # 4. build
```

Steps 2 and 3 are the two halves of firmware imports, and leaving out either
one produces the *same* startup failure. They are described in full below.

### Step 1: Get a decrypted ELF

PS3 executables ship as SELF. The recompiler needs the plain ELF inside:

- **Retail `EBOOT.BIN`** — encrypted. Dump it with RPCS3 (it writes the
  decrypted ELF to its cache when it loads a game), or decrypt it yourself with
  keys from your own console. This repository ships no keys and does no
  decryption.
- **Debug / prototype fSELF** — unencrypted, and often carries symbols. Rebuild
  the original ELF with no keys at all:

```bash
python tools/unfself.py EBOOT.BIN --output game/EBOOT.ELF
python tools/unfself.py EBOOT.BIN --info       # describe it, write nothing
```

See [PROTO_BUILDS.md](PROTO_BUILDS.md) for why a debug build is worth hunting
for: symbols make every later step checkable.

### Step 2: Load the ELF — functions, imports, TOC

```bash
python tools/ppu_loader.py game/EBOOT.ELF -o out/
```

This is stage 1 of the pipeline and it writes everything the lifter needs:

| File | Contents |
|------|----------|
| `EBOOT.functions.json` | `[{start,end,toc,opd}]` — one entry per OPD descriptor |
| `EBOOT.imports.json` | `[{library,nid,stub}]` — every firmware import and the address of its trampoline |
| `EBOOT.image.json` | PT_LOAD segment manifest (vaddr/filesz/memsz/flags) |
| `EBOOT.loader.json` | entry OPD → (code, TOC), module TOC, OPD extent, counts |

It prints a summary as it goes — entry point, module TOC (`r2`), segment layout,
function count and imports per library. Those numbers are the first sanity check
on a title: no OPD found, or a suspiciously small function count, means the
input is not what you think it is.

Function boundaries come from the `.opd` table, which lists a descriptor for
every address-taken function — far better than scanning for prologues.
`tools/find_functions.py` is the heuristic scanner for binaries where that is
not enough (it does prologue detection, leaf detection and branch-target
detection, and takes `--seed-json` to merge in what the loader found).

`tools/gen_imports.py` produces the same import list with the NIDs resolved to
names where the database knows them, which is useful to read but not required
by the lifter:

```bash
python tools/gen_imports.py game/EBOOT.ELF -o out/imports_named.json
# 1 libraries, 12 imports, 12 named (100%) -> out/imports_named.json
```

### Step 3: Create a project

```bash
cp -r templates/project/ my_game/
cd my_game/
# config.toml is reference only (no tool reads it yet); the paths go on the command lines below
# Edit CMakeLists.txt -- set the project name
```

### Step 4: Lift PPU code to C++

```bash
python ../tools/ppu_lifter.py game/EBOOT.ELF \
    --functions out/EBOOT.functions.json \
    --hle-stubs out/EBOOT.imports.json \
    --output recompiled/
```

This writes `ppu_recomp.h` plus `ppu_recomp_NNN.cpp` chunks (which can total
100 MB+ for a large title) and the `ppu_recomp_register()` the template's
`main.cpp` calls. No post-processing step is needed; the lifter emits C++
directly.

**`--hle-stubs` is not optional for a real title.** Without it the lifter
translates each firmware import trampoline literally — `li r12,0; oris r12,r12,hi;
lwz r12,lo(r12); lwz r0,0(r12); mtctr r0; bctr` — against an import table that,
in a file lifted offline, no host loader ever patched. The guest then branches to
whatever word it read, which is the next trampoline's own first instruction:

```
[ppu] unresolved indirect call -> 0x39800000 (tid=1 lr=0x000103C0)
```

`0x39800000` is not an address, it is the PowerPC instruction `li r12,0`. With
`--hle-stubs` each stub is instead emitted as its own function whose body is
`ps3_hle_call(<nid>, ctx)`, so both direct `bl` and indirect calls to it reach
the HLE handler.

### Step 4b: Lift SPU programs (only if the title has SPU code)

Skip this if the title never starts SPU programs. Otherwise the SPU ELFs are
embedded in the PPU binary (and sometimes in other decrypted files), and each
one is lifted to C separately from the PPU code. `tools/extract_spu_images.py`
finds them, `tools/spu_lifter.py` lifts one image, and
`tools/build_spu_workloads.py` runs the lifter over a directory of images and
writes the registration file:

```bash
python ../tools/extract_spu_images.py game/EBOOT.ELF --output spu/images
python ../tools/build_spu_workloads.py \
    --images spu/images --lifted spu --out spu/spu_workloads.c --constructor
```

`build_spu_workloads.py --input FILE` (repeatable) does the discovery itself,
so the first command is only needed if you want to look at the images.
`tools/lift_jobmods.py` does the same for SPURS job modules (raw code and
data blobs, not ELFs; `SPU_DUMP_MISS=<dir>` collects the ones that only exist
at run time) once they are saved as `.bin` files (`--jobmods DIR --out spu`, optionally
`--merge-chunks`); see [Game Porting Guide](GAME_PORTING_GUIDE.md#lifting-spu-programs).

**The project template can run the discovery and lift at configure time.**
`PORT_ENABLE_SPU` (default `ON`) and `PORT_SPU_INPUTS` (a semicolon-separated
list of decrypted ELF/PRX files to scan) are in `templates/project/CMakeLists.txt`;
the helper is `cmake/PS3RecompSPU.cmake`:

```bash
cmake -B build -G Ninja -DPS3RECOMP_DIR=/path/to/ps3recomp -DRECOMP_DIR=$PWD/recompiled \
    '-DPORT_SPU_INPUTS=game/EBOOT.ELF'
```

Without `PORT_SPU_INPUTS`, the template compiles any `spu/*/spu_recomp.c` it
finds, plus `spu_workloads.c`, `jobmods_register.c` and
`taskset_policy_bytes.c` when they exist.

The generated file registers each lifted image with the runtime by content
fingerprint (`spu_workload_register` / `spu_workload_register_img` in
`runtime/spu/spu_workload.c`); cellSpurs then runs the lifted code when the
game hands it a matching image. SPU code that is not lifted (for example a job
body the game DMAs into local store at run time) is not an error: it runs in
the SPU interpreter (`runtime/spu/spu_interp.c`), slower, and when it branches
to a lifted entry the lifted code resumes
([SPU_INTERPRETER_LIFTED_HANDOFF.md](SPU_INTERPRETER_LIFTED_HANDOFF.md)).
The lifter itself is documented in [SPU_LIFTER.md](SPU_LIFTER.md).

### Step 5: Generate the HLE handler table

Lifting produces the game's own code. It does **not** produce the bridge from
firmware NIDs to this runtime's HLE implementations — that is a separate
generated file:

```bash
python ../tools/gen_hle_nids.py --all --out recompiled/ppu_hle_nids.cpp
```

Compile the result into the port along with the lifted sources. It defines
`ppu_hle_register_all()`, which `ppu_hle_init()` calls at startup;
`runtime/ppu/ppu_hle.cpp` carries a weak do-nothing version so a build without
it still links, which is why the omission is not a build error. The runtime
warns at startup when it ends up with no handlers registered.

**The project template does this for you.** Its `CMakeLists.txt` generates the
every-module table at configure time unless `recompiled/ppu_hle_nids.cpp`
already exists, so this step only needs running by hand in a build system you
rolled yourself — which is exactly where it gets forgotten.

Regenerate it whenever you update ps3recomp: the table is generated from the
toolkit's registered modules, so one built against a different revision can
reference handlers this one does not have.

**The two failures look identical, and both print `0x39800000`.** No handler
table means every import returns 0; no `--hle-stubs` means the call never
reaches the table in the first place. Do both.

### Step 6: Build the Recompiled Game

```bash
cmake -B build -G Ninja -DPS3RECOMP_DIR=/path/to/ps3recomp -DRECOMP_DIR=$PWD/recompiled
cmake --build build
```

Use **clang-cl** on Windows, not `cl`. The runtime uses `__atomic_*` builtins,
`__int128` and `__builtin_bswap*`, and the lifted output uses `__int128` for the
PPC 64x64 multiplies; `cl` has none of them. clang-cl consumes the same MSVC
headers and libraries, so nothing else about the build changes:

```bash
cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
```

Lifted output also produces very large translation units, so add `/bigobj` to the
target that compiles them or the object files overflow their section limit:

```cmake
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
endif()
```

`templates/project/CMakeLists.txt` in this repository does exactly that and is the working
reference; the starter template does not set it for you.

### Step 7: Run

The game executable takes the path to the decrypted ELF as its first argument
-- it is loaded into the guest address space at startup, not baked into the
binary:

```bash
./build/my_game path/to/EBOOT.elf
```

Most ports also need to know where the title's files live, so that guest mount
points such as `/dev_hdd0` resolve to a real directory. The in-tree ports derive
that from the ELF path and accept `PS3_VFS_ROOT` as an override:

```bash
PS3_VFS_ROOT=/path/to/game-files ./build/my_game path/to/EBOOT.elf
```

You'll see:
- `[init]` messages as the runtime initializes (VM, syscalls, HLE modules)
- `[HLE]` messages as PS3 API functions are called
- `[LV2]` messages for unimplemented system calls
- A window opens for RSX output -- Direct3D 12 on Windows, Metal on macOS. See
  [Graphics backends](BUILDING.md#graphics-backends) for what each platform gets.

**Expected first-run behavior:** CRT startup executes, the game calls `cellSysmoduleLoadModule` to load libraries, then begins initialization. Missing HLE functions show as `[HLE] UNIMPLEMENTED` — add bridges in your game's `hle_modules.cpp`.

## Project Structure

After setup, your project should look like:

```
my_game/
  CMakeLists.txt          # Build configuration
  config.toml             # Reference only: not read by the tools yet
  main.cpp                # Entry point (from template)
  stubs.cpp               # Game-specific overrides
  out/                    # ppu_loader.py stage-1 output
    EBOOT.functions.json
    EBOOT.imports.json
    EBOOT.image.json
    EBOOT.loader.json
  recompiled/             # RECOMP_DIR: generated by ppu_lifter.py
    ppu_recomp.h
    ppu_recomp_000.cpp
    ppu_recomp_001.cpp
    ...
    ppu_hle_nids.cpp      # Optional, from gen_hle_nids.py (CMake generates one in build/ if absent)
  build/                  # CMake build directory
```

## Troubleshooting

### `[ppu] unresolved indirect call -> 0x39800000`

The single most common first-port failure, and it has two independent causes —
check both. `0x39800000` is the instruction `li r12,0`, the first word of a
firmware import trampoline, so the guest is calling an import and landing in
raw stub bytes:

1. The lift was run without `--hle-stubs` (Step 4), so import trampolines were
   translated as literal code instead of `ps3_hle_call`.
2. `ppu_hle_nids.cpp` was never generated or never compiled in (Step 5), so no
   handler is registered for any NID.

The runtime names whichever of the two it can see at the point of failure.

### `[HOTREAD] spinning on 0xADDR (=0xVALUE)`

The guest is polling one word of its own memory and the value is not changing —
a mutex, a flag an SPU job was supposed to set, an RSX `get` pointer. It is a
report, not a crash, and some titles legitimately spin during startup; it only
matters if nothing else advances afterwards. The address is the thing to chase:
find who is supposed to write it.

### "Unimplemented NID 0xXXXXXXXX in module YYY"

The game is calling a PS3 API function that does not have an HLE stub. Options:
1. Check if the function is trivial (e.g., returns CELL_OK) and add a stub in `stubs.cpp`.
2. Look up the NID: `python tools/nid_database.py --lookup 0xXXXXXXXX`.
3. Check RPCS3's source for reference implementations.

### `/* sync: cache/sync — no-op */` in the lifted output

Expected. `sync`, `lwsync`, `isync`, `eieio` and the `dcb*` cache hints have no
host equivalent worth emitting: the lifted code runs on a host with a stronger
memory model, and the ordering that actually matters — `lwarx`/`stwcx.` — is
lifted to real host atomics rather than to fences. A spin loop around
`lwarx/stwcx.` that never exits is not this comment's fault; it means the value
the loop waits on is never written (see `[HOTREAD]` above).

### Crash on startup / segfault

- Run under a debugger. (`break_on_unimplemented` in `config.toml` has no effect yet: nothing reads that file.)
- Check that the ELF was properly decrypted (wrong keys produce garbage code).
- Verify memory layout: some games require specific address ranges.

### Graphics not rendering

- The RSX command processor tracks GPU state but needs a rendering backend.
- Start with the **null backend** (`rsx_null_backend_init()`) to verify command flow.
- Windows uses D3D12, macOS uses Metal, and Linux's Vulkan backend is opt-in.
- On macOS, verify Metal without a game:
  `PS3RECOMP_METAL_HEADLESS=1 ./build/ps3recomp_host --draw`.
- See [RSX_GRAPHICS.md](RSX_GRAPHICS.md) for the full graphics architecture.

### Audio crackling or silence

The `config.toml` settings are reference only: nothing reads that file yet.
Windows uses WASAPI; Linux and macOS use SDL2. Check SDL's selected device and
driver before changing guest-side audio code.

### Recompiler fails on indirect branches

- Some indirect branches (function pointers, virtual calls, switch tables) cannot be resolved statically.
- The recompiler emits a runtime dispatch table for these cases.
- If a jump target is missing, add it manually to the function table.

### Build errors in generated code

- Ensure your compiler supports C17 and C++20.
- On Windows use **clang-cl**, not `cl`: the runtime and the lifted output use
  `__atomic_*`, `__int128` and `__builtin_bswap*`, none of which MSVC has. See
  Step 6.
- Lifted translation units are large enough to need `/bigobj`.

---

## Next Steps

Once you have a basic build working:

- **[Game Porting Guide](GAME_PORTING_GUIDE.md)** — Full 12-phase walkthrough with [flOw case study](GAME_PORTING_GUIDE.md#case-study-flow)
- **[Custom Modules](CUSTOM_MODULES.md)** — How to write new HLE modules
- **[RSX Graphics](RSX_GRAPHICS.md)** — RSX command processor, D3D12/Vulkan backends, shader translation
- **[FAQ & Troubleshooting](FAQ.md)** — Common issues and solutions
- **[Contributing](../CONTRIBUTING.md)** — How to contribute code and docs

Reference docs:
- **[Runtime](RUNTIME.md)** — VM, PPU/SPU contexts, types, syscalls
- **[Syscalls](SYSCALLS.md)** — LV2 kernel syscall implementations
- **[NID System](NID_SYSTEM.md)** — PS3 function linking and NID resolution
- **[Module Reference](MODULES_REFERENCE.md)** — All 95 HLE modules documented
- **[Module Status](MODULE_STATUS.md)** — Quick coverage check
- **[Tools](TOOLS.md)** — Recompiler pipeline tools reference
- **[Building](BUILDING.md)** — Build system and compiler notes
- **[Architecture](ARCHITECTURE.md)** — Cell processor and pipeline overview
- **[Platform Abstraction](PLATFORM_ABSTRACTION.md)** — Win32/POSIX cross-platform details
