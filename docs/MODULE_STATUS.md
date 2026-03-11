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
| sysPrxForUser | Core Runtime | Stubbed | Thread create/join, lwmutex, lwcond, printf |
| cellSysmodule | Module Loader | Partial | Load/unload tracking, 55+ module ID constants |
| cellSysutil | System Utility | Partial | Callbacks, system params (lang, region, timezone) |
| sys_ppu_thread | Kernel Threading | **Complete** | Real host threads (CreateThread/pthreads), join, detach, priority, rename |
| sys_fs | Kernel Filesystem | **Complete** | Full file I/O with PS3-to-host path mapping, directory ops, stat, big-endian output |
| sys_memory | Kernel Memory | **Complete** | Bump allocator from VM, containers, shared memory, mmapper |
| sys_event | Kernel Events | **Complete** | Event queues (circular buffer), ports (connect/send), 64-bit event flags (AND/OR wait) |
| sys_mutex | Kernel Mutex | **Complete** | CRITICAL_SECTION/pthread_mutex, recursive, timed lock, deadlock detection |
| sys_cond | Kernel Condvar | **Complete** | CONDITION_VARIABLE/pthread_cond, timed wait, signal, broadcast |
| sys_semaphore | Kernel Semaphore | **Complete** | Win32 Semaphore/POSIX sem_t, timeout, multi-count post |
| sys_rwlock | Kernel RWLock | **Complete** | SRWLock/pthread_rwlock, full read/write/try variants |
| sys_timer | Kernel Timer | **Complete** | QueryPerformanceCounter/clock_gettime, periodic event timers, usleep, timebase freq |
| sys_interrupt | Kernel Interrupt | Not Started | Interrupt thread management |

## Filesystem

| Module | Category | Status | Notes |
|---|---|---|---|
| cellFs | Filesystem | **Complete** | Real file I/O, configurable path mapping, dir enumeration, stat, truncate, chmod |
| cellFsUtility | FS Utility | Not Started | High-level file utilities |

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
| cellResc | Resolution | Not Started | Resolution scaling/conversion |
| cellVideoOut | Video Output | **Complete** | Resolution config, device info, all PS3 resolution IDs, default 720p |

## Audio

| Module | Category | Status | Notes |
|---|---|---|---|
| cellAudio | Audio Output | **Complete** | WASAPI (Win) / SDL2 backends, mixing thread @ 5.33ms, multi-port mixing, 7.1 downmix |
| cellVoice | Voice Chat | Not Started | |
| cellMic | Microphone | Not Started | |

## Media / Codec

| Module | Category | Status | Notes |
|---|---|---|---|
| cellPamf | PAMF Container | Not Started | PlayStation media file parsing |
| cellVdec | Video Decode | Not Started | H.264/MPEG2 video decoding |
| cellAdec | Audio Decode | Not Started | AAC/ATRAC3+ audio decoding |
| cellDmux | Demuxer | Not Started | AV stream demultiplexing |
| cellVpost | Video Post | Not Started | Video post-processing |
| cellJpgDec | JPEG Decode | **Complete** | Header parsing + stb_image decode, file/buffer sources |
| cellPngDec | PNG Decode | **Complete** | Header parsing + stb_image decode, RGBA/ARGB/RGB output |
| cellGifDec | GIF Decode | **Complete** | Header parsing + stb_image decode |
| cellJpgEnc | JPEG Encode | Not Started | JPEG encoding |
| cellPngEnc | PNG Encode | Not Started | PNG encoding |
| cellSail | Media Player | Not Started | High-level media playback |

## Font / Text

| Module | Category | Status | Notes |
|---|---|---|---|
| cellFont | Font Rendering | **Complete** | Full lifecycle, stb_truetype backend, fallback metrics without STB |
| cellFontFT | FreeType Font | Not Started | FreeType-based rendering |
| cellFreeType | FreeType | Not Started | FreeType library wrapper |
| cellL10n | Localization | Not Started | Character encoding conversion |

## Network

| Module | Category | Status | Notes |
|---|---|---|---|
| sys_net | BSD Sockets | **Complete** | Full BSD socket API — socket, bind, listen, accept, connect, send/recv/sendto/recvfrom, poll, select, setsockopt/getsockopt, shutdown, close, gethostbyname, inet_aton, errno |
| cellNet | Network Core | Not Started | Higher-level network utilities |
| cellNetCtl | Network Control | **Complete** | Real host IP detection, NAT type, connection state, handler callbacks |
| cellHttp | HTTP Client | Partial | Client/transaction management, config APIs work, actual HTTP returns CONNECTION_FAILED |
| cellHttpUtil | HTTP Utility | Not Started | URL parsing, cookie management |
| cellSsl | SSL/TLS | Not Started | Secure socket layer |
| cellRudp | Reliable UDP | Not Started | Sony's reliable UDP protocol |

## PSN / NP

| Module | Category | Status | Notes |
|---|---|---|---|
| sceNp | NP Core | **Complete** | Configurable username, fake NP IDs, account region/age |
| sceNpBasic | NP Basic | Not Started | Friends, messaging |
| sceNpCommerce | NP Commerce | Not Started | In-game store |
| sceNpClans | NP Clans | Not Started | Clan system |
| sceNpTus | NP TUS | Not Started | Title user storage |
| sceNpMatching2 | NP Matching | Not Started | Online matchmaking |
| sceNpSignaling | NP Signaling | Not Started | P2P connection signaling |
| sceNpSns | NP SNS | Not Started | Social networking integration |
| sceNpTrophy | NP Trophies | **Complete** | Persistent JSON storage, unlock with timestamps, progress tracking |
| sceNpUtil | NP Utility | Not Started | Utility functions |

## System Utilities (cellSysutil sub-modules)

| Module | Category | Status | Notes |
|---|---|---|---|
| cellSaveData | Save Data | **Complete** | Callback-driven save/load, directory enumeration, file ops, simplified PARAM.SFO |
| cellGame | Game Utility | **Complete** | Boot check, content permit, param read, data directory management |
| cellMsgDialog | Message Dialog | **Complete** | Prints to stdout, auto-responds YES/OK, progress bar tracking |
| cellOskDialog | OSK Dialog | **Complete** | UTF-16 support, configurable default response, async pattern |
| cellVideoUpload | Video Upload | Not Started | |
| cellScreenshot | Screenshot | Partial | Enable/disable, parameter/overlay tracking (no actual capture) |
| cellBGDL | Background DL | Not Started | Background download manager |
| cellUserInfo | User Info | **Complete** | Default user (00000001/"User"), GetStat/GetList/SelectUser |

## SPU / Multi-core

| Module | Category | Status | Notes |
|---|---|---|---|
| cellSpurs | SPURS | Partial | Management APIs, workloads, tasks, tasksets, event flags (no actual SPU execution) |
| cellSpursJq | SPURS Job Queue | Not Started | SPURS job queue extension |
| cellFiber | Fiber | Not Started | Cooperative multitasking fibers |
| cellSync | Sync Primitives | **Complete** | Atomic spinlock mutex, counter barrier, RWM, bounded queue, lock-free queue |
| cellSync2 | Sync Primitives 2 | **Complete** | OS-backed mutex/cond/semaphore/queue with timeouts |

## Hardware / Peripheral

| Module | Category | Status | Notes |
|---|---|---|---|
| cellUsbd | USB Driver | Not Started | USB device access |
| cellCamera | Camera | Not Started | PlayStation Eye camera |
| cellGem | Move Controller | Not Started | PlayStation Move |
| cellAvconfExt | AV Config | Not Started | Audio/video output config |

## Miscellaneous

| Module | Category | Status | Notes |
|---|---|---|---|
| cellRtc | Real-Time Clock | **Complete** | Host time -> PS3 ticks, DateTime, time arithmetic, RFC formatting, day-of-week |
| cellOvis | Overlay | Not Started | |
| cellSheap | Shared Heap | Not Started | Shared memory heap |
| cellKey2char | Key to Char | Not Started | Keycode conversion |
| cellSubdisplay | Sub-display | Not Started | Vita remote play |
| cellImeJp | IME Japanese | Not Started | Japanese input method |
| cellDaisy | Daisy Chain | Not Started | SPU pipeline framework |
| cellGameExec | Game Execute | Not Started | Boot other games |
| cellLicenseArea | License Area | Not Started | License verification |
| cellMusicDecode | Music Decode | Not Started | Background music decoding |
| cellMusicDecode2 | Music Decode 2 | Not Started | Extended music decoding |

## Summary

| Status | Count |
|---|---|
| **Complete** | 34 |
| Partial | 5 |
| Stubbed | 1 |
| Not Started | ~58 |
| **Total** | **~98** |

## Next Priorities

1. **cellGcmSys** — Full RSX command buffer processing (the graphics mountain)
2. **cellSpurs** — Actual SPU program execution on host threads
3. **cellPamf / cellVdec / cellAdec / cellDmux** — Video cutscene playback pipeline
4. **cellNet** — Socket-level networking for multiplayer
5. **cellFiber** — Cooperative multitasking
6. **cellL10n** — Character encoding conversion
