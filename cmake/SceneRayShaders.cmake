# Embed source, not opaque compiler output, so runtime shader diagnostics use
# the exact reviewed kernels and never depend on a working-directory path.
function(rsx_scene_embed_string name source out)
    file(READ "${source}" text)
    string(REPLACE "\r" "" text "${text}")
    string(REPLACE "\\" "\\\\" text "${text}")
    string(REPLACE "\"" "\\\"" text "${text}")
    string(REPLACE "\n" "\\n\"\n\"" text "${text}")
    set(${out} "static const char ${name}[] =\n\"${text}\";\n" PARENT_SCOPE)
endfunction()
set(rsx_scene_shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/libs/video")
set(rsx_scene_shader_sources
    "${rsx_scene_shader_dir}/rsx_scene_gather.comp"
    "${rsx_scene_shader_dir}/rsx_scene_trace.comp"
    "${rsx_scene_shader_dir}/rsx_scene_alpha.glsl")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${rsx_scene_shader_sources})
rsx_scene_embed_string(rsx_scene_gather_glsl "${rsx_scene_shader_dir}/rsx_scene_gather.comp" gather)
rsx_scene_embed_string(rsx_scene_trace_glsl "${rsx_scene_shader_dir}/rsx_scene_trace.comp" trace)
rsx_scene_embed_string(rsx_scene_alpha_glsl "${rsx_scene_shader_dir}/rsx_scene_alpha.glsl" alpha)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/scene")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/scene/rsx_scene_shader_sources.h"
    "/* Generated from reviewed GLSL sources by SceneRayShaders.cmake. */\n#pragma once\n${gather}${trace}${alpha}")
include_directories("${CMAKE_CURRENT_BINARY_DIR}/generated/scene")
target_include_directories(ps3recomp_runtime PRIVATE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated/scene>")
