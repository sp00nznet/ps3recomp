# ps3recomp

### *Because the Cell processor deserves a second life*

> Static recompilation runtime libraries for PlayStation 3 titles.
> Turn PS3 binaries into native executables. No emulator required.

---

## What Is This?

**ps3recomp** is an open-source toolkit that provides the runtime libraries, system stubs, and analysis tools needed to **statically recompile PlayStation 3 games into native PC executables**.

Instead of interpreting or dynamically recompiling PowerPC/SPU instructions at runtime (what emulators like RPCS3 do brilliantly), we take the opposite approach: **translate everything ahead of time** into C/C++ that compiles with your favorite compiler on any modern platform.

This is the same philosophy behind:
- [N64Recomp](https://github.com/N64Recomp/N64Recomp) (N64 -> native)
- [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) (Xbox 360 -> native)
- [PS2Recomp](https://github.com/ran-j/PS2Recomp) (PS2 -> native)
- [burnout3](https://github.com/sp00nznet/burnout3) (Original Xbox -> native)
- [360tools](https://github.com/sp00nznet/360tools) (Xbox 360 -> native)

...but for the PS3's glorious, terrifying **Cell Broadband Engine**.

## Why PS3?

The PlayStation 3 has over **3,000 titles** and some of the most beloved games ever made. RPCS3 has done heroic work making ~70% of the library playable through emulation, but static recompilation offers something different:

- **Native performance** — no runtime translation overhead
- **Lower hardware requirements** — runs on mid-range PCs
- **Moddability** — recompiled C/C++ is infinitely more hackable than PPC binaries
- **Preservation** — native ports survive long after emulators stop being maintained
- **Portability** — compile for Windows, Linux, macOS, ARM, whatever

## The Challenge

The PS3's Cell processor is a beast unlike anything else:

| Component | What It Is | Why It's Hard |
|-----------|-----------|---------------|
| **PPU** | PowerPC 64-bit main CPU | Custom ISA extensions, VMX/AltiVec SIMD |
| **6x SPU** | 128-bit SIMD vector processors | Proprietary ISA, 256KB local store, explicit DMA |
| **Memory** | 256MB XDR RAM | 128-byte alignment requirements, split bus |
| **LV2 Kernel** | Proprietary OS layer | ~300+ system calls, complex threading model |
| **PRX Modules** | Dynamic libraries | 100+ system libraries with intricate interdependencies |

We don't shy away from hard problems. We just break them into smaller ones.

## Architecture

```
ps3recomp/
├── tools/                    # Binary analysis & recompilation pipeline
│   ├── elf_parser.py         # Parse PS3 ELF/SELF/PRX binaries
│   ├── ppu_disasm.py         # PowerPC disassembler with PS3 extensions
│   ├── ppu_lifter.py         # PPU assembly -> C code lifter
│   ├── spu_disasm.py         # SPU instruction set disassembler
│   ├── spu_lifter.py         # SPU assembly -> C code lifter
│   ├── extract_spu_images.py # Extract embedded SPU images from PRX/ELF
│   ├── find_spu_functions.py # SPU function boundary detection
│   ├── wrap_spu_elf.py       # Wrap a raw SPU blob as a loadable ELF
│   ├── prx_analyzer.py       # Analyze PRX imports/exports and NIDs
│   ├── nid_database.py       # Function Name ID (NID) resolver
│   ├── find_functions.py     # Function boundary detection
│   └── generate_stubs.py     # Auto-generate HLE stubs from RPCS3 modules
│
├── runtime/                  # Core runtime for recompiled executables
│   ├── ppu/                  # PPU execution context
│   │   ├── ppu_context.h     # Register file, CR, LR, CTR, FPSCR
│   │   ├── ppu_memory.h      # Memory access macros (loads, stores, byte-swap)
│   │   └── ppu_ops.h         # Instruction operation macros
│   ├── spu/                  # SPU execution context
│   │   ├── spu_context.h     # 128×128-bit register file, 256KB local store, image_id
│   │   ├── spu_channels.c    # Channel ops + per-image function-table dispatch
│   │   ├── spu_helpers.h     # SPU intrinsics (incl. float conversions)
│   │   ├── spu_dma.h         # DMA (MFC) transfer engine
│   │   └── tests/            # Self-contained SPU runtime tests
│   ├── memory/               # Memory subsystem
│   │   ├── vm.h              # Virtual memory manager (256MB address space)
│   │   └── atomic.h          # PS3 atomic operations (lwarx/stwcx)
│   └── syscalls/             # LV2 kernel system call implementations
│       ├── sys_ppu_thread.h  # Thread creation/management
│       ├── sys_mutex.h       # Mutex/cond synchronization
│       ├── sys_memory.h      # Memory allocation/mapping
│       ├── sys_fs.h          # Filesystem operations
│       ├── sys_prx.h         # PRX module loading
│       └── lv2_syscall_table.h # Full syscall dispatch table
│
├── libs/                     # HLE library implementations (from RPCS3's module system)
│   ├── audio/                # cellAudio, cellAdec, libmixer, libsnd3, libsynth2
│   ├── video/                # cellGcmSys, cellResc, cellVideoOut, cellVpost, cellVdec
│   ├── input/                # cellPad, cellKb, cellMouse, cellGem
│   ├── network/              # cellNetCtl, cellHttp, cellHttps, cellSsl, sceNp*
│   ├── filesystem/           # cellFs, cellGame, cellSaveData, cellStorage
│   ├── system/               # cellSysutil, cellSysmodule, cellRtc, sysPrxForUser
│   ├── spurs/                # cellSpurs, cellFiber, cellSpursJq, cellDaisy (SPU task management)
│   ├── sync/                 # cellSync, cellSync2 (cross-PPU/SPU synchronization)
│   ├── codec/                # cellJpgDec, cellPngDec, cellGifDec, cellPamf, cellDmux, cellAdecAtrac3p, cellAdecCelp8, cellVdecDivx
│   ├── font/                 # cellFont, cellFontFT, cellFreeType, cellL10n
│   └── misc/                 # cellScreenshot, cellSubdisplay, cellImeJp, cellVideoExport, cellMusicExport, cellGameRecording, etc.
│
├── include/ps3emu/           # Public API headers
│   ├── ps3types.h            # Fundamental PS3 types (u8-u128, s8-s64, be_t<>)
│   ├── endian.h              # Big-endian <-> little-endian conversion
│   ├── nid.h                 # NID hashing and lookup
│   ├── module.h              # Module registration framework
│   └── error_codes.h         # PS3 system error codes (CELL_OK, CELL_E*)
│
├── templates/project/        # Starter template for new game ports
│   ├── CMakeLists.txt        # Build system
│   ├── main.cpp              # Application entry point
│   ├── stubs.cpp             # Game-specific function overrides
│   └── config.toml           # Recompiler configuration
│
├── config/                   # Reference configurations
│   └── example.toml          # Example recompilation config
│
├── patches/                  # Patches for upstream tools
│   └── xenonrecomp-ppu.patch # XenonRecomp adaptations for PPU (Cell != Xenon)
│
└── docs/                     # Documentation (incl. ARCHITECTURE, RUNTIME, SPU_LIFTER, RSX_GRAPHICS, ...)
    ├── ARCHITECTURE.md        # Cell processor and recomp pipeline deep-dive
    ├── GETTING_STARTED.md     # How to recompile your first PS3 game
    ├── MODULE_STATUS.md       # Implementation status of all HLE modules
    ├── MODULES_REFERENCE.md   # Detailed per-module documentation (all 77+)
    ├── RUNTIME.md             # Runtime reference: VM, PPU/SPU contexts, types
    ├── SYSCALLS.md            # All LV2 kernel syscall implementations
    ├── NID_SYSTEM.md          # PS3 NID linking system explained
    ├── TOOLS.md               # Recompiler pipeline tools reference
    ├── BUILDING.md            # Build system, compilers, platform notes
    ├── GAME_PORTING_GUIDE.md  # Step-by-step game porting walkthrough
    └── PLATFORM_ABSTRACTION.md # Win32/POSIX cross-platform details
```

## How It Works

The pipeline follows the same proven approach as our [360tools](https://github.com/sp00nznet/360tools) and [burnout3](https://github.com/sp00nznet/burnout3) projects, adapted for the Cell architecture:

```
   PS3 Game Disc / PKG
         │
         ▼
   ┌─────────────┐
   │  SELF/EBOOT  │  Encrypted PS3 executable
   │  Decryption  │  (requires keys / fSELF)
   └──────┬──────┘
          │
          ▼
   ┌─────────────┐
   │  ELF Parser  │  Extract sections, segments, relocations
   │  + PRX Scan  │  Identify imported NIDs & library deps
   └──────┬──────┘
          │
          ▼
   ┌─────────────┐
   │  PPU Disasm  │  Disassemble PowerPC 64-bit + VMX
   │  + Analysis  │  Function boundaries, jump tables, ABI
   └──────┬──────┘
          │
          ▼
   ┌─────────────┐
   │  PPU Lifter  │  PowerPC → C code generation
   │  + SPU Lift  │  SPU programs → C code generation
   └──────┬──────┘
          │
          ▼
   ┌─────────────┐
   │  Link with   │  ps3recomp runtime + HLE libs
   │  Runtime     │  Provides all PS3 OS services
   └──────┬──────┘
          │
          ▼
   ┌─────────────┐
   │  Compile &   │  MSVC / GCC / Clang
   │  Ship!       │  Native x86-64 executable
   └─────────────┘
```

## SPU Strategy

The SPU is the elephant in the room. Each SPU has its own instruction set, its own 256KB memory, and communicates with the PPU via DMA and mailbox channels. Our approach:

1. **SPU programs are self-contained** — they live in ELF segments loaded to local store (`extract_spu_images.py` pulls them out of PRX/ELF)
2. **Recompile each SPU program separately** — dedicated `spu_disasm.py` + `spu_lifter.py` handle the SPU ISA
3. **SPU local store becomes an array** — 256KB per "virtual SPU"
4. **DMA operations become memcpy** — via the MFC engine (`spu_dma.h`)
5. **Channels become cross-thread message queues** — preserving ordering guarantees
6. **Per-image function-table dispatch** — `image_id` lets multiple SPU programs (e.g. a SPURS kernel, a policy module, and a job) coexist at overlapping local-store addresses

This mirrors how RPCS3 handles SPU but at compile time rather than runtime. The
disassembler, lifter, and runtime are functional and covered by self-contained
tests (`runtime/spu/tests/`: sum, shufb, DMA, brsl-return).

## Module Status

We're building HLE implementations based on RPCS3's module system. **97 modules complete, 1 partial (media decode), 283 files, ~74,000 lines of code.**

| Category | Modules | Status |
|----------|---------|--------|
| **Kernel Threading** | sys_ppu_thread, sys_mutex, sys_cond, sys_semaphore, sys_rwlock | ✅ Complete |
| **Kernel Events** | sys_event (queues, ports, flags), sys_timer | ✅ Complete |
| **Kernel Memory** | sys_memory, sys_mmapper (containers, shared mem) | ✅ Complete |
| **Kernel Filesystem** | sys_fs (path mapping, real I/O) | ✅ Complete |
| **Filesystem** | cellFs (real file I/O, dir ops, stat, path translation) | ✅ Complete |
| **Save System** | cellSaveData (callback-driven, PARAM.SFO), cellGame | ✅ Complete |
| **Input** | cellPad (XInput/SDL2), cellKb, cellMouse | ✅ Complete |
| **Audio** | cellAudio (WASAPI/SDL2, mixing thread, multi-port) | ✅ Complete |
| **Video Output** | cellVideoOut (resolution config, 720p default) | ✅ Complete |
| **Codecs** | cellPngDec, cellJpgDec, cellGifDec (stb_image) | ✅ Complete |
| **Font** | cellFont (stb_truetype backend + fallback metrics) | ✅ Complete |
| **Network** | sys_net (BSD sockets), cellNet, cellNetCtl, cellHttpUtil, cellSsl, sceNp*, sceNpTrophy | ✅ Complete |
| **Hardware** | cellUsbd (USB), cellCamera (PS Eye), cellGem (PS Move) — stub, no devices | ✅ Complete |
| **Sync** | cellSync (atomic spinlocks, LF queue), cellSync2 (OS-backed) | ✅ Complete |
| **System** | cellRtc, cellMsgDialog, cellOskDialog, cellUserInfo, cellGameExec | ✅ Complete |
| **Localization** | cellL10n (UTF-8/16/32/UCS-2, ISO-8859-1, generic converter) | ✅ Complete |
| **Resolution** | cellResc (display modes, scaling, interlace, aspect ratio) | ✅ Complete |
| **Fibers** | cellFiber (PPU fibers via Windows Fibers/ucontext) | ✅ Complete |
| **AV Config** | cellAvconfExt (audio output info, gamma, sound availability) | ✅ Complete |
| **Input Util** | cellKey2char (HID scancode → Unicode, US-101 layout) | ✅ Complete |
| **Graphics** | cellGcmSys (cmd buffer, IO mapping, tile/zcull, flip handlers, timestamps, FIFO walker + ring recycle), RSX command processor (NV47xx parsing, state tracking, backend callbacks), **live D3D12 backend** (executes the title's real NV4097 vertex/fragment programs — indexed draws, per-draw VP constants + textures, render-to-texture, MRT, FP predication), null backend (Win32 window) | ✅ Complete |
| **SPURS** | cellSpurs (management APIs, event flags, task/workload tracking), cellSpursJq (job queues with wait/signal), cellDaisy (real ring buffer FIFO) | ✅ Complete |
| **Core Runtime** | cellSysutil (BGM, cache, disc), cellSysmodule, sysPrxForUser (real lwmutex/lwcond/threads) | ✅ Complete |
| **Media Pipeline** | cellPamf (real PAMF header + stream descriptor parsing), cellDmux (callback sequencing), cellVdec/cellAdec (PICOUT/PCMOUT callbacks, no actual decode — needs FFmpeg), cellSail (state machine) | 🔨 Partial |
| **HTTP/HTTPS** | cellHttp (real HTTP/1.1 via native sockets), cellHttps (TLS stub with cert management) | ✅ Complete |
| **Misc/Export** | cellSubdisplay, cellImeJp, cellVideoExport, cellMusicExport, cellPhotoExport/Import, cellGameRecording, cellPrint, cellRemotePlay | ✅ Complete |

Full status tracking: [docs/MODULE_STATUS.md](docs/MODULE_STATUS.md)

## Documentation

We've written extensive docs covering every aspect of the project. Whether you're contributing, porting a game, or just trying to understand how PS3 static recompilation works, there's a doc for you.

| Document | What It Covers |
|----------|---------------|
| **[Getting Started](docs/GETTING_STARTED.md)** | Prerequisites, installation, first recompilation walkthrough |
| **[Architecture](docs/ARCHITECTURE.md)** | Cell processor overview, recomp pipeline stages, memory model, comparison with RPCS3 |
| **[Building](docs/BUILDING.md)** | CMake build system, compiler support, platform-specific notes, troubleshooting |
| **[Game Porting Guide](docs/GAME_PORTING_GUIDE.md)** | Full 12-phase walkthrough for porting a PS3 game — with [flOw case study](docs/GAME_PORTING_GUIDE.md#case-study-flow) |
| **[Custom Modules](docs/CUSTOM_MODULES.md)** | Step-by-step tutorial for writing new HLE modules — NID system, calling convention bridge, patterns |
| **[FAQ & Troubleshooting](docs/FAQ.md)** | Common questions, build errors, runtime crashes, lifter issues, performance tips |
| **[Contributing](CONTRIBUTING.md)** | Development setup, code style, PR guidelines, ways to contribute |
| **[Runtime Reference](docs/RUNTIME.md)** | Virtual memory manager, PPU/SPU execution contexts, type system, endianness, syscall dispatch, DMA engine |
| **[Syscall Reference](docs/SYSCALLS.md)** | All LV2 kernel syscalls: threading, sync, events, timers, memory, filesystem |
| **[NID System](docs/NID_SYSTEM.md)** | How PS3 function linking works, NID computation, module registration framework |
| **[Module Reference](docs/MODULES_REFERENCE.md)** | Detailed documentation for all 93+ HLE modules — what they do and how they're implemented |
| **[Module Status](docs/MODULE_STATUS.md)** | Quick-reference status table for all modules |
| **[RSX Graphics](docs/RSX_GRAPHICS.md)** | RSX GPU translation architecture: command processor, D3D12/Vulkan backends, shader strategy |
| **[Tools Reference](docs/TOOLS.md)** | Every recompiler pipeline tool documented: ELF parser, disassembler, lifter, NID database |
| **[SPU Lifter](docs/SPU_LIFTER.md)** | SPU ISA decoding/lifting, the runtime model (local store, channels, DMA, per-image dispatch), and the SPU tests |
| **[Platform Abstraction](docs/PLATFORM_ABSTRACTION.md)** | How we handle Win32 vs POSIX: threading, sockets, timers, audio, memory, fibers |

## Getting Started

> **Prerequisites**: Python 3.10+, CMake 3.20+, a C17/C++20 compiler (MSVC 19.x, GCC 12+, or Clang 15+)

```bash
# Clone the repo
git clone https://github.com/sp00nznet/ps3recomp.git
cd ps3recomp

# Install Python tools
pip install -r requirements.txt

# Analyze a decrypted PS3 ELF
python tools/elf_parser.py /path/to/EBOOT.ELF --output analysis/

# Disassemble PPU code
python tools/ppu_disasm.py analysis/EBOOT.ELF --output disasm/

# Lift to C
python tools/ppu_lifter.py disasm/ --output recomp/

# Build with the runtime
cd templates/project
cmake -B build -G Ninja
cmake --build build
```

See [docs/GETTING_STARTED.md](docs/GETTING_STARTED.md) for the full walkthrough.

## Game Ports Using ps3recomp

| Game | Title ID | Status | Repo |
|------|----------|--------|------|
| **flOw** (thatgamecompany) | NPUA80001 | 92K functions, game reaches main() init, module loading + sysutil callbacks working, trampoline system for split-function chains, ~10K TODOs, D3D12 backend ready | [sp00nznet/flow](https://github.com/sp00nznet/flow) |
| **Tokyo Jungle** (Crispy's/SCE Japan) | NPUA80523 | 33K functions lifted, CRT init + HLE framework wired, indirect call dispatch | [sp00nznet/tokyojungle](https://github.com/sp00nznet/tokyojungle) |
| **The Simpsons Arcade Game** (Konami) | NPUB30563 | 14.7K functions; boots through CRT + GCM init with a live D3D12 window; a Konami arcade *emulator* EBOOT that routes rendering through CRI middleware on SPURS SPU tasks — drove the SPU recompilation work; blocked on the SPU-task pipeline | [sp00nznet/simpsonsarcade-ps3](https://github.com/sp00nznet/simpsonsarcade-ps3) |
| **You Don't Know Jack** (Jellyvision/THQ) | BLUS30569 | 5,859 functions; **boots the full init stack and renders** — a real double-buffered flip loop through the live D3D12 backend (~2,300 guest flip requests in 40 s, alternating buffers). Scaleform Flash UI + FMOD audio — a menu/UI-heavy title, an ideal recomp target. The `0xC708C708` scene-dispatch poison and the GCM ring-wrap wedge that used to gate the first draws are both fixed (real command-buffer-full callback → drain + ring recycle). Frontier: the SPURS/CRI task pipeline — the LLE `libsre` SPU-kernel path currently only comes up in Debug builds | [sp00nznet/youdontknowjack](https://github.com/sp00nznet/youdontknowjack) |

Want to port a game? Start with the [Getting Started](#getting-started) section, check [docs/MODULE_STATUS.md](docs/MODULE_STATUS.md) for system library coverage, and see the [flOw case study](docs/GAME_PORTING_GUIDE.md#case-study-flow) for a real-world walkthrough.

## Relationship to Other Projects

| Project | Role |
|---------|------|
| **[RPCS3](https://github.com/RPCS3/rpcs3)** | Our primary reference for HLE module behavior and system call semantics. Standing on the shoulders of giants. |
| **[XenonRecomp](https://github.com/hedge-dev/XenonRecomp)** | PowerPC recompiler for Xbox 360. Both CPUs are PowerPC — we adapt its lifter for PPU-specific extensions. |
| **[N64Recomp](https://github.com/N64Recomp/N64Recomp)** | Pioneered the modern static recomp approach. Our architecture follows the same "recompile to C, link with runtime" philosophy. |
| **[PS2Recomp](https://github.com/ran-j/PS2Recomp)** | Sibling project for PS2. Different ISA (MIPS vs PowerPC) but same spirit. |
| **[360tools](https://github.com/sp00nznet/360tools)** | Our own Xbox 360 toolkit. ps3recomp follows the same project structure and conventions. |
| **[pcrecomp](https://github.com/sp00nznet/pcrecomp)** | Our PC recompilation toolkit. Shared philosophy of "analyze → disassemble → lift → link → ship". |

## Building Blocks We Leverage

- **RPCS3's HLE modules** — 100+ modules of battle-tested PS3 system behavior
- **XenonRecomp's PowerPC lifter** — adapted for Cell PPU (same ISA family, different extensions)
- **LLVM** — for optimized native code generation from lifted C
- **Vulkan / Direct3D 12** — for RSX graphics translation
- **SDL2** — cross-platform input, audio, windowing

## Contributing

This is a massive undertaking and we need help. Here's how to get involved:

- **HLE Module Authors** — Port RPCS3's C++ HLE implementations to standalone link-time libraries
- **PPU/SPU Experts** — Improve the disassembler and lifter accuracy
- **Graphics Engineers** — Build the RSX → Vulkan/D3D12 translation layer
- **Game Testers** — Try recompiling titles and report what breaks
- **Documentation** — Help us map out the PS3's system library landscape

See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

## Legal

This project does not contain any proprietary Sony code, encryption keys, or copyrighted material. It provides clean-room implementations of system library interfaces based on publicly documented behavior. You must provide your own legally obtained PS3 game files.

Contributors are credited in **[CONTRIBUTORS.md](CONTRIBUTORS.md)**.

## License

MIT License. See [LICENSE](LICENSE) for details.

---

## Contributors

ps3recomp is built by a growing community. See **[CONTRIBUTORS.md](CONTRIBUTORS.md)**
for who did what — thank you, everyone.

## Changelog

### Unreleased
*Everything folded via `integrate/fold-2026-08-14` — [@sagemono](https://github.com/sagemono)'s
guest-ABI HLE batch and SPU-lifter faithful adoption, plus
[@canersaka](https://github.com/canersaka)'s guest-callback/lv2/ABI batch.*

**HLE & guest ABI**
- **Guest-EA / big-endian out-param marshalling** across the HLE surface — `cellUserInfo`, trophy u64s, `cellGameGetParamInt` DISC type, open PSID, `cellRtc`, save-data user paths, `cellJpgDecDecodeData`, `cellPngDec`, and the `cellAudio` read index as a BE u64 — *[@sagemono](https://github.com/sagemono)* (#79, #81)
- **`cellFsSdataOpen` with real SDATA/EDAT (NPD) decryption**, `cellFsMkdir` + the game-data dir created where the VFS looks, and a `/dev_hdd0` overlay onto the installed-update tree (`PS3_HDD0_ROOT`) — *[@sagemono](https://github.com/sagemono)* (#79)
- **Honest state, not silently-faked `CELL_OK`** — a module name matching no file fails; net reports a real offline state with real SDK NP error codes; `sys_time_get_system_time` returns real microseconds instead of a call counter — *[@sagemono](https://github.com/sagemono)* (#79)
- **Guest callbacks receive r3 through r10** — the dispatch hook plumbed only four arguments, so any callback taking more read stale registers — *[@canersaka](https://github.com/canersaka)* (#86)
- **`cellFsReaddir` writes the real 258-byte dirent ABI** + a corrected CellOS generic error-code table (#85), and **`cellGame` firmware parameter IDs, error values and buffer bounds** matched to the SDK (#84) — *[@canersaka](https://github.com/canersaka)*
- **`cellAudio` clears each consumed ring-buffer block** — an underrun replayed the previous block instead of silence — *[@canersaka](https://github.com/canersaka)* (#83)

**lv2**
- **One authoritative semaphore count** — the count lived twice (in `sys_semaphore_info::value` and in the Win32 handle), so timeout/wake races could lose tokens or park a waiter while `value` was already positive. `value` under `value_lock` is now the only count, the handle is a wake channel, and overflow returns lv2 `CELL_EBUSY`. The `sync_stress` target is wired into the build so this stays executable outside a generated title — *[@canersaka](https://github.com/canersaka)* (#88)

**Lift & decode**
- **SPU-lifter faithful adoption** — a from-scratch re-derivation of the SPU decode/lift + runtime, consolidating the interpreter and function registry into `spu_channels.c`, plus LBP SPU/SPURS bring-up and RSX texture support (G8B8/DXT, deswizzle). Credit for the underlying decoder/lifter design belongs to [@sp00nznet](https://github.com/sp00nznet) and [@canersaka](https://github.com/canersaka) — *[@sagemono](https://github.com/sagemono)* (#82)
- **SPU `sync`/`dsync`/`syncc` emit real host memory fences**, with the `sync` C bit decoded instead of folded into plain `sync` — *[@canersaka](https://github.com/canersaka)* (#87)
- **Jump-table discovery no longer silently skips every table** — a `toc` list was concatenated as an int, and the caller swallowed the exception; plus `rA=0` base resolution and generic `ld`/offset-table discovery (#78, #81)

### v0.7.0 — *"First Draw"* (July 2026)
<!-- TODO(maintainer): version + codename are a guess -- rename freely. -->
*The RSX draw engine lands. [@sagemono](https://github.com/sagemono)'s renderer executes the title's own NV4097 vertex and fragment programs on D3D12, and **You Don't Know Jack now renders** — a real double-buffered flip loop, verified by booting it. Plus [@canersaka](https://github.com/canersaka)'s PPU/SPU correctness batch, and the SPU image-import path SPURS titles need before any SPU work can run.*

**Rendering**
- **Live D3D12 draw engine** — executes the guest's real vertex program (pixel-perfect dbgfont text), then guest fragment-program pipelines, per-draw VP constants and textures, indexed draws, render-to-texture, MRT, multi-unit textures, FP predication/branches, float RTs, and a VS cache keyed by ucode hash — *[@sagemono](https://github.com/sagemono)* (#43, #76)
- **Present correctness** — present only on guest flip (partial-frame flicker), present at the clear boundary (ring-wrap blink), flips ordered against the drain, honest flip status — *[@sagemono](https://github.com/sagemono)*
- **GCM FIFO ring recycle on wrap** — the command ring never recycled (the context callback OPD was left null), so titles wedged ~200 frames in and the render path aborted with the infamous `0xC708C708`. A real command-buffer-full callback now drains and resets the ring — *[@sagemono](https://github.com/sagemono)*

**Lift & decode**
- **`blrl` dispatched as an indirect call** — it was lumped with `blr` and emitted a bare `return;`, dropping the call *and* skipping the frame epilogue, so `r1` leaked the frame size on every function-descriptor call — *[@sagemono](https://github.com/sagemono)* (#42)
- **EBOOT (`ET_EXEC`) import parsing** — the lib-stub table was only reachable via the PRX `module_info` path, so every retail main executable reported 0 imports; now located via `PT_PROC_PRX_PARAM`. Minecraft: 0 → 345 NIDs across 30 modules — *[@sagemono](https://github.com/sagemono)* (#41)
- **`lift_function` indexed by address** — an O(refs·N) mid-function pass hung the lifter indefinitely after "100% lifted" on large titles; now bisect over a cached sorted index — *[@sagemono](https://github.com/sagemono)* (#41)
- **Rotate-by-0 undefined behaviour** — `v >> (64 - n)` at n=0 is UB, and clang miscompiled the `clrldi` truncation idiom every 32-bit address cast lowers to — *[@sagemono](https://github.com/sagemono)* (#41)
- **Lifter torture suite** — 3,793 KATs against an independent PPC model — *[@sagemono](https://github.com/sagemono)* (#64)
- **VMX element-wise ops read big-endian lanes** — values were byte-reversed on x86, so every float lane op quietly computed on the wrong data — *[@canersaka](https://github.com/canersaka)* (#74)
- **D-form loads/stores treat `rA=0` as a literal zero base**, not `GPR[0]` (#72); **update forms write back `rA`** for indexed stores (#71) and FP loads/stores (#73) — dropping the write-back leaves `rA` stale and corrupts every subsequent `(rA)`-relative access — *[@canersaka](https://github.com/canersaka)*
- **`lmw`/`stmw`** (#68), **`mulldo`** overflow form (#70), **`stvlx`/`stvrx`/`stvlxl`/`stvrxl`** Cell unaligned vector stores (#61), **value-verified CAS** for `lwarx`/`stwcx` and `ldarx`/`stdcx` (#62), and an **FP correctness batch** — `fcmpu` NaN ordering, `fcti` saturation, single rounding, `vctsxs`/`vctuxs` (#60) — *[@canersaka](https://github.com/canersaka)*
- **`fpscr` bit ops + `vrsave` decoded as visible no-ops** — `mtfsb0`/`mtfsb1`/`mtfsfi` were undecoded and emitted as a raw `.word`, an invisible dropped instruction — *[@canersaka](https://github.com/canersaka)* (#69)
- **SPU**: `brsl` target parse broken by the disasm link-register fix, so every call lifted as a silent no-op (#58); `bisl`/`bisled` target register; single-round `fma`/`fms`/`fnms`, `fesd`/`frds` from the left word slot, `fi`/`frest`/`frsqest` interpolation (#51) — *[@canersaka](https://github.com/canersaka)*

**Runtime & lv2**
- **`sys_spu_image_import` implemented + SPU-image syscall numbers fixed** — import was mis-numbered 157 (that's `_sys_spu_image_import`) and collided with `SYS_SPU_IMAGE_CLOSE`, so a title's import call dispatched to the close stub and the image struct was left zeroed (entry=0 → matched no fallback → the SPU thread "instantly completed"). Replaced the zero-init stub with a real SPU-ELF32 `PT_LOAD` → `sys_spu_segment[]` parser — *[@sagemono](https://github.com/sagemono)* (#57)
- **`_sys_spu_image_import` (sysPrxForUser NID `0xEBE5F72F`)** — the user-space wrapper `libsre` invokes during `cellSpursInitialize`; previously unresolved, so the SPURS SPU kernel image was never parsed and the SPU threads came up at entry 0
- **18 lv2 syscall numbers corrected** against the SDK's own `sys/syscall.h` (475.001) — the authoritative table. Three were live collisions: the lwmutex block sat on `SYS_RAW_SPU_CREATE_INTERRUPT_TAG`/`SET_INT_MASK`/`GET_INT_MASK` (150–152) and lwcond on `SYS_SPU_IMAGE_OPEN` (156). Now 125/125 match the SDK, 0 duplicates — *[@canersaka](https://github.com/canersaka)* (#65)
- **`sys_mutex_trylock`** recursive fast path pairs the host critical section (#67); **`sys_spu_thread_group_create`** reads r5 as priority and the name from the attr struct (#66) — *[@canersaka](https://github.com/canersaka)*
- **PS3 process-entry registers** (TLS descriptor + page size) and a function registry sized for >64k-function titles — *[@sagemono](https://github.com/sagemono)*

### v0.6.7 — *"Fine Print"* (July 2026)
*The second half of the two-port review pass: [@canersaka](https://github.com/canersaka)'s remaining decode/ABI/timing fixes plus a lifter/HLE audit toolkit, and the title-agnostic robustness fixes from [@JonathanDC64](https://github.com/JonathanDC64)'s port that could be ported and build-verified cleanly.*

**Lift & decode**
- **NV40 VP/FP shader decompilers hardened** against retail-game shaders (LIT/SFL/STR/ARL/RCC/RSQ, relative constant addressing, all 16 ATTR inputs, viewport transform; FP masked-store fix) — *[@canersaka](https://github.com/canersaka)* (#47)
- **`spu_disasm`**: channel field widened 5→7 bits (channels ≥32 no longer wrap), hbra/hbrr field layout, `bisl`/`brsl`/`brasl` link register shown, `ri7` sign-extension, stop-code preserved — *[@canersaka](https://github.com/canersaka)* (#49)
- **SPU `stop` signal code preserved** to `ctx->stop_code` — SPURS leaf tasks invoke kernel syscalls via `stop <code>` (EXIT/YIELD/WAIT/…), which was being discarded — *[@JonathanDC64](https://github.com/JonathanDC64)*

**Runtime & lv2**
- **Sub-millisecond timeouts extended** to `sys_event_flag`, `sys_semaphore`, and `sys_mutex` timed waits (the same 15.6 ms-granularity fix as v0.6.5's event-queue path) — *[@canersaka](https://github.com/canersaka)* (#50)
- **`g_active_ctx` restored after guest callbacks** — it was left dangling at a stack-local scratch ctx, corrupting the crash handler / diagnostics; applied to both `ppu_guest_call` and `ppu_guest_call_ct` — *[@JonathanDC64](https://github.com/JonathanDC64)*

**HLE**
- **Corrected ABI signatures** for cellPamf, cellSail, cellHttp/cellHttpUtil/cellHttps/cellSsl, cellNet/sysNet, sceNpTrophy, and cellGameData (arg order/count/width → correct register mapping) — *[@canersaka](https://github.com/canersaka)* (#55)
- **`cellGameDataCheckCreate2`** (NID `0xC9645C41`) implemented — marshals StatGet/StatSet and fires the guest `funcStat` callback — *[@JonathanDC64](https://github.com/JonathanDC64)*

**Tooling**
- **Lifter/HLE audit kit** — PPU/SPU decode cross-checks vs Capstone / an RPCS3-output oracle, HLE ABI + dispatch-classify audits, an endianness heuristic, a lift-structure census, and an lv2 sync stress test — *[@canersaka](https://github.com/canersaka)* (#56)

### v0.6.6 — *"Two Ports, One Toolkit"* (July 2026)
*Two independent retail-game ports kept fuzzing the toolkit: [@canersaka](https://github.com/canersaka)'s **Yakuza: Dead Souls** and [@JonathanDC64](https://github.com/JonathanDC64)'s **Demon's Souls**. This release distills the title-agnostic correctness/robustness wins each surfaced — the port-specific scaffolding stays in the forks.*

**PPU / SPU lift**
- **`fsqrt`/`fsqrts` source-register decode** + **`vspltis`** printed as a signed immediate (`ppu_disasm`) — *[@canersaka](https://github.com/canersaka)* (#46)
- **`addeo`/`subfeo`/`mulhwo`/`mulhwuo` overflow forms** (previously fell through to a no-op stub) + **`vupklsb`/`vupkhsb`/`vupklsh`** unpacks — *[@canersaka](https://github.com/canersaka)* (#52)
- **Cross-function SPU tail calls forced with `musttail`** — a guest loop whose back-edge crosses a lifted-function boundary was nesting one host C frame per iteration and silently overflowing the stack (a stack-overflow SE runs no unhandled filter, so the process died with no log and exit code 0); now an O(1)-stack jump under clang — *[@JonathanDC64](https://github.com/JonathanDC64)*
- **Mid-function / gap lifting sliced O(n²)→O(span)** — a ~40-minute no-output hang on 96k+ function titles is now bounded work — *[@JonathanDC64](https://github.com/JonathanDC64)*

**Runtime & lv2**
- **Sub-millisecond `sys_timer` usleep** — was a single `SwitchToThread()`, so `usleep(<1000)` was a no-op; now paces to a QPC deadline — *[@canersaka](https://github.com/canersaka)* (#44)
- **`sys_event_queue_receive` returns the event in r4–r7** (lv2 ABI), not just the guest memory buffer — callers reading the registers saw stale values — *[@JonathanDC64](https://github.com/JonathanDC64)*
- **`sys_memory_get_page_attribute`** renumbered 358→**351** (0x15F) and implemented — *[@JonathanDC64](https://github.com/JonathanDC64)*
- **`CellFsStat` runtime-side layout** corrected to the 52-byte / 4-byte-aligned ABI — fixes the `ppu_fs.cpp` / `sys_fs.c` stat writers the v0.6.5 libs-side packing fix didn't cover — *[@JonathanDC64](https://github.com/JonathanDC64)*

**HLE libs**
- **`cellNetCtl`** integer out-params written big-endian — *[@canersaka](https://github.com/canersaka)* (#45)
- **`cellAudio`** delivers real period events to the registered notify queues (games blocked on the audio event now wake) — *[@canersaka](https://github.com/canersaka)* (#54)

### v0.6.5 — *"The Fifteen-Millisecond Tax"* (July 2026)
*Driving **You Don't Know Jack** (Scaleform UI + FMOD, 5,859 functions) from an instant crash to its running main loop surfaced one global performance bug worth more than any single lift fix — plus a batch of community lifter/HLE correctness PRs.*

**Runtime — the event-poll timer-resolution fix** (`sys_event.c`, `tests/boot_main.cpp`):
- Titles that poll an event queue every frame with a **sub-millisecond timeout** (YDKJ uses 30 µs) were throttled ~**500× game-wide**. `sys_event_queue_receive` floored the timeout to a 1 ms `SleepConditionVariableCS`, but Windows' default ~15.6 ms timer granularity inflates *any* sub-15 ms wait to a full tick — a two-queue-per-frame poll loop burned ~30 ms/frame doing nothing. Sub-millisecond timeouts now do a true non-blocking check (immediate `ETIMEDOUT` when empty), and the harness calls `timeBeginPeriod(1)`. YDKJ went from ~0.5 fps effective to ~60 Hz, reaching online-init ~6× faster.
- GCM FIFO drain moved into the 60 Hz vblank tick, so RSX sync-fence labels advance regardless of `present()` latency.

**Community PRs incorporated** — thank you:
- **cellFs big-endian out-params + `CellFsStat` PS3 packing** — *[@canersaka](https://github.com/canersaka)* (#22)
- **cellGame reads the real title id from `PARAM.SFO`** — *[@canersaka](https://github.com/canersaka)* (#24)
- **`sys_rwlock` → `EDEADLK`/`EPERM`** on writer self-relock / bad unlock — *[@canersaka](https://github.com/canersaka)* (#25)
- **`ppu_disasm`: opcodes 33/225 are `crnor`/`crnand`** (were swapped) — *[@canersaka](https://github.com/canersaka)* (#40)
- **Static firmware LLE** (`tools/lift_prx.py` + `docs/FIRMWARE_LLE.md`) — relocate a decrypted PRX and lift the real firmware module (e.g. the libsre SPURS kernel) instead of HLE-ing it; a bring-your-own-firmware method that ships no firmware — *[@canersaka](https://github.com/canersaka)* (#53)
- **cellPad DIGITAL2 face-button packing + analog-Y reflect-about-128** — *[@sagemono](https://github.com/sagemono)* (#42)

**You Don't Know Jack port** — now public at [sp00nznet/youdontknowjack](https://github.com/sp00nznet/youdontknowjack): boots the full init stack to its **main game loop** with a live D3D12 clear+flip at 60 Hz. Current frontier: the GCM command buffer's ring-wrap flush callback is `null` and a computed `0xC708C708` poison reaches the scene-graph dispatch — so only black clears render so far.

### v0.6.4 — *"Carry the One"* (July 2026)
*A pass of hard-won correctness fixes surfaced by driving flOw (PhyreEngine, ~104k functions) deep into its boot — plus the SPU-side plumbing to get a SPURS taskset actually dispatching. Most of these are silent-corruption bugs: the lift produced valid C for the *wrong* computation, so nothing crashed until a data structure quietly filled with garbage thousands of frames later.*

**Community PRs incorporated** — thank you [@canersaka](https://github.com/canersaka), who found and fixed all of the below and sent them in as pull requests:

*PPU lift*
- **XER[CA] for the shift-algebraic ops** (`sraw`/`srad`/`srawi`/`sradi`) + `mtcrf` field mask — (#21)
- **XER[CA] for `subfe`/`subfme`/`addme`** — (#26)
- **`cntlzw(0)` is 32**, not undefined `__builtin_clz` garbage — (#35)
- **PPU lifter conformance suite** + six emission fixes it found — (#37)
- **`vcmpgtsb`/`sh`/`sw`/`ub`/`uh`/`uw` handlers** (dot forms set CR6) — (#39)

*SPU lift*
- **`bi $r0` reloaded via `lqa`/`lqr` is a computed tail jump**, not a return — (#36)
- **self-referential SPU branch mislifts** — (#30)
- **complete SPU ISA coverage** (all 199 ops + double-precision) — (#31)
- **byte-correct SPU quadword helpers** (`shufb`, `cbd`/`chd`/`cwd`/`cdd`) — (#32)
- **`il` negative-immediate double sign-extension** — (#33)
- **preferred-slot-only link register + `rchcnt`** — (#34)

*Runtime*
- **`mftb`/`mftbu` read a real timebase** — (#38)

**PPU lift — the carry/borrow bug class** (`ppu_lifter.py`):
- **64-bit `add` never wrote XER[CA]** and truncated the carry-out to 32 bits. Any `adde`/`addze`/`subfe` consumer downstream (bignum math, 64-bit pointer arithmetic, the CRT's own `__eabi` helpers) then read a stale carry. Now sets CA from the true unsigned 64-bit carry-out.
- **`subfic` computed a 32-bit result and dropped the borrow** — rewritten as `EXTS(SI) - RA` over the full 64 bits with `XER[CA] = NOT borrow` (`EXTS(SI) >= RA`).
- **`subfme` was missing its XER[CA] update entirely**; `adde`/`addme` carry-out recomputed to match the PowerISA / RPCS3 `add64_flags` ADC semantics.
- **`sraw`/`srad`/`srawi`/`sradi` now set XER[CA]** (was never written) via new `ppc_sraw`/`ppc_srad` helpers — CA = `rS<0 AND any 1-bits shifted out`, which feeds the same `adde`/`subfe` consumers.
- **Fall-through tail-call / computed-`bctr` dispatch leaked the frame**: these were lifted as `ps3_indirect_call(ctx); return;` *without* the function epilogue, leaking `r1` and the callee-saved GPRs on every jump-table `switch` and tail call. Now emits the full epilogue before the tail jump.
- **`ppu_disasm.py --va`**: disassemble a window at a *virtual* address, mapping va→file offset through the ELF's `PT_LOAD` segments — fixing the long-standing footgun where `--raw --offset <vaddr>` read the wrong bytes (off by the `.text` segment base).

**SPU lift — computed jumps & wide ops** (`spu_lifter.py`, `spu_disasm.py`):
- **`bi $r0` computed tail-jumps vs. returns**: a `bi $r0` whose `r0` was just reloaded via `lqa`/`lqr` is a *computed tail jump*, not a return. Treating them all as returns made the SPURS taskset launcher fall through and the dispatcher loop forever — the exact SPU analogue of the PPU tail-call bug above. `compute_bi_r0_jumps()` now classifies them by backward-scanning for the nearest `r0` writer.
- **RRR 4-operand destination register** decoded from bits 21–27 (the low 7 bits are the `RC` *source*, not the dest) — every `selb`/`shufb`/`fma`-family op wrote the wrong register.
- **Double-precision family**: `fesd`/`frds` single↔double convert, and the `dfma`/`dfms`/`dfnms`/`dfnma` double FMA ops (3-register, `RT` is the accumulator).
- **`cgx` extended carry-generate** (RT is also a source), **`mpyhha`/`mpyhhau`** high-half multiply-accumulate, and a **`br .` self-loop trap** (target == self is a deliberate infinite-hang trap on real SPU, not forward progress).

**Runtime robustness**:
- **PPU function-hash table widened** (`ppu_loader.cpp`, `PPU_HASH_BITS` 16→18): the open-addressed registration table live-locked filling past 65,536 functions — flOw registers ~104k. Titles above ~50k functions now register in bounded time.
- **`/app_home/` VFS mapping** (`sys_fs.c`): the install-dir prefix (opened as `/app_home/...`, `app_home/...`, `e:/app_home/...`) now maps to the USRDIR root instead of appending a literal `app_home` directory that isn't on disk.
- **Page-guard watchpoint** (`ppu_guard_page()`, Windows): arm a guest EA and a VEH logs the faulting host RIP of any raw store into that 4 KB page — catches vector/`memcpy` stores that `vm_write*` instrumentation can't see. Env-gated; a diagnostic aid, off by default.
- **`cellGcmGetTiledPitchSize` ABI fix** + real flip/vblank handler dispatch (see prior commits) — the wrong return convention was corrupting the caller's stack.

**SPU SPURS plumbing**: new `libs/spurs/spurs_taskset.{c,h}` + `spurs_pm.c` (taskset descriptor + power-management scaffolding) and `runtime/spu/` workload-dispatch wiring toward running a real SPURS task kernel. Still in progress — the taskset dispatches but completion-event delivery back to the PPU isn't verified yet.

**Tooling**: `tools/test_ppu_lift.py` (PPU lifter unit harness) and `tools/rpcs3_probe/` oracle-introspection scripts used to diff our lift against the running RPCS3 reference.

### v0.6.3 — *"Knowing the Names"* (June 2026)
- **NID resolution, doubled+**: `nid_database` now auto-scans `libs/` + `runtime/` for every function the toolkit implements (`load_implemented()`), and ships 44 more curated names recovered by exact-NID brute-force against the harness corpus. Resolution went from 175 → 421 of the 660 distinct NIDs the sample titles import; the harness stub-prioritization ranking is now fully named through the high-frequency tiers.
- **Harness stub-prioritization ranking**: a new non-gating `imports` stage (via `ppu_loader`'s `proc_prx_param` → libstub walk) extracts each EBOOT's firmware imports and resolves them, and the report ranks the most-imported libraries and functions across titles — the data that says which stubs to write next.
- **`cellNetCtl` net-start dialog** implemented (`LoadAsync`/`UnloadAsync`, imported by every sampled title): posts the `LOADED`/`FINISHED` sysutil callback events so games clear the boot-time "connect to PSN" gate, and reports a connected result.
- **`cellGcmSetupContext()`** — reusable core of `_cellGcmInitBody` (the function `cellGcmInit`'s SDK macro actually calls). Captures the proven `CellGcmContextData` layout (begin/end/current/callback) from the shipping Simpsons port so game bridges stop hand-rolling it.
- **`.opd` function detection hardened**: the TOC base may live in BSS / a segment gap, so detection no longer requires it inside a file-backed range — recovered ~20–23k address-taken functions each on flOw and Marvel Ultimate Alliance.

### v0.6.2 — *"Boot Sequence"* (June 2026)
- **PPU boot scaffold** (thanks [@pauloadrianoalves](https://github.com/pauloadrianoalves), PR #3): `runtime/ppu/` (loader, HLE dispatch, sysprx, filesystem) + `runtime/host/host_main.c` — the per-game path that loads a lifted PPU image, links it with the HLE runtime, and boots it. Built per-game against the lifter-generated `ppu_recomp.h`, so it's excluded from the game-agnostic runtime library. See `docs/PPU_RECOMP.md`.
- **RSX shader decompilers** (thanks [@pauloadrianoalves](https://github.com/pauloadrianoalves)): `rsx_fp_decompiler` / `rsx_vp_decompiler` translate NV40 fragment/vertex programs to host shaders, with a validation-test corpus. See `docs/RSX_FRAGMENT_PROGRAM.md`.
- **PPU tooling**: `tools/ppu_loader.py` (image manifest / OPD table / TOC / imports) and `tools/gen_hle_nids.py`.
- *Note:* PR #3's SPU lifter/disassembler and `nid_database` changes were **not** taken — they predate and would regress the v0.6.0 SPU subsystem and the v0.6.1 NID fix.

### v0.6.1 — *"Many Hands"* (June 2026)
*The community showed up. Most of this release came in over the wall as pull requests — almost all of it discovered by [**@canersaka**](https://github.com/canersaka) while stress-testing the toolkit against a 22 MB / ~45,000-function **Yakuza: Dead Souls** port, which turns out to be a fantastic fuzzer for everything we got subtly wrong. Huge thanks to every contributor below.*

**Correctness — decode & lift** (thanks [@canersaka](https://github.com/canersaka)):
- **NID computation, fixed** (`nid_database.py`): the suffix constant was truncated to 12 bytes and corrupted past byte 8, and the digest was read big-endian. The authoritative 16-byte suffix + little-endian read now matches real EBOOT import tables — `cellSysmoduleLoadModule` → `0x32267A31`. Previously matched **0 of 354** import NIDs on a real game; now 343/354 resolve. (Our own `include/ps3emu/nid.h` already documented the correct values; the Python tool just disagreed with it.)
- **VMX/AltiVec decode tables, cross-referenced against the Power ISA manuals** (`ppu_disasm.py`): dozens of mnemonics mapped to the wrong codes (`vmaxfp`/`vminfp`, `vcfsx`/`vcfux` swapped with `vrefp`/`vrsqrtefp`, the whole `vpk*` pack family permuted, signed/unsigned `vcmpgt*` swapped, `vsumsws`, …). These decoded *silently* and lifted to valid C for the wrong operation — `vcfsx` alone was wrong 9,728× in one title.
- **VMX lifter handlers**: `addc`/`subfc` with carry-out into XER[CA] (86% of all unlifted instructions in Yakuza — 63k of 73k), the unaligned vector loads `lvlx`/`lvrx`/`lvlxl`, `ldbrx`, `vsrw`, `vsububs`, `vsum2sws`, `vupkhsh`, `vrfim`, and `vand` (which had an implementation but was missing from the dispatch list).
- **`sradi` decode for shifts ≥ 32**: the 6-bit shift carries its top bit in instruction bit 30, so the XO field reads 827 (which was unmapped) for shifts of 32+. Now composed correctly. *(Follow-up: removed a dead, incorrect `XO 827 → lhaux` block — real `lhaux` is XO 375.)*
- **Function detection seeded from the `.opd` table** (`find_functions.py`): PS3 executables list a descriptor for every address-taken function (every C++ virtual, every callback) — 21,893 of them in Yakuza, of which exactly **1** was being detected by prologue scanning. Now seeds starts from `.opd` (located by shape, since section names are often stripped) + the ELF entry, bounds them at the first `blr`, and filters phantom branch targets from data decoded as code.
- **Lifter parallelization + chunked output**: per-function translation now fans out across a process pool (`--jobs N`) — a multi-hour single-core lift on a 45k-function game drops to minutes. Plus record-form CR0 handling, runtime-matched context, and split-file C output so the generated source is actually buildable.

**Runtime & libs** (thanks [@canersaka](https://github.com/canersaka)):
- **`sys_vm` syscall family (300–313)** implemented — `sys_vm_memory_map` returned ENOSYS with an unwritten out-pointer, crashing callers on first deref. Mappings now bump-allocated from a committed 0x60000000 window.
- **`sys_memory` allocations** now come from a committed-on-demand 0x40000000–0x50000000 window (the old bump base at 0x20000000 failed every allocate with silent ENOMEM); addresses match real-hardware traces byte-for-byte, and the bump pointer is now thread-safe.
- **`sys_lwmutex_destroy` lv2 semantics**: returns ESRCH for a dead mutex / EBUSY for a held one (libc teardown branches on these) instead of unconditional OK; slot allocation is now locked.
- **TTY syscall numbers** corrected to 402/403 (`lv2_syscall_table.h` had 400/402).
- **Big-endian guest structs in `cellVideoOut`** + corrected `CellGcmContextData` layout (callback at +0xC): `GetResolution` was writing host-endian, so the guest read 720×480 as 53250×57345 and sized display buffers from garbage.

**Build & tooling**:
- **Linux/GCC build fixed** (thanks [@LucasPicoli](https://github.com/LucasPicoli)): glibc's `st_atime`/`st_mtime` macros were shadowing `CellFsStat` members; five translation units now compile clean so downstream projects get a Linux runtime to link against.
- **Runtime test harnesses excluded from the library build** (thanks [@canersaka](https://github.com/canersaka)): `runtime/spu/tests/` defines its own `main()`; the library now builds clean with MSVC.
- **`tools/show_func.py`** (thanks [@canersaka](https://github.com/canersaka)): dump a single lifted function's C, its original PowerPC disassembly, or both, straight from the chunked output.

### v0.6.0 — *"SPU, For Real This Time"* (June 2026)
- **SPU recompilation, corrected end-to-end**: rebuilt `spu_disasm.py`'s opcode tables from rpcs3's authoritative SPUOpcodes.h. Pervasive mis-decodes (lqr/stqr/fsmbi were excluded from RI16 and shadowed by spurious RI10 rotate entries; RI16 branch/load forms at wrong opcodes; ~15 wrong SPU_RR entries; missing RI8 float conversions) had silently corrupted *every* SPU lift.
- **SPU lifter fall-through fix**: `spu_lifter.py` now emits a tail-call when a function ends in a non-branch — previously such functions fell off the end and truncated execution mid-image. Added cflts/cfltu/csflt/cuflt float conversions.
- **Per-image SPU dispatch**: added `image_id` to the SPU context + an image-tagged function registry (`spu_channels.c`, `spu_begin_image()`), so a SPURS kernel, a policy module, and a job can coexist at overlapping local-store addresses.
- **SPU tooling**: `extract_spu_images.py` (pull SPU images from PRX/ELF), `find_spu_functions.py` (SPU function bounds), `wrap_spu_elf.py` (wrap raw SPU blobs).
- **SPU runtime tests**: self-contained sum / shufb / DMA / brsl-return tests under `runtime/spu/tests/`, plus `test_spu_helpers.c`. New `docs/SPU_LIFTER.md`.
- **New port target — The Simpsons Arcade Game** (NPUB30563): a Konami arcade-emulator EBOOT that drove all of the above. Boots through the CRT to full GCM init with a live D3D12 window; rendering is gated behind the game's CRI-middleware-on-SPURS task pipeline.

### v0.5.1 — *"All-Register FIFO"* (April 2026)
- **All-register FIFO flag clearing**: PhyreEngine sets +0x18 pending flags on r27/r28/r30/r26 (4 structures). Previously only cleared r31. Now clears all, solving 2 of 3 init spin levels.
- **Watchdog CTR=0 patching**: Background thread detects NULL function pointer spins, patches heap objects with NOP OPDs.
- **D3D12 backend active**: Device FL11.0, vertex-colored PSO, batched DrawInstanced, VSync present. First GPU-rendered geometry from PS3 recomp.
- **Level data rendering**: flOw XML level definitions parsed — authentic ocean gradient, snake creatures, food objects, particles. 486 vertices per frame.
- **Batched DRAW_ARRAYS**: RSX method format caps at 255 verts per command. Auto-batching splits larger draws.
- **Control register host-endian**: RSX MMIO accessed via lwbrx/stwbrx in recompiled code. Fixed endianness for put/get/ref.
- **flOw progress**: D3D12 renders Level 3 scene at ~17fps. 12 subsystems ticking. Full GCM pipeline: NV40 → RSX processor → D3D12. Render context init blocked by 3rd-level data loop (no HLE escape).

### v0.5.0 — *"Pixels on Screen"* (April 2026)
- **RSX command buffer processing wired up**: `gcm_flush_guest_cmdbuf()` now initializes RSX state via `rsx_state_init()`, passes NV40 commands to `rsx_process_command_buffer()`, and dispatches to the backend. Games can write GCM commands and see results on screen.
- **FIFO watchdog thread**: Background thread monitors `ctrl->put` for changes, scans command buffer for SET_REFERENCE (method 0x0050) and WRITE_BACK_END_LABEL (0x1D6C), updates `ctrl->get`/`ctrl->ref` automatically. Breaks cellGcmFinish spin loops without needing HLE bridge interception.
- **Control register endianness fix**: RSX DMA control registers (put/get/ref) are MMIO accessed via `stwbrx`/`lwbrx` in recompiled code (host-endian, no byte-swap). The HLE now reads/writes these in host-endian via direct `uint32_t*` access instead of `vm_write32` (which byte-swaps for big-endian).
- **Command buffer IO mapping**: `cellGcmInit` now maps the command buffer region (1MB-aligned) via `cellGcmMapMainMemory` so `cellGcmAddressToOffset` succeeds for buffer addresses. Previously only the game's explicit mapping was registered.
- **AddressToOffset fallback**: Bridge now synthesizes offsets for unmapped addresses in the heap/command buffer range (0xA00000-0x2000000), preventing failures in GCM inline code that converts arbitrary guest addresses to RSX offsets.
- **GCM callback handler upgrade**: Buffer overflow callback now flushes pending commands via `rsx_process_command_buffer`, updates control register put/get, then resets `ctx->current`. Previously only reset current without processing.
- **cellGcmSetFlip error logging**: Bridge logs failed flip commands with buffer ID and error code. Fixed buffer ID passing (game passes context ptr in r3 and buffer ID in r4, but bridge reads r3 as buffer ID).
- **flOw progress**: **Window visible with animated clear color!** Full GCM rendering pipeline working: NV40 clear commands → RSX processor → null backend → Win32 window repaint. 12 subsystems ticking with inline switch case implementation. 1200+ frames, buffer flips alternating 0/1. **Next blocker**: PhyreEngine FIFO sync protocol blocks render context creation — not cellGcmFinish, needs custom sync protocol investigation.

### v0.4.3 — *"Living Code"* (March 2026)
- **Lifter: mid-function tail-entry resolution**: When a trampoline branch targets an address inside an existing function body, the lifter now generates a real tail-entry function containing the code from that address to the function's end. Eliminated ~23K stubs in flOw (91K → 101K real functions). Iterative resolution handles chains of mid-function entries.
- **TLS `__declspec(thread)` fix**: `g_trampoline_fn` declaration mismatch caused MSVC to compile 37K+ writes as direct stores to the read-only TLS template in `.rdata`, crashing with AV WRITE. Fixed by declaring `__declspec(thread)` consistently across all translation units.
- **DRAIN_TRAMPOLINE after all calls**: Lifter now emits the trampoline drain macro after every `bl` (direct call) and `bctrl` (indirect call). Previously only emitted in the preamble; callers that set `g_trampoline_fn` and returned wouldn't have their chains drained.
- **CTR decrement wrap fix**: `bdnz`/`bdz` decremented CTR as uint64, causing loops with 32-bit counts sign-extended to ~0xFFFFFFFF_8XXXXXXX to wrap past zero into 18-quintillion-iteration infinite loops. Fixed: `ctx->ctr = (uint32_t)(ctx->ctr - 1)`.
- **MSVC compatibility in lifter output**: `__builtin_clz` replaced with `_BitScanReverse`, `__int128` multiply replaced with `_mul128`/`_umul128` intrinsics, all function/memory declarations wrapped in `extern "C"` for correct C++ linkage.
- **ppu_context struct expanded**: Added VMX/AltiVec `ppu_vr vr[32]`, `vscr`, `fpscr`, `cia`, `reserve_addr`/`reserve_value`, `thread_id` fields. Enables full VMX vector operations, atomic reservations, and thread identification.
- **sys_lwmutex_reset_all()**: New function to destroy all lwmutex/lwcond state for clean CRT abort redirect. Prevents deadlocks when CRT initialization is interrupted mid-mutex-lock.
- **RLD instruction decode fix** (ppu_disasm.py): MD-form rotate instructions use 3-bit sub-opcode (bits 27-29), MDS-form uses 4-bit (bits 27-30). Was using wrong bit fields, producing incorrect `rldcl`/`rldcr` decoding.
- **Inline VM access optimization**: Game projects can define `vm_read`/`vm_write` as inline functions in ppu_recomp.cpp preamble, bypassing function call overhead. Eliminates ~100x slowdown on byte-by-byte memory operations (critical for CRT memset of multi-GB address spaces).
- **cellSysmodule duplicate fix**: `cellSysmoduleLoadModule` returns CELL_OK for already-loaded modules instead of CELL_SYSMODULE_ERROR_DUPLICATED. Games like flOw treat the error as fatal.
- **OPD-resolved ELF constructors**: Extracted 166 constructor OPD entries from `.ctors` section, resolved through OPD to real function addresses (0x12F14, 0x1CD2C, etc.), and execute them directly. Constructors now allocate PhyreEngine objects (3 objects with malloc + lwmutex_create).
- **flOw progress**: Game boots through CRT, runs 166 constructors (creating PhyreEngine globals), loads all modules (NP, SPURS, USBD, JPGDEC, NET), registers sysutil callback with correct function pointer (0x84C630). PhyreEngine enters early init and makes 10+ memory allocations. 102K+ recompiled functions. **Next blocker**: PhyreEngine asserts on null internal pointers — needs CRT full init or manual global initialization.

### v0.4.2 — *"Main Event"* (March 2026)
- **flOw reaches game main() initialization**: CRT startup completes, abort intercepted via longjmp redirect, game enters `func_000CB9CC` with clean stack. Loads modules (GCM_SYS, SPURS, USBD, JPGDEC, NET), registers sysutil callback. First PS3 recomp project to reach game-level code execution.
- **Trampoline system for split-function chains**: `convert_trampolines.py` and `post_lift.py` transform fallthrough edges between split functions into explicit trampoline calls. When the lifter splits a large function at branch targets, the resulting pieces are chained via trampolines so control flow is preserved without relying on physical code adjacency.
- **DRAIN_TRAMPOLINE pattern**: Macro inserted after every `bl` call site in `ppu_recomp.cpp` (143K+ sites) to drain pending trampoline returns. Prevents unbounded host stack growth when recompiled split-function chains would otherwise nest deeply through call/return mismatches. Converts deep recursion into a flat loop.
- **Manual dispatch stub registration**: `indirect_dispatch.cpp` supports registering stubs for mid-function entry points that the lifter does not emit as standalone functions. Allows `bctrl` indirect calls to reach branch targets inside split functions via hash table lookup.
- **cellSysmodule complete module ID name mapping**: 30+ module IDs now map to correct human-readable names (0x00=NET, 0x0A=SPURS, 0x10=GCM_SYS, 0x17=Camera, 0x1C=USBD, etc.). Previously showed wrong names for several IDs.
- **SEH-based crash recovery**: Three CRT constructors that access unmapped guest memory are caught via structured exception handling and recovered from, allowing startup to continue past constructor failures.
- **malloc override**: Bump allocator at guest address range 0x00A00000-0x10000000 provides malloc/free for recompiled code without requiring full guest heap infrastructure.

### v0.4.1 — *"First Light+"* (March 2026)
- **RPCS3 audit**: Cross-referenced all major modules against RPCS3's implementations. Added 28 cellGcmSys functions, 5 sysPrxForUser functions, 4 cellSpurs functions. cellGcmSys now at 61+ functions (was 33).
- **D3D12 backend**: Real vertex upload from guest memory with BE byte-swap, DrawInstanced, vertex attrib + shader logging, blend/depth/stencil state callbacks, texture format decode (25 RSX→DXGI mappings). 990 lines.
- **Image encoding**: Real PNG + JPEG encoding via stb_image_write v1.16. ARGB→RGBA/RGB swizzle, configurable quality, in-memory encoding.
- **dcbz fix**: `dcbz` (data cache block zero) now zeroes 128 bytes instead of being a no-op. This was THE root cause of the flOw PhyreEngine init stall — dcbz loops ran trillions of no-op iterations.
- **VMX disasm critical fix**: lvx/stvx decode was in unreachable code path. Moved to main opcode 31 block, eliminating 6,620 TODO instructions.
- **VMX disasm duplicate key fix**: VX-form decode table had overlapping keys. Split compares into separate table with Rc bit handling. Added 60+ VMX VX-form instructions.
- **Lifter**: 100+ instruction mnemonics across 18 categories. New: dcbz (real memset), bctrl→ps3_indirect_call, VMX int/float/logical/compare/convert/splat/merge/shift, mcrf, rlwnm, creqv, mffs/mtfsf, byte-reverse loads, load algebraic, all cache/sync ops. **~10K TODOs remaining** (down from 27K).
- **sys_get_random_number**: Real crypto RNG via BCryptGenRandom / /dev/urandom.
- **stb_image_write v1.16**: Vendored for PNG/JPEG encoding.
- **cellSail**: Expanded to 28 functions with descriptor management for Tokyo Jungle.
- **sceNpCommerce2 + cellSysutilNpEula**: New modules for Tokyo Jungle.
- **RSX primitive/vertex/texture mapping**: Complete utility headers for D3D12 translation.
- **Documentation**: 8 docs updated with comprehensive rewrites. RSX_GRAPHICS.md now 16KB with full D3D12 capability matrix, vertex format tables, shader translation strategy.
- **97+ complete modules**, 250+ files, 70K+ lines of code.

### v0.4.0 — *"First Light"* (March 2026)
- **RSX Command Buffer Processor**: New NV47xx GPU command parser (`rsx_commands.h/.c`) — FIFO command buffer parsing, state tracking for surfaces, viewport, scissor, clear, blend, depth/stencil, culling, color mask, alpha test, textures (16 units × 8 registers), vertex attributes (16 × format/offset), shader programs (fragment/vertex), draw arrays + draw indexed. Backend callback interface (`rsx_backend`) with 12 dispatch points for pluggable rendering.
- **Null RSX Backend**: Win32 window backend (`rsx_null_backend.h/.c`) — creates a window, displays RSX clear color via GDI, FPS counter and debug overlay. Proves command pipeline works before D3D12/Vulkan.
- **D3D12 RSX Backend**: Real GPU rendering backend (`rsx_d3d12_backend.h/.c`) — D3D12 device (FL 11.0), DXGI swap chain (double-buffered flip-discard), RTV descriptor heap, fence-based frame sync, clear to RSX color, VSync present. Ready for vertex buffer upload and shader translation.
- **cellPamf**: Upgraded from Partial — real PAMF stream descriptor parsing (AVC profile/level/resolution, ATRAC3+ channels from actual descriptor bytes), EP table navigation via header offsets.
- **cellVdec**: Upgraded callback pipeline — generates PICOUT callbacks with populated CellVdecPicItem (codec type, PTS/DTS, dimensions) after each DecodeAu. Games checking decode completion now proceed correctly.
- **cellAdec**: Same treatment — generates PCMOUT callbacks with channel count, sample rate, bit depth. Silence output but correct pipeline flow.
- **cellDmux**: Improved callback sequencing and AU tracking for proper ES dispatch.
- **cellSpursJq**: Better job submission/completion tracking, TryWait returns BUSY correctly.
- **cellDaisy**: Marked Complete (was already a real ring buffer implementation).
- **Image decoders**: cellPngDec, cellJpgDec, cellGifDec upgraded with real stb_image v2.30 decoding paths, guest memory path translation via `cellfs_translate_path()`.
- **cellSync2**: Timed mutex lock (spin with SwitchToThread/pthread_mutex_timedlock).
- **stb_image.h**: Vendored v2.30 for PNG/JPG/GIF decoding.
- **Lifter**: Split-function fallthrough fix, branch-target function detection, bctrl→ps3_indirect_call dispatch. Scalar: rldcl/rldcr, addze/addme/subfze/subfme, mulld/divd/mulhd/mulhdu, adde/subfe, cror/crand/crnand/crxor/crnor/creqv, mcrf, rlwnm, stdux/ldux, lwarx/stwcx./ldarx/stdcx., lwbrx/stwbrx/lhbrx/sthbrx, lwax/lhaux, mftb/mftbu, lswi/stswi, tw/td, mffs/mtfsf, cache/sync no-ops. **VMX/AltiVec**: lvx/stvx/lvebx/stvebx/lvsl/lvsr, vperm, vmaddfp/vnmsubfp/vaddfp/vsubfp, vsel, vsldoi, vmsumshm, vand/vandc/vor/vxor/vnor, vaddubm/vadduhm/vadduwm/vsububm/vsubuhm/vsubuwm, vmaxub-sw/vminub-sw/vmaxfp/vminfp, vspltb/vsplth/vspltw/vspltisb/vspltish/vspltisw, vmrghb-w/vmrglb-w, vcfsx/vcfux/vctsxs/vctuxs, vrefp/vrsqrtefp, vcmpeqfp/vcmpgefp/vcmpgtfp/vcmpbfp/vcmpequb-w/vcmpgtub-w/vcmpgtsb-w. **~20K fewer TODO instructions on relift** (27K→~7K estimated).
- **Disassembler**: XO-form arithmetic split (addze/addme/subfze/subfme), rldcl/rldcr register-shift rotate, byte-reverse loads, load algebraic, mftb, string word, VMX shift helpers.
- **Runtime**: sys_tty_write/read (syscall 402/403), unimplemented syscall logging, WASAPI COM GUID fix, all duplicate symbol warnings eliminated.
- **Documentation**: CONTRIBUTING.md, docs/CUSTOM_MODULES.md, docs/FAQ.md, updated case study with flOw v0.4.0 technical lessons.
- **SPURS**: Upgraded from Partial to Complete (management + event flags + job queues + Daisy FIFO all functional).
- **95 complete modules**, only media decode (cellVdec/cellAdec need FFmpeg) remains truly partial.

### v0.3.1 — *"Finishing the Sweep"* (March 2026)
- **16 new modules** — clearing out nearly every remaining "Not Started" module
- **cellFontFT**: FreeType font rendering — 16 font slots, fallback metrics (ascender=0.8×size), glyph bitmap stubs, kerning
- **cellFreeType**: FreeType2 library wrapper — reports version 2.4.12 (PS3 firmware 4.x)
- **cellSpursJq**: SPURS Job Queue — queue create/destroy, job push (completes immediately), wait, port system, 16 max queues
- **cellDaisy**: SPURS Daisy Chain FIFO pipes — **real ring buffer** with push/pop, 32 max pipes, 256 max depth, count/free queries
- **cellHttps**: HTTPS client — TLS stub with CA cert and client cert management, verify level config, 8 handles
- **cellSubdisplay**: PS Vita Remote Play — init/start/stop lifecycle, always reports not connected, empty touch data
- **cellImeJp**: Japanese IME — character input, backspace, confirm, passthrough conversion (no kanji dictionary), 4 handles
- **cellVideoExport**: Video export to XMB — init/end, export fires NOT_SUPPORTED callback
- **cellMusicExport**: Music export to XMB — init/end, export fires NOT_SUPPORTED callback
- **cellGameRecording**: In-game video recording — start/stop/pause/resume state tracking, no actual capture
- **cellAdecAtrac3p**: ATRAC3+ audio decoder — 2048 samples/frame, mono/stereo, 44.1/48kHz, outputs silence
- **cellAdecCelp8**: CELP8 voice codec — 160 samples/frame @ 8kHz mono, multiple bitrate modes, outputs silence
- **cellVdecDivx**: DivX video decoder — Mobile/Home/HD profiles, outputs black frames
- **cellPhotoExport / cellPhotoImport**: Photo export/import to/from XMB — NOT_SUPPORTED stubs with callbacks
- **cellPrint**: USB printer support — init/end, reports 0 printers
- **cellRemotePlay**: Remote Play availability — init/end, always unavailable
- **93 complete modules** (up from 77), only ~3 niche modules remain unstarted

### v0.3.0 — *"Full Metal RSX"* (March 2026)
- **cellGcmSys**: Major upgrade from Partial to Complete — command buffer control (put/get/ref), local memory bump allocator, IO memory mapping with proper offset table population (1MB page granularity), flip handler + VBlank callbacks, 15 tile slots + 8 zcull regions, 256 report data + label slots, platform-native timestamps, GetTiledPitchSize, 27+ functions total
- **cellHttp**: Major upgrade from Partial to Complete — real HTTP/1.1 via native sockets (Winsock2/POSIX), DNS resolution via getaddrinfo, TCP connect, request formatting with custom headers, response header parsing (status code, Content-Length, Connection: close), streaming body receive, per-transaction socket lifecycle, SO_RCVTIMEO/SO_SNDTIMEO timeouts
- **cellSpurs**: Event flags now use real OS blocking — CRITICAL_SECTION + CONDITION_VARIABLE (Windows) / pthread_mutex + pthread_cond (POSIX) via side table, EventFlagWait truly blocks until condition met, broadcast on EventFlagSet
- **77 complete modules** (up from 75), 3 remaining partial (cellVdec/cellAdec need FFmpeg, cellDmux/cellSpurs management-only)

### v0.2.5 — *"The Long Tail"* (March 2026)
- **13 more modules** — mopping up the remaining "Not Started" list
- **cellFsUtility**: Recursive mkdir, file read/write/copy/size/exists helpers
- **cellSail**: Media player lifecycle — state machine with immediate finish (stub, no actual playback)
- **cellVoice**: Voice chat port management — create/delete/connect, no actual voice data
- **cellMic**: Microphone — reports no device attached
- **cellRudp**: Reliable UDP — context management, connect/send return NOT_CONNECTED
- **sceNpClans**: NP clan system — create/join/leave/search (offline stub)
- **cellMusicDecode** / **cellMusicDecode2**: Background music decode stubs
- **cellBGDL**: Background download manager — empty download list
- **cellVideoUpload**: Video upload — returns NOT_SUPPORTED
- **cellLicenseArea**: License verification — Americas default, all areas valid
- **cellOvis**: Overlay system — no-op stubs
- **cellScreenshot**: Upgraded from Partial to Complete
- **75 complete modules** (up from 62), only ~19 "Not Started" remain

### v0.2.4 — *"Peripheral Vision"* (March 2026)
- **13 new modules** in one batch — biggest single release yet
- **sceNpUtil**: Bandwidth test (fake 100 Mbps), NP environment, online ID validation, parental control
- **sceNpCommerce**: Commerce context management, store operations return NOT_CONNECTED
- **sceNpMatching2**: Matchmaking contexts with start/stop, signaling/room callbacks (offline stub)
- **sceNpSignaling**: P2P signaling contexts, connection ops return NOT_CONNECTED
- **sceNpSns**: Facebook/Twitter social integration stubs
- **cellVpost**: Video post-processing handle management, query/exec stubs
- **cellJpgEnc**: JPEG encoder handles (encode needs stb_image_write)
- **cellPngEnc**: PNG encoder handles (encode needs stb_image_write)
- **sys_interrupt**: Interrupt tag/thread tracking (no real interrupts in HLE)
- **cellSheap**: Shared heap bump allocator with block tracking, alloc/free/query
- **cellUsbd**: USB device driver — LDD registration, empty device list
- **cellCamera**: PlayStation Eye — reports no camera attached
- **cellGem**: PlayStation Move — reports no controllers connected
- **62 complete modules** (up from 49), new `libs/hardware/` directory

### v0.2.3 — *"Half a Hundred"* (March 2026)
- **sysPrxForUser**: Upgraded from stub to real — lwmutex backed by CRITICAL_SECTION/pthread_mutex, lwcond backed by CONDITION_VARIABLE/pthread_cond, real host threads, heap management, snprintf, string/mem ops
- **cellSysutil**: Upgraded — BGM playback control, system cache mount/clear, disc game check, license area
- **cellSysmodule**: Upgraded to complete — all module names mapped
- **cellPamf**: PAMF container parser — big-endian header parsing, stream queries, AVC/ATRAC3+/LPCM/AC3 codec info, entry point navigation
- **cellDmux**: AV demuxer — open/close, ES management, stream feed/reset, AU retrieval, flush callbacks
- **cellVdec**: Video decoder API — H.264/MPEG2 codec types, AU submit with AUDONE callback (actual decode needs FFmpeg)
- **cellAdec**: Audio decoder API — AAC/ATRAC3+/MP3 codec types, AU submit with AUDONE callback (actual decode needs FFmpeg)
- **cellNet**: Network core — Winsock/POSIX initialization, DNS resolver with real getaddrinfo
- **cellSsl**: SSL/TLS lifecycle — init/end, certificate stubs, cryptographic RNG (BCryptGenRandom/urandom)
- **sceNpTus**: NP Title User Storage — local variable/data storage with set/get/add/delete per slot
- **49 complete modules** (up from 42), 3 partial modules upgraded to complete

### v0.2.2 — *"Fiber Optics"* (March 2026)
- **cellL10n**: Full Unicode conversion — UTF-8 ↔ UTF-16 ↔ UTF-32 ↔ UCS-2, ISO-8859-1, ASCII, generic `l10n_convert()` API
- **cellFiber**: Cooperative multitasking with native OS fibers (Windows `CreateFiber`/POSIX `ucontext`), 64 concurrent fibers, sleep/wakeup
- **cellResc**: Resolution scaling/conversion — display mode config, buffer management, interlace tables, aspect ratio, flip/vblank handlers
- **cellHttpUtil**: URL parsing/building, percent-encoding/decoding, form URL encoding, Base64 codec
- **sceNpBasic**: Friends list, presence management, messaging, game invitations, block list (offline stub)
- **cellKey2char**: HID keyboard scancode → Unicode character conversion, US-101 layout, shift/caps handling
- **cellAvconfExt**: Audio output device info, sound availability queries, LPCM/AC3/DTS config, video gamma
- **cellGameExec**: Boot parameters, exit to shelf, boot game info
- **42 complete modules** (up from 34)

### v0.2.1 — *"Now With Sockets"* (March 2026)
- **sys_net**: Full BSD socket API — socket, bind, listen, accept, connect, send, recv, sendto, recvfrom, poll, select, setsockopt/getsockopt, getsockname, shutdown, close, gethostbyname, inet_aton, errno
- Wraps Winsock2 (Windows) and POSIX sockets (Linux/macOS) with PS3 error code translation
- PS3-specific SO_NBIO non-blocking mode support
- 128-socket descriptor table with host FD mapping
- **34 complete modules** (up from 33)
- First game port target: **flOw** (NPUA80001) — see [sp00nznet/flow](https://github.com/sp00nznet/flow)

### v0.2.0 — *"Now We're Cooking with Cell"* (March 2026)
- **33 complete module implementations** — up from 7 stubs
- **Real threading**: sys_ppu_thread creates actual host threads (CreateThread/pthreads)
- **Full synchronization suite**: Mutexes with deadlock detection, condvars, semaphores, rwlocks, event queues/flags
- **Functional filesystem**: cellFs and sys_fs with configurable PS3-to-host path mapping, real file I/O, directory enumeration
- **Save data system**: cellSaveData with full callback-driven save/load flow, directory management
- **Game utilities**: cellGame boot check, content paths, PARAM.SFO reading
- **Real gamepad input**: cellPad with XInput (Windows) and SDL2 (cross-platform) backends, analog sticks, triggers, rumble
- **Keyboard & mouse**: cellKb with raw/ASCII modes, cellMouse with delta accumulation and ring buffer
- **Real audio output**: cellAudio with WASAPI/SDL2 backends, background mixing thread @ 5.33ms, multi-port mixing, 7.1 downmix
- **Image decoders**: cellPngDec, cellJpgDec, cellGifDec with stb_image backend
- **Font rendering**: cellFont with stb_truetype backend and fallback metrics
- **Network**: cellNetCtl with real host IP detection, sceNp with configurable PSN identity
- **Trophy system**: sceNpTrophy with persistent JSON storage, unlock timestamps, progress tracking
- **SPURS framework**: cellSpurs management APIs (workloads, tasks, tasksets, event flags)
- **Sync primitives**: cellSync (atomic spinlocks, barriers, lock-free queues), cellSync2 (OS-backed mutex/cond/sem)
- **System utilities**: cellRtc (real clock with PS3 epoch), cellVideoOut, cellMsgDialog, cellOskDialog, cellUserInfo, cellScreenshot
- **Memory management**: sys_memory with bump allocator, containers, shared memory, mmapper
- **Timers**: sys_timer with QueryPerformanceCounter/clock_gettime, periodic events, PS3 timebase frequency

### v0.1.0 — *"Hello, Cell"* (March 2026)
- Initial project structure and architecture
- PS3 ELF/SELF/PRX binary parser
- PPU disassembler with full PowerPC 64-bit + VMX support
- PPU → C code lifter (instruction-level translation)
- SPU disassembler and lifter (basic support)
- NID database with 2000+ function name mappings
- Core runtime: PPU context, memory model, big-endian support
- LV2 syscall stubs: threading, memory, filesystem, PRX loading
- HLE library stubs for 15+ core modules
- Template project for bootstrapping new game ports

---

*"The Cell Processor was ahead of its time. Now it's time to bring it to ours."*
