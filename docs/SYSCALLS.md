# LV2 Kernel Syscall Reference

Complete documentation for all implemented PS3 LV2 (Lv-2 kernel) system calls in ps3recomp.

---

## Table of Contents

1. [Overview](#overview)
2. [Calling Convention](#calling-convention)
3. [PPU Thread Management](#ppu-thread-management)
4. [Mutex (sys_mutex)](#mutex-sys_mutex)
5. [Condition Variables (sys_cond)](#condition-variables-sys_cond)
6. [Semaphores (sys_semaphore)](#semaphores-sys_semaphore)
7. [Read-Write Locks (sys_rwlock)](#read-write-locks-sys_rwlock)
8. [Lightweight Mutex/Condvar (sys_lwmutex, sys_lwcond)](#lightweight-mutexcondvar)
9. [Event Queues and Ports (sys_event)](#event-queues-and-ports-sys_event)
10. [Event Flags (sys_event_flag)](#event-flags-sys_event_flag)
11. [Timers (sys_timer)](#timers-sys_timer)
12. [Memory Management (sys_memory)](#memory-management-sys_memory)
13. [Memory Mapper (sys_mmapper)](#memory-mapper-sys_mmapper)
14. [Filesystem (sys_fs)](#filesystem-sys_fs)
15. [Interrupts (sys_interrupt)](#interrupts-sys_interrupt)
16. [SPU Management](#spu-management)
17. [PRX Module Management](#prx-module-management)

---

## Overview

The LV2 kernel provides system services to PS3 games through the `sc` (System Call) instruction. ps3recomp implements these syscalls as native host OS calls, translating PS3 semantics to Windows (Win32 API) or POSIX (pthread, mmap) equivalents.

**Implementation files:** `runtime/syscalls/sys_*.c` and `runtime/syscalls/sys_*.h`

### Platform Mapping

| PS3 Concept | Windows Implementation | POSIX Implementation |
|---|---|---|
| PPU Threads | `CreateThread` | `pthread_create` |
| Mutexes | `CRITICAL_SECTION` | `pthread_mutex` |
| Condition Variables | `CONDITION_VARIABLE` | `pthread_cond` |
| Semaphores | `CreateSemaphoreW` | POSIX `sem_t` |
| Read-Write Locks | `SRWLOCK` | `pthread_rwlock` |
| Event Flags | Manual (bitmask + CV) | Manual (bitmask + CV) |
| Timers | `QueryPerformanceCounter` | `clock_gettime` |
| Memory | `VirtualAlloc` | `mmap` |
| File I/O | Win32 file API | POSIX file API |

---

## Calling Convention

### PS3 Side (PowerPC)

```
sc          ; Triggers syscall
; r11 = syscall number
; r3–r10 = up to 8 arguments
; Return value placed in r3
```

### Host Side (Recompiled)

When the recompiler encounters `sc`, it emits a call to `lv2_syscall(ctx)`:

```c
void lv2_syscall(ppu_context* ctx)
{
    uint32_t num = (uint32_t)ctx->gpr[11];
    lv2_syscall_fn handler = g_lv2_syscalls.handlers[num];
    ctx->gpr[3] = (uint64_t)handler(ctx);
}
```

Each handler extracts its arguments from the context:

```c
int64_t sys_ppu_thread_create(ppu_context* ctx)
{
    uint32_t tid_ptr    = LV2_ARG_PTR(ctx, 0);  // r3
    uint32_t entry_addr = LV2_ARG_U32(ctx, 1);  // r4
    uint64_t arg        = LV2_ARG_U64(ctx, 2);  // r5
    int32_t  priority   = LV2_ARG_S32(ctx, 3);  // r6
    uint32_t stack_size = LV2_ARG_U32(ctx, 4);  // r7
    uint64_t flags      = LV2_ARG_U64(ctx, 5);  // r8
    uint32_t name_ptr   = LV2_ARG_PTR(ctx, 6);  // r9
    // ...
}
```

---

## PPU Thread Management

**File:** `runtime/syscalls/sys_ppu_thread.c`
**Syscall numbers:** 41–49, 56

### Implementation Strategy

PS3 PPU threads map 1:1 to host OS threads. Each thread gets its own `ppu_context`, a 1 MB stack in the guest VM, and a native thread handle.

### Syscalls

#### `sys_ppu_thread_create` (41)

Creates a new PPU thread with its own register context and stack.

**Arguments:**
- `r3`: Pointer to store thread ID (`sys_ppu_thread_t*`)
- `r4`: Entry point address (guest address of the function to call)
- `r5`: Argument passed to the entry function (placed in r3 of new thread)
- `r6`: Thread priority (0 = highest, 3071 = lowest)
- `r7`: Stack size in bytes (rounded up to 4 KB alignment)
- `r8`: Flags (e.g., `SYS_PPU_THREAD_CREATE_JOINABLE`)
- `r9`: Thread name string (guest address, may be NULL)

**Implementation:**
1. Allocates a new `ppu_context` from a static pool (max 64 threads)
2. Allocates a guest stack via `vm_stack_allocate()`
3. Sets up the new context: `r1` = stack top, `r3` = arg, `cia` = entry
4. Creates a host thread:
   - Windows: `CreateThread(NULL, 0, thread_wrapper, ctx, 0, NULL)`
   - POSIX: `pthread_create(&handle, NULL, thread_wrapper, ctx)`
5. Writes the thread ID to the output pointer

**Returns:** `CELL_OK` on success, `CELL_EAGAIN` if thread limit reached, `CELL_ENOMEM` if stack allocation fails.

#### `sys_ppu_thread_exit` (42)

Terminates the calling thread. The exit code is in `r3`.

#### `sys_ppu_thread_yield` (43)

Yields the current thread's time slice.
- Windows: `SwitchToThread()`
- POSIX: `sched_yield()`

#### `sys_ppu_thread_join` (44)

Blocks until the specified thread exits. Stores the exit code.

#### `sys_ppu_thread_detach` (45)

Detaches a thread so its resources are auto-cleaned on exit.

#### `sys_ppu_thread_get_join_state` (46)

Queries whether a thread is joinable or detached.

#### `sys_ppu_thread_set_priority` (47) / `get_priority` (48)

Get/set thread priority. ps3recomp maps PS3 priority levels to host priority levels as best it can (the mapping is approximate since host schedulers differ).

#### `sys_ppu_thread_get_stack_information` (49)

Returns the stack base address and size for a thread.

#### `sys_ppu_thread_rename` (56)

Sets the thread's debug name.
- Windows: `SetThreadDescription()` (Win10+)
- POSIX: `pthread_setname_np()`

---

## Mutex (sys_mutex)

**File:** `runtime/syscalls/sys_mutex.c`
**Syscall numbers:** 100–104

### Implementation

Mutexes are backed by `CRITICAL_SECTION` (Windows) or `pthread_mutex_t` (POSIX). Each mutex handle maps to an entry in a static table.

**Features:**
- Recursive locking (configurable at creation)
- Non-recursive deadlock detection (returns `CELL_EDEADLK`)
- Timed lock with timeout (emulated via trylock + sleep loop)
- Owner tracking for debug/error reporting

### Syscalls

| # | Name | Description |
|---|------|-------------|
| 100 | `sys_mutex_create` | Create mutex. Args: attributes (recursive, priority), protocol (FIFO/priority). Returns handle. |
| 101 | `sys_mutex_destroy` | Destroy mutex. Fails if currently locked. |
| 102 | `sys_mutex_lock` | Lock mutex. Blocks until acquired. Recursive if configured. |
| 103 | `sys_mutex_trylock` | Try to lock without blocking. Returns `CELL_EBUSY` if locked by another thread. |
| 104 | `sys_mutex_unlock` | Unlock mutex. Returns `CELL_EMUTEX_UNLOCK_NOT_OWNED` if caller doesn't own it. |

---

## Condition Variables (sys_cond)

**File:** `runtime/syscalls/sys_cond.c`
**Syscall numbers:** 105–110

### Implementation

Backed by `CONDITION_VARIABLE` (Windows) or `pthread_cond_t` (POSIX). Each condvar is associated with a specific mutex.

### Syscalls

| # | Name | Description |
|---|------|-------------|
| 105 | `sys_cond_create` | Create condvar associated with a mutex handle. |
| 106 | `sys_cond_destroy` | Destroy condvar. |
| 107 | `sys_cond_wait` | Atomically unlock mutex and wait for signal. Supports timeout. |
| 108 | `sys_cond_signal` | Wake one waiting thread. |
| 109 | `sys_cond_signal_all` | Wake all waiting threads (broadcast). |
| 110 | `sys_cond_signal_to` | Wake a specific thread (by thread ID). |

---

## Semaphores (sys_semaphore)

**File:** `runtime/syscalls/sys_semaphore.c`
**Syscall numbers:** 90–94, 114

### Implementation

- Windows: `CreateSemaphoreW` / `WaitForSingleObject` / `ReleaseSemaphore`
- POSIX: `sem_init` / `sem_wait` / `sem_post`

### Syscalls

| # | Name | Description |
|---|------|-------------|
| 90 | `sys_semaphore_create` | Create with initial/max count. |
| 91 | `sys_semaphore_destroy` | Destroy semaphore. |
| 92 | `sys_semaphore_wait` | Decrement (block if zero). Supports timeout. |
| 93 | `sys_semaphore_trywait` | Non-blocking decrement. Returns `CELL_EBUSY` if zero. |
| 94 | `sys_semaphore_post` | Increment by given count (wakes up to N waiters). |
| 114 | `sys_semaphore_get_value` | Read current count. |

---

## Read-Write Locks (sys_rwlock)

**File:** `runtime/syscalls/sys_rwlock.c`
**Syscall numbers:** 120–127

### Implementation

- Windows: `SRWLOCK` (Slim Reader/Writer Lock — non-recursive, very fast)
- POSIX: `pthread_rwlock_t`

Multiple readers can hold the lock simultaneously. Writers get exclusive access.

### Syscalls

| # | Name | Description |
|---|------|-------------|
| 120 | `sys_rwlock_create` | Create read-write lock. |
| 121 | `sys_rwlock_destroy` | Destroy. |
| 122 | `sys_rwlock_rlock` | Acquire shared (read) lock. |
| 123 | `sys_rwlock_tryrlock` | Try shared lock without blocking. |
| 124 | `sys_rwlock_runlock` | Release shared lock. |
| 125 | `sys_rwlock_wlock` | Acquire exclusive (write) lock. |
| 126 | `sys_rwlock_trywlock` | Try exclusive lock without blocking. |
| 127 | `sys_rwlock_wunlock` | Release exclusive lock. |

---

## Lightweight Mutex/Condvar

**File:** `runtime/syscalls/sys_ppu_thread.c` (part of sysPrxForUser)
**Syscall numbers:** 150–160

Lightweight mutexes (`sys_lwmutex`) and condvars (`sys_lwcond`) are the user-space fast path for synchronization. They avoid kernel transitions in the uncontested case.

In ps3recomp, they use the same backing as regular mutexes (`CRITICAL_SECTION`/`pthread_mutex`) since there's no kernel/user boundary.

### Syscalls

| # | Name | Description |
|---|------|-------------|
| 150 | `sys_lwmutex_create` | Create lightweight mutex. |
| 151 | `sys_lwmutex_destroy` | Destroy. |
| 152 | `sys_lwmutex_lock` | Lock. |
| 153 | `sys_lwmutex_trylock` | Try lock. |
| 154 | `sys_lwmutex_unlock` | Unlock. |
| 155 | `sys_lwcond_create` | Create lightweight condvar. |
| 156 | `sys_lwcond_destroy` | Destroy. |
| 157 | `sys_lwcond_wait` | Wait. |
| 158 | `sys_lwcond_signal` | Signal one. |
| 159 | `sys_lwcond_signal_all` | Signal all. |
| 160 | `sys_lwcond_signal_to` | Signal specific thread. |

---

## Event Queues and Ports (sys_event)

**File:** `runtime/syscalls/sys_event.c`
**Syscall numbers:** 128–138

### Event Queue

A **circular buffer** of events (default 256 slots). Threads can block waiting for events, and producers can send events through connected ports.

**Event structure:**
```c
struct sys_event {
    uint64_t source;    // Event source ID
    uint64_t data1;     // User data 1
    uint64_t data2;     // User data 2
    uint64_t data3;     // User data 3
};
```

### Event Port

A **producer handle** that connects to a queue. Sending an event through a port enqueues it in the connected queue.

### Syscalls

| # | Name | Description |
|---|------|-------------|
| 128 | `sys_event_queue_create` | Create queue with given depth and key. |
| 129 | `sys_event_queue_destroy` | Destroy queue (must be empty). |
| 130 | `sys_event_queue_receive` | Block until an event arrives. Supports timeout. |
| 131 | `sys_event_queue_tryreceive` | Non-blocking receive. |
| 133 | `sys_event_queue_drain` | Discard all pending events. |
| 134 | `sys_event_port_create` | Create a port. |
| 135 | `sys_event_port_destroy` | Destroy a port. |
| 136 | `sys_event_port_connect_local` | Connect port to a queue. |
| 137 | `sys_event_port_disconnect` | Disconnect port from queue. |
| 138 | `sys_event_port_send` | Send event through port to connected queue. |

---

## Event Flags (sys_event_flag)

**File:** `runtime/syscalls/sys_event.c`
**Syscall numbers:** 139–146

### Design

A 64-bit bitmask that threads can wait on. Supports AND/OR wait modes:
- **OR mode**: Wake when *any* of the specified bits are set
- **AND mode**: Wake when *all* of the specified bits are set

### Syscalls

| # | Name | Description |
|---|------|-------------|
| 139 | `sys_event_flag_create` | Create with initial value and wait mode. |
| 140 | `sys_event_flag_destroy` | Destroy. |
| 141 | `sys_event_flag_wait` | Block until condition met. Supports timeout. |
| 142 | `sys_event_flag_trywait` | Non-blocking wait. |
| 143 | `sys_event_flag_set` | Set bits (OR them into the current value). |
| 144 | `sys_event_flag_clear` | Clear bits (AND with complement). |
| 145 | `sys_event_flag_cancel` | Cancel all waiting threads. |
| 146 | `sys_event_flag_get` | Read current flag value. |

---

## Timers (sys_timer)

**File:** `runtime/syscalls/sys_timer.c`
**Syscall numbers:** 70–76, 141–142, 145, 147

### Time Model

PS3 time is measured in **timebase ticks** at 79,800,000 Hz (79.8 MHz). This is the Cell processor's timebase frequency.

```c
#define PS3_TIMEBASE_FREQUENCY  79800000ULL
```

**Platform time sources:**
- Windows: `QueryPerformanceCounter` / `QueryPerformanceFrequency`
- POSIX: `clock_gettime(CLOCK_MONOTONIC)` / `CLOCK_REALTIME`

### Syscalls

| # | Name | Description |
|---|------|-------------|
| 70 | `sys_timer_create` | Create a timer object. |
| 71 | `sys_timer_destroy` | Destroy timer. |
| 72 | `sys_timer_get_information` | Get timer state (period, next fire time). |
| 73 | `sys_timer_start` | Start periodic timer (fires events to a connected queue). |
| 74 | `sys_timer_stop` | Stop timer. |
| 75 | `sys_timer_connect_event_queue` | Connect timer to an event queue. |
| 76 | `sys_timer_disconnect_event_queue` | Disconnect timer from queue. |
| 141 | `sys_timer_usleep` | Sleep for N microseconds. |
| 142 | `sys_timer_sleep` | Sleep for N seconds. |
| 145 | `sys_time_get_current_time` | Get current time (seconds + nanoseconds since epoch). |
| 147 | `sys_time_get_timebase_frequency` | Returns 79,800,000. |

### High-Resolution Timing

For `sys_time_get_current_time`:
- Windows: `QueryPerformanceCounter` → convert to seconds/nanoseconds
- POSIX: `clock_gettime(CLOCK_REALTIME, &ts)`

---

## Memory Management (sys_memory)

**File:** `runtime/syscalls/sys_memory.c`
**Syscall numbers:** 348–355, 358

### Implementation

A **bump allocator** within the guest VM's main memory region. Allocations are aligned to 1 MB boundaries (PS3 requirement). Memory containers provide isolated allocation pools.

### Syscalls

| # | Name | Description |
|---|------|-------------|
| 348 | `sys_memory_allocate` | Allocate from main memory. Args: size, flags (1MB/64KB page size). |
| 349 | `sys_memory_free` | Free a previous allocation. |
| 350 | `sys_memory_allocate_from_container` | Allocate from a specific container. |
| 352 | `sys_memory_get_user_memory_size` | Get total/available memory (256 MB total). |
| 353 | `sys_memory_container_create` | Create a memory container with a size budget. |
| 354 | `sys_memory_container_destroy` | Destroy container. |
| 355 | `sys_memory_container_get_size` | Query container's total/used size. |
| 358 | `sys_memory_get_page_attribute` | Get page attributes (size, protection). |

### Memory Containers

A container is a logical partition of memory with a fixed budget. Up to 128 containers can exist. Allocations from a container are tracked against its budget.

---

## Memory Mapper (sys_mmapper)

**File:** `runtime/syscalls/sys_memory.c`
**Syscall numbers:** 330–337

### Implementation

The mmapper provides shared memory regions that can be mapped at specific guest addresses.

### Syscalls

| # | Name | Description |
|---|------|-------------|
| 330 | `sys_mmapper_allocate_address` | Reserve an address range in the guest space. |
| 331 | `sys_mmapper_free_address` | Release a reserved address range. |
| 332 | `sys_mmapper_allocate_shared_memory` | Create a shared memory object. |
| 333 | `sys_mmapper_free_shared_memory` | Destroy shared memory. |
| 334 | `sys_mmapper_map_shared_memory` | Map shared memory at a specific guest address. |
| 335 | `sys_mmapper_unmap_shared_memory` | Unmap shared memory. |
| 337 | `sys_mmapper_search_and_map` | Find free space and map shared memory. |

---

## Filesystem (sys_fs)

**File:** `runtime/syscalls/sys_fs.c`
**Syscall numbers:** 801–841

### Path Translation

PS3 paths start with device prefixes that are mapped to host directories:

| PS3 Path | Default Host Path | Description |
|---|---|---|
| `/dev_hdd0/` | `./hdd0/` | Internal hard drive |
| `/dev_flash/` | `./flash/` | System firmware files |
| `/dev_bdvd/` | `./disc/` | Blu-ray disc content |
| `/app_home/` | `./` | Application directory |
| `/dev_usb000/` | `./usb0/` | USB storage |
| `/host_root/` | `/` (or `C:\`) | Host filesystem root (debug only) |

### Syscalls

| # | Name | Description |
|---|------|-------------|
| 801 | `sys_fs_open` | Open file. Flags: `CELL_FS_O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_CREAT`, `O_TRUNC`, `O_APPEND`. |
| 802 | `sys_fs_read` | Read bytes from file descriptor. Returns bytes read. |
| 803 | `sys_fs_write` | Write bytes to file descriptor. Returns bytes written. |
| 804 | `sys_fs_close` | Close file descriptor. |
| 805 | `sys_fs_opendir` | Open directory for enumeration. |
| 806 | `sys_fs_readdir` | Read next directory entry. Returns name, type, and attributes. |
| 807 | `sys_fs_closedir` | Close directory handle. |
| 808 | `sys_fs_stat` | Get file/directory status (size, timestamps, mode). Output in big-endian. |
| 809 | `sys_fs_fstat` | Get status by file descriptor. |
| 811 | `sys_fs_mkdir` | Create directory. |
| 812 | `sys_fs_rename` | Rename file or directory. |
| 813 | `sys_fs_rmdir` | Remove empty directory. |
| 814 | `sys_fs_unlink` | Delete file. |
| 818 | `sys_fs_lseek` | Seek to position. Supports `SEEK_SET`, `SEEK_CUR`, `SEEK_END`. |
| 820 | `sys_fs_ftruncate` | Truncate file to specified size. |
| 840 | `sys_fs_fget_block_size` | Get filesystem block size for a file (returns 4096). |
| 841 | `sys_fs_get_block_size` | Get filesystem block size for a path. |

### File Descriptors

ps3recomp maintains its own file descriptor table mapping PS3 FDs to host file handles. Up to 256 concurrent file descriptors are supported.

---

## Interrupts (sys_interrupt)

**File:** `runtime/syscalls/sys_interrupt.c`

### Implementation

In a full emulator, interrupts would trigger SPU-to-PPU signaling and preemption. In ps3recomp's HLE model, interrupts are **tracked but not actively delivered** — the interrupt tag/thread management is maintained for API compatibility, but End-of-Interrupt (EOI) is a no-op.

---

## SPU Management

**Syscall numbers:** 156–157, 170–194, 229–251

### Status

SPU management syscalls are **defined but minimally implemented**. The full set of syscall numbers is registered for API compatibility:

- `sys_spu_thread_group_create/destroy/start/stop/join`
- `sys_spu_initialize`
- `sys_spu_thread_write_ls/read_ls/write_snr`
- `sys_spu_thread_bind_queue/unbind_queue`
- `sys_spu_image_open/close`

Most games use the higher-level `cellSpurs` framework instead of raw SPU syscalls. The SPURS HLE module in `libs/spurs/` provides the management layer.

---

## PRX Module Management

**Syscall numbers:** 480–494

### Implementation

PRX management is handled by the HLE module system rather than true LLE module loading. These syscalls integrate with `cellSysmodule`:

| # | Name | Description |
|---|------|-------------|
| 480 | `sys_prx_load_module` | Load a PRX file — resolves to `ps3_find_module()` |
| 481 | `sys_prx_start_module` | Start a loaded module — calls `ps3_module_load()` |
| 482 | `sys_prx_stop_module` | Stop a module — calls `ps3_module_unload()` |
| 483 | `sys_prx_unload_module` | Unload a module |
| 491 | `sys_prx_get_module_list` | List loaded modules |
| 492 | `sys_prx_get_module_info` | Query module info (name, version) |
| 493 | `sys_prx_get_module_id_by_name` | Find module ID by name |
