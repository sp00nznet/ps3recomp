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
│   ├── network/              # cellNetCtl, cellHttp, cellSsl, sceNp*
│   ├── filesystem/           # cellFs, cellGame, cellSaveData, cellStorage
│   ├── system/               # cellSysutil, cellSysmodule, cellRtc, sysPrxForUser
│   ├── spurs/                # cellSpurs, cellFiber (SPU task management)
│   ├── sync/                 # cellSync, cellSync2 (cross-PPU/SPU synchronization)
│   ├── codec/                # cellJpgDec, cellPngDec, cellGifDec, cellPamf, cellDmux
│   ├── font/                 # cellFont, cellFontFT, cell_FreeType2, cellL10n
│   └── misc/                 # Everything else: cellScreenshot, cellWebBrowser, etc.
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
└── docs/                     # Documentation
    ├── ARCHITECTURE.md        # Technical deep-dive
    ├── GETTING_STARTED.md     # How to recompile your first PS3 game
    └── MODULE_STATUS.md       # Implementation status of all HLE modules
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

We're building HLE implementations based on RPCS3's module system. **77 modules complete, 2 partial, 200+ files, 55,000+ lines of code.**

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
| **Graphics** | cellGcmSys (cmd buffer, IO mapping, tile/zcull, flip handlers, timestamps) | ✅ Complete |
| **SPURS** | cellSpurs (management APIs, no SPU execution yet) | 🔨 Partial |
| **Core Runtime** | cellSysutil (BGM, cache, disc), cellSysmodule, sysPrxForUser (real lwmutex/lwcond/threads) | ✅ Complete |
| **Media Pipeline** | cellPamf, cellDmux, cellVdec, cellAdec (API stubs, actual decode needs FFmpeg), cellSail | 🔨 Partial |
| **HTTP** | cellHttp (real HTTP/1.1 via native sockets, DNS, headers, timeouts) | ✅ Complete |

Full status tracking: [docs/MODULE_STATUS.md](docs/MODULE_STATUS.md)

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
| **flOw** (thatgamecompany) | NPUA80001 | Binary analysis complete, 139/140 imports resolved | [sp00nznet/flow](https://github.com/sp00nznet/flow) |

Want to port a game? Start with the [Getting Started](#getting-started) section and check [docs/MODULE_STATUS.md](docs/MODULE_STATUS.md) to see which system libraries are already implemented.

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

### v0.3.0 — *"Full Metal RSX"* (March 2026)
- **cellGcmSys**: Major upgrade from Partial to Complete — command buffer control (put/get/ref), local memory bump allocator, IO memory mapping with proper offset table population (1MB page granularity), flip handler + VBlank callbacks, 15 tile slots + 8 zcull regions, 256 report data + label slots, platform-native timestamps, GetTiledPitchSize, 27+ functions total
- **cellHttp**: Major upgrade from Partial to Complete — real HTTP/1.1 via native sockets (Winsock2/POSIX), DNS resolution via getaddrinfo, TCP connect, request formatting with custom headers, response header parsing (status code, Content-Length, Connection: close), streaming body receive, per-transaction socket lifecycle, SO_RCVTIMEO/SO_SNDTIMEO timeouts
- **cellSpurs**: Event flags now use real OS blocking — CRITICAL_SECTION + CONDITION_VARIABLE (Windows) / pthread_mutex + pthread_cond (POSIX) via side table, EventFlagWait truly blocks until condition met, broadcast on EventFlagSet
- **77 complete modules** (up from 75), 2 remaining partial (cellVdec/cellAdec need FFmpeg, cellDmux/cellSpurs management-only)

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
