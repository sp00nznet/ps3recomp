# Contributing to ps3recomp

Thanks for your interest in helping preserve PS3 games through static recompilation. Whether you're a seasoned reverse engineer or just getting started, there's meaningful work to do here.

## Ways to Contribute

### Port a New Game
The most impactful contribution. Pick a PS3 title, analyze its binary, lift the code, and wire up the HLE modules it needs. See the [Game Porting Guide](docs/GAME_PORTING_GUIDE.md) and the [flOw case study](docs/GAME_PORTING_GUIDE.md#case-study-flow) for a real-world walkthrough.

### Improve HLE Modules
Many modules have stub implementations that return `CELL_OK` without doing real work. Upgrading a stub to a real implementation — backed by host OS services — directly unblocks games. Check [docs/MODULE_STATUS.md](docs/MODULE_STATUS.md) for what needs work.

### Add New HLE Modules
Some PS3 system libraries aren't covered yet. If a game you're working on imports from an unimplemented module, adding it to `libs/` helps everyone. See [docs/CUSTOM_MODULES.md](docs/CUSTOM_MODULES.md) for the full walkthrough.

### Improve the Lifter/Tools
The PPU and SPU lifters in `tools/` handle most instructions but some edge cases remain (VMX/AltiVec SIMD, complex branch patterns). Improving instruction coverage benefits every game port.

### Documentation
Clear docs lower the barrier for new contributors. Fix errors, add examples, improve explanations — especially for areas you found confusing when getting started.

### Testing and Bug Reports
Run existing game ports, compare behavior with RPCS3, and report discrepancies. Detailed bug reports with RPCS3 log comparisons are extremely valuable.

---

## Development Setup

### Prerequisites
- **Python 3.10+** — for recompiler tools
- **CMake 3.20+** — build system
- **C17/C++20 compiler** — MSVC 19.x (VS 2019+), GCC 12+, or Clang 15+
- **Git** — version control

### Building from Source

```bash
git clone https://github.com/sp00nznet/ps3recomp.git
cd ps3recomp
pip install -r requirements.txt

# Build the runtime library
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

On Windows with Visual Studio:
```bash
cmake -B build
cmake --build build --config Release
```

### Running Tests (if available)
```bash
cmake -B build -DPS3RECOMP_BUILD_TESTS=ON
cmake --build build --target test
```

---

## Project Architecture

Quick orientation for newcomers. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the deep dive.

```
ps3recomp/
├── tools/          # Python pipeline: ELF parsing → disassembly → code lifting
├── runtime/        # Core C runtime: PPU/SPU contexts, virtual memory, LV2 syscalls
├── libs/           # 93 HLE module implementations (the bulk of the code)
├── include/ps3emu/ # Public API headers for game projects
├── templates/      # Starter project template
└── docs/           # 11+ documentation files
```

**Data flow:** PS3 ELF → `tools/` analyzes and lifts to C → game project links against `runtime/` + `libs/` → native executable.

---

## Code Style

### C Code (runtime/, libs/)
- **C17 standard** — no C++ features in runtime or lib code
- **snake_case** for functions and variables: `vm_read32()`, `s_initialized`
- **SCREAMING_CASE** for constants and macros: `CELL_OK`, `PS3_MAX_MODULES`
- **HLE function names match PS3 SDK**: `cellAudioInit()`, `cellGcmSetFlipMode()`
- **Static internal state**: use `static` for module-local state, prefix with `s_`
- **Include guards**: `#ifndef PS3RECOMP_CELL_FOO_H` / `#define` / `#endif`
- **Comments**: `/* C-style */` block comments for sections, inline for non-obvious logic

### C++ Code (game projects, templates/)
- **C++20 standard** — game projects use C++ for convenience
- **`extern "C"`** when including runtime/lib headers from C++

### Python Code (tools/)
- **Python 3.10+**, type hints encouraged
- **snake_case** for functions and variables
- Standard library preferred over external dependencies

### General
- No trailing whitespace
- LF line endings (Git handles CRLF on Windows)
- 4-space indentation (no tabs) in C/C++
- Keep files under 500 lines when practical (HLE modules should be self-contained)

---

## Adding an HLE Module

Here's the condensed version. See [docs/CUSTOM_MODULES.md](docs/CUSTOM_MODULES.md) for the full tutorial.

### 1. Create the Header (`libs/<category>/cellFoo.h`)

```c
#ifndef PS3RECOMP_CELL_FOO_H
#define PS3RECOMP_CELL_FOO_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define CELL_FOO_ERROR_NOT_INIT    (s32)0x80410001
#define CELL_FOO_ERROR_ALREADY     (s32)0x80410002

/* Structures */
typedef struct CellFooConfig {
    u32 flags;
    u32 reserved;
} CellFooConfig;

/* Functions */
s32 cellFooInit(u32 flags);
s32 cellFooEnd(void);
s32 cellFooGetConfig(CellFooConfig* config);

#ifdef __cplusplus
}
#endif
#endif
```

### 2. Create the Implementation (`libs/<category>/cellFoo.c`)

```c
#include "cellFoo.h"
#include <stdio.h>
#include <string.h>

static int s_initialized = 0;
static CellFooConfig s_config;

s32 cellFooInit(u32 flags)
{
    printf("[cellFoo] Init(flags=0x%x)\n", flags);
    if (s_initialized) return CELL_FOO_ERROR_ALREADY;
    memset(&s_config, 0, sizeof(s_config));
    s_config.flags = flags;
    s_initialized = 1;
    return CELL_OK;
}

s32 cellFooEnd(void)
{
    if (!s_initialized) return CELL_FOO_ERROR_NOT_INIT;
    s_initialized = 0;
    return CELL_OK;
}

s32 cellFooGetConfig(CellFooConfig* config)
{
    if (!s_initialized) return CELL_FOO_ERROR_NOT_INIT;
    if (!config) return CELL_EFAULT;
    *config = s_config;
    return CELL_OK;
}
```

### 3. Add to CMakeLists.txt

Add your `.c` file to the runtime library's source list.

### 4. Register in a Game Project

In the game's `hle_modules.cpp`, register functions by NID:

```c
reg_func(&mod_cellFoo, "cellFooInit", (void*)bridge_cellFooInit);
```

---

## Testing Your Changes

1. **Build the runtime** — `cmake --build build` must succeed
2. **Link to a game project** — build the flOw port or another test project
3. **Compare with RPCS3** — run the same game in RPCS3 with logging enabled and compare HLE call sequences
4. **Check for regressions** — make sure existing functionality still works

---

## Pull Request Guidelines

- **One logical change per PR** — don't mix unrelated fixes
- **Describe the "why"** — the diff shows what changed; the PR description explains why
- **Include test evidence** — screenshots, log comparisons, or before/after behavior
- **Keep generated code out** — don't commit lifter output (156 MB `.cpp` files)
- **No secrets** — never commit game binaries, encryption keys, or proprietary data

### Good PR Examples
- "Add cellFoo HLE module with Init/End/GetConfig (needed for Game X)"
- "Fix cellGcmSetTile pitch calculation for non-power-of-2 sizes"
- "Improve PPU lifter: handle `rldicl` with mb > me (wrap-around mask)"

---

## Reporting Issues

When filing a bug report, include:
- **What game** you're porting (title ID, engine if known)
- **What happened** vs. what you expected
- **Relevant log output** — HLE call traces, crash dumps, lifter warnings
- **RPCS3 comparison** — if the same call sequence works in RPCS3
- **Your environment** — OS, compiler, CMake version

For feature requests, describe the use case — which game needs it and why.

---

## License

ps3recomp is [MIT licensed](LICENSE). By contributing, you agree that your contributions will be licensed under the same terms.

## Questions?

Open a [GitHub Discussion](https://github.com/sp00nznet/ps3recomp/discussions) or file an issue. We're a small community but we're responsive.
