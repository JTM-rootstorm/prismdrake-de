#!/usr/bin/env python3
"""Run one test under an isolated Xvfb selected through -displayfd."""

from __future__ import annotations

import os
import selectors
import subprocess
import sys
from pathlib import Path


def fail(message: str) -> int:
    print(f"Xvfb test harness: {message}", file=sys.stderr)
    return 2


def main() -> int:
    if len(sys.argv) != 3:
        return fail("expected the Xvfb and test executable paths")

    xvfb = Path(sys.argv[1])
    test = Path(sys.argv[2])
    if not xvfb.is_absolute() or not test.is_absolute():
        return fail("executable paths must be absolute")

    server = subprocess.Popen(
        [
            str(xvfb),
            "-displayfd",
            "1",
            "-screen",
            "0",
            "1024x768x24",
            "-noreset",
            "-nolisten",
            "tcp",
        ],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        if server.stdout is None:
            return fail("server display channel is unavailable")
        selector = selectors.DefaultSelector()
        selector.register(server.stdout, selectors.EVENT_READ)
        if not selector.select(timeout=5):
            return fail("server did not publish a display number")
        display_number = server.stdout.readline().strip()
        if not display_number.isascii() or not display_number.isdecimal():
            return fail("server published an invalid display number")

        environment = os.environ.copy()
        environment["DISPLAY"] = f":{display_number}"
        completed = subprocess.run(
            [str(test)],
            stdin=subprocess.DEVNULL,
            env=environment,
            timeout=20,
            check=False,
        )
        return completed.returncode
    except subprocess.TimeoutExpired:
        return fail("test exceeded its bounded timeout")
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait(timeout=5)


if __name__ == "__main__":
    raise SystemExit(main())
