#!/usr/bin/env python3
"""Exercise strict IO mapping through the real GCM tables and translators."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from gen_hle_nids import scan
root=Path(__file__).resolve().parents[1]
assert (0x626E8518,'cellGcmMapEaIoAddressWithFlags') in scan(root/'libs/video/cellGcmSys.c'), 'Missing flagged IO mapping registration'
if os.name=='nt':
    print('SKIP: mapping fixture currently uses POSIX linking; registration checked')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='gcm-map-flags-') as tmp:
    binary=Path(tmp)/'test'
    linker=['-Wl,-dead_strip'] if sys.platform=='darwin' else ['-Wl,--gc-sections','-pthread','-lm']
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-D_GNU_SOURCE','-O1','-UNDEBUG',
        # A small table: the capacity check fills every slot with 1 MB IO maps,
        # and #255's default of 512 would run past the 256 MB aperture.
        '-DCELL_GCM_MAX_IO_MAPPINGS=16',
        '-ffunction-sections','-fdata-sections','-I'+str(root/'include'),
        str(root/'tests/test_gcm_map_flags.c'),*linker,'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10,
        env={k:v for k,v in os.environ.items() if not k.startswith('A2O_')})
