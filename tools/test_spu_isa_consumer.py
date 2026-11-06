#!/usr/bin/env python3
"""Actual CMake C/C++ consumers inherit the SDK's probed SPU ISA contract."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]

def run(command,**kwargs):
    result=subprocess.run(command,capture_output=True,text=True,timeout=120,**kwargs)
    assert result.returncode==0,(command,result.returncode,result.stdout,result.stderr)
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name,default in [('cmake',os.environ.get('CMAKE') or shutil.which('cmake')),
                         ('ninja',os.environ.get('NINJA') or shutil.which('ninja')),
                         ('cc',os.environ.get('CC') or shutil.which('cc')),
                         ('cxx',os.environ.get('CXX') or shutil.which('c++'))]:
        p.add_argument('--'+name,default=default)
    a=p.parse_args()
    if not all((a.cmake,a.ninja,a.cc,a.cxx)):
        print('SKIP: CMake, Ninja and C/C++ compilers required');return 0
    msvc=Path(a.cxx).name.lower() in ('clang-cl','clang-cl.exe','cl','cl.exe')
    with tempfile.TemporaryDirectory(prefix='spu-isa-consumer-') as directory:
        out=Path(directory);source=ROOT/'runtime/spu/tests/test_spu_isa_consumer.c'
        (out/'consumer.c').write_text('#include "'+source.as_posix()+'"\n')
        (out/'consumer.cpp').write_text('#include "'+source.as_posix()+'"\n')
        (out/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(spu_isa_consumer C CXX)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_subdirectory("{ROOT.as_posix()}" sdk EXCLUDE_FROM_ALL)
# Match direct game sources plus independently compiled object-library consumers.
add_executable(consumer_c consumer.c)
target_link_libraries(consumer_c PRIVATE ps3recomp_runtime)
add_library(consumer_cxx OBJECT consumer.cpp)
target_link_libraries(consumer_cxx PRIVATE ps3recomp_runtime)
if(PS3RECOMP_HAS_MSSE41)
    target_compile_definitions(consumer_c PRIVATE SPU_CONSUMER_REQUIRE_SSE41=1)
    target_compile_definitions(consumer_cxx PRIVATE SPU_CONSUMER_REQUIRE_SSE41=1)
endif()
''')
        for label,extra in [('probed',[]),('probe-false',['-DPS3RECOMP_HAS_MSSE41:BOOL=OFF'])]:
            build=out/label
            run([a.cmake,'-S',str(out),'-B',str(build),'-G','Ninja',
                 '-DCMAKE_MAKE_PROGRAM='+a.ninja,'-DCMAKE_BUILD_TYPE=Release',
                 '-DCMAKE_C_COMPILER='+a.cc,'-DCMAKE_CXX_COMPILER='+a.cxx,
                 '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
                 *extra])
            cache=(build/'CMakeCache.txt').read_text()
            enabled=bool(re.search(r'^PS3RECOMP_HAS_MSSE41:(?:INTERNAL|BOOL)=(?:1|ON|TRUE)$',cache,re.M))
            commands=json.loads((build/'compile_commands.json').read_text())
            consumers=[row for row in commands if Path(row['file']).parent==out]
            assert len(consumers)==2
            for row in consumers:
                # Build only the real generated object rules. The semantic
                # fixture is header-only and needs no SDK duplicate archive.
                cmd=row['command']
                obj=row.get('output')
                if not obj:
                    match=re.search(r'/Fo([^ ]+)|(?:^| )-o ([^ ]+)',cmd);assert match,cmd
                    obj=(match[1] or match[2]).strip('"')
                if Path(obj).is_absolute():obj=os.path.relpath(obj,build)
                obj=obj.replace('\\','/')
                run([a.ninja,'-C',str(build),obj])
                suffix=Path(row['file']).suffix
                exe=out/(label+('-cxx' if suffix=='.cpp' else '-c')+('.exe' if os.name=='nt' else ''))
                if msvc:run([a.cxx,'/nologo','/MT',str(build/obj),'/Fe'+str(exe)])
                else:run([a.cxx,str(build/obj),'-pthread','-lm','-o',str(exe)])
                result=run([str(exe)])
                print(label,suffix,result.stdout.strip(),flush=True)
                assert ('-msse4.1' in cmd)==enabled,cmd
                if enabled:
                    objdump=shutil.which('llvm-objdump') or str(Path(a.cxx).with_name('llvm-objdump.exe'))
                    if Path(objdump).is_file():
                        assembly=run([objdump,'-d','--disassemble-symbols=spu_consumer_shuffle',str(build/obj)]).stdout
                        assert re.search(r'\bv?pshufb\b',assembly),'x86 consumer wrapper lacks shuffle fast path'
            if label=='probe-false':assert not enabled,'Forced unsupported-probe path ignored'
    print('PASS actual CMake C/C++ ISA usage, architectural byte/selector/alias semantics, and unsupported-probe fallback')

if __name__=='__main__':main()
