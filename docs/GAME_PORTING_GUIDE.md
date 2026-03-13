# Game Porting Guide

A comprehensive, step-by-step guide to porting a PS3 game to native PC using ps3recomp.

---

## Table of Contents

1. [Overview](#overview)
2. [Phase 1: Game Selection and Assessment](#phase-1-game-selection-and-assessment)
3. [Phase 2: Binary Acquisition and Decryption](#phase-2-binary-acquisition-and-decryption)
4. [Phase 3: Analysis](#phase-3-analysis)
5. [Phase 4: Disassembly](#phase-4-disassembly)
6. [Phase 5: Code Generation](#phase-5-code-generation)
7. [Phase 6: Project Setup](#phase-6-project-setup)
8. [Phase 7: Building and Debugging](#phase-7-building-and-debugging)
9. [Phase 8: Stubbing and Implementation](#phase-8-stubbing-and-implementation)
10. [Phase 9: Graphics](#phase-9-graphics)
11. [Phase 10: Audio and Input](#phase-10-audio-and-input)
12. [Phase 11: SPU Handling](#phase-11-spu-handling)
13. [Phase 12: Testing and Polish](#phase-12-testing-and-polish)
14. [Tips and Best Practices](#tips-and-best-practices)
15. [Case Study: flOw](#case-study-flow)

---

## Overview

Porting a PS3 game with ps3recomp is a **per-game project**. Unlike emulation (where one binary runs many games), static recompilation requires understanding each game's specific needs. The reward is a truly native port with full modding potential.

### What You'll Need

- A legally obtained copy of the PS3 game
- Decryption keys or a fSELF version of the game binary
- Python 3.9+ for the recompiler tools
- CMake 3.20+ and a C17/C++20 compiler
- RPCS3 as a reference (for testing and behavior verification)
- Patience and debugging skills

### Expected Timeline

| Game Complexity | Estimate | Examples |
|---|---|---|
| Simple PSN title | Days to weeks | flOw, fl0w, Flower |
| Standard indie game | Weeks to months | Journey, Limbo, Braid |
| Full retail title | Months | Standard action/adventure games |
| Complex AAA title | Months to years | The Last of Us, Uncharted |

The main variables are: how many unique system APIs the game uses, whether it uses SPU programs extensively, and how complex its graphics pipeline is.

---

## Phase 1: Game Selection and Assessment

### Choose a Good First Target

For your first port, look for games that:

1. **Work well in RPCS3** — this means the game's behavior is well-understood
2. **Are simple** — fewer system API calls = fewer HLE modules needed
3. **Use common engines** — PhyreEngine, Unreal Engine 3, etc. have known patterns
4. **Don't rely on SPU-heavy compute** — audio-only SPU use is much simpler than physics/particle SPU use
5. **Have a small binary** — fewer functions to recompile and debug

### Pre-Analysis with RPCS3

Before touching ps3recomp, run the game in RPCS3 with logging enabled:

1. Enable `Log Level: All` in RPCS3 settings
2. Play through the game's startup and early gameplay
3. Check the log for:
   - Which modules are loaded (`cellSysmoduleLoadModule`)
   - Which NID calls are frequent
   - Any "TODO" or "Unimplemented" warnings
   - SPU thread creation patterns

This tells you which HLE modules the game needs most.

### Check Module Coverage

Cross-reference the game's imports against [MODULE_STATUS.md](MODULE_STATUS.md):

```bash
# After ELF analysis
python tools/prx_analyzer.py game/EBOOT.ELF --coverage
```

If coverage is 95%+, the game is a good candidate.

---

## Phase 2: Binary Acquisition and Decryption

### Getting the ELF

PS3 game binaries are encrypted in SELF (Signed ELF) format. You need a decrypted ELF:

**Option A: fSELF (fake SELF)**
- Some development/debug versions of games use fSELF which can be trivially "decrypted"
- The elf_parser tool handles fSELF automatically

**Option B: RPCS3 Decryption**
- RPCS3 can dump decrypted ELFs when loading a game
- Enable "Dump Executable" in RPCS3 debug settings
- Look for the decrypted file in RPCS3's cache directory

**Option C: ps3recomp decrypt tool**
```bash
python tools/elf_parser.py EBOOT.BIN --decrypt --output game/
```

### Required Files

| File | Location on Disc | Purpose |
|------|-----------------|---------|
| `EBOOT.BIN` | `PS3_GAME/USRDIR/EBOOT.BIN` | Main game executable |
| `PARAM.SFO` | `PS3_GAME/PARAM.SFO` | Game metadata (title, ID, version) |
| `*.sprx` | `PS3_GAME/USRDIR/*.sprx` | Additional game modules (if any) |
| `TROPHY.TRP` | `PS3_GAME/TROPDIR/*/TROPHY.TRP` | Trophy configuration |
| Game assets | `PS3_GAME/USRDIR/*` | Textures, models, audio, scripts |

---

## Phase 3: Analysis

### ELF Analysis

```bash
python tools/elf_parser.py game/EBOOT.ELF --output analysis/
```

This produces:
- `elf_info.json` — header, entry point, architecture
- `imports.json` — all imported functions (module + NID)
- `exports.json` — exported symbols (if any)
- `segments.json` — memory layout

### Key Questions to Answer

1. **How many imports?** — fewer is better; 100-200 is typical
2. **Which modules?** — check coverage against ps3recomp's HLE modules
3. **Any unresolved NIDs?** — these need stubs or implementations
4. **Entry point?** — the address where execution starts
5. **TOC (Table of Contents)?** — the r2 value needed for data access
6. **PRX modules?** — does the game load additional .sprx files?
7. **Memory layout?** — where does code, data, and BSS live?

### NID Resolution

```bash
python tools/nid_database.py --resolve analysis/imports.json --output resolved.json
```

Review the output for unresolved NIDs. For each one:
- Check RPCS3 for the function name
- Determine if it's critical or can be stubbed
- Add it to the game's stubs.cpp if needed

---

## Phase 4: Disassembly

### Function Detection

```bash
python tools/find_functions.py game/EBOOT.ELF --output analysis/functions.json
```

Review the function list:
- How many functions? (typical: 500–5000 for an indie game, 10000+ for AAA)
- Are there symbols? (debug builds have names; retail usually stripped)
- Any suspiciously large "functions"? (might be data misidentified as code)

### Full Disassembly

```bash
python tools/ppu_disasm.py game/EBOOT.ELF \
    --functions analysis/functions.json \
    --annotate \
    --output disasm/
```

### SPU Program Detection

If the game uses SPU programs:
```bash
python tools/elf_parser.py game/EBOOT.ELF --extract-spu --output spu_programs/
```

SPU ELF segments are embedded within the main PPU ELF. Each one needs separate analysis.

---

## Phase 5: Code Generation

### PPU Code Lifting

```bash
python tools/ppu_lifter.py disasm/ \
    --nid-db tools/nid_db.json \
    --output recomp/
```

This generates:
- `functions_NNNN.c` — batches of recompiled C functions
- `func_table.cpp` — maps guest address → host function pointer
- `data_segments.c` — initialized data as C arrays

### SPU Code Lifting (if applicable)

```bash
python tools/spu_disasm.py spu_programs/spu_0.elf --output spu_disasm/
# SPU lifter generates similar C output for each SPU program
```

---

## Phase 6: Project Setup

### Create the Project

```bash
cp -r ps3recomp/templates/project/ my_game/
```

### Configure

Edit `my_game/config.toml`:

```toml
[input]
elf_path = "../game/EBOOT.ELF"

[modules]
# Enable only the modules your game uses
cellSysutil   = "hle"
cellGcmSys    = "hle"
cellPad       = "hle"
cellAudio     = "hle"
cellFs        = "hle"
cellFont      = "hle"
# ... add all modules from your import analysis

[debug]
log_hle_calls = true
log_missing_nids = true
break_on_unimplemented = true   # Stop on unimplemented calls
```

### Copy Generated Files

```bash
cp recomp/*.c recomp/*.cpp my_game/recompiled/
cp data_segments.c my_game/recompiled/
```

### Set Up Game Assets

Create the virtual filesystem:

```bash
mkdir -p my_game/hdd0/game/TITLEID/USRDIR
mkdir -p my_game/hdd0/home/00000001/trophy/TITLEID
# Copy game assets
cp -r PS3_GAME/USRDIR/* my_game/hdd0/game/TITLEID/USRDIR/
```

---

## Phase 7: Building and Debugging

### First Build

```bash
cd my_game
cmake -B build -G Ninja -DPS3RECOMP_DIR=/path/to/ps3recomp
cmake --build build 2>&1 | tee build.log
```

Expect compilation errors on first try. Common issues:

**Undefined functions:**
```
error: undefined reference to 'recomp_func_00012345'
```
→ Function was referenced but not in the function list. Add it or add a stub.

**Type mismatches:**
```
error: incompatible pointer types
```
→ The lifter may need manual adjustment for complex calling conventions.

### First Run

```bash
./build/my_game 2>&1 | tee run.log
```

Expected output:
```
=== ps3recomp game runner ===
Initializing runtime...
Loaded 1234 recompiled functions
Creating main PPU thread...
[WARNING] Unimplemented NID 0xABCDEF01 in module cellFoo
[WARNING] Unimplemented NID 0x12345678 in module cellBar
```

### Debugging Strategy

1. **Run under a debugger** (GDB/LLDB/Visual Studio)
2. **Set `break_on_unimplemented = true`** to catch missing functions immediately
3. **Compare with RPCS3** — run the same game in RPCS3 with logging and compare the call sequences
4. **Use log output** — `log_hle_calls = true` shows every HLE function call
5. **Binary search for crashes** — if the game crashes in recompiled code, narrow down the problematic function by checking the guest address from the backtrace

---

## Phase 8: Stubbing and Implementation

### Adding Game-Specific Stubs

In `stubs.cpp`, add implementations for unresolved NIDs:

```cpp
#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"
#include <cstdio>

// Stub for a non-critical function
extern "C" int32_t cellFooDoSomething(uint32_t param1, uint32_t param2)
{
    printf("[STUB] cellFooDoSomething(0x%x, 0x%x)\n", param1, param2);
    return CELL_OK;
}

// Stub for a function that returns data
extern "C" int32_t cellBarGetInfo(uint32_t* out_info)
{
    if (out_info) *out_info = 0;
    printf("[STUB] cellBarGetInfo -> 0\n");
    return CELL_OK;
}
```

### Priority Order

1. **Functions that crash if missing** — stub immediately
2. **Functions that produce wrong behavior** — implement properly
3. **Functions called during init** — must at least return CELL_OK
4. **Functions called during gameplay** — implement for correctness
5. **Rarely-called functions** — stub last

---

## Phase 9: Graphics

### The Challenge

RSX graphics is the hardest part of any PS3 port. The game writes NV47xx GPU commands to a command buffer, and these need to be translated to a modern graphics API.

### Strategy

**Phase 1: Null renderer**
- Set `backend = "null"` in config
- Verify all game logic runs correctly without graphics
- This proves the recompilation and HLE are working

**Phase 2: State tracking**
- cellGcmSys already tracks RSX state (tiles, zcull, display buffers)
- Log all command buffer writes to understand the rendering pipeline
- Identify the vertex formats, shader programs, and draw call patterns

**Phase 3: Graphics backend**
- Implement a Vulkan or D3D12 renderer that consumes the command buffer
- Start with basic triangle rendering
- Progressively add shader translation, texture sampling, blending modes

### What cellGcmSys Provides

The HLE module already handles:
- Command buffer management (put/get pointers)
- Local memory allocation (VRAM heap)
- IO memory mapping (main memory → GPU accessible)
- Tile and zcull configuration
- Display buffer registration
- Flip and VBlank callbacks
- Timestamps and report data

What it does NOT do (yet):
- Parse and execute NV47xx method commands
- Translate vertex/fragment programs to host shaders
- Actually render anything to the screen

---

## Phase 10: Audio and Input

### Audio

cellAudio provides real audio output. Most games just need:
1. `cellAudioInit()` — starts the mixing thread
2. Write PCM samples to port buffers
3. `cellAudioSetNotifyEventQueue()` — sync with the mixing interval

**Common issues:**
- Audio crackling → increase buffer size in config
- No sound → check that the audio backend matches your OS
- Wrong sample rate → PS3 native is 48000 Hz

### Input

cellPad provides real gamepad input via XInput (Windows) or SDL2.

**Button mapping (XInput → PS3):**
| XInput | PS3 |
|--------|-----|
| A | Cross |
| B | Circle |
| X | Square |
| Y | Triangle |
| LB | L1 |
| RB | R1 |
| LT | L2 (analog) |
| RT | R2 (analog) |
| Back | Select |
| Start | Start |
| Guide | PS |

---

## Phase 11: SPU Handling

### Assessment

Determine how the game uses SPUs:

1. **SPURS framework** — most games use this
   - Check for `cellSpursInitialize`, `cellSpursCreateTask` calls
   - The SPURS HLE handles management; actual SPU tasks need attention

2. **Raw SPU** — some games create SPU threads directly
   - Check for `sys_spu_thread_group_create` syscalls
   - Each SPU program needs separate analysis

3. **SPU task types** — identify what the SPU programs do:
   - **Audio mixing** (ATRAC3+, PCM mixing) → replace with host decoding
   - **Physics** (Havok, Bullet) → intercept and run on host CPU
   - **Decompression** (zlib, LZ) → replace with host zlib
   - **Vertex processing** → handle in graphics backend
   - **Custom compute** → most complex; may need SPU interpreter

### HLE Approach (Preferred)

If you can identify the SPU task type:

```cpp
// In stubs.cpp — intercept a known SPU audio mixing task
void hle_spu_audio_mixer(spu_context* ctx)
{
    // Read parameters from SPU local store
    uint32_t src_addr = spu_ls_read32(ctx, 0x100);
    uint32_t dst_addr = spu_ls_read32(ctx, 0x104);
    uint32_t num_samples = spu_ls_read32(ctx, 0x108);

    // Perform the mixing on the host CPU
    float* src = (float*)(vm_base + src_addr);
    float* dst = (float*)(vm_base + dst_addr);
    for (uint32_t i = 0; i < num_samples; i++)
        dst[i] += src[i];
}
```

---

## Phase 12: Testing and Polish

### Testing Checklist

- [ ] Game starts without crashes
- [ ] Main menu is functional
- [ ] Gameplay runs at correct speed
- [ ] Audio plays correctly
- [ ] Input is responsive
- [ ] Save data loads and saves
- [ ] Trophy unlocks work
- [ ] No memory leaks (check with AddressSanitizer/Valgrind)
- [ ] Performance is acceptable (profile with host profiling tools)

### Performance Tuning

1. **Compile with optimizations** — `-O2` or `/O2` makes a huge difference
2. **Profile** — use perf/VTune/Instruments to find hotspots
3. **Batch DMA** — if SPU DMA is a bottleneck, consider batching transfers
4. **Reduce logging** — disable `log_hle_calls` for release builds
5. **Memory access patterns** — the endian conversion can be a bottleneck; consider caching

---

## Tips and Best Practices

1. **Start with RPCS3** — always verify game behavior in RPCS3 first
2. **Incremental approach** — get one screen working, then the next
3. **Version control everything** — commit early and often
4. **Document your findings** — each game has quirks; write them down
5. **Check RPCS3's game compatibility wiki** — known issues are documented
6. **Join the community** — other porters may have solved your problem
7. **Don't fight the lifter** — if generated code is wrong, fix the lifter, not individual functions (unless it's truly a one-off issue)
8. **Test on multiple platforms** — if you're only on Windows, test on Linux too (or vice versa)

---

## Case Study: flOw

The first game port in progress using ps3recomp.

**Game:** flOw by thatgamecompany (NPUA80001)
**Engine:** PhyreEngine
**Repository:** [sp00nznet/flow](https://github.com/sp00nznet/flow)

**Analysis results:**
- 140 total imports across all modules
- 139 out of 140 resolved (99.3% coverage)
- Modules used: cellSysutil, cellGcmSys, cellPad, cellFs, cellFont, cellL10n, cellNet, cellNetCtl, sceNp, sceNpTrophy, cellSpurs, cellSync, sysPrxForUser

**What makes flOw a good first target:**
- Small PSN title (~50 MB)
- Simple gameplay (2D physics-based)
- Well-documented engine (PhyreEngine)
- Works perfectly in RPCS3
- Limited SPU usage (audio only)
- No complex graphics (no 3D, no advanced shaders)

**Status:** Binary analysis complete, project scaffolded, ready for disassembly and lifting phase.
