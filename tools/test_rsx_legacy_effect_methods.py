#!/usr/bin/env python3
"""Every method outside cellGcmSys.c's gcm_legacy_method_has_effect acts only on
the rsx_state it is handed (libs/video/tests/test_rsx_legacy_effect_methods.c,
built against rsx_commands.c). The set is read from cellGcmSys.c, so a method
that gains an effect outside rsx_state fails here until the filter learns it."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
src = (root / 'libs/video/cellGcmSys.c').read_text(encoding='utf-8', errors='replace')
body = re.search(r'static int gcm_legacy_method_has_effect\(u32 m\)\s*\{(.*?)\n\}', src, re.S)
if not body:
    raise SystemExit('gcm_legacy_method_has_effect not found in cellGcmSys.c')
methods = re.findall(r'case\s+0x([0-9A-Fa-f]+)u?\s*:', body.group(1))
if not methods:
    raise SystemExit('gcm_legacy_method_has_effect has no cases')
if os.name == 'nt':
    print('SKIP: builds with a POSIX cc')
    raise SystemExit(0)
print('effect set:', ' '.join('0x' + m.upper() for m in methods))
with tempfile.TemporaryDirectory(prefix='rsx-legacy-effects-') as tmp:
    binary = Path(tmp) / 'test'
    extra = [] if sys.platform == 'darwin' else ['-lm']
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-I' + str(root / 'include'), '-I' + str(root / 'libs/video'),
                    str(root / 'libs/video/tests/test_rsx_legacy_effect_methods.c'),
                    str(root / 'libs/video/rsx_commands.c'), *extra,
                    '-o', str(binary)], check=True)
    result = subprocess.run([str(binary), *methods], timeout=60, capture_output=True, text=True)
    print('\n'.join(l for l in result.stdout.splitlines() if l.startswith('[') and not l.startswith('[RSX')))
    if result.returncode:
        print(result.stdout[-2000:])
        raise SystemExit('test_rsx_legacy_effect_methods failed')
