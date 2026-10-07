"""Sanitizer builds for the conformance runners.

A runner given --sanitize thread|address configures its project build with
that sanitizer (in its own build directory), runs the same guest programs on
it, and treats any sanitizer report in a run's stderr as a failure of that
run, just like a transcript difference. The guest programs drive the real
runtime -- SPU contexts, channels, the MFC, lock-line coherence, lv2 objects --
from many host threads at once, which is exactly where ThreadSanitizer and
AddressSanitizer find what a transcript comparison cannot: a race that
happened to produce the right answer this time, a read of a freed context.
"""
import os
import re

KINDS = ("thread", "address", "undefined")


def cmake_args(kind):
    if not kind:
        return []
    # UBSan: undefined shifts, signed overflow (-fwrapv defines most), bad
    # float->int casts, misaligned pointers, out-of-range enums/bools. The
    # SPU/PPU instruction helpers are full of shifts by guest-controlled
    # amounts, where "shift >= width" is undefined and hosts disagree.
    if kind == "undefined":
        f = "-fsanitize=undefined -fno-omit-frame-pointer -g"
        return ["-DCMAKE_C_FLAGS=" + f, "-DCMAKE_CXX_FLAGS=" + f,
                "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=undefined"]
    f = "-fsanitize=%s -fno-omit-frame-pointer -g" % kind
    return ["-DCMAKE_C_FLAGS=" + f, "-DCMAKE_CXX_FLAGS=" + f,
            "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=" + kind]


def build_dir(base, kind):
    return base + ("_" + kind if kind else "")


def env(base, kind):
    e = dict(base)
    if kind == "thread":
        e.setdefault("TSAN_OPTIONS", "halt_on_error=0 second_deadlock_stack=1")
    elif kind == "undefined":
        e.setdefault("UBSAN_OPTIONS", "print_stacktrace=1 halt_on_error=0")
    elif kind == "address":
        e.setdefault("ASAN_OPTIONS", "detect_leaks=0 abort_on_error=0 detect_stack_use_after_return=1")
    return e


def reports(path):
    """One line per distinct sanitizer report in a run's stderr: the kind and
    the first in-tree frame of its stack, so repeats of one race collapse."""
    try:
        text = open(path, "rb").read().decode("latin-1")
    except OSError:
        return []
    out = []
    for m in re.finditer(r"(WARNING: ThreadSanitizer: [^\n(]+|ERROR: AddressSanitizer: [^\n]+)"
                         r"(.*?)(?=\n\n|\Z)", text, re.S):
        frames = re.findall(r"#\d+ (?:0x[0-9a-f]+ in )?(\S+) (\S+)", m.group(2))
        where = next((f + " " + os.path.basename(loc) for f, loc in frames
                      if "/ps3recomp/" in loc or "spu_" in f or "ppu_" in f), "?")
        line = "%s @ %s" % (m.group(1).strip(), where)
        if line not in out:
            out.append(line)
    # UBSan: "file:line:col: runtime error: message", one line each
    for m in re.finditer(r"([^\s:]+):(\d+):\d+: runtime error: ([^\n]+)", text):
        line = "UBSan: %s @ %s:%s" % (re.sub(r"0x[0-9a-f]+|-?\d+", "N", m.group(3)),
                                      os.path.basename(m.group(1)), m.group(2))
        if line not in out:
            out.append(line)
    return out
