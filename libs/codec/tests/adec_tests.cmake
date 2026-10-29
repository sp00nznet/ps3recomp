# cellAdec AC3, driven through its guest API (test_adec_ac3.c). Included from
# CMakeLists.txt. The streams are made by the ffmpeg program at test time.
find_program(PS3RECOMP_FFMPEG_EXECUTABLE ffmpeg)
if(NOT PS3RECOMP_FFMPEG_EXECUTABLE)
    message(STATUS "  FFmpeg:       no ffmpeg program; cellAdec AC3 tests not added")
    return()
endif()

set(ADEC_MEDIA "${CMAKE_CURRENT_BINARY_DIR}/adec_media")
add_test(NAME adec_media_gen COMMAND "${CMAKE_COMMAND}"
    -DFFMPEG=${PS3RECOMP_FFMPEG_EXECUTABLE} -DOUT=${ADEC_MEDIA}
    -P "${CMAKE_CURRENT_SOURCE_DIR}/gen_adec_media.cmake")
set_tests_properties(adec_media_gen PROPERTIES FIXTURES_SETUP adec_media)

# Without FFmpeg: silence of the real AC3 frame size.
add_executable(test_adec_stub test_adec_ac3.c)
target_include_directories(test_adec_stub PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(test_adec_stub PRIVATE ${VDEC_TEST_LIBS})
add_test(NAME adec_ac3_stub COMMAND test_adec_stub stub
    ${ADEC_MEDIA}/t51.ac3 ${ADEC_MEDIA}/t20.ac3)
set_tests_properties(adec_ac3_stub PROPERTIES FIXTURES_REQUIRED adec_media)

if(NOT PS3RECOMP_FFMPEG_RUNTIME)
    return()
endif()

add_executable(test_adec_ffmpeg test_adec_ac3.c)
target_include_directories(test_adec_ffmpeg PRIVATE
    "${PROJECT_SOURCE_DIR}/include" "${PS3RECOMP_FFMPEG_INCLUDE_DIR}")
target_compile_definitions(test_adec_ffmpeg PRIVATE PS3RECOMP_FFMPEG_RUNTIME)
target_link_libraries(test_adec_ffmpeg PRIVATE ${VDEC_TEST_LIBS} ${CMAKE_DL_LIBS})

# Where the system's libavcodec is, as for the cellVdec tests below.
find_library(PS3RECOMP_AVCODEC_LIBRARY NAMES avcodec)
set(ADEC_LIB_ENV "")
if(PS3RECOMP_AVCODEC_LIBRARY AND UNIX)
    get_filename_component(ADEC_LIB_DIR "${PS3RECOMP_AVCODEC_LIBRARY}" DIRECTORY)
    if(APPLE)
        set(ADEC_LIB_ENV "DYLD_LIBRARY_PATH=${ADEC_LIB_DIR}")
    else()
        set(ADEC_LIB_ENV "LD_LIBRARY_PATH=${ADEC_LIB_DIR}")
    endif()
endif()

add_test(NAME adec_ac3_disabled COMMAND test_adec_ffmpeg stub
    ${ADEC_MEDIA}/t51.ac3 ${ADEC_MEDIA}/t20.ac3)
set_tests_properties(adec_ac3_disabled PROPERTIES
    ENVIRONMENT "PS3RECOMP_FFMPEG=0;${ADEC_LIB_ENV}" FIXTURES_REQUIRED adec_media)
add_test(NAME adec_ac3_decode COMMAND test_adec_ffmpeg decode
    ${ADEC_MEDIA}/t51.ac3 ${ADEC_MEDIA}/t51.f32)
set_tests_properties(adec_ac3_decode PROPERTIES
    ENVIRONMENT "${ADEC_LIB_ENV}" FIXTURES_REQUIRED adec_media SKIP_RETURN_CODE 77)
