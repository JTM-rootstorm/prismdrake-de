#!/usr/bin/env python3
"""Run one test under an isolated Xvfb selected through -displayfd."""

from __future__ import annotations

import os
import selectors
import subprocess
import sys
from pathlib import Path


STARTUP_TIMEOUT_SECONDS = 3
TEST_TIMEOUT_SECONDS = 16
TERMINATE_TIMEOUT_SECONDS = 3
KILL_TIMEOUT_SECONDS = 2


def fail(message: str) -> int:
    print(f"Xvfb test harness: {message}", file=sys.stderr)
    return 2


def main() -> int:
    try:
        separator = sys.argv.index("--")
    except ValueError:
        return fail("expected '--' before test arguments")

    harness_arguments = sys.argv[1:separator]
    test_arguments = sys.argv[separator + 1 :]
    disable_randr = False
    executable_arguments: list[str] = []
    for argument in harness_arguments:
        if argument == "--xvfb-disable-randr":
            if disable_randr:
                return fail("the RandR-disable option may appear only once")
            disable_randr = True
        else:
            executable_arguments.append(argument)

    if len(executable_arguments) != 2:
        return fail("expected the Xvfb and test executable paths")

    xvfb = Path(executable_arguments[0])
    test = Path(executable_arguments[1])
    if not xvfb.is_absolute() or not test.is_absolute():
        return fail("executable paths must be absolute")

    server_arguments = [
        str(xvfb),
        "-displayfd",
        "1",
        "-screen",
        "0",
        "1024x768x24",
        "-noreset",
        "-nolisten",
        "tcp",
    ]
    if disable_randr:
        server_arguments.extend(["-extension", "RANDR"])

    server = subprocess.Popen(
        server_arguments,
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
        if not selector.select(timeout=STARTUP_TIMEOUT_SECONDS):
            return fail("server did not publish a display number")
        display_number = server.stdout.readline().strip()
        if not display_number.isascii() or not display_number.isdecimal():
            return fail("server published an invalid display number")

        environment = os.environ.copy()
        environment["DISPLAY"] = f":{display_number}"
        completed = subprocess.run(
            [str(test), *test_arguments],
            stdin=subprocess.DEVNULL,
            env=environment,
            timeout=TEST_TIMEOUT_SECONDS,
            check=False,
        )
        return completed.returncode
    except subprocess.TimeoutExpired:
        return fail("test exceeded its bounded timeout")
    finally:
        server.terminate()
        try:
            server.wait(timeout=TERMINATE_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait(timeout=KILL_TIMEOUT_SECONDS)


if __name__ == "__main__":
    raise SystemExit(main())
