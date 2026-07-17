#!/usr/bin/env python3
"""Run one test under an isolated Xvfb selected through -displayfd."""

from __future__ import annotations

import os
import selectors
import subprocess
import sys
import time
from pathlib import Path


# A cold CI runner can take several seconds to initialize fonts and publish the
# display number even though the server is healthy. Keep startup bounded while
# leaving enough of CTest's 30-second envelope for the test and cleanup.
STARTUP_TIMEOUT_SECONDS = 8
TEST_TIMEOUT_SECONDS = 16
OPENBOX_READY_TIMEOUT_SECONDS = 2
OPENBOX_TEST_TIMEOUT_SECONDS = 12
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
    openbox: Path | None = None
    xprop: Path | None = None
    executable_arguments: list[str] = []
    argument_index = 0
    while argument_index < len(harness_arguments):
        argument = harness_arguments[argument_index]
        if argument == "--xvfb-disable-randr":
            if disable_randr:
                return fail("the RandR-disable option may appear only once")
            disable_randr = True
        elif argument == "--openbox":
            if openbox is not None:
                return fail("the Openbox option may appear only once")
            argument_index += 1
            if argument_index >= len(harness_arguments):
                return fail("the Openbox option requires an executable path")
            openbox = Path(harness_arguments[argument_index])
        elif argument == "--xprop":
            if xprop is not None:
                return fail("the xprop option may appear only once")
            argument_index += 1
            if argument_index >= len(harness_arguments):
                return fail("the xprop option requires an executable path")
            xprop = Path(harness_arguments[argument_index])
        else:
            executable_arguments.append(argument)
        argument_index += 1

    if len(executable_arguments) != 2:
        return fail("expected the Xvfb and test executable paths")

    xvfb = Path(executable_arguments[0])
    test = Path(executable_arguments[1])
    if not xvfb.is_absolute() or not test.is_absolute():
        return fail("executable paths must be absolute")
    if openbox is not None and not openbox.is_absolute():
        return fail("the Openbox executable path must be absolute")
    if xprop is not None and not xprop.is_absolute():
        return fail("the xprop executable path must be absolute")
    if (openbox is None) != (xprop is None):
        return fail("the Openbox lane requires both --openbox and --xprop")

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
    openbox_process: subprocess.Popen[str] | None = None
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
        if openbox is not None:
            environment["PRISMDRAKE_TEST_EXPECT_EWMH_WM"] = "1"
            environment["LC_ALL"] = "C"
            openbox_process = subprocess.Popen(
                [str(openbox), "--sm-disable"],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                env=environment,
                text=True,
            )
            readiness_deadline = time.monotonic() + OPENBOX_READY_TIMEOUT_SECONDS
            while time.monotonic() < readiness_deadline:
                status = openbox_process.poll()
                if status is not None:
                    return fail(f"Openbox exited during startup with status {status}")
                try:
                    readiness = subprocess.run(
                        [str(xprop), "-notype", "-root", "_NET_SUPPORTING_WM_CHECK"],
                        stdin=subprocess.DEVNULL,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.DEVNULL,
                        env=environment,
                        text=True,
                        timeout=1,
                        check=False,
                    )
                except subprocess.TimeoutExpired:
                    continue
                if readiness.returncode == 0 and "window id # 0x" in readiness.stdout:
                    break
                time.sleep(0.02)
            else:
                return fail("Openbox did not publish _NET_SUPPORTING_WM_CHECK")
        completed = subprocess.run(
            [str(test), *test_arguments],
            stdin=subprocess.DEVNULL,
            env=environment,
            timeout=(OPENBOX_TEST_TIMEOUT_SECONDS if openbox is not None else TEST_TIMEOUT_SECONDS),
            check=False,
        )
        return completed.returncode
    except subprocess.TimeoutExpired:
        return fail("test exceeded its bounded timeout")
    finally:
        if openbox_process is not None and openbox_process.poll() is None:
            openbox_process.terminate()
            try:
                openbox_process.wait(timeout=TERMINATE_TIMEOUT_SECONDS)
            except subprocess.TimeoutExpired:
                openbox_process.kill()
                openbox_process.wait(timeout=KILL_TIMEOUT_SECONDS)
        server.terminate()
        try:
            server.wait(timeout=TERMINATE_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait(timeout=KILL_TIMEOUT_SECONDS)


if __name__ == "__main__":
    raise SystemExit(main())
