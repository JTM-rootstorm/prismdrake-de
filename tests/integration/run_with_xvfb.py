#!/usr/bin/env python3
"""Run one test under an isolated Xvfb selected through -displayfd."""

from __future__ import annotations

import os
import selectors
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path


# A cold CI runner can take several seconds to initialize fonts and publish the
# display number even though the server is healthy. Keep the default lanes
# within CTest's 30-second envelope. The compositional WP15 collector may opt
# into its separately bounded 4200-second ceiling without changing those lanes.
STARTUP_TIMEOUT_SECONDS = 8
TEST_TIMEOUT_SECONDS = 16
OPENBOX_READY_TIMEOUT_SECONDS = 2
OPENBOX_TEST_TIMEOUT_SECONDS = 12
MAXIMUM_TEST_TIMEOUT_SECONDS = 4200
TERMINATE_TIMEOUT_SECONDS = 3
KILL_TIMEOUT_SECONDS = 2
PROCESS_GROUP_POLL_SECONDS = 0.01


def fail(message: str) -> int:
    print(f"Xvfb test harness: {message}", file=sys.stderr)
    return 2


class HarnessArgumentError(ValueError):
    """One fixed harness-argument contract failure."""


@dataclass(frozen=True)
class HarnessArguments:
    disable_randr: bool
    openbox: Path | None
    xprop: Path | None
    executable_arguments: list[str]
    test_timeout_seconds: int | None


def parse_test_timeout_seconds(value: str) -> int:
    if not value.isascii() or not value.isdecimal() or value.startswith("0"):
        raise HarnessArgumentError("the test timeout must be a strict positive ASCII integer")
    parsed = int(value, 10)
    if parsed > MAXIMUM_TEST_TIMEOUT_SECONDS:
        raise HarnessArgumentError(
            f"the test timeout exceeds the fixed {MAXIMUM_TEST_TIMEOUT_SECONDS}-second bound"
        )
    return parsed


def parse_harness_arguments(arguments: list[str]) -> HarnessArguments:
    disable_randr = False
    openbox: Path | None = None
    xprop: Path | None = None
    test_timeout_seconds: int | None = None
    executable_arguments: list[str] = []
    argument_index = 0
    while argument_index < len(arguments):
        argument = arguments[argument_index]
        if argument == "--xvfb-disable-randr":
            if disable_randr:
                raise HarnessArgumentError("the RandR-disable option may appear only once")
            disable_randr = True
        elif argument in {"--openbox", "--xprop", "--test-timeout-seconds"}:
            argument_index += 1
            if argument_index >= len(arguments):
                raise HarnessArgumentError(f"the {argument} option requires a value")
            value = arguments[argument_index]
            if argument == "--openbox":
                if openbox is not None:
                    raise HarnessArgumentError("the Openbox option may appear only once")
                openbox = Path(value)
            elif argument == "--xprop":
                if xprop is not None:
                    raise HarnessArgumentError("the xprop option may appear only once")
                xprop = Path(value)
            else:
                if test_timeout_seconds is not None:
                    raise HarnessArgumentError("the test timeout option may appear only once")
                test_timeout_seconds = parse_test_timeout_seconds(value)
        else:
            executable_arguments.append(argument)
        argument_index += 1
    if test_timeout_seconds is not None and openbox is None:
        raise HarnessArgumentError("the test timeout override requires the Openbox lane")
    return HarnessArguments(
        disable_randr,
        openbox,
        xprop,
        executable_arguments,
        test_timeout_seconds,
    )


def selected_test_timeout(options: HarnessArguments) -> int:
    if options.test_timeout_seconds is not None:
        return options.test_timeout_seconds
    return OPENBOX_TEST_TIMEOUT_SECONDS if options.openbox is not None else TEST_TIMEOUT_SECONDS


def openbox_environment(environment: dict[str, str]) -> dict[str, str]:
    """Return an Openbox-only C-locale environment without changing the test."""

    selected = environment.copy()
    selected["LC_ALL"] = "C"
    return selected


def failed_test_status(return_code: int) -> str:
    """Describe one non-zero child status without exposing its arguments."""

    if return_code < 0:
        signal_number = -return_code
        try:
            signal_name = signal.Signals(signal_number).name
        except ValueError:
            signal_name = "UNKNOWN"
        return f"test terminated by signal {signal_number} ({signal_name})"
    return f"test exited with status {return_code}"


def process_status(return_code: int | None) -> str:
    """Describe one harness process using bounded, non-private state."""

    if return_code is None:
        return "running"
    if return_code < 0:
        return failed_test_status(return_code).removeprefix("test ")
    return f"exited with status {return_code}"


def start_test_process(
    executable: Path, arguments: list[str], environment: dict[str, str]
) -> subprocess.Popen[bytes]:
    """Start the wrapped command as leader of one cleanup-owned process group."""

    return subprocess.Popen(
        [str(executable), *arguments],
        stdin=subprocess.DEVNULL,
        env=environment,
        start_new_session=True,
    )


def process_group_exists(group_id: int) -> bool:
    """Return whether one exact process group still has any members."""

    try:
        os.killpg(group_id, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def wait_for_test_process_group_exit(
    process: subprocess.Popen[bytes], timeout_seconds: float
) -> bool:
    """Bound waiting for both the direct test child and its process group."""

    deadline = time.monotonic() + timeout_seconds
    while process_group_exists(process.pid):
        process.poll()
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return False
        time.sleep(min(PROCESS_GROUP_POLL_SECONDS, remaining))
    remaining = max(0.0, deadline - time.monotonic())
    try:
        process.wait(timeout=remaining)
    except subprocess.TimeoutExpired:
        return False
    return True


def terminate_test_process_group(
    process: subprocess.Popen[bytes],
    terminate_timeout_seconds: float = TERMINATE_TIMEOUT_SECONDS,
    kill_timeout_seconds: float = KILL_TIMEOUT_SECONDS,
) -> bool:
    """Terminate the exact test process group with bounded TERM/KILL escalation."""

    if not process_group_exists(process.pid):
        return wait_for_test_process_group_exit(process, terminate_timeout_seconds)
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    if wait_for_test_process_group_exit(process, terminate_timeout_seconds):
        return True
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    return wait_for_test_process_group_exit(process, kill_timeout_seconds)


def main() -> int:
    try:
        separator = sys.argv.index("--")
    except ValueError:
        return fail("expected '--' before test arguments")

    harness_arguments = sys.argv[1:separator]
    test_arguments = sys.argv[separator + 1 :]
    try:
        options = parse_harness_arguments(harness_arguments)
    except HarnessArgumentError as error:
        return fail(str(error))
    disable_randr = options.disable_randr
    openbox = options.openbox
    xprop = options.xprop
    executable_arguments = options.executable_arguments

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
        "-fp",
        "built-ins",
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
    test_process: subprocess.Popen[bytes] | None = None
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
            openbox_process = subprocess.Popen(
                [str(openbox), "--sm-disable"],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                env=openbox_environment(environment),
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
        test_process = start_test_process(test, test_arguments, environment)
        try:
            test_return_code = test_process.wait(timeout=selected_test_timeout(options))
        except subprocess.TimeoutExpired:
            cleanup_complete = terminate_test_process_group(test_process)
            if cleanup_complete:
                test_process = None
            openbox_status = (
                process_status(openbox_process.poll())
                if openbox_process is not None
                else "not requested"
            )
            cleanup_status = "complete" if cleanup_complete else "incomplete"
            return fail(
                "test exceeded its bounded timeout; "
                f"test process-group cleanup {cleanup_status}; "
                f"Xvfb {process_status(server.poll())}; Openbox {openbox_status}"
            )
        cleanup_complete = terminate_test_process_group(test_process)
        if cleanup_complete:
            test_process = None
        if not cleanup_complete:
            return fail("test process-group cleanup exceeded its fixed bound")
        if test_return_code < 0:
            openbox_status = (
                process_status(openbox_process.poll())
                if openbox_process is not None
                else "not requested"
            )
            return fail(
                f"{failed_test_status(test_return_code)}; "
                f"Xvfb {process_status(server.poll())}; Openbox {openbox_status}"
            )
        return test_return_code
    finally:
        if test_process is not None:
            terminate_test_process_group(test_process)
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
