#!/usr/bin/env python3
"""Synthetic checks for the project template's build extensions. No game files.

1. Compiles templates/project/port_extension.cpp with its test and runs it:
   inert by default, single-owner slots, fan-out order, refusal when full.
2. Configures a throwaway CMake project with templates/project/cmake/
   PortExtensions.cmake: no extensions is a no-op, listed extensions are
   included in order with PORT_TARGET set, and a missing one fails.
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TEMPLATE = ROOT / 'templates/project'


def compiler():
    name = os.environ.get('CXX')
    for candidate in ([name] if name else []) + ['c++', 'clang++', 'g++', 'clang-cl', 'cl']:
        path = shutil.which(candidate)
        if path:
            return path
    return None


def registry_test(work):
    cxx = compiler()
    if not cxx:
        print('No C++ compiler found')
        return 1 if os.environ.get('CI') else 0
    binary = work / ('registry.exe' if os.name == 'nt' else 'registry')
    sources = [str(TEMPLATE / 'tests/test_port_extension.cpp'), str(TEMPLATE / 'port_extension.cpp')]
    if Path(cxx).stem.lower() in ('clang-cl', 'cl'):
        command = [cxx, '/nologo', '/std:c++17', '/EHsc', *sources, '/Fo' + str(work) + '\\', '/Fe' + str(binary)]
    else:
        command = [cxx, '-std=c++17', '-O1', *sources, '-o', str(binary), '-pthread']
    subprocess.run(command, check=True, timeout=120)
    return subprocess.run([str(binary)], timeout=30).returncode


def configure(work, name, extensions):
    source = work / name
    source.mkdir()
    module = (TEMPLATE / 'cmake/PortExtensions.cmake').as_posix()
    (source / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.20)\n'
        'project(extension_probe NONE)\n'
        'add_custom_target(game)\n'
        f'include("{module}")\n'
        'port_apply_extensions(game)\n'
        'get_target_property(order game PORT_PROBE_ORDER)\n'
        'file(WRITE "${CMAKE_BINARY_DIR}/order.txt" "${order}")\n')
    command = ['cmake', '-S', str(source), '-B', str(source / 'build'),
               '-DPORT_EXTENSIONS=' + ';'.join(p.as_posix() for p in extensions)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=120)
    order = source / 'build/order.txt'
    return result, order.read_text() if order.exists() else None


def cmake_test(work):
    if not shutil.which('cmake'):
        print('No cmake found')
        return 1 if os.environ.get('CI') else 0
    failures = 0
    first, second = work / 'first.cmake', work / 'second.cmake'
    for path, tag in ((first, 'first'), (second, 'second')):
        path.write_text(f'set_property(TARGET ${{PORT_TARGET}} APPEND PROPERTY PORT_PROBE_ORDER {tag})\n')

    result, order = configure(work, 'none', [])
    if result.returncode or order not in ('order-NOTFOUND', ''):
        print('FAIL: no extensions should change nothing', result.stderr[-2000:], order)
        failures += 1
    if 'Build extension' in result.stdout:
        print('FAIL: no extensions should print nothing')
        failures += 1

    result, order = configure(work, 'two', [first, second])
    if result.returncode or order != 'first;second':
        print('FAIL: extensions should be included in order', result.stderr[-2000:], order)
        failures += 1

    result, _ = configure(work, 'missing', [work / 'absent.cmake'])
    if result.returncode == 0 or 'PORT_EXTENSIONS entry not found' not in result.stderr:
        print('FAIL: a missing extension should stop the configure')
        failures += 1
    if not failures:
        print('PortExtensions.cmake: configure checks passed')
    return failures


def main():
    with tempfile.TemporaryDirectory(prefix='port-extension-') as temp:
        work = Path(temp)
        return 1 if (registry_test(work) or cmake_test(work)) else 0


if __name__ == '__main__':
    raise SystemExit(main())
