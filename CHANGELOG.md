# Changelog

### v0.15.0 — *"Seventy-Six"* (October 2026)

*76 pull requests from six contributors, merged in #325. They come from five
ports in progress: Killzone 2 and 3, inFAMOUS and inFAMOUS 2, and Twisted
Metal, plus tekulvw's correctness sweep of the runtime and Metal. Every
change ships with a test or a measurement.*

**SPU and SPURS**
- **Hardware-exact SPU float** (*[@NoahBPeterson](https://github.com/NoahBPeterson)*, #287).
  Single precision now follows the ISA: extended range, truncation, denormal
  flush and saturation. `frest` uses the hardware table. Results are checked
  against an exact-arithmetic referee, and NEON paths run 1.6-4x faster with
  bit-identical output. This replaces the previous GPL-derived float code.
- **SPURS policy scheduling** (*[@tekulvw](https://github.com/tekulvw)*, #291).
  Each instance gets a coordinator and `nSpus` persistent workers, with
  admission capped by each workload's contention, so a waiting consumer no
  longer starves its producer. Every live SPU context also keeps private DMA
  and reservation state (#290).
- Interpreter fallback resumes lifted code (#266), and machine status reports
  the interrupt-enable bit (#289). MFC `SDCRZ` is implemented (#295), FNMS
  keeps its addend for zero times an extended operand (#318), and parked-list
  diagnostics are bounded (#298). SPURS task exit-code containers (#283),
  taskset storage size (#284) and TaskInfo requeue masking (#306)
  (*@tekulvw*).
- `build_spu_workloads.py --input` discovers, validates and registers SPU ELFs
  from supplied files, and the project template runs it (*@tekulvw*, #288).
- `SPU_RdDec` no longer wraps 231 s after its base stamp
  (*[@erikomis](https://github.com/erikomis)*, #313).
- From *[@undecuple](https://github.com/undecuple)*: the SMC micro-interpreter
  addresses `lqa`/`stqa` at I16 << 2 (#268). The MFC engine table grows past
  eight contexts (#272, later replaced by #290's unbounded registry). The
  policy module's u64 argument is passed in the preferred doubleword (#309),
  and `_cellSpursAttributeInitialize` gets its SDK argument order (#310).

**PPU lifter and loader**
- **Exact VMX** (*@NoahBPeterson*, #286). About 45 VMX ops that lifted as
  no-ops are now implemented, along with VSCR saturation and CR6 compares.
  BO/BI branches are decoded (`bdnzt` and `bdzf` were always taken), CTR is
  64-bit, and get-PC idioms are recognised.
- From *@undecuple*:
  - big-endian lane order for ten VMX integer ops (#269);
  - `lwax` jump tables (#285);
  - `sc` at the end of a function falls through (#297);
  - a carry-form definition is no longer mistaken for a callee-save restore
    (#320);
  - `--chunks N` cuts lifted source by guest address and rewrites only the
    files that changed (#254).
- Loader, from *@undecuple*: TLS blocks sized by the TLS image (#253), a
  per-coroutine indirect-call depth guard (#270), and thread-locals that stay
  thread-local under MinGW (#271).
- Jump tables behind a tail-call epilogue are found (*@erikomis*, #311).
- Boot, from *@tekulvw*: PPU entry argv/envp live in stack-owned storage
  (#263), and HLE registrations regenerate when library sources change (#262).

**RSX, draw engine, Metal and Vulkan**
- **Depth:** from *@undecuple*, fragment depth export (#279), depth buffers
  sampled as textures return real depth (#280), and Z16 clear values are
  decoded (#282). From *@tekulvw*, 2D depth-compare textures (#302), depth24
  endpoints preserved (#299), depth export in the shader cache keys (#301),
  and Metal depth attachments read back to guest memory (#300). From
  *[@Kurtisone](https://github.com/Kurtisone)*, the depth bounds test (#319).
- **Colour:** from *@tekulvw*, sRGB framebuffer output (#304), per-channel
  texture input gamma (#305), fragment colour inputs clamped before source
  modifiers (#303), and register-fed DATA3F/DATA4F immediate vertices (#292).
- **Fragment decompiler:** predicated writes are a select, not a lerp
  (*@undecuple*, #278). The NV40 pack/unpack opcodes are implemented
  (*@erikomis*, #314).
- Antialiased surfaces render at sample resolution (#281), and the sampler
  cache key covers every field it decodes (#321) (*@undecuple*).
- **Metal:**
  - from *@tekulvw*: owned colour targets snapshotted in GPU order (#307),
    clears replayed over complete surfaces (#293), real window-close
    reporting (#308), and opt-in `PS3RECOMP_METAL_FPS` frame timing (#294);
  - from *@erikomis*: a command buffer that fails is reported (#316).

**cellGcm**
- From *@undecuple*: IO mapping table 16 -> 512 with no leak on refusal
  (#255), `GET_REPORT` honours its DMA context (#256), FIFO user commands
  reach `cellGcmSetUserHandler` (#273), and the in-FIFO flip wraps only the
  runtime's own ring (#275).
- Strict-order IO mapping (*@tekulvw*, #248). A parked FIFO gets time to be
  patched before the walker follows `put` (*@erikomis*, #312).

**System libraries**
- **cellFs:** directory entries (*@tekulvw*, #247), configured disc roots
  (*@tekulvw*, #249), `GetFreeSize`/`Chmod`/`Utime`/`SdataOpenByFd` (#257),
  and 64-bit sizes on Windows (#258).
- **cellSaveData:** PARAM.SFO is written and read as a real binary PSF, and
  old text saves still load (#322). UserAutoSave keeps its ninth argument
  (#276).
- **Network:**
  - from *@undecuple*: cellHttp's URI path (#260) and response getters (#261);
    sceNpBasic usable after `sceNpInit` (#259); sceNpCommerce2 `CreateCtx`
    (#323); the EDAT klicensee from `sceNpDrmIsAvailable` (#274);
  - from *@tekulvw*: no SIGPIPE on host sends (#250) and `inet_pton` (#265).
- **Other:**
  - from *@undecuple*: `sys_lwcond_t` layout (#277) and
    `cellFontInitializeWithRevision` (#324);
  - from *@erikomis*: `cellGemGetMemorySize` and the event-queue notify forms
    (#315);
  - from *@tekulvw*: the timezone syscall (#264), extended sysmodule tracking
    (#267), message-dialog abort (#251) and Sail boot parameters (#252).

### v0.14.0 — *"Outside In"* (October 2026)

*This release is mostly community work: 28 pull requests from five
contributors. Most of it came out of the Twisted Metal port on Linux, which put
the shared draw engine and the Vulkan backend under a real deferred renderer.*

**Draw engine and Vulkan**, from *[@Kurtisone](https://github.com/Kurtisone)*:
- **Surfaces read in another format** now go through guest memory: a depth
  buffer read as colour, a Z16 shadow map, a float packed into RGBA8. Each
  colour and depth write carries a serial, so the engine knows which one is
  newer (#236).
- Surfaces stay coherent around NV3089 transfers (#224), and cellGcmSys hands
  the engine its display buffers and flips (#223).
- Texel-addressed texture coordinates are scaled (#221), B8 presents its one
  channel as R, G and B (#220), and a Z16 zeta clears to its own far plane
  (#235).
- Vulkan gains RGBA16F colour targets (#222), cube maps (#229), images sized by
  the device's limit (#227) and readbacks from cached memory (#245).
- New diagnostics: `RSX_ENGINE_PASS_DUMP` dumps a frame pass by pass, with
  each pass's targets, depth and programs (#237).
  `PS3RECOMP_VK_DUMP_EVERY` dumps every nth presented frame (#219).
- **Vulkan stencil** (*[@gabryboy12-ai](https://github.com/gabryboy12-ai)*,
  #231): the depth format carries stencil, so the engine's stencil state now
  takes effect.
- **D3D12:** fragment programs whose HLSL ran past 32 KiB no longer fail to
  compile (*[@robbe1912](https://github.com/robbe1912)*, #230).

**Runtime and SPU**, from *[@Kurtisone](https://github.com/Kurtisone)* unless noted:
- **SPU-to-SPU task signals.** A SPURS task signalled from another SPU wakes
  immediately. Before, it woke only when its 1 s timed wait ran out, which made
  one frame of Twisted Metal's post-FX take 4.1 s (#242).
- **SPURS:** the guest's taskset no longer has native fields written into it
  (#240). `cellSpursBarrierInitialize` and `cellSpursGetTasksetInfo` are
  implemented (#241).
- **cellGcmSys:** a main-memory offset now resolves through the IO table first
  (#238). `ps3_ms_now` gets a clock off Windows (#244).
- **POSIX:** lwmutex and lwcond lock for real (#225), and stack scans skip
  unreadable guest pages (#217).
- **SPU disassembler:** decodes `orbi`, `xorhi` and `xorbi` (#239).
- **SPU interpreter:** `ILH` replicates its halfword (*[@tekulvw](https://github.com/tekulvw)*, #234).
- **Lifter:** `--chunk-lines` sets the size of the source chunks (#216).
- **Diagnostics:** `PS3_NULL_READ_STACK` names the guest code behind a NULL
  read (#226). The main thread's context is published as `g_ppu_main_ctx`
  (#228).

**Media**: cellVdec decodes H.264 and MPEG-2 through FFmpeg loaded at run time
(*[@undecuple](https://github.com/undecuple)*, #215). It is opt-in
(`-DPS3RECOMP_FFMPEG_RUNTIME=ON`) and FFmpeg is never linked. Without it,
cellVdec behaves as before.

**Housekeeping**:
- `fw_spu/` is no longer tracked, and `*.prx` is ignored. The tree had been
  carrying decrypted firmware PRXs and C lifted from them, against our own
  firmware policy. Thanks to *[@deucebucket](https://github.com/deucebucket)*
  for reporting it (#210, #212).
- README audit: the Getting Started commands now produce a buildable project,
  the platform table matches CI, and the provenance wording is corrected (RPCS3
  is a reference, not a code source). Changelogs before v0.14.0 moved to
  CHANGELOG.md.

### v0.13.0 — *"Player Two"* (October 2026)

*Two titles in game, and one of them online. The Simpsons Arcade Game plays a*
*two-player match over the network, and Tornado Outbreak reaches its first level*
*rendering correctly with no per-title overrides. Thirty-odd pull requests from*
*six people since v0.12.1.*

**Online**
- **NP online over [psnr](https://github.com/sp00nznet/psnr)** (#196, #200) —
  real host sockets behind `PS3_NET_ONLINE`; sceNpMatching2 rooms, signaling and
  callbacks, sceNpScore leaderboards, sceNpLookup, cellSysutilAvc2; NAT probes,
  hole punching and a relay for peers that cannot reach each other.
  `--psnr host --username NAME` on the runtime's command line. Offline is still
  the default and unchanged. See [docs/ONLINE.md](docs/ONLINE.md).

**Rendering**
- **A Vulkan backend for Linux** — *[@gabryboy12-ai](https://github.com/gabryboy12-ai)*
  (#182, #192). Opt-in; runs the guest's own shaders on the shared register-file
  draw engine and passes every `ps3recomp_host` scene under the validation layer.
- **`SET_TRANSFORM_CONSTANT` is a 32-dword window** — *[@andrebrumdev](https://github.com/andrebrumdev)*
  (#208); three registers were corrupting God of War II's skinning palette.
- Fragment-program flow control (IFE/LOOP/REP/BRK) and LIF (#187), vertex-program
  flow control, GPU resolve copies and raw-SPU proxy DMA (#189), job-chain Join
  and in-FIFO flips (#185).

**Lifter**
- *[@andrebrumdev](https://github.com/andrebrumdev)*: jump-table base arbitrated
  by validated targets (#179); callee-save snapshots only for real epilogue
  restores, and `vcmpequb.`/`vcmpequh.` set CR6 (#207).
- Conditional fallthrough at a chunk end (#202), multi-TOC jump-table ranking
  (#195), table-less computed jumps with a constant base (#193).

**SPU** — *[@sagemono](https://github.com/sagemono)*, from LittleBigPlanet:
non-r0 link returns (#174), data-only job-module entries (#175), drain resume
and `iret` unwind (#176), the taskset syscall on a planted context (#177),
`_sys_spu_image_import` segment source (#178).

**Audio and media** — cellAtrac with ATRAC3plus (#186), cellVdec/cellAdec query
and open fixes (#197, #198, #201), cellSail state changes (#190), job-chain
completion carrying the job index (#204).

**Build** — Linux and macOS building again (*[@gabryboy12-ai](https://github.com/gabryboy12-ai)*
#191, #205), `-msse4.1` probed (*[@andrebrumdev](https://github.com/andrebrumdev)* #206),
`spu_lifter --header-name` honoured (*[@tomspilman](https://github.com/tomspilman)* #181),
the onboarding docs rewritten to the pipeline the tools implement (#180), and
[AnyPS5](https://github.com/boykopovar/AnyPS5) added to the sibling projects
(*[@bojogon7](https://github.com/bojogon7)* #209).

### v0.12.1 — *"Downloads, Again"* (September 2026)

*A packaging release. v0.10.0 and v0.11.0 were both tagged and neither was ever*
*published — their release runs died on macOS with an undeclared*
*`RtlCaptureStackBackTrace`, so the newest download anyone could get was v0.9.1*
*from August. That is fixed, and Linux now builds alongside Windows and macOS.*

- **Bundles for all three platforms.** Linux joins the release matrix. It ships
  as the toolkit and runtime library rather than a game runner, and each bundle's
  `VERSION.txt` says what its platform can actually do — Windows renders and runs
  titles through D3D12, macOS renders through Metal but cannot yet load one
  because the PPU boot scaffold does not run on POSIX, and Linux has no renderer
  at all. Someone downloading the Linux zip should learn that from the file, not
  after building something with it.
- **A dry run works from any branch.** `workflow_dispatch` set the bundle version
  from the *branch* name, so a branch with a slash in it nested the staging
  directory and the upload glob found nothing — every platform built, tested and
  staged, and the run failed anyway.
- **`cellGame` content root strips `/dev_hdd0`** — *[@sagemono](https://github.com/sagemono)*
  (#163, landed as #173). `ppu_fs` strips the mount prefix, so guest
  `/dev_hdd0/game/<id>` resolves to `<root>/game/<id>`; spelling the device out
  recreated the very split v0.12.0 set out to close, one directory over.

### v0.12.0 — *"One Tree"* (September 2026)

*Nineteen open pull requests land on one branch. They came from the two people*
*who have games actually running —* [@canersaka](https://github.com/canersaka)
*with Yakuza: Dead Souls on macOS,* [@sagemono](https://github.com/sagemono)
*with LittleBigPlanet — and the capability they represent existed for months*
*without existing on `master`. Every one of caner's carried the same sentence:*
*"Windows gameplay has not been rerun for this batch." Nobody could answer that,*
*because nothing ran the ports together. That is what this release is.*

**The nineteen**

- [@canersaka](https://github.com/canersaka) (#152–#161): lifted SPU threads in
  per-thread architectural contexts with lv2 copy semantics for thread arguments;
  lv2 condition signals and static guest mutexes; cellAudio ring indices and
  block tags; cellSysutil dialog completions from the guest callback poll;
  sceNpTrophy first-use slots; cellGame content-volume space; cellSaveData
  callback ABI; **RSX full FIFO driver methods, serialized callbacks and MRT
  exports**; **macOS native host, Metal rendering and game runner**; sceNp local
  score while offline.
- [@sagemono](https://github.com/sagemono) (#162–#170): `cellGameContentErrorDialog`;
  **cellGame content root** — the host directory `/dev_hdd0/game` maps to must be
  the one `ppu_fs` resolves it to, or a title writes its save data to one
  directory, reads it from another, and reports the data *corrupt* rather than
  missing; `bi $rN` classified by basic block rather than by the lifted function,
  because the lifter splits straight-line runs into functions that chain by
  fallthrough and a split is not a control-flow edge; `hbra`/`hbrr` decoded as
  the 7-bit opcodes they are; `--extra-funcs` passthrough for images entered by
  an interrupt vector, which `--auto-functions` cannot see; EDAT license types 1
  and 2 with the missing HMAC-SHA1 hash modes; list-DMA issue tracing.

**What unifying them found**

- **`cellGameSetParamString` was missing.** Sage's cellGame work makes Twisted
  Metal reach `cellGameCreateGameData` for the first time, and that path calls a
  function nothing implemented. Exactly the kind of gap a port owner cannot see
  from inside their own title — which is the argument for unifying, stated as a
  bug.
- **One fix was found twice.** Sage's `hbra`/`hbrr` decode is byte-identical to
  one already in the tree, reached independently from a different witness.

**The gate had to be rebuilt before it could say anything**

Four things it could not do, each discovered by it letting something through:

- **Occurrence counts never reached a golden.** `ps3_ms_dump()` ran only at
  `atexit()`, and every gated port is *killed* on its timeout — so `DROPPED`, the
  rule whose own docstring reads *"Rendering stopping looks like this"*, had
  nothing on either side to compare. Counts now checkpoint on a timer.
- **It never measured pixels.** `--shots` runs each port in the configuration its
  README documents and compares against the port's own reference screenshot.
  Frames are judged by distinct colour count, never brightness: a cleared buffer
  scores 100% non-black while showing nothing.
- **`PS3_VFS_ROOT` was doubled for two ports.** The guest path is appended whole,
  so a root of `vfs/PS3_GAME/USRDIR` produced
  `vfs/PS3_GAME/USRDIR/PS3_GAME/USRDIR/…`. VF5 spun on **686,996 failed opens**
  and looked like a dead showcase title; Simpsons opened zero files and drew
  9,391 groups of nothing, which looked like a broken renderer. Both render.
  Their goldens were invalid rather than stale and are re-recorded.
- **A run straight after a build is not a measurement.** Cold page cache shifts
  guest timing enough to flip race-sensitive titles; that run is now a discarded
  warm-up, and a red is confirmed by a second run. Of five reds investigated in
  one session, four were measurement artefacts and one was real — and the real
  one came from an `UNRESOLVED` NID, a fact about the binary rather than about a
  run.

`rubberducky` leaves the gate: a Sony SDK sample recompiled from a debug fSELF
with its symbol map exposed is excellent ground truth for the lifter and says
nothing about whether a change is safe for retail titles. New:
[docs/REGRESSION_GATE.md](docs/REGRESSION_GATE.md).

### v0.11.0 — *"Correctness Pass"* (September 2026)

*Fourteen open pull requests land at once. Most of them are correctness fixes*
*found by driving real titles into real walls:* Tokyo Jungle *reaches its main*
*menu, and the bugs it exposed — a mutex that never ran its initialiser, a FIFO*
*resync that ate fences, two guest pointers dereferenced as host pointers — were*
*never title-specific.*

**Execution correctness**

- **`sys_ppu_thread_once` was unimplemented** and returned success *without
  running the initialiser* (#136). Anything waiting on state that a once-routine
  was supposed to publish waited forever. This was the keystone bug behind
  Tokyo Jungle's boot.
- **SPU `xswd` read the wrong words and stored the halves backwards** (#127) —
  small positive values became zero. **`GETLLAR` must snapshot the reservation
  from local store**, not re-read memory (#126).
- **VMX correctness batch folded back** from the port branches (#125):
  `vmsumshs`, `vsumsws`, `vmulesh` and five byte-order bugs.

**Guest pointers are not host pointers**

Two crashes, same root cause, both reachable from ordinary gameplay:

- **`cellSaveData` called the title's `funcStat` and `funcFile` as host function
  pointers.** They are guest OPDs, so this jumped into guest memory as code —
  every autosave died on it. Both now marshal through guest memory the way
  `cellSaveDataAutoLoad2` already did.
- **`cellSail` printed its `uri` and `path` arguments as host strings**, killing
  the process the moment a title opened a movie. Both are copied out of guest
  memory now, and the descriptor's long-unused `uri[512]` field is finally
  filled.

**Graphics and SPURS**

- **A FIFO resync must not swallow fences** (#137). Jumping `get` to `put` keeps
  the ring alive, but a `SET_REFERENCE` in the skipped range means the
  `cellGcmFinish` waiting on it never wakes. Losing draws costs a frame; losing
  a fence deadlocks the title. The skipped range is now swept first, and each
  resync site is named in the log.
- **SPURS carries the SPU's answer to a job query** in the completion event
  (#135), and **cancels only the finalised instance's queues**, plus a real
  `ShutdownJobChain` (#129).
- **`cellSail`**: a player handle is a guest pointer, not an index (#130), and
  async call completion is reported to the title's event handler (#133).

**Diagnostics and tooling**

- **A call graph over the lifted output** (#131), so reachability is a query
  rather than a grep.
- **Watched stores name the guest function** that made them (#132, #134), and
  condvar probes tell a lost signal from an unsatisfied predicate (#128).

**Documentation, honestly**

- Removed three documents that were surveys of a private game library, one of
  which described obtaining DRM-stripped EBOOTs. The harness no longer defaults
  to a personal path (#138).
- **The starter template could not link.** It declared a `ps3::memory` /
  `ps3::thread` / `ps3::modules` API that does not exist, and since the porting
  guide tells you to copy that template, the documented path could only ever
  produce undefined references (#114, #89). Rewritten against the real runtime
  API, and its CMake now compiles the per-game PPU scaffold and sets `/bigobj`.
- **The renderer is documented at last** — which backend each platform gets,
  that there is no CMake switch for it, and that `RSX_LIVE_DRAW` is *not*
  required to get a picture.
- `SPU_FALLBACK.md` claimed ps3recomp cannot execute SPU code, which stopped
  being true some time ago.

**CI**

- `cellSync.c` called `getenv` with no `<stdlib.h>` — invisible on Windows,
  fatal on POSIX, and one reason every Linux job had been red.
- The SPU helper suite had a broken `xswd` case that built asymmetric vectors
  with a helper documented as only safe for symmetric ones. **Windows CI was
  never running that suite at all**; it does now.

> **Known issue:** the PPU scaffold ratchet still fails on Linux and macOS
> (`ppu_loader.cpp`, 0 → 110 on gcc). It predates this release and is tracked in
> #139. Windows CI is green.

### v0.10.0 — *"Three Platforms"* (September 2026)

*Windows, macOS and Linux all build the runtime and pass the suites. A second*
*draw engine lands beside the first — [@canersaka](https://github.com/canersaka)'s,*
*brought across from his Yakuza port — the RSX backend finally gets a real*
*abstraction instead of one D3D12 file, and* You Don't Know Jack *reaches*
*gameplay.*

**A second draw engine**

- **The live NV4097 → D3D12 draw engine** — executes the guest's command stream
  as it arrives rather than replaying a recorded frame. Brought across from the
  [Yakuza: Dead Souls EX](https://github.com/canersaka/Yakuza-Dead-Souls-EX)
  port: `rsx_live_draw.c`, `rsx_dispatch.*`, `rsx_vertex_compact.*`,
  `rsx_restart_cuts.h` and the VP/FP decompiler updates are
  *[@canersaka](https://github.com/canersaka)*'s work (#113). Opt-in behind
  `RSX_LIVE_DRAW`, so the existing backend path is unchanged when it is off.
  This sits *beside* [@sagemono](https://github.com/sagemono)'s draw engine from
  v0.7.0 — two independent renderers, not a replacement.
- What a second title needed on top: the **FIFO subchannel is a binding slot,
  not an engine selector** (a title binding NV4097 on subchannel 1 had every
  `SET_SHADER_PROGRAM` dropped, so every draw ran one constant-black fragment
  program); **HDR colour surfaces** (`F_W16Z16Y16X16` targets were created as
  `R8G8B8A8_UNORM`, clamping every value above 1.0 on write); and fragment-output
  NaN guards.

**Three platforms**

- **Linux bring-up** (#100) — CI, a headless RSX backend, and the PPU boot
  scaffold taken to zero errors on POSIX. The headless path is a host-memory
  framebuffer with a CPU triangle filler: enough to prove the guest's command
  stream produced the pixels it asked for on a runner with no display and no
  GPU. It is not a renderer, and the README says so.
- **The RSX backend gets an abstraction.** Texture pixel conversion (#101), the
  `TEXTURE_CONTROL1` crossbar decode (#102), texture layout, and one primitive
  truth table with a draw record that is not a D3D structure (#103) all lifted
  out of the D3D12 backend — so Metal and the headless backend read the same
  neutral values instead of reimplementing them.
- **CI samples a real guest texture with no GPU anywhere** (#106), and the
  frame clock starts on POSIX (#105).
- Portable plumbing throughout: one thread-local spelling reachable from the
  boot scaffold, host backtrace diagnostics on POSIX, Win32 types and
  interlocked ops on POSIX, and an in-tree Win32 `dirent.h`.

**Runtime correctness**

- **`CellSyncMutex` is a ticket lock, not a 0/1 flag** (#118) — the guest word
  is big-endian `m_freed`/`m_order` and free means `m_freed == m_order`. We
  stored it host-endian and CAS'd it from zero, so a mutex the SPU left free at
  `1/1` read as `0x01000100` and `TryLock` spun forever — 160 million times, in
  the title that found it.
- **`sys_mmapper_allocate_memory`** implemented (#112), **`cellSpurs`
  `CreateJobChain` + `KickJobChain`** actually start the chain (#98),
  **`cellDiscGameGetBootDiscInfo` takes one argument, not three** (#109),
  **`sys_event` yields before refusing a receive on an invalid queue** (#110),
  and **SPU MFC PUTs into the null page are rejected** (#107).
- **The guest bounds check is inlined** (#116), and a port can **choose the
  HLE-visible GCM window base** (#104).

**Diagnostics**

- **`PPU_WVAL`** watches PPU stores by value rather than by address (#108), the
  write watch **says when it stops printing** and lets the cap be raised (#111),
  the guard **prints the writer's live arguments** on every watched-line hit
  (#99), plus `PS3_HLE_ARGS`, `PS3_WAIT_OBJ` and a settable SPU atomic-trace cap.

**Ports**

- ***You Don't Know Jack* reaches gameplay** — boots, renders its Scaleform UI,
  navigates menus, loads an episode and puts answerable questions on screen.

### v0.9.1 — *"Downloads"* (August 2026)

*A packaging release. No runtime or lifter behaviour changes: the same tree as*
*v0.9.0, plus the CI and release plumbing that makes it downloadable.*

- **Releases now ship binaries.** A `v*` tag builds an SDK bundle per platform
  — the runtime library, its public headers, the full Python pipeline and the
  docs — and attaches it with checksums. Previously a release was source-only,
  so trying the project meant reproducing the whole toolchain setup first.
  `lib/` and `include/` come from the project's own install rules, so the
  bundle cannot drift from what a local `cmake --install` produces.
- The test suites **re-run in the release job**. A tag can point at any commit,
  including one that never went through CI; without this, "downloaded from a
  release" would mean less than "green on master".
- **Windows CI.** The macOS job guarded the POSIX paths, but nothing
  guarded the platform everything actually ships on — a change made to satisfy
  Clang could have broken the clang-cl build with nobody noticing until someone
  tried to build a port.
- Bundles are built for **Windows and macOS only**: those are the two platforms
  with CI proving they compile. Shipping an untested Linux binary would repeat
  the mistake this release series started by fixing.
- Docs now point at **[ReXGlue](https://github.com/rexglue/rexglue-sdk)** for the
  Xbox 360 side rather than a tool collection of our own.

### v0.9.0 — *"Second Platform"* (August 2026)

*The first tagged release, and the first that another platform can verify.*
*Two outside contributions from [@slushiimusic](https://github.com/slushiimusic)*
*gave the project CI and a second render backend; wiring the existing test*
*suites into that CI immediately found fifteen silent miscompiles.*

**macOS, and the project's first CI** (#94, #95, #96)
- `docs/BUILDING.md` had listed Apple Clang 14+ under "full support" while the
  tree did not compile there at all. Reported in #94, which was correct.
- **#95** makes the runtime library build on arm64 macOS and adds the first CI
  workflow this repository has ever had. It caught two of its own follow-ups
  within 17 seconds of landing.
- **#96** adds an RSX **Metal** backend (clear, flip, guest vertex fetch,
  pipeline-state cache, blend translation), a POSIX host that drives
  cellGcm → RSX → backend with **no lifted game**, and .app/dmg packaging.
- CI asserts the **presented pixel**, not the exit code: the guest clear colour
  through the FIFO, and an NV4097 triangle covering the centre pixel.
- macOS still **cannot run a recompiled game** — the PPU boot scaffold is
  Win32-only. See [platform support](#platform-support).

**Fifteen VMX miscompiles, found by running the tests somewhere new**
- The conformance suite only ever compiled its generated driver on Windows.
  Everywhere else it printed `COMPILE FAILED` and executed nothing — and a
  missing compiler exited **0**, so a skip read as a pass.
- With it actually running: `vupkhsb/vupklsb/vupkhsh/vupklsh` (12 cases) read
  and wrote the big-endian vector file through host `int16_t*`/`int32_t*`,
  byte-reversing every result element. `vcmpgtsw`/`vcmpgtsh` compared
  byte-reversed values, so a mixed-sign vector produced an all-zero mask.
  `vadduwm` propagated carries the wrong way and spilled into neighbouring
  elements. The byte-width members of each family passed by luck.
- `vadduhm`, `vsubuhm`, `vsubuwm` had the same latent bug with no test
  covering them; fixed together. 1311 checks, 0 failures.
- `mtfsb0`/`mtfsb1`/`mtfsfi` reached the unhandled catch-all. `mfspr VRSAVE`
  was a bare no-op that left the destination register **stale**.

**Runtime fixes found while driving two titles**
- **The GCM label/control/offset block lived at a hardcoded `0x03000000`** —
  inside main memory, where a title's own allocator hands out blocks. flOw's
  heap swallowed it: the game memset over the control register, the FIFO drain
  read ASCII as `put` and discarded whole frames. Moved to
  `VM_HLE_INJECT_BASE`, with every user deriving from it.
- **Malformed MFC transfers were reported and then performed anyway.** Real
  hardware raises an alignment exception and moves nothing. YDKJ's FMOD mixer
  issues eight 16 KB PUTs with a 4-byte-misaligned EA marching through its own
  loaded image, which zeroed the title's OPD table; the boot now survives.
- **The host-RVA → guest-function mapper invented frames.** "Largest host
  pointer ≤ target" with no upper bound attributes any runtime or CRT address
  to whichever lifted function sits below it. It cost real debugging time on
  both titles. Now bounded, and it prints the raw RVA rather than guessing.
- **Two-level jump tables** (`lwz r30,d1(r2); lwz r11,d2(r30)`) were rejected
  outright, including the common case where the base is loaded in the prologue
  hundreds of instructions earlier.
- The VFS now tolerates a flattened extracted tree (no `PS3_GAME` level).

*Earlier versions in this changelog predate tagging. v0.9.0 was the first
tagged release; v0.9.1 is the first with downloadable artifacts.*

### v0.8.0 — *"Ground Truth"* (August 2026)
<!-- TODO(maintainer): codename is a suggestion -- rename freely. -->

*A correctness release about the guest/host boundary, and about instrumentation
that can support its own claims. Two batches: `#92` — the guest-pointer sweep,
a SPURS teardown fix, and making the logging stop changing what titles do — and
everything folded via `integrate/fold-2026-08-14`,
[@sagemono](https://github.com/sagemono)'s guest-ABI HLE batch and SPU-lifter
faithful adoption plus [@canersaka](https://github.com/canersaka)'s
guest-callback/lv2/ABI batch.*

**The guest-pointer sweep — `libs/` is clean** (#92)
- **304 → 0 candidates.** HLE entry points receive pointer arguments as raw
  32-bit guest addresses, so dereferencing one reaches whatever host address
  happens to share that number. The sweep found it was really *three* problems:
  the address; the byte order (a scalar the guest reads back needs `vm_write*`,
  not a bare store); and **layout** — a struct holding a pointer is 4 bytes per
  field to the guest and 8 to us, so every field after it shifts, and a `u64`
  cannot be moved word-at-a-time. Only the first is a translation.
- `cellFont` is the case that shows why: `CellFontRenderSurface` and the glyph
  structs lead with a host pointer, so casting a guest EA to them misplaced
  *every* field and stored floats little-endian — a title reads a glyph advance
  of `4.6e-41` and lays every string on top of itself.
- **`cellGcmGetOffsetTable` handed the title two truncated host pointers** through
  a struct whose guest form is two 4-byte fields. The host tables were always
  correct; the bug was on the way out.
- **`tools/audit_guest_ptrs.py`** finds the bug class, **`tools/audit_entry_calls.py`**
  finds a library calling its own entry points (which double-translates — it
  caught two live ones), and **`tools/test_audit_guest_ptrs.py`** pins the audit in
  both directions, because the heuristic silently stopped matching three times
  during the sweep and a clean report meant nothing until it was checked.
- `libs/guest_struct.h` documents when the easy helpers do *not* apply.

**Runtime**
- **`sys_event`: release queue waiters when SPURS finalizes.** A title's worker
  waits on the SPURS completion queue with an infinite timeout and only re-tests
  its quit flag when a completion wakes it — but `cellSpursFinalize` has already
  stopped the job chains, so none can arrive. The shutdown that called finalize
  then blocks in `sys_ppu_thread_join` forever. Cancelling the attached queues
  returns `CELL_ECANCELED` to the waiters, which is the path titles already
  handle. Lifted again on re-attach, so teardown-then-setup works.
- **Logging no longer changes what a title does.** `ps3_log_verbose()` treated a
  non-tty stderr as "be verbose" with no way to say otherwise, so *capturing a
  log turned the firehose on*; and the SPURS job path wrote two lines with an
  explicit `fflush` per job — ~6k writes/s through one `FILE` lock, enough to
  starve a guest thread for seconds. Tokyo Jungle presents four frames in 40 s
  with the log discarded and one with it redirected to a file. `PS3_VERBOSE=0`
  now decides, and the per-job pair is verbose-only.
- **A wedged thread names itself.** `g_hle_inflight[tid]` and `g_sc_inflight[tid]`
  record the HLE call and lv2 syscall each guest thread is *inside*. The
  watchdog's `CTR` is the last `bctrl` target and goes stale the moment a call
  returns — it named `sys_lwmutex_unlock` for a thread that had long since left
  it. The write-watch also covers `ppu_memory.h`'s inline `vm_write*` now, so
  "nothing writes this address" is a claim it can actually support.
- clang-cl build repaired, two missed guest-call sites, and the dropped SPU park.

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
