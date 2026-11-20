#!/usr/bin/env python3
"""Atomically publish one confirmation for a logged native OSK generation."""
import argparse
import os
from pathlib import Path
import tempfile


def token_value(value):
    if (not value or len(value) > 20 or not value.isascii() or
            not value.isdecimal() or not 0 < int(value) <= 18446744073709551615):
        raise argparse.ArgumentTypeError('token must be a positive uint64 from the ready log')
    return int(value)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('control_file', type=Path, help='per-run OSK_NATIVE_CONTROL_FILE path')
    parser.add_argument('token', type=token_value, help='token from the current native control ready log')
    args = parser.parse_args()
    path = args.control_file.absolute()
    fd, temporary = tempfile.mkstemp(prefix=path.name + '.', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as command:
            command.write(f'confirm {args.token}\n'.encode('ascii'))
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    print(f'Published native OSK confirmation token={args.token}')


if __name__ == '__main__':
    main()
