#!/usr/bin/env python3
"""Custom module headers must be used in both split and single-file output."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / 'code.bin').write_bytes(bytes.fromhex('4e800020'))  # blr
        (root / 'functions.json').write_text(json.dumps([{'start': '0x10000', 'end': '0x10004'}]))
        for single in (False, True):
            output = root / ('single' if single else 'split')
            command = [sys.executable, str(Path(__file__).with_name('ppu_lifter.py')),
                       str(root / 'code.bin'), '--raw', '--base', '0x10000',
                       '--functions', str(root / 'functions.json'), '--jobs', '1',
                       '--header-name', 'module.h', '--source-name', 'module.c',
                       '--symbol-prefix', 'module_', '--output', str(output)]
            if single:
                command.append('--single-file')
            subprocess.run(command, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            sources = [output / 'module.c'] if single else list(output.glob('module_*.cpp'))
            assert sources and (output / 'module.h').is_file()
            for source in sources:
                text = source.read_text()
                assert '#include "module.h"' in text, source
                assert '#include "ppu_recomp.h"' not in text, source
    print('PASS: custom module header in split and single-file output')


if __name__ == '__main__':
    main()
