#!/usr/bin/env python3
"""Audio clock pacing: 256 samples at 48 kHz is 187.5 blocks/s (libs/audio/audio_pace.h)."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='audio-pace-') as tmp:
    binary = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-I' + str(root / 'include'),
                    str(root / 'tests/test_audio_pace.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
