# Opt-in flags

Every switch that changes how ps3recomp builds or runs: the environment variables the runtime,
libraries, project template and tools read, and the CMake options and cache settings. With
none of them set, a build behaves as its defaults below say.

`tools/check_opt_in_flags.py` keeps this file honest. It scans the sources for the variables
they read and the CMake files for their options, and fails when a switch is missing here or
listed here but no longer read. CI runs it through `tools/test_opt_in_flags.py`.

## Keeping it up to date

When a change adds, renames or removes a switch, or changes its default, update this file in
the same change:

1. Add, rename or remove its row in the section for the area that reads it. Keep rows sorted.
2. Fill in every column. **Default** is the behaviour with the switch unset: `unset` means it
   does nothing until set, `on` means the behaviour is active unless set to `0`, and a value
   means that value is assumed. **Class** is one of:
   - `feature`: supported behaviour a port, a player or a build may rely on;
   - `experimental`: a behaviour change or workaround still being evaluated, often for one
     title;
   - `diagnostic`: logging, dumps, watches, forcing and other aids for debugging only.
3. Run `python tools/check_opt_in_flags.py` (`--list` prints every switch it finds and where).

The check finds a variable when it is read through `getenv`, `ps3_env_once` and the other
direct readers with a string literal, through a `#define NAME "VAR"` macro, or through one of
the file-local helpers listed in `WRAPPERS` in the script. A read whose name is computed (a
variable, an array element) must be listed in `DYNAMIC_READS` with the names it can read; the
check fails on any it does not know. Tests (`tests/` directories and `test_*` files) are not
scanned.

Rows whose description says "Not described where it is read" are switches whose read site
carries no description; the text gives only what the code does with the value. When you next
touch one, describe it properly. `docs/DIAGNOSTICS.md` is a separate generated index of read
sites and the comments at them.

## CMake options

| Switch | Kind | Default | Class | What it does | Read in |
|---|---|---|---|---|---|
| `PORT_ENABLE_SPU` | CMake | ON | feature | Project template: compile and register the title's lifted SPU images. | `templates/project/CMakeLists.txt` |
| `PORT_SPU_INPUTS` | CMake | empty | feature | Project template: the lifted SPU sources to compile. | `templates/project/CMakeLists.txt` |
| `PS3RECOMP_ATRAC_LGPL` | CMake | ON | feature | Link the LGPL-2.1 ATRAC3plus decoder for cellAtrac; OFF swaps in a stub that fails every open. | `CMakeLists.txt` |
| `PS3RECOMP_BUILD_TESTS` | CMake | OFF | feature | Build the unit tests and register them with CTest. | `CMakeLists.txt` |
| `PS3RECOMP_FFMPEG_RUNTIME` | CMake | OFF | feature | Decode media through FFmpeg loaded at run time (needs the FFmpeg headers at build time). | `CMakeLists.txt` |
| `PS3RECOMP_RSX_VULKAN` | CMake | OFF | feature | Build the Vulkan RSX backend (Linux). | `CMakeLists.txt` |
| `RECOMP_DIR` | CMake | `recompiled/` | feature | Project template: directory holding the lifted PPU sources. | `templates/project/CMakeLists.txt` |
| `SPU_DIR` | CMake | `spu/` | feature | Project template: directory holding the lifted SPU images. | `templates/project/CMakeLists.txt` |

## Runtime platform

| Switch | Kind | Default | Class | What it does | Read in |
|---|---|---|---|---|---|
| `PS3_FRONTIER_RING` | env | unset | diagnostic | 2026-08-06 handoff-ordering frontier): arm at init instead of waiting for the semantic Job B selection. | `runtime/diag/yz_frontier_trace.c` |
| `PS3_FRONTIER_RING_COMPACT` | env | unset | diagnostic | Behaviour switch; reads a value. Not described where it is read. | `runtime/diag/yz_frontier_trace.c` |
| `PS3_FRONTIER_RING_PARITY` | env | unset | diagnostic | Behaviour switch; reads a value. Not described where it is read. | `runtime/diag/yz_frontier_trace.c` |
| `PS3_FRONTIER_RING_PATH` | env | unset | diagnostic | Behaviour switch; reads a path. Not described where it is read. | `runtime/diag/yz_frontier_trace.c` |
| `PS3_FRONTIER_RING_TSV` | env | unset | diagnostic | Behaviour switch; reads a value. Not described where it is read. | `runtime/diag/yz_frontier_trace.c` |
| `PS3_FTRACE` | env | unset | diagnostic | Trace output; reads a value. Not described where it is read. | `include/ps3emu/ps3_ftrace.h` |
| `PS3_FTRACE_CAP` | env | unset | diagnostic | Spans kept, default 4M = 128 MB, allocated only when recording). | `include/ps3emu/ps3_ftrace.h` |
| `PS3_FTRACE_MERGE_US` | env | unset | diagnostic | Usleep polls of one thread at one call site closer than this become one span, default 300), PS3_FTRACE_CAP (spans kept, default 4M = 128 MB, allocated only when… | `include/ps3emu/ps3_ftrace.h` |
| `PS3_FTRACE_MIN_US` | env | unset | diagnostic | Shortest span kept, default 20), PS3_FTRACE_MERGE_US (usleep polls of one thread at one call site closer than this become one span, default 300), PS3_FTRACE_CAP… | `include/ps3emu/ps3_ftrace.h` |
| `PS3_MILESTONE_OUT` | env | unset | diagnostic | Behaviour switch; reads a value. Not described where it is read. | `runtime/milestone.c` |
| `PS3_STDIO_FLUSH_MS` | env | built-in | feature | How often buffered guest output is flushed, in milliseconds. | `runtime/platform/ps3_stdio.c` |
| `PS3_STDIO_UNBUFFERED` | env | unset | feature | `1` leaves stdout unbuffered. | `runtime/platform/ps3_stdio.c` |
| `PS3_TPROF` | env | unset | diagnostic | Profiling; reads a value. Not described where it is read. | `include/ps3emu/ps3_tprof.h` |
| `PS3_TPROF_CAP` | env | unset | diagnostic | Profiling; reads a value. Not described where it is read. | `include/ps3emu/ps3_tprof.h` |
| `PS3_TPROF_INTERVAL_US` | env | unset | diagnostic | Default 1000) for PS3_TPROF_SECONDS (default 10), starting when PS3_TPROF_TRIGGER=<file> exists (immediately when unset). | `include/ps3emu/ps3_tprof.h` |
| `PS3_TPROF_PPU` | env | unset | diagnostic | Profiling; reads a value. Not described where it is read. | `include/ps3emu/ps3_tprof.h` |
| `PS3_TPROF_SECONDS` | env | unset | diagnostic | Default 10), starting when PS3_TPROF_TRIGGER=<file> exists (immediately when unset). | `include/ps3emu/ps3_tprof.h` |
| `PS3_TPROF_TRIGGER` | env | unset | diagnostic | Profiling; reads a value. Not described where it is read. | `include/ps3emu/ps3_tprof.h` |
| `PS3_VERBOSE` | env | unset | diagnostic | Behaviour switch; reads a value. Not described where it is read. | `runtime/ps3_log.h` |

## PPU, loader and HLE dispatch

| Switch | Kind | Default | Class | What it does | Read in |
|---|---|---|---|---|---|
| `ALLOCPROBE` | env | unset | diagnostic | Behaviour switch; reads any value. Not described where it is read. | `runtime/ppu/ppu_loader.cpp` |
| `BCTRL_RING` | env | unset | diagnostic | Keep the last N indirect-call targets per thread, and let anything that can name a moment dump them. | `runtime/ppu/ppu_loader.cpp` |
| `FLOW_FORCE_RTCFG` | env | unset | diagnostic | FlOw calls PCoreGcmRenderInterface:: setScreenRenderTarget(this, config) exactly once, with config = NULL, and every later setScreenRenderTargetInternal() then… | `runtime/ppu/ppu_loader.cpp` |
| `FLOW_NOSPILL` | env | unset | experimental | With the lifted code using a constant main TOC (--main-toc/TOCFIX), the caller no longer reads [r1+0x28] to restore r2, so this ABI TOC-spill is unnecessary — and in… | `runtime/ppu/ppu_hle.cpp` |
| `FLOW_PSSGTRACE` | env | unset | diagnostic | When the title's tty output carries a PhyreEngine init-failure fragment, dump the HOST backtrace (RVAs vs the exe base) so we can map the error-reporter's call chain… | `runtime/ppu/ppu_loader.cpp`, `runtime/syscalls/lv2_register.c` |
| `FLOW_SPURSKERNEL` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/ppu/ppu_hle.cpp` |
| `FS_CALLER` | env | unset | diagnostic | Name the guest function that opened each file. | `runtime/ppu/ppu_fs.cpp` |
| `FS_FSTAT_CAP` | env | unset | diagnostic | <bytes>: DIAGNOSTIC. Report a smaller size than the file has. | `runtime/ppu/ppu_fs.cpp` |
| `FS_FSTAT_CAP_PATH` | env | unset | diagnostic | Counters; reads a path. Not described where it is read. | `runtime/ppu/ppu_fs.cpp` |
| `FS_READ_ALL` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/ppu/ppu_fs.cpp` |
| `FS_READ_PATH` | env | unset | experimental | Behaviour switch; reads a path. Not described where it is read. | `runtime/ppu/ppu_fs.cpp` |
| `FS_STACK` | env | unset | diagnostic | Every open funnels through one guest wrapper, so one caller level says nothing about WHICH subsystem wanted the file. | `runtime/ppu/ppu_fs.cpp` |
| `GCM_REFPOLL` | env | on | feature | Publish RSX reference values when the guest reads them; `0` publishes only on the 60 Hz ticker. | `runtime/ppu/ppu_loader.cpp` |
| `HLE_BT_EVERY` | env | unset | diagnostic | Dump the calling thread's guest stack every nth HLE call it makes. | `runtime/ppu/ppu_hle.cpp` |
| `LWM_COUNT` | env | unset | diagnostic | Counters; reads any value. Not described where it is read. | `runtime/ppu/ppu_sysprx.cpp` |
| `PPU_CASWATCH` | env | unset | diagnostic | Watch; reads a number. Not described where it is read. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_CAS_FAIL` | env | unset | experimental | Histogram the EAs whose store-conditional keeps FAILING. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_CSCHECK` | env | unset | diagnostic | Every indirect call must hand back r1, r2 and r14..r31 as it found them (the ABI's non-volatile set. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_FORCE_READ_ADDR` | env | unset | diagnostic | Force reads of one address to PPU_FORCE_READ_VAL (default 0) -- diagnostic to break a completion spin and see whether the game proceeds. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_FORCE_READ_VAL` | env | unset | diagnostic | Default 0) -- diagnostic to break a completion spin and see whether the game proceeds. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_GUARD_EA` | env | unset | experimental | Extra checks; reads a value. Not described where it is read. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_HOTMAP` | env | unset | diagnostic | Behaviour switch; reads any value. Not described where it is read. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_HOTREAD` | env | unset | diagnostic | Behaviour switch; reads a value. Not described where it is read. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_KEEP_EA` | env | unset | experimental | Snapshot that guest range at load time and restore it continuously. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_NOOP_BAD_VCALL` | env | unset | experimental | Also no-op MISALIGNED / out-of-code-range targets. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_OPD_RECOVER` | env | unset | experimental | Ctr==0 while r12 still points at a VALID OPD means the lifted thunk failed to load the code word -- not that the function pointer is genuinely null. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_POKE` | env | unset | diagnostic | Behaviour switch; reads a value. Not described where it is read. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_POKE8` | env | unset | diagnostic | <hex ea>:<hex byte>[,...] -- the byte-wide form. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_POKE_ADDR` | env | unset | diagnostic | Behaviour switch; reads a value. Not described where it is read. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_POKE_AFTER_MS` | env | unset | diagnostic | Behaviour switch; reads a value. Not described where it is read. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_POKE_MS` | env | unset | diagnostic | Behaviour switch; reads a number. Not described where it is read. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_POKE_VAL` | env | unset | diagnostic | Behaviour switch; reads a number. Not described where it is read. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_RESV_OFF` | env | unset | diagnostic | Store-conditionals skip reservation validation. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_RESV_STORE` | env | unset | experimental | Plain stores break other threads' PPU reservations on the same address. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_RETWATCH` | env | unset | diagnostic | Catch the first vcall(s) whose RETURN value (r3) equals this, pinning the exact virtual method that produces the poison. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_RVAL` | env | unset | diagnostic | Behaviour switch; reads a value. Not described where it is read. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_RVAL64` | env | unset | diagnostic | Behaviour switch; reads a value. Not described where it is read. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_RWATCH` | env | unset | diagnostic | See the note in vm_read8. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_SPINBT` | env | unset | diagnostic | One-shot host backtrace when the read32 hot spin is on the watched address -- names the guest function containing the spin loop + its callers (resolve RVAs against… | `runtime/ppu/ppu_loader.cpp` |
| `PPU_STUCKMAX` | env | unset | experimental | A limit; reads a number. Not described where it is read. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_TOCFIX` | env | unset | experimental | The dispatched function's TOC (r2) is the OPD toc we were called with. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_TOCWATCH` | env | unset | diagnostic | Watch; reads a number. Not described where it is read. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_VCALL_BT` | env | unset | diagnostic | Host backtraces; reads any value. Not described where it is read. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_WVAL` | env | unset | diagnostic | Log every PPU store that WRITES this value, wherever it lands. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_WVAL_GATE` | env | unset | diagnostic | Only while <file> exists (checked on matches). | `runtime/ppu/ppu_loader.cpp` |
| `PPU_WVAL_HI` | env | unset | diagnostic | Behaviour switch; reads a value. Not described where it is read. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_WVAL_LO` | env | unset | diagnostic | HI: restrict to a destination range. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_WWATCH` | env | unset | diagnostic | Log every PPU store into the 16-byte line at that EA, with the writing guest function -- to find who fills (or fails to fill) a struct field (e.g. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_WWATCH_LEN` | env | unset | diagnostic | Widen the watched span. | `runtime/ppu/ppu_loader.cpp` |
| `PPU_WWATCH_MAX` | env | unset | diagnostic | How many hits to print (default 64, 0 = unlimited). | `runtime/ppu/ppu_loader.cpp` |
| `PPU_WW_GUARD` | env | unset | experimental | Once the watched word is first SET, page-guard it. | `runtime/ppu/ppu_loader.cpp` |
| `PS3_ALLOCTAG` | env | unset | diagnostic | Behaviour switch; reads any value. Not described where it is read. | `runtime/ppu/ppu_loader.cpp` |
| `PS3_ARGV` | env | unset | feature | Extra guest arguments appended after `argv[0]`. | `runtime/ppu/ppu_loader.cpp` |
| `PS3_BOOT_PATH` | env | unset | feature | The guest path passed as the title's `argv[0]`. | `runtime/ppu/ppu_loader.cpp` |
| `PS3_BREADCRUMB` | env | unset | diagnostic | Per-tid last indirect-call target + call count. | `runtime/ppu/ppu_loader.cpp`, `runtime/syscalls/sys_semaphore.c` |
| `PS3_CALLTRACE` | env | unset | diagnostic | Log the first N indirect calls (target + r3/r4) -- generic visibility into vtable/callback dispatch (e.g. | `runtime/ppu/ppu_loader.cpp` |
| `PS3_CALLTRACE_FROM` | env | unset | diagnostic | Trace output; reads a value. Not described where it is read. | `runtime/ppu/ppu_loader.cpp` |
| `PS3_CALLTRACE_LR` | env | unset | diagnostic | Only calls returning to this guest address. | `runtime/ppu/ppu_loader.cpp` |
| `PS3_CALLTRACE_R5` | env | unset | diagnostic | Only calls with this selector in r5. | `runtime/ppu/ppu_loader.cpp` |
| `PS3_CALLTRACE_TO` | env | unset | diagnostic | Only calls whose TARGET is this address -- the way to catch an entry point that has no direct callers. | `runtime/ppu/ppu_loader.cpp` |
| `PS3_DEV_FLASH` | env | unset | feature | Host directory holding a `dev_flash` firmware tree. | `runtime/ppu/ppu_fs.cpp`, `runtime/syscalls/sys_fs.c` |
| `PS3_EXIT_CHAIN` | env | unset | experimental | 1 dumps those too. | `runtime/ppu/ppu_hle.cpp` |
| `PS3_FSLOG` | env | unset | diagnostic | Log output; reads any value. Not described where it is read. | `runtime/ppu/ppu_fs.cpp` |
| `PS3_FSLOG_EOF` | env | unset | diagnostic | Report end-of-file past <bytes> on one descriptor. | `runtime/ppu/ppu_fs.cpp` |
| `PS3_FSLOG_READS` | env | unset | diagnostic | Log every read on one descriptor. | `runtime/ppu/ppu_fs.cpp` |
| `PS3_FSLOG_TOC` | env | unset | diagnostic | Log output; reads any value. Not described where it is read. | `runtime/ppu/ppu_fs.cpp` |
| `PS3_FS_BLOCK_SIZE` | env | `4096` | feature | Block size cellFs reports for files and the disc. | `runtime/ppu/ppu_fs.cpp` |
| `PS3_HLE_ARGS` | env | unset | experimental | Dump the full PPC64 argument register set (r3-r10) for one import. | `runtime/ppu/ppu_hle.cpp` |
| `PS3_HLE_PTRCHECK` | env | unset | diagnostic | Extra checks; reads any value. Not described where it is read. | `runtime/ppu/ppu_hle.cpp` |
| `PS3_HLE_TOP` | env | unset | experimental | The most-called HLE imports over each window, with the guest thread and caller of the last call. | `runtime/ppu/ppu_hle.cpp` |
| `PS3_HLE_TRACE` | env | unset | diagnostic | Trace output; reads a number. Not described where it is read. | `runtime/ppu/ppu_hle.cpp` |
| `PS3_HLE_UNRESOLVED` | env | fake | feature | What an unresolved import returns: `fail` returns an error, anything else fakes success. | `runtime/ppu/ppu_hle.cpp` |
| `PS3_ICALL_GATE` | env | unset | diagnostic | While <file> exists, histogram indirect-call targets (per guest thread) and how often each returned r3 == 0. | `runtime/ppu/ppu_loader.cpp` |
| `PS3_ICALL_TRACE` | env | unset | diagnostic | Also log every call on that guest thread, in order. | `runtime/ppu/ppu_loader.cpp` |
| `PS3_LWMUTEX_TRACE` | env | unset | diagnostic | Reconstruct the lwmutex lock-convoy that stalls LBP's loader. | `runtime/ppu/ppu_sysprx.cpp` |
| `PS3_MEMTRACE` | env | unset | diagnostic | Trace output; reads any value. Not described where it is read. | `runtime/ppu/ppu_sysprx.cpp` |
| `PS3_NO_GUEST_PTR_TRAP` | env | unset | diagnostic | Turns a behaviour off; reads any value. Not described where it is read. | `runtime/ppu/ppu_loader.cpp` |
| `PS3_NULL_CALL` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `runtime/ppu/ppu_loader.cpp` |
| `PS3_NULL_READ` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `runtime/ppu/ppu_loader.cpp` |
| `PS3_NULL_READ_STACK` | env | unset | diagnostic | Guest-fn is the nearest host symbol, which can name a neighbour. | `runtime/ppu/ppu_loader.cpp` |
| `PS3_NULL_SWEEP` | env | unset | experimental | Contain a runaway sweep that started from a null pointer. | `runtime/ppu/ppu_loader.cpp` |
| `PS3_NULL_WRITE` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `runtime/ppu/ppu_loader.cpp` |
| `PS3_PRX_CALLTRACE` | env | unset | diagnostic | Trace output; reads any value. Not described where it is read. | `runtime/ppu/ppu_loader.cpp` |
| `PS3_SAMPLE` | env | unset | diagnostic | A sampling profiler over GUEST code. | `runtime/ppu/ppu_loader.cpp` |
| `PS3_SAMPLE_DRAIN` | env | unset | diagnostic | Only the thread that last walked the RSX FIFO (cellGcm_rsx_process_fifo records it), so the RVA histogram is the drain's own cost, not the spin loops of every SPU thread. | `runtime/ppu/ppu_loader.cpp` |
| `PS3_SAMPLE_MAIN` | env | unset | diagnostic | Profile ONLY the boot thread (guest tid 1), and credit a sample whose RIP is in runtime code (vm_read32, ps3_indirect_call, ...) to the nearest lifted return address… | `runtime/ppu/ppu_loader.cpp` |
| `PS3_SAMPLE_RVAS` | env | unset | diagnostic | Every sample's address as an RVA of the executable, one hex word per line, appended at each report. | `runtime/ppu/ppu_loader.cpp` |
| `PS3_SCBLOCK_PROF` | env | unset | diagnostic | Profiling; reads any value. Not described where it is read. | `runtime/ppu/ppu_loader.cpp` |
| `PS3_SCENTER` | env | unset | experimental | Log the syscall BEFORE dispatching it. | `runtime/ppu/ppu_loader.cpp` |
| `PS3_SCTRACE` | env | unset | diagnostic | Every lv2 syscall with its arguments and RETURN VALUE. | `runtime/ppu/ppu_loader.cpp` |
| `PS3_SCTRACE_TID` | env | unset | diagnostic | Trace every lv2 syscall made by the loader/worker thread (tid=1) so we can see what it does AFTER receiving its q=1 event and why it never registers handlers / loads… | `runtime/ppu/ppu_loader.cpp` |
| `PT` | env | unset | experimental | Persistent high-byte truncation detector. | `runtime/ppu/ppu_loader.cpp` |
| `RESV_DIAG` | env | unset | diagnostic | Detect a stwcx whose ctx isn't in the registry -- if that fires, that thread's reservations are never broken by others (ABA slips through), and the fix is to register… | `runtime/ppu/ppu_loader.cpp` |
| `SPURS_TRACE` | env | unset | diagnostic | Trace output; reads any value. Not described where it is read. | `runtime/ppu/ppu_hle.cpp`, `runtime/ppu/ppu_loader.cpp` |
| `SPU_IMAGE_DIAG` | env | unset | diagnostic | R6 is a caller-supplied guest buffer that differs on every call, which this handler ignores -- it points the segments at the source image instead. | `runtime/ppu/ppu_sysprx.cpp` |
| `SPU_PUTLLC_WHY` | env | unset | diagnostic | Count PPU commits that land on a line an SPU has reserved. | `runtime/ppu/ppu_loader.cpp`, `runtime/spu/spu_channels.c` +1 |
| `TTY_BT` | env | unset | diagnostic | Dump the call chain whenever the title prints a line containing it. | `runtime/ppu/ppu_loader.cpp`, `runtime/syscalls/lv2_register.c` |
| `TTY_NO_DEDUPE` | env | unset | diagnostic | Turns a behaviour off; reads a value. Not described where it is read. | `runtime/ppu/ppu_loader.cpp` |
| `YDKJ_CRI_CREATE_OK` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/ppu/ppu_hle.cpp` |
| `YDKJ_CRI_EVFLAG` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `runtime/ppu/ppu_hle.cpp` |
| `YDKJ_EVFLAG_DIR` | env | unset | experimental | 3 = ANY2ANY by default. | `runtime/ppu/ppu_hle.cpp` |
| `YDKJ_EVFLAG_INIT` | env | unset | experimental | Run Sony's OWN initializer on the flag rather than stamping bytes. | `runtime/ppu/ppu_hle.cpp` |
| `YDKJ_FORCEATTR` | env | unset | diagnostic | Forces a behaviour; reads any value. Not described where it is read. | `runtime/ppu/ppu_hle.cpp` |
| `YDKJ_FORCE_HLE` | env | unset | diagnostic | Comma-separated hex). | `runtime/ppu/ppu_hle.cpp` |
| `YDKJ_GUESTINIT` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/ppu/ppu_hle.cpp` |
| `YDKJ_GUESTINIT_CRI` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/ppu/ppu_hle.cpp` |
| `YDKJ_MEMFIX` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/ppu/ppu_loader.cpp` |
| `YDKJ_NETOFFLINE` | env | unset | experimental | Unresolved network NIDs (sys_net/cellHttp/cellSysutil) default r3=0 (fake success) -> the game stores a NULL handle as an object and later virtual-calls it -> the… | `runtime/ppu/ppu_hle.cpp` |
| `YDKJ_NO_REENTRY_ISO` | env | unset | diagnostic | Turns a behaviour off; reads a value. Not described where it is read. | `runtime/ppu/ppu_hle.cpp` |
| `YDKJ_RWATCH` | env | unset | diagnostic | Watch; reads a value. Not described where it is read. | `runtime/ppu/ppu_loader.cpp` |
| `YDKJ_SAVEDATA` | env | unset | diagnostic | Behaviour switch; reads any value. Not described where it is read. | `runtime/ppu/ppu_hle.cpp` |
| `YDKJ_TUNERFIX` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/ppu/ppu_hle.cpp` |

## SPU and SPURS

| Switch | Kind | Default | Class | What it does | Read in |
|---|---|---|---|---|---|
| `LBP_HLE_JOBDONE` | env | unset | diagnostic | HLE the WWS job COMPLETION. | `runtime/spu/spu_channels.c` |
| `LBP_NO_TASKSET` | env | unset | diagnostic | Restores the old direct-dispatch for comparison. | `runtime/spu/spu_workload.c` |
| `LBP_PERSIST_LS` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/spu/spu_workload.c` |
| `LBP_POLICY_CTX` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/spu/spu_workload.c` |
| `LBP_REAL_POLICY` | env | unset | experimental | Run the REAL lifted taskset policy to build its SpursTasksetContext and diff it against the C reimpl build_context planted in `ls` -- shows exactly which fields differ. | `runtime/spu/spu_workload.c` |
| `LBP_SKIP_NULL_DMA` | env | unset | diagnostic | Turns a behaviour off; reads any value. Not described where it is read. | `runtime/spu/spu_dma.h` |
| `LBP_SPU_SERIAL` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/spu/spu_workload.c` |
| `LBP_SYNC_ACK` | env | unset | experimental | Taskset sync-lane bookkeeping. | `runtime/spu/spu_workload.c` |
| `LBP_TASK_DELAY` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `runtime/spu/spu_workload.c` |
| `LBP_WS_DRAIN` | env | unset | experimental | A task parked in WAIT_SIGNAL resumes + releases the taskset, letting the next task run), the FMOD init + DSP tasks share one LS and the handler table is populated… | `runtime/spu/spu_workload.c` |
| `PS3_NO_JOBCHAIN` | env | unset | diagnostic | Turns a behaviour off; reads any value. Not described where it is read. | `libs/spurs/cellSpurs.c` |
| `PS3_NO_SPURS_PM` | env | unset | diagnostic | Turns a behaviour off; reads any value. Not described where it is read. | `libs/spurs/cellSpurs.c` |
| `PS3_SPU_DIRECT` | env | on | feature | Run SPU programs directly; `0` (or any of the SPU diagnostics that need the drain path) turns it off. | `runtime/spu/spu_drain.c` |
| `PS3_SPU_PCWATCH` | env | unset | diagnostic | Attribute silent SPU spins (a task that stops logging but never returns). | `runtime/spu/spu_dispatch_mt.c` |
| `SPURS_EF_FORCE` | env | unset | diagnostic | Forces a behaviour; reads any value. Not described where it is read. | `libs/spurs/cellSpurs.c` |
| `SPURS_EF_OBJ_DELIVER` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `libs/spurs/cellSpurs.c`, `runtime/spu/spu_channels.c` |
| `SPURS_EF_SPU_REPLY` | env | unset | experimental | DIAGNOSTIC BISECT, NOT A FIX. | `runtime/spu/spu_channels.c` |
| `SPURS_EF_STATS` | env | unset | diagnostic | Per flag, how many PPU waits completed in the last 5 s and their mean / max duration -- one SPU task round trip each. | `libs/spurs/cellSpurs.c` |
| `SPURS_EF_WAKE_PARKED` | env | unset | experimental | A task that parked in WAIT_SIGNAL WITHOUT registering a wait slot in this flag is unreachable by the loop above, which only signals slots present in `used`. | `libs/spurs/cellSpurs.c` |
| `SPURS_EVENT_D3` | env | unset | experimental | Probe. The value a job query returns to the PPU is read from the completion event's data3 field (the guest stores r7 at sp+0xB8 and reads the count back from sp+0xBC). | `libs/spurs/cellSpurs.c` |
| `SPURS_IDLE_MAX` | env | unset | experimental | Default 4, 0..8): cap of the idle back-off, as log2 of the pass cadence. | `libs/spurs/cellSpurs.c` |
| `SPURS_JC_DELAY` | env | unset | experimental | The real jm2 chain walker is async and picks up jobs as the PPU appends them + fills their descriptors. | `libs/spurs/cellSpurs.c` |
| `SPURS_JOB_DESCDUMP` | env | unset | diagnostic | Hexdump the guest job descriptor. | `runtime/spu/spurs_job.c` |
| `SPURS_JOB_DUMP` | env | unset | diagnostic | Dumps; reads any value. Not described where it is read. | `runtime/spu/spurs_job.c` |
| `SPURS_JOB_LSWORD` | env | unset | experimental | After a job of that image, print the LS word at lsa and every other LS offset holding the same value. | `runtime/spu/spurs_job.c` |
| `SPURS_JOB_MBOX` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/spu/spurs_job.c` |
| `SPURS_PM_CLAIMLOG` | env | unset | diagnostic | Per-run claim-vs-stage summary for BOTH lanes. | `runtime/spu/spurs_policy.c` |
| `SPURS_PM_DUMP` | env | unset | diagnostic | Dumps; reads any value. Not described where it is read. | `libs/spurs/cellSpurs.c` |
| `SPURS_PM_FLOW` | env | unset | experimental | Record every cross-function transfer of ONE policy-module run (the ctx spurs_policy.c arms) so the post-claim decision path can be reconstructed offline. | `runtime/spu/spu_drain.c`, `runtime/spu/spurs_policy.c` |
| `SPURS_PM_FLOW_ALL` | env | unset | experimental | Trace EVERY spu_num-0 run of this workload without requiring the PPU-side barrier probe to have armed g_barrier_sync_watch. | `runtime/spu/spurs_policy.c` |
| `SPURS_PM_FRESH` | env | unset | experimental | Treat EVERY dispatch as first contact (zeroed LS, image reloaded). | `runtime/spu/spurs_policy.c` |
| `SPURS_PM_PIPE` | env | unset | experimental | Trace how the WWS job PIPELINE state evolves across kernel dispatches. | `runtime/spu/spurs_policy.c` |
| `SPURS_PM_TRACE` | env | unset | diagnostic | #521) -- and even with the trace asked for they are bounded by process, not by run (#431), which is what the exit-to-kernel line above already does. | `runtime/spu/spu_channels.c`, `runtime/spu/spurs_policy.c` |
| `SPURS_QUEUE_PUSH` | env | unset | experimental | EXPERIMENT, off by default. | `libs/spurs/cellSpurs.c` |
| `SPURS_TASKATTR_DESC` | env | unset | experimental | A THIRD caller shape, found in Saints Row 2. | `libs/spurs/cellSpurs.c` |
| `SPURS_TASKATTR_LEGACY` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `libs/spurs/cellSpurs.c` |
| `SPURS_TASKSET_TRACE` | env | unset | diagnostic | Trace output; reads any value. Not described where it is read. | `libs/spurs/cellSpurs.c`, `runtime/spu/spu_lifted_job.h` |
| `SPU_ARGWATCH` | env | unset | diagnostic | Watch; reads any value. Not described where it is read. | `runtime/spu/spu_channels.c` |
| `SPU_ATOMTRACE` | env | unset | diagnostic | Trace output; reads a value. Not described where it is read. | `runtime/spu/spu_channels.c` |
| `SPU_ATOM_EA` | env | unset | experimental | Log every lock-line atomic touching that EA`s 128B line. | `runtime/spu/spu_channels.c` |
| `SPU_ATOM_EA_MAX` | env | unset | experimental | Lines to print (default 48, 0 = unlimited). | `runtime/spu/spu_channels.c` |
| `SPU_ATOM_FULL` | env | unset | experimental | Behaviour switch; reads a number. Not described where it is read. | `runtime/spu/spu_channels.c` |
| `SPU_CALLTRACE` | env | unset | diagnostic | Trace output; reads any value. Not described where it is read. | `runtime/spu/spu_interp.c` |
| `SPU_CHHIST` | env | unset | diagnostic | Which channels a job actually touches, and how often. | `runtime/spu/spu_channels.c` |
| `SPU_CH_BLOCK` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/spu/spu_channels.c` |
| `SPU_CMDDUMP` | env | unset | diagnostic | Decode the FULL WWS command list (8-byte cmds). | `runtime/spu/spu_dma.h` |
| `SPU_CMDHIST` | env | unset | diagnostic | Every MFC command that reaches the engine, by opcode and SPU image. | `runtime/spu/spu_dma.h` |
| `SPU_CTX_LOG` | env | unset | diagnostic | One line per job context, with its address. | `runtime/spu/spu_lifted_job.h`, `runtime/spu/spurs_policy.c` |
| `SPU_DBG_MBOX` | env | unset | diagnostic | Depth after the write. | `runtime/spu/spu_channels.c`, `runtime/spu/spu_raw.c` |
| `SPU_DISPATCH_CACHE` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `runtime/spu/spu_channels.c` |
| `SPU_DMACHK` | env | unset | experimental | Report transfers that break the MFC rules the guest's own dma.h asserts on -- size 0 or > 16 KB, size not a multiple of 16 for transfers of 16+ bytes, or LSA/EA not… | `runtime/spu/spu_dma.h` |
| `SPU_DMATRACE` | env | unset | diagnostic | Trace output; reads a value. Not described where it is read. | `runtime/spu/spu_dma.h` |
| `SPU_DMATRACE_ALL` | env | unset | diagnostic | Trace output; reads any value. Not described where it is read. | `runtime/spu/spu_dma.h` |
| `SPU_DMATRACE_RAW` | env | unset | diagnostic | Trace output; reads any value. Not described where it is read. | `runtime/spu/spu_dma.h` |
| `SPU_DMA_LAX` | env | unset | experimental | 1 restores the old permissive behaviour. | `runtime/spu/spu_dma.h` |
| `SPU_DMA_RANGE` | env | unset | experimental | Hex EAs): log transfers touching [lo,hi), first 64 -- who reads or writes a buffer the RSX shares with the SPUs. | `runtime/spu/spu_dma.h` |
| `SPU_DMA_REPEAT_BACKOFF_MS` | env | unset | experimental | Default 2) of an episode, where a healthy wait ends, and back off 1 ms per repeat after that. | `runtime/spu/spu_dma.h` |
| `SPU_DMA_REPEAT_LIMIT` | env | unset | diagnostic | A limit; reads a value. Not described where it is read. | `runtime/spu/spu_dma.h` |
| `SPU_DMA_REPEAT_LOG` | env | unset | diagnostic | Report the first n repeat episodes per thread as they end (image, pc, transfer, repeats, duration), and a per-image total every 4096 episodes. | `runtime/spu/spu_dma.h` |
| `SPU_DMA_REPEAT_SECS` | env | unset | experimental | Default 30) of wall time. | `runtime/spu/spu_dma.h` |
| `SPU_DMA_SKIP_DUMP` | env | unset | diagnostic | Show the LS payload a skipped PUT was carrying. | `runtime/spu/spu_dma.h` |
| `SPU_DMA_WATCH` | env | unset | diagnostic | Report any transfer whose destination RANGE covers this guest address. | `runtime/spu/spu_dma.h` |
| `SPU_DSPDESC` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `runtime/spu/spu_channels.c` |
| `SPU_DSP_IMAGE_EA` | env | unset | experimental | Recover an SPU image load whose source address the title lost. | `runtime/spu/spu_dma.h` |
| `SPU_DUMP_GET` | env | unset | diagnostic | Dump the source bytes the first time an SPU fetches from that address. | `runtime/spu/spu_dma.h` |
| `SPU_DUMP_MISS` | env | unset | diagnostic | Write the PM exactly as fingerprinted, like the two miss paths in spu_workload.c. | `libs/spurs/cellSpurs.c`, `runtime/spu/spu_workload.c` |
| `SPU_DUMP_OVL` | env | unset | diagnostic | Dumps; reads a value. Not described where it is read. | `runtime/spu/spu_channels.c` |
| `SPU_EXITTRACE` | env | unset | diagnostic | Remember the last PCs this image executed and dump them when the job ends. | `runtime/spu/spu_drain.c` |
| `SPU_FLOATFIX_STATS` | env | unset | diagnostic | Count why lanes reach the fix-ups (T-0014) and print the tally every 2^24 lanes. | `runtime/spu/spu_helpers.h` |
| `SPU_FLOAT_RZ` | env | unset | experimental | SPU code runs with FPCR at round toward zero with flush-to-zero, so the single-precision helpers take their native form (spu_helpers.h, spu__nv_rz). | `runtime/spu/spu_channels.c` |
| `SPU_GETHIST` | env | unset | diagnostic | The same accounting for GETs, on the SOURCE side. | `runtime/spu/spu_dma.h` |
| `SPU_H2LSRC` | env | unset | experimental | For each Host2Local_Body write to PS1 VRAM, the bytes it is copying OUT of local store next to the bytes already at the destination. | `runtime/spu/spu_dma.h` |
| `SPU_HOST_DEPTH_MAX` | env | unset | experimental | A limit; reads a value. Not described where it is read. | `runtime/spu/spu_drain.c` |
| `SPU_IBCOV` | env | unset | diagnostic | Image-3 (Bink SPU) PC-page coverage. | `runtime/spu/spu_channels.c` |
| `SPU_INTERP_STATS` | env | unset | diagnostic | How much SPU code runs interpreted, by image and 1 KB page of local store, and where the interpreter was entered. | `runtime/spu/spu_interp.c` |
| `SPU_INTERP_UNLIFTED` | env | unset | experimental | If real code is present at the target, run it through the interpreter instead of ending the job. | `runtime/spu/spu_channels.c` |
| `SPU_IRQTRACE` | env | unset | diagnostic | Trace output; reads any value. Not described where it is read. | `runtime/spu/spu_channels.c`, `runtime/spu/spu_drain.c` |
| `SPU_IRQWAIT_DIAG` | env | unset | diagnostic | An SPU with interrupts enabled and nothing pending to take, every 2^22 such dispatches on a host thread (24 lines at most): pc, event status and mask, and the MFC… | `runtime/spu/spu_drain.c` |
| `SPU_IRQWAIT_LS` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `runtime/spu/spu_drain.c` |
| `SPU_JOBDRAIN` | env | unset | experimental | Default off while validating). | `runtime/spu/spu_drain.c` |
| `SPU_JOBEXEC` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/spu/spu_channels.c` |
| `SPU_JOBEXEC_DUMP` | env | unset | diagnostic | Write the full 256KB LS the first time a job entry (pc==codeBuffer+entryOffset) dispatches, to diff vs the RPCS3 oracle SPU4 dump (why our job bails where the… | `runtime/spu/spu_channels.c` |
| `SPU_JOBPROF` | env | unset | diagnostic | Host time spent per job image, printed every <secs>. | `runtime/spu/spu_workload.c` |
| `SPU_JOBTRACE` | env | unset | diagnostic | Log EVERY dispatcher entry while a job module (overlay id >= 200) is resident -- the job's cross-function control-flow trail. | `runtime/spu/spu_channels.c` |
| `SPU_LLARWATCH` | env | unset | diagnostic | Watch; reads a value. Not described where it is read. | `runtime/spu/spu_channels.c` |
| `SPU_LOCKGUARD` | env | unset | experimental | Extra checks; reads a number. Not described where it is read. | `runtime/spu/spu_channels.c` |
| `SPU_LOCKLINE_SLEEP` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `runtime/spu/spu_coherency.c` |
| `SPU_LOCKSTEP` | env | unset | diagnostic | The quantum tick), SPU_TAILRET (SPU_DRAIN's return check), SPU_PCHIST, SPU_NO_LS0_END, SPU_EXITTRACE, SPU_STEPTRACE, SPU_JOBDRAIN and SPURS_PM_FLOW. | `runtime/spu/spu_drain.c`, `runtime/spu/spu_lockstep.c` |
| `SPU_LOCKSTEP_QUANTUM` | env | unset | diagnostic | Behaviour switch; reads a value. Not described where it is read. | `runtime/spu/spu_lockstep.c` |
| `SPU_LONGJMP_UNWIND` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/spu/spu_channels.c` |
| `SPU_LSDUMP` | env | unset | diagnostic | Dumps; reads a value. Not described where it is read. | `runtime/spu/spu_channels.c` |
| `SPU_LS_DUMP` | env | unset | diagnostic | Write this job's whole local store when it ends. | `runtime/spu/spu_workload.c` |
| `SPU_LS_LOWREAD` | env | unset | experimental | A job that reads its OWN first bytes as data is dereferencing a null base -- the job binary loads at LS 0, so [NULL+off] returns its own instruction words. | `runtime/spu/spu_context.h` |
| `SPU_LS_WATCH` | env | unset | diagnostic | Watch up to 4 16-byte LS lines (reads and writes) in every SPU image. | `runtime/spu/spu_channels.c` |
| `SPU_MBOXTRACE` | env | unset | diagnostic | Trace output; reads any value. Not described where it is read. | `runtime/spu/spu_channels.c`, `runtime/syscalls/lv2_register.c` |
| `SPU_MBOX_CHAIN` | env | unset | experimental | The guest call chain behind each of the first n inbound mailbox writes, per SPU. | `runtime/spu/spu_raw.c` |
| `SPU_MFC_LOOKUP_CACHE` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `runtime/spu/spu_channels.c` |
| `SPU_MFC_TRACE` | env | unset | diagnostic | Attribute silent DMA-poll loops (a wedged task whose host thread samples "in ntdll" because VirtualQuery dominates). | `runtime/spu/spu_dma.h` |
| `SPU_MISS_DUMP_IMG` | env | unset | diagnostic | Reserve the deep-dump budget for image n's misses (the global 2-shot budget was always consumed by an earlier image's misses, hiding the one under investigation). | `runtime/spu/spu_channels.c` |
| `SPU_MISS_DUMP_PCMIN` | env | unset | diagnostic | Dumps; reads a number. Not described where it is read. | `runtime/spu/spu_channels.c` |
| `SPU_MISS_FATAL` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `runtime/spu/spu_workload.c` |
| `SPU_NO_LS0_END` | env | unset | diagnostic | Turns a behaviour off; reads any value. Not described where it is read. | `runtime/spu/spu_drain.c` |
| `SPU_OVL_DIAG` | env | unset | diagnostic | Debug output; reads any value. Not described where it is read. | `runtime/spu/spu_dma.h` |
| `SPU_PCHIST` | env | unset | diagnostic | Bucket SPU execution by the GPU module's own symbols. | `runtime/spu/spu_context.h`, `runtime/spu/spu_drain.c` |
| `SPU_PCSAMPLE` | env | unset | diagnostic | Every <secs>, print image and pc of every context holding an MFC slot -- i.e. | `runtime/spu/spu_channels.c` |
| `SPU_POISON_DMA_WATCH` | env | unset | diagnostic | Any value): scan every PUT payload for the poison word and also log PUTs that carry it outside the region. | `runtime/spu/spu_dma.h` |
| `SPU_POLLTRACE` | env | unset | diagnostic | Trace output; reads any value. Not described where it is read. | `runtime/spu/spu_channels.c` |
| `SPU_PUTEA` | env | unset | experimental | The first few FULL destination addresses per SPU. | `runtime/spu/spu_dma.h` |
| `SPU_PUTHIST` | env | unset | diagnostic | Histogram of PUT destinations by 1 MB bucket. | `runtime/spu/spu_dma.h` |
| `SPU_RAW_DBG` | env | unset | diagnostic | Debug output; reads any value. Not described where it is read. | `runtime/spu/spu_raw.c` |
| `SPU_RAW_DUMP` | env | unset | diagnostic | Write the image exactly as the guest presents it, so a fingerprint that does not match the registered one can be diffed against the file the offline extractor produced. | `runtime/spu/spu_raw.c` |
| `SPU_RAW_DUMP_DIR` | env | unset | diagnostic | Write an unmatched run out as rawspu_<fp>_<lo>.bin -- the bytes exactly as they sit in local store at <lo>, which is the base to lift them at. | `runtime/spu/spu_raw.c` |
| `SPU_RELOC_OFF` | env | unset | diagnostic | Leave that code to the interpreter (A/B). | `runtime/spu/spu_channels.c` |
| `SPU_ROWCOV` | env | unset | diagnostic | Behaviour switch; reads a value. Not described where it is read. | `runtime/spu/spu_dma.h` |
| `SPU_SIGSTAT` | env | unset | diagnostic | Every n signal writes, the per-SPU tally and the last value written. | `runtime/spu/spu_raw.c` |
| `SPU_SIG_STATS` | env | unset | diagnostic | Signals per second per (taskset, task, caller). | `runtime/spu/spu_workload.c` |
| `SPU_SMC_HI` | env | unset | experimental | Behaviour switch; reads a number. Not described where it is read. | `runtime/spu/spu_context.h`, `runtime/spu/spu_dma.h` |
| `SPU_SMC_LO` | env | unset | experimental | Behaviour switch; reads a number. Not described where it is read. | `runtime/spu/spu_context.h`, `runtime/spu/spu_dma.h` |
| `SPU_SMC_WATCH` | env | unset | diagnostic | Self-modification detector. | `runtime/spu/spu_context.h`, `runtime/spu/spu_dma.h` |
| `SPU_SN_DEFER` | env | unset | experimental | Behaviour switch; reads a number. Not described where it is read. | `runtime/spu/spu_drain.c` |
| `SPU_STACKEA_WATCH` | env | unset | diagnostic | Watch; reads any value. Not described where it is read. | `runtime/spu/spu_dma.h` |
| `SPU_STEPCAP` | env | unset | diagnostic | A task that never halts (infinite work/wait loop) never dumps its ring. | `runtime/spu/spu_interp.c` |
| `SPU_STEPTRACE` | env | unset | diagnostic | Pc and the argument registers at every trampoline step. | `runtime/spu/spu_drain.c` |
| `SPU_STEPTRACE_FROM` | env | unset | diagnostic | Trace output; reads a value. Not described where it is read. | `runtime/spu/spu_drain.c` |
| `SPU_SYSCALL_IMG` | env | unset | experimental | Every syscall of image n, uncapped (the 24-line budget is spent by FMOD's WAIT_SIGNAL loop long before a later task). | `runtime/spu/spu_channels.c` |
| `SPU_TAGPROBE` | env | unset | diagnostic | Behaviour switch; reads any value. Not described where it is read. | `runtime/spu/spu_dma.h` |
| `SPU_TAILRET` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `runtime/spu/spu_drain.c` |
| `SPU_TRACE` | env | unset | diagnostic | Stderr) or SPU_TRACE_FILE=<path>. | `runtime/spu/spu_channels.c` |
| `SPU_TRACE_AT_WALL` | env | unset | diagnostic | Hold the trace until the PPU's job-barrier probe arms g_barrier_sync_watch, so the budget covers the post-ticket claim/ stage pass instead of boot-time idle polling. | `runtime/spu/spu_channels.c` |
| `SPU_TRACE_FILE` | env | unset | diagnostic | Trace output; reads a path. Not described where it is read. | `runtime/spu/spu_channels.c` |
| `SPU_TRACE_FLOWCTX` | env | unset | diagnostic | Trace ONLY the run the SPURS_PM_FLOW tracer armed (the stalled queue's spu0 dispatch) -- post-wall idle dispatches of the other 13 jobmanager wids otherwise exhaust… | `runtime/spu/spu_channels.c` |
| `SPU_TRACE_IMG` | env | unset | diagnostic | Trace only contexts running image n (e.g. | `runtime/spu/spu_channels.c` |
| `SPU_TRACE_MAX` | env | unset | diagnostic | Trace output; reads a value. Not described where it is read. | `runtime/spu/spu_channels.c` |
| `SPU_VRAMPC` | env | unset | experimental | Which SPU CODE writes PS1 VRAM, bucketed by the symbol it falls in. | `runtime/spu/spu_dma.h` |
| `SPU_WATCHEA` | env | unset | diagnostic | Watch; reads a value. Not described where it is read. | `runtime/spu/spu_dma.h` |
| `SPU_WATCHEA_EVERY` | env | unset | diagnostic | Default 64): print every nth hit. | `runtime/spu/spu_dma.h` |
| `SPU_WATCHLSA` | env | unset | diagnostic | Watch; reads a value. Not described where it is read. | `runtime/spu/spu_dma.h` |
| `SPU_WHOPOLLS` | env | unset | diagnostic | Rchcnt/park accounting PER SPU, printed every n calls with the pc each SPU is sitting at. | `runtime/spu/spu_channels.c` |
| `SPU_WORKER_TRACE` | env | unset | diagnostic | A worker that produces no outbound word leaves the PPU blocked in sys_event_queue_receive forever, and the difference between "parked at its idle poll" and "fell over… | `runtime/spu/spu_lifted_job.h` |
| `SPU_WWS_PROBE` | env | unset | diagnostic | The WWS buffer/code logs. | `runtime/spu/spu_context.h` |
| `YDKJ_CRI_CHAIN` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/spu/spu_channels.c`, `runtime/spu/spu_dma.h` |
| `YDKJ_CRI_EXIT_HALT` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/spu/spu_channels.c` |
| `YDKJ_CRI_GATE1TRACE` | env | unset | diagnostic | Trace output; reads a number. Not described where it is read. | `runtime/spu/spu_interp.c` |
| `YDKJ_CRI_RESUME` | env | unset | experimental | A real SPURS task is PERSISTENT -- on yield (num=0) the kernel re-enters it when work is signaled. | `runtime/spu/spu_workload.c` |
| `YDKJ_CRI_TASKSET` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/spu/spu_workload.c` |
| `YDKJ_CRI_WAKE` | env | unset | experimental | Probe: on cri task (image 22) completion, wake any PPU completion-waiter (the SPU->PPU cellSpursEventFlag completion isn't propagated yet). | `runtime/spu/spu_workload.c` |
| `YDKJ_DMA_IMG` | env | unset | experimental | Trace ONLY image N, uncapped (the 300-cap otherwise fills with the always-running PM's DMA and hides a late image entirely). | `runtime/spu/spu_dma.h` |
| `YDKJ_E60` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/spu/spu_channels.c` |
| `YDKJ_G1_CTX` | env | unset | experimental | Behaviour switch; reads a number. Not described where it is read. | `runtime/spu/spu_workload.c` |
| `YDKJ_G1_LSPAT` | env | unset | experimental | Behaviour switch; reads a number. Not described where it is read. | `runtime/spu/spu_workload.c` |
| `YDKJ_MINIMAL_CTX` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/spu/spu_workload.c` |
| `YDKJ_NO_CRI_CTX` | env | unset | diagnostic | Turns a behaviour off; reads any value. Not described where it is read. | `runtime/spu/spu_workload.c` |
| `YDKJ_NO_CRI_R4` | env | unset | diagnostic | Turns a behaviour off; reads any value. Not described where it is read. | `runtime/spu/spu_channels.c`, `runtime/spu/spu_dma.h` |
| `YDKJ_POLTRACE` | env | unset | diagnostic | Trace output; reads any value. Not described where it is read. | `runtime/spu/spu_channels.c` |
| `YDKJ_SPU_LSDUMP` | env | unset | diagnostic | Else ./recomp_spu_ls.bin. | `runtime/spu/spu_channels.c` |
| `YDKJ_SPU_TRACE` | env | unset | diagnostic | Log the last N PCs into a ring buffer and dump them when the interp halts -- shows the path to a branch-to-0 (the cri task/policy wall). | `runtime/spu/spu_interp.c` |
| `YDKJ_STOP_HALT` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/spu/spu_channels.c` |
| `YDKJ_SYSCALL_ARG` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/spu/spu_channels.c` |

## LV2 syscalls

| Switch | Kind | Default | Class | What it does | Read in |
|---|---|---|---|---|---|
| `EVT_PUSHLOG` | env | unset | diagnostic | Log every push to that queue with the host caller, to find which producer feeds a queue when nothing logs a port_send. | `runtime/syscalls/sys_event.c` |
| `EVT_RECV_KICK` | env | unset | experimental | An infinite wait on that queue that sees nothing for <ms> gets the last event pushed to the queue re-delivered. | `runtime/syscalls/sys_event.c` |
| `FLOW_FSDBG` | env | unset | diagnostic | The lv2 path is what PhyreEngine titles actually use (they do not go through the cellFs HLE), so PS3_FSLOG in ppu_fs.cpp never fires. | `runtime/syscalls/sys_fs.c` |
| `FLOW_TITLEFIX` | env | unset | experimental | The game builds the title path with an EMPTY filename (a stale-lift string-construction bug) -> opens the dir. | `runtime/syscalls/sys_fs.c` |
| `FLOW_TITLEOPEN` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/syscalls/sys_fs.c` |
| `LV2_ERRDBG` | env | unset | diagnostic | Every syscall that returns non-OK, deduped by (number, result). | `runtime/syscalls/lv2_register.c` |
| `MUTEX_WATCH` | env | unset | diagnostic | The wait stays a real blocking lock (the polling of =1 changes the timing enough to hide some hangs), and a reporter thread names every waiter older than 5 s and the… | `runtime/syscalls/sys_mutex.c` |
| `POKE7` | env | unset | diagnostic | One-shot force-post of an arbitrary semaphore, to test whether a starved gate (e.g. | `runtime/syscalls/sys_semaphore.c` |
| `POKESEM` | env | unset | diagnostic | Behaviour switch; reads any value. Not described where it is read. | `runtime/syscalls/sys_semaphore.c` |
| `POLLSITE` | env | unset | experimental | Resolve the host chain of the 1ms poller (LBP bringup) to guest functions -- names the stage that is starving. | `runtime/syscalls/sys_timer.c` |
| `PPU_STACKINFO_LOG` | env | unset | diagnostic | Log output; reads any value. Not described where it is read. | `runtime/syscalls/sys_ppu_thread.c` |
| `PPU_THREADARG` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/syscalls/sys_ppu_thread.c` |
| `PPU_THREADGATE` | env | unset | diagnostic | PS3 priority scheduling — a newly created same/lower-priority thread does NOT run until the creating thread blocks. | `runtime/syscalls/sys_ppu_thread.c` |
| `PS1_LOOPWATCH` | env | unset | diagnostic | Watch; reads a value. Not described where it is read. | `runtime/syscalls/sys_ppu_thread.c` |
| `PS1_PC_CENSUS` | env | unset | diagnostic | Has the R3000 EVER executed in a given range? * The top-N bucket report answers "where is it now". | `runtime/syscalls/sys_ppu_thread.c` |
| `PS1_R3000_PC` | env | unset | experimental | The R3000's LIVE program counter. | `runtime/syscalls/sys_ppu_thread.c` |
| `PS1_SPINWAIT` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/syscalls/sys_ppu_thread.c` |
| `PS3_COND_PEEK` | env | unset | diagnostic | Behaviour switch; reads a value. Not described where it is read. | `runtime/syscalls/sys_cond.c` |
| `PS3_COND_SPURIOUS_MS` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `runtime/syscalls/sys_cond.c` |
| `PS3_EVF_FORCE` | env | unset | diagnostic | The game's init blocks polling event_flag (flag=100 bits=0x2) for a subsystem/SPU completion that our HLE never fires, so boot stalls on a black screen. | `runtime/syscalls/sys_event.c` |
| `PS3_EVF_OK_IF_MISSING` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/syscalls/sys_event.c` |
| `PS3_EVQSTAT` | env | unset | diagnostic | Every n pushes, report each live queue's push count and how many events are sitting in it unread. | `runtime/syscalls/sys_event.c` |
| `PS3_EVT_RECV` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `runtime/syscalls/sys_event.c` |
| `PS3_EVT_RECV_TRACE` | env | unset | diagnostic | Trace output; reads any value. Not described where it is read. | `runtime/syscalls/sys_event.c` |
| `PS3_EVT_SEND_STACK` | env | unset | diagnostic | Behaviour switch; reads any value. Not described where it is read. | `runtime/syscalls/sys_event.c` |
| `PS3_FSTRACE` | env | unset | diagnostic | Every nth read, the fd and the file offset it came from. | `runtime/syscalls/sys_fs.c` |
| `PS3_NO_THREAD_EXIT_UNWIND` | env | unset | diagnostic | Turns a behaviour off; reads a value. Not described where it is read. | `runtime/syscalls/sys_ppu_thread.c` |
| `PS3_OPEN_PSID` | env | unset | feature | Set the OpenPSID outright (32 hex digits). | `runtime/syscalls/lv2_register.c` |
| `PS3_POLLTOP` | env | unset | diagnostic | Name the usleep poll sites. | `runtime/syscalls/sys_timer.c` |
| `PS3_PRIO_START` | env | unset | experimental | Let a newly started high-priority PPU thread run until it blocks. | `runtime/syscalls/sys_ppu_thread.c` |
| `PS3_SDK_VERSION` | env | unset | feature | Override the SDK version the runtime reports to the title. | `runtime/syscalls/lv2_register.c` |
| `PS3_SEMTRACE` | env | unset | diagnostic | Trace output; reads any value. Not described where it is read. | `runtime/syscalls/sys_semaphore.c` |
| `PS3_SYNCLOG` | env | unset | diagnostic | Log output; reads any value. Not described where it is read. | `runtime/syscalls/sys_cond.c` |
| `PS3_USRDIR_BASE` | env | unset | feature | Where a title installed as a full `/dev_hdd0/game/<ID>` tree keeps its USRDIR. | `runtime/syscalls/sys_fs.c` |
| `PS3_WAITBT` | env | unset | diagnostic | One-shot guest-stack dump per (tid,queue) so we can name the exact game function the main thread is stuck polling in the flip loop. | `runtime/syscalls/sys_event.c` |
| `PS3_WAIT_OBJ` | env | unset | experimental | When a usleep spin is reached from this return address, dump the registers and the object they point at. | `runtime/syscalls/sys_timer.c` |
| `RD_SPU_DONE_EVENT` | env | unset | experimental | On SPU thread completion, deliver an event to the connected queue -- PPU code may block in sys_event_queue_receive for the SPU's completion signal (which normally… | `runtime/syscalls/lv2_register.c` |
| `RD_SPU_FRAME_MBOX` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `runtime/syscalls/lv2_register.c` |
| `RD_SPU_INTERP` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/syscalls/lv2_register.c` |
| `RD_SPU_INTERP_ASYNC` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/syscalls/lv2_register.c` |
| `SEMCHAIN` | env | unset | experimental | Dump the guest call-chain at the main thread's sem=7 poll (the "loading done" wait) to locate its loop -- is it meant to re-post sem=3?. | `runtime/syscalls/sys_semaphore.c` |
| `SEMTID` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/syscalls/sys_semaphore.c` |
| `SEM_BADID` | env | unset | experimental | A wait/post on a handle that was never created means some earlier init did not run, and the useful evidence is WHICH object the caller read the handle out of -- not… | `runtime/syscalls/sys_semaphore.c` |
| `SEM_ERRDBG` | env | unset | diagnostic | Report non-OK returns. | `runtime/syscalls/sys_semaphore.c` |
| `SEM_WAITDBG` | env | unset | diagnostic | Debug output; reads any value. Not described where it is read. | `runtime/syscalls/sys_semaphore.c` |
| `SPU_ARGS_DUMP` | env | unset | diagnostic | Dumps; reads any value. Not described where it is read. | `runtime/syscalls/lv2_register.c` |
| `SPU_DUMP_IMPORT` | env | unset | diagnostic | Save each unique imported ELF (FMOD's runtime- materialized SPU overlay plugins) so they can be lifted + registered. | `runtime/syscalls/lv2_register.c` |
| `SPU_HOST_STACK_BIG` | env | unset | diagnostic | Behaviour switch; reads a value. Not described where it is read. | `runtime/syscalls/lv2_register.c` |
| `SPU_IMG_DUMP` | env | unset | diagnostic | Dumps; reads any value. Not described where it is read. | `runtime/syscalls/lv2_register.c` |
| `SPU_LSREAD_TRACE` | env | unset | diagnostic | Trace output; reads any value. Not described where it is read. | `runtime/syscalls/lv2_register.c` |
| `SPU_SEED_DBG` | env | unset | diagnostic | Debug output; reads any value. Not described where it is read. | `runtime/syscalls/lv2_register.c` |
| `SPU_SPEED` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/syscalls/lv2_register.c` |
| `SPU_WORKDESC_DUMP` | env | unset | diagnostic | The work descriptor the PPU hands the worker. | `runtime/syscalls/lv2_register.c` |
| `SPU_WORKDESC_HDR` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/syscalls/lv2_register.c` |
| `WAITBT_EVERY` | env | unset | diagnostic | The same dump, but every nth wait per (tid,queue) instead of once. | `runtime/syscalls/sys_event.c` |
| `YDKJ_ASSERTBT` | env | unset | diagnostic | The libspurs _cellSpursIsLaunchedFromTuner assertion (which aborts the SPURS task subsystem) prints through here. | `runtime/syscalls/lv2_register.c` |
| `YDKJ_CRIBT` | env | unset | experimental | Dump the guest LR chain when a CRI null-pointer / criFs error is printed, to locate the failing call. | `runtime/syscalls/lv2_register.c` |
| `YDKJ_EVT_LENIENT` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `runtime/syscalls/sys_event.c` |
| `YDKJ_FAKECOMPLETE` | env | unset | experimental | The SPURS task-completion events the SPU would post to q=2/q=3 never arrive (SPU video task not running), so the main thread times out after 30s and the title tears down. | `runtime/syscalls/sys_event.c` |
| `YDKJ_GUARD_INST` | env | unset | experimental | Extra checks; reads any value. Not described where it is read. | `runtime/syscalls/lv2_register.c` |
| `YDKJ_HLE_DRAW` | env | unset | experimental | Also unblock the AsyncLoad q=1 wait (timeout=0, blocks forever) so the loader thread proceeds. | `runtime/syscalls/sys_event.c` |
| `YDKJ_INSTDUMP` | env | unset | diagnostic | Dumps; reads any value. Not described where it is read. | `runtime/syscalls/lv2_register.c` |
| `YDKJ_KEEPGROUP` | env | unset | experimental | Libsre rolls back the SPURS kernel group during cellSpursInitialize (the handler asserts the SPU side is dead). | `runtime/syscalls/lv2_register.c` |
| `YDKJ_NOHDLR` | env | unset | experimental | Suppress libsre's SPURS handler threads (entry in the libsre image range) -- they assert that the SPU side isn't operational and crash. | `runtime/syscalls/sys_ppu_thread.c` |
| `YDKJ_NO_PORTROUTE` | env | unset | diagnostic | Turns a behaviour off; reads any value. Not described where it is read. | `runtime/syscalls/sys_event.c` |
| `YDKJ_Q23STACK` | env | unset | diagnostic | Behaviour switch; reads any value. Not described where it is read. | `runtime/syscalls/sys_event.c` |
| `YDKJ_RDY_D2` | env | unset | experimental | Overrides data2 for quick format experiments. | `runtime/syscalls/sys_event.c` |
| `YDKJ_SPUREADY` | env | unset | experimental | The lifted libsre cellSpursInitialize busy-polls the SPU-thread descriptor field at img_ea+0x38 (e.g. | `runtime/syscalls/lv2_register.c` |
| `YDKJ_SPURSKERNEL` | env | unset | experimental | So group_start runs a live SPU instead of an instant no-op. | `runtime/syscalls/lv2_register.c` |
| `YDKJ_SPURS_READY` | env | unset | experimental | Break the SPURS bring-up deadlock from the PPU side. | `runtime/syscalls/sys_event.c` |
| `YDKJ_SPUTASK` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `runtime/syscalls/sys_event.c` |
| `YDKJ_SYNCLOG` | env | unset | diagnostic | Log output; reads any value. Not described where it is read. | `runtime/syscalls/sys_cond.c` |

## RSX and graphics

| Switch | Kind | Default | Class | What it does | Read in |
|---|---|---|---|---|---|
| `A2O_DBG` | env | unset | diagnostic | The EA->offset conversions a title asks for. | `libs/video/cellGcmSys.c` |
| `A2O_FAILDBG` | env | unset | diagnostic | Debug output; reads any value. Not described where it is read. | `libs/video/cellGcmSys.c` |
| `BLENDDBG` | env | unset | diagnostic | Debug output; reads any value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `CELLMARK_BLINKDBG` | env | unset | diagnostic | Debug output; reads any value. Not described where it is read. | `libs/video/cellGcmSys.c`, `libs/video/rsx_d3d12_backend.c` |
| `CELLMARK_DUMP` | env | unset | diagnostic | Dumps; reads a value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `CELLMARK_DUMP_DIR` | env | unset | diagnostic | Dumps; reads a path. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `CELLMARK_DUMP_EVERY` | env | unset | diagnostic | After each dump, skip N-1 frames -- samples the whole run instead of one contiguous window, so a short burst of real content can't fall between the dumped frames. | `libs/video/rsx_d3d12_backend.c` |
| `CELLMARK_DUMP_MINDRAWS` | env | unset | diagnostic | Only consider frames carrying at least N draw records. | `libs/video/rsx_d3d12_backend.c` |
| `CELLMARK_DUMP_SKIP` | env | unset | diagnostic | Presents to ignore first. | `libs/video/rsx_d3d12_backend.c` |
| `CELLMARK_IGPU` | env | unset | diagnostic | Behaviour switch; reads a value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `CLEAR_RGB` | env | unset | experimental | Override the guest clear colour. | `libs/video/rsx_d3d12_backend.c` |
| `CMASK_FORCE` | env | unset | diagnostic | Ignore the guest colour mask (write RGBA on every draw). | `libs/video/rsx_d3d12_backend.c` |
| `CUBEDBG` | env | unset | diagnostic | Debug output; reads any value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `CUBEKEYDBG` | env | unset | diagnostic | Which (program, cube mask) pairs are being compiled. | `libs/video/rsx_d3d12_backend.c` |
| `CULL_FLIP` | env | unset | experimental | Invert the front-face convention. | `libs/video/rsx_d3d12_backend.c` |
| `CULL_OFF` | env | unset | diagnostic | 1 forces double-sided, the old unconditional behaviour -- geometry that vanishes under culling is a winding/front-face problem, not a missing draw. | `libs/video/rsx_d3d12_backend.c` |
| `D3D12_DBG` | env | unset | diagnostic | Debug output; reads a value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `D3D12_IQ` | env | unset | experimental | Dump exactly what the GPU is about to see -- the MVP root constants and the first uploaded host vertices. | `libs/video/rsx_d3d12_backend.c` |
| `D3D12_NO_DBG` | env | unset | diagnostic | D3D12: turn the debug layer off in debug builds. | `libs/video/rsx_d3d12_backend.c` |
| `D3D12_VSYNC` | env | on | feature | D3D12: `0` presents without waiting for vertical sync. | `libs/video/rsx_d3d12_backend.c` |
| `DBG_CENTER` | env | unset | diagnostic | A debug camera applied in CLIP space, not a framebuffer crop -- the geometry is re-rasterized at full resolution. | `libs/video/rsx_d3d12_backend.c` |
| `DBG_LOCK` | env | unset | diagnostic | Running clip-space centroid of the tracked mesh (RSX_DBG_VTX's texture), so the debug camera can follow it. | `libs/video/rsx_d3d12_backend.c` |
| `DBG_ZOOM` | env | unset | diagnostic | Debug output; reads a value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `DEPTH_OFF` | env | unset | diagnostic | Drop the depth test for guest-FP draws, so submission order alone decides what is on top. | `libs/video/rsx_d3d12_backend.c` |
| `DRAWARGS` | env | unset | experimental | The arguments actually handed to the GPU, plus the target they are aimed at. | `libs/video/rsx_d3d12_backend.c` |
| `DRAW_KEEP_BLEND` | env | unset | experimental | Further restrict DRAW_KEEP_TEX to draws with that blend flag. | `libs/video/rsx_d3d12_backend.c` |
| `DRAW_KEEP_NOCLEAR` | env | unset | experimental | Also drop the clears. | `libs/video/rsx_d3d12_backend.c` |
| `DRAW_KEEP_TEX` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `DRAW_LAST_TEX` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `DRAW_LIMIT` | env | unset | diagnostic | Keep only the first N recorded ops this frame. | `libs/video/rsx_d3d12_backend.c` |
| `DRAW_SKIP_TEX` | env | unset | diagnostic | Turns a behaviour off; reads a value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `EMPTYTEX` | env | unset | experimental | Which geometry binds a texture whose source is all zero. | `libs/video/rsx_d3d12_backend.c` |
| `FLIP_DBG` | env | unset | diagnostic | Debug output; reads any value. Not described where it is read. | `libs/video/cellGcmSys.c` |
| `FPK_DBG` | env | unset | diagnostic | The fragment constants as the shader will see them. | `libs/video/rsx_d3d12_backend.c` |
| `FP_CONSTBUF` | env | on | experimental | D3D12: pass fragment-program constants in a constant buffer; `0` inlines them. | `libs/video/rsx_d3d12_backend.c` |
| `FP_DUMP` | env | unset | diagnostic | The first n compiled programs (1 = the old default of 4). | `libs/video/rsx_d3d12_backend.c` |
| `FP_FORCE` | env | unset | diagnostic | Replace the translated body with solid magenta -- isolates geometry/transform problems from texture/blend problems. | `libs/video/rsx_d3d12_backend.c` |
| `FP_IDCOLOR` | env | unset | diagnostic | Give every fragment program a distinct flat colour derived from its address. | `libs/video/rsx_d3d12_backend.c` |
| `FP_KDBG` | env | unset | diagnostic | Debug output; reads a number. Not described where it is read. | `libs/video/rsx_fp_decompiler.c` |
| `FP_KILL` | env | unset | diagnostic | <hex fp_addr>: make that program discard every fragment, so what it covers becomes visible. | `libs/video/rsx_d3d12_backend.c` |
| `FP_LIST` | env | unset | diagnostic | Every program the title actually compiles, with its size. | `libs/video/rsx_d3d12_backend.c` |
| `FP_OFF` | env | unset | diagnostic | Turns a behaviour off; reads any value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `FP_ONE` | env | unset | diagnostic | <hex fp_addr>: force that program's colour output to all-ones (e.g. | `libs/video/rsx_d3d12_backend.c` |
| `FP_PICK` | env | unset | diagnostic | Behaviour switch; reads a value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `FP_SHOW` | env | unset | diagnostic | Behaviour switch; reads a value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `FP_TEX` | env | unset | experimental | Make every program output its unit-0 texture sample verbatim. | `libs/video/rsx_d3d12_backend.c` |
| `FP_TEX_FP` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `FP_TEX_UV` | env | unset | experimental | Sample at a fixed centre UV instead of the interpolated texcoord. | `libs/video/rsx_d3d12_backend.c` |
| `FRAME_BUDGET` | env | unset | experimental | Geometry a frame asked for vs what the batch can hold. | `libs/video/rsx_d3d12_backend.c` |
| `GCM2D_DBG` | env | unset | diagnostic | Debug output; reads a number. Not described where it is read. | `libs/video/cellGcmSys.c` |
| `GCM2D_TRACE` | env | unset | diagnostic | Histogram of (subchannel, method) actually seen. | `libs/video/cellGcmSys.c` |
| `GCM_CIRCLE_DUMP` | env | unset | diagnostic | Dumps; reads any value. Not described where it is read. | `libs/video/cellGcmSys.c` |
| `GCM_CTXDBG` | env | unset | diagnostic | Which context the title is actually driving. | `libs/video/cellGcmSys.c` |
| `GCM_DEFAULT_CTX_REPOINT` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `libs/video/cellGcmSys.c` |
| `GCM_DRAINDBG` | env | unset | diagnostic | Log one drain pass in GCM_DRAINDBG_EVERY (default 60). | `libs/video/cellGcmSys.c` |
| `GCM_DRAINDBG_EVERY` | env | unset | diagnostic | Default 60). | `libs/video/cellGcmSys.c` |
| `GCM_FIFO_CIRCLE_PASSES` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `libs/video/cellGcmSys.c` |
| `GCM_FIFO_KICK` | env | on | feature | Wake the FIFO consumer as soon as the guest moves PUT; `0` waits for its timed poll (old behaviour). | `libs/video/cellGcmSys.c` |
| `GCM_FIFO_SEGMENT` | env | unset | experimental | Hand the title its command ring a segment at a time, as libgcm does (RPCS3's _cellGcmInitBody uses 32 KB). | `libs/video/cellGcmSys.c` |
| `GCM_FIFO_SNAP` | env | unset | diagnostic | Dump N snapshots of the raw words at both ends of the ring -- what `get` is about to decode, and what the title just wrote at `put`. | `libs/video/cellGcmSys.c` |
| `GCM_FLIPCB_ONTICK` | env | unset | experimental | Fire the guest flip handler only when the flip completes (cellGcmTickFlip), not also when it is requested. | `libs/video/cellGcmSys.c` |
| `GCM_FLIPCOUNT` | env | unset | diagnostic | Every flip, with a timestamp. | `libs/video/cellGcmSys.c` |
| `GCM_FLIP_BT` | env | unset | diagnostic | Dump the flipping thread's guest stack for the flips around n. | `libs/video/cellGcmSys.c` |
| `GCM_FLIP_NEEDS_FIFO` | env | unset | experimental | Only mark a flip pending once the walker has actually consumed up to `put`. | `libs/video/cellGcmSys.c` |
| `GCM_GET_EQ_PUT` | env | unset | experimental | See where get is published at the end of a pass. | `libs/video/cellGcmSys.c` |
| `GCM_GUEST_FB` | env | unset | experimental | Present the guest display buffer. | `libs/video/rsx_d3d12_backend.c` |
| `GCM_HANDLER_THREAD` | env | unset | experimental | Deliver GCM flip/vblank handlers from a dedicated thread. | `libs/video/cellGcmSys.c` |
| `GCM_METHOD_HIST` | env | unset | diagnostic | Every <secs>, the most frequent (subchannel, method) pairs dispatched since the last report -- what a frame is actually made of when nothing draws. | `libs/video/cellGcmSys.c` |
| `GCM_OBJDBG` | env | unset | diagnostic | Log every SET_OBJECT (method 0). | `libs/video/cellGcmSys.c` |
| `GCM_PARK_GRACE_MS` | env | built-in | experimental | How long the FIFO consumer keeps spinning before it parks. | `libs/video/cellGcmSys.c` |
| `GCM_PUMP_DBG` | env | unset | diagnostic | How often each guest handler is actually delivered. | `libs/video/cellGcmSys.c` |
| `GCM_RATE` | env | unset | experimental | Profiling; reads any value. Not described where it is read. | `libs/video/cellGcmSys.c` |
| `GCM_RECDBG` | env | unset | diagnostic | The words the walker stopped on are the one thing a run that wedged once in three can still tell us, and GCM_RECDBG is far too loud to leave on for a 15-minute run. | `libs/video/cellGcmSys.c` |
| `GCM_REFLOG` | env | unset | diagnostic | Log output; reads any value. Not described where it is read. | `libs/video/cellGcmSys.c` |
| `GCM_REFPUB` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `libs/video/cellGcmSys.c` |
| `GCM_REPORT_SPU_HOLD_MS` | env | unset | diagnostic | Off by default): a GET_REPORT waits, up to <ms>, while SPU tasks that work in signal-driven rounds are in one (spu_taskset_round_busy). | `libs/video/cellGcmSys.c` |
| `GCM_SCANREF` | env | unset | diagnostic | One-shot): list every NV406E_SET_REFERENCE actually present in 0..put. | `libs/video/cellGcmSys.c` |
| `GCM_SEMA_ACQUIRE` | env | unset | experimental | Behaviour switch; reads a number. Not described where it is read. | `libs/video/cellGcmSys.c` |
| `GCM_SEMA_ACQUIRE_MS` | env | unset | experimental | A bounded wait. The hardware waits for ever. | `libs/video/cellGcmSys.c` |
| `GCM_SUBCH1_2D` | env | unset | experimental | Behaviour switch; reads a number. Not described where it is read. | `libs/video/cellGcmSys.c` |
| `GCM_SUBCH7` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `libs/video/cellGcmSys.c` |
| `GCM_SUBCH_GATE` | env | unset | diagnostic | While <file> exists, log every method on a non-zero subchannel -- what each binding slot actually carries. | `libs/video/cellGcmSys.c` |
| `GCM_VAL_WATCH` | env | unset | diagnostic | Every FIFO word carrying that value, any method. | `libs/video/cellGcmSys.c` |
| `GCM_WORD_STATS` | env | unset | diagnostic | NOP words vs headers vs jumps/calls/returns walked -- a frame that is mostly NOPs is reserved space someone (an SPU job) was supposed to fill. | `libs/video/cellGcmSys.c` |
| `HASHDBG` | env | unset | diagnostic | How many DISTINCT ucode hashes each program produces. | `libs/video/rsx_d3d12_backend.c` |
| `IDXDBG` | env | unset | diagnostic | Debug output; reads a value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `IDX_DBG` | env | unset | diagnostic | The first indices and the position they resolve to. | `libs/video/rsx_d3d12_backend.c` |
| `LD_ALIAS_DBG` | env | unset | diagnostic | Debug output; reads a value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `LD_BLIT_DUMP` | env | unset | diagnostic | While the CSV gate is open, read back the first four blits' destination surfaces right after the copy (one frame's worth). | `libs/video/rsx_live_draw.c` |
| `LD_CLEAR_TEST` | env | unset | diagnostic | Clear to magenta instead of the guest's colour. | `libs/video/rsx_live_draw.c` |
| `LD_DRAW_DUMP` | env | unset | diagnostic | Dumps; reads a value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `LD_DRAW_DUMP_EVERY` | env | unset | diagnostic | Dumps; reads a number. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `LD_FBDBG` | env | unset | diagnostic | Debug output; reads any value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `LD_FORCE_A1_OPAQUE` | env | unset | diagnostic | Experiment, not a fix. | `libs/video/rsx_live_draw.c` |
| `LD_FORCE_COL0` | env | unset | diagnostic | Every vertex program outputs white COLOR0 -- separates "the VS computes a wrong colour" from everything downstream of it. | `libs/video/rsx_live_draw.c` |
| `LD_FORCE_GREEN` | env | unset | diagnostic | Make every fragment program return solid green. | `libs/video/rsx_live_draw.c` |
| `LD_FORCE_VA` | env | unset | diagnostic | Debug -- inline draws whose UB4 vertex colour (attr 3) has alpha 0 everywhere get alpha 255, to see what a faded-out element is. | `libs/video/rsx_live_draw.c` |
| `LD_FRAME_DUMP` | env | unset | diagnostic | Dumps; reads a value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `LD_FRAME_DUMP_EVERY` | env | unset | diagnostic | Dumps; reads a value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `LD_FRAME_DUMP_FROM` | env | unset | diagnostic | Start dumping there (dense captures of a late scene, e.g. | `libs/video/rsx_live_draw.c` |
| `LD_HLSL_DUMP` | env | unset | diagnostic | Files named by the PSO key the draw CSV prints. | `libs/video/rsx_live_draw.c` |
| `LD_NO_DEPTH` | env | unset | diagnostic | Disable the depth test on every draw. | `libs/video/rsx_live_draw.c` |
| `LD_PLANE_DUMP` | env | unset | diagnostic | Write the first few big single-channel (B8) planes straight out as PGM, exactly as the guest laid them down. | `libs/video/rsx_live_draw.c` |
| `LD_PRESENT_DBG` | env | unset | diagnostic | Is this surface actually finished? A surface whose last clear is NEWER than its last draw has been wiped and not redrawn -- that presents as a black frame. | `libs/video/rsx_live_draw.c` |
| `LD_PS1_BUF0` | env | unset | experimental | DIAGNOSTIC, not a fix. | `libs/video/rsx_live_draw.c` |
| `LD_PSO_DBG` | env | unset | diagnostic | Name the first bail-out of each kind in get_pso(). | `libs/video/rsx_live_draw.c` |
| `LD_RECORD` | env | unset | experimental | Boot_main's --record): every presented frame, as PPM, into an ffmpeg pipe -- ffmpeg's image2pipe reads concatenated PPMs as a stream. | `libs/video/rsx_live_draw.c` |
| `LD_SKIP_PSO` | env | unset | diagnostic | Turns a behaviour off; reads a value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `LD_SKIP_SAMPLING` | env | unset | diagnostic | Turns a behaviour off; reads a number. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `LD_SURF_DUMP` | env | unset | diagnostic | Dump EVERY live surface once, with its non-black count, at LD_SURF_DUMP_FRAME (default 1200). | `libs/video/rsx_live_draw.c` |
| `LD_SURF_DUMP_FRAME` | env | unset | diagnostic | Default 1200). * LD_FRAME_DUMP only reads back the presented surface, which answers "is the screen black" (it is: nonblack=0 on every frame) but not WHERE the pixels… | `libs/video/rsx_live_draw.c` |
| `LD_SURF_DUMP_GATE` | env | unset | diagnostic | Fire when <file> appears instead. | `libs/video/rsx_live_draw.c` |
| `LD_SURF_DUMP_ON_VRAM` | env | unset | diagnostic | Dumps; reads any value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `LD_SWIN` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `LD_TEXRGBA_DUMP` | env | unset | diagnostic | The DECODED level-0 RGBA, with a non-black count and the remap that produced it. | `libs/video/rsx_live_draw.c` |
| `LD_TEXSRC_DBG` | env | unset | diagnostic | Is the guest data there AT ALL? * "Draws execute, forced green fills the screen, the sampled texture is black" keeps arriving as the same question, and the… | `libs/video/rsx_live_draw.c` |
| `LD_TEX_RACE` | env | unset | experimental | Hash the SAME source twice, back to back. | `libs/video/rsx_live_draw.c` |
| `LD_TRACE_PSO` | env | unset | diagnostic | Trace output; reads a value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `LD_TRACE_PSO_FROM` | env | unset | diagnostic | The first three draws at or after that frame -- a PSO's first uses can predate the data it later draws. | `libs/video/rsx_live_draw.c` |
| `LD_UNITS` | env | unset | experimental | Every enabled texture unit of every draw that samples PS1 VRAM, once per distinct combination. | `libs/video/rsx_live_draw.c` |
| `LD_UVDBG` | env | unset | diagnostic | The texture coordinates of the draw that samples the PS1 framebuffer. | `libs/video/rsx_live_draw.c` |
| `LOAD_DBG` | env | unset | diagnostic | Debug output; reads any value. Not described where it is read. | `libs/video/rsx_commands.c` |
| `MEMPEEK` | env | unset | diagnostic | <hex addr>:<n>: n big-endian floats at a guest address each frame, with a min/max so a buffer of constants is distinguishable from real data. | `libs/video/rsx_d3d12_backend.c` |
| `MOVIE_BIND` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `MOVIE_FIND` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `MVPDBG` | env | unset | diagnostic | Debug output; reads any value. Not described where it is read. | `libs/video/rsx_commands.c` |
| `MVP_BASE` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `NO_ALPHATEST` | env | unset | diagnostic | Turns a behaviour off; reads a value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `NV3089_DBG` | env | unset | diagnostic | Debug output; reads a number. Not described where it is read. | `libs/video/cellGcmSys.c` |
| `NV3089_GATE` | env | unset | diagnostic | Log every blit's geometry while <file> exists, so a capture covers the scene of interest instead of the first N since boot. | `libs/video/cellGcmSys.c` |
| `NV3089_ROWCOPY` | env | unset | experimental | Default 1): an unscaled blit copies a row at a time. | `libs/video/cellGcmSys.c` |
| `NV309E_BLIT` | env | unset | experimental | Behaviour switch; reads a number. Not described where it is read. | `libs/video/cellGcmSys.c` |
| `PERF` | env | unset | diagnostic | Where a frame's CPU time actually goes. | `libs/video/rsx_d3d12_backend.c` |
| `PPU_SAMPLE_FRAMES` | env | unset | diagnostic | Profiling; reads a value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `PPU_SAMPLE_PRESENT` | env | unset | diagnostic | Profiling; reads any value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `PPU_SAMPLE_RUN` | env | unset | diagnostic | Profiling; reads any value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `PPU_SAMPLE_START` | env | unset | diagnostic | Profiling; reads a value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `PS1_EVENTS` | env | unset | experimental | The three kernel events the BIOS polls. | `libs/video/rsx_live_draw.c` |
| `PS1_FBDUMP` | env | unset | diagnostic | Write PS1 VRAM out as a PPM. | `libs/video/rsx_live_draw.c` |
| `PS1_FBDUMP_MIN` | env | unset | diagnostic | Dumps; reads a value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `PS1_GP0HIST` | env | unset | diagnostic | Which GP0 PRIMITIVES the ring actually carries, as a histogram over the last 64 packets. | `libs/video/rsx_live_draw.c` |
| `PS1_PC` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `PS1_RINGDUMP` | env | unset | diagnostic | The last few GP0 command packets. | `libs/video/rsx_live_draw.c` |
| `PS1_VRAM_READY` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `PS3RECOMP_ENG_BORDER` | env | on | feature | Draw engine: honour the texture border colour; `0` samples transparent black outside the texture (old behaviour). | `libs/video/rsx_draw_engine.c` |
| `PS3RECOMP_ENG_DEPTH_BOUNDS` | env | on | feature | Draw engine: apply the depth-bounds test; `0` turns it off. | `libs/video/rsx_draw_engine.c` |
| `PS3RECOMP_ENG_DEPTH_CLAMP` | env | on | feature | Draw engine: honour depth clamp; `0` turns it off. | `libs/video/rsx_draw_engine.c` |
| `PS3RECOMP_ENG_DEPTH_VIA_GUEST` | env | unset | experimental | Draw engine: route depth-texture reads through guest memory. | `libs/video/rsx_draw_engine.c` |
| `PS3RECOMP_ENG_POLY_OFFSET` | env | on | feature | Draw engine: apply polygon offset; `0` turns it off. | `libs/video/rsx_draw_engine.c` |
| `PS3RECOMP_ENG_RECT_CLEAR` | env | on | feature | Draw engine: clear only the scissored rectangle; `0` clears whole targets (old behaviour). | `libs/video/rsx_draw_engine.c` |
| `PS3RECOMP_ENG_TEXHASH_FULL` | env | unset | diagnostic | Draw engine: always hash whole textures when checking for changes. | `libs/video/rsx_draw_engine.c` |
| `PS3RECOMP_ENG_USER_CLIP` | env | on | feature | Draw engine: apply user clip planes; `0` ignores the register (old behaviour). | `libs/video/rsx_draw_engine.c` |
| `PS3RECOMP_FULLSCREEN` | env | unset | feature | Start the game window full screen (Metal and Vulkan). | `libs/video/rsx_metal_backend.m`, `libs/video/rsx_vulkan_backend.c` |
| `PS3RECOMP_HOST_SURFACES` | env | unset | experimental | Metal: the launcher's bootstrap service name; when set, the backend renders into surfaces the launcher provides. | `libs/video/rsx_metal_backend.m` |
| `PS3RECOMP_METAL_FIXED_FUNCTION` | env | unset | diagnostic | Metal: draw everything with the built-in fixed-function shader instead of translated guest programs. | `libs/video/rsx_metal_backend.m` |
| `PS3RECOMP_METAL_FPS` | env | unset | diagnostic | Profiling; reads a value. Not described where it is read. | `libs/video/rsx_metal_backend.m` |
| `PS3RECOMP_METAL_FRAME_DUMP` | env | unset | diagnostic | Dumps; reads any value. Not described where it is read. | `libs/video/rsx_draw_engine.c`, `libs/video/rsx_metal_backend.m` |
| `PS3RECOMP_METAL_HEADLESS` | env | unset | feature | Metal: render without a window. | `libs/video/rsx_metal_backend.m` |
| `PS3RECOMP_METAL_MRT_MASK` | env | `1` | feature | Metal: per-target colour-mask rule for multiple render targets (`1` hardware rule, `0` old behaviour, `2` per-target blend). | `libs/video/rsx_metal_backend.m` |
| `PS3RECOMP_METAL_SHADER_DUMP` | env | unset | diagnostic | Write every translated program's HLSL and MSL there, named by the cache key. | `libs/video/rsx_metal_backend.m` |
| `PS3RECOMP_METAL_WINDOW` | env | unset | feature | Metal: initial window size as `WxH`. | `libs/video/rsx_metal_backend.m` |
| `PS3RECOMP_MSL_CACHE` | env | unset | feature | Directory for the translated Metal shader cache; unset keeps no disk cache. | `libs/video/rsx_shader_msl.cpp` |
| `PS3RECOMP_MSL_FAST` | env | unset | experimental | Keep spirv-cross's fast math in translated Metal shaders. | `libs/video/rsx_shader_msl.cpp` |
| `PS3RECOMP_MSL_INVARIANT` | env | on | feature | Mark translated Metal vertex positions `[[invariant]]`; `0` turns it off. | `libs/video/rsx_shader_msl.cpp` |
| `PS3RECOMP_RSX_ENGINE` | env | unset | experimental | `dispatch` routes draws through the register-file draw engine; any other value turns it off. | `libs/video/rsx_draw_engine.c` |
| `PS3RECOMP_SPIRV_CACHE` | env | unset | feature | Directory for the translated SPIR-V shader cache; unset keeps no disk cache. | `libs/video/rsx_shader_spirv.cpp` |
| `PS3RECOMP_VK_DEVICE` | env | unset | feature | Vulkan: use physical device *n* instead of the first non-CPU one. | `libs/video/rsx_vulkan_backend.c` |
| `PS3RECOMP_VK_DUMP` | env | unset | diagnostic | Dumps; reads a value. Not described where it is read. | `libs/video/rsx_vulkan_backend.c` |
| `PS3RECOMP_VK_DUMP_EVERY` | env | unset | diagnostic | Dumps; reads a value. Not described where it is read. | `libs/video/rsx_vulkan_backend.c` |
| `PS3RECOMP_VK_FULLSCREEN` | env | unset | feature | Vulkan: start the window full screen (as `PS3RECOMP_FULLSCREEN`). | `libs/video/rsx_vulkan_backend.c` |
| `PS3RECOMP_VK_GUEST_PROGRAMS` | env | unset | experimental | Vulkan: draw with translated guest shader programs when a SPIR-V compiler is available. | `libs/video/rsx_vulkan_backend.c` |
| `PS3RECOMP_VK_HOLD` | env | unset | diagnostic | Vulkan: keep the window open this many seconds after the last frame. | `libs/video/rsx_vulkan_backend.c` |
| `PS3RECOMP_VK_MRT_MASK` | env | `1` | feature | Vulkan: per-target colour-mask rule for multiple render targets (as the Metal switch). | `libs/video/rsx_vulkan_backend.c` |
| `PS3RECOMP_VK_WINDOW` | env | unset | feature | Vulkan: open an SDL window and present to it. | `libs/video/rsx_vulkan_backend.c` |
| `PS3RECOMP_WINDOW_STATE` | env | unset | diagnostic | Counters; reads a value. Not described where it is read. | `libs/video/rsx_metal_backend.m` |
| `PS3_AA` | env | unset | feature | Metal engine anti-aliasing (`smaa`, `fxaa`). | `libs/video/rsx_metal_backend.m` |
| `PS3_ANISO` | env | unset | feature | Engine anisotropic filtering level. | `libs/video/rsx_metal_backend.m` |
| `PS3_CONTRAST` | env | unset | feature | Engine contrast adjustment. | `libs/video/rsx_metal_backend.m` |
| `PS3_SHARPEN` | env | unset | feature | Engine sharpening pass strength. | `libs/video/rsx_metal_backend.m` |
| `PS3_VIBRANCE` | env | unset | feature | Engine vibrance adjustment. | `libs/video/rsx_metal_backend.m` |
| `PSOMISSDBG` | env | unset | diagnostic | On a miss, name the key field that differs from an existing entry for the same program. | `libs/video/rsx_d3d12_backend.c` |
| `REMAPDBG` | env | unset | diagnostic | Debug output; reads any value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `RENDERDOC_DIR` | env | unset | diagnostic | D3D12: directory for RenderDoc captures. | `libs/video/rsx_d3d12_backend.c` |
| `RENDERDOC_DLL` | env | unset | diagnostic | D3D12: path of the RenderDoc library to load for captures. | `libs/video/rsx_d3d12_backend.c` |
| `RENDERDOC_MAX` | env | unset | diagnostic | D3D12: the most RenderDoc captures to take. | `libs/video/rsx_d3d12_backend.c` |
| `RSX_A010_CAPTURE_ONCE` | env | unset | diagnostic | A limit; reads any value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `RSX_A010_EAGER_DUMP` | env | unset | diagnostic | Dumps; reads a value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `RSX_A010_GEOM` | env | unset | experimental | Trace one stable orphanage mesh. | `libs/video/rsx_live_draw.c` |
| `RSX_A010_PROBE` | env | unset | diagnostic | Behaviour switch; reads any value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `RSX_A010_SURFACE_DUMP` | env | unset | diagnostic | Dumps; reads any value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `RSX_A010_SURFACE_EVERY` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `RSX_ACCUM_FRAME` | env | unset | experimental | Never present at a clear boundary, so every draw in a guest frame accumulates into one image. | `libs/video/rsx_d3d12_backend.c` |
| `RSX_CAP` | env | unset | diagnostic | Stride]] frame_counts. | `libs/video/rsx_d3d12_backend.c` |
| `RSX_CAP_DIR` | env | unset | diagnostic | A limit; reads a path. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `RSX_CONST_LOG` | env | unset | diagnostic | Every transform-constant upload into that slot range (first 400) -- what a shader's constants really were. | `libs/video/rsx_dispatch.c` |
| `RSX_CUBE_TEX` | env | unset | experimental | Behaviour switch; reads a number. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `RSX_D3D12_WARP` | env | auto | feature | D3D12: `1` always uses the WARP software adapter, `0` never does; unset falls back to WARP only without a hardware adapter. | `libs/video/rsx_d3d12_device.h` |
| `RSX_D3D_DEBUG` | env | unset | diagnostic | Debug output; reads any value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `RSX_DBG_CB` | env | unset | diagnostic | Debug output; reads a value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `RSX_DBG_PICK` | env | unset | diagnostic | Track ONLY the draw with that starting index. | `libs/video/rsx_d3d12_backend.c` |
| `RSX_DBG_RECENTER` | env | unset | diagnostic | Recentre EVERY chunk on its own centroid, overlaying them. | `libs/video/rsx_d3d12_backend.c` |
| `RSX_DBG_SCALE` | env | unset | diagnostic | Multiply attribute-0 positions for the draws binding RSX_DBG_VTX's texture. | `libs/video/rsx_d3d12_backend.c` |
| `RSX_DBG_SHIFT` | env | unset | diagnostic | Nudge after scaling. VP_BYPASS applies the guest's posoffset, so a recentred mesh does not land at screen centre -- without this the magnified geometry sits clipped… | `libs/video/rsx_d3d12_backend.c` |
| `RSX_DBG_TRACK` | env | unset | diagnostic | Debug output; reads a number. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `RSX_DBG_VP` | env | unset | diagnostic | Debug output; reads a value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `RSX_DBG_VTX` | env | unset | diagnostic | Debug output; reads a value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `RSX_DRAW_CSV` | env | unset | diagnostic | Uncapped per-draw fingerprints for direct comparison with the working RPCS3 .rxs replay. | `libs/video/rsx_live_draw.c` |
| `RSX_DRAW_CSV_A010_ONLY` | env | unset | diagnostic | Behaviour switch; reads any value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `RSX_DRAW_CSV_FROM` | env | unset | diagnostic | Skip rows before that frame. | `libs/video/rsx_live_draw.c` |
| `RSX_DRAW_CSV_GATE` | env | unset | diagnostic | Rows only while <file> exists (polled every 512 draws), so a capture can be switched on when the scene of interest is on screen instead of guessing its frame number. | `libs/video/rsx_live_draw.c` |
| `RSX_DUMP` | env | unset | diagnostic | Dumps; reads a value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `RSX_DUMP_EVERY` | env | unset | diagnostic | Also dump every Nth frame, not just the first 8 -- a title whose content starts after the boot clears is invisible otherwise. | `libs/video/rsx_live_draw.c` |
| `RSX_ENGINE_COHERENCE_LOG` | env | unset | diagnostic | Log the first n transfers the engine syncs, with how many non-zero pixels moved -- the question when a composite comes out black is whether the GPU surface had… | `libs/video/rsx_draw_engine.c` |
| `RSX_ENGINE_PASS_DUMP` | env | unset | diagnostic | Dumps; reads a value. Not described where it is read. | `libs/video/rsx_draw_engine.c` |
| `RSX_ENGINE_PASS_DUMP_AFTER` | env | unset | diagnostic | Dumps; reads a value. Not described where it is read. | `libs/video/rsx_draw_engine.c` |
| `RSX_ENGINE_PASS_DUMP_DRAWS` | env | unset | diagnostic | One line a draw. | `libs/video/rsx_draw_engine.c` |
| `RSX_ENGINE_PASS_DUMP_MAX` | env | unset | diagnostic | Dumps; reads a value. Not described where it is read. | `libs/video/rsx_draw_engine.c` |
| `RSX_ENGINE_PASS_DUMP_MINDRAWS` | env | unset | diagnostic | Dumps; reads a value. Not described where it is read. | `libs/video/rsx_draw_engine.c` |
| `RSX_ENGINE_REGDUMP_FP` | env | unset | diagnostic | Dumps; reads a value. Not described where it is read. | `libs/video/rsx_draw_engine.c` |
| `RSX_ENGINE_SHADER_DUMP` | env | unset | diagnostic | Dumps; reads a value. Not described where it is read. | `libs/video/rsx_draw_engine.c` |
| `RSX_ENGINE_SKIP_FP` | env | unset | diagnostic | Turns a behaviour off; reads a value. Not described where it is read. | `libs/video/rsx_draw_engine.c` |
| `RSX_FP_CONSTANT_MODE` | env | unset | experimental | Live D3D12: how fragment-program constants reach shaders (buffered or literal). | `libs/video/rsx_live_draw.c` |
| `RSX_FP_CTRL_AUTO` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c`, `libs/video/rsx_live_draw.c` |
| `RSX_FP_DEBUG` | env | unset | diagnostic | Debug output; reads a value. Not described where it is read. | `libs/video/rsx_fp_decompiler.c` |
| `RSX_LIVE_DRAW` | env | unset | experimental | Windows: use the alternative live D3D12 draw engine instead of the default D3D12 backend. | `libs/video/rsx_live_draw.c` |
| `RSX_LIVE_FEED_DBG` | env | unset | diagnostic | What the FIFO actually carries. | `libs/video/cellGcmSys.c`, `libs/video/rsx_d3d12_backend.c` |
| `RSX_MOVEMENT_PROBE_INTERVAL_MS` | env | unset | diagnostic | Behaviour switch; reads a value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `RSX_MOVEMENT_PROOF` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `RSX_MOVEMENT_PROOF_DELAY_MS` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `RSX_MOVIE_COMPOSITE_UI` | env | unset | experimental | Live D3D12: keep drawing UI over movie playback. | `libs/video/rsx_live_draw.c` |
| `RSX_MOVIE_TRACK` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `RSX_NO_RESTART` | env | unset | diagnostic | Live D3D12: ignore primitive restart, for A/B comparison. | `libs/video/rsx_live_draw.c` |
| `RSX_NO_SHADER_DISK_CACHE` | env | unset | feature | Live D3D12: keep no compiled shaders on disk. | `libs/video/rsx_live_draw.c` |
| `RSX_NO_TEX_REFRESH` | env | unset | diagnostic | Turns a behaviour off; reads a value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `RSX_NO_ZETA_TRACK` | env | unset | diagnostic | Turns a behaviour off; reads a value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `RSX_PARITY_DIAG` | env | unset | diagnostic | Debug output; reads a value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `RSX_SCHEDULE_DIAG` | env | unset | diagnostic | Debug output; reads any value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `RSX_SHADER_CACHE_DIR` | env | unset | feature | Live D3D12: directory for the compiled shader cache. | `libs/video/rsx_live_draw.c` |
| `RSX_TARGET_TRACE` | env | unset | diagnostic | Trace output; reads any value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `RSX_TRACE` | env | unset | diagnostic | Trace the first N methods (bare "1" keeps the old 250). | `libs/video/cellGcmSys.c`, `libs/video/rsx_commands.c` |
| `RSX_VALIDATION_CAPTURE` | env | unset | diagnostic | A limit; reads a value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `RSX_VALIDATION_DIR` | env | unset | diagnostic | Live D3D12: directory for validation captures. | `libs/video/rsx_live_draw.c` |
| `RSX_VERTEX_DIAG_DIR` | env | unset | diagnostic | Debug output; reads a path. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `RSX_VERTEX_MODE` | env | unset | experimental | Live D3D12: vertex fetch path (legacy, hoisted, masked or compact). | `libs/video/rsx_live_draw.c` |
| `RSX_VERTEX_PATH` | env | unset | experimental | Live D3D12: older spelling of `RSX_VERTEX_MODE`. | `libs/video/rsx_live_draw.c` |
| `RSX_VERTEX_STATE_TRACE` | env | unset | diagnostic | Trace output; reads any value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `RSX_VTEX_TRACE` | env | unset | diagnostic | Trace output; reads any value. Not described where it is read. | `libs/video/rsx_live_draw.c` |
| `RSX_VTXDBG` | env | unset | diagnostic | Debug output; reads a number. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `RSX_WALKER_PROF` | env | unset | diagnostic | An in-process sampling profile of the FIFO walker thread, for hosts where an external sampler cannot attach. | `libs/video/cellGcmSys.c` |
| `RSX_WALKER_PROF_SECONDS` | env | unset | diagnostic | Default 10); the handler records the interrupted pc and the frame-pointer chain. | `libs/video/cellGcmSys.c` |
| `RSX_WALKER_PROF_TRIGGER` | env | unset | diagnostic | Profiling; reads a value. Not described where it is read. | `libs/video/cellGcmSys.c` |
| `RTT_DUMP` | env | unset | diagnostic | Dumps; reads any value. Not described where it is read. | `libs/video/cellGcmSys.c`, `libs/video/rsx_commands.c` +1 |
| `RTT_PASS` | env | unset | experimental | N shows pass N's output directly on screen (drops later ops, retargets pass N to the backbuffer). | `libs/video/rsx_d3d12_backend.c` |
| `RTT_SAVEA` | env | unset | diagnostic | Show the alpha lane in RED (gates like wave's mask.w live there). | `libs/video/rsx_d3d12_backend.c` |
| `RTT_SAVERT` | env | unset | diagnostic | Behaviour switch; reads a value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `RTT_SAVERT_AFTER` | env | unset | diagnostic | Behaviour switch; reads a value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `RTT_SAVERT_SKIP` | env | unset | diagnostic | Turns a behaviour off; reads a value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `RTT_VIEWRT` | env | unset | diagnostic | Keep op N intact and pull the display composite forward so the chosen RT is shown as of this point in the chain. | `libs/video/rsx_d3d12_backend.c` |
| `RT_CLEARDBG` | env | unset | diagnostic | Magenta-clear the surface the DRAWS bind, at the moment they bind it. | `libs/video/rsx_d3d12_backend.c` |
| `RT_DISPLAY_BY_SIZE` | env | unset | experimental | Also treat a single-target surface whose clip exactly matches the display resolution as the backbuffer. | `libs/video/rsx_d3d12_backend.c` |
| `RT_FORCE_RGBA8` | env | unset | diagnostic | Allocate every offscreen target as R8G8B8A8 rather than its native format. | `libs/video/rsx_d3d12_backend.c` |
| `RT_OFFDBG` | env | unset | diagnostic | Debug output; reads any value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `RT_REBIND` | env | unset | experimental | Re-issue OMSetRenderTargets immediately before every draw instead of only when the target changes. | `libs/video/rsx_d3d12_backend.c` |
| `RT_SEQDBG` | env | unset | diagnostic | Debug output; reads any value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `SCANOUT_FB` | env | unset | diagnostic | Behaviour switch; reads a value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `SCANOUT_FB_DBG` | env | unset | diagnostic | Debug output; reads any value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `SCREENCOPY_DUMP` | env | unset | diagnostic | Stage the captured image into the readback buffer so it can be written out and LOOKED AT. | `libs/video/rsx_d3d12_backend.c` |
| `SCREENTEX` | env | unset | experimental | Report draws that sample a DISPLAY-SIZED texture. | `libs/video/rsx_d3d12_backend.c` |
| `SCREEN_AS_TEX` | env | unset | experimental | Behaviour switch; reads a number. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `SEQ_DBG` | env | unset | diagnostic | Debug output; reads any value. Not described where it is read. | `libs/video/rsx_commands.c` |
| `SUBVP_DBG` | env | unset | diagnostic | Debug output; reads any value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `SUBVP_RTT` | env | unset | experimental | Behaviour switch; reads a number. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `SURFDBG` | env | unset | diagnostic | Debug output; reads any value. Not described where it is read. | `libs/video/rsx_commands.c` |
| `SYS_RSX_DBG` | env | unset | diagnostic | Debug output; reads any value. Not described where it is read. | `libs/video/sys_rsx.c` |
| `SYS_RSX_DMACTL_OFF` | env | unset | diagnostic | Turns a behaviour off; reads a value. Not described where it is read. | `libs/video/sys_rsx.c` |
| `TCONST_ALL` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `libs/video/rsx_commands.c` |
| `TCONST_DBG` | env | unset | diagnostic | Debug output; reads any value. Not described where it is read. | `libs/video/rsx_commands.c` |
| `TCONST_FIFO` | env | unset | experimental | How the title actually streams vertex constants. | `libs/video/cellGcmSys.c` |
| `TCONST_MAX` | env | unset | experimental | A limit; reads a number. Not described where it is read. | `libs/video/rsx_commands.c` |
| `TEXFMTDBG` | env | unset | diagnostic | Debug output; reads a value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `TEXFMT_ALL` | env | unset | experimental | Every distinct texture a draw binds, whatever its format. | `libs/video/rsx_d3d12_backend.c` |
| `TEXUP_DBG` | env | unset | diagnostic | Texture uploads per second. | `libs/video/rsx_d3d12_backend.c` |
| `TEX_AVGDBG` | env | unset | diagnostic | One line per distinct bound texture offset with its average source colour. | `libs/video/rsx_d3d12_backend.c` |
| `TEX_BINDDBG` | env | unset | diagnostic | A unit that ends up with a NULL SRV. | `libs/video/rsx_d3d12_backend.c` |
| `TEX_BUDGET` | env | unset | experimental | Cap texture uploads per frame. | `libs/video/rsx_d3d12_backend.c` |
| `TEX_HILITE` | env | unset | diagnostic | Paint the duck's texture pure red. | `libs/video/rsx_d3d12_backend.c` |
| `TEX_MARKEMPTY` | env | unset | diagnostic | Paint any texture whose guest source is entirely zero a flat magenta instead of uploading the empty bytes. | `libs/video/rsx_d3d12_backend.c` |
| `TEX_NOCACHE` | env | unset | experimental | Always re-upload. | `libs/video/rsx_d3d12_backend.c` |
| `TEX_OFF_BIAS` | env | unset | experimental | Sample the image one whole level-0 BELOW the bound offset. | `libs/video/rsx_d3d12_backend.c` |
| `TEX_PROBE` | env | unset | diagnostic | The location bits are not always right. | `libs/video/rsx_d3d12_backend.c` |
| `TEX_REMAP` | env | unset | experimental | If the bound offset holds nothing, look the image up by size in the VRAM upload registry (cellGcmSys). | `libs/video/rsx_d3d12_backend.c` |
| `TEX_REMAP_ID` | env | unset | experimental | Override the derived crossbar with a fixed mapping, to test channel order directly. | `libs/video/rsx_d3d12_backend.c` |
| `TEX_RESOLVE_AUTO` | env | unset | experimental | Resolve through the page tables (local-page map then IO table) instead of trusting the format's location bits. | `libs/video/rsx_d3d12_backend.c` |
| `TEX_RGBA` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `libs/video/rsx_texture_layout.c` |
| `TEX_SAVE` | env | unset | diagnostic | Also dump wide B8 uploads (Bink video planes are B8 -- the Y plane IS the movie frame in grayscale) + a content stat, so "is the decoder producing pixels" is… | `libs/video/rsx_d3d12_backend.c` |
| `TEX_SRCDBG` | env | unset | diagnostic | Is the guest memory this slot uploads FROM actually populated? An empty source and a broken sampler both render flat. | `libs/video/rsx_d3d12_backend.c` |
| `VBFULL` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `VDATA_DBG` | env | unset | diagnostic | Debug output; reads any value. Not described where it is read. | `libs/video/rsx_commands.c` |
| `VK_DRAW_BATCH` | env | unset | experimental | Vulkan: `1` records engine draws, uploads and clears into batched command buffers. | `libs/video/rsx_vulkan_backend.c` |
| `VK_DRAW_BATCH_MAX` | env | unset | experimental | Vulkan: the most draws one batch records (with `VK_DRAW_BATCH=1`). | `libs/video/rsx_vulkan_backend.c` |
| `VP_ATTRS` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `VP_ATTRS_DUMP` | env | unset | diagnostic | Read n entries of that attribute straight out of guest memory. | `libs/video/rsx_d3d12_backend.c` |
| `VP_ATTRS_FP` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `VP_BYPASS` | env | unset | experimental | Replace the guest transform with a direct map of attribute 0 into clip space. | `libs/video/rsx_d3d12_backend.c` |
| `VP_DUMP` | env | unset | diagnostic | Dumps; reads any value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `VP_FIXPROJ` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `VP_MVP` | env | unset | experimental | The constant bank as the shader will see it, for the first N draws. | `libs/video/rsx_d3d12_backend.c` |
| `VP_NOFIXPROJ` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `VP_NOFREQ` | env | unset | experimental | Instancing kill-switch. | `libs/video/rsx_vertex_fetch.c` |
| `VP_START_OFF` | env | unset | diagnostic | Turns a behaviour off; reads any value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `VP_SUBMIT` | env | unset | experimental | Prove the VP pass actually reaches the GPU. | `libs/video/rsx_d3d12_backend.c` |
| `VP_TESTTRI` | env | unset | diagnostic | Our own triangle, through the game's exact pipeline -- same vpso, same root signature, same vertex buffer, same RTV just bound. | `libs/video/rsx_d3d12_backend.c` |
| `VP_W_ONE` | env | unset | diagnostic | Keep the guest transform but force the clip-space w to 1. | `libs/video/rsx_d3d12_backend.c` |
| `VRAMSCAN` | env | unset | diagnostic | Which 64 KB blocks of guest memory change between frames. | `libs/video/rsx_d3d12_backend.c` |
| `VRAMSCAN_BASE` | env | unset | diagnostic | Behaviour switch; reads a value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `VTX_CACHE_CHECK` | env | unset | diagnostic | Also refetch on every lookup and count differences (validation for a new title. | `libs/video/rsx_d3d12_backend.c` |
| `VTX_DUMP` | env | unset | diagnostic | Dumps; reads a value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `VTX_LIST` | env | unset | diagnostic | Behaviour switch; reads a value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `VTX_NOCACHE` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `libs/video/rsx_d3d12_backend.c` |
| `VTX_POS` | env | unset | experimental | Print the first fetched position of the first N draws. | `libs/video/rsx_d3d12_backend.c` |
| `VTX_STATS` | env | unset | diagnostic | Every 5 s, what the vertex path is given and what it does with it -- draws by primitive, index references against distinct vertices, the conversions actually run,… | `libs/video/rsx_d3d12_backend.c` |
| `VTX_TYPEDBG` | env | unset | diagnostic | Debug output; reads any value. Not described where it is read. | `libs/video/rsx_vertex_fetch.c` |
| `YDKJ_HANDLERFIX` | env | unset | experimental | The handler OPD's code word (*(opd)) gets clobbered to 0 by a mislifted store, so g_ps3_guest_caller reads code=0 -> "not registered" and the game's per-flip/vblank… | `libs/video/cellGcmSys.c` |

## Audio

| Switch | Kind | Default | Class | What it does | Read in |
|---|---|---|---|---|---|
| `AUDIO_PEAK` | env | unset | diagnostic | Report the mixed block's peak amplitude periodically so "is any port producing sound" is answerable (LBP Bink movie audio). | `libs/audio/cellAudio.c` |
| `AUDIO_RATE` | env | unset | experimental | Empty blocks right after a block with sound. | `libs/audio/cellAudio.c` |
| `AUDIO_WAV` | env | unset | experimental | Append the final mix as raw f32le stereo 48 kHz (ffmpeg -f f32le -ar 48000 -ac 2 -i <file>). | `libs/audio/cellAudio.c` |
| `PS3_AUDIO_BUF_MS` | env | `60` | feature | Windows: WASAPI buffer length in milliseconds (10 to 500). | `libs/audio/cellAudio.c` |
| `PS3_MUTE` | env | unset | feature | Mute audio output. | `libs/audio/cellAudio.c` |
| `PS3_NO_AUDIO` | env | unset | experimental | Report that the audio library is unavailable. | `libs/audio/cellAudio.c` |

## Codecs and media

| Switch | Kind | Default | Class | What it does | Read in |
|---|---|---|---|---|---|
| `ADEC_WHO` | env | unset | diagnostic | Name the guest function driving this. | `libs/codec/cellAdec.c` |
| `PS3RECOMP_FFMPEG` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `libs/codec/ffmpeg_runtime.c` |
| `PS3RECOMP_FFMPEG_AVCODEC` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `libs/codec/ffmpeg_runtime.c` |
| `PS3RECOMP_FFMPEG_AVUTIL` | env | unset | experimental | Behaviour switch; reads a value. Not described where it is read. | `libs/codec/ffmpeg_runtime.c` |
| `PS3_NO_SAIL` | env | unset | experimental | Report that the Sail media player cannot be created. | `libs/codec/cellSail.c` |
| `VDEC_IDLE_DRAIN_MS` | env | built-in | experimental | How long cellVdec waits idle before draining queued pictures. | `libs/codec/cellVdec.c` |
| `VDEC_PICLOG` | env | unset | diagnostic | The first pictures handed out, with where they went, then every 1000th and the last few before the AU count. | `libs/codec/cellVdec.c` |
| `VDEC_TRACE` | env | unset | diagnostic | Trace output; reads any value. Not described where it is read. | `libs/codec/cellVdec.c` |

## Filesystem

| Switch | Kind | Default | Class | What it does | Read in |
|---|---|---|---|---|---|
| `PS3_EDAT` | env | on | feature | Decrypt EDAT/SDATA files transparently on open; `0` turns it off. | `libs/filesystem/edat.c` |
| `PS3_FSLOG_BT` | env | unset | diagnostic | Dump the guest back-chain when the game logs the render-config failure, to locate setScreenRenderTargetInternal & the config obj. | `libs/filesystem/cellFs.c`, `runtime/ppu/ppu_fs.cpp` |
| `PS3_RIF_KEY` | env | unset | feature | The 16-byte licence key (hex) used to decrypt NPDRM EDAT files. | `libs/filesystem/edat.c` |
| `PS3_VFS_ROOT` | env | unset | feature | Host directory served as the disc (`/dev_bdvd`, `/app_home`). | `libs/filesystem/cellFs.c`, `runtime/syscalls/sys_fs.c` +1 |

## Input

| Switch | Kind | Default | Class | What it does | Read in |
|---|---|---|---|---|---|
| `PAD_ALWAYS_CHANGE` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `libs/input/cellPad.c` |
| `PAD_AUTOPRESS` | env | unset | diagnostic | Pulse Cross then Start every few seconds, for headless bring-up. | `libs/input/cellPad.c` |
| `PAD_CLOCK` | env | unset | diagnostic | `flips` times `PAD_SCRIPT` by display flips instead of wall time. | `libs/input/cellPad.c` |
| `PAD_FILE` | env | unset | diagnostic | Read pad input from a file a script writes. | `libs/input/cellPad.c` |
| `PAD_NO_KEYBOARD` | env | unset | feature | Do not drive pad 0 from the keyboard. | `libs/input/cellPad.c` |
| `PAD_SCRIPT` | env | unset | diagnostic | Play a scripted sequence of pad presses. | `libs/input/cellPad.c` |
| `PAD_STICK` | env | unset | diagnostic | Hold a stick position (with optional start and end times). | `libs/input/cellPad.c` |
| `PAD_SWEEP` | env | unset | diagnostic | Sweep the sticks through each direction in turn. | `libs/input/cellPad.c` |
| `YDKJ_INJECT_PAD` | env | unset | experimental | The game's wait loop polls the pad (cellPadSetActDirect / GetData) -- if it's parked on a "press button" prompt after loading, no host input means it waits forever. | `libs/input/cellPad.c` |
| `YDKJ_PAD_BTN` | env | unset | experimental | Behaviour switch; reads a number. Not described where it is read. | `libs/input/cellPad.c` |
| `YDKJ_PAD_DELAY` | env | unset | experimental | Behaviour switch; reads a number. Not described where it is read. | `libs/input/cellPad.c` |

## Network

| Switch | Kind | Default | Class | What it does | Read in |
|---|---|---|---|---|---|
| `PS3_GAMEDATA_ROOT` | env | unset | feature | Host directory for trophy data. | `libs/network/sceNpTrophy.c`, `libs/system/cellSaveData.c` |
| `PS3_NET_ONLINE` | env | unset | feature | Report the network as connected. | `libs/network/cellNetCtl.c`, `libs/network/np_psnr.c` +1 |
| `PS3_NET_P2P_PORT` | env | `3658` | feature | The host port this instance's P2P sockets bind. | `libs/network/np_psnr.c` |
| `PS3_NET_TRACE` | env | unset | diagnostic | Log every packet sent and every one received (not the empty polls), with its peer. | `libs/network/sysNet.c` |
| `PS3_NP_ONLINE_ID` | env | unset | feature | The online ID to report (after `--username`). | `libs/network/np_psnr.c` |
| `PS3_NP_SIGNALING_DELAY_MS` | env | `1000` | feature | How long after a member joins that signaling reports them established. | `libs/network/sceNpMatching2.c` |
| `PSNR_SERVER` | env | unset | feature | Online play: the matchmaking server address (set by `--server`); with `PS3_NET_ONLINE` it turns the online services on. | `libs/network/np_psnr.c` |
| `USER` | env | host | feature | Host login name, the fallback online ID when online (POSIX). | `libs/network/np_psnr.c` |
| `USERNAME` | env | host | feature | Host login name, the fallback online ID when online (Windows). | `libs/network/np_psnr.c` |

## System and other HLE libraries

| Switch | Kind | Default | Class | What it does | Read in |
|---|---|---|---|---|---|
| `CELLMARK_HOLD` | env | unset | diagnostic | So the last rendered frame is visible / capturable. | `libs/system/sysPrxForUser.c` |
| `FLOW_LOCKOK` | env | unset | experimental | Behaviour switch; reads any value. Not described where it is read. | `libs/system/sysPrxForUser.c` |
| `LANG` | env | host | feature | Host locale, read after `LC_MESSAGES` (POSIX). | `libs/system/cellSysutil.c` |
| `LC_ALL` | env | host | feature | Host locale, read for the system language when `PS3_LANG` is unset (POSIX). | `libs/system/cellSysutil.c` |
| `LC_MESSAGES` | env | host | feature | Host locale, read after `LC_ALL` (POSIX). | `libs/system/cellSysutil.c` |
| `MSGDIALOG_ANSWER` | env | unset | diagnostic | Log output; reads a value. Not described where it is read. | `libs/system/cellMsgDialog.c` |
| `MSGDIALOG_AUTODISMISS_MS` | env | unset | diagnostic | Log output; reads a value. Not described where it is read. | `libs/system/cellMsgDialog.c` |
| `MSGDIALOG_NOBUTTON_WAITS` | env | unset | diagnostic | Log output; reads any value. Not described where it is read. | `libs/system/cellMsgDialog.c` |
| `PS3_CAMERA` | env | on | feature | `0` reports that no camera is attached. | `libs/hardware/cellCamera.c` |
| `PS3_DISCGAME_BOOT` | env | on | feature | `0` reports the title as installed to HDD instead of booted from disc. | `libs/system/cellSysutil.c` |
| `PS3_DISPLAY_GAMMA` | env | unset | feature | Display gamma applied by the video out (`out = in^g`), overriding the title's setting. | `libs/misc/cellAvconfExt.c` |
| `PS3_GAME_FREE_SPACE_RAW` | env | unset | feature | Report the real free space to cellGame instead of the capped value. | `libs/system/cellGame.c` |
| `PS3_HDD0_ROOT` | env | unset | feature | Host directory that `/dev_hdd0` maps to (game updates, saves). | `libs/system/cellGame.c`, `runtime/ppu/ppu_fs.cpp` +1 |
| `PS3_HDD1_ROOT` | env | unset | feature | Writable host directory for the `/dev_hdd1` cache partition. | `libs/system/cellSysutil.c`, `runtime/ppu/ppu_fs.cpp` +1 |
| `PS3_LANG` | env | unset | feature | System language reported to the title (else the host locale). | `libs/system/cellSysutil.c` |
| `PS3_LICENSE_AREA` | env | unset | feature | Console licence area reported to the title (picks region variants of assets). | `libs/system/cellSysutil.c` |
| `PS3_QUIT_GRACE_MS` | env | built-in | feature | How long a quit request waits for save data to finish. | `libs/system/cellSysutil.c` |
| `PS3_SAVEDATA_ARG9_OFF` | env | unset | diagnostic | Turns a behaviour off; reads a value. Not described where it is read. | `libs/system/cellSaveData.c` |
| `SYNC_QTRACE` | env | unset | diagnostic | Trace output; reads any value. Not described where it is read. | `libs/sync/cellSync.c` |
| `SYNC_TRACE` | env | unset | diagnostic | Trace output; reads any value. Not described where it is read. | `libs/sync/cellSync.c` |
| `SYNC_WATCH` | env | unset | diagnostic | <hex ea>: log every HLE operation on that one mutex with its word before the op, so a PPU re-initialise under an SPU waiter is visible. | `libs/sync/cellSync.c` |
| `SYS_HEAP_DBG` | env | unset | diagnostic | Debug output; reads any value. Not described where it is read. | `libs/system/sysPrxForUser.c` |
| `YDKJ_NOEXIT` | env | unset | experimental | The title self-exits after ~2 frames on a teardown assert triggered by incomplete SPU/SPURS state (a timing race). | `libs/system/sysPrxForUser.c` |

## Project template

| Switch | Kind | Default | Class | What it does | Read in |
|---|---|---|---|---|---|
| `PS3_TITLE` | env | unset | feature | Project template: window caption (else the PARAM.SFO title). | `templates/project/main.cpp` |

## Tools

| Switch | Kind | Default | Class | What it does | Read in |
|---|---|---|---|---|---|
| `HOST_CC` | env | unset | feature | Tools: host C compiler for the SPU lifter self-test. | `tools/spu_lift_selftest.py` |
| `HOST_CXX` | env | unset | feature | Tools: host C++ compiler for the PPU lifter self-test. | `tools/lift_selftest.py` |
| `JT_DEBUG` | env | `0` | diagnostic | Tools: PPU lifter jump-table debug output level. | `tools/ppu_lifter.py` |
| `LBP_JOBMOD_EXTRA_FUNCS` | env | unset | experimental | Tools: extra function addresses for job-module lifting. | `tools/lift_jobmods.py` |
| `OBJDUMP_SPU` | env | unset | feature | Tools: SPU objdump for the disassembly audit. | `tools/objdump_audit.py` |
| `PPU_GCC` | env | unset | feature | Tools: PPU cross compiler for the lifter self-test. | `tools/lift_selftest.py` |
| `PS3_USRDIR` | env | unset | feature | Tools: the title's USRDIR for job-module extraction. | `tools/extract_jobmods.py` |
| `SPU_DUMP_TOOL_EXE` | env | unset | feature | Tools: SPU disassembler for the disassembly audits. | `tools/spu_disasm_audit.py`, `tools/spu_disasm_audit_operands.py` |
| `SPU_GCC` | env | unset | feature | Tools: SPU cross compiler for the SPU lifter self-test. | `tools/spu_lift_selftest.py` |
