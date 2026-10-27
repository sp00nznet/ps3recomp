cmake_minimum_required(VERSION 3.20)
project(torture
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "ps3recomp lifter torture-test harness"
)

# Must be built with clang-cl (generated code uses __int128 etc.). Configure:
#   cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(WARNING "This project requires clang-cl; CXX compiler is "
                    "${CMAKE_CXX_COMPILER_ID}.")
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ps3recomp SDK root (this project lives at <sdk>/tests/torture/)
if(NOT DEFINED PS3RECOMP_DIR)
    get_filename_component(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()
message(STATUS "ps3recomp SDK: ${PS3RECOMP_DIR}")

set(PS3RECOMP_RUNTIME_LIB "${PS3RECOMP_DIR}/build/Release/ps3recomp_runtime.lib"
    CACHE FILEPATH "Path to the pre-built ps3recomp_runtime static library")
if(NOT EXISTS "${PS3RECOMP_RUNTIME_LIB}")
    message(FATAL_ERROR "runtime lib not found at ${PS3RECOMP_RUNTIME_LIB} -- "
                        "build the SDK root first (cmake --build build --config Release).")
endif()

file(GLOB RECOMP_CHUNKS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/recompiled/ppu_recomp_*.cpp")
list(LENGTH RECOMP_CHUNKS N_CHUNKS)
message(STATUS "lifted chunks: ${N_CHUNKS}")

set(PPU_GLUE
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
)

add_executable(torture
    main.cpp
    gen/ppu_hle_nids.cpp
    ${PPU_GLUE}
    ${RECOMP_CHUNKS}
)

target_include_directories(torture PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/recompiled"
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"
)

# ppu_fs.cpp includes <dirent.h> (POSIX); reuse vkcube's win32 shim for that TU.
set_source_files_properties(
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
    PROPERTIES INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/vkcube/compat")

target_compile_options(torture PRIVATE
    /bigobj
    /EHsc
    $<$<COMPILE_LANGUAGE:CXX>:/std:c++20>
    -O1
    -Wno-everything
)
target_compile_definitions(torture PRIVATE _CRT_SECURE_NO_WARNINGS)

# The runtime lib is monolithic (RSX D3D12 backend included), so the full link
# line is required even though torture guests never touch GCM.
target_link_libraries(torture PRIVATE
    "${PS3RECOMP_RUNTIME_LIB}"
    d3d12 dxgi d3dcompiler uuid dxguid
    ws2_32 xinput ole32 bcrypt
)

target_link_options(torture PRIVATE "/MAP:torture.map")
