# ps3recomp - tests/gcm_fifo_jump
#
# The cellGcm FIFO walker's JUMP decode, old (0x20000000 | offset) and new
# (offset | 1), with no graphics backend and no lifted game. Built with the
# runtime (PS3RECOMP_BUILD_TESTS).
#
#   ctest --test-dir build-tests -R gcm_fifo_jump --output-on-failure

add_executable(test_gcm_fifo_jump
    main.c
    "${PROJECT_SOURCE_DIR}/runtime/host/host_stubs.c"
)
target_link_libraries(test_gcm_fifo_jump PRIVATE ps3recomp_runtime)
if(NOT WIN32)
    target_link_libraries(test_gcm_fifo_jump PRIVATE m)
endif()
target_include_directories(test_gcm_fifo_jump PRIVATE
    "${PROJECT_SOURCE_DIR}/libs/video"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/runtime"
)
# The runtime carries C++, so the C++ driver does the final link.
set_target_properties(test_gcm_fifo_jump PROPERTIES LINKER_LANGUAGE CXX)
add_test(NAME gcm_fifo_jump COMMAND test_gcm_fifo_jump)
