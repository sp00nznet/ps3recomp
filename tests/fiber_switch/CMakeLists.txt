# ps3recomp - tests/fiber_switch
#
# cellFiber cooperative switching on the host's own mechanism: Windows fibers
# on Windows, ucontext elsewhere. Standalone project with its own build dir,
# and also added from the top-level CMakeLists under PS3RECOMP_BUILD_TESTS.
#
#   cmake -S tests/fiber_switch -B build-fiber -G Ninja
#   cmake --build build-fiber && ./build-fiber/fiber_switch
#
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(fiber_switch LANGUAGES C)
    enable_testing()
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

get_filename_component(PS3RECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
if(NOT DEFINED PS3RECOMP_TREE_ROOT)
    set(PS3RECOMP_TREE_ROOT "${PS3RECOMP_ROOT}")
endif()

# cellFiber.c is compiled directly rather than linked from the runtime
# library, so this stays a fast standalone unit test with no HLE, RSX or SDL2
# dependency -- the same arrangement tests/sync_stress uses for the lv2 sync
# sources.
add_executable(fiber_switch
    main.c
    "${PS3RECOMP_TREE_ROOT}/libs/spurs/cellFiber.c"
)

target_include_directories(fiber_switch PRIVATE
    "${PS3RECOMP_TREE_ROOT}/libs/spurs"
    "${PS3RECOMP_TREE_ROOT}/include"
)

if(MSVC)
    target_compile_options(fiber_switch PRIVATE /W3 /wd4100)
    target_compile_definitions(fiber_switch PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    # Apple deprecated getcontext/makecontext/swapcontext in 10.6 and never
    # removed them; this suite is the evidence that they still work, so the
    # warning is noise rather than news.
    target_compile_options(fiber_switch PRIVATE
        -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations)
endif()

add_test(NAME fiber_switch COMMAND fiber_switch)
set_tests_properties(fiber_switch PROPERTIES
    LABELS "runtime;fiber"
    TIMEOUT 60
)
