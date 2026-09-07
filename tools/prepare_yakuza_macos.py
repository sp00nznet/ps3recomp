#!/usr/bin/env python3
"""Configure the external Yakuza macOS runner against this toolkit.

Only generated files in --build-dir are changed. The runner must already have
the PS3RECOMP_DIR split and macOS host support; no game assets are copied.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path
import subprocess
import sys


def replace_once(text, old, new, label):
    if text.count(old) != 1:
        raise SystemExit(f"Unsupported runner revision: expected one {label}")
    return text.replace(old, new, 1)


def write_changed(path, text):
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def prepare_shader(toolkit, game, build):
    """Lift the already relocated module; never copy game assets into source."""
    module = game / 'recomp_prx' / 'ogrez_shader_ps3.ppu'
    image = Path(str(module) + '_image.bin')
    functions = Path(str(module) + '_functions.json')
    output = build / 'shader-module'
    inputs = [image, functions, toolkit / 'tools/ppu_lifter.py']
    fingerprint = hashlib.sha256(b''.join(p.read_bytes() for p in inputs)).hexdigest()
    stamp = output / 'inputs.sha256'
    if not stamp.exists() or stamp.read_text() != fingerprint:
        subprocess.run([sys.executable, str(toolkit / 'tools/ppu_lifter.py'),
            str(image), '--raw', '--base', '0x02200000', '--toc', '0x02673020',
            '--functions', str(functions), '--output', str(output),
            '--header-name', 'pxd_shader_recomp.h', '--source-name', 'pxd_shader_recomp.c',
            '--symbol-prefix', 'pxd_shader_', '--jobs', '3'], check=True)
        stamp.write_text(fingerprint)
    return fingerprint


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-dir', type=Path, required=True)
    parser.add_argument('--build-dir', type=Path, required=True)
    args = parser.parse_args()
    toolkit = Path(__file__).resolve().parents[1]
    game = args.game_dir.resolve()
    build = args.build_dir.resolve()
    source = game / 'yakuza'
    adapter = build / 'runner-adapter'
    original_main = (source / 'main.cpp').read_text()
    original_imports = (source / 'import_overrides.cpp').read_text()
    original_dispatch = (source / 'dispatch.cpp').read_text()
    shader_hash = prepare_shader(toolkit, game, build)

    # The legacy wrapper uses the obsolete (size, out-pointer) host signature.
    # The actual guest ABI returns the pitch in r3; r4 must never be touched.
    imports = replace_once(original_imports,
        '''    uint32_t pitch = 0;
    int32_t rc = cellGcmGetTiledPitchSize((uint32_t)ctx->gpr[3], &pitch);
    if (ctx->gpr[4]) vm_write32((uint32_t)ctx->gpr[4], pitch);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;''',
        '    ctx->gpr[3] = cellGcmGetTiledPitchSize((uint32_t)ctx->gpr[3]);',
        'legacy tiled-pitch wrapper')
    imports = replace_once(imports,
        'int32_t  cellGcmGetTiledPitchSize(uint32_t size, uint32_t* pitch);',
        'uint32_t cellGcmGetTiledPitchSize(uint32_t size);',
        'legacy tiled-pitch declaration')

    imports = replace_once(imports, '#include "rsx_live_draw.h"',
        '#include "rsx_draw_engine.h"\nextern "C" void cellGcmQueueUserCommand(uint32_t);\n#include "rsx_live_draw.h"', 'Metal draw engine include')
    imports = replace_once(imports, '    rsx_live_draw_method(method, arg);',
        '''    rsx_live_draw_method(method, arg);
#ifdef __APPLE__
    if (rsx_draw_engine_enabled() && method != 0xE944u)
        rsx_draw_engine_method(method, arg);
#endif''', 'Metal FIFO feed')
    imports = replace_once(imports,
        '    if (YZ_RSX_BACKEND_INIT(1280, 720, "Yakuza: Dead Souls (ps3recomp)") != 0) {',
        '''#ifdef __APPLE__
    rsx_draw_engine_set_guest_memory(yz_rsx_live_guest_ptr, nullptr);
#endif
    if (YZ_RSX_BACKEND_INIT(1280, 720, "Yakuza: Dead Souls (ps3recomp)") != 0) {''',
        'Metal guest memory mapping')
    imports = replace_once(imports,
        '            if (id + 1 > g_rsx_dispbuf_count) g_rsx_dispbuf_count = id + 1;',
        '''            if (id + 1 > g_rsx_dispbuf_count) g_rsx_dispbuf_count = id + 1;
#ifdef __APPLE__
            rsx_draw_engine_set_display_buffer(id, 0, g_rsx_dispbuf[id].offset,
                g_rsx_dispbuf[id].pitch, g_rsx_dispbuf[id].width, g_rsx_dispbuf[id].height);
#endif''', 'Metal display buffer registration')

    imports = replace_once(imports, 'static void yz_rsx_present(uint32_t buffer_id)\n{',
        '''extern "C" void yz_rsx_fifo_acquire(void);
extern "C" void yz_rsx_fifo_release(void);
static void yz_rsx_present(uint32_t buffer_id)
{
#ifdef __APPLE__
    if (rsx_draw_engine_enabled()) {
        yz_rsx_fifo_acquire();
        rsx_draw_engine_present_buffer(buffer_id);
        yz_rsx_fifo_release();
        return;
    }
#endif''', 'Metal queued flip presentation')

    imports = replace_once(imports,
        '    uint32_t ea = (uint32_t)ctx->gpr[3], out = (uint32_t)ctx->gpr[4];',
        '''    uint32_t ea = (uint32_t)ctx->gpr[3], out = (uint32_t)ctx->gpr[4];
    if (ea >= YZ_GCM_LOCAL_BASE && ea - YZ_GCM_LOCAL_BASE < YZ_GCM_LOCAL_SIZE) {
        if (out) vm_write32(out, ea - YZ_GCM_LOCAL_BASE);
        ctx->gpr[3] = 0;
        return;
    }''', 'local VRAM address translation')

    # A single CFRunLoopRun may return when AppKit's nested event handling
    # stops it or when no sources remain. Do not join a still-running guest:
    # that starves Metal's dispatch_sync and CoreAudio's main-queue work.
    main_cpp = replace_once(original_main, '#include <CoreFoundation/CoreFoundation.h>',
        '#include <CoreFoundation/CoreFoundation.h>\n#include <atomic>', 'CoreFoundation include')
    main_cpp = replace_once(main_cpp, '    pthread_t game_thr;\n',
        '    static std::atomic<bool> game_done{false};\n    pthread_t game_thr;\n', 'guest thread declaration')
    main_cpp = replace_once(main_cpp, '        CFRunLoopStop(CFRunLoopGetMain());',
        '        game_done.store(true, std::memory_order_release);\n        CFRunLoopStop(CFRunLoopGetMain());', 'guest completion')
    main_cpp = replace_once(main_cpp, '    CFRunLoopRun();\n    pthread_join(game_thr, NULL);',
        '''    while (!game_done.load(std::memory_order_acquire)) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
        Sleep(1);
    }
    pthread_join(game_thr, NULL);''', 'main-thread event loop')

    # This legacy runner owns its own import dispatcher, so it never calls
    # the toolkit scaffold's HLE-boundary pump. With no libgcm LLE interrupt
    # thread, deliver HLE events on its dedicated ticker instead.
    imports = replace_once(imports, '      cellGcmTickFlip();',
        """      cellGcmTickFlip();
#ifndef YZ_LLE_LIBGCM_SYS
      extern void ppu_gcm_pump(void);
      ppu_gcm_pump();
#endif""", 'HLE interrupt delivery')
    imports = replace_once(imports,
        '        vm_write32(RSX_DRIVER_INFO + 0x12CC, arg);       /* driverInfo.userCmdParam */',
        """        vm_write32(RSX_DRIVER_INFO + 0x12CC, arg);       /* driverInfo.userCmdParam */
#ifndef YZ_LLE_LIBGCM_SYS
        cellGcmQueueUserCommand(arg);
        break;
#endif""", 'HLE user-command interrupt delivery')
    # The save-data library's legacy scratch address is inside pxd_shader's
    # relocated image. Use an otherwise unclaimed window above audio buffers.
    main_cpp = replace_once(main_cpp, 'int main(int argc, char** argv)',
        'extern "C" int32_t cellSaveData_set_scratch_region(uint32_t, uint32_t);\n'
        'int main(int argc, char** argv)', 'save-data scratch declaration')
    main_cpp = replace_once(main_cpp, '    vm_stack_alloc_init(&g_stacks);',
        '''    vm_stack_alloc_init(&g_stacks);
    if (vm_commit(0x59000000u, 0x20000u) != 0 ||
        cellSaveData_set_scratch_region(0x59000000u, 0x20000u) != 0) {
        fprintf(stderr, "ERROR: save-data callback scratch allocation failed\\n");
        return 1;
    }''', 'save-data scratch allocation')

    # The legacy Yakuza host omits the firmware NP trophy initialization chain.
    # Supply it in this runner, preserving the toolkit API's NOT_INITIALIZED
    # contract for all other games.
    main_cpp = replace_once(main_cpp, 'int main(int argc, char** argv)',
        'extern "C" int32_t sceNpTrophyInit(void*, uint32_t, uint32_t, uint64_t);\n'
        'int main(int argc, char** argv)', 'trophy initialization declaration')
    main_cpp = replace_once(main_cpp,
        '    CreateThread(NULL, 0, yz_vblank_thread, NULL, 0, NULL);',
        '    sceNpTrophyInit(nullptr, 0, 0, 0);\n'
        '    CreateThread(NULL, 0, yz_vblank_thread, NULL, 0, NULL);',
        'legacy host trophy initialization')
    main_cpp = replace_once(main_cpp, '#include <atomic>',
        '#include <atomic>\n#include <mutex>', 'callback allocator include')
    main_cpp = replace_once(main_cpp,
        '        cb_stack = vm_stack_allocate(&g_stacks, 256 * 1024);',
        """        static std::mutex stack_lock;
        {
            std::lock_guard<std::mutex> guard(stack_lock);
            cb_stack = vm_stack_allocate(&g_stacks, 256 * 1024);
        }""", 'callback stack allocation')
    main_cpp = replace_once(main_cpp,
        '    cb_ctx.thread_id = yz_thread_current_id();',
        """    cb_ctx.thread_id = yz_thread_current_id();
    if (!g_yz_cur_ctx || !cb_ctx.thread_id) {
        static std::atomic<uint32_t> next_id{0x70000000u};
        static thread_local uint32_t interrupt_id = next_id.fetch_add(1);
        cb_ctx.thread_id = interrupt_id;
    }""", 'interrupt callback identity')
    main_cpp = replace_once(main_cpp,
        '    CreateThread(NULL, 0, yz_vblank_thread, NULL, 0, NULL);',
        '    CreateThread(NULL, 256ull * 1024 * 1024, yz_vblank_thread, NULL, 0, NULL);',
        'interrupt host stack')
    main_cpp = replace_once(main_cpp,
        '    pthread_create(&game_thr, NULL, +[](void*) -> void* {',
        """    pthread_attr_t guest_attr;
    pthread_attr_init(&guest_attr);
    if (pthread_attr_setstacksize(&guest_attr, 256ull * 1024 * 1024) != 0) {
        pthread_attr_destroy(&guest_attr);
        fprintf(stderr, "[boot] could not reserve guest host stack\\n");
        return 1;
    }
    int guest_rc = pthread_create(&game_thr, &guest_attr, +[](void*) -> void* {""",
        'guest host stack')
    main_cpp = replace_once(main_cpp, '    }, NULL);\n    while (!game_done.load',
        """    }, NULL);
    pthread_attr_destroy(&guest_attr);
    if (guest_rc != 0) {
        fprintf(stderr, "[boot] could not start guest thread\\n");
        return 1;
    }
    while (!game_done.load""", 'guest thread startup result')

    main_cpp = replace_once(main_cpp,
        'extern \"C\" void spu_overlay_register_source(uint32_t content_ea, int image_id);',
        'extern \"C\" void spu_overlay_register_source(uint32_t content_ea, int image_id);\nextern \"C\" void spu_overlay_register_region(uint32_t, uint32_t, int);',
        'streamed job registration declaration')
    main_cpp = replace_once(main_cpp,
        '    spu_begin_image(14); spu_recomp_register_jobbin_a();',
        '''    spu_overlay_register_region(0x01254500u, 0x9540u, 14);
    spu_overlay_register_region(0x01275A00u, 0x14C0u, 15);
    spu_overlay_register_region(0x02025500u, 0x680u, 12);
    spu_begin_image(14); spu_recomp_register_jobbin_a();''', 'streamed job code spans')
    main_cpp = replace_once(main_cpp,
        'extern "C" void spu_taskset_register_task_entry(uint32_t entry, int image_id);',
        'extern "C" void spu_taskset_register_task_entry(uint32_t entry, int image_id);\nextern "C" void spu_taskset_register_task_elf(uint32_t, int, int);',
        'task ELF registration declaration')
    main_cpp = replace_once(main_cpp,
        '    spu_taskset_register_task_entry(0x3070u, 3);',
        '''    for (int i = 0; i < SPU_IMAGE_COUNT; ++i) {
        const spu_image_desc& task = g_spu_images[i];
        /* The legacy table calls gs_task image 0; this runner registers it as 17. */
        spu_taskset_register_task_elf(task.elf_ea, task.image_id == 0 ? 17 : task.image_id, 2);
    }
    spu_taskset_register_task_entry(0x3070u, 3);''', 'task ELF registration')
    main_cpp = replace_once(main_cpp,
        'extern "C" void spu_taskset_register_task_elf(uint32_t, int, int);',
        'extern "C" void spu_taskset_register_task_elf(uint32_t, int, int);\nextern "C" void spu_register_stack_reset_entry(uint32_t, int);',
        'kernel stack reset declaration')
    main_cpp = replace_once(main_cpp,
        '    spu_begin_image(16); spu_recomp_register();',
        '''    spu_register_stack_reset_entry(0x838u, 16);
    /* Sony's policy changes or restores its stack in these blocks. Register
     * reachable trampoline PCs as well as the indirect kernel exit above. */
    spu_register_stack_reset_entry(0xA14u, 2);
    spu_register_stack_reset_entry(0xB48u, 2);
    spu_register_stack_reset_entry(0xB64u, 2);
    spu_register_stack_reset_entry(0x177Cu, 2);
    spu_begin_image(16); spu_recomp_register();''',
        'kernel module-exit stack reset')
    dispatch = replace_once(original_dispatch,
        'extern "C" yz_ppu_fn yz_lookup_func(uint32_t guest_addr)\n{',
        '''extern "C" const func_entry pxd_shader_function_table[];
extern "C" const uint64_t pxd_shader_function_table_count;
extern "C" yz_ppu_fn yz_lookup_func(uint32_t guest_addr)
{
    if (guest_addr >= 0x02200000u && guest_addr < 0x02600000u) {
        for (uint64_t i = 0; i < pxd_shader_function_table_count; ++i)
            if (pxd_shader_function_table[i].addr == guest_addr)
                return pxd_shader_function_table[i].func;
    }''', 'shader module dispatch')
    # Upstream filesystem diagnostics resolve host PCs through the scaffold.
    # This runner owns its function table, so supply the equivalent lookup here.
    dispatch += """
extern "C" uint32_t ppu_prof_resolve_host(void* address)
{
    uintptr_t pc = (uintptr_t)address, closest = 0;
    uint32_t guest = 0;
    for (unsigned i = 0; i < g_yz_func_count; ++i) {
        uintptr_t host = (uintptr_t)g_yz_func_table[i].fn;
        if (host <= pc && host > closest) {
            closest = host; guest = g_yz_func_table[i].addr;
        }
    }
    return guest && pc - closest < 0x20000 ? guest : 0;
}
"""
    adapter.mkdir(parents=True, exist_ok=True)
    write_changed(adapter / 'dispatch.cpp', dispatch)
    write_changed(adapter / 'main.cpp', main_cpp)
    write_changed(adapter / 'import_overrides.cpp', imports)
    # Keep legacy lifts read-only; upgrade their explicit call brackets in the
    # build tree. The captured link is the return PC even when RA is not r0.
    spu_adapter = adapter / 'spu'
    spu_adapter.mkdir(exist_ok=True)
    spu_adapted = {}
    call = re.compile(r'(spu_link\((0x[0-9A-Fa-f]+)\);[^\n]*?)SPU_DRAIN\(ctx\);(?= ctx->host_depth--;)')
    for original in sorted((game / 'recomp_prx').glob('*.c')):
        content = original.read_text()
        adapted, count = call.subn(lambda m: m[1] + 'spu_drain_call(ctx, ' + m[2] + ');', content)
        if count:
            if 'SPU_DRAIN(ctx); ctx->host_depth--;' in adapted:
                raise SystemExit(f'Unsupported SPU call bracket: {original}')
            write_changed(spu_adapter / original.name, adapted)
            spu_adapted[str(original.resolve())] = {
                'sha256': hashlib.sha256(content.encode()).hexdigest(), 'calls': count}
    # Defer until the external project's add_executable has defined its target.
    injection = '''function(ps3recomp_adapt_yakuza)
  get_target_property(runner_sources yakuza_recomp SOURCES)
  list(REMOVE_ITEM runner_sources main.cpp import_overrides.cpp dispatch.cpp)
  set(adapted_sources)
  foreach(source IN LISTS runner_sources)
    get_filename_component(source_name "${source}" NAME)
    if(EXISTS "${CMAKE_BINARY_DIR}/runner-adapter/spu/${source_name}")
      list(APPEND adapted_sources "${CMAKE_BINARY_DIR}/runner-adapter/spu/${source_name}")
    else()
      list(APPEND adapted_sources "${source}")
    endif()
  endforeach()
  set(runner_sources "${adapted_sources}")
  set_property(TARGET yakuza_recomp PROPERTY SOURCES "${runner_sources}")
  target_sources(yakuza_recomp PRIVATE
    "${CMAKE_BINARY_DIR}/runner-adapter/main.cpp"
    "${CMAKE_BINARY_DIR}/runner-adapter/import_overrides.cpp"
    "${CMAKE_BINARY_DIR}/runner-adapter/dispatch.cpp")
  file(GLOB shader_sources "${CMAKE_BINARY_DIR}/shader-module/pxd_shader_recomp_*.cpp")
  target_sources(ppu_recomp_objs PRIVATE ${shader_sources})
  target_include_directories(yakuza_recomp PRIVATE "${CMAKE_SOURCE_DIR}")
  # Generated SPU helpers are compiled in the runner, outside the runtime's
  # PRIVATE flags. Match its arithmetic and aliasing policy on Apple Clang.
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(yakuza_recomp PRIVATE -fno-strict-aliasing -ffp-contract=off)
  endif()
endfunction()
cmake_language(DEFER CALL ps3recomp_adapt_yakuza)
'''
    write_changed(adapter / 'attach.cmake', injection)
    write_changed(adapter / 'provenance.json', json.dumps({
        'game_dir': str(game), 'toolkit_dir': str(toolkit),
        'main_sha256': hashlib.sha256(original_main.encode()).hexdigest(),
        'imports_sha256': hashlib.sha256(original_imports.encode()).hexdigest(),
        'dispatch_sha256': hashlib.sha256(original_dispatch.encode()).hexdigest(),
        'shader_inputs_sha256': shader_hash,
        'spu_call_adapters': spu_adapted,
        'adaptations': ['tiled-pitch guest ABI', 'main run loop until guest completion',
                        'HLE interrupt delivery', 'guest and interrupt host stacks',
                        'translated shader module and dispatch'],
    }, indent=2) + '\n')
    subprocess.run(['cmake', '-S', str(source), '-B', str(build), '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=RelWithDebInfo', f'-DPS3RECOMP_DIR={toolkit}',
        '-DRECOMP_JOBS=3', f'-DCMAKE_PROJECT_YakuzaRecomp_INCLUDE={adapter / "attach.cmake"}'], check=True)


if __name__ == '__main__':
    main()
