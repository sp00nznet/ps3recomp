"""Validated discovery and fresh generation for build_spu_workloads --input.

Uses the existing detector, lifter CLI and registry emitter. Owns its entire
output directory; manifests and logs remain available after failed generation.
"""
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from spu_elf import fnv1a64, scan_spu_candidates


def sha256(buf):
    return hashlib.sha256(buf).hexdigest()


def code_ranges(info):
    # The loader applies every PT_LOAD in header order, including zero filling.
    # Lifting a segment in isolation is unsafe if another load can replace its
    # bytes. Treat any overlapping memory layout as unsupported until generation
    # explicitly models the final deployed local store. Adjacent/empty loads
    # remain valid.
    loads = sorted((p['addr'], p['addr'] + p['memsz']) for p in info['loads'] if p['memsz'])
    if any(b > c for (_, b), (c, _) in zip(loads, loads[1:])):
        raise ValueError('overlapping PT_LOAD memory ranges (including BSS) are unsupported')
    executable = info['executable']
    if len(executable) != 1:
        raise ValueError('automated lifting requires exactly one executable PT_LOAD')
    segment = executable[0]
    start, end = segment['addr'], segment['addr'] + segment['filesz']
    sections = [s for s in info['sections'] if s['flags'] & 4 and s['size']]
    ranges = sorted((s['addr'], s['addr'] + s['size']) for s in sections) or [(start, end)]
    if any(a < start or b > end for a, b in ranges):
        raise ValueError('executable sections outside supported text segment')
    if any(b > c for (_, b), (c, _) in zip(ranges, ranges[1:])):
        raise ValueError('overlapping executable sections are unsupported')
    if not any(a <= info['entry'] < b for a, b in ranges):
        raise ValueError('ELF entry is not in an executable file range')
    return ranges


def publish(stage, destination):
    """Replace owned output as a unit, never reuse a previous generated lift."""
    backup = destination.with_name(destination.name + '.previous')
    if backup.exists():
        raise ValueError('previous interrupted publication exists: ' + str(backup))
    if destination.exists():
        destination.rename(backup)
    try:
        stage.rename(destination)
    except BaseException:
        if backup.exists():
            backup.rename(destination)
        raise
    if backup.exists():
        shutil.rmtree(backup)


def build_programs(args, registry_writer):
    destination = Path(args.lifted).absolute()
    registry = Path(args.out).absolute()
    if destination.is_symlink():
        raise SystemExit('--input output directory must not be a symlink')
    if registry.parent != destination:
        raise SystemExit('--input --out must be directly inside the owned --lifted directory')
    if destination.exists() and any(destination.iterdir()):
        try:
            previous = json.loads((destination / 'inventory.json').read_text())
            owned = previous.get('generator') == 'build_spu_workloads' and previous.get('schema_version') == 1
        except (OSError, ValueError):
            owned = False
        if not owned:
            raise SystemExit('--input requires an empty or previously generated output directory')
    inputs = sorted({str(Path(p).resolve()) for p in args.input})
    if any(Path(p).is_relative_to(destination.resolve()) for p in inputs):
        raise SystemExit('input files must be outside the owned output directory')
    destination.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=destination.name + '.stage-', dir=destination.parent))
    report = dict(schema_version=1, generator='build_spu_workloads', complete=False,
                  inputs=[], programs=[], errors=[], provenance={})
    try:
        if args.skip_lift:
            raise ValueError('--input always generates fresh code; --skip-lift is incompatible')
        if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', args.register_fn):
            raise ValueError('--register-fn must be a C identifier')
        # Hash every tool contributing to generation; fresh execution means changed
        # source/options/input can never validate an old registration implicitly.
        tool_names = ['spu_program_pipeline.py', 'build_spu_workloads.py', 'spu_elf.py',
                      'find_spu_functions.py', 'spu_disasm.py']
        tool_paths = [Path(__file__).with_name(n) for n in tool_names] + [Path(args.lifter).resolve()]
        handled = json.loads(Path(args.handled).read_text()) if getattr(args, 'handled', None) else {}
        pinned_ids = json.loads(Path(args.image_ids).read_text()) if getattr(args, 'image_ids', None) else None
        if not all(isinstance(v, str) and v for v in handled.values()):
            raise ValueError('--handled maps sha256 to a non-empty reason')
        if pinned_ids is not None:
            values = list(pinned_ids.values())
            if not all(type(v) is int and 0 < v < 0x80000000 for v in values) or len(set(values)) != len(values):
                raise ValueError('--image-ids maps sha256 to distinct positive image ids')
        report['provenance'] = dict(tools={str(p): sha256(p.read_bytes()) for p in tool_paths},
                                    python=sys.version, options=dict(register_fn=args.register_fn, constructor=args.constructor,
                                                 extra_funcs=args.extra_funcs, allow_partial=args.allow_partial,
                                                 handled=handled, image_ids=pinned_ids))
        unique = {}
        blobs = {}
        layouts = {}
        for path in inputs:
            try:
                buf = Path(path).read_bytes()
            except OSError as error:
                report['errors'].append(str(error))
                continue
            report['inputs'].append(dict(path=path, sha256=sha256(buf), size=len(buf)))
            for offset, info, error in scan_spu_candidates(buf):
                occurrence = dict(path=path, offset=offset)
                if error:
                    report['programs'].append(dict(status='invalid', reason=error, occurrences=[occurrence]))
                    continue
                blob = buf[offset:offset + info['size']]
                digest = sha256(blob)
                if digest in unique:
                    if blobs[digest] != blob:
                        raise ValueError('SHA256 collision between distinct images')
                    unique[digest]['occurrences'].append(occurrence)
                    continue
                name = 'spu_' + digest
                image_id = 0x10000000 | int(digest[:7], 16)  # positive signed-int range, away from legacy IDs
                if pinned_ids is not None and digest in pinned_ids:
                    image_id = pinned_ids[digest]
                p = dict(name=name, sha256=digest, size=len(blob), entry=info['entry'], image_id=image_id,
                         fingerprint=f'{fnv1a64(blob):016X}', status='discovered', occurrences=[occurrence])
                unique[digest], blobs[digest], layouts[digest] = p, blob, info
                report['programs'].append(p)
        # Runtime keys cannot distinguish collisions, including identical text
        # with differing data or one image's text matching another's full ELF.
        owners = {}
        ids = {}
        for digest, program in sorted(unique.items()):
            info = layouts[digest]
            if len(info['executable']) == 1:
                x = info['executable'][0]
                program['text_fingerprint'] = f"{fnv1a64(blobs[digest][x['off']:x['off'] + x['filesz']]):016X}"
            keys = {program['fingerprint'], program.get('text_fingerprint')} - {None}
            for key in keys:
                owners.setdefault(key, []).append(program)
            ids.setdefault(program['image_id'], []).append(program)
        for group in list(owners.values()) + list(ids.values()):
            if len(group) > 1:
                for program in group:
                    program.update(status='unsupported', reason='ambiguous runtime fingerprint or image-id collision')
        for digest, program in unique.items():
            if digest in handled and program['status'] == 'discovered':
                program.update(status='handled', reason=handled[digest])
                program.pop('image_id', None)
            elif pinned_ids is not None and program['status'] == 'discovered' and digest not in pinned_ids:
                program.update(status='unsupported', reason='no pinned image id (--image-ids)')
        stale = (set(handled) | set(pinned_ids or {})) - set(unique)
        if stale:
            report['errors'].append('--handled/--image-ids name programs that were not discovered: ' +
                                    ', '.join(sorted(stale)))
        extra = {}
        for spec in args.extra_funcs:
            name, sep, value = spec.partition('=')
            if not sep or name not in {p['name'] for p in unique.values()}:
                raise ValueError('--extra-funcs requires discovered content name=ADDR[,ADDR...]')
            extra[name] = [int(s.strip(), 0) for s in value.split(',')]
        imgs, text_fps, image_ids = [], {}, {}
        from find_spu_functions import detect_functions
        from build_spu_workloads import debase_includes
        for digest, program in sorted(unique.items()):
            if program['status'] in ('unsupported', 'handled'):
                continue
            name, prefix = program['name'], program['name'] + '_'
            outdir = stage / name
            outdir.mkdir()
            blob, info = blobs[digest], layouts[digest]
            (outdir / 'image.elf').write_bytes(blob)
            try:
                ranges = code_ranges(info)
                functions, (offset, base, size) = detect_functions(blob, verbose=False)
                bounds = sorted({(max(s, a), min(e, b)) for s, e in functions for a, b in ranges
                                 if max(s, a) < min(e, b)})
                for address in extra.get(name, []):
                    if not any(a <= address < b and address % 4 == 0 for a, b in ranges):
                        raise ValueError('extra function address is outside executable ranges')
                program['executable_ranges'] = ranges
                (outdir / 'code.bin').write_bytes(blob[offset:offset + size])
                (outdir / 'functions.json').write_text(json.dumps([dict(start=s, end=e) for s, e in bounds]))
                command = [sys.executable, args.lifter, str(outdir / 'code.bin'), '--base', str(base),
                           '--functions', str(outdir / 'functions.json'), '--symbol-prefix', prefix,
                           '--report', str(outdir / 'lift.json'), '--output', str(outdir)]
                if extra.get(name):
                    command += ['--extra-funcs', ','.join(hex(a) for a in extra[name])]
                if getattr(args, 'merge_chunks', False):
                    command += ['--merge-chunks']
                result = subprocess.run(command, capture_output=True, text=True)
                (outdir / 'lift.log').write_text(result.stdout + result.stderr)
                if result.returncode:
                    raise ValueError('lifter failed; see ' + name + '/lift.log')
                diagnostics = json.loads((outdir / 'lift.json').read_text())
                program['diagnostics'] = diagnostics
                if diagnostics['unsupported']:
                    raise ValueError('unsupported instructions emitted by lifter')
                starts = {s for s, _ in diagnostics['functions']}
                if info['entry'] not in starts:
                    raise ValueError('lifter did not generate the ELF entry')
                if any(not any(a <= s < e <= b for a, b in ranges) for s, e in diagnostics['functions']):
                    raise ValueError('generated function extends beyond executable ranges')
                if (set(diagnostics['call_targets']) | set(diagnostics['branch_targets'])) - starts:
                    raise ValueError('direct branch/call targets lack generated function entries')
                for filename in ['spu_recomp.c', 'spu_recomp.h']:
                    if not (outdir / filename).is_file():
                        raise ValueError('lifter output missing ' + filename)
                    debase_includes(str(outdir / filename))
                expected = f"{prefix}spu_func_{info['entry']:08X}("
                if '/* TODO spu:' in (outdir / 'spu_recomp.c').read_text():
                    raise ValueError('unresolved instruction or branch emitted by lifter')
                if expected not in (outdir / 'spu_recomp.h').read_text():
                    raise ValueError('generated header is missing entry symbol')
            except (ValueError, OSError, KeyError, SystemExit) as error:
                program.update(status='unsupported', reason=str(error))
                continue
            program['status'] = 'registered'
            imgs.append((name, prefix, int(program['fingerprint'], 16), info['entry']))
            image_ids[name] = program['image_id']
            text_fps[name] = int(program['text_fingerprint'], 16)
        # Default runtime capacity is 256 registrations, including text aliases.
        if sum(len({fp, text_fps[name]}) for name, _, fp, _ in imgs) > 256:
            report['errors'].append('generated registrations exceed default runtime capacity (256)')
        if sum(len(p.get('diagnostics', {}).get('functions', [])) for p in report['programs']
               if p['status'] == 'registered') > 262144:
            report['errors'].append('generated functions exceed default runtime registry capacity (262144)')
        report['complete'] = not report['errors'] and all(p['status'] in ('registered', 'handled')
                                                          for p in report['programs'])
        usable = not report['errors'] and (report['complete'] or args.allow_partial)
        if usable:
            registry_args = type('RegistryArgs', (), dict(out=str(stage / registry.name), title='discovered SPU programs',
                                                        register_fn=args.register_fn, constructor=args.constructor))()
            registry_writer(registry_args, imgs, text_fps, image_ids)
            sources = [destination / name / 'spu_recomp.c' for name, _, _, _ in imgs] + [registry]
            # Bracket arguments preserve spaces and avoid expansion of $ in paths.
            cmake = 'set(SPU_GENERATED_SOURCES\n' + ''.join(f'  [==[{p.as_posix()}]==]\n' for p in sources) + ')\n'
            (stage / 'sources.cmake').write_text(cmake)
        if not usable:
            for program in report['programs']:
                if program['status'] == 'registered':
                    program.update(status='generated', reason='registration omitted because coverage is incomplete')
        for program in report['programs']:
            if 'name' in program:
                outdir = stage / program['name']
                if outdir.exists():
                    program['outputs'] = {str(p.relative_to(stage)): sha256(p.read_bytes())
                                          for p in sorted(outdir.iterdir()) if p.is_file()}
        report['outputs'] = {filename: sha256((stage / filename).read_bytes())
                             for filename in [registry.name, 'sources.cmake'] if (stage / filename).is_file()}
        (stage / 'inventory.json').write_text(json.dumps(report, indent=2) + '\n')
        publish(stage, destination)
        print(f"[build_spu_workloads] {len(imgs) if usable else 0}/{len(report['programs'])} candidates registered; "
              f"complete={report['complete']}; report={destination / 'inventory.json'}")
        return 0 if usable else 1
    except (ValueError, OSError) as error:
        report['complete'] = False
        report['errors'].append(str(error))
        for filename in [registry.name, 'sources.cmake']:
            (stage / filename).unlink(missing_ok=True)
        (stage / 'inventory.json').write_text(json.dumps(report, indent=2) + '\n')
        publish(stage, destination)
        print('[build_spu_workloads] generation failed: ' + str(error), file=sys.stderr)
        return 1
    finally:
        if stage.exists():
            shutil.rmtree(stage)
