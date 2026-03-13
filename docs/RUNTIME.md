# ps3recomp Runtime Reference

Complete reference for the ps3recomp core runtime layer — the foundation that makes recompiled PS3 code run natively on any host platform.

---

## Table of Contents

1. [Overview](#overview)
2. [Virtual Memory Manager](#virtual-memory-manager)
3. [PPU Execution Context](#ppu-execution-context)
4. [SPU Execution Context](#spu-execution-context)
5. [Type System](#type-system)
6. [Endianness Layer](#endianness-layer)
7. [Syscall Dispatch](#syscall-dispatch)
8. [DMA Engine](#dma-engine)

---

## Overview

The runtime is the bridge between recompiled PS3 PowerPC code and the host operating system. It provides:

- A **4 GB virtual address space** mapped into host memory, mirroring the PS3's 32-bit effective addressing
- **PPU execution contexts** that model the full PowerPC register file (32 GPR + 32 FPR + 32 VMX/AltiVec VR)
- **SPU execution contexts** with 128 x 128-bit registers and 256 KB local store each
- **LV2 syscall dispatch** translating PS3 kernel calls into host OS equivalents
- **DMA engine** for SPU local store ↔ main memory transfers

All runtime components are header-only with `static inline` functions, meaning zero link-time overhead — the compiler inlines everything directly into recompiled code.

**Key files:**

| File | Purpose |
|------|---------|
| `runtime/memory/vm.h` | Virtual memory manager |
| `runtime/ppu/ppu_context.h` | PPU register file and helpers |
| `runtime/ppu/ppu_memory.h` | Memory access macros |
| `runtime/ppu/ppu_ops.h` | Instruction operation helpers |
| `runtime/spu/spu_context.h` | SPU register file and local store |
| `runtime/spu/spu_dma.h` | MFC DMA transfer engine |
| `runtime/syscalls/lv2_syscall_table.h` | Syscall numbers and dispatch |
| `runtime/syscalls/sys_*.c` / `sys_*.h` | Individual syscall implementations |

---

## Virtual Memory Manager

**File:** `runtime/memory/vm.h`

### Design

The PS3 uses 32-bit effective addresses despite having a 64-bit PPU. Games never exceed the 4 GB address space. ps3recomp exploits this by reserving a contiguous 4 GB region on the host and translating addresses with simple pointer arithmetic:

```c
host_ptr = vm_base + guest_addr
```

On Windows, `VirtualAlloc` with `MEM_RESERVE` reserves the 4 GB range without consuming physical memory. On POSIX, `mmap` with `MAP_NORESERVE | MAP_ANONYMOUS` does the same. Pages are committed on demand as regions are needed.

### Memory Map

| Guest Address Range | Size | Region | Committed At |
|---|---|---|---|
| `0x00010000 – 0x0FFFFFFF` | 256 MB | User ELF text + data (main memory) | `vm_init()` |
| `0x10000000 – 0x1FFFFFFF` | 256 MB | RSX mapped memory / PRX module space | On demand via `vm_commit()` |
| `0x30000000 – 0x3001FFFF` | 8 × 256 KB | SPU local store windows (raw SPU) | `vm_spu_ls_map()` |
| `0xD0000000 – 0xDFFFFFFF` | 256 MB | Stack region | `vm_init()` |

### Key Constants

```c
#define VM_TOTAL_SIZE       0x100000000ull  // 4 GB total address space
#define VM_MAIN_MEM_BASE    0x00010000u     // Main memory starts here
#define VM_MAIN_MEM_SIZE    0x10000000u     // 256 MB main memory
#define VM_RSX_BASE         0x10000000u     // RSX region start
#define VM_RSX_SIZE         0x10000000u     // 256 MB RSX
#define VM_SPU_BASE         0x30000000u     // SPU LS windows start
#define VM_SPU_WINDOW_SIZE  0x00040000u     // 256 KB per SPU
#define VM_STACK_BASE       0xD0000000u     // Stack region start
#define VM_STACK_REGION     0x10000000u     // 256 MB for stacks
#define VM_PPU_STACK_SIZE   0x00100000u     // 1 MB default per thread
#define VM_PAGE_SIZE        0x00001000u     // 4 KB pages
```

### API

#### `vm_init()`

Reserves the 4 GB address space and commits the main memory (256 MB) and stack (256 MB) regions. Zeros main memory. Returns `CELL_OK` on success, `CELL_ENOMEM` on failure.

**Platform behavior:**
- **Windows**: `VirtualAlloc(NULL, 4GB, MEM_RESERVE, PAGE_NOACCESS)` then `VirtualAlloc(region, size, MEM_COMMIT, PAGE_READWRITE)` for each active region
- **POSIX**: `mmap(NULL, 4GB, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0)` then `mprotect(region, size, PROT_READ|PROT_WRITE)`

#### `vm_shutdown()`

Releases the entire 4 GB mapping. All guest pointers become invalid.

#### `vm_commit(addr, size)`

Commits pages within the reserved range, making them readable and writable. Used to bring RSX memory, SPU windows, or dynamic allocations online. Size is rounded up to `VM_PAGE_SIZE` (4 KB).

#### `vm_protect(addr, size, read, write, exec)`

Changes page protection for a committed region. Used for guard pages, read-only data segments, and executable code regions.

#### `vm_to_host(addr)` / `vm_to_guest(host_ptr)`

Convert between PS3 guest addresses (32-bit) and host pointers. These are the fundamental translation primitives.

#### `vm_is_valid_addr(addr)`

Checks if a guest address falls within any committed memory region (main memory, stack, SPU LS, or RSX).

### Stack Allocator

The `vm_stack_alloc` structure provides a bump allocator for PPU thread stacks within the `0xD0000000` region:

```c
vm_stack_alloc sa;
vm_stack_alloc_init(&sa);

// Allocate a 1 MB stack with guard page
uint32_t stack_base = vm_stack_allocate(&sa, VM_PPU_STACK_SIZE);
// stack_base points past the guard page
// Usable stack top = stack_base + VM_PPU_STACK_SIZE
```

Each allocation includes a **guard page** (4 KB, `PAGE_NOACCESS`) at the bottom of the stack to detect overflow. The allocator returns the address *above* the guard page.

### SPU Local Store Mapping

For raw SPU access (PPU reading/writing SPU local store via memory-mapped IO), each SPU's 256 KB local store is mapped into the guest address space:

```c
vm_spu_ls_map(2);  // Commit SPU 2's LS window

// SPU 2's LS is now accessible at 0x30000000 + 2 * 0x40000 = 0x30080000
uint8_t* ls = vm_spu_ls_host_ptr(2);
```

---

## PPU Execution Context

**File:** `runtime/ppu/ppu_context.h`

### Register File

The `ppu_context` struct models the complete architectural state of a Cell PPU hardware thread:

```c
typedef struct ppu_context {
    uint64_t gpr[32];       // General-purpose registers r0–r31
    double   fpr[32];       // Floating-point registers f0–f31
    u128     vr[32];        // VMX/AltiVec vector registers vr0–vr31 (128-bit, 16-byte aligned)
    uint32_t cr;            // Condition register (8 × 4-bit fields)
    uint64_t lr;            // Link register (return address from bl/blr)
    uint64_t ctr;           // Count register (loop counter for bdnz)
    uint32_t xer;           // Fixed-point exception register (SO/OV/CA)
    uint32_t fpscr;         // Floating-point status and control register
    uint32_t vscr;          // Vector status and control register
    uint64_t cia;           // Current instruction address (program counter)
    uint64_t thread_id;     // Thread identification
    uint64_t reserve_addr;  // lwarx/stwcx reservation address
    uint64_t reserve_value; // lwarx/stwcx reservation value
    int      reserve_valid; // Reservation validity flag
} ppu_context;
```

### Register Roles (PPC64 ABI)

| Register | ABI Name | Purpose |
|----------|----------|---------|
| `r0` | — | Volatile, used by prologue/epilogue |
| `r1` | SP | Stack pointer (16-byte aligned, grows down) |
| `r2` | TOC | Table of Contents pointer (per-module global data) |
| `r3` | ARG0/RET | First argument / return value |
| `r4–r10` | ARG1–ARG7 | Arguments 2–8 |
| `r11` | — | Syscall number (for `sc` instruction) |
| `r12` | — | Indirect call target / scratch |
| `r13` | SDA | Small Data Area base pointer |
| `r14–r31` | — | Non-volatile (callee-saved) |
| `f0` | — | Volatile FP scratch |
| `f1–f13` | — | FP arguments / return |
| `f14–f31` | — | Non-volatile FP |
| `vr0–vr19` | — | Volatile VMX |
| `vr20–vr31` | — | Non-volatile VMX |

Convenience macros provide named access:

```c
PPU_SP(ctx)     // ctx->gpr[1]  — stack pointer
PPU_TOC(ctx)    // ctx->gpr[2]  — table of contents
PPU_ARG0(ctx)   // ctx->gpr[3]  — first argument
PPU_ARG1(ctx)   // ctx->gpr[4]  — second argument
// ...
PPU_ARG7(ctx)   // ctx->gpr[10] — eighth argument
PPU_RET(ctx)    // ctx->gpr[3]  — return value
```

### Condition Register

The 32-bit CR is divided into eight 4-bit fields (CR0–CR7). Each field contains:

| Bit | Name | Meaning |
|-----|------|---------|
| 3 (MSB) | LT | Result is negative / less than |
| 2 | GT | Result is positive / greater than |
| 1 | EQ | Result is zero / equal |
| 0 (LSB) | SO | Summary Overflow (copied from XER[SO]) |

**Helper functions:**

```c
ppu_cr_get(ctx, field)              // Read 4-bit CR field (0–7)
ppu_cr_set(ctx, field, value)       // Write 4-bit CR field
ppu_cr_set_s64(ctx, field, a, b)    // Set CR field from signed comparison
ppu_cr_set_u64(ctx, field, a, b)    // Set CR field from unsigned comparison
ppu_cr_set_s32(ctx, field, a, b)    // Set CR field from 32-bit signed comparison
ppu_cr_set_u32(ctx, field, a, b)    // Set CR field from 32-bit unsigned comparison
```

### XER (Fixed-Point Exception Register)

| Bit | Name | Meaning |
|-----|------|---------|
| 31 | SO | Summary Overflow — sticky, set when OV is set |
| 30 | OV | Overflow — set by arithmetic with "o" suffix (addo, subfo) |
| 29 | CA | Carry — set by add carrying, subtract from carrying |
| 0–6 | — | Byte count for lswx/stswx string instructions |

```c
ppu_xer_get_so(ctx)         // Read SO flag
ppu_xer_get_ov(ctx)         // Read OV flag
ppu_xer_get_ca(ctx)         // Read CA flag
ppu_xer_set_ca(ctx, ca)     // Set/clear CA
ppu_xer_set_ov(ctx, ov)     // Set/clear OV (also sets SO if ov=1)
ppu_xer_get_byte_count(ctx) // Read string instruction byte count
```

### VSCR (Vector Status and Control Register)

| Bit | Name | Meaning |
|-----|------|---------|
| 16 | NJ | Non-Java mode (flush denorms to zero) |
| 0 | SAT | Saturation (sticky, set by saturating vector ops) |

### Load-Linked / Store-Conditional

The `reserve_addr`, `reserve_value`, and `reserve_valid` fields implement the PowerPC atomic compare-and-swap pattern used by `lwarx`/`stwcx.` instructions. The recompiled code sets a reservation on `lwarx` and checks/clears it on `stwcx.`.

### Stack Setup

```c
ppu_set_stack(ctx, stack_base, stack_size);
```

Places the stack pointer (r1) at the top of the given region, aligned down to 16 bytes, with 48 bytes reserved for the minimum PPC64 stack frame (back-chain + LR save area).

---

## SPU Execution Context

**File:** `runtime/spu/spu_context.h`

### Architecture

Each SPU is a self-contained processor with:
- **128 general-purpose 128-bit registers** (not 32 like PPU — SPU has a much wider register file)
- **256 KB local store** — all code and data must fit here; no cache, no virtual memory
- **Channels** — communication ports for DMA, mailboxes, and signals

### Register File

```c
typedef struct spu_context {
    u128     gpr[128];          // 128 × 128-bit registers (16-byte aligned)
    uint8_t  ls[256 * 1024];    // 256 KB local store (16-byte aligned)
    uint32_t pc;                // Program counter (0x00000–0x3FFFF)
    uint32_t status;            // Running/stopped/waiting state
    uint32_t spu_id;            // SPU index (0–7)
    uint32_t spu_group_id;      // Thread group ID
    uint32_t decrementer;       // Free-running down counter
    uint32_t srr0;              // Save/Restore Register 0 (exception return)
    uint32_t event_status;      // Event flags
    uint32_t event_mask;        // Event mask
    spu_channel ch_out_mbox;    // SPU → PPU outbound mailbox
    spu_channel ch_in_mbox;     // PPU → SPU inbound mailbox
    spu_channel ch_out_intr_mbox; // SPU → PPU interrupt mailbox
    spu_channel ch_sig_notify[2]; // Signal notification 1 & 2
    uint32_t mfc_lsa;           // MFC staging: local store address
    uint32_t mfc_eah;           // MFC staging: EA high word
    uint32_t mfc_eal;           // MFC staging: EA low word
    uint32_t mfc_size;          // MFC staging: transfer size
    uint32_t mfc_tag;           // MFC staging: tag group ID
    uint32_t mfc_tag_mask;      // MFC tag mask for wait/poll
    uint32_t mfc_tag_status;    // MFC tag completion status
} spu_context;
```

### SPU Status Flags

```c
#define SPU_STATUS_STOPPED          0x0   // Stopped
#define SPU_STATUS_RUNNING          0x1   // Currently executing
#define SPU_STATUS_STOPPED_BY_STOP  0x2   // Hit a `stop` instruction
#define SPU_STATUS_STOPPED_BY_HALT  0x4   // Hit a `halt` instruction
#define SPU_STATUS_WAITING_CHANNEL  0x8   // Blocked on channel read
#define SPU_STATUS_SINGLE_STEP      0x10  // Single-step mode
```

### Local Store Access

All local store access uses a mask (`SPU_LS_MASK = 0x3FFFF`) to wrap addresses within the 256 KB range:

```c
spu_ls_ptr(ctx, lsa)            // Get host pointer to LS address
spu_ls_read32(ctx, lsa)         // Read 32-bit big-endian value
spu_ls_write32(ctx, lsa, val)   // Write 32-bit big-endian value
spu_ls_read128(ctx, lsa)        // Read 128-bit value (16-byte aligned)
spu_ls_write128(ctx, lsa, val)  // Write 128-bit value (16-byte aligned)
```

### Preferred Slot

SPU instructions operate on the "preferred slot" of a 128-bit register — the leftmost element in big-endian layout. For 32-bit word operations, this is element 0 of the `_u32` array:

```c
spu_preferred_u32(&reg)         // Extract preferred word (uint32_t)
spu_preferred_s32(&reg)         // Extract preferred word (int32_t)
spu_preferred_f32(&reg)         // Extract preferred word (float)
spu_preferred_u64(&reg)         // Extract preferred doubleword
spu_make_preferred_u32(val)     // Create register with value in preferred slot
```

### Channel System

Channels are the SPU's communication mechanism. Each channel has a value and a count (0 = empty, 1 = has data):

```c
spu_channel_write(&ch, val)     // Write value, set count to 1
spu_channel_read(&ch)           // Read value, set count to 0
spu_channel_has_data(&ch)       // Check if channel has data
```

**Channel IDs:**

| ID | Name | Direction | Purpose |
|---|---|---|---|
| 0 | `SPU_RdEventStat` | Read | Event status |
| 1 | `SPU_WrEventMask` | Write | Event mask |
| 3 | `SPU_RdSigNotify1` | Read | Signal notification register 1 |
| 4 | `SPU_RdSigNotify2` | Read | Signal notification register 2 |
| 7 | `SPU_WrDec` | Write | Write decrementer |
| 8 | `SPU_RdDec` | Read | Read decrementer |
| 28 | `SPU_WrOutMbox` | Write | Write outbound mailbox (SPU → PPU) |
| 29 | `SPU_RdInMbox` | Read | Read inbound mailbox (PPU → SPU) |
| 30 | `SPU_WrOutIntrMbox` | Write | Write outbound interrupt mailbox |

---

## Type System

**File:** `include/ps3emu/ps3types.h`

### Fixed-Width Integers

```c
// Unsigned
typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;

// Signed
typedef int8_t    s8;
typedef int16_t   s16;
typedef int32_t   s32;
typedef int64_t   s64;

// Boolean
typedef uint8_t   b8;       // PS3 boolean (1 byte)
```

### 128-bit Vector Type

The `u128` union supports accessing a 128-bit value at any lane width:

```c
typedef union u128_t {
    uint64_t _u64[2];   // 2 × 64-bit unsigned
    uint32_t _u32[4];   // 4 × 32-bit unsigned
    uint16_t _u16[8];   // 8 × 16-bit unsigned
    uint8_t  _u8[16];   // 16 × 8-bit unsigned
    int64_t  _s64[2];   // 2 × 64-bit signed
    int32_t  _s32[4];   // 4 × 32-bit signed
    int16_t  _s16[8];   // 8 × 16-bit signed
    int8_t   _s8[16];   // 16 × 8-bit signed
    float    _f32[4];   // 4 × 32-bit float
    double   _f64[2];   // 2 × 64-bit double
} u128;
```

This is used for both PPU VMX/AltiVec registers and SPU general-purpose registers.

### PS3 Address Type

```c
typedef uint32_t ps3_addr_t;    // Always 32-bit, even on 64-bit host
```

### System Handle Types

```c
typedef s32  CellError;                 // Return code (0 = OK, negative = error)
typedef u64  sys_ppu_thread_t;          // PPU thread ID
typedef u32  sys_spu_thread_t;          // SPU thread ID
typedef u32  sys_spu_thread_group_t;    // SPU thread group ID
typedef u32  sys_mutex_t;               // Mutex handle
typedef u32  sys_cond_t;                // Condition variable handle
typedef u32  sys_rwlock_t;              // Read-write lock handle
typedef u32  sys_event_queue_t;         // Event queue handle
typedef u32  sys_semaphore_t;           // Semaphore handle
typedef u32  sys_lwmutex_t;             // Lightweight mutex handle
typedef u32  sys_event_flag_t;          // Event flag handle
typedef u32  sys_memory_container_t;    // Memory container handle
```

### Big-Endian Wrapper (`be_t<T>`)

**(C++ only)**

`be_t<T>` stores a value in PS3 big-endian byte order and automatically converts to/from host order on read/write. It works with any arithmetic or enum type.

```cpp
be_t<u32> x = 0x12345678;  // Stores bytes as 12 34 56 78 in memory
u32 y = x;                  // Reads back as 0x12345678 (byte-swapped on LE host)
x += 1;                     // Increments correctly (reads, adds, writes back)
```

**Key design principles:**
- Storage is always big-endian (matching PS3 memory layout)
- Conversion happens at the read/write boundary, not during computation
- `raw()` access returns the un-converted bytes (for serialization/DMA)
- Supports full arithmetic: `+=`, `-=`, `*=`, `/=`, `&=`, `|=`, `^=`, `++`, `--`

**Common aliases:**

```cpp
be_u16, be_u32, be_u64      // Unsigned big-endian
be_s16, be_s32, be_s64      // Signed big-endian
be_f32, be_f64              // Floating-point big-endian
```

### Virtual Memory Pointers (`vm::ptr<T>`)

**(C++ only)**

`vm::ptr<T>` is a typed pointer into the PS3 guest address space. It holds a 32-bit guest address and translates through `vm::g_base` on dereference:

```cpp
vm::ptr<u32> p(0x10000);    // Points to guest address 0x10000
*p = 42;                     // Writes to vm::g_base + 0x10000
u32 val = p[3];             // Reads from vm::g_base + 0x1000C
p += 1;                      // Advances by sizeof(u32) = 4 bytes in guest space
```

**Methods:**
- `addr()` — returns the raw 32-bit guest address
- `get_ptr()` — returns the host pointer (`vm::g_base + addr`)
- `operator*` / `operator->` — dereference through host translation
- `operator[]` — array-style indexing
- Pointer arithmetic advances by `sizeof(T)` in guest space
- Explicit `operator bool` — true if non-null
- Comparison operators: `==`, `!=`, `<`

**Specialization for `void`:** `vm::ptr<void>` holds an untyped address and provides `get_ptr()` returning `void*`.

**Alias:** `vm::bptr<T>` = `vm::ptr<be_t<T>>` — a pointer to a big-endian value (the most common case when mapping PS3 structures).

---

## Endianness Layer

**File:** `include/ps3emu/endian.h`

### Platform Intrinsics

The byte-swap primitives use compiler builtins for maximum performance:

| Platform | 16-bit | 32-bit | 64-bit |
|----------|--------|--------|--------|
| **MSVC** | `_byteswap_ushort` | `_byteswap_ulong` | `_byteswap_uint64` |
| **GCC/Clang** | `__builtin_bswap16` | `__builtin_bswap32` | `__builtin_bswap64` |
| **Portable** | Shift/mask fallback | Shift/mask fallback | Shift/mask fallback |

These compile to single instructions on x86 (`bswap`), ARM (`rev`), etc.

### 128-bit Swap

For VMX/AltiVec 128-bit values, a swap reverses both 64-bit halves and exchanges them:

```c
ps3_u128 ps3_bswap128(ps3_u128 v);
// Equivalent to: { bswap64(v.lo), bswap64(v.hi) }
```

### C++ Template API

```cpp
namespace ps3::endian {
    template <typename T> T byte_swap(T v);     // Generic byte swap
    template <typename T> T be_to_host(T v);    // Big-endian → host order
    template <typename T> T host_to_be(T v);    // Host order → big-endian
}
```

`be_to_host` and `host_to_be` are the same operation (swap is its own inverse) but the two names make intent clear:
- Use `be_to_host` when **reading** from PS3 memory
- Use `host_to_be` when **writing** to PS3 memory

Specializations exist for: `uint8_t`/`int8_t` (no-op), `uint16_t`/`int16_t`, `uint32_t`/`int32_t`, `uint64_t`/`int64_t`, `float`, `double`.

Float/double swap uses `memcpy` to avoid strict-aliasing violations:

```cpp
template <> float byte_swap<float>(float v) {
    uint32_t tmp;
    memcpy(&tmp, &v, sizeof(tmp));
    tmp = ps3_bswap32(tmp);
    float result;
    memcpy(&result, &tmp, sizeof(result));
    return result;
}
```

---

## Syscall Dispatch

**File:** `runtime/syscalls/lv2_syscall_table.h`

### How PS3 Syscalls Work

When PS3 code executes the `sc` (System Call) instruction:
1. The syscall number is in `r11`
2. Up to 8 arguments are in `r3–r10`
3. The kernel handler executes and places the return value in `r3`

In recompiled code, the `sc` instruction becomes a call to `lv2_syscall(ctx)`, which dispatches through a function pointer table.

### Dispatch Table

```c
typedef int64_t (*lv2_syscall_fn)(ppu_context* ctx);

typedef struct lv2_syscall_table {
    lv2_syscall_fn handlers[1024];  // Up to 1024 syscall slots
} lv2_syscall_table;

extern lv2_syscall_table g_lv2_syscalls;  // Global instance
```

### Argument Access

Syscall handlers extract arguments from the PPU context:

```c
#define LV2_ARG_U64(ctx, n)  ((ctx)->gpr[3 + (n)])     // 64-bit unsigned
#define LV2_ARG_U32(ctx, n)  ((uint32_t)(ctx)->gpr[3 + (n)])  // 32-bit unsigned
#define LV2_ARG_S32(ctx, n)  ((int32_t)(ctx)->gpr[3 + (n)])   // 32-bit signed
#define LV2_ARG_PTR(ctx, n)  ((uint32_t)(ctx)->gpr[3 + (n)])  // Guest address
```

### Syscall Number Ranges

| Range | Category | Examples |
|---|---|---|
| 1–14 | Process management | `getpid`, `exit`, `wait` |
| 41–56 | PPU thread management | `create`, `exit`, `join`, `detach`, `yield` |
| 70–76 | Timers | `create`, `start`, `stop`, `usleep` |
| 90–94, 114 | Semaphores | `create`, `destroy`, `wait`, `post` |
| 100–110 | Mutexes and condvars | `create`, `lock`, `unlock`, `wait`, `signal` |
| 120–127 | Read-write locks | `rlock`, `wlock`, `runlock`, `wunlock` |
| 128–146 | Event queues/ports/flags | `create`, `receive`, `send`, `wait`, `set` |
| 150–160 | Lightweight mutexes/condvars | `lwmutex`, `lwcond` |
| 170–194, 229–251 | SPU management | `group_create`, `start`, `join`, `write_ls` |
| 330–358 | Memory management | `allocate`, `free`, `container`, `mmapper` |
| 400–402 | TTY I/O | `read`, `write` |
| 480–494 | PRX module management | `load`, `start`, `stop`, `unload` |
| 801–841 | Filesystem | `open`, `read`, `write`, `close`, `stat`, `mkdir` |

### Registration

```c
// Register a single syscall
LV2_REGISTER_SYSCALL(SYS_PPU_THREAD_CREATE, sys_ppu_thread_create);

// Or use the bulk registration function
lv2_register_all_syscalls(&g_lv2_syscalls);
```

Unimplemented syscalls return `CELL_ENOSYS` (0x80010003).

---

## DMA Engine

**File:** `runtime/spu/spu_dma.h`

### Overview

The Memory Flow Controller (MFC) handles all data transfer between an SPU's local store and main memory. In real hardware, DMA is asynchronous — the SPU issues a command and continues executing while the MFC handles the transfer in the background. In ps3recomp, **DMA is executed synchronously** because all transfers complete before the next SPU instruction in recompiled code.

### MFC Command Opcodes

| Opcode | Name | Direction | Notes |
|--------|------|-----------|-------|
| `0x20` | `MFC_PUT_CMD` | LS → Main memory | Basic put |
| `0x21` | `MFC_PUTB_CMD` | LS → Main memory | Put with barrier |
| `0x22` | `MFC_PUTF_CMD` | LS → Main memory | Put with fence |
| `0x40` | `MFC_GET_CMD` | Main memory → LS | Basic get |
| `0x41` | `MFC_GETB_CMD` | Main memory → LS | Get with barrier |
| `0x42` | `MFC_GETF_CMD` | Main memory → LS | Get with fence |
| `0x24` | `MFC_PUTL_CMD` | LS → Main memory | Put list (scatter) |
| `0x44` | `MFC_GETL_CMD` | Main memory → LS | Get list (gather) |
| `0xA0` | `MFC_SNDSIG_CMD` | — | Send signal |
| `0xC0` | `MFC_BARRIER_CMD` | — | Ordering barrier |
| `0xC8` | `MFC_EIEIO_CMD` | — | Enforce in-order execution |
| `0xCC` | `MFC_SYNC_CMD` | — | Full synchronization |

**Opcode bit encoding:**
- Bit 6 (`0x40`): GET family
- Bit 5 (`0x20`): PUT family
- Bit 2 (`0x04`): List variant
- Bit 1 (`0x02`): Fence variant
- Bit 0 (`0x01`): Barrier variant

### Transfer Flow

1. SPU writes to MFC staging channels (via `wrch` instruction):
   - `MFC_LSA` — local store address
   - `MFC_EAH` — effective address high 32 bits
   - `MFC_EAL` — effective address low 32 bits
   - `MFC_Size` — transfer size (max 16 KB)
   - `MFC_TagID` — tag group (0–31) for synchronization
2. SPU writes command opcode to `MFC_Cmd` — this triggers the transfer
3. For synchronization, SPU writes tag mask to `MFC_WrTagMask`, then writes update type to `MFC_WrTagUpdate`, then reads `MFC_RdTagStat`

### DMA List Commands

List commands (PUTL/GETL) perform scatter/gather transfers. The list resides in local store and consists of 8-byte elements:

```c
typedef struct mfc_list_element {
    uint32_t size_and_flags;  // bits 0-14: size, bit 15: stall-and-notify
    uint32_t eal;             // effective address low 32 bits
} mfc_list_element;
```

### Tag Synchronization

Each DMA command is assigned to one of 32 tag groups. The SPU can wait for specific groups to complete:

| Update Type | Value | Behavior |
|---|---|---|
| `MFC_TAG_UPDATE_IMMEDIATE` | 0 | Return current status immediately |
| `MFC_TAG_UPDATE_ANY` | 1 | Wait until any tagged transfer completes |
| `MFC_TAG_UPDATE_ALL` | 2 | Wait until all tagged transfers complete |

In synchronous mode, all transfers complete immediately, so tag waits always succeed.

---

## Error Codes

**File:** `include/ps3emu/error_codes.h`

### Error Code Format

PS3 error codes are 32-bit values. `CELL_OK` (0) indicates success. All error codes have the high bit set (0x80000000).

**Format:** `0x8XXYYZZ` where:
- `XX` identifies the subsystem
- `YYZZ` is the specific error within that subsystem

### Generic System Errors (0x8001xxxx)

| Code | Name | Meaning |
|------|------|---------|
| `0x00000000` | `CELL_OK` | Success |
| `0x80010001` | `CELL_EAGAIN` | Resource temporarily unavailable |
| `0x80010002` | `CELL_EINVAL` | Invalid argument |
| `0x80010003` | `CELL_ENOSYS` | Function not implemented |
| `0x80010004` | `CELL_ENOMEM` | Not enough memory |
| `0x80010005` | `CELL_ESRCH` | No such entity |
| `0x80010006` | `CELL_ENOENT` | No such file or directory |
| `0x80010008` | `CELL_EDEADLK` | Deadlock avoided |
| `0x80010009` | `CELL_EPERM` | Not permitted |
| `0x8001000A` | `CELL_EBUSY` | Device or resource busy |
| `0x8001000B` | `CELL_ETIMEDOUT` | Connection timed out |
| `0x8001000C` | `CELL_EABORT` | Operation aborted |
| `0x80010013` | `CELL_ECANCELED` | Operation canceled |
| `0x80010014` | `CELL_EEXIST` | Already exists |

### Module Error Bases

Each HLE module defines errors as `(base | local_code)`:

| Base | Module |
|------|--------|
| `0x80012000` | cellSysmodule |
| `0x80010700` | cellFs / sys_fs |
| `0x80310700` | cellAudio |
| `0x8002B100` | cellVideoOut |
| `0x80210700` | cellGcmSys |
| `0x80130100` | cellNet |
| `0x80121100` | cellPad |
| `0x8002B000` | cellSysutil |
| `0x80540000` | cellFont |
| `0x80410700` | cellSpurs |
| `0x80710100` | cellHttp |
| `0x80720100` | cellSsl |

### Helper Macros

```c
CELL_IS_ERROR(code)             // True if high bit is set
CELL_RETURN_IF_ERROR(expr)      // Early return if expression returns an error
CELL_MODULE_ERROR(base, code)   // Build module-specific error code
cell_error_name(code)           // Pretty-print error code for logging (C++ only)
```
