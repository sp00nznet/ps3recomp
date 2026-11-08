#!/usr/bin/env python3
"""Real emitted nonlocal transfers through a Windows CMake runtime consumer.

Requires clang-cl, CMake and Ninja on PATH (or CXX/CMAKE/NINJA overrides).
Only the consumer objects are built: its bridge is extracted from the actual
loader, so linking the SDK archive as well would duplicate those symbols.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

import test_ppu_nonlocal as N


def run(command, **kwargs):
    result = subprocess.run(command, capture_output=True, text=True, timeout=120, **kwargs)
    assert result.returncode == 0, (command, result.returncode, result.stdout, result.stderr)
    return result


def main():
    if os.name != 'nt':
        print('SKIP Windows C++ bridge exception contract')
        return
    compiler = os.environ.get('CXX') or shutil.which('clang-cl')
    cmake = os.environ.get('CMAKE') or shutil.which('cmake')
    ninja = os.environ.get('NINJA') or shutil.which('ninja')
    assert compiler and cmake and ninja, 'clang-cl, CMake and Ninja required'
    with tempfile.TemporaryDirectory(prefix='ppu-nonlocal-windows-') as directory:
        out = Path(directory)
        N.validation_tests(out)
        lifter = N.make_lifter()
        (out/'ppu_recomp.h').write_text(lifter.emit_header())
        (out/'lifted.cpp').write_text(lifter.emit_source())
        module = N.make_lifter(prefix='module_')
        module.header_name = 'module.h'
        (out/'module.h').write_text(module.emit_header())
        (out/'module.cpp').write_text(module.emit_source())
        (out/'driver.cpp').write_text('#include <windows.h>\n'+N.runtime_driver())
        root = N.ROOT.as_posix()
        (out/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(nonlocal_windows C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_subdirectory("{root}" sdk EXCLUDE_FROM_ALL)
add_library(nonlocal_consumer OBJECT lifted.cpp module.cpp driver.cpp
    "{root}/runtime/syscalls/ppu_host_unwind.c")
target_link_libraries(nonlocal_consumer PRIVATE ps3recomp_runtime)
target_include_directories(nonlocal_consumer PRIVATE "{root}/runtime/ppu" "{root}/runtime")
target_compile_definitions(nonlocal_consumer PRIVATE PPU_NO_INLINE_LOADS HAVE_NONLOCAL)
target_compile_options(nonlocal_consumer PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/O1> /UNDEBUG)
''')
        build = out/'build'
        run([cmake, '-S', str(out), '-B', str(build), '-G', 'Ninja',
             '-DCMAKE_MAKE_PROGRAM='+ninja, '-DCMAKE_BUILD_TYPE=Release',
             '-DCMAKE_C_COMPILER='+compiler, '-DCMAKE_CXX_COMPILER='+compiler,
             '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
             '-DPS3RECOMP_PPU_NONLOCAL_CONTROL_FLOW=ON',
             '-DPS3RECOMP_FFMPEG_RUNTIME=OFF'])
        commands = json.loads((build/'compile_commands.json').read_text())
        consumer = [row for row in commands if 'nonlocal_consumer.dir' in row['command']]
        assert len(consumer) == 4
        objects = []
        for row in consumer:
            # Build the exact CMake-generated object rule, retaining its usage
            # requirements without pulling the synthetic bridge's SDK duplicate.
            obj = row['output'] if 'output' in row else row['command'].split('/Fo', 1)[1].split()[0].strip('"')
            obj = os.path.relpath(obj, build).replace('\\', '/') if Path(obj).is_absolute() else obj
            run([ninja, '-C', str(build), obj])
            objects.append(str(build/obj))
        exe = out/'nonlocal.exe'
        run([compiler, '/nologo', '/MT', *objects, '/Fe'+str(exe)])
        for mode in ['basic', 'zero', 'recursive', 'outer', 'unrelated', 'crossmodule']:
            result = subprocess.run([str(exe), mode], capture_output=True, text=True, timeout=20)
            assert result.returncode == 0, (mode, result.returncode, result.stdout, result.stderr)
        for mode in ['wrongsp', 'wrongtarget', 'wrongctx', 'wrongthread', 'stale']:
            result = subprocess.run([str(exe), mode], capture_output=True, text=True, timeout=20)
            assert result.returncode != 0 and 'no live continuation' in result.stderr, (mode, result.returncode, result.stderr)
        # Windows CRT longjmp runs C++ destructors after the explicit host-exit
        # hook has cleared its registry. Unlike POSIX, it rejects during that
        # unwind, before reentry. Preserve this existing fail-closed distinction;
        # this test does not claim POSIX host-exit recovery on Windows.
        result = subprocess.run([str(exe), 'hostexit'], capture_output=True, text=True, timeout=20)
        assert result.returncode != 0 and 'corrupt continuation lifetime' in result.stderr, (result.returncode, result.stderr)
        # Also assert the production runtime and independent C++ consumer both
        # receive the override after CMake's default /EHsc; C sources do not.
        for row in commands:
            cmd = row['command']
            if Path(row['file']).suffix.lower() == '.cpp':
                assert '/EHsc-' in cmd and cmd.rfind('/EHsc-') > cmd.find('/EHsc'), cmd
            elif Path(row['file']).suffix.lower() == '.c':
                assert '/EHsc-' not in cmd, cmd
    print('PASS Windows CMake C++ consumer: 11 transfer/rejection modes plus distinct CRT host-exit rejection; dispatcher cleanup, registers, TLS, 4100 repeats; C++-only EH propagation')


if __name__ == '__main__':
    main()
