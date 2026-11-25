# Differential firmware call trace (port vs RPCS3)

A recompiled title calls the same firmware imports, in the same order, as the
same title running on a PS3 or in RPCS3, until a firmware call returns
something different. This tooling records every firmware call on both sides
in one format and diffs them, so a firmware difference is found by diff
instead of by hand-tracing Cell/HLE calls.

RPCS3 is used as a **reference only**: its behaviour, its logs and its GDB
stub are read; none of its code is in this toolkit.

| Piece | What it does |
|---|---|
| `runtime/ppu/ppu_calltrace.h` + `ppu_hle.cpp` | `PS3_CALLTRACE=<file>`: the port writes one JSON line per HLE import (`ps3_hle_call`) and lv2 syscall (`lv2_syscall`) |
| `tools/rpcs3_probe/rpcs3_calltrace.py` | the same records from RPCS3, over its GDB stub (breakpoints on the title's import stubs and `sc` sites) |
| `tools/rpcs3_probe/rpcs3_log2calltrace.py` | a cheaper, partial reference trace from `RPCS3.log` at Trace level |
| `tools/rpcs3_probe/check_capture.py` | completeness of a GDB capture, checked against the `RPCS3.log` of the same run |
| `tools/hle_diff.py` | aligns two traces per thread and reports the first divergence and a per-function summary |
| `tools/calltrace.py` | the shared format: loader, NID computation, written-memory runs |
| `tools/test_hle_diff.py` | tests on synthetic traces and log lines, and the GDB capture loop against a simulated stub |

## The trace format (`ps3-calltrace/1`)

One JSON object per line:

```
{"type":"header","schema":"ps3-calltrace/1","side":"ps3recomp"|"rpcs3","win":256,...}
{"type":"thread","tid":3,"name":"PhyreEngineRenderThread"}
{"type":"call","tid":3,"seq":118,"depth":0,"kind":"hle","nid":"0x21AC3697","name":"cellGcmAddressToOffset",
 "lr":"0x0025D4F8","args":["0x30100000","0xD0FFFE80",...8],"ret":"0x0",
 "wr":[[1,0,4,"00100000"]],"t_us":81234,"frame":3,"impl":"hle"}
```

| Field | Meaning |
|---|---|
| `tid` | the side's own thread id (port: slot index, RPCS3: `0x01000000+n`); paired across sides by thread name and creation order |
| `seq` | per-thread call index, taken at entry (a nested call, made from a guest callback inside an HLE call, has a higher seq and `depth+1`) |
| `kind`, `nid` / `sc`, `name` | the function: NID of the import, or syscall number |
| `lr` | the guest return address: identifies the call site in the title |
| `args` | r3..r10 at entry (all eight; the diff compares only the first `argc` when the arity is known) |
| `argc` | the function's argument count, when the capture knows it (from RPCS3's log line) |
| `ret` | r3 on return; `null` when the method cannot see it |
| `wr` | guest memory the call wrote: for each argument that points into guest memory, the bytes that changed in a window of `win` bytes behind it, as `[arg, offset, length, data]`; data is hex up to 32 bytes, `#<fnv1a64>` beyond; runs closer than 8 bytes are merged. `null` when the method cannot see memory |
| `t_us`, `frame` | port: microseconds since start, guest flips requested so far; diagnostic only, never compared |
| `impl` | port: `hle` (our handler), `prx` (a loaded firmware PRX, lifted), `unresolved` |

Both sides compute `wr` the same way (snapshot before, compare after), so the
records compare directly. Limits of that definition: a byte the call writes
with the value it already had is not seen as written; a buffer larger than the
window is only seen in its first `win` bytes; another thread writing into the
window during a blocking call shows up as the call's write.

## Port side

```
set PS3_CALLTRACE=C:\runs\port.jsonl          :: on; the file is truncated at start
set PS3_CALLTRACE_FILTER=cellGcm*,sc,-sc:141  :: optional: names/prefixes, NIDs, sc, sc:N; '-' excludes
set PS3_CALLTRACE_WIN=256                     :: optional: memory window per pointer argument (0 = none)
set PS3_CALLTRACE_MAX=200000                  :: optional: stop after n records
```

Off, it costs one load and a not-taken branch per call (the scope object in
`ps3_hle_call` and `lv2_syscall`). On, a boot to a title screen writes on
the order of half a million records (about 150 MB) in a minute. The file is
flushed every 250 ms, so a run can be ended by killing the process; the last
line may be torn and the loader skips it.
Nothing in it is game-specific.

## RPCS3 side

Two methods; use the log for a quick look and the GDB stub when the diff
needs return values and memory.

| | GDB stub (`rpcs3_calltrace.py`) | Log (`rpcs3_log2calltrace.py`) |
|---|---|---|
| RPCS3 speed | Interpreter (breakpoints need it), plus a pause per call | full speed (LLVM) |
| HLE calls covered | every import stub hit (filterable) | only functions that log an entry line at an enabled level |
| syscalls covered | `sc` sites in the title's ELF | every syscall (`PPU: Syscall '...' finished` at Trace) |
| arguments | r3..r10 | as RPCS3 formats them; arity exact |
| return value | always | syscalls always; HLE only on failure (first 3 of each message per thread) |
| written memory | yes, same definition as the port | no |
| thread names | merged from the run's `RPCS3.log` (`--log`) | yes |
| caller (`lr`) | yes | yes (`[HLE:..., LR:...]` in the line prefix) |
| calls made by LLE firmware | not seen (breakpoints are on the title's stubs) | dropped by caller address unless `--keep-prx` |

Setup used here (portable RPCS3, `config/config.yml`): firmware installed with
`rpcs3.exe --headless --installfw PS3UPDAT.PUP`; the title's update linked into
`dev_hdd0/game/<TITLE_ID>`; pads `Null`, background input off; `Log:` entries
at `Trace` for `PPU` and the title's modules and lv2 channels; for the GDB
method also `Core: PPU Decoder: Interpreter` (RPCS3 refuses breakpoints under
LLVM; `Miscellaneous: GDB Server` defaults to `127.0.0.1:2345`).

Apply the title's own settings from RPCS3's compatibility wiki to the
reference config: a setting that fixes a rendering or timing problem there
also keeps the reference's game state right (for example `Write Color
Buffers`, for titles that read a render target with the CPU: without it
RPCS3's own state differs from a PS3's from the first such read on).

RPCS3's GDB stub as it behaves (v0.0.43; checked on the running stub, and
read in RPCS3's `Emu/GDB.cpp`, `CPUThread.cpp` and `PPUThread.cpp` as a
reference; the rig is built around each point):

| Behaviour | What the rig does |
|---|---|
| no `target.xml`; `g` is GDB's powerpc-64 block: r0-r31, f0-f31, pc, msr 64-bit, cr 32-bit, lr, ctr 64-bit, xer, fpscr 32-bit (1112 hex digits; msr/xer/fpscr sent as `x`) | `rsp.py` falls back to that layout |
| `vCont;c`/`vCont;s` only, no plain `c`; a stop reply is a bare `S05` with no thread id | `qC` after every stop |
| the emulator pauses at boot until a client continues | breakpoints are set before the first `vCont;c` |
| a breakpoint debug-pauses the thread that executes it; the first one since the last `vCont` becomes the stub's `pausedBy` and ends the wait; any other stays paused, unreported ("parked") | after `--quiet` seconds (default 3) without a stop, and every `--sweep-every` seconds (default 10) while stops keep coming, the rig interrupts and lists all threads (`qfThreadInfo`/`qsThreadInfo`); each thread on a breakpoint it has not recorded there is recorded and queued for release |
| at every stop the stub pauses the whole emulator, selects `pausedBy` (what `qC` returns) and removes that thread's debug pause; after a `^C`, `pausedBy` is the thread of the previous stop | the `qC` thread is recorded before anything runs again, after a `^C` too (it may have parked on a breakpoint since its own stop, and the stub has just released it) |
| `vCont;c` removes the debug pause of the `Hc` thread only, then resumes everything | before each `vCont;c` the rig sets `Hc` to one queued parked thread, else to the thread just handled (already free): no unrecorded thread is ever released |
| a thread released on its breakpoint executes the original instruction; the breakpoint does not fire again (checked: a breakpoint at the next instruction is the next stop) | no single-stepping. Up to the first captures the rig stepped each thread off its breakpoint: a step resumes every thread, so another thread's breakpoint stop was taken for the step's end after the stub had released that thread, which then ran on unrecorded (the 1.2% lost returns and the missing calls); and a step over a blocking `sc` had to be interrupted, leaving a step flag behind |
| an emulator pause can catch a thread at a breakpoint address before it executed the breakpoint; it then stops there when it runs | a thread is recorded once per arrival at a breakpoint: recorded at `(pc, r1)`, it is not recorded again there until it has been seen elsewhere (it cannot come back without stopping at another breakpoint, its call's return or the next stub) |
| a `^C` that races a stop is answered outside `vCont` with an empty packet | after an interrupt the `qC` reply skips an empty packet |
| a thread blocked in a syscall shows the `sc` as its pc | not recorded again (it is at the position it was recorded at); released again only every 30 s, in case it is in fact parked on the `sc` breakpoint |

The `sc` sites carry their syscall number statically (`li r11,N` before the
`sc`), so `--filter` drops `sc:N` sites before they cost a stop; a site whose
number is not found statically stays traced. Thread names come from the trace
itself: the thread present at connect is `main_thread`, each successful
`sys_ppu_thread_create` names the new id from its name argument (`--log`
still merges names from `RPCS3.log`).

```
python tools/rpcs3_probe/rpcs3_log2calltrace.py RPCS3.log --imports EBOOT.imports.json \
       --elf EBOOT.elf -o rpcs3_log.jsonl --arity-out arity.json
python tools/rpcs3_probe/rpcs3_calltrace.py --rpcs3 rpcs3.exe --title <EBOOT.BIN> \
       --imports EBOOT.imports.json --elf EBOOT.elf --arity arity.json \
       --out rpcs3_gdb.jsonl --seconds 1800 --log <rpcs3>/log/RPCS3.log \
       "--filter=-cellGcmAddressToOffset,-sc:141"
```

Pass `--filter` with `=` (a value starting with `-` is otherwise read as an
option). The last packets each way are written to `<out>.rsp.txt` at exit.

**Checking a capture.** Run RPCS3 with the log channels at Trace (the
`Log:` section of `config.trace.yml`, the PPU decoder left at Interpreter)
while the GDB capture runs, convert that log, and compare:

```
python tools/rpcs3_probe/rpcs3_log2calltrace.py RPCS3.log --imports EBOOT.imports.json --elf EBOOT.elf -o log.jsonl
python tools/rpcs3_probe/check_capture.py rpcs3_gdb.jsonl log.jsonl --imports EBOOT.imports.json
```

It is one run seen twice with the same thread ids: every call in the log
that the capture's filter (kept in its header) traces must be in the
capture, per thread and in order, from the same call site and, for a
syscall, with the same r3. It prints matched, lost, extra and still-open
calls per thread and exits non-zero on a lost call or a differing return.
Log calls after a thread's last matched call count as "after the capture".

## The diff

```
python tools/hle_diff.py port.jsonl rpcs3.jsonl --imports EBOOT.imports.json [--json result.json]
```

1. Threads are paired by name (`main` = `main_thread`), the k-th thread of a
   name with the k-th; `--pair A=B` forces a pair.
2. Calls whose function the other side never calls at all are listed apart
   ("Called on one side only") and kept out of the alignment.
3. Repeated identical calls (same function, same result, nothing written) are
   collapsed, so a poll that spins 3 times on one side and 40 on the other
   aligns.
4. Each thread pair is aligned greedily by function, resyncing within
   `--window` calls (preferring a resync the next call confirms); skipped
   calls are "only" on one side.
5. Paired calls compare: `ret` (low 32 bits, plus a mismatch of the upper
   32 bits: a 64-bit value on one side only is typically a host pointer
   handed to the guest), `args` (first `argc`), `mem` (same offset written
   with different data), `mem1` (written on one side only, a softer signal
   because of the window definition above).
6. Tolerated differences: values that look like handles, ids or addresses are
   mapped on first use (a bijection: `3` here may be `0x85000100` there, as
   long as it stays so); CELL error codes (`0x80XXyyyy`, XX != 0) and values
   below 0x10000 on both sides must match exactly, and so must the return of
   a function named for a quantity (`...Size`, `...Count`, `...Num...`,
   `...Version`); time, input, lwmutex and lwcond calls are volatile
   (aligned, results not compared; `--volatile` adds patterns,
   `--no-default-volatile` removes them).
7. Layout differences: `--addr-map PORT_BASE=REF_BASE[:SIZE]` (hex, SIZE
   default `0x10000000`, repeatable) says the first side's region at
   PORT_BASE is the second side's at REF_BASE, e.g. `0x40000000=0x30000000`
   for user memory. A value in the region is translated before it is
   compared (returns, arguments, written words) and does not use up an id
   mapping. A written run is byte-granular, so an address stored in memory
   often shows as part of a word (`40` against `30` when only its top byte
   changed): each word a run touches is compared with the bytes outside the
   run taken as zero on both sides.

Output: per thread pair the first divergence with the calls before it, then
the functions with a divergence, then the functions called on one side only.
`--json` writes all of it.

Functions are ranked (`--rank first`, the default) by the worst kind of
divergence they show, then by where in the first side's trace that kind
first happens (column "first at": thread and call number):

| kind | meaning |
|---|---|
| `host` | a return with upper 32 bits on one side only: a host pointer handed to the guest, always a bug |
| `value` | a return, argument or written bytes that differ |
| `only` | calls made on one side only (a function the other side never calls, or calls the alignment skipped) |
| `mem1` | bytes written on one side only (soft, see the window definition) |

So one wrong return early in the boot ranks above a soft difference repeated
a thousand times. `--rank count` restores the older order, a score weighted
by how many calls diverge.

## Using it on a title

Capture both sides with the same filter and the same window, boot to the same
point, then diff with `--imports`. Read the first divergence of the main
thread first: later divergences are usually its consequences.

