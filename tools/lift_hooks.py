#!/usr/bin/env python3
"""Pinned, round-trip-verified hooks for lifted PPU sources.

A port often needs a few extra lines inside lifted functions: a trace point,
an observer call, a guarded diagnostic. Editing generated files by hand is
lost at the next lift, and a script that patches by text can silently land in
the wrong place once the lifter's output changes. This module gives such
scripts a safe shape.

A *stage* is a Python object (usually a module) with:

    NAME              short label for messages
    INCLUDE           text put once at the top of a file the stage changed
                      ('' for none)
    PINS              {function name: {'sha256': ..., 'file': ...}} for the
                      functions the stage instruments
    DEPENDENCIES      the same, for functions it relies on but leaves alone
    SHARED (optional) instrumented names other stages may also instrument
    instrument_body(body, name) -> body with the stage's lines added
    strip_body(body, name)      -> body with the stage's lines removed

A pin is the SHA-256 of a function's complete lifted text (signature to
closing brace) and the basename of the lifted file holding it. Before a stage
touches a body it checks the body (with every sharing stage's lines removed)
against its pin, checks every dependency, and checks that removing the new
lines gives back the original byte for byte. Any mismatch -- a missing,
duplicated, moved or changed function, or a seam that is not unique -- raises
HookError and nothing is written. Applying a stage twice gives the same text.
Keep the added lines under a compile guard (see guarded()) so the same lift
compiled without it is the plain program.

Command line:

    lift_hooks.py apply LIFT_DIR --hook MODULE [--hook MODULE ...]
        Runs each MODULE.instrument_source(source) -> source over the lifted
        sources in LIFT_DIR, in order, and records the modules and the
        changed files' hashes in LIFT_DIR/lift_hooks.json. A module that
        drives a stage implements instrument_source() with
        lift_hooks.instrument_source(source, stage).

    lift_hooks.py pins LIFT_DIR NAME [NAME ...]
        Prints {name: {sha256, file}} for the named functions, for writing or
        refreshing a stage's pin table after a lifter change; review the
        result before committing it.
"""
import argparse
import hashlib
import importlib
import json
import re
import sys
from pathlib import Path

# One lifted function: optional weak attribute, any lifter name prefix, the
# ppu_context signature, through the closing brace at column 0.
FUNCTION_BODY = re.compile(
    r'^(?:__attribute__\(\(weak\)\) )?void (\w+)\(ppu_context\* ctx\) \{.*?^}', re.M | re.S)
DIGEST = re.compile('[0-9a-f]{64}')
LIFTED_FILE = re.compile(r'[\w.-]+\.(?:c|cc|cpp)')
NAME = re.compile(r'\w+')
DEFAULT_GLOB = 'ppu_recomp*.c*'


class HookError(ValueError):
    """A pinned source, seam or hook input did not match."""


def sha256_text(text):
    return hashlib.sha256(text.encode()).hexdigest()


def sha256_file(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def guarded(guard, text):
    """Hook lines under a compile guard, as their own lines."""
    return f'\n#if {guard}\n{text}\n#endif\n'


def bodies(source, names):
    """Map each wanted function name to its single body match in `source`."""
    found = {}
    for match in FUNCTION_BODY.finditer(source):
        name = match[1]
        if name not in names:
            continue
        if name in found:
            raise HookError(f'duplicate lifted function {name}')
        found[name] = match
    return found


def check_pins(stage):
    """Reject a malformed pin table before it is used."""
    for table in (stage.PINS, stage.DEPENDENCIES):
        for name, pin in table.items():
            if (not NAME.fullmatch(name) or not isinstance(pin, dict) or set(pin) != {'sha256', 'file'}
                    or not DIGEST.fullmatch(pin['sha256']) or not LIFTED_FILE.fullmatch(pin['file'])):
                raise HookError(f'{stage.NAME}: malformed pin {name}')
    if set(stage.PINS) & set(stage.DEPENDENCIES):
        raise HookError(f'{stage.NAME}: a function is both instrumented and a dependency')
    if not set(getattr(stage, 'SHARED', ())) <= set(stage.PINS):
        raise HookError(f'{stage.NAME}: a shared function is not instrumented by the stage')


def replace_once(body, old, new, what):
    if body.count(old) != 1:
        raise HookError(f'{what}: seam is not unique')
    return body.replace(old, new, 1)


def apply_patches(body, patches, what):
    """Apply (old, new) replacements, each of which must match exactly once."""
    for old, new in patches:
        body = replace_once(body, old, new, what)
    return body


def remove_patches(body, patches):
    """Undo apply_patches()."""
    for old, new in reversed(patches):
        body = body.replace(new, old)
    return body


def instrument_source(source, stage, pins=None, dependencies=None, others=()):
    """Return `source` with `stage`'s hooks; idempotent on already hooked source.

    `pins` and `dependencies` default to the stage's tables. Each instrumented
    body is verified after removing this stage's hooks, so a second
    application reproduces the same text instead of stacking hooks.

    A function that `stage` and the stages in `others` all list in SHARED may
    already carry their hooks. Its pin is checked with every stage's hooks
    removed, and this stage's hooks are added next to theirs untouched.
    """
    check_pins(stage)
    pins = stage.PINS if pins is None else pins
    dependencies = stage.DEPENDENCIES if dependencies is None else dependencies
    if stage.INCLUDE:
        source = source.replace(stage.INCLUDE, '')
    found = bodies(source, set(pins) | set(dependencies))
    for name, match in found.items():
        if name in dependencies and sha256_text(match[0]) != dependencies[name]['sha256']:
            raise HookError(f'{stage.NAME}: dependency {name} does not match its pin')
    changed = False
    # Splice from the end so earlier match offsets stay valid.
    for name, match in sorted(found.items(), key=lambda item: item[1].start(), reverse=True):
        if name not in pins:
            continue
        current = stage.strip_body(match[0], name)
        sharing = [other for other in others if name in getattr(other, 'SHARED', ())]
        if sharing and name not in getattr(stage, 'SHARED', ()):
            raise HookError(f'{stage.NAME}: function {name} is not declared shared')
        original = current
        for other in sharing:
            original = other.strip_body(original, name)
        if sha256_text(original) != pins[name]['sha256']:
            raise HookError(f'{stage.NAME}: function {name} does not match its pin')
        hooked = stage.instrument_body(current, name)
        restored = stage.strip_body(hooked, name)
        for other in sharing:
            restored = other.strip_body(restored, name)
        if stage.strip_body(hooked, name) != current or restored != original:
            raise HookError(f'{stage.NAME}: hooks for {name} would change original statements')
        source = source[:match.start()] + hooked + source[match.end():]
        changed = True
    return stage.INCLUDE + source if changed and stage.INCLUDE else source


def lifted_files(directory, pattern=DEFAULT_GLOB):
    return sorted(p for p in Path(directory).glob(pattern) if p.suffix in ('.c', '.cc', '.cpp'))


def locate(directory, stage, pattern=DEFAULT_GLOB):
    """Check every pinned function appears exactly once, in its pinned file.

    Returns the sorted basenames of the files the stage needs."""
    names = set(stage.PINS) | set(stage.DEPENDENCIES)
    counts = dict.fromkeys(names, 0)
    for path in lifted_files(directory, pattern):
        for match in FUNCTION_BODY.finditer(path.read_text()):
            name = match[1]
            if name not in counts:
                continue
            pin = stage.PINS.get(name) or stage.DEPENDENCIES[name]
            if path.name != pin['file']:
                raise HookError(f'{stage.NAME}: function {name} moved to {path.name}')
            counts[name] += 1
    missing = sorted(name for name, count in counts.items() if count != 1)
    if missing:
        raise HookError(f'{stage.NAME}: missing or duplicate functions {", ".join(missing)}')
    return sorted({pin['file'] for table in (stage.PINS, stage.DEPENDENCIES) for pin in table.values()})


def current_pins(directory, names, pattern=DEFAULT_GLOB):
    """Name -> {sha256, file} of the named functions in a lifted directory."""
    result = {}
    for path in lifted_files(directory, pattern):
        for name, match in bodies(path.read_text(), set(names)).items():
            if name in result:
                raise HookError(f'duplicate lifted function {name}')
            result[name] = {'sha256': sha256_text(match[0]), 'file': path.name}
    missing = sorted(set(names) - set(result))
    if missing:
        raise HookError(f'missing lifted functions {", ".join(missing)}')
    return dict(sorted(result.items()))


# ---------------------------------------------------------------------------
# Hook modules
# ---------------------------------------------------------------------------
def load_hooks(names):
    """Import hook modules; each must expose instrument_source(source) -> source."""
    hooks = []
    for name in names:
        module = importlib.import_module(name)
        if not callable(getattr(module, 'instrument_source', None)):
            raise HookError(f'lift hook {name} has no instrument_source(source)')
        hooks.append((name, module))
    return hooks


def apply_hooks(source, hooks):
    for name, module in hooks:
        source = module.instrument_source(source)
        if not isinstance(source, str):
            raise HookError(f'lift hook {name} returned {type(source).__name__}, not source text')
    return source


def hook_records(hooks):
    return [{'module': name, 'path': Path(module.__file__).name, 'sha256': sha256_file(module.__file__)}
            for name, module in hooks]


def apply_directory(directory, names, pattern=DEFAULT_GLOB):
    """Run hook modules over every lifted file; write nothing unless all succeed."""
    directory = Path(directory)
    hooks = load_hooks(names)
    files = lifted_files(directory, pattern)
    if not files:
        raise HookError(f'no lifted sources matching {pattern} in {directory}')
    updates, changed = {}, {}
    for path in files:
        before = path.read_text()
        after = apply_hooks(before, hooks)
        if after != before:
            updates[path] = after
            changed[path.name] = {'before_sha256': sha256_text(before), 'after_sha256': sha256_text(after)}
    for path, text in updates.items():
        path.write_text(text)
    record = {'hooks': hook_records(hooks), 'changed': changed}
    (directory / 'lift_hooks.json').write_text(json.dumps(record, indent=2) + '\n')
    return record


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = parser.add_subparsers(dest='command', required=True)
    apply = sub.add_parser('apply', help='run hook modules over a lifted directory')
    apply.add_argument('lift_dir', type=Path)
    apply.add_argument('--hook', action='append', required=True, help='hook module (repeatable, applied in order)')
    apply.add_argument('--glob', default=DEFAULT_GLOB, help=f'lifted source pattern (default {DEFAULT_GLOB})')
    pins = sub.add_parser('pins', help='print pins for named lifted functions')
    pins.add_argument('lift_dir', type=Path)
    pins.add_argument('names', nargs='+')
    pins.add_argument('--glob', default=DEFAULT_GLOB)
    args = parser.parse_args(argv)
    try:
        if args.command == 'apply':
            record = apply_directory(args.lift_dir, args.hook, args.glob)
            print(f'lift_hooks: {len(record["changed"])} file(s) changed by {len(record["hooks"])} hook(s)')
        else:
            print(json.dumps(current_pins(args.lift_dir, args.names, args.glob), indent=2))
    except HookError as error:
        print(f'lift_hooks: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
