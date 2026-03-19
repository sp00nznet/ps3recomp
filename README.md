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
│   │   ├── spu_context.h     # 128x128-bit register file, channels
│   │   ├── spu_local_store.h # 256KB local memory simulation
│   │   └── spu_dma.h         # DMA transfer engine
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
└── docs/                     # Documentation (15 documents, 65K+ words)
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

1. **SPU programs are self-contained** — they live in ELF segments loaded to local store
2. **Recompile each SPU program separately** — dedicated lifter handles the SPU ISA
3. **SPU local store becomes a thread-local array** — 256KB per "virtual SPU"
4. **DMA operations become memcpy** — with proper synchronization
5. **Channels become cross-thread message queues** — preserving ordering guarantees

This mirrors how RPCS3 handles SPU but at compile time rather than runtime.

## Module Status

We're building HLE implementations based on RPCS3's module system. **97 modules complete, 1 partial (media decode), 250+ files, 70,000+ lines of code.**

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
| **Graphics** | cellGcmSys (cmd buffer, IO mapping, tile/zcull, flip handlers, timestamps), RSX command processor (NV47xx parsing, state tracking, backend callbacks), null backend (Win32 window) | ✅ Complete |
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
| **flOw** (thatgamecompany) | NPUA80001 | 92K functions, CRT + static constructors + engine init running, ~20K TODOs, D3D12 window | [sp00nznet/flow](https://github.com/sp00nznet/flow) |
| **Tokyo Jungle** (Crispy's/SCE Japan) | NPUA80523 | 24K functions lifted, 18 MB exe, all 24 modules covered by ps3recomp | (local) |

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

## License

MIT License. See [LICENSE](LICENSE) for details.

---

## Changelog

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
