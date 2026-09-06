# SPU PPU Fallback API

> **This document predates the SPU lifter.** ps3recomp *does* execute SPU code
> now: `tools/spu_lifter.py` lifts SPU images to C, and `runtime/spu/` provides
> the local store, channels, DMA and per-image dispatch. See
> [SPU_LIFTER.md](SPU_LIFTER.md) for the current path, which is what new ports
> should use.
>
> The fallback API below is still present and still supported
> (`runtime/syscalls/spu_fallback.c`). It remains the right tool when you want a
> PPU-side implementation of a job instead of running the SPU code — a
> hand-written replacement for a decompressor or mixer, or a bring-up shim
> before an image is lifted.

Historically ps3recomp did not execute SPU code at all. For many games that was
fine — plenty of SPU jobs produce side effects that nothing actually depends on,
or the PPU code is happy when the group reports "all threads exited cleanly".

Some games need real SPU output: PhyreEngine asset decompressors, audio
mixers, particle simulations, physics. Stubbing those silently leaves
PPU code reading garbage.

The SPU PPU-fallback registry lets a per-game shim provide a PPU-side
implementation for any SPU job, keyed on the SPU image's entry point.
When the game starts a thread group, threads with a matching fallback
run on a host thread (real concurrency); `sys_spu_thread_group_join`
blocks until they're all done.

## API

`#include "ps3emu/spu_fallback.h"`

```c
typedef int32_t (*spu_ppu_fallback_fn)(uint32_t tid, uint32_t args_ea,
                                       uint32_t args_size, void* user);

int  spu_register_ppu_fallback(uint32_t entry_point,
                               spu_ppu_fallback_fn handler, void* user);
int  spu_unregister_ppu_fallback(uint32_t entry_point);
spu_ppu_fallback_fn spu_lookup_ppu_fallback(uint32_t entry_point,
                                            void** out_user);

/* Local store access (256 KB per thread, lazily allocated) */
uint8_t* spu_thread_get_local_store(uint32_t tid);
uint32_t spu_thread_local_store_size(void);
```

Handler args:
- `tid` — synthesized SPU thread id from `sys_spu_thread_initialize`
- `args_ea` — guest EA of the args block (set via `sys_spu_thread_set_argument`
  or the args parameter of `sys_spu_thread_initialize`)
- `args_size` — currently always 0 (size isn't part of the syscall API)
- `user` — opaque pointer registered alongside the handler

Return value becomes the SPU thread's exit status. The worst (most
negative) status across all threads in a group becomes the group's
exit status, reported back via `sys_spu_thread_group_join`.

## Lifecycle

Register at startup, before any SPU activity:

```c
static int32_t my_decompress_fallback(uint32_t tid, uint32_t args_ea,
                                      uint32_t args_size, void* user)
{
    /* args_ea points at a guest struct the SPU job would have processed.
     * Decode it via vm_read*; do the work on the host; write results
     * back via vm_write*. */
    uint32_t src_ea  = vm_read32(args_ea + 0);
    uint32_t dst_ea  = vm_read32(args_ea + 4);
    uint32_t src_len = vm_read32(args_ea + 8);
    /* ... read src bytes from vm_base + src_ea, decompress on host,
     * write to vm_base + dst_ea ... */
    return 0;  /* CELL_OK */
}

static void register_my_spu_fallbacks(void)
{
    /* Entry point comes from sys_spu_image_open: it parses the ELF and
     * writes the entry to image+4. The "[SPU] image_open" log line
     * shows it; you'll typically read it once with an instrumented run
     * and then hard-code it. */
    spu_register_ppu_fallback(0x000028F0, my_decompress_fallback, NULL);
}
```

Find the entry point via the `[SPU] image_open` log:

```
[SPU] image_open img=0x10001234 path='/dev_flash/sys/spu/decompress.elf' entry=0x000028F0
```

## Execution model

- Synchronous registration; not thread-safe. Call all
  `spu_register_ppu_fallback()` once at startup.
- Asynchronous execution. `sys_spu_thread_group_start` spawns one host
  thread per registered fallback (Win32 `CreateThread`, POSIX
  `pthread_create`). Threads without a fallback complete instantly with
  status 0.
- `sys_spu_thread_group_join` blocks on each running thread's completion
  event, then collects the worst exit status into the group state.
- `sys_spu_thread_get_exit_status` returns CELL_ESTAT (0x80010003) if the
  thread is still in flight — match Sony's documented behaviour.

## Local store

Each SPU thread has a virtual 256 KB local store, allocated lazily on
first `sys_spu_thread_write_ls` / `_read_ls` syscall (or on first
`spu_thread_get_local_store` call). PPU code uses the syscalls; the
fallback handler reaches the same buffer via `spu_thread_get_local_store(tid)`.

Typical pattern:

```c
/* PPU side, before group_start */
sys_spu_thread_write_ls(tid, /*offset*/ 0x100, /*value*/ src_ea, /*type*/ 4);
sys_spu_thread_write_ls(tid, /*offset*/ 0x104, /*value*/ dst_ea, /*type*/ 4);

/* Fallback handler */
static int32_t my_decompress_fallback(uint32_t tid, uint32_t args_ea,
                                      uint32_t args_size, void* user)
{
    uint8_t* ls = spu_thread_get_local_store(tid);
    uint32_t src_ea = (ls[0x100] << 24) | (ls[0x101] << 16) |
                      (ls[0x102] <<  8) |  ls[0x103];
    uint32_t dst_ea = (ls[0x104] << 24) | (ls[0x105] << 16) |
                      (ls[0x106] <<  8) |  ls[0x107];
    /* ... do the work, write completion flag back to LS ... */
    ls[0x200] = 1;
    return 0;
}

/* PPU side, after group_join */
uint64_t done = 0;
sys_spu_thread_read_ls(tid, /*offset*/ 0x200, &done, /*type*/ 1);
```

The buffer is freed when `sys_spu_thread_group_destroy` runs.

## Caveats

- The fallback runs on a host thread, not in the guest VM. It cannot
  call recompiled guest functions or take guest locks. It can read/write
  guest memory freely via `vm_read*` / `vm_write*` helpers.
- Be deterministic about output bytes — games may hash/checksum results.
- If you need to coordinate with PPU code that's running concurrently,
  use the existing host-side sync primitives (mutexes, atomics). Do
  *not* use the guest's lwmutex APIs from a fallback.
- The args_size parameter is currently always 0. If your job needs to
  know the descriptor size, encode it in the descriptor itself.

## Related

- `runtime/syscalls/lv2_register.c` — SPU group/thread state machine and
  the dispatch site in `sys_spu_thread_group_start_handler`.
- `include/ps3emu/spu_fallback.h` — public header.
- `runtime/syscalls/spu_fallback.c` — registry implementation.
