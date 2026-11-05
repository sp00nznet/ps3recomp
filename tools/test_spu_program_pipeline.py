#!/usr/bin/env python3
"""Synthetic discovery -> generated registration coverage, no guest assets."""
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parent


def elf(value=7, *, bad_bounds=False, unsupported=False, multi=False, rodata=False):
    # il r3,value; bi r0. Both variants deliberately share entry/LS addresses.
    code = struct.pack('>II', 0x40800003 | (value << 7), 0x35000000)
    if unsupported:
        code = struct.pack('>I', 0x01000000) + code[4:]
    phnum = 2 if multi else 1
    payload = 0x100
    data = bytearray(payload + len(code) + (8 if rodata or multi else 0))
    ident = b'\x7fELF\x01\x02\x01' + b'\x00' * 9
    shoff = len(data) if rodata else 0
    data[:52] = struct.pack('>16sHHIIIIIHHHHHH', ident, 2, 23, 1, 0x100,
                           52, shoff, 0, 52, 32, phnum, 40 if rodata else 0,
                           2 if rodata else 0, 0)
    struct.pack_into('>IIIIIIII', data, 52, 1, payload, 0x100, 0,
                     0x10000 if bad_bounds else len(code) + (8 if rodata else 0),
                     len(code) + (8 if rodata else 0), 5, 16)
    data[payload:payload + len(code)] = code
    if multi:
        struct.pack_into('>IIIIIIII', data, 84, 1, payload + 8, 0x200, 0, 8, 8, 5, 16)
        data[payload + 8:] = code
    if rodata:
        data[payload + 8:] = struct.pack('>II', 0x01000000, 0x01000000)
        data.extend(struct.pack('>IIIIIIIIII', 0, 1, 6, 0x100, payload, 8, 0, 0, 4, 0))
        data.extend(struct.pack('>IIIIIIIIII', 0, 1, 2, 0x108, payload + 8, 8, 0, 0, 4, 0))
    return bytes(data)


class Pipeline(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.output = self.root / 'generated'

    def run_pipeline(self, *inputs, partial=False, extra=()):
        command = [sys.executable, str(TOOLS / 'build_spu_workloads.py'),
                   '--lifted', str(self.output), '--out', str(self.output / 'spu_workloads.c')]
        for name, blob in inputs:
            path = self.root / name
            path.write_bytes(blob)
            command += ['--input', str(path)]
        if partial:
            command.append('--allow-partial')
        result = subprocess.run(command + list(extra), capture_output=True, text=True)
        return result

    def manifest(self):
        return json.loads((self.output / 'inventory.json').read_text())

    def write_runtime_blocks(self):
        runtime = TOOLS.parent / 'runtime' / 'spu'
        # Compile the runtime's actual registry/fingerprint/ELF-size blocks
        # unchanged, without the unrelated DMA/job/platform dependency graph.
        workload = (runtime / 'spu_workload.c').read_text()
        workload = workload[workload.index('uint64_t spu_workload_fingerprint('):
                            workload.index('/* ---- dispatch')]
        (self.root / 'workload.c').write_text('#include "spu_workload.h"\n#include <stdio.h>\n#include <string.h>\n' + workload)
        channels = (runtime / 'spu_channels.c').read_text()
        channels = channels[channels.index('typedef void (*spu_fn)'):
                            channels.index('/* Which image registered')]
        # SPU_TLS is defined earlier in spu_channels.c, outside the extracted block.
        tls = '#if defined(_MSC_VER)\n#define SPU_TLS __declspec(thread)\n#else\n#define SPU_TLS __thread\n#endif\n'
        (self.root / 'registry.c').write_text('#include "spu_context.h"\n#include <stdio.h>\n' + tls + channels)
        return runtime

    def test_exact_dedup_and_same_address_isolation(self):
        result = self.run_pipeline(('container.bin', b'prefix' + elf(7) + elf(9) + elf(7)))
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        report = self.manifest()
        self.assertTrue(report['complete'])
        self.assertEqual(len(report['programs']), 2)
        self.assertEqual(sum(len(p['occurrences']) for p in report['programs']), 3)
        self.assertEqual(len({p['image_id'] for p in report['programs']}), 2)
        for p in report['programs']:
            self.assertGreater(p['image_id'], 22)
            self.assertEqual(p['status'], 'registered')
            self.assertTrue((self.output / p['name'] / 'spu_recomp.c').exists())
        self.assertTrue((self.output / 'sources.cmake').exists())

    def test_bad_bounds_and_unsupported_are_reported_without_registration(self):
        for blob, status in [(elf(bad_bounds=True), 'invalid'), (elf(unsupported=True), 'unsupported'),
                             (elf(multi=True), 'unsupported')]:
            with self.subTest(status=status):
                result = self.run_pipeline(('input.bin', blob))
                self.assertNotEqual(result.returncode, 0)
                report = self.manifest()
                self.assertFalse(report['complete'])
                self.assertEqual(report['programs'][0]['status'], status)
                self.assertFalse((self.output / 'spu_workloads.c').exists())
                self.assertFalse((self.output / 'sources.cmake').exists())

    def test_partial_is_explicit_and_rodata_is_not_lifted(self):
        result = self.run_pipeline(('input.bin', elf(rodata=True) + elf(unsupported=True)), partial=True)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        report = self.manifest()
        self.assertFalse(report['complete'])
        self.assertEqual(sorted(p['status'] for p in report['programs']), ['registered', 'unsupported'])
        registered = next(p for p in report['programs'] if p['status'] == 'registered')
        source = (self.output / registered['name'] / 'spu_recomp.c').read_text()
        self.assertNotIn('TODO spu', source)

    def test_fresh_rerun_cannot_register_stale_code(self):
        first = self.run_pipeline(('input.bin', elf(7)))
        self.assertEqual(first.returncode, 0, first.stderr)
        old_name = self.manifest()['programs'][0]['name']
        second = self.run_pipeline(('input.bin', elf(unsupported=True)))
        self.assertNotEqual(second.returncode, 0)
        self.assertFalse((self.output / old_name).exists())
        self.assertFalse((self.output / 'spu_workloads.c').exists())

    def test_ambiguous_text_fingerprint_is_rejected(self):
        a = elf(7)
        b = bytearray(a)
        b[15] = 1  # Same loaded code, different exact ELF identity.
        result = self.run_pipeline(('input.bin', a + b))
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(all(p['status'] == 'unsupported' for p in self.manifest()['programs']))
        self.assertFalse((self.output / 'spu_workloads.c').exists())

    def test_strict_mixed_inventory_omits_all_registrations(self):
        result = self.run_pipeline(('input.bin', elf(7) + elf(unsupported=True)))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(sorted(p['status'] for p in self.manifest()['programs']), ['generated', 'unsupported'])
        self.assertFalse((self.output / 'spu_workloads.c').exists())

    def test_changed_generation_options_cannot_leave_stale_registry(self):
        first = self.run_pipeline(('input.bin', elf(7)))
        self.assertEqual(first.returncode, 0, first.stderr)
        second = self.run_pipeline(('input.bin', elf(7)), extra=('--extra-funcs', 'unknown=0x100'))
        self.assertNotEqual(second.returncode, 0)
        self.assertFalse((self.output / 'spu_workloads.c').exists())
        self.assertFalse(self.manifest()['complete'])
        self.assertTrue(self.manifest()['errors'])

    def test_extractor_bounds_and_runtime_extent(self):
        sys.path.insert(0, str(TOOLS))
        from extract_spu_images import find_spu_images, parse_spu_elf_size
        original = elf(rodata=True)
        self.assertEqual(parse_spu_elf_size(original, 0), len(original))
        self.assertEqual(list(find_spu_images(b'prefix' + original + original)),
                         [(6, len(original)), (6 + len(original), len(original))])
        # Malformed PH/SH table dimensions and LS bounds must be rejected.
        for offset, fmt, value in [(42, '>H', 1), (28, '>I', len(original)),
                                   (46, '>H', 1), (32, '>I', len(original)),
                                   (60, '>I', 0x40000), (72, '>I', 1)]:
            malformed = bytearray(original)
            struct.pack_into(fmt, malformed, offset, value)
            self.assertIsNone(parse_spu_elf_size(malformed, 0), (offset, value))
        # Non-LOAD content past the code is still in the runtime fingerprint.
        nonload = bytearray(elf(multi=True))
        struct.pack_into('>I', nonload, 84, 4)
        self.assertEqual(parse_spu_elf_size(nonload, 0), len(nonload))

    def test_missing_lifted_entry_is_an_explicit_failure(self):
        blob = bytearray(elf())
        symbol_offset = len(blob)
        blob.extend(struct.pack('>IIIBBH', 0, 0x100, 8, 2, 0, 0))
        section_offset = len(blob)
        blob.extend(struct.pack('>IIIIIIIIII', 0, 1, 6, 0x100, 0x100, 8, 0, 0, 4, 0))
        blob.extend(struct.pack('>IIIIIIIIII', 0, 2, 0, 0, symbol_offset, 16, 0, 0, 4, 16))
        struct.pack_into('>I', blob, 24, 0x104)
        struct.pack_into('>I', blob, 32, section_offset)
        struct.pack_into('>H', blob, 46, 40)
        struct.pack_into('>H', blob, 48, 2)
        result = self.run_pipeline(('input.bin', blob))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('ELF entry', self.manifest()['programs'][0]['reason'])
        self.assertFalse((self.output / 'spu_workloads.c').exists())

    def test_load_overwrite_and_bss_overlap_are_rejected(self):
        cc = shutil.which('clang') or shutil.which('cc')
        for bss in (False, True):
            with self.subTest(bss=bss):
                blob = bytearray(elf(7, multi=True))
                # The later non-executable load writes onto the first load's
                # code. In BSS mode its zero-filled tail reaches the same code.
                struct.pack_into('>I', blob, 84 + 24, 4)
                if bss:
                    struct.pack_into('>I', blob, 84 + 8, 0xf0)
                    struct.pack_into('>I', blob, 84 + 16, 0)
                    struct.pack_into('>I', blob, 84 + 20, 24)
                else:
                    struct.pack_into('>I', blob, 84 + 8, 0x100)
                    struct.pack_into('>I', blob, 0x108, 0x40800003 | (9 << 7))
                result = self.run_pipeline(('input.bin', blob))
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                program = self.manifest()['programs'][0]
                self.assertEqual(program['status'], 'unsupported')
                self.assertIn('overlapping PT_LOAD', program['reason'])
                self.assertFalse((self.output / 'spu_workloads.c').exists())
                if not cc:
                    if __import__('os').environ.get('CI'):
                        self.fail('C compiler required for runtime overwrite control in CI')
                    continue
                # Negative control: the compatibility CLI still demonstrates
                # why accepting this layout is unsafe. Compile its generated
                # entry against the actual runtime loader and observe different
                # loaded instructions versus the instructions baked into C.
                images = self.root / 'legacy_images'
                images.mkdir(exist_ok=True)
                (images / 'sample.elf').write_bytes(blob)
                output = self.root / 'legacy_lift'
                subprocess.run([sys.executable, str(TOOLS / 'build_spu_workloads.py'),
                                '--images', str(images), '--lifted', str(output),
                                '--out', str(output / 'registry.c'), '--relift'],
                               check=True, capture_output=True, text=True)
                runtime = self.write_runtime_blocks()
                byte_list = ','.join(str(x) for x in blob)
                loaded = 0 if bss else 9
                control = (
                    '#include "spu_workload.h"\n#include <assert.h>\n'
                    'extern void spu_register_all(void);\n'
                    'SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);\n'
                    'void spu_indirect_branch(spu_context* ctx) { (void)ctx; assert(0); }\n'
                    'unsigned char image[] = {' + byte_list + '};\n'
                    'int main(void) { unsigned char ls[SPU_LS_SIZE] = {0}; uint32_t entry;\n'
                    'assert(spu_elf_load_to_ls(image, sizeof image, ls, &entry));\n'
                    'uint32_t word = ((uint32_t)ls[0x100]<<24) | ((uint32_t)ls[0x101]<<16) | '
                    '((uint32_t)ls[0x102]<<8) | ls[0x103];\n'
                    'assert(((word>>7)&65535) == ' + str(loaded) + ');\n'
                    'assert(word != 0x40800383u);\n'
                    'spu_register_all(); spu_lifted_entry_fn f = spu_workload_find('
                    'spu_workload_fingerprint(image, spu_elf_image_size(image, sizeof image))); assert(f);\n'
                    'spu_context ctx = {0}; ctx.ls = ls; f(&ctx);\n'
                    'assert(ctx.gpr[3]._u32[0] == 7); return 0; }\n')
                (self.root / 'overlap_control.c').write_text(control)
                binary = self.root / ('overlap_control.exe' if sys.platform == 'win32' else 'overlap_control')
                compiled = subprocess.run([cc, '-std=c11', '-I' + str(runtime),
                                str(self.root / 'overlap_control.c'), str(self.root / 'workload.c'),
                                str(self.root / 'registry.c'), str(output / 'registry.c'),
                                str(output / 'sample' / 'spu_recomp.c'), '-o', str(binary)],
                               capture_output=True, text=True)
                self.assertEqual(compiled.returncode, 0, compiled.stderr + compiled.stdout)
                subprocess.run([str(binary)], check=True)

    def test_compiled_runtime_registration_and_cmake(self):
        cc = shutil.which('clang') or shutil.which('cc')
        cmake = shutil.which('cmake')
        if not cc or not cmake:
            if __import__('os').environ.get('CI'):
                self.fail('C compiler and CMake required in CI')
            self.skipTest('C compiler or CMake unavailable')
        note = bytearray(elf(13, multi=True))
        struct.pack_into('>I', note, 84, 4)
        result = self.run_pipeline(('input.bin', elf(7) + elf(9) + elf(11, rodata=True) + note))
        self.assertEqual(result.returncode, 0, result.stderr)
        programs = self.manifest()['programs']
        runtime = self.write_runtime_blocks()
        body = []
        for i, program in enumerate(programs):
            blob = (self.output / program['name'] / 'image.elf').read_bytes()
            immediate = (struct.unpack_from('>I', blob, 0x100)[0] >> 7) & 0xffff
            body += [f'unsigned char image{i}[] = {{' + ','.join(str(x) for x in blob) + '};',
                     f'assert(spu_elf_image_size(image{i}, sizeof image{i}) == sizeof image{i});',
                     f'uint64_t fp{i} = spu_workload_fingerprint(image{i}, sizeof image{i});',
                     f'assert(fp{i} == 0x{program["fingerprint"]}ULL);',
                     f'int id{i} = 0; spu_lifted_entry_fn f{i} = spu_workload_find_img(fp{i}, &id{i});',
                     f'assert(f{i} && id{i} == {program["image_id"]});',
                     f'assert(spu_lookup(0x100, id{i}) == f{i});',
                     # Each context contains a 256 KB local store. Keep the
                     # four fixtures off Windows' default 1 MB main stack.
                     f'static spu_context c{i}; f{i}(&c{i});',
                     f'assert(c{i}.gpr[3]._u32[0] == {immediate});']
        body += ['assert(f0 != f1 && id0 != id1);', 'assert(spu_workload_count() == 8);']
        (self.root / 'proof.c').write_text(
            '#include "spu_workload.h"\n#include <assert.h>\n'
            'extern void spu_register_all(void);\n'
            'extern spu_lifted_entry_fn spu_lookup(uint32_t, int);\n'
            'SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);\n'
            'void spu_indirect_branch(spu_context* ctx) { (void)ctx; assert(0); }\n'
            'int main(void) { spu_register_all();\n' + '\n'.join(body) + '\nreturn 0; }\n')
        # Minimal consumer uses the same CMake generation helper as the template.
        cmake_source = 'cmake_minimum_required(VERSION 3.20)\nproject(proof C)\n'
        cmake_source += f'set(PS3RECOMP_DIR "{TOOLS.parent.as_posix()}")\n'
        cmake_source += 'include("${PS3RECOMP_DIR}/cmake/PS3RecompSPU.cmake")\n'
        cmake_source += 'ps3recomp_generate_spu("${CMAKE_CURRENT_BINARY_DIR}/spu" "${CMAKE_CURRENT_SOURCE_DIR}/input.bin")\n'
        cmake_source += 'add_executable(proof proof.c workload.c registry.c ${SPU_GENERATED_SOURCES})\n'
        cmake_source += f'target_include_directories(proof PRIVATE "{runtime.as_posix()}")\n'
        (self.root / 'CMakeLists.txt').write_text(cmake_source)
        build = self.root / 'build'
        # Visual Studio generators select their own toolset instead of this
        # compiler, and put the executable in a configuration subdirectory.
        # Ninja honors the selected Clang and the same executable path as POSIX.
        generator = ['-G', 'Ninja'] if sys.platform == 'win32' else []
        configured = subprocess.run([cmake, '-S', str(self.root), '-B', str(build), *generator,
                                     '-DCMAKE_C_COMPILER=' + cc,
                                     '-DPython3_EXECUTABLE=' + sys.executable], capture_output=True, text=True)
        self.assertEqual(configured.returncode, 0, configured.stderr + configured.stdout)
        built = subprocess.run([cmake, '--build', str(build)], capture_output=True, text=True)
        self.assertEqual(built.returncode, 0, built.stderr + built.stdout)
        subprocess.run([str(build / ('proof.exe' if sys.platform == 'win32' else 'proof'))], check=True)
        # A changed input must reconfigure and fail coverage instead of linking
        # the previously successful registration on an incremental build.
        (self.root / 'input.bin').write_bytes(elf(unsupported=True))
        rebuild = subprocess.run([cmake, '--build', str(build)], capture_output=True, text=True)
        self.assertNotEqual(rebuild.returncode, 0)
        self.assertIn('SPU generation incomplete', rebuild.stdout + rebuild.stderr)
        self.assertFalse((build / 'spu' / 'spu_workloads.c').exists())

    def digests(self, *blobs):
        import hashlib
        return [hashlib.sha256(b).hexdigest() for b in blobs]

    def test_handled_programs_are_covered_without_a_second_registration(self):
        a, b = elf(7), elf(9)
        da, db = self.digests(a, b)
        handled = self.root / 'handled.json'
        handled.write_text(json.dumps({da: 'registered by the title build as image 102'}))
        result = self.run_pipeline(('input.bin', a + b), extra=['--handled', str(handled)])
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        report = self.manifest()
        self.assertTrue(report['complete'])
        by = {p['sha256']: p for p in report['programs']}
        self.assertEqual(by[da]['status'], 'handled')
        self.assertEqual(by[da]['reason'], 'registered by the title build as image 102')
        self.assertFalse((self.output / by[da]['name']).exists())
        self.assertEqual(by[db]['status'], 'registered')
        registry = (self.output / 'spu_workloads.c').read_text()
        self.assertNotIn(by[da]['fingerprint'], registry)
        self.assertIn(by[db]['fingerprint'], registry)

    def test_pinned_image_ids_are_used_and_required(self):
        a, b = elf(7), elf(9)
        da, db = self.digests(a, b)
        ids = self.root / 'ids.json'
        ids.write_text(json.dumps({da: 120, db: 121}))
        result = self.run_pipeline(('input.bin', a + b), extra=['--image-ids', str(ids)])
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        report = self.manifest()
        self.assertEqual({p['sha256']: p['image_id'] for p in report['programs']}, {da: 120, db: 121})
        self.assertIn('spu_begin_image(120)', (self.output / 'spu_workloads.c').read_text())
        # A discovered program without a pinned id fails coverage instead of
        # taking a content-derived id.
        ids.write_text(json.dumps({da: 120}))
        result = self.run_pipeline(('input.bin', a + b), extra=['--image-ids', str(ids)])
        self.assertNotEqual(result.returncode, 0)
        report = self.manifest()
        self.assertFalse(report['complete'])
        self.assertEqual({p['sha256']: p['status'] for p in report['programs']}[db], 'unsupported')

    def test_stale_handled_or_pinned_entries_fail(self):
        stale = self.root / 'stale.json'
        stale.write_text(json.dumps({'0' * 64: 'gone'}))
        result = self.run_pipeline(('input.bin', elf(7)), extra=['--handled', str(stale)])
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.manifest()['complete'])
        self.assertTrue(any('not discovered' in e for e in self.manifest()['errors']))

    def test_legacy_images_mode_still_builds(self):
        images = self.root / 'images'
        images.mkdir()
        (images / 'sample.elf').write_bytes(elf())
        result = subprocess.run([sys.executable, str(TOOLS / 'build_spu_workloads.py'),
                                 '--images', str(images), '--lifted', str(self.output),
                                 '--out', str(self.output / 'registry.c')], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertIn('sample_spu_func_00000100', (self.output / 'registry.c').read_text())


if __name__ == '__main__':
    unittest.main()
