# Building ps3recomp

Complete guide to building the ps3recomp runtime library and game projects.

---

## Table of Contents

1. [Prerequisites](#prerequisites)
2. [Building the Runtime Library](#building-the-runtime-library)
3. [Build Options](#build-options)
4. [Platform-Specific Notes](#platform-specific-notes)
5. [Compiler Support](#compiler-support)
6. [CMake Build System Details](#cmake-build-system-details)
7. [Linking Against the Runtime](#linking-against-the-runtime)
8. [Building a Game Project](#building-a-game-project)
9. [Troubleshooting](#troubleshooting)

---

## Prerequisites

### Required

| Tool | Minimum Version | Purpose |
|------|----------------|---------|
| **CMake** | 3.20 | Build system generator |
| **C Compiler** | C17 support | Compile runtime and HLE modules |
| **C++ Compiler** | C++20 support | Compile templates, module registration |
| **Python** | 3.9+ | Recompiler tools |

### Recommended

| Tool | Purpose |
|------|---------|
| **Ninja** | Faster builds than Make/MSBuild (`pip install ninja`) |
| **ccache** | Compiler cache for faster rebuilds |

### Platform-Specific Requirements

**Windows:**
- Visual Studio 2022 (MSVC 17.x) or Clang/LLVM 14+
- Windows SDK 10.0.19041+
- System libraries (auto-linked): `ws2_32.lib`, `xinput.lib`, `ole32.lib`, `bcrypt.lib`

**Linux:**
- GCC 12+ or Clang 14+
- Development headers: `libpthread`, `libm`
- Optional: SDL2 development package (`libsdl2-dev`) for input/audio

**macOS:**
- Xcode 14+ (Apple Clang 14+)
- Optional: SDL2 via Homebrew (`brew install sdl2`)

---

## Building the Runtime Library

### Quick Start

```bash
# Clone
git clone https://github.com/sp00nznet/ps3recomp.git
cd ps3recomp

# Configure and build
cmake -B build -G Ninja
cmake --build build
```

### Step by Step

#### 1. Configure

```bash
# Using Ninja (recommended)
cmake -B build -G Ninja

# Using Make
cmake -B build -G "Unix Makefiles"

# Using Visual Studio
cmake -B build -G "Visual Studio 17 2022" -A x64

# Using Xcode
cmake -B build -G Xcode

# With specific build type
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

#### 2. Build

```bash
cmake --build build

# Parallel build (if not using Ninja which is parallel by default)
cmake --build build -j$(nproc)

# Build specific target
cmake --build build --target ps3recomp_runtime
```

#### 3. Verify

```bash
# Check that the library was built
ls build/libps3recomp_runtime.a       # Linux/macOS
dir build\ps3recomp_runtime.lib       # Windows (MSVC)
dir build\libps3recomp_runtime.a      # Windows (MinGW)
```

#### 4. Install (Optional)

```bash
cmake --install build --prefix /usr/local
# Installs:
#   /usr/local/lib/libps3recomp_runtime.a
#   /usr/local/include/ps3emu/*.h
```

---

## Build Options

| CMake Option | Default | Description |
|---|---|---|
| `PS3RECOMP_BUILD_TESTS` | `OFF` | Build unit tests |
| `CMAKE_BUILD_TYPE` | — | `Debug`, `Release`, `RelWithDebInfo`, `MinSizeRel` |
| `CMAKE_INSTALL_PREFIX` | platform default | Installation directory |

### Compile Definitions

| Definition | Default | Description |
|---|---|---|
| `PS3_MODULE_MAX_FUNCS` | `512` | Maximum functions per HLE module NID table |
| `PS3_MODULE_MAX_VARS` | `64` | Maximum variables per HLE module NID table |

Override at build time:

```bash
cmake -B build -DPS3_MODULE_MAX_FUNCS=1024
```

---

## Platform-Specific Notes

### Windows

**Link Libraries (auto-linked by CMake):**
- `ws2_32` — Winsock2 for networking (sys_net, cellHttp, cellNet)
- `xinput` — XInput for gamepad (cellPad)
- `ole32` — COM initialization for WASAPI audio (cellAudio)
- `bcrypt` — BCryptGenRandom for cryptographic RNG (cellSsl)

**MSVC Compiler Flags:**
- `/W3` — Warning level 3
- `/wd4100` — Suppress "unreferenced parameter" (common in HLE stubs)
- `/wd4244` — Suppress "conversion" warnings (u64 → u32 in register access)
- `/wd4267` — Suppress "size_t to uint32_t" warnings
- `/wd4996` — Suppress "deprecated" warnings (CRT security warnings)
- `/Zc:__cplusplus` — Report correct C++ standard version

**Definitions:**
- `_CRT_SECURE_NO_WARNINGS` — Suppress MSVC CRT security warnings

### Linux

**Compiler Flags (GCC/Clang):**
- `-Wall -Wextra` — Enable warnings
- `-Wno-unused-parameter` — Suppress for HLE stubs
- `-fno-strict-aliasing` — Required for `be_t<T>` endian conversion through unions

**Link Libraries:**
- `-lpthread` — POSIX threads (linked automatically by most CMake configurations)
- `-lm` — Math library

### macOS

Similar to Linux. Use Xcode command-line tools or a standalone Clang installation.

---

## Compiler Support

### Tested Compilers

| Compiler | Version | Platform | Status |
|----------|---------|----------|--------|
| MSVC | 19.35+ (VS 2022) | Windows | Full support |
| GCC | 12+ | Linux | Full support |
| Clang | 14+ | Linux/macOS | Full support |
| Apple Clang | 14+ (Xcode 14) | macOS | Full support |
| MinGW-w64 | GCC 12+ | Windows | Supported |

### Required C17 Features

- `_Static_assert` / `static_assert`
- Designated initializers
- `__attribute__((aligned))` (GCC/Clang) — used for VMX register alignment

### Required C++20 Features

- `std::is_enum_v`, `std::underlying_type_t` (type traits)
- `constexpr` if
- Designated initializers
- `thread_local` variables
- Lambdas in `constexpr` context

---

## CMake Build System Details

### Source Collection

The CMake build uses `GLOB_RECURSE` to automatically include all `.c` files:

```cmake
# HLE modules — all .c files under libs/
file(GLOB_RECURSE HLE_SOURCES
    "libs/system/*.c"
    "libs/filesystem/*.c"
    "libs/input/*.c"
    "libs/audio/*.c"
    "libs/video/*.c"
    "libs/network/*.c"
    "libs/spurs/*.c"
    "libs/sync/*.c"
    "libs/codec/*.c"
    "libs/font/*.c"
    "libs/misc/*.c"
    "libs/hardware/*.c"
)

# Runtime core
file(GLOB_RECURSE RUNTIME_SOURCES
    "runtime/*.c"
    "runtime/*.cpp"
)
```

**Adding a new module:** Just create a `.c` file in the appropriate `libs/` subdirectory. CMake will pick it up automatically on the next configure.

**Adding a new category:** Add a new `GLOB_RECURSE` pattern for `libs/newcategory/*.c`.

### Include Directories

```cmake
# Public headers (available to downstream projects)
target_include_directories(ps3recomp_runtime PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# Private headers (for internal HLE module use)
target_include_directories(ps3recomp_runtime PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/system"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/filesystem"
    # ... all libs/ subdirs ...
)
```

### Library Target

The build produces a single **static library**: `ps3recomp_runtime`.

This library contains:
- All HLE module implementations (77+ modules)
- Runtime core (memory, threading, syscalls)
- Module registration framework
- NID lookup tables

---

## Linking Against the Runtime

### From a Game Project

```cmake
# In your game's CMakeLists.txt

# Option 1: Add ps3recomp as a subdirectory
add_subdirectory(path/to/ps3recomp)
target_link_libraries(my_game PRIVATE ps3recomp_runtime)

# Option 2: Find an installed ps3recomp
find_library(PS3RECOMP_LIB ps3recomp_runtime PATHS /usr/local/lib)
find_path(PS3RECOMP_INCLUDE ps3emu/ps3types.h PATHS /usr/local/include)
target_link_libraries(my_game PRIVATE ${PS3RECOMP_LIB})
target_include_directories(my_game PRIVATE ${PS3RECOMP_INCLUDE})

# Option 3: Specify path directly
target_link_libraries(my_game PRIVATE
    /path/to/ps3recomp/build/libps3recomp_runtime.a
)
target_include_directories(my_game PRIVATE
    /path/to/ps3recomp/include
)
```

### Required Link Dependencies

On Windows, your game project must also link:
```cmake
target_link_libraries(my_game PRIVATE
    ws2_32 xinput ole32 bcrypt
)
```

On Linux with SDL2:
```cmake
find_package(SDL2 REQUIRED)
target_link_libraries(my_game PRIVATE
    SDL2::SDL2 pthread m
)
```

---

## Building a Game Project

### Using the Template

```bash
# Copy the template
cp -r ps3recomp/templates/project/ my_game/

# Edit configuration
# - config.toml: set elf_path, module settings, graphics/audio backends
# - CMakeLists.txt: set project name, add ps3recomp path
# - stubs.cpp: add game-specific function overrides

# Run the recompiler (generates C source files)
python ps3recomp/tools/ppu_lifter.py disasm/ --output my_game/recompiled/

# Build
cd my_game
cmake -B build -G Ninja -DPS3RECOMP_DIR=/path/to/ps3recomp
cmake --build build

# Run
./build/my_game
```

### Template Files

| File | Purpose |
|------|---------|
| `main.cpp` | Entry point — initializes runtime subsystems, loads function table, creates main PPU thread |
| `stubs.cpp` | Game-specific NID overrides and custom function implementations |
| `config.toml` | Recompiler and runtime configuration |
| `CMakeLists.txt` | Build configuration — links against ps3recomp_runtime |

### Configuration Reference (config.toml)

See `templates/project/config.toml` for the full reference with comments. Key sections:

```toml
[input]
elf_path = "EBOOT.ELF"

[output]
output_dir = "recompiled/"
func_prefix = "recomp_"

[modules]
cellSysutil = "hle"    # Use HLE stub
cellGcmSys = "hle"
# Set to "lle" to recompile the actual PRX module code

[memory]
main_ram_mb = 256
vram_mb = 256

[threading]
max_ppu_threads = 64
max_spu_threads = 6

[graphics]
backend = "vulkan"      # "vulkan", "d3d12", "opengl", "null"

[audio]
backend = "sdl"         # "wasapi", "sdl", "null"

[debug]
log_hle_calls = true
log_missing_nids = true
break_on_unimplemented = false
```

---

## Troubleshooting

### "fatal error: 'ps3emu/ps3types.h' file not found"

The include path is not set. Make sure your CMakeLists.txt includes:
```cmake
target_include_directories(my_target PRIVATE /path/to/ps3recomp/include)
```

### "undefined reference to `vm_base`" / "unresolved external symbol vm_base"

`vm_base` is declared `extern` in `vm.h` but must be defined in exactly one translation unit. The runtime's `runtime/memory/vm.c` defines it. Make sure you're linking `ps3recomp_runtime`.

### "undefined reference to `g_ps3_module_registry`"

Same issue — link against `ps3recomp_runtime`.

### MSVC: "C2065: 'be_t': undeclared identifier"

You're compiling a `.c` file that includes `ps3types.h`. The `be_t<T>` template is C++ only. Either:
- Rename the file to `.cpp`
- Only use C-compatible types (`u32`, `u64`, etc.) in C files

### GCC/Clang: "warning: 'u128_t' attribute ignored [-Wattributes]"

The `__attribute__((aligned(16)))` on `u128` in a struct may warn if the alignment exceeds the struct's natural alignment. This is safe to ignore.

### Linking errors with Winsock2 on Windows

Make sure `ws2_32` is in your link libraries. The CMake build does this automatically for `ps3recomp_runtime`, but if you're building separately you need to add it manually.

### "cannot find -lSDL2"

Install SDL2 development package:
- Ubuntu/Debian: `sudo apt install libsdl2-dev`
- Fedora: `sudo dnf install SDL2-devel`
- macOS: `brew install sdl2`
- Windows: Download from libsdl.org, set `SDL2_DIR` in CMake

### Build takes too long

- Use Ninja (`-G Ninja`) instead of Make for better parallelism
- Use ccache: `cmake -B build -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache`
- Use a Release build type (fewer debug symbols): `-DCMAKE_BUILD_TYPE=Release`
