#!/usr/bin/env python3
"""
Build the firmware as WebAssembly: sim/mono_wallet.wasm
========================================================
The same mono_firmware.ino that runs on the XIAO, compiled with the same mocks
the test suite uses, for the browser demo and the Node integration tests.

    python sim/build_wasm.py

Needs Zig (https://ziglang.org/download), which ships clang, wasi-libc and
libc++ in one download: `zig c++ -target wasm32-wasi` is all it takes. Point
ZIG at the binary if it is not on PATH.
"""

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(HERE, "mono_wallet.wasm")


def main():
    zig = os.environ.get("ZIG") or shutil.which("zig")
    if not zig:
        sys.exit("zig not found. Install it from https://ziglang.org/download "
                 "or set ZIG=/path/to/zig")

    cmd = [
        zig, "c++", "-target", "wasm32-wasi",
        "-std=gnu++11",                 # the firmware's real language standard
        "-Os", "-s",                    # small: it is downloaded by every visitor
        "-fno-exceptions", "-fno-rtti",
        "-mexec-model=reactor",         # a library the host calls, not a program
        "-Wno-nullability-completeness",
        "-I", os.path.join(HERE, "mocks"),
        "-o", OUT,
        os.path.join(HERE, "wasm", "main.cpp"),
    ]
    print("  " + " ".join(os.path.relpath(c, ROOT) if os.path.isabs(c) else c for c in cmd[1:]))
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    noise = ("nullability", "In file included from", "note:", "|", "^")
    errors = [l for l in r.stderr.splitlines() if l.strip() and not any(n in l for n in noise)]
    if r.returncode != 0:
        print("\n".join(errors) or r.stderr)
        sys.exit("wasm build failed")
    print(f"  built {os.path.relpath(OUT, ROOT)}  ({os.path.getsize(OUT) / 1024:.1f} KB)")


if __name__ == "__main__":
    main()
