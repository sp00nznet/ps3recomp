# PS3 HLE Module Status

Status of HLE (High-Level Emulation) stubs for PS3 system modules in ps3recomp.

**Legend:**
- **Not Started**: No implementation exists
- **Stubbed**: Functions exist but only log and return CELL_OK
- **Partial**: Core functions implemented, advanced features missing
- **Complete**: Fully functional for known game use cases

## System / Core

| Module | Category | Status | Notes |
|---|---|---|---|
| sysPrxForUser | Core Runtime | Stubbed | Thread create/join, lwmutex, lwcond, printf |
| cellSysmodule | Module Loader | Stubbed | Load/unload tracking, ID constants |
| cellSysutil | System Utility | Stubbed | Callbacks, system params (lang, region) |
| sys_fs | Kernel Filesystem | Not Started | Lower-level than cellFs |
| sys_memory | Kernel Memory | Not Started | Memory container alloc/free |
| sys_event | Kernel Events | Not Started | Event queues, event flags |
| sys_mutex | Kernel Mutex | Not Started | Kernel-level mutex (heavier than lwmutex) |
| sys_cond | Kernel Condvar | Not Started | Kernel-level condition variable |
| sys_semaphore | Kernel Semaphore | Not Started | Counting semaphore |
| sys_rwlock | Kernel RWLock | Not Started | Reader-writer lock |
| sys_timer | Kernel Timer | Not Started | High-resolution timers |
| sys_interrupt | Kernel Interrupt | Not Started | Interrupt thread management |

## Filesystem

| Module | Category | Status | Notes |
|---|---|---|---|
| cellFs | Filesystem | Stubbed | Open/close/read/write/stat/dir ops |
| cellFsUtility | FS Utility | Not Started | High-level file utilities |

## Input

| Module | Category | Status | Notes |
|---|---|---|---|
| cellPad | Gamepad | Stubbed | Init, GetData, GetInfo2, button constants |
| cellKb | Keyboard | Not Started | USB keyboard input |
| cellMouse | Mouse | Not Started | USB mouse input |

## Graphics

| Module | Category | Status | Notes |
|---|---|---|---|
| cellGcmSys | RSX System | Stubbed | Init, flip control, display buffers, addr translation |
| cellResc | Resolution | Not Started | Resolution scaling/conversion |
| cellVideoOut | Video Output | Not Started | Video output configuration |

## Audio

| Module | Category | Status | Notes |
|---|---|---|---|
| cellAudio | Audio Output | Stubbed | Init, port open/close/start/stop |
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
| cellJpgDec | JPEG Decode | Not Started | JPEG image decoding |
| cellPngDec | PNG Decode | Not Started | PNG image decoding |
| cellJpgEnc | JPEG Encode | Not Started | JPEG encoding |
| cellPngEnc | PNG Encode | Not Started | PNG encoding |
| cellSail | Media Player | Not Started | High-level media playback |

## Font / Text

| Module | Category | Status | Notes |
|---|---|---|---|
| cellFont | Font Rendering | Not Started | System font management |
| cellFontFT | FreeType Font | Not Started | FreeType-based rendering |
| cellFreeType | FreeType | Not Started | FreeType library wrapper |
| cellL10n | Localization | Not Started | Character encoding conversion |

## Network

| Module | Category | Status | Notes |
|---|---|---|---|
| cellNet | Network Core | Not Started | Socket-level networking |
| cellNetCtl | Network Control | Not Started | Connection management |
| cellHttp | HTTP Client | Not Started | HTTP/HTTPS requests |
| cellHttpUtil | HTTP Utility | Not Started | URL parsing, cookie management |
| cellSsl | SSL/TLS | Not Started | Secure socket layer |
| cellRudp | Reliable UDP | Not Started | Sony's reliable UDP protocol |

## PSN / NP

| Module | Category | Status | Notes |
|---|---|---|---|
| sceNp | NP Core | Not Started | PSN account management |
| sceNpBasic | NP Basic | Not Started | Friends, messaging |
| sceNpCommerce | NP Commerce | Not Started | In-game store |
| sceNpClans | NP Clans | Not Started | Clan system |
| sceNpTus | NP TUS | Not Started | Title user storage |
| sceNpMatching2 | NP Matching | Not Started | Online matchmaking |
| sceNpSignaling | NP Signaling | Not Started | P2P connection signaling |
| sceNpSns | NP SNS | Not Started | Social networking integration |
| sceNpTrophy | NP Trophies | Not Started | Trophy/achievement system |
| sceNpUtil | NP Utility | Not Started | Utility functions |

## System Utilities (cellSysutil sub-modules)

| Module | Category | Status | Notes |
|---|---|---|---|
| cellSaveData | Save Data | Not Started | Game save management |
| cellGame | Game Utility | Not Started | Game data, boot info, content info |
| cellMsgDialog | Message Dialog | Not Started | System message popups |
| cellOskDialog | OSK Dialog | Not Started | On-screen keyboard |
| cellVideoUpload | Video Upload | Not Started | |
| cellScreenshot | Screenshot | Not Started | Screenshot capture |
| cellBGDL | Background DL | Not Started | Background download manager |
| cellUserInfo | User Info | Not Started | User account information |

## SPU / Multi-core

| Module | Category | Status | Notes |
|---|---|---|---|
| cellSpurs | SPURS | Not Started | SPU Runtime System -- the big one |
| cellSpursJq | SPURS Job Queue | Not Started | SPURS job queue extension |
| cellFiber | Fiber | Not Started | Cooperative multitasking fibers |
| cellSync | Sync Primitives | Not Started | SPU-safe sync objects |
| cellSync2 | Sync Primitives 2 | Not Started | Extended sync primitives |

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
| cellRtc | Real-Time Clock | Not Started | Date/time functions |
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
| Complete | 0 |
| Partial | 0 |
| Stubbed | 7 |
| Not Started | ~90 |
| **Total** | **~97** |

Priority for implementation (based on frequency of use in games):
1. cellGcmSys (full RSX command processing)
2. cellSpurs (SPU task system)
3. cellFs (real file I/O with path mapping)
4. cellSaveData (save/load)
5. cellPngDec / cellJpgDec (texture loading)
6. cellFont (text rendering)
7. cellPamf / cellVdec / cellAdec (cutscene playback)
