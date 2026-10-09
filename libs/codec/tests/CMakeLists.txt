# cellVdec, driven through its guest API (test_vdec_decode.c).

if(UNIX)
    set(VDEC_TEST_LIBS m)
endif()

# The callback-only decoder, as every build without FFmpeg has it.
add_executable(test_vdec_stub test_vdec_decode.c)
target_include_directories(test_vdec_stub PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(test_vdec_stub PRIVATE ${VDEC_TEST_LIBS})
add_test(NAME vdec_stub COMMAND test_vdec_stub stub)

if(NOT PS3RECOMP_FFMPEG_RUNTIME)
    return()
endif()

add_executable(test_vdec_ffmpeg test_vdec_decode.c)
target_include_directories(test_vdec_ffmpeg PRIVATE
    "${PROJECT_SOURCE_DIR}/include" "${PS3RECOMP_FFMPEG_INCLUDE_DIR}")
target_compile_definitions(test_vdec_ffmpeg PRIVATE PS3RECOMP_FFMPEG_RUNTIME)
target_link_libraries(test_vdec_ffmpeg PRIVATE ${VDEC_TEST_LIBS} ${CMAKE_DL_LIBS})

# The library the tests load: wherever the system's libavcodec is, which need
# not be on the loader's search path (Nix, a local FFmpeg build).
find_library(PS3RECOMP_AVCODEC_LIBRARY NAMES avcodec)
set(VDEC_LIB_ENV "")
if(PS3RECOMP_AVCODEC_LIBRARY AND UNIX)
    get_filename_component(VDEC_LIB_DIR "${PS3RECOMP_AVCODEC_LIBRARY}" DIRECTORY)
    if(APPLE)
        set(VDEC_LIB_ENV "DYLD_LIBRARY_PATH=${VDEC_LIB_DIR}")
    else()
        set(VDEC_LIB_ENV "LD_LIBRARY_PATH=${VDEC_LIB_DIR}")
    endif()
endif()

# Off at run time, or no library: the same callback-only behaviour.
add_test(NAME vdec_ffmpeg_disabled COMMAND test_vdec_ffmpeg stub)
set_tests_properties(vdec_ffmpeg_disabled PROPERTIES
    ENVIRONMENT "PS3RECOMP_FFMPEG=0;${VDEC_LIB_ENV}")
add_test(NAME vdec_ffmpeg_missing COMMAND test_vdec_ffmpeg stub)
set_tests_properties(vdec_ffmpeg_missing PROPERTIES ENVIRONMENT
    "PS3RECOMP_FFMPEG_AVCODEC=${CMAKE_CURRENT_BINARY_DIR}/no-such-libavcodec;${VDEC_LIB_ENV}")

# Real decode needs streams, made by the ffmpeg CLI at test time.
find_program(PS3RECOMP_FFMPEG_EXECUTABLE ffmpeg)
if(NOT PS3RECOMP_FFMPEG_EXECUTABLE)
    message(STATUS "  FFmpeg:       no ffmpeg program; cellVdec decode tests not added")
    return()
endif()
set(VDEC_MEDIA "${CMAKE_CURRENT_BINARY_DIR}/vdec_media")
add_test(NAME vdec_media_gen COMMAND "${CMAKE_COMMAND}"
    -DFFMPEG=${PS3RECOMP_FFMPEG_EXECUTABLE} -DOUT=${VDEC_MEDIA}
    -P "${CMAKE_CURRENT_SOURCE_DIR}/gen_vdec_media.cmake")
set_tests_properties(vdec_media_gen PROPERTIES FIXTURES_SETUP vdec_media)

foreach(c avc mpeg2)
    if(c STREQUAL "avc")
        set(es h264)
    else()
        set(es m2v)
    endif()
    add_test(NAME vdec_decode_${c} COMMAND test_vdec_ffmpeg decode ${c}
        ${VDEC_MEDIA}/v.${es} ${VDEC_MEDIA}/ref_${es}.yuv 192 108)
    add_test(NAME vdec_rgba_${c} COMMAND test_vdec_ffmpeg rgba ${c}
        ${VDEC_MEDIA}/v.${es} ${VDEC_MEDIA}/ref_${es}.rgba 192 108)
    # A real stream, with the library switched off, still gets the old path.
    add_test(NAME vdec_disabled_${c} COMMAND test_vdec_ffmpeg stub ${c} ${VDEC_MEDIA}/v.${es})
    set_tests_properties(vdec_disabled_${c} PROPERTIES
        ENVIRONMENT "PS3RECOMP_FFMPEG=0;${VDEC_LIB_ENV}")
    set_tests_properties(vdec_decode_${c} vdec_rgba_${c} PROPERTIES
        ENVIRONMENT "${VDEC_LIB_ENV}" SKIP_RETURN_CODE 77)
    set_tests_properties(vdec_decode_${c} vdec_rgba_${c} vdec_disabled_${c}
        PROPERTIES FIXTURES_REQUIRED vdec_media)
endforeach()
