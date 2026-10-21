cmake_minimum_required(VERSION 3.20)
project(rsx_fsr3_bridge LANGUAGES CXX)

# Optional C ABI DLL around the AMD FSR 3.1.4 upscaler (FidelityFX SDK 1.1.4,
# MIT). No AMD file is vendored: FFX_SDK_DIR is a checkout of tag v1.1.4
# (c6efa6bf), verified file by file against ffx-sdk-1.1.4.sha256 and used
# unmodified. Shader permutations are generated with the local glslang by
# gen_fsr3_blobs.py instead of the SDK's prebuilt FidelityFX_SC.exe.
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The FSR3 bridge targets 64-bit Windows")
endif()
set(FFX_SDK_DIR "" CACHE PATH "FidelityFX SDK v1.1.4 checkout (c6efa6bf)")
set(SPIRV_TOOLS_DIR "" CACHE PATH "Directory with glslangValidator, spirv-val, spirv-cross and spirv-dis")
set(VULKAN_INCLUDE_DIR "" CACHE PATH "Directory containing vulkan/vulkan.h")
set(VULKAN_IMPORT_LIBRARY "" CACHE FILEPATH "Vulkan loader import library")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
foreach(required "${VULKAN_INCLUDE_DIR}/vulkan/vulkan.h" "${VULKAN_IMPORT_LIBRARY}"
                 "${SPIRV_TOOLS_DIR}/glslangValidator.exe" "${SPIRV_TOOLS_DIR}/spirv-cross.exe")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "Missing explicit dependency: ${required}")
    endif()
endforeach()

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/ffx-sdk-1.1.4.sha256" ffx_manifest REGEX "^[0-9a-f]+  ")
set(ffx_shaders)
foreach(line IN LISTS ffx_manifest)
    string(STRIP "${line}" line)
    string(REGEX MATCH "^([0-9a-f]+)  (.+)$" _ "${line}")
    set(expected "${CMAKE_MATCH_1}")
    set(path "${FFX_SDK_DIR}/${CMAKE_MATCH_2}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "FidelityFX input missing: ${path}")
    endif()
    file(SHA256 "${path}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "FidelityFX input differs from v1.1.4 as stored upstream "
                            "(check out with core.autocrlf=false): ${path}")
    endif()
    if(path MATCHES "/shaders/fsr3upscaler/.*\\.glsl$")
        list(APPEND ffx_shaders "${path}")
    endif()
endforeach()
list(LENGTH ffx_manifest ffx_count)
message(STATUS "FidelityFX SDK v1.1.4: ${ffx_count} input files verified")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/ffx-sdk-1.1.4.sha256")

set(blob_dir "${CMAKE_CURRENT_BINARY_DIR}/fsr3-permutations")
set(blob_manifest "${blob_dir}/generation-manifest.json")
add_custom_command(
    OUTPUT "${blob_manifest}"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${blob_dir}"
    COMMAND Python3::Interpreter -I "${CMAKE_CURRENT_LIST_DIR}/gen_fsr3_blobs.py"
            "${FFX_SDK_DIR}" "${blob_dir}" --tools "${SPIRV_TOOLS_DIR}"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/gen_fsr3_blobs.py" ${ffx_shaders}
    COMMENT "Generating FSR3 upscaler SPIR-V permutations with the local glslang"
    VERBATIM)
add_custom_target(fsr3_permutations DEPENDS "${blob_manifest}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(ffx_sources
    "${FFX_SDK_DIR}/sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/ffx_vk_unit.cpp"  # ffx_vk.cpp, unmodified, plus layout queries
    "${FFX_SDK_DIR}/sdk/src/backends/shared/ffx_shader_blobs.cpp"
    "${FFX_SDK_DIR}/sdk/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp"
    "${FFX_SDK_DIR}/sdk/src/shared/ffx_assert.cpp"
    "${FFX_SDK_DIR}/sdk/src/shared/ffx_message.cpp"
    "${FFX_SDK_DIR}/sdk/src/shared/ffx_object_management.cpp"
    "${FFX_SDK_DIR}/sdk/src/shared/ffx_breadcrumbs_list.cpp")
add_library(rsx_fsr3_vk SHARED "${SDK_ROOT}/libs/video/rsx_fsr3_vk.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/ffx_vk_no_frame_generation.cpp" ${ffx_sources})
add_dependencies(rsx_fsr3_vk fsr3_permutations)
set_target_properties(rsx_fsr3_vk PROPERTIES PREFIX "")
target_include_directories(rsx_fsr3_vk PUBLIC "${SDK_ROOT}/libs/video" "${VULKAN_INCLUDE_DIR}")
target_include_directories(rsx_fsr3_vk SYSTEM PRIVATE
    "${FFX_SDK_DIR}/sdk/include" "${FFX_SDK_DIR}/sdk/include/FidelityFX/host"
    "${FFX_SDK_DIR}/sdk/src" "${FFX_SDK_DIR}/sdk/src/shared" "${FFX_SDK_DIR}/sdk/src/components"
    "${FFX_SDK_DIR}/sdk/src/backends/shared" "${blob_dir}")
# FFX_GCC keeps FFX_API empty, so only the rsx_fsr3_* C ABI is exported.
target_compile_definitions(rsx_fsr3_vk PRIVATE FFX_GCC FFX_FSR3UPSCALER MINGW_HAS_SECURE_API=1)
set_source_files_properties(${ffx_sources} PROPERTIES
    COMPILE_OPTIONS "-include;${CMAKE_CURRENT_LIST_DIR}/ffx_compat_mingw.h;-w")
target_compile_options(rsx_fsr3_vk PRIVATE -O2)
set_source_files_properties("${SDK_ROOT}/libs/video/rsx_fsr3_vk.cpp" PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
target_link_libraries(rsx_fsr3_vk PRIVATE "${VULKAN_IMPORT_LIBRARY}")

option(RSX_FSR3_BUILD_FIXTURE "Build the synthetic GPU consumer test" OFF)
if(RSX_FSR3_BUILD_FIXTURE)
    add_executable(test_vulkan_fsr3_consumer "${SDK_ROOT}/tests/test_vulkan_fsr3_consumer.cpp")
    target_compile_options(test_vulkan_fsr3_consumer PRIVATE -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers)
    target_link_libraries(test_vulkan_fsr3_consumer PRIVATE rsx_fsr3_vk "${VULKAN_IMPORT_LIBRARY}")
endif()
