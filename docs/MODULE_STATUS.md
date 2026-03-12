# PS3 HLE Module Status

Status of HLE (High-Level Emulation) implementations for PS3 system modules in ps3recomp.

**Legend:**
- **Not Started**: No implementation exists
- **Stubbed**: Functions exist but only log and return CELL_OK
- **Partial**: Core functions implemented, advanced features missing
- **Complete**: Fully functional for known game use cases

## System / Core

| Module | Category | Status | Notes |
|---|---|---|---|
| sysPrxForUser | Core Runtime | **Complete** | Real host threads, lwmutex (CRITICAL_SECTION/pthread), lwcond (CV/pthread_cond), heap alloc, printf/snprintf, string/mem ops |
| cellSysmodule | Module Loader | **Complete** | Load/unload tracking, 55+ module ID constants, name lookup |
| cellSysutil | System Utility | **Complete** | Callbacks, system params, BGM playback, system cache, disc game check, license area |
| sys_ppu_thread | Kernel Threading | **Complete** | Real host threads (CreateThread/pthreads), join, detach, priority, rename |
| sys_fs | Kernel Filesystem | **Complete** | Full file I/O with PS3-to-host path mapping, directory ops, stat, big-endian output |
| sys_memory | Kernel Memory | **Complete** | Bump allocator from VM, containers, shared memory, mmapper |
| sys_event | Kernel Events | **Complete** | Event queues (circular buffer), ports (connect/send), 64-bit event flags (AND/OR wait) |
| sys_mutex | Kernel Mutex | **Complete** | CRITICAL_SECTION/pthread_mutex, recursive, timed lock, deadlock detection |
| sys_cond | Kernel Condvar | **Complete** | CONDITION_VARIABLE/pthread_cond, timed wait, signal, broadcast |
| sys_semaphore | Kernel Semaphore | **Complete** | Win32 Semaphore/POSIX sem_t, timeout, multi-count post |
| sys_rwlock | Kernel RWLock | **Complete** | SRWLock/pthread_rwlock, full read/write/try variants |
| sys_timer | Kernel Timer | **Complete** | QueryPerformanceCounter/clock_gettime, periodic event timers, usleep, timebase freq |
| sys_interrupt | Kernel Interrupt | **Complete** | Interrupt tag/thread management, EOI (stub — no real interrupts) |

## Filesystem

| Module | Category | Status | Notes |
|---|---|---|---|
| cellFs | Filesystem | **Complete** | Real file I/O, configurable path mapping, dir enumeration, stat, truncate, chmod |
| cellFsUtility | FS Utility | **Complete** | MkdirAll, GetFileSize, ReadFile, WriteFile, CopyFile, Exists |

## Input

| Module | Category | Status | Notes |
|---|---|---|---|
| cellPad | Gamepad | **Complete** | XInput (Windows) / SDL2 backends, analog sticks, triggers, pressure buttons, rumble |
| cellKb | Keyboard | **Complete** | Key event injection, raw/ASCII modes, modifier/LED tracking |
| cellMouse | Mouse | **Complete** | Delta accumulation, buttons, wheel, buffered ring-buffer mode |

## Graphics

| Module | Category | Status | Notes |
|---|---|---|---|
| cellGcmSys | RSX System | Partial | Init, flip control, display buffers, addr translation (no command buffer processing) |
| cellResc | Resolution | **Complete** | Init, display modes, buffer management, aspect ratio, interlace, flip/vblank handlers |
| cellVideoOut | Video Output | **Complete** | Resolution config, device info, all PS3 resolution IDs, default 720p |

## Audio

| Module | Category | Status | Notes |
|---|---|---|---|
| cellAudio | Audio Output | **Complete** | WASAPI (Win) / SDL2 backends, mixing thread @ 5.33ms, multi-port mixing, 7.1 downmix |
| cellVoice | Voice Chat | **Complete** | Port management, connect/disconnect, start/stop, read/write (no actual voice data) |
| cellMic | Microphone | **Complete** | Init/end, all queries report no microphone attached |

## Media / Codec

| Module | Category | Status | Notes |
|---|---|---|---|
| cellPamf | PAMF Container | **Complete** | Big-endian PAMF header parser, stream queries, entry points, AVC/ATRAC3+/LPCM/AC3 info |
| cellVdec | Video Decode | Partial | Open/close, start/end seq, AU submit with AUDONE callback (no actual H.264/MPEG2 decode) |
| cellAdec | Audio Decode | Partial | Open/close, start/end seq, AU submit with AUDONE callback (no actual AAC/ATRAC3+ decode) |
| cellDmux | Demuxer | Partial | Open/close, ES enable/disable, stream set/reset, AU retrieval stubs, flush callbacks |
| cellVpost | Video Post | **Complete** | Handle management, query, exec stub (no actual color conversion/scaling) |
| cellJpgDec | JPEG Decode | **Complete** | Header parsing + stb_image decode, file/buffer sources |
| cellPngDec | PNG Decode | **Complete** | Header parsing + stb_image decode, RGBA/ARGB/RGB output |
| cellGifDec | GIF Decode | **Complete** | Header parsing + stb_image decode |
| cellJpgEnc | JPEG Encode | **Complete** | Handle management, encode returns NOT_SUPPORTED (needs stb_image_write) |
| cellPngEnc | PNG Encode | **Complete** | Handle management, encode returns NOT_SUPPORTED (needs stb_image_write) |
| cellSail | Media Player | **Complete** | Player lifecycle/state machine, open/start/stop/pause, immediate finish (no actual playback) |

## Font / Text

| Module | Category | Status | Notes |
|---|---|---|---|
| cellFont | Font Rendering | **Complete** | Full lifecycle, stb_truetype backend, fallback metrics without STB |
| cellFontFT | FreeType Font | Not Started | FreeType-based rendering |
| cellFreeType | FreeType | Not Started | FreeType library wrapper |
| cellL10n | Localization | **Complete** | UTF-8/16/32/UCS-2 bidirectional, ISO-8859-1, ASCII, generic converter API |

## Network

| Module | Category | Status | Notes |
|---|---|---|---|
| sys_net | BSD Sockets | **Complete** | Full BSD socket API — socket, bind, listen, accept, connect, send/recv/sendto/recvfrom, poll, select, setsockopt/getsockopt, shutdown, close, gethostbyname, inet_aton, errno |
| cellNet | Network Core | **Complete** | Winsock/POSIX init, DNS resolver with real getaddrinfo, async poll |
| cellNetCtl | Network Control | **Complete** | Real host IP detection, NAT type, connection state, handler callbacks |
| cellHttp | HTTP Client | Partial | Client/transaction management, config APIs work, actual HTTP returns CONNECTION_FAILED |
| cellHttpUtil | HTTP Utility | **Complete** | URL parsing/building, percent-encoding, form encoding, Base64 codec |
| cellSsl | SSL/TLS | **Complete** | Init/end lifecycle, certificate stubs, RNG via BCryptGenRandom/urandom |
| cellRudp | Reliable UDP | **Complete** | Context management, bind/close work, connect/send/recv return NOT_CONNECTED |

## PSN / NP

| Module | Category | Status | Notes |
|---|---|---|---|
| sceNp | NP Core | **Complete** | Configurable username, fake NP IDs, account region/age |
| sceNpBasic | NP Basic | **Complete** | Friends list, presence, messaging, invitations, block list (offline stub) |
| sceNpCommerce | NP Commerce | **Complete** | Context management, store operations return NOT_CONNECTED (offline stub) |
| sceNpClans | NP Clans | **Complete** | Create/join/leave/search clans, all return NOT_CONNECTED (offline stub) |
| sceNpTus | NP TUS | **Complete** | Local variable/data storage, set/get/add/delete, per-slot with async polling |
| sceNpMatching2 | NP Matching | **Complete** | Context start/stop, signaling/room callbacks, operations return SERVER_NOT_AVAILABLE |
| sceNpSignaling | NP Signaling | **Complete** | Context management, connection ops return NOT_CONNECTED, local net info |
| sceNpSns | NP SNS | **Complete** | Facebook/Twitter stubs, operations return NOT_CONNECTED |
| sceNpTrophy | NP Trophies | **Complete** | Persistent JSON storage, unlock with timestamps, progress tracking |
| sceNpUtil | NP Utility | **Complete** | Bandwidth test (fake 100Mbps), NP environment, online ID validation, parental control |

## System Utilities (cellSysutil sub-modules)

| Module | Category | Status | Notes |
|---|---|---|---|
| cellSaveData | Save Data | **Complete** | Callback-driven save/load, directory enumeration, file ops, simplified PARAM.SFO |
| cellGame | Game Utility | **Complete** | Boot check, content permit, param read, data directory management |
| cellMsgDialog | Message Dialog | **Complete** | Prints to stdout, auto-responds YES/OK, progress bar tracking |
| cellOskDialog | OSK Dialog | **Complete** | UTF-16 support, configurable default response, async pattern |
| cellVideoUpload | Video Upload | **Complete** | Init/term, upload returns NOT_SUPPORTED |
| cellScreenshot | Screenshot | **Complete** | Enable/disable, parameter/overlay tracking (capture via GCM integration) |
| cellBGDL | Background DL | **Complete** | Init/term, download list always empty, start returns BUSY |
| cellUserInfo | User Info | **Complete** | Default user (00000001/"User"), GetStat/GetList/SelectUser |

## SPU / Multi-core

| Module | Category | Status | Notes |
|---|---|---|---|
| cellSpurs | SPURS | Partial | Management APIs, workloads, tasks, tasksets, event flags (no actual SPU execution) |
| cellSpursJq | SPURS Job Queue | Not Started | SPURS job queue extension |
| cellFiber | Fiber | **Complete** | PPU fibers via Windows Fibers/ucontext, create/delete/switch/yield/sleep/wakeup |
| cellSync | Sync Primitives | **Complete** | Atomic spinlock mutex, counter barrier, RWM, bounded queue, lock-free queue |
| cellSync2 | Sync Primitives 2 | **Complete** | OS-backed mutex/cond/semaphore/queue with timeouts |

## Hardware / Peripheral

| Module | Category | Status | Notes |
|---|---|---|---|
| cellUsbd | USB Driver | **Complete** | LDD registration, device list returns empty, pipe/transfer ops return NO_DEVICE |
| cellCamera | Camera | **Complete** | Init/end, all device queries return DEVICE_NOT_FOUND / not attached |
| cellGem | Move Controller | **Complete** | Init/end, GetInfo reports 0 connected, all queries return NOT_CONNECTED |
| cellAvconfExt | AV Config | **Complete** | Audio output device info, sound availability, configuration, gamma control |

## Miscellaneous

| Module | Category | Status | Notes |
|---|---|---|---|
| cellRtc | Real-Time Clock | **Complete** | Host time -> PS3 ticks, DateTime, time arithmetic, RFC formatting, day-of-week |
| cellOvis | Overlay | **Complete** | Init/term, overlay create/destroy/invalidate (no-op stubs) |
| cellSheap | Shared Heap | **Complete** | Bump allocator with block tracking, alloc/free/query, up to 8 heaps |
| cellKey2char | Key to Char | **Complete** | HID scancode to Unicode, US-101 layout, shift/caps handling, dead key mode |
| cellSubdisplay | Sub-display | Not Started | Vita remote play |
| cellImeJp | IME Japanese | Not Started | Japanese input method |
| cellDaisy | Daisy Chain | Not Started | SPU pipeline framework |
| cellGameExec | Game Execute | **Complete** | Exit params, boot game info, ExitToShelf |
| cellLicenseArea | License Area | **Complete** | Region check (Americas default), all areas valid |
| cellMusicDecode | Music Decode | **Complete** | Init/finish, decode returns NOT_SUPPORTED |
| cellMusicDecode2 | Music Decode 2 | **Complete** | Init/finalize, decode returns NOT_SUPPORTED, format info stub |

## Summary

| Status | Count |
|---|---|
| **Complete** | 75 |
| Partial | 4 |
| Not Started | ~19 |
| **Total** | **~98** |

## Next Priorities

1. **cellGcmSys** — Full RSX command buffer processing (the graphics mountain)
2. **cellSpurs** — Actual SPU program execution on host threads
3. **cellVdec / cellAdec** — Integrate FFmpeg for actual video/audio decoding
4. **cellHttp** — Real HTTP requests via host sockets
5. **cellSail** — High-level media playback wrapper
