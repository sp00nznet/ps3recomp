#!/usr/bin/env python3
"""SDL2 native OSK dialog fixture (Linux and other non-Apple POSIX hosts).

Builds tests/test_native_osk_sdl.c twice (SDL host alone, and with the
headless fallback) and runs each under SDL's dummy and offscreen video
drivers, plus a private Xvfb X server when one is installed. No game files.
"""
from pathlib import Path
import os, shutil, signal, subprocess, sys, tempfile, time

r = Path(__file__).resolve().parents[1]
if sys.platform in ('win32', 'darwin'):
    print('SKIP: the SDL dialog host is built only where no toolkit dialog exists'); raise SystemExit(0)

def sdl_flags():
    for tool in (['sdl2-config', '--cflags', '--libs'], ['pkg-config', '--cflags', '--libs', 'sdl2']):
        if shutil.which(tool[0]):
            out = subprocess.run(tool, check=True, capture_output=True, text=True).stdout.split()
            return out
    return None

flags = sdl_flags()
if flags is None:
    print('SKIP: SDL2 development files not found (sdl2-config/pkg-config)'); raise SystemExit(0)
cc = os.environ.get('CC', 'cc')

def start_xvfb(work):
    xvfb = shutil.which('Xvfb')
    if not xvfb or os.environ.get('OSK_SDL_NO_XVFB'):
        return None, None
    for n in range(87, 120):
        if Path(f'/tmp/.X11-unix/X{n}').exists() or Path(f'/tmp/.X{n}-lock').exists():
            continue
        proc = subprocess.Popen([xvfb, f':{n}', '-screen', '0', '1024x768x24', '-nolisten', 'tcp'],
                                stdout=subprocess.DEVNULL, stderr=open(work / 'xvfb.log', 'w'))
        for _ in range(100):
            if Path(f'/tmp/.X11-unix/X{n}').exists(): return proc, f':{n}'
            if proc.poll() is not None: break
            time.sleep(0.05)
        if proc.poll() is None: proc.terminate(); proc.wait(5)
    return None, None

with tempfile.TemporaryDirectory(prefix='native-osk-sdl-') as t:
    work = Path(t)
    src = [r/'tests/test_native_osk_sdl.c', r/'libs/misc/cellOskDialog.c', r/'libs/misc/osk_native.c',
           r/'libs/misc/osk_sdl.c', r/'libs/misc/osk_headless.c', r/'libs/system/cellSysutil.c']
    builds = {}
    for name, extra in (('sdl', []), ('sdl+headless', ['-DPS3RECOMP_NATIVE_OSK_HEADLESS=1'])):
        out = work / name.replace('+', '_')
        # The new host must build warning-free; the shared sources keep their own flags.
        subprocess.run([cc, '-std=gnu17', '-fsyntax-only', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Werror',
                        '-Wno-unused-command-line-argument',
                        '-DPS3RECOMP_NATIVE_OSK_SDL=1', *extra, '-I' + str(r/'include'),
                        str(r/'libs/misc/osk_sdl.c'), *[f for f in flags if f.startswith(('-I', '-D'))]], check=True)
        subprocess.run([cc, '-std=gnu17', '-O1', '-UNDEBUG', '-ffunction-sections', '-fdata-sections',
                        '-DPS3RECOMP_NATIVE_OSK_SDL=1', *extra, '-I' + str(r/'include'),
                        *map(str, src), '-o', str(out), *flags, '-pthread', '-Wl,--gc-sections'], check=True)
        builds[name] = out
    xvfb, display = start_xvfb(work)
    drivers = [('dummy', None), ('offscreen', None)] + ([('x11', display)] if display else [])
    failed = 0
    try:
        for name, exe in builds.items():
            for driver, disp in drivers:
                env = {k: v for k, v in os.environ.items()
                       if k not in ('PS3RECOMP_OSK_INITIAL_TEXT', 'DISPLAY', 'WAYLAND_DISPLAY')}
                env['SDL_VIDEODRIVER'] = driver
                env['OSK_NATIVE_CONTROL_FILE'] = str(work / f'control-{name}-{driver}')
                if disp: env['DISPLAY'] = disp
                proc = subprocess.run([str(exe)], env=env, timeout=60, capture_output=True, text=True)
                lines = [l for l in (proc.stdout + proc.stderr).splitlines()
                         if l.startswith(('PASS', 'FAIL', 'SDL video', 'field', 'native SDL'))]
                print(f'== {name} / {driver}: exit {proc.returncode}')
                for l in lines: print('  ' + l)
                if proc.returncode:
                    failed += 1
                    print(proc.stdout[-4000:]); print(proc.stderr[-4000:])
    finally:
        if xvfb:
            xvfb.send_signal(signal.SIGTERM); xvfb.wait(10)
    if not display: print('NOTE: Xvfb not found; the x11 driver was not exercised')
    raise SystemExit(1 if failed else 0)
