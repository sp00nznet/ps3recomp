# ps3recomp - tests/host_fiber
#
# The Apple AArch64 host context switch on its own: a two-sided assembly ABI
# probe, the guard-page stack layout, lifecycle and thread-pinning checks, and
# the fail-closed invariants (each one aborts in a child process). Standalone
# project with its own build dir, and also added from the top-level CMakeLists
# under PS3RECOMP_BUILD_TESTS on Apple arm64.
#
#   cmake -S tests/host_fiber -B build-host-fiber -G Ninja
#   cmake --build build-host-fiber
#   ./build-host-fiber/test_host_fiber_aarch64 abi
#
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(host_fiber LANGUAGES C ASM)
    enable_testing()
else()
    # The top-level project is C/C++ only; the switch and its ABI probe are
    # the one place in the tree that needs an assembler.
    enable_language(ASM)
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

get_filename_component(PS3RECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
if(NOT DEFINED PS3RECOMP_TREE_ROOT)
    set(PS3RECOMP_TREE_ROOT "${PS3RECOMP_ROOT}")
endif()

find_package(Threads REQUIRED)

# The backend is compiled here rather than linked from the runtime library, so
# this stays a unit test of the switch itself: no HLE, RSX or SDL2 dependency.
set(HF_BACKEND
    "${PS3RECOMP_TREE_ROOT}/runtime/platform/host_fiber_aarch64.c"
    "${PS3RECOMP_TREE_ROOT}/runtime/platform/host_fiber_aarch64.S")

add_executable(test_host_fiber_aarch64 main.c abi_probe.S ${HF_BACKEND})
target_include_directories(test_host_fiber_aarch64 PRIVATE
    "${PS3RECOMP_TREE_ROOT}/runtime/platform")
target_link_libraries(test_host_fiber_aarch64 PRIVATE Threads::Threads)
# The frame tests walk the back chain from the fiber's own frames.
target_compile_options(test_host_fiber_aarch64 PRIVATE
    "$<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Wpedantic;-fno-omit-frame-pointer>")

foreach(mode abi frame lifecycle many deep enomem enomem_mprotect nested_distinct annot)
    add_test(NAME host_fiber_${mode} COMMAND test_host_fiber_aarch64 ${mode})
    set_tests_properties(host_fiber_${mode} PROPERTIES LABELS "runtime;fiber" TIMEOUT 120)
endforeach()

# A no-return call from a fiber must not make ASan print that it ignores the
# unwind: that text means the fiber stack bounds it was given are wrong.
add_test(NAME host_fiber_noret COMMAND test_host_fiber_aarch64 noret)
set_tests_properties(host_fiber_noret PROPERTIES LABELS "runtime;fiber" TIMEOUT 30
    FAIL_REGULAR_EXPRESSION "ignoring requested __asan_handle_no_return")

add_test(NAME host_fiber_guard COMMAND test_host_fiber_aarch64 guard)
set_tests_properties(host_fiber_guard PROPERTIES LABELS "stress;runtime;fiber" TIMEOUT 30)

# An un-destroyed TSan fiber state is invisible except in the peak RSS.
add_test(NAME host_fiber_tsan_handoff COMMAND test_host_fiber_aarch64 tsan_handoff)
add_test(NAME host_fiber_tsan_destroy COMMAND test_host_fiber_aarch64 tsan_destroy)
set_tests_properties(host_fiber_tsan_handoff PROPERTIES LABELS "runtime;fiber" TIMEOUT 30)
set_tests_properties(host_fiber_tsan_destroy PROPERTIES LABELS "runtime;fiber" TIMEOUT 60)

# Each stop_ mode aborts on its invariant. The expected op, reason and ea live
# in main.c (stops[]); expect_stop_ runs it in a child and demands SIGABRT
# plus exactly one named line, so a PASS_REGULAR_EXPRESSION would not do.
set(HF_STOPS entry_returned resume_exited resume_running resume_nested resume_destroyed
    uncreated yield_foreign_stack destroy_suspended foreign_thread size mprotect)
foreach(mode ${HF_STOPS})
    add_test(NAME host_fiber_stop_${mode}
        COMMAND test_host_fiber_aarch64 expect_stop_${mode})
    set_tests_properties(host_fiber_stop_${mode} PROPERTIES LABELS "runtime;fiber" TIMEOUT 30)
endforeach()

# Optional release target: raw switch pairs, resume/yield pairs and ucontext
# pairs on the same machine, so the numbers are comparable.
add_executable(bench_host_fiber EXCLUDE_FROM_ALL bench.c ${HF_BACKEND})
target_include_directories(bench_host_fiber PRIVATE
    "${PS3RECOMP_TREE_ROOT}/runtime/platform")
target_link_libraries(bench_host_fiber PRIVATE Threads::Threads)
