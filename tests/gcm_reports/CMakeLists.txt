# ps3recomp - tests/gcm_reports
#
# NV4097_SET_CONTEXT_DMA_REPORT + NV4097_GET_REPORT through the cellGcm FIFO
# walker, with no graphics backend and no lifted game: a hand-built command
# ring, run once, then the report slots are read back from the host report
# area and from guest memory. Built with the runtime (PS3RECOMP_BUILD_TESTS).
#
#   ctest --test-dir build-tests -R gcm_reports --output-on-failure

add_executable(test_gcm_reports
    main.c
    "${PROJECT_SOURCE_DIR}/runtime/host/host_stubs.c"
)
target_link_libraries(test_gcm_reports PRIVATE ps3recomp_runtime)
if(NOT WIN32)
    target_link_libraries(test_gcm_reports PRIVATE m)
endif()
target_include_directories(test_gcm_reports PRIVATE
    "${PROJECT_SOURCE_DIR}/libs/video"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/runtime"
)
# The runtime carries C++, so the C++ driver does the final link.
set_target_properties(test_gcm_reports PROPERTIES LINKER_LANGUAGE CXX)
add_test(NAME gcm_reports COMMAND test_gcm_reports)
