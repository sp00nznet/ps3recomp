cmake_minimum_required(VERSION 3.20)
project(ps3recomp_runtime
    VERSION 0.3.0
    LANGUAGES C CXX
    DESCRIPTION "ps3recomp - Static recompilation runtime for PS3 games"
)

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(PS3RECOMP_BUILD_TESTS "Build unit tests" OFF)

# ---------------------------------------------------------------------------
# Collect source files
# ---------------------------------------------------------------------------

# HLE library sources
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
# Exclude test harnesses (e.g. libs/video/tests/*): they define their own main().
list(FILTER HLE_SOURCES EXCLUDE REGEX "/tests/")

# Runtime core (syscalls, PPU, SPU, memory)
file(GLOB_RECURSE RUNTIME_SOURCES
    "runtime/*.c"
    "runtime/*.cpp"
)
# Exclude:
#  - test harnesses (runtime/*/tests/*): they have their own main() and #include
#    per-game generated headers (spu_recomp.h) that a plain library build lacks.
#  - the PPU boot scaffold (runtime/ppu/, runtime/host/): these are compiled
#    per-game against the lifter-generated ppu_recomp.h (and the game's RSX/D3D12
#    backend), not as part of the game-agnostic runtime library. See
#    docs/PPU_RECOMP.md.
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/tests/|/runtime/ppu/|/runtime/host/")

# ---------------------------------------------------------------------------
# Static library
# ---------------------------------------------------------------------------
add_library(ps3recomp_runtime STATIC
    ${HLE_SOURCES}
    ${RUNTIME_SOURCES}
)

target_include_directories(ps3recomp_runtime PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# HLE modules include their own headers
target_include_directories(ps3recomp_runtime PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/system"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/filesystem"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/input"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/audio"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/network"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/spurs"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/sync"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/codec"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/font"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/misc"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/spu"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# Platform-specific link libraries
if(WIN32)
    target_link_libraries(ps3recomp_runtime PRIVATE
        ws2_32      # Winsock for network modules
        xinput      # XInput for gamepad
        ole32       # COM for WASAPI audio
        bcrypt      # BCryptGenRandom for SSL RNG
    )
endif()

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(ps3recomp_runtime PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
    )
elseif(MSVC)
    target_compile_options(ps3recomp_runtime PRIVATE
        /W3 /wd4100 /wd4244 /wd4267 /wd4996
        /Zc:__cplusplus
        /experimental:c11atomics   # Enable C11 <stdatomic.h> support
    )
    target_compile_definitions(ps3recomp_runtime PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(PS3RECOMP_BUILD_TESTS)
    include(CTest)
    add_subdirectory(tests/sync_stress)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS ps3recomp_runtime
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(DIRECTORY include/ps3emu
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "ps3recomp runtime v${PROJECT_VERSION}")
message(STATUS "  C compiler:   ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "  C++ compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Build type:   ${CMAKE_BUILD_TYPE}")
message(STATUS "")
