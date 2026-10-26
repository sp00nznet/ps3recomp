# ps3recomp - tests/syscalls
#
# Focused unit tests for the pure logic of individual lv2 syscall handlers.
# The first one is syscall 14's address/flags classification: split into
# runtime/syscalls/lv2_lock_line.c precisely so this test can link one
# translation unit instead of the whole syscall layer.
#
# Standalone project: its OWN build dir, never the runtime's build tree.
#
# Build on Linux / macOS:
#
#   cmake -S tests/syscalls -B tests/syscalls/build -G Ninja && ninja -C tests/syscalls/build
#
# Run:
#
#   ./tests/syscalls/build/test_lock_line
#
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(syscall_tests LANGUAGES C)
    enable_testing()
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

get_filename_component(PS3RECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(test_lock_line
    test_lock_line.c
    "${PS3RECOMP_ROOT}/runtime/syscalls/lv2_lock_line.c"
)

target_include_directories(test_lock_line PRIVATE
    "${PS3RECOMP_ROOT}/runtime/syscalls"
    "${PS3RECOMP_ROOT}/runtime"
    "${PS3RECOMP_ROOT}/include"
)

if(MSVC)
    target_compile_definitions(test_lock_line PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_lock_line PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

add_test(NAME lock_line COMMAND test_lock_line)
set_tests_properties(lock_line PROPERTIES
    LABELS "runtime;syscalls"
    TIMEOUT 30
)

# sys_ppu_thread_join / detach through the real sys_ppu_thread.c with host
# threads: the slot a join frees stays free (Windows), self-join, second joiner.
add_executable(test_ppu_thread_join test_ppu_thread_join.c)
target_include_directories(test_ppu_thread_join PRIVATE
    "${PS3RECOMP_ROOT}/runtime/syscalls"
    "${PS3RECOMP_ROOT}/runtime"
    "${PS3RECOMP_ROOT}/include"
)
if(MSVC)
    target_compile_definitions(test_ppu_thread_join PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_ppu_thread_join PRIVATE -Wall -Wextra -Wno-unused-parameter -UNDEBUG)
    find_package(Threads REQUIRED)
    target_link_libraries(test_ppu_thread_join PRIVATE Threads::Threads)
endif()
add_test(NAME ppu_thread_join COMMAND test_ppu_thread_join)
set_tests_properties(ppu_thread_join PROPERTIES
    LABELS "runtime;syscalls"
    TIMEOUT 120
)
