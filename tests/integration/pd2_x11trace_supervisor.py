#!/usr/bin/env python3
"""Bounded long-lived x11trace supervisor for the installed PD2 Tier C replay."""

from __future__ import annotations

import os
import re
import stat
import subprocess
import tempfile
import threading
import time
from collections.abc import Callable, Mapping
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Protocol

from pd2_x11trace import (
    MAX_TRACE_BYTES,
    X11TraceSummary,
    require_tier_c_no_capture_or_blur,
)


X11TRACE_EXECUTABLE = Path("/usr/bin/x11trace")
MAX_LIST_LENGTH = 32
MAX_ENVIRONMENT_ENTRIES = 128
MAX_ENVIRONMENT_VALUE_BYTES = 8192
MIN_CAPTURE_SECONDS = 0.05
MAX_CAPTURE_SECONDS = 900.0
MIN_BOUND_SECONDS = 0.02
MAX_BOUND_SECONDS = 10.0
MONITOR_INTERVAL_SECONDS = 0.02
QUIESCENCE_SECONDS = 0.05
PROCESS_STOP_SECONDS = 0.5

_DISPLAY = re.compile(r"^:(?P<number>0|[1-9][0-9]{0,4})(?:\.0)?$")
_CATEGORIES = frozenset(
    {
        "x11trace_already_finished",
        "x11trace_capture_incomplete",
        "x11trace_client_environment_invalid",
        "x11trace_clients_active",
        "x11trace_executable_invalid",
        "x11trace_fake_display_conflict",
        "x11trace_fake_display_invalid",
        "x11trace_fake_display_timeout",
        "x11trace_output_invalid",
        "x11trace_output_overflow",
        "x11trace_private_root_invalid",
        "x11trace_process_failed",
        "x11trace_process_timeout",
        "x11trace_real_display_invalid",
        "x11trace_runtime_bound_invalid",
        "x11trace_start_failed",
        "x11trace_trace_invalid",
    }
)


class X11TraceSupervisorError(RuntimeError):
    """One redacted collector failure category."""


class ProcessPort(Protocol):
    pid: int

    def poll(self) -> int | None: ...

    def terminate(self) -> None: ...

    def kill(self) -> None: ...

    def wait(self, timeout: float | None = None) -> int: ...


ProcessFactory = Callable[[tuple[str, ...], Mapping[str, str]], ProcessPort]
ExecutableInspector = Callable[[Path], bool]
FakeDisplayObserver = Callable[[int], bool]
Clock = Callable[[], float]
Sleeper = Callable[[float], None]


@dataclass(frozen=True, slots=True)
class X11TraceConfig:
    """Validated private collector inputs; local paths stay out of repr."""

    private_root: Path = field(repr=False)
    real_display: str
    fake_display: str
    maximum_capture_seconds: float = 300.0
    readiness_seconds: float = 3.0
    shutdown_seconds: float = 2.0


def _fail(category: str) -> None:
    if category not in _CATEGORIES:
        category = "x11trace_process_failed"
    error = X11TraceSupervisorError(category)
    error.__cause__ = None
    error.__context__ = None
    raise error from None


def _display_number(value: Any, category: str) -> int:
    if not isinstance(value, str) or "\x00" in value:
        _fail(category)
    match = _DISPLAY.fullmatch(value)
    if match is None:
        _fail(category)
    number = int(match.group("number"))
    if number > 65535:
        _fail(category)
    return number


def _private_directory(path: Path, category: str) -> os.stat_result:
    try:
        metadata = path.lstat()
    except Exception:
        _fail(category)
    if (
        not isinstance(path, Path)
        or not path.is_absolute()
        or ".." in path.parts
        or stat.S_ISLNK(metadata.st_mode)
        or not stat.S_ISDIR(metadata.st_mode)
        or metadata.st_uid != os.geteuid()
        or stat.S_IMODE(metadata.st_mode) != 0o700
    ):
        _fail(category)
    return metadata


def inspect_x11trace_executable(path: Path) -> bool:
    """Accept only the exact non-writable installed Gentoo executable."""

    if path != X11TRACE_EXECUTABLE:
        return False
    try:
        metadata = path.lstat()
    except OSError:
        return False
    return (
        not stat.S_ISLNK(metadata.st_mode)
        and stat.S_ISREG(metadata.st_mode)
        and (metadata.st_mode & 0o111) != 0
        and (metadata.st_mode & 0o022) == 0
    )


def observe_fake_display_socket(number: int) -> bool:
    """Observe x11trace's exact local Unix socket without connecting to it."""

    if isinstance(number, bool) or not isinstance(number, int) or not 0 <= number <= 65535:
        return False
    path = Path(f"/tmp/.X11-unix/X{number}")
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        return False
    except OSError:
        _fail("x11trace_fake_display_invalid")
    if (
        stat.S_ISLNK(metadata.st_mode)
        or not stat.S_ISSOCK(metadata.st_mode)
        or metadata.st_uid != os.geteuid()
    ):
        _fail("x11trace_fake_display_invalid")
    return True


def _default_process_factory(
    arguments: tuple[str, ...], environment: Mapping[str, str]
) -> ProcessPort:
    return subprocess.Popen(
        arguments,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        cwd="/",
        env=dict(environment),
        start_new_session=True,
    )


def _checked_environment(environment: Mapping[str, str]) -> dict[str, str]:
    if (
        not isinstance(environment, Mapping)
        or len(environment) > MAX_ENVIRONMENT_ENTRIES
    ):
        _fail("x11trace_client_environment_invalid")
    checked: dict[str, str] = {}
    for key, value in environment.items():
        if (
            not isinstance(key, str)
            or not key
            or "=" in key
            or "\x00" in key
            or not isinstance(value, str)
            or "\x00" in value
            or len(key.encode("utf-8")) > 256
            or len(value.encode("utf-8")) > MAX_ENVIRONMENT_VALUE_BYTES
        ):
            _fail("x11trace_client_environment_invalid")
        checked[key] = value
    return checked


def _validate_config(config: Any) -> tuple[int, int]:
    if not isinstance(config, X11TraceConfig):
        _fail("x11trace_runtime_bound_invalid")
    _private_directory(config.private_root, "x11trace_private_root_invalid")
    real_number = _display_number(config.real_display, "x11trace_real_display_invalid")
    fake_number = _display_number(config.fake_display, "x11trace_fake_display_invalid")
    if real_number == fake_number:
        _fail("x11trace_fake_display_conflict")
    bounds = (
        (config.maximum_capture_seconds, MIN_CAPTURE_SECONDS, MAX_CAPTURE_SECONDS),
        (config.readiness_seconds, MIN_BOUND_SECONDS, MAX_BOUND_SECONDS),
        (config.shutdown_seconds, MIN_BOUND_SECONDS, MAX_BOUND_SECONDS),
    )
    if any(
        isinstance(value, bool)
        or not isinstance(value, (int, float))
        or not minimum <= value <= maximum
        for value, minimum, maximum in bounds
    ):
        _fail("x11trace_runtime_bound_invalid")
    return real_number, fake_number


class X11TraceSupervisor:
    """Own one long-lived x11trace proxy and destroy all raw trace state."""

    def __init__(
        self,
        config: X11TraceConfig,
        environment: Mapping[str, str],
        *,
        process_factory: ProcessFactory = _default_process_factory,
        executable_inspector: ExecutableInspector = inspect_x11trace_executable,
        fake_display_observer: FakeDisplayObserver = observe_fake_display_socket,
        clock: Clock = time.monotonic,
        sleeper: Sleeper = time.sleep,
    ) -> None:
        _, fake_number = _validate_config(config)
        try:
            executable_valid = executable_inspector(X11TRACE_EXECUTABLE)
        except Exception:
            executable_valid = False
        if executable_valid is not True:
            _fail("x11trace_executable_invalid")
        checked_environment = _checked_environment(environment)
        checked_environment["DISPLAY"] = config.real_display
        checked_environment["FAKEDISPLAY"] = config.fake_display
        checked_environment["LC_ALL"] = "C"
        checked_environment["LANG"] = "C"
        checked_environment.pop("XAUTHORITY", None)
        if len(checked_environment) > MAX_ENVIRONMENT_ENTRIES:
            _fail("x11trace_client_environment_invalid")

        self._config = config
        self._environment = checked_environment
        self._fake_number = fake_number
        self._observer = fake_display_observer
        self._clock = clock
        self._sleeper = sleeper
        self._process: ProcessPort | None = None
        self._started_at = clock()
        self._failure: str | None = None
        self._finished = False
        self._finishing = False
        self._fake_ready = False
        self._monitor_stop = threading.Event()
        self._lock = threading.Lock()
        self._monitor: threading.Thread | None = None
        self._trace_directory: Path | None = None
        self._trace_path: Path | None = None
        self._trace_identity: tuple[int, int] | None = None

        try:
            if fake_display_observer(fake_number):
                _fail("x11trace_fake_display_conflict")
            trace_directory = Path(
                tempfile.mkdtemp(prefix="pd2-x11trace-", dir=config.private_root)
            )
            trace_directory.chmod(0o700)
            _private_directory(trace_directory, "x11trace_output_invalid")
            descriptor, trace_name = tempfile.mkstemp(
                prefix="raw-", suffix=".trace", dir=trace_directory
            )
            os.fchmod(descriptor, 0o600)
            metadata = os.fstat(descriptor)
            os.close(descriptor)
            trace_path = Path(trace_name)
            self._trace_directory = trace_directory
            self._trace_path = trace_path
            self._trace_identity = (metadata.st_dev, metadata.st_ino)
            arguments = (
                str(X11TRACE_EXECUTABLE),
                "--nocopyauthentication",
                "--display",
                config.real_display,
                "--fakedisplay",
                config.fake_display,
                "--waitforclient",
                "--stopwhendone",
                "--maxlistlength",
                str(MAX_LIST_LENGTH),
                "--outfile",
                str(trace_path),
            )
            process = process_factory(arguments, self._environment)
            if (
                isinstance(process.pid, bool)
                or not isinstance(process.pid, int)
                or process.pid <= 0
            ):
                _fail("x11trace_start_failed")
            self._process = process
            deadline = self._clock() + float(config.readiness_seconds)
            while True:
                self._check_running()
                if fake_display_observer(fake_number):
                    self._fake_ready = True
                    break
                if self._clock() >= deadline:
                    _fail("x11trace_fake_display_timeout")
                self._sleeper(min(MONITOR_INTERVAL_SECONDS, max(0.0, deadline - self._clock())))
            self._monitor = threading.Thread(
                target=self._monitor_capture,
                name="pd2-x11trace-supervisor",
                daemon=True,
            )
            self._monitor.start()
        except X11TraceSupervisorError:
            self._abort()
            raise
        except Exception:
            self._abort()
            _fail("x11trace_start_failed")

    @property
    def proxy_display(self) -> str:
        return self._config.fake_display

    @property
    def finished(self) -> bool:
        """Report only whether process/raw-trace cleanup has completed."""

        return self._finished

    def client_environment(self, environment: Mapping[str, str]) -> dict[str, str]:
        """Return a bounded no-auth-copy client environment for the proxy."""

        if self._finished:
            _fail("x11trace_already_finished")
        checked = _checked_environment(environment)
        checked["DISPLAY"] = self._config.fake_display
        checked.pop("FAKEDISPLAY", None)
        checked.pop("XAUTHORITY", None)
        if len(checked) > MAX_ENVIRONMENT_ENTRIES:
            _fail("x11trace_client_environment_invalid")
        return checked

    def _trace_metadata(self) -> os.stat_result:
        path = self._trace_path
        identity = self._trace_identity
        if path is None or identity is None:
            _fail("x11trace_output_invalid")
        try:
            metadata = path.lstat()
        except Exception:
            _fail("x11trace_output_invalid")
        if (
            stat.S_ISLNK(metadata.st_mode)
            or not stat.S_ISREG(metadata.st_mode)
            or metadata.st_uid != os.geteuid()
            or stat.S_IMODE(metadata.st_mode) != 0o600
            or (metadata.st_dev, metadata.st_ino) != identity
        ):
            _fail("x11trace_output_invalid")
        return metadata

    def _check_capture_bounds(self) -> None:
        process = self._process
        if process is None:
            _fail("x11trace_start_failed")
        if (
            self._fake_ready
            and not self._finishing
            and not self._observer(self._fake_number)
        ):
            _fail("x11trace_fake_display_invalid")
        metadata = self._trace_metadata()
        if metadata.st_size > MAX_TRACE_BYTES:
            _fail("x11trace_output_overflow")
        if self._clock() - self._started_at > self._config.maximum_capture_seconds:
            _fail("x11trace_process_timeout")

    def _check_running(self) -> None:
        process = self._process
        if process is None:
            _fail("x11trace_start_failed")
        if process.poll() is not None:
            _fail("x11trace_capture_incomplete")
        self._check_capture_bounds()

    def _set_monitor_failure(self, category: str) -> None:
        with self._lock:
            if self._failure is None:
                self._failure = category
        self._terminate_process()

    def _monitor_capture(self) -> None:
        while not self._monitor_stop.wait(MONITOR_INTERVAL_SECONDS):
            try:
                process = self._process
                if process is None:
                    _fail("x11trace_start_failed")
                returncode = process.poll()
                if returncode is not None:
                    if returncode != 0:
                        self._set_monitor_failure("x11trace_capture_incomplete")
                    return
                self._check_capture_bounds()
            except X11TraceSupervisorError as error:
                self._set_monitor_failure(str(error))
                return
            except Exception:
                self._set_monitor_failure("x11trace_process_failed")
                return

    def _monitor_failure(self) -> str | None:
        with self._lock:
            return self._failure

    def poll(self) -> None:
        """Fail immediately if the proxy, byte cap, or time cap was lost."""

        if self._finished:
            _fail("x11trace_already_finished")
        failure = self._monitor_failure()
        if failure is not None:
            _fail(failure)
        self._check_running()

    def _terminate_process(self) -> None:
        process = self._process
        if process is None or process.poll() is not None:
            return
        try:
            process.terminate()
        except Exception:
            pass
        try:
            process.wait(timeout=float(self._config.shutdown_seconds))
            return
        except Exception:
            pass
        try:
            process.kill()
        except Exception:
            pass
        try:
            process.wait(timeout=PROCESS_STOP_SECONDS)
        except Exception:
            pass

    def _read_complete_trace(self) -> bytes:
        path = self._trace_path
        identity = self._trace_identity
        if path is None or identity is None:
            _fail("x11trace_output_invalid")
        descriptor = -1
        try:
            flags = os.O_RDONLY | os.O_CLOEXEC | os.O_NONBLOCK
            if hasattr(os, "O_NOFOLLOW"):
                flags |= os.O_NOFOLLOW
            descriptor = os.open(path, flags)
            metadata = os.fstat(descriptor)
            if (
                not stat.S_ISREG(metadata.st_mode)
                or metadata.st_uid != os.geteuid()
                or stat.S_IMODE(metadata.st_mode) != 0o600
                or (metadata.st_dev, metadata.st_ino) != identity
                or metadata.st_size > MAX_TRACE_BYTES
            ):
                _fail("x11trace_output_invalid")
            payload = bytearray()
            while len(payload) <= MAX_TRACE_BYTES:
                chunk = os.read(descriptor, min(65_536, MAX_TRACE_BYTES + 1 - len(payload)))
                if not chunk:
                    break
                payload.extend(chunk)
            extra = os.read(descriptor, 1)
            if extra or len(payload) > MAX_TRACE_BYTES:
                _fail("x11trace_output_overflow")
            after = os.fstat(descriptor)
            if (
                (after.st_dev, after.st_ino) != identity
                or after.st_size != metadata.st_size
            ):
                _fail("x11trace_output_invalid")
            return bytes(payload)
        except X11TraceSupervisorError:
            raise
        except Exception:
            _fail("x11trace_output_invalid")
        finally:
            if descriptor >= 0:
                try:
                    os.close(descriptor)
                except OSError:
                    pass

    def _cleanup_raw_trace(self) -> None:
        path = self._trace_path
        directory = self._trace_directory
        self._trace_path = None
        self._trace_directory = None
        if path is not None:
            try:
                metadata = path.lstat()
                if (
                    self._trace_identity is not None
                    and not stat.S_ISLNK(metadata.st_mode)
                    and stat.S_ISREG(metadata.st_mode)
                    and (metadata.st_dev, metadata.st_ino) == self._trace_identity
                ):
                    path.unlink()
            except OSError:
                pass
        if directory is not None:
            try:
                directory.rmdir()
            except OSError:
                pass

    def _abort(self) -> None:
        self._monitor_stop.set()
        monitor = self._monitor
        if monitor is not None and monitor is not threading.current_thread():
            monitor.join(PROCESS_STOP_SECONDS)
        self._terminate_process()
        self._cleanup_raw_trace()
        self._finished = True

    def finish(self, *, clients_exited: bool) -> X11TraceSummary:
        """Stop after client teardown, validate the complete trace, and erase it."""

        if self._finished:
            _fail("x11trace_already_finished")
        if clients_exited is not True:
            self._abort()
            _fail("x11trace_clients_active")
        self._finishing = True
        summary: X11TraceSummary | None = None
        try:
            failure = self._monitor_failure()
            if failure is not None:
                _fail(failure)
            process = self._process
            if process is None:
                _fail("x11trace_process_failed")
            if process.poll() is None:
                self._check_capture_bounds()
            self._sleeper(QUIESCENCE_SECONDS)
            self._monitor_stop.set()
            if self._monitor is not None:
                self._monitor.join(PROCESS_STOP_SECONDS)
            failure = self._monitor_failure()
            if failure is not None:
                _fail(failure)
            try:
                returncode = process.wait(timeout=float(self._config.shutdown_seconds))
            except Exception:
                self._terminate_process()
                _fail("x11trace_capture_incomplete")
            if returncode != 0 or process.poll() != 0:
                _fail("x11trace_capture_incomplete")
            payload = self._read_complete_trace()
            try:
                summary = require_tier_c_no_capture_or_blur(
                    payload, capture_complete=True
                )
            except Exception:
                _fail("x11trace_trace_invalid")
            return summary
        finally:
            self._abort()

    def __enter__(self) -> "X11TraceSupervisor":
        return self

    def __exit__(self, exception_type: Any, exception: Any, traceback: Any) -> None:
        del exception_type, traceback
        if not self._finished:
            if exception is None:
                self.finish(clients_exited=True)
            else:
                self._abort()
