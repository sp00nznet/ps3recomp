"""CPU-only point-sprite text and production glslang/SPIR-V regressions."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from msvc_env import environ
root=Path(__file__).resolve().parents[1].as_posix()
with tempfile.TemporaryDirectory(prefix="rsx-point-sprite-") as directory:
    d=Path(directory)
    (d/"CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.20)
project(point_sprite_tests LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
add_compile_options(-UNDEBUG)
find_package(Threads REQUIRED)
if(WIN32)
  find_package(glslang CONFIG QUIET)
else()
  find_package(glslang CONFIG REQUIRED)
endif()
add_executable(point_sprite "{root}/libs/video/tests/test_rsx_point_sprite.c")
if(glslang_FOUND)
add_executable(point_spirv "{root}/libs/video/tests/test_point_sprite_spirv.c" "{root}/libs/video/rsx_shader_spirv.cpp")
target_compile_definitions(point_spirv PRIVATE PS3RECOMP_HAVE_SPIRV_TRANSLATION=1)
target_include_directories(point_spirv PRIVATE "{root}/include" "{root}/libs/video")
target_link_libraries(point_spirv PRIVATE glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits)
if(NOT WIN32)
add_executable(shader_cache "{root}/libs/video/tests/test_shader_spirv.c" "{root}/libs/video/rsx_shader_spirv.cpp")
target_compile_definitions(shader_cache PRIVATE PS3RECOMP_HAVE_SPIRV_TRANSLATION=1)
target_include_directories(shader_cache PRIVATE "{root}/include" "{root}/libs/video")
target_link_libraries(shader_cache PRIVATE glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits)
endif()
file(WRITE "${{CMAKE_BINARY_DIR}}/has_spirv" "1")
endif()
''')
    env=environ()
    config=["cmake","-S",str(d),"-B",str(d/"build"),"-DCMAKE_BUILD_TYPE=Release"]
    if os.name == 'nt':
        config += ["-G", "Ninja", "-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl"]
    subprocess.run(config,env=env,check=True)
    subprocess.run(["cmake","--build",str(d/"build"),"--parallel","2"],env=env,check=True)
    names=["point_sprite"]
    if (d/"build/has_spirv").exists():
        names += ["point_spirv"]
        if os.name != 'nt':
            names += ["shader_cache"]
        else:
            print("SKIP: the shared SPIR-V disk-cache fixture uses POSIX dirent; translation still runs")
    else:
        print("SKIP: SPIR-V translation/cache checks require glslang; point-sprite CPU checks still run")
    for name in names:
        subprocess.run([str(d/"build"/name)],cwd=d,check=True,timeout=60)
    validator=shutil.which("spirv-val")
    if validator and "point_spirv" in names:
        for p in sorted(d.glob("*.spv")):
            subprocess.run([validator,"--target-env","vulkan1.0",str(p)],check=True)
        print("PASS: generated SPIR-V validates for Vulkan 1.0")
    elif "point_spirv" in names:
        print("SKIP: spirv-val unavailable; translation assertions passed")
