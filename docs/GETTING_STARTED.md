# Getting Started with ps3recomp

Step-by-step guide to recompiling your first PS3 game.

## Prerequisites

### Required Software

- **CMake** 3.20 or later: [cmake.org](https://cmake.org/download/)
- **C/C++ compiler** with C17 and C++20 support:
  - Windows: Visual Studio 2022 (MSVC 17+) or Clang 14+
  - Linux: GCC 12+ or Clang 14+
  - macOS: Xcode 14+ (Apple Clang)
- **Python** 3.9+: Required for the recompiler tools
- **Ninja** (recommended): Faster than Make/MSBuild -- `pip install ninja`

### Required Game Files

- A **decrypted PS3 EBOOT.BIN** (ELF format). You need:
  - A legally obtained copy of the game
  - The decryption keys (from your own console or PS3 firmware)
  - RPCS3's decrypted-ELF dump, or `tools/unfself.py` for an unencrypted
    debug/prototype fSELF (see Step 1)

### Python Dependencies

```bash
pip install -r tools/requirements.txt
```

## Installation

### 1. Clone the Repository

```bash
git clone https://github.com/sp00nznet/ps3recomp.git
cd ps3recomp
```

### 2. Build the Runtime Library

```bash
mkdir build && cd build
cmake .. -G Ninja
ninja
```

This builds `libps3recomp_runtime.a` (or `ps3recomp_runtime.lib` on Windows), which contains the memory manager, thread manager, and all HLE library stubs.

### 3. Verify the Build

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
                            --hle-stubs out/EBOOT.imports.json -o src/recomp/   # 2. lift
python tools/gen_hle_nids.py --all --out src/gen/ppu_hle_nids.cpp   # 3. HLE handler table
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
# Edit config.toml -- set the elf_path
# Edit CMakeLists.txt -- set the project name
```

### Step 4: Lift PPU code to C++

```bash
python ../tools/ppu_lifter.py game/EBOOT.ELF \
    --functions out/EBOOT.functions.json \
    --hle-stubs out/EBOOT.imports.json \
    --output src/recomp/
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

### Step 5: Generate the HLE handler table

Lifting produces the game's own code. It does **not** produce the bridge from
firmware NIDs to this runtime's HLE implementations — that is a separate
generated file:

```bash
python ../tools/gen_hle_nids.py --all --out src/gen/ppu_hle_nids.cpp
```

Compile the result into the port along with the lifted sources. It defines
`ppu_hle_register_all()`, which `ppu_hle_init()` calls at startup;
`runtime/ppu/ppu_hle.cpp` carries a weak do-nothing version so a build without
it still links, which is why the omission is not a build error. The runtime
warns at startup when it ends up with no handlers registered.

**The project template does this for you.** Its `CMakeLists.txt` generates the
every-module table at configure time unless `src/recomp/ppu_hle_nids.cpp`
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
cmake -B build -G Ninja -DPS3RECOMP_DIR=/path/to/ps3recomp
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

`lbp/CMakeLists.txt` in this repository does exactly that and is the working
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
  config.toml             # Recompiler settings
  main.cpp                # Entry point (from template)
  stubs.cpp               # Game-specific overrides
  out/                    # ppu_loader.py stage-1 output
    EBOOT.functions.json
    EBOOT.imports.json
    EBOOT.image.json
    EBOOT.loader.json
  src/
    recomp/               # Generated by ppu_lifter.py
      ppu_recomp.h
      ppu_recomp_000.cpp
      ppu_recomp_001.cpp
      ...
    gen/
      ppu_hle_nids.cpp    # Generated by gen_hle_nids.py
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

- Enable `break_on_unimplemented = true` in `config.toml` and run under a debugger.
- Check that the ELF was properly decrypted (wrong keys produce garbage code).
- Verify memory layout: some games require specific address ranges.

### Graphics not rendering

- The RSX command processor tracks GPU state but needs a rendering backend.
- Start with the **null backend** (`rsx_null_backend_init()`) to verify command flow.
- Switch to the **D3D12 backend** (`rsx_d3d12_backend_init()`) for real GPU rendering.
- See [RSX_GRAPHICS.md](RSX_GRAPHICS.md) for the full graphics architecture.

### Audio crackling or silence

- Check that `cellAudio` is set to `"hle"` in modules config.
- Verify the audio backend matches your OS (`wasapi` for Windows, `pulseaudio` for Linux).
- Increase `buffer_size` to reduce crackling (at the cost of latency).

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
