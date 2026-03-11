cmake_minimum_required(VERSION 3.20)
project(MyGameRecomp
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "Recompiled PS3 game project (ps3recomp)"
)

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefer Ninja if available
if(NOT CMAKE_GENERATOR MATCHES "Ninja" AND NOT DEFINED ENV{CMAKE_GENERATOR})
    message(STATUS "Tip: use -G Ninja for faster builds")
endif()

# ---------------------------------------------------------------------------
# ps3recomp SDK location
# ---------------------------------------------------------------------------
# Set PS3RECOMP_DIR to the root of your ps3recomp checkout, or pass it on
# the command line:   cmake -DPS3RECOMP_DIR=/path/to/ps3recomp ..
if(NOT DEFINED PS3RECOMP_DIR)
    # Default: assume this project is inside ps3recomp/templates/project/
    get_filename_component(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../" ABSOLUTE)
endif()
message(STATUS "ps3recomp SDK: ${PS3RECOMP_DIR}")

# ---------------------------------------------------------------------------
# ps3recomp runtime library (built from the SDK's root CMakeLists.txt)
# ---------------------------------------------------------------------------
# If the runtime has already been built, find it.  Otherwise, add it as a
# subdirectory so it builds alongside this project.
set(PS3RECOMP_RUNTIME_DIR "${PS3RECOMP_DIR}/build" CACHE PATH
    "Directory containing the pre-built ps3recomp_runtime library")

if(EXISTS "${PS3RECOMP_RUNTIME_DIR}/libps3recomp_runtime.a" OR
   EXISTS "${PS3RECOMP_RUNTIME_DIR}/ps3recomp_runtime.lib")
    add_library(ps3recomp_runtime STATIC IMPORTED GLOBAL)
    if(WIN32)
        set_target_properties(ps3recomp_runtime PROPERTIES
            IMPORTED_LOCATION "${PS3RECOMP_RUNTIME_DIR}/ps3recomp_runtime.lib")
    else()
        set_target_properties(ps3recomp_runtime PROPERTIES
            IMPORTED_LOCATION "${PS3RECOMP_RUNTIME_DIR}/libps3recomp_runtime.a")
    endif()
    target_include_directories(ps3recomp_runtime INTERFACE
        "${PS3RECOMP_DIR}/include")
else()
    message(STATUS "Building ps3recomp runtime from source")
    add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk"
                     EXCLUDE_FROM_ALL)
endif()

# ---------------------------------------------------------------------------
# Recompiled game executable
# ---------------------------------------------------------------------------
add_executable(${PROJECT_NAME}
    main.cpp
    stubs.cpp

    # Add your recompiled .c / .cpp files here:
    # recompiled/func_00000100.c
    # recompiled/func_00000200.c
)

target_include_directories(${PROJECT_NAME} PRIVATE
    "${PS3RECOMP_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ps3recomp_runtime
)

# ---------------------------------------------------------------------------
# Compiler-specific flags
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
    )
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
    )
elseif(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3 /wd4100 /wd4244 /wd4267
        /Zc:__cplusplus         # report correct __cplusplus value
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
