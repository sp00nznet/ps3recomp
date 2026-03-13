# PS3 NID (Name Identifier) System

Complete reference for how PS3 function linking works, how NIDs are computed, and how ps3recomp resolves them.

---

## Table of Contents

1. [What Is a NID?](#what-is-a-nid)
2. [NID Computation](#nid-computation)
3. [How PS3 Linking Works](#how-ps3-linking-works)
4. [ps3recomp NID Resolution](#ps3recomp-nid-resolution)
5. [NID Lookup Table Implementation](#nid-lookup-table-implementation)
6. [Module Registration Framework](#module-registration-framework)
7. [NID Database Tools](#nid-database-tools)
8. [Custom NID Suffixes](#custom-nid-suffixes)
9. [Troubleshooting Unresolved NIDs](#troubleshooting-unresolved-nids)

---

## What Is a NID?

A **NID (Name IDentifier)** is a 32-bit value that uniquely identifies a function or variable exported from a PS3 system library (PRX module). Instead of linking by symbol name (like ELF does on Linux), the PS3 uses NIDs as a compact, obfuscated form of symbolic linking.

**Why NIDs?**
- **Space efficiency** — 4 bytes instead of a full function name string
- **Obfuscation** — makes reverse engineering harder (slightly)
- **Fast lookup** — integer comparison vs. string comparison
- **Versioning** — different firmware versions can use different NID suffixes to change the mapping

**Example:**
```
Function name:  cellFsOpen
NID:            0x718BF5F8  (first 4 bytes of SHA-1("cellFsOpen" + suffix))
```

---

## NID Computation

### Algorithm

```
NID = first 4 bytes of SHA-1(function_name + suffix), interpreted as big-endian uint32
```

### Default Suffix

The standard NID suffix is a 4-byte sequence:

```c
#define PS3_NID_SUFFIX      "\x67\x59\x65\x99"
#define PS3_NID_SUFFIX_LEN  4
```

This suffix is used by most firmware libraries. Its origin: these are simply the first 4 bytes of a specific hash — Sony chose them as the "salt" for NID computation.

### Step-by-Step Example

Computing the NID for `cellSysutilGetSystemParamInt`:

1. **Concatenate**: `"cellSysutilGetSystemParamInt" + "\x67\x59\x65\x99"`
2. **SHA-1 hash** the concatenated bytes (32 bytes total)
3. **Extract** the first 4 bytes of the 20-byte digest
4. **Interpret** as a big-endian 32-bit unsigned integer

### Implementation

```c
static inline uint32_t ps3_compute_nid(const char* name)
{
    ps3_sha1_ctx ctx;
    uint8_t digest[20];

    ps3_sha1_init(&ctx);
    ps3_sha1_update(&ctx, name, strlen(name));
    ps3_sha1_update(&ctx, PS3_NID_SUFFIX, PS3_NID_SUFFIX_LEN);
    ps3_sha1_final(&ctx, digest);

    return ((uint32_t)digest[0] << 24) |
           ((uint32_t)digest[1] << 16) |
           ((uint32_t)digest[2] << 8)  |
           ((uint32_t)digest[3]);
}
```

ps3recomp includes a **minimal SHA-1 implementation** (RFC 3174) embedded in `nid.h` — just enough for NID computation, not intended for cryptographic use. This avoids external dependencies for a core operation.

---

## How PS3 Linking Works

### On Real Hardware

1. **Game ELF** contains import stubs for each library function it calls
2. Each import stub is tagged with a `(module_name, NID)` pair
3. When the PS3 OS loads a PRX module, it populates an **export table** mapping NIDs to function addresses
4. The OS **resolves imports** by matching each import's `(module, NID)` against loaded module exports
5. The import stub is patched to jump directly to the resolved function address

### Import Table Structure (ELF)

In the PS3 ELF, each imported module has a structure like:

```
Module: "cellFs"
  Imports:
    NID 0x718BF5F8 → cellFsOpen
    NID 0x4D5FF8E2 → cellFsRead
    NID 0x6D3BB15B → cellFsWrite
    NID 0x8ACAC8B6 → cellFsClose
    ...
```

The `tools/elf_parser.py` extracts these import tables during the analysis phase.

### In Static Recompilation

Since we're compiling everything ahead of time, the NID resolution happens during the recompilation phase:

1. **Parse** the ELF to extract all `(module, NID)` import pairs
2. **Look up** each NID in the precomputed database to get the function name
3. **Replace** the import call site with a direct call to the HLE implementation
4. **Unresolved NIDs** become calls to a fallback that logs a warning and returns `CELL_ENOSYS`

---

## ps3recomp NID Resolution

### Architecture

```
┌──────────────────────────┐
│     Recompiled Code      │
│                          │
│  call_import(NID)        │
│         │                │
│         ▼                │
│  ps3_resolve_func_nid()  │
│         │                │
│         ▼                │
│  ┌──────────────────┐    │
│  │  Module Registry  │    │
│  │  ┌──────────────┐ │    │
│  │  │  cellFs       │ │    │
│  │  │  func_table:  │ │    │
│  │  │  NID → handler│ │    │
│  │  └──────────────┘ │    │
│  │  ┌──────────────┐ │    │
│  │  │  cellPad      │ │    │
│  │  │  func_table:  │ │    │
│  │  │  NID → handler│ │    │
│  │  └──────────────┘ │    │
│  │  ...              │    │
│  └──────────────────┘    │
│         │                │
│         ▼                │
│  HLE function pointer    │
└──────────────────────────┘
```

### Resolution Function

```c
static inline void* ps3_resolve_func_nid(uint32_t nid)
{
    for (uint32_t i = 0; i < g_ps3_module_registry.count; i++) {
        ps3_module* m = g_ps3_module_registry.modules[i];
        if (!m->loaded) continue;
        ps3_nid_entry* e = ps3_nid_table_find(&m->func_table, nid);
        if (e) return e->handler;
    }
    return NULL;  // Not found — caller should log warning
}
```

Resolution walks all loaded modules' NID tables until a match is found. This is a linear scan — fine for the module counts we deal with (< 128 modules, < 512 functions each).

There's also `ps3_resolve_var_nid()` for resolving variable imports (exported global variables).

---

## NID Lookup Table Implementation

### Data Structures

```c
typedef struct ps3_nid_entry {
    uint32_t    nid;        // The 32-bit NID
    const char* name;       // Human-readable name (e.g., "cellFsOpen")
    void*       handler;    // HLE function pointer
} ps3_nid_entry;

typedef struct ps3_nid_table {
    ps3_nid_entry*  entries;    // Array of entries
    uint32_t        count;      // Current number of entries
    uint32_t        capacity;   // Maximum entries
} ps3_nid_table;
```

### Operations

```c
// Initialize a table with pre-allocated storage
ps3_nid_table_init(&table, storage_array, capacity);

// Add an entry
ps3_nid_table_add(&table, nid, "funcName", (void*)handler_fn);

// Find an entry by NID (linear scan)
ps3_nid_entry* entry = ps3_nid_table_find(&table, nid);
if (entry) {
    // entry->handler is the HLE function pointer
    // entry->name is the function name string
}
```

### Table Sizes

Each module is statically allocated with:
- **512 function slots** (`PS3_MODULE_MAX_FUNCS`) — overridable with `-DPS3_MODULE_MAX_FUNCS=N`
- **64 variable slots** (`PS3_MODULE_MAX_VARS`)

The global registry holds up to **128 modules** (`PS3_MAX_MODULES`).

---

## Module Registration Framework

### How Modules Register

The `DECLARE_PS3_MODULE` macro creates a static module instance and a C++ static-initialization constructor that registers it in the global registry before `main()` runs:

```cpp
DECLARE_PS3_MODULE(cellFs, "cellFs")
{
    REGISTER_FUNC(cellFsOpen);        // NID computed from "cellFsOpen"
    REGISTER_FUNC(cellFsRead);
    REGISTER_FUNC(cellFsWrite);
    REGISTER_FUNC(cellFsClose);
    REGISTER_FNID(0xE2A2D3AB, cellFsStat);  // Explicit NID

    SET_MODULE_INIT(cellFs_init);     // Called on module load
    SET_MODULE_SHUTDOWN(cellFs_shutdown);  // Called on module unload
}
```

### Macro Expansion

`DECLARE_PS3_MODULE(cellFs, "cellFs")` expands to:

```cpp
static ps3_module cellFs;
static void _ps3_register_cellFs();
namespace {
    struct _ps3_auto_cellFs {
        _ps3_auto_cellFs() {
            ps3_module_init(&cellFs, "cellFs");
            _ps3_register_cellFs();
            ps3_register_module(&cellFs);
        }
    };
    static _ps3_auto_cellFs _ps3_inst_cellFs;
}
static void _ps3_register_cellFs()  // ← the body you write follows
```

### Registration Macros

| Macro | Purpose |
|-------|---------|
| `REGISTER_FUNC(func)` | Register function with auto-computed NID from name |
| `REGISTER_FNID(nid, func)` | Register function with explicit NID value |
| `REGISTER_VAR(var)` | Register variable with auto-computed NID |
| `REGISTER_VAR_FNID(nid, var)` | Register variable with explicit NID |
| `SET_MODULE_INIT(fn)` | Set the module's initialization callback |
| `SET_MODULE_SHUTDOWN(fn)` | Set the module's shutdown callback |

### Under the Hood

`REGISTER_FUNC(cellFsOpen)` expands to:

```cpp
ps3_nid_table_add(&cellFs.func_table,
                   ps3_compute_nid("cellFsOpen"),  // SHA-1 hash at startup
                   "cellFsOpen",                    // Name string for debugging
                   (void*)(cellFsOpen));             // Function pointer
```

The NID computation happens once during static initialization (before `main()`), so there's no runtime hashing cost.

### Module Lifecycle

```c
// Loading a module (called by cellSysmodule)
ps3_module* m = ps3_find_module("cellFs");
int32_t rc = ps3_module_load(m);
// Calls m->on_load() if set, marks module as loaded

// Unloading a module
ps3_module_unload(m);
// Calls m->on_unload() if set, marks module as unloaded
```

Modules start unloaded. The `cellSysmodule` HLE module manages loading/unloading in response to game calls.

---

## NID Database Tools

### `tools/nid_database.py`

The Python NID database tool provides:

1. **2000+ precomputed NID-to-name mappings** derived from RPCS3's function tables and Sony SDK headers
2. **NID computation** for new function names
3. **Reverse lookup** from NID to function name
4. **Module classification** — which NIDs belong to which modules

### Usage

```bash
# Look up a NID
python tools/nid_database.py --lookup 0x718BF5F8
# Output: cellFsOpen (module: cellFs)

# Compute a NID from a function name
python tools/nid_database.py --compute cellFsOpen
# Output: 0x718BF5F8

# List all NIDs for a module
python tools/nid_database.py --module cellFs
# Output: 0x718BF5F8  cellFsOpen
#         0x4D5FF8E2  cellFsRead
#         ...
```

### Database Sources

The NID database is compiled from:

1. **RPCS3 source code** — the most comprehensive source, with module-by-module NID tables
2. **Sony SDK headers** — official function declarations with NID annotations
3. **Community research** — reverse-engineered NIDs from homebrew and analysis tools
4. **ps3recomp discoveries** — NIDs found during game analysis

---

## Custom NID Suffixes

Some PS3 libraries use a non-default suffix for NID computation. This was used as a form of library versioning — changing the suffix changes all NIDs, preventing old code from linking against new versions.

```c
// Compute NID with a custom suffix
uint32_t nid = ps3_compute_nid_ex("functionName",
                                    custom_suffix, suffix_length);
```

In practice, the vast majority of game-facing libraries use the default suffix (`\x67\x59\x65\x99`). Custom suffixes are mainly found in:
- Internal Sony debug/development libraries
- Certain firmware-version-specific system modules
- Some encryption-related libraries

---

## Troubleshooting Unresolved NIDs

### Symptom

```
[WARNING] Unimplemented NID 0xABCDEF01 in module cellFoo
```

### Steps to Resolve

1. **Look up the NID** in the database:
   ```bash
   python tools/nid_database.py --lookup 0xABCDEF01
   ```
   If found, you now know the function name.

2. **Check if it's already implemented** under a different module name (some functions are registered in unexpected modules).

3. **Check RPCS3** for a reference implementation:
   - Search for the NID or function name in RPCS3's `rpcs3/Emu/Cell/Modules/` directory
   - Adapt the implementation to ps3recomp's standalone C runtime

4. **Add a stub** if the function is non-critical:
   ```c
   // In stubs.cpp (game-specific)
   int32_t cellFooBarBaz(/* args */)
   {
       printf("[STUB] cellFooBarBaz called\n");
       return CELL_OK;
   }
   ```

5. **Add a full implementation** if the function is needed:
   ```c
   // In libs/misc/cellFoo.c
   int32_t cellFooBarBaz(uint32_t param1, uint32_t param2)
   {
       // Real implementation here
       return CELL_OK;
   }
   ```
   Then register it:
   ```cpp
   DECLARE_PS3_MODULE(cellFoo, "cellFoo")
   {
       REGISTER_FUNC(cellFooBarBaz);
   }
   ```

### Common Patterns

- **NID 0x00000000** — usually means the import table was parsed incorrectly; check ELF integrity
- **Multiple NIDs resolving to the same name** — different suffix versions; use `REGISTER_FNID` with both
- **NID not in database** — function may be from a debug/dev library or a very niche module; check RPCS3 first, then consider stubbing
