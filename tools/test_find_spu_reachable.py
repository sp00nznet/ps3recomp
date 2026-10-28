#!/usr/bin/env python3
"""find_spu_functions: reachable detection keeps read-only data out of the lift.

An SPU image without section headers has its strings in the executable
segment. An `ila` that loads a string's address seeds the default detection's
continuation scan, and the string's words then lift as a function. With
reachable=True the seed is dropped, because the straight line from it branches
past the end of the image instead of returning.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from find_spu_functions import detect_functions

BASE = 0x3000
WORDS = [
    0x42181003,   # 0x3000  ila   $r3, 0x3020     (a string's address)
    0x33000180,   # 0x3004  brsl  $r0, 0x3010
    0x35000000,   # 0x3008  bi    $r0
    0x00200000,   # 0x300C  lnop
    0x35000000,   # 0x3010  bi    $r0
    0x00200000,   # 0x3014  lnop
    0x00200000,   # 0x3018  lnop
    0x00200000,   # 0x301C  lnop
    0x205A4C49,   # 0x3020  " ZLI"  decodes as brz past the end of the image
    0x312E322E,   # 0x3024  "1.2."
    0x332E3300,   # 0x3028  "3.3\0"
    0x00200000,   # 0x302C
]


def main():
    blob = b"".join(struct.pack(">I", w) for w in WORDS)
    default, _ = detect_functions(blob, base_override=BASE, verbose=False, raw=True)
    reachable, _ = detect_functions(blob, base_override=BASE, verbose=False, raw=True,
                                    reachable=True)
    starts = [s for s, _ in reachable]
    assert any(s >= 0x3020 for s, _ in default), \
        "the default detection is expected to take the string for code: %r" % default
    assert BASE in starts and 0x3010 in starts, "reachable code kept: %r" % reachable
    assert all(e <= 0x3020 for _, e in reachable), "string lifted as code: %r" % reachable
    print("PASS: reachable SPU function detection leaves the string out")


if __name__ == "__main__":
    main()
