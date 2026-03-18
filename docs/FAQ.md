# Frequently Asked Questions

---

## General

### What is ps3recomp?

ps3recomp is a toolkit for **statically recompiling** PlayStation 3 games into native PC executables. Instead of emulating the PS3's Cell processor at runtime (like RPCS3), we translate the PowerPC assembly into C/C++ ahead of time, compile it with a standard compiler, and link it against a runtime library that provides PS3 system services.

### How is this different from RPCS3?

| | RPCS3 | ps3recomp |
|---|---|---|
| **Approach** | Dynamic emulation | Static recompilation |
| **Translation** | At runtime (JIT) | Ahead of time (to C/C++) |
| **Performance** | Good, but CPU-intensive | Native speed — no translation overhead |
| **Compatibility** | ~70% of library (one binary, many games) | Per-game project (deep but narrow) |
| **Moddability** | Limited | Full — it's just C/C++ you can edit |
| **Hardware requirements** | High-end PC recommended | Mid-range PC is fine |

Both approaches have value. RPCS3 is better for broad compatibility; ps3recomp is better for native ports and preservation.

### Is this legal?

Yes, under the same principles as all clean-room recompilation projects:
- No Sony code is included — the runtime is written from scratch
- Users must supply their own legally obtained game binaries
- Function behavior is reimplemented based on public documentation and RPCS3's open-source reference

### What games work?

**flOw** by thatgamecompany is the first active port — see the [case study](GAME_PORTING_GUIDE.md#case-study-flow). The framework is ready for other games; contributions welcome.

### What platforms are supported?

- **Windows** (primary) — MSVC, XInput, WASAPI
- **Linux** — GCC/Clang, SDL2
- **macOS** — Clang, SDL2

Windows gets the most testing. Linux and macOS should work but may need minor fixes.

---

## Technical

### How does static recompilation work?

1. **Parse** the PS3 ELF binary to find code, data, and imports
2. **Disassemble** PowerPC instructions into structured IR
3. **Lift** each function to equivalent C code (register file → local variables, memory ops → `vm_read`/`vm_write`)
4. **Link** the generated C with ps3recomp's runtime library (VM, syscalls, HLE modules)
5. **Compile** with MSVC/GCC/Clang to produce a native x86-64 executable

### What about the SPUs?

The Cell processor has 6 SPU cores with their own ISA and 256KB local store each. ps3recomp handles SPUs via:
- **HLE** (preferred) — many SPU tasks (audio mixing, decompression) can be replaced with native equivalents
- **SPU lifter** — for complex SPU programs, `tools/spu_lifter.py` translates SPU assembly to C
- **SPURS management** — the cellSpurs module handles task scheduling without actual SPU execution

### Why C instead of C++?

The runtime and HLE modules are C17 for maximum ABI compatibility:
- C has a stable, well-defined ABI across compilers
- Simpler linkage with generated code (no name mangling)
- Game projects use C++20 for convenience — the public API headers have `extern "C"` guards

### What's a NID?

A **Name ID** is how PS3 dynamic linking works. Instead of symbol names, the PS3 uses 32-bit hashes:

```
NID = first_4_bytes_LE(SHA1(function_name + 16_byte_suffix))
```

When a game imports `cellAudioInit`, the binary contains NID `0x0B168F92` (not the string). ps3recomp's module system computes NIDs from names and matches them at load time.

See [NID_SYSTEM.md](NID_SYSTEM.md) for the deep dive.

### What about graphics (RSX)?

The RSX (Reality Synthesizer) is an NV47-class GPU. Games write GPU commands to a command buffer via cellGcmSys. Currently:
- **cellGcmSys is fully implemented** — display buffers, memory mapping, tile/zcull config, flip control
- **Command buffer parsing is not** — translating NV47 methods to Vulkan/D3D12 draw calls is the largest remaining task

### How accurate does recompilation need to be?

**Functionally equivalent**, not cycle-accurate. The goal is identical observable behavior (same pixels on screen, same audio, same game logic) — not matching PS3 timing to the nanosecond. This gives us freedom to use modern APIs and optimizations.

---

## Troubleshooting

### Build Errors

**`undefined reference to 'vm_base'`**
`vm_base` is declared `extern` in the runtime. Your game project's `main.cpp` must define it:
```c
uint8_t* vm_base = nullptr;
```

**`/bigobj required`** (MSVC)
Large generated source files (>65K sections) need the `/bigobj` compiler flag:
```cmake
target_compile_options(my_game PRIVATE /bigobj)
```

**`incompatible pointer types`**
Runtime headers are C; if you're including them from C++, wrap with:
```cpp
extern "C" {
#include "runtime/memory/vm.h"
}
```

**`C11 _Atomic in C++ file`**
Some headers (e.g., `cellSync.h`) use C11 atomics incompatible with C++. Only include headers for modules you actually bridge — use stubs for the rest.

### Runtime Errors

**`[HLE] UNIMPLEMENTED: funcName (NID 0xABCDEF01)`**
The game called a function your HLE modules don't handle. Either:
1. Add the function to your module registration
2. Check if ps3recomp already has a real implementation in `libs/`
3. Add a stub that returns `CELL_OK` if the function isn't critical

**`vm_init failed`**
The runtime couldn't allocate the 4GB virtual address space. Check:
- Sufficient virtual memory available (Windows: increase pagefile; Linux: check `ulimit -v`)
- You're building 64-bit, not 32-bit

**Crash in recompiled code (no HLE log)**
The crash is in lifted game code, not HLE. Common causes:
- **Missing TOC save** — import stubs should save r2 to sp+40 before calling handlers
- **Empty stub function** — a branch target wasn't in the function list, so it's a no-op
- **Unlifted instruction** — check generated code for `/* TODO: ... */` comments
- **Address sign-extension** — 32-bit addresses sign-extended to 64 bits in GPRs

**Wrong endianness (garbled data)**
All guest memory is big-endian. Use `vm_read32()`/`vm_write32()` (which byte-swap) instead of raw pointer access. For struct output from HLE bridges, write each field individually with `vm_write*`.

### Pipeline Errors

**`function not found at address 0xNNNNNNNN`**
The lifter references a function that wasn't in the function list. Add it manually:
```json
{"address": "0xNNNNNNNN", "size": 64, "source": "manual"}
```

**`TODO: unimplemented instruction`**
The lifter doesn't support this instruction yet. Common remaining gaps:
- VMX/AltiVec SIMD (`vaddfp`, `vmaddfp`, `vsel`, `vsldoi`, etc.) — the largest category
- Some `rld*` rotate variants with uncommon operand patterns
- Extended opcode 31 forms (`op31_x202`, `op31_x231`)

Recently added (may still show as TODO in older lifter output):
- `mulld`/`divd` (64-bit arithmetic), `adde`/`subfe` (carry ops)
- `cror`/`crand`/`crnand`/`crxor`/`crnor` (CR logical)
- `lwarx`/`stwcx.` (atomic reservation), `stdux`/`ldux` (update indexed)
- `tw`/`td` (trap — no-oped)

Fix in `tools/ppu_lifter.py` to benefit all game ports.

**Split-function crash (prologue returns without executing body)**
The lifter may split one PPC function into multiple C functions at boundary points. If the first piece (prologue) returns without calling the second piece (body), the function never executes. Fix: the lifter now emits fallthrough calls when a function doesn't end with `blr`/`b`. If you see this in older output, re-run the lifter or patch with a fallthrough call script.

---

## Performance

### How fast is recompiled code?

Within 2-3x of the original PS3 performance on modern hardware, often faster. The overhead comes from:
- Endian byte-swapping on every memory access
- Function call overhead for `vm_read`/`vm_write`
- Unoptimized generated code (the lifter doesn't do advanced optimization)

### The generated .cpp file is huge — compilation tips

- **Split into batches** — the lifter can output multiple smaller files instead of one giant one
- **Use `/bigobj`** on MSVC for files with >65K sections
- **Parallel compilation** — let MSBuild/Ninja use multiple cores
- **Incremental builds** — only recompile changed files
- **Release builds** — debug builds of 156 MB source files can take 30+ minutes; Release is faster

### Can I optimize the generated code?

Yes, but prefer fixing the **lifter** over hand-editing individual functions. Lifter improvements benefit every game port. Only hand-edit for truly game-specific one-off issues.
