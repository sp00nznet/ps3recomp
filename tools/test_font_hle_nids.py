#!/usr/bin/env python3
"""cellFontInitializeWithRevision (NID 0xF03DCC29) is registered as an HLE export.

gen_hle_nids.py computes each NID from the name of a non-static cell*/sys*/sce*
function defined in a libs/**/*.c file and registers it. A title's import of a
function nothing defines is faked as CELL_OK without running anything: for
cellFontInitializeWithRevision that leaves the font library uninitialised and
every later cellFont call fails with CELL_FONT_ERROR_UNINITIALIZED.
(The behaviour of the function itself: libs/font/tests/test_font_initialize_with_revision.c.)

Run: python tools/test_font_hle_nids.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_hle_nids                     # noqa: E402

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
NID = 0xF03DCC29


def test_nid_is_the_published_one():
    assert gen_hle_nids.compute_nid("cellFontInitializeWithRevision") == NID


def test_cellfont_registers_it():
    found = {nid: name for nid, name in gen_hle_nids.scan(os.path.join(ROOT, "libs/font/cellFont.c"))}
    assert found.get(NID) == "cellFontInitializeWithRevision", \
        "libs/font/cellFont.c defines no cellFontInitializeWithRevision: the import is faked as CELL_OK"


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print(f"PASS: {name}")
