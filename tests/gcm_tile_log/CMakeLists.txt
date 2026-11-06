# ps3recomp - tests/gcm_tile_log
#
# The per-call lines of cellGcmSetTile / SetZcull / BindTile / BindZcull /
# UnbindTile / UnbindZcull are printed only when ps3_log_verbose() is on.
# Built with the runtime (PS3RECOMP_BUILD_TESTS).
#
#   ctest --test-dir build-tests -R gcm_tile_log --output-on-failure

add_executable(test_gcm_tile_log
    main.c
    "${PROJECT_SOURCE_DIR}/runtime/host/host_stubs.c"
)
target_link_libraries(test_gcm_tile_log PRIVATE ps3recomp_runtime)
if(NOT WIN32)
    target_link_libraries(test_gcm_tile_log PRIVATE m)
endif()
target_include_directories(test_gcm_tile_log PRIVATE
    "${PROJECT_SOURCE_DIR}/libs/video"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/runtime"
)
# The runtime carries C++, so the C++ driver does the final link.
set_target_properties(test_gcm_tile_log PROPERTIES LINKER_LANGUAGE CXX)
# ps3_log_verbose() reads PS3_VERBOSE once, so each setting is its own process.
add_test(NAME gcm_tile_log_quiet COMMAND test_gcm_tile_log quiet)
add_test(NAME gcm_tile_log_verbose COMMAND test_gcm_tile_log verbose)
