#!/usr/bin/env python3
"""Module error bases: one literal per base against the console's value (tests/test_error_bases.c)."""
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='error-bases-') as tmp:
    binary=Path(tmp)/'test'
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-O1','-UNDEBUG','-I'+str(root/'include'),
        str(root/'tests/test_error_bases.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)
