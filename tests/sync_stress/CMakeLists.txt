# ps3recomp - tests/sync_stress
#
# Deterministic lv2 sync-primitive stress suite.
# Standalone project: its OWN build dir, never yakuza\build.
#
# Build (chain in ONE command -- Launch-VsDevShell is blocked in this harness;
# shell state does not persist between tool calls):
#
#   cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"" >nul && cmake -S tests\sync_stress -B tests\sync_stress\build -G Ninja && ninja -C tests\sync_stress\build"
#
# Run:
#   .\tests\sync_stress\build\sync_stress.exe [seed]
#
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(sync_stress LANGUAGES C)
    enable_testing()
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Location of the ps3recomp runtime sources under test. Overridable so a
# seeded-bug self-test can point PS3RECOMP_TREE_ROOT at a temp MIRROR of the
# runtime/+include/ tree (e.g. with sys_cond.c mutated) without touching the
# real one. The mirror must keep the same relative layout (runtime/syscalls,
# runtime/ppu, runtime/memory, include/ps3emu) because the runtime headers
# use relative includes like "../ppu/ppu_context.h".
#   cmake -DPS3RECOMP_TREE_ROOT=C:\path\to\temp\tree ...
#   (RUNTIME_SYNC_DIR alone can also be overridden if only the four .c/.h
#   sync files were copied into an otherwise-normal tree layout.)
# ---------------------------------------------------------------------------
get_filename_component(PS3RECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

if(NOT DEFINED PS3RECOMP_TREE_ROOT)
    set(PS3RECOMP_TREE_ROOT "${PS3RECOMP_ROOT}")
endif()

if(NOT DEFINED RUNTIME_SYNC_DIR)
    set(RUNTIME_SYNC_DIR "${PS3RECOMP_TREE_ROOT}/runtime/syscalls")
endif()

add_executable(sync_stress
    main.c
    test_shims.c
    "${RUNTIME_SYNC_DIR}/sys_mutex.c"
    "${RUNTIME_SYNC_DIR}/sys_cond.c"
    "${RUNTIME_SYNC_DIR}/sys_semaphore.c"
    "${RUNTIME_SYNC_DIR}/sys_event.c"
)

target_include_directories(sync_stress PRIVATE
    "${RUNTIME_SYNC_DIR}"
    "${PS3RECOMP_TREE_ROOT}/runtime"
    "${PS3RECOMP_TREE_ROOT}/include"
)

if(MSVC)
    target_compile_options(sync_stress PRIVATE
        /W3 /wd4100 /wd4244 /wd4267
        /experimental:c11atomics
    )
    target_compile_definitions(sync_stress PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(sync_stress PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(sync_stress PRIVATE pthread)
endif()

add_test(NAME sync_stress COMMAND sync_stress)
set_tests_properties(sync_stress PROPERTIES
    LABELS "stress;runtime;sync"
    TIMEOUT 120
)
