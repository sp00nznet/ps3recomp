#!/usr/bin/env python3
"""Real network writes must return EPIPE without killing the host process."""
import os
import pathlib
import signal
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    if os.name == "nt":
        print("SKIP: SIGPIPE is a POSIX host signal")
        return
    with tempfile.TemporaryDirectory(prefix="ps3recomp-sigpipe-") as tmp:
        for define, modes in (("TEST_SYS_NET", ("send", "sendto-null", "sendto")),
                              ("TEST_HTTP", ("send",)), ("TEST_PSNR", ("send",))):
            binary = pathlib.Path(tmp) / define
            subprocess.run(["cc", "-std=gnu17", "-ffunction-sections", "-fdata-sections",
                            "-Iinclude", "-D" + define, "libs/network/tests/test_socket_sigpipe.c",
                            "-Wl,-dead_strip" if os.uname().sysname == "Darwin" else "-Wl,--gc-sections",
                            "-pthread", "-o", str(binary)], cwd=ROOT, check=True, timeout=120)
            control = subprocess.run([str(binary), "raw", "default"], capture_output=True, timeout=10)
            assert control.returncode == -signal.SIGPIPE, control
            for mode in modes:
                for disposition in ("default", "custom", "success"):
                    p = subprocess.run([str(binary), mode, disposition], capture_output=True,
                                       text=True, timeout=10)
                    assert p.returncode == 0, (define, mode, disposition, p.returncode, p.stderr)
        print("Socket SIGPIPE PASS: sys_net send/sendto, HTTP, PSNR, guest EPIPE, scoped signals")


if __name__ == "__main__":
    main()
