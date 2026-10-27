#!/usr/bin/env python3
"""Find the MSVC toolchain environment, without hardcoding a Visual Studio version.

clang-cl needs the MSVC headers and import libraries, which only exist in the
environment vcvars64.bat sets up. In CI a GitHub action does that before anything
runs; locally nothing does, so every tool that shells out to a compiler has to
arrange it. Doing that by writing a literal path into each tool is how
test_ppu_lift.py ended up pointing at "Visual Studio\\18\\Community" -- a version
that is not installed on the machine this is being written on, which silently
reduced a 1300-case conformance suite to compiling nothing.

    from msvc_env import environ, find_vcvars
    subprocess.run(cmd, env=environ())        # MSVC set up on Windows, os.environ elsewhere

Both are no-ops off Windows, so callers need no platform branch of their own.
"""
import glob
import os
import subprocess
import sys
import tempfile

_CACHED = None


def find_vcvars():
    """Path to vcvars64.bat, newest install first, or None."""
    if os.name != "nt":
        return None
    roots = [
        os.environ.get("ProgramFiles", r"C:\Program Files"),
        os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
    ]
    hits = []
    for root in roots:
        hits += glob.glob(os.path.join(
            root, "Microsoft Visual Studio", "*", "*",
            "VC", "Auxiliary", "Build", "vcvars64.bat"))
    # Newest version, then edition, by plain reverse sort: "2022" > "2019", and
    # within a year "Enterprise" > "Community". Any of them can compile this.
    return sorted(hits, reverse=True)[0] if hits else None


def environ():
    """os.environ with the MSVC toolchain added on Windows.

    Result is cached: starting a cmd.exe and running vcvars costs about a second,
    and a gate run does this for every port.
    """
    global _CACHED
    if os.name != "nt":
        return dict(os.environ)
    if _CACHED is not None:
        return dict(_CACHED)

    env = dict(os.environ)
    vcvars = find_vcvars()
    if vcvars:
        # Ask cmd to apply vcvars and then print the resulting environment, which
        # is the only reliable way to get at it -- the batch file sets dozens of
        # variables and the list changes between releases.
        #
        # Via a temp .bat rather than an argument: vcvars lives under "Program
        # Files", and Python quotes a Windows argument containing spaces with
        # backslash-escaped quotes, which cmd does not understand -- `call` then
        # fails with "The system cannot find the path specified" and the whole
        # environment silently comes back unmodified. The same .bat trick is why
        # test_ppu_lift.py and test_milestone.py invoke compilers the way they do.
        bat = None
        try:
            fd, bat = tempfile.mkstemp(suffix=".bat", prefix="ps3_msvc_")
            with os.fdopen(fd, "w") as f:
                f.write("@echo off\n")
                f.write('call "%s" >nul 2>&1\n' % vcvars)
                f.write("set\n")
            out = subprocess.run(["cmd", "/c", bat], capture_output=True,
                                 text=True, timeout=120).stdout
            for line in out.splitlines():
                key, sep, value = line.partition("=")
                if sep and key:
                    env[key] = value
        except (OSError, subprocess.SubprocessError):
            pass   # fall through to the unmodified environment
        finally:
            if bat:
                try:
                    os.remove(bat)
                except OSError:
                    pass

    # clang-cl ships with LLVM and is usually not on PATH even after vcvars.
    for cand in [os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"),
                              "LLVM", "bin")]:
        if os.path.isdir(cand) and cand.lower() not in env.get("PATH", "").lower():
            env["PATH"] = cand + os.pathsep + env.get("PATH", "")

    _CACHED = env
    return dict(env)


if __name__ == "__main__":
    v = find_vcvars()
    print("vcvars64.bat: %s" % (v or "not found"))
    if os.name == "nt":
        e = environ()
        print("INCLUDE entries: %d" % len(e.get("INCLUDE", "").split(os.pathsep)))
        print("LIB entries:     %d" % len(e.get("LIB", "").split(os.pathsep)))
        import shutil
        print("clang-cl:        %s" % (shutil.which("clang-cl", path=e.get("PATH")) or "not found"))
    sys.exit(0)
