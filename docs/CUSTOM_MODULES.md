# Writing Custom HLE Modules

A step-by-step guide to adding new High-Level Emulation modules to ps3recomp.

---

## What Are HLE Modules?

Every PS3 game depends on system libraries (PRX modules) for graphics, audio, input, filesystem, networking, and more. Instead of running the original Sony code, ps3recomp replaces these libraries with **HLE (High-Level Emulation) modules** — native implementations that provide equivalent behavior using host OS services.

When a game calls `cellAudioInit()`, our HLE module initializes WASAPI (Windows) or SDL2 (Linux) audio instead of the PS3's audio hardware. The game can't tell the difference.

---

## Module Anatomy

Every module follows this pattern:

```
libs/<category>/
├── cellFoo.h     # Public API: constants, error codes, structs, function declarations
└── cellFoo.c     # Implementation + optional lifecycle hooks
```

**Categories:** `audio/`, `video/`, `input/`, `network/`, `filesystem/`, `system/`, `spurs/`, `sync/`, `codec/`, `font/`, `misc/`

Choose the category that matches the PS3 SDK grouping.

---

## Step-by-Step: Creating a Module

### 1. Create the Header

`libs/codec/cellFooDec.h`:

```c
#ifndef PS3RECOMP_CELL_FOO_DEC_H
#define PS3RECOMP_CELL_FOO_DEC_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Error Codes ──────────────────────────── */
#define CELL_FOODEC_ERROR_NOT_INIT    (s32)0x80611001
#define CELL_FOODEC_ERROR_ARG         (s32)0x80611002
#define CELL_FOODEC_ERROR_SEQ         (s32)0x80611003

/* ─── Constants ────────────────────────────── */
#define CELL_FOODEC_MAX_HANDLES  8

/* ─── Structures ───────────────────────────── */
typedef struct CellFooDecSrc {
    u32 srcSize;
    const void* srcPtr;     /* guest pointer to encoded data */
} CellFooDecSrc;

typedef struct CellFooDecInfo {
    u32 width;
    u32 height;
    u32 format;
} CellFooDecInfo;

/* ─── Functions ────────────────────────────── */

s32 cellFooDecCreate(u32* handle);
s32 cellFooDecDestroy(u32 handle);
s32 cellFooDecDecode(u32 handle, const CellFooDecSrc* src, CellFooDecInfo* info);

#ifdef __cplusplus
}
#endif
#endif
```

**Key conventions:**
- Error codes use the module's assigned error base from the PS3 SDK
- Structs match PS3 SDK layout (member names, sizes, alignment)
- All functions return `s32` (CELL_OK on success, negative on error)
- `extern "C"` guards for C++ compatibility

### 2. Create the Implementation

`libs/codec/cellFooDec.c`:

```c
#include "cellFooDec.h"
#include <stdio.h>
#include <string.h>

/* ─── Internal State ──────────────────────── */

typedef struct {
    int in_use;
    /* decoder-specific state */
} FooDecHandle;

static FooDecHandle s_handles[CELL_FOODEC_MAX_HANDLES];
static int s_initialized = 0;

/* ─── Internal Helpers ────────────────────── */

static int find_free_handle(void)
{
    for (int i = 0; i < CELL_FOODEC_MAX_HANDLES; i++) {
        if (!s_handles[i].in_use) return i;
    }
    return -1;
}

/* ─── Public API ──────────────────────────── */

s32 cellFooDecCreate(u32* handle)
{
    printf("[cellFooDec] Create()\n");

    if (!handle)
        return CELL_FOODEC_ERROR_ARG;

    int idx = find_free_handle();
    if (idx < 0)
        return CELL_FOODEC_ERROR_SEQ;

    memset(&s_handles[idx], 0, sizeof(FooDecHandle));
    s_handles[idx].in_use = 1;
    *handle = (u32)idx;

    return CELL_OK;
}

s32 cellFooDecDestroy(u32 handle)
{
    printf("[cellFooDec] Destroy(handle=%u)\n", handle);

    if (handle >= CELL_FOODEC_MAX_HANDLES || !s_handles[handle].in_use)
        return CELL_FOODEC_ERROR_ARG;

    s_handles[handle].in_use = 0;
    return CELL_OK;
}

s32 cellFooDecDecode(u32 handle, const CellFooDecSrc* src, CellFooDecInfo* info)
{
    if (handle >= CELL_FOODEC_MAX_HANDLES || !s_handles[handle].in_use)
        return CELL_FOODEC_ERROR_ARG;
    if (!src || !info)
        return CELL_FOODEC_ERROR_ARG;

    printf("[cellFooDec] Decode(handle=%u, srcSize=%u)\n", handle, src->srcSize);

    /* TODO: actual decoding logic */
    memset(info, 0, sizeof(*info));
    info->width = 64;
    info->height = 64;

    return CELL_OK;
}
```

### 3. Add to CMakeLists.txt

Add your source file to the runtime library's source list in the top-level `CMakeLists.txt`.

### 4. Register in Game Projects

In the game's `hle_modules.cpp`, create bridge functions and register by NID:

```c
static int64_t bridge_cellFooDecCreate(ppu_context* ctx) {
    uint32_t handle_addr = (uint32_t)ctx->gpr[3];
    u32 host_handle = 0;
    s32 rc = cellFooDecCreate(&host_handle);
    if (rc == CELL_OK && handle_addr)
        vm_write32(handle_addr, host_handle);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}
```

---

## The NID System

PS3 dynamic linking uses **Name IDs** instead of symbol names. The NID is computed as:

```
NID = first_4_bytes_little_endian(SHA1(function_name + suffix))
```

Where `suffix` is the fixed 16-byte sequence:
```
\x67\x59\x65\x99\x04\x25\x04\x90\x56\x64\x27\x49\x94\x89\x74\x1A
```

ps3recomp provides `ps3_compute_nid(name)` in `include/ps3emu/nid.h` to compute NIDs from function names at runtime.

When a game binary imports NID `0x0B168F92`, the runtime walks all registered modules looking for a handler registered under that NID. If it finds one, it calls it; otherwise it logs "UNIMPLEMENTED".

---

## Implementation Patterns

### Init/Shutdown Lifecycle

Most modules follow init → use → shutdown:

```c
static int s_initialized = 0;

s32 cellFooInit(void) {
    if (s_initialized) return CELL_FOO_ERROR_ALREADY;
    /* allocate resources */
    s_initialized = 1;
    return CELL_OK;
}

s32 cellFooEnd(void) {
    if (!s_initialized) return CELL_FOO_ERROR_NOT_INIT;
    /* free resources */
    s_initialized = 0;
    return CELL_OK;
}
```

### Guest Memory Access

Guest memory is big-endian and accessed through `vm_base`:

```c
/* In HLE implementations that need guest memory access */
extern uint8_t* vm_base;

/* Read a big-endian u32 from guest address */
u32 val = vm_read32(guest_addr);

/* Write a big-endian u32 to guest address */
vm_write32(guest_addr, val);

/* Access raw bytes (no endian swap needed) */
void* ptr = vm_base + guest_addr;
memcpy(ptr, data, size);
```

### Handle/ID Allocation

Use static arrays with `in_use` flags:

```c
#define MAX_HANDLES 64
static MyHandle s_handles[MAX_HANDLES];

static int alloc_handle(void) {
    for (int i = 0; i < MAX_HANDLES; i++)
        if (!s_handles[i].in_use) return i;
    return -1;
}
```

This is simpler than dynamic allocation and matches PS3 behavior (fixed limits per module).

---

## Calling Convention Bridge

When recompiled code calls an HLE function, the call goes through an import stub that dispatches via NID. The handler receives a `ppu_context*` with the full PPC64 register state.

### PPC64 ABI Register Usage

| Register | Purpose |
|----------|---------|
| r3-r10 | Integer/pointer arguments (first 8 args) |
| f1-f13 | Floating-point arguments |
| r3 | Integer return value |
| f1 | Float return value |
| r1 | Stack pointer |
| r2 | TOC (Table of Contents) base |
| r13 | TLS base |

### Bridge Function Template

```c
static int64_t bridge_cellFooBar(ppu_context* ctx)
{
    /* Extract arguments from GPRs */
    uint32_t arg1 = (uint32_t)ctx->gpr[3];
    uint32_t arg2 = (uint32_t)ctx->gpr[4];

    /* For pointer arguments: translate guest → host */
    const char* str = (const char*)(vm_base + (uint32_t)ctx->gpr[5]);

    /* Call real implementation */
    s32 rc = cellFooBar(arg1, arg2, str);

    /* Store return value */
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}
```

### Output Struct Pattern

When the HLE writes a struct to guest memory, each field must be byte-swapped:

```c
static int64_t bridge_cellFooGetInfo(ppu_context* ctx)
{
    uint32_t info_addr = (uint32_t)ctx->gpr[3];

    CellFooInfo host_info;
    s32 rc = cellFooGetInfo(&host_info);

    if (rc == CELL_OK && info_addr) {
        vm_write32(info_addr,     host_info.field1);  /* big-endian write */
        vm_write32(info_addr + 4, host_info.field2);
        vm_write64(info_addr + 8, host_info.field3);
    }

    ctx->gpr[3] = (uint64_t)(int64_t)rc;
    return rc;
}
```

---

## Testing

1. **Build the runtime** — your module must compile clean
2. **Link to a game** — add bridge functions to a game's `hle_modules.cpp`
3. **Run and compare** — check HLE log output against RPCS3's behavior
4. **Stub first** — start with functions returning CELL_OK, then add real behavior

### RPCS3 Cross-Reference

RPCS3's source is the best reference for PS3 system behavior:
- Module implementations: `rpcs3/Emu/Cell/Modules/`
- Syscall implementations: `rpcs3/Emu/Cell/lv2/`
- Error codes and struct layouts match the PS3 SDK

When implementing a function, search RPCS3 for the function name to see:
- Parameter validation logic
- Return value semantics
- Edge cases and error conditions
- Struct layouts with field offsets

---

## Real Examples in This Project

| Module | Complexity | Key Pattern | File |
|--------|-----------|-------------|------|
| cellSysmodule | Simple | State tracking, module ID management | `libs/system/cellSysmodule.c` |
| cellVideoOut | Medium | Struct output, resolution config | `libs/video/cellVideoOut.c` |
| cellAudio | Complex | Background thread, WASAPI/SDL2 backend, port management | `libs/audio/cellAudio.c` |
| cellSync | Advanced | C11 atomics, spinlocks, barriers | `libs/sync/cellSync.c` |
| cellFs | Medium | Host filesystem, path translation | `libs/filesystem/cellFs.c` |
| cellPad | Medium | XInput/SDL2 backend, button mapping | `libs/input/cellPad.c` |
