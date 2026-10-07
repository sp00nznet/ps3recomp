# Multicore conformance suite

Differential test of SPU thread groups and PPU<->SPU interaction: RPCS3
(headless, same oracle config as `../ppu`) against ps3recomp, on the same ELF.

```
python3 tests/conformance/mc/run_mc_conform.py --work /tmp/mcconf
python3 tests/conformance/mc/run_mc_conform.py --work /tmp/mcconf --skip-oracle   # reuse oracle.txt
```

`gen_mc_conform.py` writes a bare lv2 ELF (raw syscalls, no imports) whose PPU
side creates thread groups from three SPU images assembled by `spu_asm.py`:

- **dma**: GET / PUT of 20 sizes and alignments, plus a GETL list; data dumped.
- **atomic**: 6 SPUs and the PPU thread each add 1 to shared words 400 times
  through GETLLAR/PUTLLC and lwarx/stwcx. on one 128-byte line; final counts
  dumped. Each SPU exits with `0xA000 | index` (sys_spu_thread_exit: status
  in SPU_WrOutMbox, then `stop 0x102`).
- **mbox**: the PPU writes four inbound-mailbox words and both signal
  notification registers; the SPU sums them and exits with the sum.
- **event**: the SPU sends four `sys_spu_thread_send_event` events (each
  answered in its inbound mailbox) and one `throw_event` to a PPU event queue;
  the PPU prints each event as `sys_event_queue_receive` returns it in r4..r7
  (the SPU thread id relative to the real one, since ids differ).
- **PPU threads + sync**: four threads increment a counter under a
  `sys_mutex`, yielding inside the critical section, then post a semaphore;
  a `sys_cond` ping-pong with a worker writes an alternating log; an event
  flag AND-wait with clear wakes only once both bits are set. Raw syscalls 52
  / 53 create and start the threads; they exit through syscall 41.
- **recv**: the PPU sends three events through a local event port to an SPU
  queue bound to the thread (`sys_spu_thread_bind_queue`); the SPU receives
  them with `sys_spu_thread_receive_event` (spuq in SPU_WrOutMbox, `stop
  0x110`, then status and data1-3 from the inbound mailbox). The group's
  RUN-event queue reports the start.
- **mfc2**: atomic status after GETLLAR (4), PUTLLC held (0), PUTLLC with no
  reservation (1), PUTLLUC (2); immediate tag status; a GETL whose middle
  element has stall-and-notify (RdListStallStat, WrListStallAck).

The transcript also carries every join cause/status and per-thread exit status.
The runner requires the MCCONF BEGIN..END blocks to be identical.

ps3recomp runs the SPU images on its interpreter as resident, concurrent
threads with blocking channel reads (`RD_SPU_INTERP=1 RD_SPU_INTERP_ASYNC=1
SPU_CH_BLOCK=1`). Without those, an SPU image with no lifted registration does
not run at all and its group "completes" with status 0.

SPU thread arguments follow LV2/RPCS3: each u64 in the register's preferred
doubleword (`v128::from64(0, arg)`), so the programs `rotqbyi 4` to reach the
low word.

## SPURS (`--suite spurs`)

`gen_spurs_conform.py` builds an executable with real firmware imports
(`lv2_imports.py`: sys_process_param, sys_proc_prx_param, .lib.stub entries and
SDK-shaped 0x20-byte call trampolines). On RPCS3 the imports bind to the
firmware's own `libsre.sprx` (RPCS3 runs libsre LLE by default), so the oracle
is Sony's SPURS kernel; ps3recomp answers with its HLE `libs/spurs`.

- **custom workload**: SPURS on 2 SPUs runs a hand-written policy module (the
  path WWS-style job managers take) with readyCount 1. Each dispatch does one
  capped GETLLAR/PUTLLC increment and returns to the kernel through r0; the
  PPU waits for the cap, shuts the workload down, waits, removes it and
  finalizes. Every return code is printed, with the module's entry r4
  (workload data) and r5 (poll status).

- **taskset**: four SPU tasks (ELF at LS 0x3000) each add {index + 1, 1} to a
  shared line atomically and exit through the taskset syscall (LS 0x27C4,
  r3 = 0). The PPU waits for all four (a shutdown request discards tasks that
  have not started), shuts the taskset down and joins it.

- **event flag**: an ANY2ANY, auto-clear SPURS event flag driven from the PPU:
  attach an lv2 queue, set / OR-wait / AND-wait / clear, then a blocking wait
  satisfied by a second PPU thread's `cellSpursEventFlagSet`; detach.

ps3recomp runs unlifted policy modules and tasks on its SPU interpreter.
Not covered yet: the SPU side of event flags and LF queues, and job chains
(their SPU halves are SDK library code a test would have to reproduce).

```
python3 tests/conformance/mc/run_mc_conform.py --suite spurs --work /tmp/spursconf
```
