#!/usr/bin/env python3
"""Apply a PPU lifter fix to an existing lift, function by function.

A port often cannot simply re-lift after a lifter fix: its lift may carry
reviewed hand edits or post-lift transforms, or a full re-lift may be too large
to review. This tool applies only the fix's footprint.

Lift the same inputs twice with ppu_lifter.py, immediately before and after the
fix (--before and --after). Every function whose body differs between those two
lifts, or that only the fixed lifter emits, is the fix's exact footprint. Each
changed function must still be byte-identical in the existing lift (--base) to
its --before body, which proves no hand edit or transform touched it and that
the base was made by the same lifter. Then:

- changed bodies are replaced by their --after bodies, in place;
- added functions go into one new translation unit, with their declarations
  appended to the header and their rows appended to the function table (the
  table count is updated);
- every other byte of the base lift stays as it was.

The result is written to a fresh --output directory, together with
function_splices.json, which records the reason, both lifter revisions, the
footprint and the SHA-256 of every base file and spliced body. A base that
already carries function_splices.json has the new record appended.

The fix must not remove functions; such a fix needs a re-lift. If the base lift
was post-processed so that every unresolved-instruction TODO comment
(`/* TODO: ... */;`) is followed by a statement (a halt, say), pass the same
statement with --todo-guard and spliced bodies get it too.

  ppu_function_splice.py --base recompiled/ --before lift-before/ --after lift-after/
      --output recompiled-fixed/ --before-revision <rev> --after-revision <rev>
      --reason "fix(ppu_lifter): ..."
"""
import argparse
import hashlib
import json
import re
import shutil
import sys
from pathlib import Path

FUNC = re.compile(r'^void (?:__attribute__\(\(weak\)\) )?([A-Za-z_]\w*)\(ppu_context\* ctx\) \{\n.*?^\}\n',
                  re.M | re.S)
ENTRY = re.compile(r'^    \{ 0x[0-9A-F]+ULL, ([A-Za-z_]\w*), "[^"]*" \},\n', re.M)
SENTINEL = '    { 0, NULL, NULL }\n'
COUNT = re.compile(r'(\w*function_table_count = )(\d+);')
RECORD = 'function_splices.json'


class SpliceError(Exception):
    pass


def sha256(data):
    return hashlib.sha256(data if isinstance(data, bytes) else data.encode()).hexdigest()


def sources(directory, stem):
    return sorted(Path(directory).glob(f'{stem}_*.cpp')) + sorted(Path(directory).glob(f'{stem}_*.c'))


def functions(directory, stem):
    """{name: (file name, body text)} over every lifted translation unit."""
    out = {}
    for path in sources(directory, stem):
        for match in FUNC.finditer(path.read_text()):
            if match[1] in out:
                raise SpliceError(f'{directory}: duplicate definition of {match[1]}')
            out[match[1]] = (path.name, match[0])
    return out


def table(directory, stem):
    """({name: table row}, file holding the table)."""
    rows, holder = {}, None
    for path in sources(directory, stem):
        text = path.read_text()
        if SENTINEL in text:
            holder = path.name
        for match in ENTRY.finditer(text):
            rows[match[1]] = match[0]
    return rows, holder


def guarded(body, guard):
    if not guard:
        return body
    return re.sub(r'(/\* TODO: [^*]*\*/);', lambda m: m[1] + ';' + guard, body)


def splice(base, before, after, output, before_revision, after_revision, reason,
           stem='ppu_recomp', only=None, todo_guard=''):
    base, before, after, output = map(Path, (base, before, after, output))
    if output.exists():
        raise SpliceError('output must not exist yet')
    old, new, existing = functions(before, stem), functions(after, stem), functions(base, stem)
    removed = sorted(set(old) - set(new))
    if removed:
        raise SpliceError('the fix removes functions; re-lift instead: ' + ', '.join(removed))
    changed = sorted(n for n in old if old[n][1] != new[n][1])
    added = sorted(set(new) - set(old))
    footprint = dict(changed=changed, added=added)
    if only is not None:
        stray = [n for n in only if n not in changed]
        if stray:
            raise SpliceError('--only names functions the fix does not change: ' + ', '.join(stray))
        changed, added = sorted(only), []
    for name in changed:
        if name not in existing or existing[name][1] != guarded(old[name][1], todo_guard):
            raise SpliceError(f'{name}: the base lift differs from the --before lift; the splice would not be exact')
    clash = [n for n in added if n in existing]
    if clash:
        raise SpliceError('added functions already exist in the base lift: ' + ', '.join(clash))
    new_rows, _ = table(after, stem)
    _, table_file = table(base, stem)
    missing = [n for n in added if n not in new_rows]
    if added and (missing or table_file is None):
        raise SpliceError('added functions need --after table rows and a base function table: '
                          + ', '.join(missing))

    output.mkdir(parents=True)
    for path in base.iterdir():
        if path.is_file():
            shutil.copy2(path, output / path.name)
    edits, spliced = {}, {}
    for name in changed:
        file, body = existing[name]
        text = edits.setdefault(file, (output / file).read_text())
        if text.count(body) != 1:
            raise SpliceError(f'{name}: body is not unique in {file}')
        spliced[name] = guarded(new[name][1], todo_guard)
        edits[file] = text.replace(body, spliced[name])
    new_unit = None
    if added:
        numbers = [int(p.stem.rsplit('_', 1)[1]) for p in sources(output, stem)
                   if p.stem.rsplit('_', 1)[1].isdigit()]
        suffix = sources(base, stem)[0].suffix
        new_unit = f'{stem}_{1 + max(numbers, default=-1):03d}{suffix}'
        first = sources(base, stem)[0].read_text()
        preamble = FUNC.split(first, maxsplit=1)[0] if FUNC.search(first) else first.split('/* Function table */')[0]
        for name in added:
            spliced[name] = guarded(new[name][1], todo_guard)
        (output / new_unit).write_text(preamble.rstrip('\n') + '\n\n' + '\n'.join(spliced[n] for n in added))
        header = output / f'{stem}.h'
        text = header.read_text()
        declarations = ''.join(f'void {n}(ppu_context* ctx);\n' for n in added)
        last = text.rfind('(ppu_context* ctx);\n')
        if last < 0:
            raise SpliceError(f'{header.name} declares no lifted functions')
        cut = last + len('(ppu_context* ctx);\n')
        header.write_text(text[:cut] + declarations + text[cut:])
        text = edits.setdefault(table_file, (output / table_file).read_text())
        text = text.replace(SENTINEL, ''.join(new_rows[n] for n in added) + SENTINEL, 1)
        count = COUNT.search(text)
        if not count:
            raise SpliceError(f'{table_file} has no function_table_count')
        edits[table_file] = text.replace(count[0], f'{count[1]}{int(count[2]) + len(added)};', 1)
    for file, text in edits.items():
        (output / file).write_text(text)

    history = []
    if (base / RECORD).is_file():
        history = json.loads((base / RECORD).read_text())
    history.append(dict(
        reason=reason, before_revision=before_revision, after_revision=after_revision,
        lifter_footprint=footprint, changed=changed, added=added, new_translation_unit=new_unit,
        base_sha256={p.name: sha256(p.read_bytes()) for p in sorted(base.iterdir())
                     if p.is_file() and p.name != RECORD},
        body_sha256={n: sha256(spliced[n]) for n in changed + added}))
    (output / RECORD).write_text(json.dumps(history, indent=2) + '\n')
    return dict(changed=changed, added=added, new_translation_unit=new_unit)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                     epilog=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('base', 'before', 'after', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--before-revision', required=True, help='lifter revision of --before')
    parser.add_argument('--after-revision', required=True, help='lifter revision of --after')
    parser.add_argument('--reason', required=True, help='why (recorded in function_splices.json)')
    parser.add_argument('--stem', default='ppu_recomp', help='lift file stem (default: ppu_recomp)')
    parser.add_argument('--only', help='comma-separated changed functions: splice just these')
    parser.add_argument('--todo-guard', default='',
                        help='statement the base lift appends after each TODO comment')
    args = parser.parse_args(argv)
    only = [n.strip() for n in args.only.split(',') if n.strip()] if args.only else None
    try:
        result = splice(args.base, args.before, args.after, args.output, args.before_revision,
                        args.after_revision, args.reason, args.stem, only, args.todo_guard)
    except SpliceError as error:
        print(f'error: {error}', file=sys.stderr)
        return 1
    print(json.dumps(result, indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
