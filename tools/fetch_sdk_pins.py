#!/usr/bin/env python3
"""Check out the ps3recomp revisions a port pins, and verify them.

A port that builds against ps3recomp should name the exact revisions it was
tested with, so every checkout and CI run uses the same SDK. This tool reads a
small JSON pin file from the port repository:

    {
      "format": 1,
      "repository": "https://github.com/sp00nznet/ps3recomp",
      "pins": {
        "sdk": {"commit": "<40-hex commit>", "tag": "v0.19.0", "path": "third_party/ps3recomp"}
      }
    }

Each pin (a "role": a port may pin, say, the runtime and the lifter at
different revisions) is cloned or fetched into its `path`, relative to the port
root, and checked out detached at `commit`. The commit is authoritative. A
`tag`, when given, must resolve to exactly that commit, and the checkout must be
clean; anything else is refused. A pin may set its own `repository` to use a
fork.
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

COMMIT = re.compile(r'^[0-9a-f]{40}$')


def git(path, *args):
    return subprocess.run(['git', '-C', str(path), *args], check=True,
                          capture_output=True, text=True).stdout.strip()


def load_pins(document):
    if document.get('format') != 1:
        raise ValueError(f"unsupported pin file format {document.get('format')!r}")
    pins = document.get('pins')
    if not isinstance(pins, dict) or not pins:
        raise ValueError('pin file has no pins')
    for role, pin in pins.items():
        if not isinstance(pin.get('commit'), str) or not COMMIT.match(pin['commit']):
            raise ValueError(f'{role}: commit must be a full 40-hex SHA-1')
        if not isinstance(pin.get('path'), str) or not pin['path']:
            raise ValueError(f'{role}: path is required')
        path = Path(pin['path'])
        if path.is_absolute() or '..' in path.parts:
            raise ValueError(f'{role}: path must stay inside the port root')
        if 'tag' in pin and not (isinstance(pin['tag'], str) and pin['tag']):
            raise ValueError(f'{role}: tag must be a non-empty string')
        if not (pin.get('repository') or document.get('repository')):
            raise ValueError(f'{role}: no repository')
    return pins


def fetch(root, url, role, pin):
    path = Path(root) / pin['path']
    if not (path / '.git').exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['git', 'clone', '--quiet', '--no-checkout', url, str(path)], check=True)
    else:
        git(path, 'fetch', '--quiet', '--tags', url)
    if not _has(path, pin['commit']):
        # Not reachable from the fetched refs (an untagged commit on another branch).
        git(path, 'fetch', '--quiet', url, pin['commit'])
    if pin.get('tag'):
        tagged = _rev(path, f"refs/tags/{pin['tag']}^{{commit}}")
        if tagged != pin['commit']:
            raise ValueError(f"{role}: tag {pin['tag']} is {tagged or 'missing'}, pinned commit is {pin['commit']}")
    git(path, 'checkout', '--quiet', '--detach', pin['commit'])
    if git(path, 'rev-parse', 'HEAD') != pin['commit'] or git(path, 'status', '--porcelain', '--untracked-files=no'):
        raise ValueError(f'{role}: checkout at {path} is not clean at the pinned commit')
    return path


def _rev(path, ref):
    result = subprocess.run(['git', '-C', str(path), 'rev-parse', '--verify', '--quiet', ref],
                            capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else ''


def _has(path, commit):
    return _rev(path, f'{commit}^{{commit}}') == commit


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('pins', type=Path, help='pin file (JSON)')
    parser.add_argument('--root', type=Path,
                        help='port root that pin paths are relative to (default: the pin file\'s directory)')
    parser.add_argument('--role', action='append', help='only this role (repeatable)')
    parser.add_argument('--url', help='override every pin\'s repository (e.g. a local mirror)')
    args = parser.parse_args(argv)
    document = json.loads(args.pins.read_text(encoding='utf-8'))
    root = (args.root or args.pins.resolve().parent).resolve()
    for role, pin in load_pins(document).items():
        if args.role and role not in args.role:
            continue
        url = args.url or pin.get('repository') or document['repository']
        path = fetch(root, url, role, pin)
        label = f"{pin['tag']} = " if pin.get('tag') else ''
        print(f"{role}: {label}{pin['commit'][:12]} at {path.relative_to(root)}")
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (ValueError, subprocess.CalledProcessError) as error:
        print(f'error: {error}', file=sys.stderr)
        sys.exit(1)
