cmake_minimum_required(VERSION 3.20)
project(lbp_ps3
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "LittleBigPlanet, recompiled via ps3recomp"
)

# ---------------------------------------------------------------------------
# Toolchain: the generated code + runtime glue use Clang/GCC builtins
# (__int128, __builtin_clz, __builtin_bswap, __attribute__((weak))), so this
# project MUST be built with clang-cl. Configure with:
#   cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
# ---------------------------------------------------------------------------
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(WARNING "This project requires clang-cl; CXX compiler is "
                    "${CMAKE_CXX_COMPILER_ID}. The generated code uses __int128 "
                    "and will not compile under MSVC.")
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# ps3recomp SDK root (this project lives at <sdk>/minecraft/)
# ---------------------------------------------------------------------------
if(NOT DEFINED PS3RECOMP_DIR)
    get_filename_component(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
endif()
message(STATUS "ps3recomp SDK: ${PS3RECOMP_DIR}")

# Pre-built runtime library (built from the SDK root with MSVC or clang-cl;
# clang-cl targets the MSVC ABI so linking against either is fine).
set(PS3RECOMP_RUNTIME_LIB "${PS3RECOMP_DIR}/build/Release/ps3recomp_runtime.lib"
    CACHE FILEPATH "Path to the pre-built ps3recomp_runtime static library")
if(NOT EXISTS "${PS3RECOMP_RUNTIME_LIB}")
    message(FATAL_ERROR "runtime lib not found at ${PS3RECOMP_RUNTIME_LIB} -- "
                        "build the SDK root first (cmake --build build --config Release).")
endif()

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
# Generated lifted code (37k functions across chunk TUs).
file(GLOB RECOMP_CHUNKS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/recompiled/ppu_recomp_*.cpp")
list(LENGTH RECOMP_CHUNKS N_CHUNKS)
message(STATUS "lifted chunks: ${N_CHUNKS}")

# Lifted SPU images + generated workload registration. cellSpurs (in the runtime
# lib) dispatches these by content fingerprint when the game adds a SPURS
# workload/task/jobchain. gen/spu_workloads.c auto-registers all images at
# startup (constructor). The lifted sources use bare #include "spu_context.h"
# (rewritten by build_spu_workloads.py), resolved via the runtime/spu -I below.
option(LBP_ENABLE_SPU "Compile + register the title's lifted SPU images" ON)
if(LBP_ENABLE_SPU)
    file(GLOB SPU_LIFTED CONFIGURE_DEPENDS
        "${PS3RECOMP_DIR}/lbp_spu/lifted/*/spu_recomp.c")
    list(LENGTH SPU_LIFTED N_SPU)
    message(STATUS "lifted SPU images: ${N_SPU}")
    # Embedded raw SPURS taskset-policy bytes (not a *_recomp.c, so not glob-caught).
    set(SPU_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/gen/spu_workloads.c" ${SPU_LIFTED}
        "${CMAKE_CURRENT_SOURCE_DIR}/spu_dispatch_mt.c"
        "${PS3RECOMP_DIR}/lbp_spu/lifted/taskset_policy/taskset_policy_bytes.c")
else()
    set(SPU_SOURCES "")
endif()

# Per-game runtime glue (excluded from the runtime lib on purpose -- compiled
# against the game's generated ppu_recomp.h). See the root CMakeLists.txt
# EXCLUDE REGEX and docs/PPU_RECOMP.md.
set(PPU_GLUE
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
)

add_executable(lbp_ps3
    main.cpp
    gen/ppu_hle_nids.cpp        # generated NID -> HLE handler table
    ${PPU_GLUE}
    ${RECOMP_CHUNKS}
    ${SPU_SOURCES}              # lifted SPU images + workload registration
)

target_include_directories(lbp_ps3 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/recompiled"   # ppu_recomp.h
    "${PS3RECOMP_DIR}/include"                  # ps3emu/*.h
    "${PS3RECOMP_DIR}/runtime/ppu"              # ppu_context.h etc.
)

# Scope the runtime/spu include to ONLY the lifted SPU sources + registry, so the
# 29 PPU chunk TUs keep an unchanged compile command (ninja stays cached -- adding
# it target-wide forced a full PPU rebuild). The lifted sources use bare
# #include "spu_context.h"; the registry uses "spu_workload.h".
if(LBP_ENABLE_SPU)
    # SPU_USE_DISPATCH_MT: route the lifted SPU_IB_DISPATCH sites through the
    # clang musttail fast path (spu_dispatch_mt.c) instead of the MSVC-built
    # lib resolver, whose plain-call dispatch leaks a frame per guest loop
    # iteration through a computed jump.
    # -O2: the target-wide -O1 exists for the 37MB PPU chunk TUs (compile
    # time); the SPU lifted sources are small and DECODE-HOT (the Bink movie
    # runs entirely in them), so they get full optimization. Appended options
    # come after the target's -O1, and the last -O wins under clang-cl.
    set_source_files_properties(${SPU_SOURCES} PROPERTIES
        INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu"
        COMPILE_DEFINITIONS SPU_USE_DISPATCH_MT
        COMPILE_OPTIONS "-O2")
endif()

# ppu_fs.cpp includes <dirent.h> (POSIX); provide the win32 shim for just that
# TU so the chunk TUs' compile commands are untouched (no needless rebuild).
set_source_files_properties(
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
    PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/compat")

# ---------------------------------------------------------------------------
# Compile options (clang-cl). Generated TUs are huge (~37 MB) -- keep the
# optimizer light for a first boot (correctness over speed; O2 on these is very
# slow), enable /bigobj (obj sections overflow at this size), and silence the
# flood of benign warnings from machine-generated code.
# ---------------------------------------------------------------------------
target_compile_options(lbp_ps3 PRIVATE
    /bigobj
    /EHsc
    $<$<COMPILE_LANGUAGE:CXX>:/std:c++20>
    -O1
    -Wno-everything          # generated code: suppress all warnings
)
target_compile_definitions(lbp_ps3 PRIVATE _CRT_SECURE_NO_WARNINGS)

# ---------------------------------------------------------------------------
# Link
# ---------------------------------------------------------------------------
target_link_libraries(lbp_ps3 PRIVATE
    "${PS3RECOMP_RUNTIME_LIB}"
    d3d12 dxgi d3dcompiler uuid dxguid   # RSX D3D12 backend (dxguid: named IIDs)
    ws2_32 xinput ole32 bcrypt           # net / input / audio-COM / RNG
)

# Emit a linker map (symbol -> RVA) so crash/watchdog RVAs can be resolved to
# func_XXXXXXXX guest functions without a full debug rebuild.
target_link_options(lbp_ps3 PRIVATE "/MAP:lbp_ps3.map")
