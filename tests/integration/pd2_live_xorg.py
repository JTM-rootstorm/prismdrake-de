#!/usr/bin/env python3
"""Live Xorg-dummy adapter for the PD2 installed demonstration contract.

This module owns only observations it can make directly through Xorg, RandR,
and the reference EWMH window manager.  Every shell, service, and accessibility
predicate remains unsupported until a live observer is added; unsupported
steps fail closed and therefore cannot produce PD2 completion evidence.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import signal
import socket
import stat
import subprocess
import tempfile
import time
from collections.abc import Callable, Mapping, Sequence
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Protocol

from pd2_demo import EXECUTABLE_NAMES, HarnessError, Observation
from pd2_live_atspi import (
    ControlSelector,
    LiveAtspiError,
    invoke_reviewed_action,
    observe_control_absence,
    observe_control_state,
    observe_showing_control,
)
from pd2_live_dbus import LiveDbusProbe, PrivateBusEndpoint
from pd2_live_notification_workflow import (
    InstalledNotificationWorkflow,
    LiveDbusNotificationPort,
    NotificationSurfacePort,
    WorkflowResult,
    validate_workflow_result,
)
from pd2_live_notifications import PersistentNotificationProbe
from pd2_live_settings_workflow import (
    AccessibilitySurfacePort,
    InstalledSettingsWorkflow,
    SettingsWorkflowConfig,
    SettingsWorkflowResult,
    SurfaceObservationPort,
)
from pd2_xwd import capture_window_fingerprint


MAX_COMMAND_OUTPUT_BYTES = 65_536
MAX_EXECUTABLE_BYTES = 128 * 1024 * 1024
MAX_PROCESS_SCAN_ENTRIES = 32_768
MAX_PROCESS_ANCESTRY_DEPTH = 64
MAX_SURFACE_WINDOWS = 64
OUTPUT_MODE = "1600x900"
OUTPUT_WIDTH = 1600
OUTPUT_HEIGHT = 900
PANEL_HEIGHT = 48
PRIMARY_OUTPUT = "DUMMY0"
SECONDARY_OUTPUT = "DUMMY1"
CONTROLLED_APPLICATION_NAME = "Prismdrake Controlled Application"
CONTROLLED_DESKTOP_FILE = "prismdrake-controlled.desktop"
CONTROLLED_WINDOW_ONE = "Prismdrake Demo App One"
CONTROLLED_WINDOW_TWO = "Prismdrake Demo App Two"
QUICK_SETTINGS_WINDOW_TITLE = "Prismdrake Quick Settings"
SETTINGS_WORKFLOW_CONFIG = SettingsWorkflowConfig(
    ("panel_primary", "panel_secondary", "quick_settings"),
    (
        ((400, 2), (800, 2), (1200, 2)),
        ((400, 2), (800, 2), (1200, 2)),
        ((24, 2), (190, 2), (355, 2)),
    ),
)


@dataclass(frozen=True)
class CommandResult:
    """Bounded command result used by the adapter."""

    returncode: int
    stdout: bytes = b""
    stderr: bytes = b""


@dataclass(frozen=True)
class ProcessIdentity:
    """Ephemeral identity used only to prove distinct live process owners."""

    pid: int
    start_time: int


class ChildProcess(Protocol):
    """Minimal exact-child interface needed for bounded cleanup."""

    pid: int

    def poll(self) -> int | None: ...

    def wait(self, timeout: float | None = None) -> int: ...


class Operations(Protocol):
    """Injectable process boundary; production uses real subprocesses."""

    def spawn(self, argv: Sequence[str], env: Mapping[str, str]) -> ChildProcess: ...

    def run(
        self, argv: Sequence[str], env: Mapping[str, str], timeout: float
    ) -> CommandResult: ...

    def stop(self, child: ChildProcess, timeout: float) -> bool: ...

    def display_available(self, number: int) -> bool: ...

    def identify(
        self, child: ChildProcess, executable: Path
    ) -> ProcessIdentity | None: ...

    def descendants(
        self, child: ChildProcess, executable: Path
    ) -> Sequence[ProcessIdentity]: ...

    def identify_pid(self, pid: int, executable: Path) -> ProcessIdentity | None: ...

    def stop_identity(
        self, identity: ProcessIdentity, executable: Path, timeout: float
    ) -> bool: ...

    def socket_identity(self, path: Path) -> tuple[int, int] | None: ...

    def socket_connectable(self, path: Path) -> bool: ...

    def monotonic(self) -> float: ...

    def pause(self, duration: float) -> None: ...


class AccessibilityOperations(Protocol):
    """Bounded shell-owned AT-SPI observation and action boundary."""

    def observe(
        self, shell_pid: int, selector: ControlSelector, environment: Mapping[str, str]
    ) -> bool: ...

    def invoke(
        self, shell_pid: int, selector: ControlSelector, environment: Mapping[str, str]
    ) -> bool: ...

    def observe_state(
        self,
        shell_pid: int,
        selector: ControlSelector,
        state_name: str,
        expected: bool,
        environment: Mapping[str, str],
    ) -> bool: ...

    def observe_absent(
        self, shell_pid: int, selector: ControlSelector, environment: Mapping[str, str]
    ) -> bool: ...

    def reject_stale_action(
        self, shell_pid: int, selector: ControlSelector, environment: Mapping[str, str]
    ) -> bool: ...


class SystemAccessibilityOperations:
    """Real AT-SPI adapter that returns only fixed categorical outcomes."""

    def __init__(self) -> None:
        self._atspi: object | None = None

    @staticmethod
    @contextmanager
    def _environment(environment: Mapping[str, str]):
        if not all(
            isinstance(environment.get(name), str) and environment[name]
            for name in ("DBUS_SESSION_BUS_ADDRESS", "DISPLAY")
        ):
            raise HarnessError("live_atspi_environment_invalid")
        selected = {
            name: environment[name]
            for name in ("DBUS_SESSION_BUS_ADDRESS", "DISPLAY")
        }
        previous = {name: os.environ.get(name) for name in selected}
        try:
            os.environ.update(selected)
            yield
        finally:
            for name, value in previous.items():
                if value is None:
                    os.environ.pop(name, None)
                else:
                    os.environ[name] = value

    def _binding(self) -> object:
        if self._atspi is None:
            try:
                import gi

                gi.require_version("Atspi", "2.0")
                from gi.repository import Atspi

                Atspi.init()
            except Exception as error:
                raise HarnessError("live_atspi_unavailable") from error
            self._atspi = Atspi
        return self._atspi

    @staticmethod
    def _owner(shell_pid: int):
        def matches(application: object) -> bool:
            return application.get_process_id() == shell_pid

        return matches

    def observe(
        self, shell_pid: int, selector: ControlSelector, environment: Mapping[str, str]
    ) -> bool:
        try:
            with self._environment(environment):
                observe_showing_control(
                    self._binding(), self._owner(shell_pid), selector
                )
        except LiveAtspiError:
            return False
        return True

    def invoke(
        self, shell_pid: int, selector: ControlSelector, environment: Mapping[str, str]
    ) -> bool:
        try:
            with self._environment(environment):
                invoke_reviewed_action(
                    self._binding(), self._owner(shell_pid), selector
                )
        except LiveAtspiError:
            return False
        return True

    def observe_state(
        self,
        shell_pid: int,
        selector: ControlSelector,
        state_name: str,
        expected: bool,
        environment: Mapping[str, str],
    ) -> bool:
        try:
            with self._environment(environment):
                observe_control_state(
                    self._binding(),
                    self._owner(shell_pid),
                    selector,
                    state_name,
                    expected,
                )
        except LiveAtspiError:
            return False
        return True

    def observe_absent(
        self, shell_pid: int, selector: ControlSelector, environment: Mapping[str, str]
    ) -> bool:
        try:
            with self._environment(environment):
                observe_control_absence(
                    self._binding(), self._owner(shell_pid), selector
                )
        except LiveAtspiError:
            return False
        return True

    def reject_stale_action(
        self, shell_pid: int, selector: ControlSelector, environment: Mapping[str, str]
    ) -> bool:
        try:
            with self._environment(environment):
                invoke_reviewed_action(
                    self._binding(), self._owner(shell_pid), selector
                )
        except LiveAtspiError as error:
            return str(error) == "atspi_control_identity"
        return False


class SystemOperations:
    """Real bounded process operations for a private live demonstration run."""

    def spawn(self, argv: Sequence[str], env: Mapping[str, str]) -> ChildProcess:
        try:
            return subprocess.Popen(  # noqa: S603 - exact reviewed argv, no shell
                list(argv),
                env=dict(env),
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
                close_fds=True,
            )
        except OSError as error:
            raise HarnessError("live_process_start_failed") from error

    def run(
        self, argv: Sequence[str], env: Mapping[str, str], timeout: float
    ) -> CommandResult:
        try:
            completed = subprocess.run(  # noqa: S603 - exact reviewed argv, no shell
                list(argv),
                env=dict(env),
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=timeout,
                check=False,
            )
        except subprocess.TimeoutExpired as error:
            raise HarnessError("live_command_timeout") from error
        except OSError as error:
            raise HarnessError("live_command_failed") from error
        if len(completed.stdout) + len(completed.stderr) > MAX_COMMAND_OUTPUT_BYTES:
            raise HarnessError("live_command_output_oversized")
        return CommandResult(completed.returncode, completed.stdout, completed.stderr)

    def stop(self, child: ChildProcess, timeout: float) -> bool:
        deadline = time.monotonic() + timeout
        group = child.pid
        try:
            if child.poll() is None:
                group = os.getpgid(child.pid)
            if self._process_group_alive(group):
                os.killpg(group, signal.SIGTERM)
                try:
                    child.wait(timeout=max(timeout / 2.0, 0.01))
                except subprocess.TimeoutExpired:
                    pass
        except OSError:
            return child.poll() is not None
        if not self._process_group_alive(group):
            return child.poll() is not None
        try:
            os.killpg(group, signal.SIGKILL)
            if child.poll() is None:
                child.wait(timeout=max(deadline - time.monotonic(), 0.01))
        except (OSError, subprocess.TimeoutExpired):
            pass
        while time.monotonic() < deadline:
            if not self._process_group_alive(group):
                return child.poll() is not None
            time.sleep(0.01)
        return child.poll() is not None and not self._process_group_alive(group)

    @staticmethod
    def _process_group_alive(group: int) -> bool:
        try:
            os.killpg(group, 0)
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
        return True

    def display_available(self, number: int) -> bool:
        return not Path(f"/tmp/.X{number}-lock").exists() and not Path(
            f"/tmp/.X11-unix/X{number}"
        ).exists()

    def identify(
        self, child: ChildProcess, executable: Path
    ) -> ProcessIdentity | None:
        if child.poll() is not None:
            return None
        record = self._process_record(child.pid)
        if record is None or not self._executable_matches(child.pid, executable):
            return None
        _parent, start_time = record
        return ProcessIdentity(child.pid, start_time)

    def descendants(
        self, child: ChildProcess, executable: Path
    ) -> Sequence[ProcessIdentity]:
        if child.poll() is not None:
            return ()
        records: dict[int, tuple[int, int]] = {}
        try:
            entries = tuple(Path("/proc").iterdir())
        except OSError:
            return ()
        if len(entries) > MAX_PROCESS_SCAN_ENTRIES:
            return ()
        for entry in entries:
            if not entry.name.isascii() or not entry.name.isdecimal():
                continue
            pid = int(entry.name)
            record = self._process_record(pid)
            if record is not None:
                records[pid] = record

        matches: list[ProcessIdentity] = []
        for pid, (_parent, start_time) in records.items():
            if pid == child.pid or not self._is_descendant(pid, child.pid, records):
                continue
            if self._executable_matches(pid, executable):
                matches.append(ProcessIdentity(pid, start_time))
        return tuple(sorted(matches, key=lambda identity: identity.pid))

    def identify_pid(self, pid: int, executable: Path) -> ProcessIdentity | None:
        record = self._process_record(pid)
        if record is None or not self._executable_matches(pid, executable):
            return None
        return ProcessIdentity(pid, record[1])

    def stop_identity(
        self, identity: ProcessIdentity, executable: Path, timeout: float
    ) -> bool:
        if self.identify_pid(identity.pid, executable) != identity:
            return True
        try:
            os.kill(identity.pid, signal.SIGTERM)
        except ProcessLookupError:
            return True
        except OSError:
            return False
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.identify_pid(identity.pid, executable) != identity:
                return True
            time.sleep(0.01)
        if self.identify_pid(identity.pid, executable) != identity:
            return True
        try:
            os.kill(identity.pid, signal.SIGKILL)
        except ProcessLookupError:
            return True
        except OSError:
            return False
        while time.monotonic() < deadline + 0.25:
            if self.identify_pid(identity.pid, executable) != identity:
                return True
            time.sleep(0.01)
        return False

    def socket_identity(self, path: Path) -> tuple[int, int] | None:
        try:
            metadata = path.lstat()
        except FileNotFoundError:
            return None
        except OSError as error:
            raise HarnessError("private_session_bus_socket_invalid") from error
        if not stat.S_ISSOCK(metadata.st_mode) or metadata.st_uid != os.geteuid():
            raise HarnessError("private_session_bus_socket_invalid")
        return metadata.st_dev, metadata.st_ino

    def socket_connectable(self, path: Path) -> bool:
        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            client.settimeout(0.05)
            client.connect(os.fspath(path))
        except OSError:
            return False
        finally:
            client.close()
        return True

    @staticmethod
    def _process_record(pid: int) -> tuple[int, int] | None:
        try:
            payload = Path(f"/proc/{pid}/stat").read_bytes()
        except OSError:
            return None
        closing = payload.rfind(b")")
        if closing < 1 or closing + 2 >= len(payload):
            return None
        fields = payload[closing + 2 :].split()
        if len(fields) <= 19:
            return None
        try:
            parent = int(fields[1])
            start_time = int(fields[19])
        except ValueError:
            return None
        if parent < 0 or start_time <= 0:
            return None
        return parent, start_time

    @staticmethod
    def _is_descendant(
        pid: int, ancestor: int, records: Mapping[int, tuple[int, int]]
    ) -> bool:
        visited: set[int] = set()
        current = pid
        for _depth in range(MAX_PROCESS_ANCESTRY_DEPTH):
            record = records.get(current)
            if record is None:
                return False
            parent = record[0]
            if parent == ancestor:
                return True
            if parent <= 1 or parent in visited:
                return False
            visited.add(parent)
            current = parent
        return False

    @staticmethod
    def _executable_matches(pid: int, executable: Path) -> bool:
        try:
            expected = executable.stat()
            observed = Path(f"/proc/{pid}/exe").stat()
        except OSError:
            return False
        return (expected.st_dev, expected.st_ino) == (observed.st_dev, observed.st_ino)

    def monotonic(self) -> float:
        return time.monotonic()

    def pause(self, duration: float) -> None:
        time.sleep(duration)


class LiveNotificationSurfaceAdapter:
    """Narrow bounded AT-SPI surface for installed notification evidence."""

    _ACTION = ControlSelector("Review", "push_button", "Press")
    _CENTER = ControlSelector("Notification center", "panel")
    _DO_NOT_DISTURB = ControlSelector("Do not disturb", "check_box")
    _TRANSIENT = ControlSelector("Transient notifications", "notification")

    def __init__(
        self,
        accessibility: AccessibilityOperations,
        shell_identity: Callable[[], ProcessIdentity],
        environment: Callable[[], Mapping[str, str]],
        monotonic: Callable[[], float],
        pause: Callable[[float], None],
    ) -> None:
        self._accessibility = accessibility
        self._shell_identity = shell_identity
        self._environment = environment
        self._monotonic = monotonic
        self._pause = pause

    def _wait(
        self, predicate: Callable[[int, Mapping[str, str]], bool], timeout: float,
        category: str,
    ) -> None:
        if (
            isinstance(timeout, bool)
            or not isinstance(timeout, (int, float))
            or not 0.05 <= timeout <= 10.0
        ):
            raise HarnessError("notification_surface_timeout_invalid")
        deadline = self._monotonic() + float(timeout)
        while self._monotonic() < deadline:
            shell = self._shell_identity()
            environment = self._environment()
            if predicate(shell.pid, environment):
                return
            remaining = deadline - self._monotonic()
            if remaining > 0:
                self._pause(min(0.05, remaining))
        raise HarnessError(category)

    def invoke_live_action(self, timeout_seconds: float) -> None:
        self._wait(
            lambda pid, environment: self._accessibility.observe(
                pid, self._ACTION, environment
            ),
            timeout_seconds,
            "notification_surface_action_timeout",
        )
        shell = self._shell_identity()
        if not self._accessibility.invoke(
            shell.pid, self._ACTION, self._environment()
        ):
            raise HarnessError("notification_surface_action_rejected")

    def observe_center(self, timeout_seconds: float) -> None:
        self._wait(
            lambda pid, environment: self._accessibility.observe(
                pid, self._CENTER, environment
            ),
            timeout_seconds,
            "notification_surface_center_timeout",
        )

    def observe_do_not_disturb(
        self, enabled: bool, timeout_seconds: float
    ) -> None:
        if enabled is not True:
            raise HarnessError("notification_surface_state_not_reviewed")
        self._wait(
            lambda pid, environment: self._accessibility.observe_state(
                pid, self._DO_NOT_DISTURB, "CHECKED", True, environment
            ),
            timeout_seconds,
            "notification_surface_dnd_timeout",
        )

    def observe_transient(self, present: bool, timeout_seconds: float) -> None:
        if not isinstance(present, bool):
            raise HarnessError("notification_surface_state_not_reviewed")
        if present:
            predicate = lambda pid, environment: self._accessibility.observe(
                pid, self._TRANSIENT, environment
            )
            category = "notification_surface_transient_presence_timeout"
        else:
            predicate = lambda pid, environment: self._accessibility.observe_absent(
                pid, self._TRANSIENT, environment
            )
            category = "notification_surface_transient_absence_timeout"
        self._wait(predicate, timeout_seconds, category)


class LiveSettingsAccessibilityAdapter:
    """Exact shell-owned AT-SPI observer for the quick-settings surface."""

    _SELECTOR = ControlSelector("Quick settings", "pane")

    def __init__(
        self,
        accessibility: AccessibilityOperations,
        shell_identity: Callable[[], ProcessIdentity],
        environment: Callable[[], Mapping[str, str]],
        monotonic: Callable[[], float],
        pause: Callable[[float], None],
        timeout_seconds: float,
    ) -> None:
        self._accessibility = accessibility
        self._shell_identity = shell_identity
        self._environment = environment
        self._monotonic = monotonic
        self._pause = pause
        self._timeout_seconds = timeout_seconds

    def observe_surface(self, name: str, role: str) -> bool:
        if (name, role) != (self._SELECTOR.name, self._SELECTOR.role):
            return False
        deadline = self._monotonic() + self._timeout_seconds
        while self._monotonic() < deadline:
            shell = self._shell_identity()
            if self._accessibility.observe(
                shell.pid, self._SELECTOR, self._environment()
            ):
                return True
            remaining = deadline - self._monotonic()
            if remaining > 0:
                self._pause(min(0.05, remaining))
        return False


SettingsWindowResolver = Callable[[str], int]
SettingsFingerprintCapture = Callable[
    [int, Sequence[tuple[int, int]]], bytes
]


class LiveSettingsSurfaceAdapter:
    """Sequential reviewed-surface capture through the private XWD boundary."""

    def __init__(
        self,
        config: SettingsWorkflowConfig,
        resolve_window: SettingsWindowResolver,
        capture_fingerprint: SettingsFingerprintCapture,
        pause: Callable[[float], None],
    ) -> None:
        self._config = config
        self._resolve_window = resolve_window
        self._capture_fingerprint = capture_fingerprint
        self._pause = pause

    def reviewed_surface_sequence(self) -> Sequence[str]:
        return self._config.reviewed_surfaces

    def capture_settled_fingerprints(
        self, surface: str, points: Sequence[tuple[int, int]]
    ) -> tuple[bytes, bytes]:
        if (
            surface not in self._config.reviewed_surfaces
            or tuple(points) != self._config.points_for(surface)
        ):
            raise HarnessError("settings_surface_capture_not_reviewed")
        window = self._resolve_window(surface)
        first = self._capture_fingerprint(window, points)
        self._pause(0.05)
        second = self._capture_fingerprint(window, points)
        return first, second


class _Closable(Protocol):
    def close(self) -> None: ...


@dataclass
class _NotificationEpoch:
    workflow: InstalledNotificationWorkflow
    sender: _Closable
    dbus_probe: _Closable

    def close(self) -> bool:
        clean = True
        for resource in (self.sender, self.dbus_probe):
            try:
                resource.close()
            except Exception:
                clean = False
        return clean


@dataclass
class _SettingsEpoch:
    workflow: InstalledSettingsWorkflow
    dbus_probe: _Closable

    def close(self) -> bool:
        try:
            self.dbus_probe.close()
        except Exception:
            return False
        return True


NotificationEpochFactory = Callable[
    [Mapping[str, str], PrivateBusEndpoint, NotificationSurfacePort],
    _NotificationEpoch,
]
SettingsEpochFactory = Callable[
    [
        Mapping[str, str],
        PrivateBusEndpoint,
        AccessibilitySurfacePort,
        SurfaceObservationPort,
    ],
    _SettingsEpoch,
]


def _create_notification_epoch(
    environment: Mapping[str, str],
    endpoint: PrivateBusEndpoint,
    surface: NotificationSurfacePort,
) -> _NotificationEpoch:
    sender: PersistentNotificationProbe | None = None
    dbus_probe: LiveDbusProbe | None = None
    try:
        import gi

        gi.require_version("Gio", "2.0")
        gi.require_version("GLib", "2.0")
        from gi.repository import Gio, GLib

        sender = PersistentNotificationProbe.connect_private(environment, Gio, GLib)
        dbus_probe = LiveDbusProbe(environment, endpoint=endpoint)
        workflow = InstalledNotificationWorkflow(
            sender, LiveDbusNotificationPort(dbus_probe), surface, PRIMARY_OUTPUT
        )
        return _NotificationEpoch(workflow, sender, dbus_probe)
    except Exception:
        for resource in (sender, dbus_probe):
            if resource is not None:
                try:
                    resource.close()
                except Exception:
                    pass
        raise HarnessError("live_notification_epoch_initialization_failed") from None


def _create_settings_epoch(
    environment: Mapping[str, str],
    endpoint: PrivateBusEndpoint,
    accessibility: AccessibilitySurfacePort,
    surfaces: SurfaceObservationPort,
) -> _SettingsEpoch:
    dbus_probe: LiveDbusProbe | None = None
    try:
        dbus_probe = LiveDbusProbe(environment, endpoint=endpoint)
        workflow = InstalledSettingsWorkflow(
            SETTINGS_WORKFLOW_CONFIG, dbus_probe, accessibility, surfaces
        )
        return _SettingsEpoch(workflow, dbus_probe)
    except Exception:
        if dbus_probe is not None:
            try:
                dbus_probe.close()
            except Exception:
                pass
        raise HarnessError("live_settings_epoch_initialization_failed") from None


@dataclass(frozen=True)
class XrandrOutput:
    """One strictly parsed RandR output line."""

    name: str
    connected: bool
    primary: bool
    width: int | None
    height: int | None
    x: int | None
    y: int | None

    @property
    def active(self) -> bool:
        return self.connected and self.width is not None


@dataclass(frozen=True)
class WindowGeometry:
    """Bounded mapped X11 window geometry from ``xwininfo``."""

    x: int
    y: int
    width: int
    height: int


_OUTPUT_LINE = re.compile(
    rb"^(?P<name>[A-Za-z0-9_.:-]{1,128}) "
    rb"(?P<state>connected|disconnected)"
    rb"(?P<primary> primary)?"
    rb"(?: (?P<width>[1-9][0-9]{0,5})x(?P<height>[1-9][0-9]{0,5})"
    rb"\+(?P<x>[0-9]{1,6})\+(?P<y>[0-9]{1,6}))?"
    rb"(?: |$)"
)


def parse_xrandr_outputs(payload: bytes) -> tuple[XrandrOutput, ...]:
    """Parse bounded ``xrandr --query`` output without accepting ambiguity."""

    if not payload or len(payload) > MAX_COMMAND_OUTPUT_BYTES or b"\x00" in payload:
        raise HarnessError("xrandr_output_invalid")
    outputs: list[XrandrOutput] = []
    names: set[str] = set()
    for line in payload.splitlines():
        match = _OUTPUT_LINE.match(line)
        if match is None:
            continue
        try:
            name = match.group("name").decode("ascii")
        except UnicodeDecodeError as error:
            raise HarnessError("xrandr_output_invalid") from error
        if name in names:
            raise HarnessError("xrandr_output_duplicate")
        names.add(name)
        values = {
            key: int(match.group(key)) if match.group(key) is not None else None
            for key in ("width", "height", "x", "y")
        }
        connected = match.group("state") == b"connected"
        if not connected and any(value is not None for value in values.values()):
            raise HarnessError("xrandr_output_invalid")
        outputs.append(
            XrandrOutput(
                name=name,
                connected=connected,
                primary=match.group("primary") is not None,
                width=values["width"],
                height=values["height"],
                x=values["x"],
                y=values["y"],
            )
        )
    if not outputs:
        raise HarnessError("xrandr_output_invalid")
    return tuple(outputs)


def parse_window_ids(payload: bytes, property_name: bytes) -> tuple[int, ...]:
    """Parse one bounded X11 WINDOW-list property without retaining names."""

    if (
        not payload
        or len(payload) > MAX_COMMAND_OUTPUT_BYTES
        or b"\x00" in payload
        or re.fullmatch(rb"_[A-Z0-9_]{1,80}", property_name) is None
    ):
        raise HarnessError("x11_window_list_invalid")
    prefix = property_name + b"(WINDOW): window id # "
    if not payload.startswith(prefix) or not payload.endswith(b"\n"):
        raise HarnessError("x11_window_list_invalid")
    values = payload[len(prefix) : -1].split(b", ")
    if not values or len(values) > MAX_SURFACE_WINDOWS:
        raise HarnessError("x11_window_list_invalid")
    result: list[int] = []
    for value in values:
        if re.fullmatch(rb"0x[0-9a-f]{1,8}", value) is None:
            raise HarnessError("x11_window_list_invalid")
        parsed = int(value, 16)
        if parsed == 0 or parsed in result:
            raise HarnessError("x11_window_list_invalid")
        result.append(parsed)
    return tuple(result)


def parse_cardinals(payload: bytes, property_name: bytes, count: int) -> tuple[int, ...]:
    """Parse one exact bounded CARDINAL property."""

    if (
        not payload
        or len(payload) > MAX_COMMAND_OUTPUT_BYTES
        or b"\x00" in payload
        or count < 1
        or count > 16
        or re.fullmatch(rb"_[A-Z0-9_]{1,80}", property_name) is None
    ):
        raise HarnessError("x11_cardinal_property_invalid")
    prefix = property_name + b"(CARDINAL) = "
    if not payload.startswith(prefix) or not payload.endswith(b"\n"):
        raise HarnessError("x11_cardinal_property_invalid")
    values = payload[len(prefix) : -1].split(b", ")
    if len(values) != count:
        raise HarnessError("x11_cardinal_property_invalid")
    result: list[int] = []
    for value in values:
        if re.fullmatch(rb"[0-9]{1,10}", value) is None:
            raise HarnessError("x11_cardinal_property_invalid")
        parsed = int(value)
        if parsed > 0xFFFF_FFFF:
            raise HarnessError("x11_cardinal_property_invalid")
        result.append(parsed)
    return tuple(result)


def parse_window_geometry(payload: bytes) -> WindowGeometry:
    """Extract geometry and mapping state without retaining a window title."""

    if not payload or len(payload) > MAX_COMMAND_OUTPUT_BYTES or b"\x00" in payload:
        raise HarnessError("x11_window_geometry_invalid")
    patterns = {
        "x": rb"^  Absolute upper-left X:\s+(-?[0-9]{1,7})$",
        "y": rb"^  Absolute upper-left Y:\s+(-?[0-9]{1,7})$",
        "width": rb"^  Width:\s+([1-9][0-9]{0,6})$",
        "height": rb"^  Height:\s+([1-9][0-9]{0,6})$",
        "border": rb"^  Border width:\s+([0-9]{1,6})$",
    }
    values: dict[str, int] = {}
    lines = payload.splitlines()
    for key, pattern in patterns.items():
        matches = [re.fullmatch(pattern, line) for line in lines]
        captures = [match.group(1) for match in matches if match is not None]
        if len(captures) != 1:
            raise HarnessError("x11_window_geometry_invalid")
        values[key] = int(captures[0])
    if (
        values["border"] != 0
        or lines.count(b"  Map State: IsViewable") != 1
        or lines.count(b"  Override Redirect State: no") != 1
    ):
        raise HarnessError("x11_window_geometry_invalid")
    return WindowGeometry(values["x"], values["y"], values["width"], values["height"])


def _session_config() -> str:
    """Return the complete private PD2 demonstration configuration."""

    return """schema_version = 2
profile = "lustre"

[appearance]
accent = "#4D7FFF"
transparency_enabled = true
blur_quality = "balanced"
reduced_motion = false
high_contrast = false
text_scale = 1.0
cursor_theme = "default"
cursor_size_px = 24
icon_theme = "hicolor"

[panel]
edge = "bottom"
size_px = 48
autohide = "never"
grouping = "always"
clock_format = "locale"
output_policy = "all"
outputs = []

[launcher]
layout = "expanded"
search_providers = ["applications", "settings"]
recent_items = "local_only"

[notifications]
enabled = true
history = "session"
default_timeout_ms = 5000
do_not_disturb = false

[desktop]
wallpaper_source = ""
background_color = "#182033"
wallpaper_mode = "fill"
icons_enabled = false

[integration]
export_gtk = true
export_qt = true
export_xsettings = true
export_portal = true

[accessibility]
focus_emphasis = "standard"
animation_scale = 1.0
minimum_target_size_px = 40

[keyboard]
menu_key_opens_launcher = true
focus_wraps = false

[developer]
diagnostics_enabled = false
mock_capability_overrides = []
"""


@dataclass(frozen=True)
class LiveXorgConfig:
    """Exact executable inputs for a private Xorg-dummy demonstration run."""

    xorg: Path
    xrandr: Path
    openbox: Path
    xprop: Path
    xwininfo: Path
    dbus_daemon: Path
    installed_executables: Mapping[str, Path]
    xdotool: Path | None = None
    xwd: Path | None = None
    controlled_window_fixture: Path | None = None
    first_display: int = 120
    last_display: int = 159
    readiness_timeout: float = 8.0
    stop_timeout: float = 3.0

    def __post_init__(self) -> None:
        if not (1 <= self.first_display <= self.last_display <= 999):
            raise HarnessError("live_display_range_invalid")
        if self.readiness_timeout <= 0 or self.stop_timeout <= 0:
            raise HarnessError("live_timeout_invalid")


def _xorg_config() -> str:
    """Return the private deterministic two-output dummy-server configuration."""

    return """Section \"ServerFlags\"
    Option \"AutoAddDevices\" \"false\"
    Option \"AutoEnableDevices\" \"false\"
EndSection

Section \"Device\"
    Identifier \"PrismdrakeDummyDevice\"
    Driver \"dummy\"
    VideoRam 256000
EndSection

Section \"Monitor\"
    Identifier \"PrismdrakeDummyMonitor\"
    HorizSync 5.0-1000.0
    VertRefresh 5.0-200.0
    Modeline \"1600x900\" 118.25 1600 1696 1856 2112 900 903 908 934 -HSync +VSync
EndSection

Section \"Screen\"
    Identifier \"PrismdrakeDummyScreen\"
    Device \"PrismdrakeDummyDevice\"
    Monitor \"PrismdrakeDummyMonitor\"
    DefaultDepth 24
    SubSection \"Display\"
        Depth 24
        Modes \"1600x900\"
        Virtual 3200 1800
    EndSubSection
EndSection

Section \"ServerLayout\"
    Identifier \"PrismdrakeDummyLayout\"
    Screen \"PrismdrakeDummyScreen\"
EndSection
"""


class LiveXorgHarnessPort:
    """Fail-closed live adapter for the X11-owned subset of the PD2 contract."""

    _NOTIFICATION_STEPS = {
        "create_normal_notification": "create_normal",
        "create_replacement_notification": "replace_normal",
        "invoke_live_notification_action": "invoke_live_action",
        "open_notification_center": "open_center",
        "toggle_do_not_disturb": "enable_do_not_disturb",
        "verify_history_without_transient": "create_suppressed_normal",
        "verify_critical_bypass_policy": "create_critical_bypass",
        "clear_notification_history": "clear_history",
    }
    _SETTINGS_STEPS = {
        "open_quick_settings": "open_quick_settings",
        "switch_lustre_to_forge": "switch_lustre_to_forge",
        "verify_complete_settings_generation": "verify_complete_settings_generation",
        "switch_back_preserving_accessibility": "switch_back_preserving_accessibility",
    }

    _SUPPORTED_STEPS = {
        ("installed_workflow", "start_isolated_two_output_x11"),
        ("installed_workflow", "start_installed_prismdrake_session"),
        ("installed_workflow", "verify_component_readiness"),
        ("installed_workflow", "verify_desktop_wallpaper_both_outputs"),
        ("installed_workflow", "verify_panel_policy_both_outputs"),
        ("installed_workflow", "invoke_launcher_through_shellctl"),
        ("installed_workflow", "search_controlled_application"),
        ("installed_workflow", "pin_controlled_application"),
        ("installed_workflow", "launch_controlled_application"),
        ("installed_workflow", "launch_second_window"),
        ("installed_workflow", "verify_task_grouping"),
        ("installed_workflow", "activate_one_window"),
        ("installed_workflow", "minimize_one_window"),
        ("installed_workflow", "reactivate_one_window"),
        ("installed_workflow", "close_both_windows_through_panel"),
        ("installed_workflow", "verify_authoritative_task_removal"),
        ("installed_workflow", "create_normal_notification"),
        ("installed_workflow", "create_replacement_notification"),
        ("installed_workflow", "invoke_live_notification_action"),
        ("installed_workflow", "open_notification_center"),
        ("installed_workflow", "toggle_do_not_disturb"),
        ("installed_workflow", "verify_history_without_transient"),
        ("installed_workflow", "verify_critical_bypass_policy"),
        ("installed_workflow", "clear_notification_history"),
        ("installed_workflow", "open_quick_settings"),
        ("installed_workflow", "switch_lustre_to_forge"),
        ("installed_workflow", "verify_complete_settings_generation"),
        ("installed_workflow", "switch_back_preserving_accessibility"),
        ("failure_replay", "two_same_scale_outputs_at_startup"),
        ("failure_replay", "add_second_output_after_startup"),
        ("failure_replay", "remove_primary_output"),
        ("failure_replay", "select_new_primary_output"),
        ("failure_replay", "change_output_geometry"),
        ("failure_replay", "restart_settingsd"),
        ("failure_replay", "restart_notifyd"),
        ("failure_replay", "restart_shell"),
        ("failure_replay", "replace_or_restart_window_manager"),
        ("failure_replay", "run_without_compositor"),
        ("failure_replay", "run_with_missing_blur_capability"),
        ("failure_replay", "start_with_invalid_wallpaper"),
        ("failure_replay", "start_with_malformed_pin_state"),
        ("failure_replay", "lose_x_server"),
        ("failure_replay", "enter_and_exit_safe_mode"),
    }

    def __init__(
        self,
        config: LiveXorgConfig,
        operations: Operations | None = None,
        accessibility: AccessibilityOperations | None = None,
        notification_epoch_factory: NotificationEpochFactory | None = None,
        settings_epoch_factory: SettingsEpochFactory | None = None,
    ) -> None:
        self._config = config
        self._operations = operations or SystemOperations()
        self._accessibility = accessibility or SystemAccessibilityOperations()
        self._notification_epoch_factory = (
            notification_epoch_factory or _create_notification_epoch
        )
        self._settings_epoch_factory = settings_epoch_factory or _create_settings_epoch
        self._runtime: Path | None = None
        self._display: str | None = None
        self._xorg: ChildProcess | None = None
        self._openbox: ChildProcess | None = None
        self._bus: ChildProcess | None = None
        self._session: ChildProcess | None = None
        self._bus_process_identity: ProcessIdentity | None = None
        self._bus_socket_identity: tuple[int, int] | None = None
        self._desktop_surfaces: dict[int, WindowGeometry] | None = None
        self._surface_shell_identity: ProcessIdentity | None = None
        self._controlled_windows: dict[str, int] = {}
        self._controlled_processes: dict[str, ProcessIdentity] = {}
        self._notification_epoch: _NotificationEpoch | None = None
        self._settings_epoch: _SettingsEpoch | None = None

    def installed_executable_hashes(self) -> Mapping[str, str]:
        if tuple(self._config.installed_executables.keys()) != EXECUTABLE_NAMES:
            raise HarnessError("installed_executable_set_invalid")
        return {
            name: self._hash_installed_executable(
                self._config.installed_executables[name], name
            )
            for name in EXECUTABLE_NAMES
        }

    def run_step(self, phase: str, identifier: str) -> Observation:
        if (phase, identifier) not in self._SUPPORTED_STEPS:
            raise HarnessError("live_step_not_implemented")
        if phase == "failure_replay":
            # Each replay scenario begins from a fresh isolated X11 epoch.  The
            # contract invokes scenarios consecutively and cleans only once at
            # the phase boundary, so carrying one scenario's topology into the
            # next would make the observation ambiguous.
            self.cleanup("failure_scenario_reset")
        if identifier == "start_isolated_two_output_x11":
            self._start_environment(two_outputs=True)
            self._require_topology(two_outputs=True)
        elif identifier == "start_installed_prismdrake_session":
            self._start_session()
        elif identifier == "verify_component_readiness":
            self._wait_for_session_readiness()
            self.capture_process_ownership()
        elif identifier == "verify_desktop_wallpaper_both_outputs":
            self._require_desktop_wallpaper_surfaces()
        elif identifier == "verify_panel_policy_both_outputs":
            self._require_panel_surfaces()
        elif identifier == "invoke_launcher_through_shellctl":
            self._invoke_shellctl("launcher", "toggle")
            self._wait_accessible(
                ControlSelector("Search applications", "editable_text", require_focused=True),
                "launcher_search_focus_timeout",
            )
        elif identifier == "search_controlled_application":
            self._xdotool("type", "--clearmodifiers", "--delay", "0", CONTROLLED_APPLICATION_NAME)
            self._wait_accessible(
                ControlSelector(CONTROLLED_APPLICATION_NAME, "push_button"),
                "controlled_launcher_result_timeout",
            )
        elif identifier == "pin_controlled_application":
            self._xdotool("key", "Down")
            self._xdotool("key", "shift+F10")
            self._invoke_accessible(ControlSelector("Pin to panel", "menu_item", "Press"))
            self._xdotool("key", "shift+F10")
            self._wait_accessible(
                ControlSelector("Unpin from panel", "menu_item"),
                "controlled_pin_publication_timeout",
            )
            self._xdotool("key", "Escape")
        elif identifier == "launch_controlled_application":
            self._invoke_accessible(
                ControlSelector(CONTROLLED_APPLICATION_NAME, "push_button", "Press")
            )
            self._controlled_windows[CONTROLLED_WINDOW_ONE] = self._wait_controlled_window(
                CONTROLLED_WINDOW_ONE
            )
            self._record_controlled_process(CONTROLLED_WINDOW_ONE)
        elif identifier == "launch_second_window":
            self._invoke_shellctl("launcher", "toggle")
            self._wait_accessible(
                ControlSelector(CONTROLLED_APPLICATION_NAME, "push_button"),
                "controlled_launcher_result_timeout",
            )
            self._xdotool("key", "Down")
            self._xdotool("key", "shift+F10")
            self._invoke_accessible(ControlSelector("Second window", "menu_item", "Press"))
            self._controlled_windows[CONTROLLED_WINDOW_TWO] = self._wait_controlled_window(
                CONTROLLED_WINDOW_TWO
            )
            self._record_controlled_process(CONTROLLED_WINDOW_TWO)
            if len(set(self._controlled_windows.values())) != 2:
                raise HarnessError("controlled_window_identity_collision")
        elif identifier == "verify_task_grouping":
            self._invoke_accessible(
                ControlSelector(CONTROLLED_APPLICATION_NAME, "push_button", "Press")
            )
            for title in (CONTROLLED_WINDOW_ONE, CONTROLLED_WINDOW_TWO):
                self._wait_accessible(
                    ControlSelector(f"Activate {title}", "menu_item"),
                    "controlled_task_group_timeout",
                )
        elif identifier == "activate_one_window":
            self._invoke_accessible(
                ControlSelector(f"Activate {CONTROLLED_WINDOW_ONE}", "menu_item", "Press")
            )
            self._wait_window_active(CONTROLLED_WINDOW_ONE, minimized=False)
        elif identifier == "minimize_one_window":
            self._open_controlled_group()
            self._invoke_accessible(
                ControlSelector(f"Minimize {CONTROLLED_WINDOW_ONE}", "menu_item", "Press")
            )
            self._wait_window_active(CONTROLLED_WINDOW_ONE, minimized=True)
        elif identifier == "reactivate_one_window":
            self._open_controlled_group()
            self._invoke_accessible(
                ControlSelector(f"Activate {CONTROLLED_WINDOW_ONE}", "menu_item", "Press")
            )
            self._wait_window_active(CONTROLLED_WINDOW_ONE, minimized=False)
        elif identifier == "close_both_windows_through_panel":
            self._open_controlled_group()
            self._invoke_accessible(
                ControlSelector(f"Close {CONTROLLED_WINDOW_TWO}", "menu_item", "Press")
            )
            self._wait_window_absent(CONTROLLED_WINDOW_TWO)
            self._invoke_shellctl("panel", "focus")
            self._wait_accessible(
                ControlSelector("Open applications", "push_button", require_focused=True),
                "panel_focus_timeout",
            )
            self._xdotool("key", "Tab")
            self._wait_accessible(
                ControlSelector(
                    CONTROLLED_APPLICATION_NAME, "push_button", require_focused=True
                ),
                "controlled_task_focus_timeout",
            )
            self._xdotool("key", "shift+F10")
            self._invoke_accessible(
                ControlSelector(
                    f"Close {CONTROLLED_APPLICATION_NAME}", "menu_item", "Press"
                )
            )
            self._wait_window_absent(CONTROLLED_WINDOW_ONE)
        elif identifier == "verify_authoritative_task_removal":
            for title in (CONTROLLED_WINDOW_ONE, CONTROLLED_WINDOW_TWO):
                if self._controlled_window_ids(title):
                    raise HarnessError("controlled_task_removal_timeout")
            if not self._reject_stale_accessible(
                ControlSelector(
                    f"Close {CONTROLLED_APPLICATION_NAME}", "menu_item", "Press"
                )
            ):
                raise HarnessError("stale_task_action_accepted")
            self._controlled_windows.clear()
        elif identifier in self._NOTIFICATION_STEPS:
            self._run_notification_step(identifier)
        elif identifier in self._SETTINGS_STEPS:
            self._run_settings_step(identifier)
        elif identifier == "two_same_scale_outputs_at_startup":
            self._start_environment(two_outputs=True)
            self._require_topology(two_outputs=True)
        elif identifier == "add_second_output_after_startup":
            self._start_environment(two_outputs=False)
            self._require_topology(two_outputs=False)
            self._enable_secondary()
            self._require_topology(two_outputs=True)
        elif identifier == "remove_primary_output":
            self._start_environment(two_outputs=True)
            self._require_topology(two_outputs=True)
            self._xrandr("--output", PRIMARY_OUTPUT, "--off")
            self._require_only_secondary()
        elif identifier == "select_new_primary_output":
            self._start_environment(two_outputs=True)
            self._xrandr("--output", SECONDARY_OUTPUT, "--primary")
            self._require_topology(two_outputs=True, primary=SECONDARY_OUTPUT)
        elif identifier == "change_output_geometry":
            self._start_environment(two_outputs=True)
            self._xrandr("--output", SECONDARY_OUTPUT, "--pos", "0x900")
            self._require_topology(two_outputs=True, secondary_position=(0, 900))
        elif identifier == "restart_settingsd":
            self._prepare_restart_state()
            self._restart_component("prismdrake-settingsd")
        elif identifier == "restart_notifyd":
            self._prepare_restart_state()
            self._restart_component("prismdrake-notifyd")
            self._invoke_shellctl("notifications", "open")
            self._wait_accessible(
                ControlSelector("Notification center", "panel"),
                "notification_owner_reacquisition_timeout",
            )
        elif identifier == "restart_shell":
            self._prepare_restart_state()
            notifyd = self._component_identity("prismdrake-notifyd")
            self._restart_component("prismdrake-shell")
            if self._component_identity("prismdrake-notifyd") != notifyd:
                raise HarnessError("notifyd_replaced_during_shell_restart")
            self._wait_accessible(
                ControlSelector(CONTROLLED_APPLICATION_NAME, "push_button"),
                "pin_state_reacquisition_timeout",
            )
        elif identifier == "replace_or_restart_window_manager":
            self._prepare_restart_state()
            components = self._component_identities()
            windows = dict(self._controlled_windows)
            if self._openbox is None or not self._operations.stop(
                self._openbox, self._config.stop_timeout
            ):
                raise HarnessError("window_manager_stop_failed")
            previous = self._openbox
            self._openbox = None
            self._start_openbox()
            if self._openbox is previous:
                raise HarnessError("window_manager_replacement_invalid")
            self._require_topology(two_outputs=True)
            self._require_controlled_windows_unchanged(windows)
            self._require_panel_surfaces()
            if self._component_identities() != components:
                raise HarnessError("prismdrake_child_changed_during_window_manager_restart")
        elif identifier == "run_without_compositor":
            self._prepare_restart_state()
            self._require_fresh_display_fallback_surfaces()
        elif identifier == "run_with_missing_blur_capability":
            self._prepare_restart_state()
            self._require_fresh_display_fallback_surfaces()
            self._require_configuration_value(
                b'blur_quality = "balanced"\n', "blur_preference_observation_invalid"
            )
        elif identifier == "start_with_invalid_wallpaper":
            configuration = _session_config().replace(
                'wallpaper_source = ""',
                'wallpaper_source = "wallpapers/missing-controlled-image.png"',
            )
            self._start_environment(two_outputs=True)
            self._start_session(configuration)
            self._wait_for_session_readiness()
            self._require_configuration_value(
                b'wallpaper_source = "wallpapers/missing-controlled-image.png"\n',
                "invalid_wallpaper_preference_observation_invalid",
            )
            self._require_desktop_wallpaper_surfaces()
            self._require_panel_surfaces()
            self._component_identities()
        elif identifier == "start_with_malformed_pin_state":
            self._start_environment(two_outputs=True)
            malformed = self._write_malformed_pin_state()
            self._start_session()
            self._wait_for_session_readiness()
            self._require_desktop_wallpaper_surfaces()
            self._require_panel_surfaces()
            if self._read_private_regular(malformed, 1024) != b"{ malformed\n":
                raise HarnessError("malformed_pin_state_not_retained")
            for step in (
                "invoke_launcher_through_shellctl",
                "search_controlled_application",
                "pin_controlled_application",
            ):
                self.run_step("installed_workflow", step)
            self._require_recovered_pin_state(malformed)
        elif identifier == "lose_x_server":
            self._prepare_restart_state()
            if self._xorg is None or not self._operations.stop(
                self._xorg, self._config.stop_timeout
            ):
                raise HarnessError("xorg_stop_failed")
            self._xorg = None
            result = self._run_x11((str(self._config.xrandr), "--query"))
            if result.returncode == 0:
                raise HarnessError("x_server_loss_unobserved")
            self.cleanup("x_server_loss_fatal_cleanup")
        elif identifier == "enter_and_exit_safe_mode":
            self._start_environment(two_outputs=True)
            self._start_session()
            self._wait_for_session_readiness()
            self._exhaust_shell_restart_budget()
            self._wait_for_safe_mode()
            self._require_desktop_wallpaper_surfaces(allow_safe_mode=True)
            self.cleanup("safe_mode_epoch")
            self._start_environment(two_outputs=True)
            self._start_session()
            self._wait_for_session_readiness()
            if self._session_marker_state() != "ready":
                raise HarnessError("fresh_normal_session_not_ready")
            self._require_desktop_wallpaper_surfaces()
        return Observation(True, True)

    def _run_notification_step(self, identifier: str) -> None:
        method_name = self._NOTIFICATION_STEPS.get(identifier)
        if method_name is None:
            raise HarnessError("live_notification_step_not_reviewed")
        workflow = self._notification_workflow_epoch().workflow
        try:
            operation = getattr(workflow, method_name)
            result = operation()
            validate_workflow_result(result)
        except Exception:
            raise HarnessError("live_notification_workflow_failed") from None
        if not isinstance(result, WorkflowResult):
            raise HarnessError("live_notification_workflow_failed")

    def _notification_workflow_epoch(self) -> _NotificationEpoch:
        if self._notification_epoch is not None:
            return self._notification_epoch
        environment = dict(self.probe_environment())
        socket_identity = self._bus_socket_identity
        if socket_identity is None:
            raise HarnessError("private_session_bus_not_started")
        endpoint = PrivateBusEndpoint(
            self._private_bus_address(), socket_identity[0], socket_identity[1]
        )
        surface = LiveNotificationSurfaceAdapter(
            self._accessibility,
            self._shell_identity,
            self.probe_environment,
            self._operations.monotonic,
            self._operations.pause,
        )
        try:
            epoch = self._notification_epoch_factory(environment, endpoint, surface)
        except Exception:
            raise HarnessError("live_notification_epoch_initialization_failed") from None
        if not isinstance(epoch, _NotificationEpoch):
            raise HarnessError("live_notification_epoch_initialization_failed")
        self._notification_epoch = epoch
        return epoch

    def _run_settings_step(self, identifier: str) -> None:
        if identifier not in self._SETTINGS_STEPS:
            raise HarnessError("live_settings_step_not_reviewed")
        workflow = self._settings_workflow_epoch().workflow
        try:
            result = workflow.run_step(identifier)
        except Exception:
            raise HarnessError("live_settings_workflow_failed") from None
        if (
            not isinstance(result, SettingsWorkflowResult)
            or result.evidence().get("status") != "passed"
        ):
            raise HarnessError("live_settings_workflow_failed")

    def _settings_workflow_epoch(self) -> _SettingsEpoch:
        if self._settings_epoch is not None:
            return self._settings_epoch
        environment = dict(self.probe_environment())
        socket_identity = self._bus_socket_identity
        if socket_identity is None:
            raise HarnessError("private_session_bus_not_started")
        endpoint = PrivateBusEndpoint(
            self._private_bus_address(), socket_identity[0], socket_identity[1]
        )
        accessibility = LiveSettingsAccessibilityAdapter(
            self._accessibility,
            self._shell_identity,
            self.probe_environment,
            self._operations.monotonic,
            self._operations.pause,
            self._config.readiness_timeout,
        )
        surfaces = LiveSettingsSurfaceAdapter(
            SETTINGS_WORKFLOW_CONFIG,
            self._settings_surface_window,
            self._capture_settings_fingerprint,
            self._operations.pause,
        )
        try:
            epoch = self._settings_epoch_factory(
                environment, endpoint, accessibility, surfaces
            )
        except Exception:
            raise HarnessError("live_settings_epoch_initialization_failed") from None
        if not isinstance(epoch, _SettingsEpoch):
            raise HarnessError("live_settings_epoch_initialization_failed")
        self._settings_epoch = epoch
        return epoch

    def _capture_settings_fingerprint(
        self, window: int, points: Sequence[tuple[int, int]]
    ) -> bytes:
        if self._config.xwd is None or self._runtime is None or self._display is None:
            raise HarnessError("live_settings_xwd_unavailable")
        return capture_window_fingerprint(
            self._config.xwd,
            window,
            self._runtime,
            {"DISPLAY": self._display},
            sample_points=points,
        )

    def _settings_surface_window(self, surface: str) -> int:
        shell = self._shell_identity()
        if surface in {"panel_primary", "panel_secondary"}:
            self._require_panel_surfaces()
            panels = self._observe_surface_windows(b"_NET_WM_WINDOW_TYPE_DOCK", shell)
            x = 0 if surface == "panel_primary" else OUTPUT_WIDTH
            expected = WindowGeometry(
                x, OUTPUT_HEIGHT - PANEL_HEIGHT, OUTPUT_WIDTH, PANEL_HEIGHT
            )
            matches = [window for window, geometry in panels.items() if geometry == expected]
            if len(matches) != 1:
                raise HarnessError("live_settings_surface_window_invalid")
            return matches[0]
        if surface != "quick_settings":
            raise HarnessError("live_settings_surface_not_reviewed")
        windows = self._visible_named_window_ids(QUICK_SETTINGS_WINDOW_TITLE)
        if len(windows) != 1:
            raise HarnessError("live_settings_surface_window_invalid")
        window = windows[0]
        if self._window_cardinals(window, b"_NET_WM_PID", 1) != (shell.pid,):
            raise HarnessError("live_settings_surface_owner_invalid")
        geometry_result = self._run_x11(
            (str(self._config.xwininfo), "-id", f"0x{window:x}", "-stats")
        )
        if geometry_result.returncode != 0:
            raise HarnessError("live_settings_surface_window_invalid")
        geometry = parse_window_geometry(geometry_result.stdout)
        if (
            geometry.width != 380
            or not 100 <= geometry.height <= OUTPUT_HEIGHT - PANEL_HEIGHT
            or geometry.x != OUTPUT_WIDTH - geometry.width
            or geometry.y != OUTPUT_HEIGHT - PANEL_HEIGHT - geometry.height
        ):
            raise HarnessError("live_settings_surface_window_invalid")
        return window

    def _visible_named_window_ids(self, title: str) -> tuple[int, ...]:
        if title != QUICK_SETTINGS_WINDOW_TITLE or self._config.xdotool is None:
            raise HarnessError("live_settings_surface_query_invalid")
        result = self._operations.run(
            (
                str(self._config.xdotool),
                "search",
                "--onlyvisible",
                "--name",
                f"^{title}$",
            ),
            self.probe_environment(),
            3.0,
        )
        if result.returncode == 1 and not result.stdout and not result.stderr:
            return ()
        if result.returncode != 0 or result.stderr or len(result.stdout) > 1024:
            raise HarnessError("live_settings_surface_query_invalid")
        identifiers: list[int] = []
        for line in result.stdout.splitlines():
            if re.fullmatch(rb"[1-9][0-9]{0,19}", line) is None:
                raise HarnessError("live_settings_surface_query_invalid")
            identifier = int(line)
            if identifier > 0xFFFFFFFF or identifier in identifiers:
                raise HarnessError("live_settings_surface_query_invalid")
            identifiers.append(identifier)
        if len(identifiers) > 1:
            raise HarnessError("live_settings_surface_query_invalid")
        return tuple(identifiers)

    def capture_process_ownership(self) -> Mapping[str, object]:
        if (
            self._xorg is None
            or self._openbox is None
            or self._bus is None
            or self._session is None
        ):
            raise HarnessError("live_process_ownership_unavailable")
        session_executable = self._config.installed_executables.get(
            "prismdrake-session"
        )
        if session_executable is None:
            raise HarnessError("live_process_ownership_invalid")
        xorg = self._operations.identify(self._xorg, self._config.xorg)
        openbox = self._operations.identify(self._openbox, self._config.openbox)
        bus = self._attest_private_bus()
        session = self._operations.identify(self._session, session_executable)
        if xorg is None or openbox is None or session is None:
            raise HarnessError("live_process_ownership_invalid")
        identities = (xorg, openbox, bus, session)
        if len(set(identities)) != len(identities):
            raise HarnessError("live_process_ownership_invalid")
        return dict(
            zip(
                (
                    "xorg_dummy_server",
                    "openbox_window_manager",
                    "isolated_session_bus",
                    "prismdrake_session",
                ),
                identities,
                strict=True,
            )
        )

    def cleanup(self, phase: str) -> Observation:
        del phase
        clean = True
        if self._settings_epoch is not None:
            clean = self._settings_epoch.close() and clean
            self._settings_epoch = None
        if self._notification_epoch is not None:
            clean = self._notification_epoch.close() and clean
            self._notification_epoch = None
        fixture = self._config.controlled_window_fixture
        if fixture is not None:
            for identity in self._controlled_processes.values():
                clean = (
                    self._operations.stop_identity(
                        identity, fixture, self._config.stop_timeout
                    )
                    and clean
                )
        for attribute in ("_session", "_bus", "_openbox", "_xorg"):
            child = getattr(self, attribute)
            if child is not None:
                clean = self._operations.stop(child, self._config.stop_timeout) and clean
                setattr(self, attribute, None)
        self._display = None
        self._bus_process_identity = None
        self._bus_socket_identity = None
        self._desktop_surfaces = None
        self._surface_shell_identity = None
        self._controlled_windows.clear()
        self._controlled_processes.clear()
        if self._runtime is not None:
            try:
                shutil.rmtree(self._runtime)
            except OSError:
                clean = False
            self._runtime = None
        if not clean:
            raise HarnessError("live_cleanup_failed")
        return Observation(True, True)

    @staticmethod
    def _hash_installed_executable(path: Path, expected_name: str) -> str:
        try:
            metadata = path.lstat()
        except OSError as error:
            raise HarnessError("installed_executable_unavailable") from error
        if (
            not path.is_absolute()
            or path.name != expected_name
            or stat.S_ISLNK(metadata.st_mode)
            or not stat.S_ISREG(metadata.st_mode)
            or metadata.st_size <= 0
            or metadata.st_size > MAX_EXECUTABLE_BYTES
            or metadata.st_mode & 0o111 == 0
        ):
            raise HarnessError("installed_executable_invalid")
        digest = hashlib.sha256()
        try:
            with path.open("rb") as executable:
                while block := executable.read(1024 * 1024):
                    digest.update(block)
        except OSError as error:
            raise HarnessError("installed_executable_read_failed") from error
        return digest.hexdigest()

    def _start_environment(self, *, two_outputs: bool) -> None:
        if self._xorg is not None or self._openbox is not None:
            raise HarnessError("live_environment_already_started")
        self._runtime = Path(tempfile.mkdtemp(prefix="prismdrake-pd2-"))
        self._runtime.chmod(0o700)
        for directory in ("home", "config", "cache", "data", "state"):
            path = self._runtime / directory
            path.mkdir(mode=0o700)
        self._write_controlled_desktop_entry()
        config_path = self._runtime / "xorg.conf"
        config_path.write_text(_xorg_config(), encoding="ascii")
        config_path.chmod(0o600)

        for number in range(self._config.first_display, self._config.last_display + 1):
            if not self._operations.display_available(number):
                continue
            display = f":{number}"
            argv = (
                str(self._config.xorg),
                display,
                "-config",
                str(config_path),
                "-nolisten",
                "tcp",
                "-noreset",
                "-logfile",
                str(self._runtime / "Xorg.log"),
            )
            child = self._operations.spawn(argv, self._base_environment())
            self._display = display
            self._xorg = child
            if self._wait_for_xorg():
                # Do not accept a query raced through an unrelated server while
                # this exact Xorg child was still failing its own startup.
                self._operations.pause(0.05)
                if child.poll() is None:
                    break
            self._operations.stop(child, self._config.stop_timeout)
            self._xorg = None
            self._display = None
        if self._xorg is None:
            raise HarnessError("xorg_readiness_failed")
        self._configure_primary()
        if two_outputs:
            self._enable_secondary()
        else:
            self._xrandr("--output", SECONDARY_OUTPUT, "--off")
        self._start_openbox()

    def _base_environment(self) -> dict[str, str]:
        env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LC_ALL": "C"}
        if self._runtime is not None:
            env.update(
                {
                    "HOME": str(self._runtime / "home"),
                    "XDG_CACHE_HOME": str(self._runtime / "cache"),
                    "XDG_CONFIG_HOME": str(self._runtime / "config"),
                    "XDG_DATA_DIRS": "/usr/local/share:/usr/share",
                    "XDG_DATA_HOME": str(self._runtime / "data"),
                    "XDG_RUNTIME_DIR": str(self._runtime),
                    "XDG_STATE_HOME": str(self._runtime / "state"),
                }
            )
        return env

    def _write_controlled_desktop_entry(self) -> None:
        fixture = self._config.controlled_window_fixture
        if fixture is None:
            return
        try:
            metadata = fixture.lstat()
        except OSError as error:
            raise HarnessError("controlled_fixture_unavailable") from error
        native = os.fspath(fixture)
        if (
            not fixture.is_absolute()
            or stat.S_ISLNK(metadata.st_mode)
            or not stat.S_ISREG(metadata.st_mode)
            or metadata.st_mode & 0o111 == 0
            or any(character.isspace() for character in native)
        ):
            raise HarnessError("controlled_fixture_invalid")
        if self._runtime is None:
            raise HarnessError("live_environment_not_started")
        applications = self._runtime / "data" / "applications"
        try:
            applications.mkdir(mode=0o700)
            desktop = applications / CONTROLLED_DESKTOP_FILE
            desktop.write_text(
                "[Desktop Entry]\n"
                "Type=Application\n"
                f"Name={CONTROLLED_APPLICATION_NAME}\n"
                f"Exec={native} --instance one\n"
                "StartupWMClass=PrismdrakeDemo\n"
                "Actions=Second;\n"
                "\n"
                "[Desktop Action Second]\n"
                "Name=Second window\n"
                f"Exec={native} --instance two\n",
                encoding="ascii",
            )
            desktop.chmod(0o600)
        except OSError as error:
            raise HarnessError("controlled_desktop_entry_failed") from error

    def _shell_identity(self) -> ProcessIdentity:
        shell = self._require_surface_shell()
        if self._surface_shell_identity is not None and shell != self._surface_shell_identity:
            raise HarnessError("surface_shell_identity_changed")
        return shell

    def _component_identity(self, name: str) -> ProcessIdentity:
        if name not in {
            "prismdrake-settingsd",
            "prismdrake-notifyd",
            "prismdrake-shell",
        }:
            raise HarnessError("component_identity_role_invalid")
        if self._session is None or self._session.poll() is not None:
            raise HarnessError("prismdrake_session_not_started")
        executable = self._config.installed_executables.get(name)
        if executable is None:
            raise HarnessError("component_identity_unavailable")
        identities = self._operations.descendants(self._session, executable)
        if len(identities) != 1:
            raise HarnessError("component_identity_invalid")
        return identities[0]

    def _component_identities(self) -> dict[str, ProcessIdentity]:
        return {
            name: self._component_identity(name)
            for name in (
                "prismdrake-settingsd",
                "prismdrake-notifyd",
                "prismdrake-shell",
            )
        }

    def _configuration_path(self) -> Path:
        if self._runtime is None:
            raise HarnessError("live_environment_not_started")
        return self._runtime / "config" / "prismdrake" / "config.toml"

    @staticmethod
    def _read_private_regular(path: Path, maximum_bytes: int) -> bytes:
        if maximum_bytes <= 0:
            raise HarnessError("private_state_file_invalid")
        flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
        flags |= getattr(os, "O_NOFOLLOW", 0)
        try:
            descriptor = os.open(path, flags)
        except OSError as error:
            raise HarnessError("private_state_file_invalid") from error
        try:
            metadata = os.fstat(descriptor)
            if (
                not stat.S_ISREG(metadata.st_mode)
                or metadata.st_uid != os.geteuid()
                or stat.S_IMODE(metadata.st_mode) != 0o600
                or metadata.st_size < 0
                or metadata.st_size > maximum_bytes
            ):
                raise HarnessError("private_state_file_invalid")
            blocks: list[bytes] = []
            remaining = maximum_bytes + 1
            while remaining > 0:
                block = os.read(descriptor, remaining)
                if not block:
                    break
                blocks.append(block)
                remaining -= len(block)
            payload = b"".join(blocks)
            if len(payload) != metadata.st_size:
                raise HarnessError("private_state_file_invalid")
            return payload
        except OSError as error:
            raise HarnessError("private_state_file_invalid") from error
        finally:
            os.close(descriptor)

    def _require_configuration_value(self, value: bytes, error_name: str) -> None:
        payload = self._read_private_regular(self._configuration_path(), 64 * 1024)
        if payload.count(value) != 1:
            raise HarnessError(error_name)

    def _write_malformed_pin_state(self) -> Path:
        if self._runtime is None:
            raise HarnessError("live_environment_not_started")
        directory = self._runtime / "state" / "prismdrake"
        path = directory / "pins.json"
        try:
            directory.mkdir(mode=0o700)
            path.write_bytes(b"{ malformed\n")
            path.chmod(0o600)
        except OSError as error:
            raise HarnessError("malformed_pin_state_setup_failed") from error
        return path

    def _require_recovered_pin_state(self, path: Path) -> None:
        payload = self._read_private_regular(path, 64 * 1024)
        try:
            document = json.loads(payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise HarnessError("malformed_pin_state_recovery_invalid") from error
        if document != {
            "schema_version": 1,
            "pins": [CONTROLLED_DESKTOP_FILE],
        }:
            raise HarnessError("malformed_pin_state_recovery_invalid")

    def _require_fresh_display_fallback_surfaces(self) -> None:
        # This attests the harness's fresh-display process construction.  It
        # deliberately does not claim to observe the X compositing-manager
        # selection owner; that requires GetSelectionOwner, not a root property.
        ownership = self.capture_process_ownership()
        if set(ownership) != {
            "xorg_dummy_server",
            "openbox_window_manager",
            "isolated_session_bus",
            "prismdrake_session",
        }:
            raise HarnessError("fresh_display_process_ownership_invalid")
        self._component_identities()
        self._require_desktop_wallpaper_surfaces()
        self._require_panel_surfaces()
        self._require_controlled_windows_unchanged(self._controlled_windows)

    def _exhaust_shell_restart_budget(self) -> None:
        executable = self._config.installed_executables.get("prismdrake-shell")
        if executable is None:
            raise HarnessError("component_identity_unavailable")
        original_siblings = self._component_identities()
        previous = original_siblings["prismdrake-shell"]
        for crash in range(4):
            if not self._operations.stop_identity(
                previous, executable, self._config.stop_timeout
            ):
                raise HarnessError("safe_mode_crash_signal_failed")
            deadline = self._operations.monotonic() + self._config.readiness_timeout
            replacement: ProcessIdentity | None = None
            while self._operations.monotonic() < deadline:
                try:
                    candidate = self._component_identity("prismdrake-shell")
                except HarnessError as error:
                    if str(error) != "component_identity_invalid":
                        raise
                else:
                    if candidate != previous:
                        replacement = candidate
                        break
                self._operations.pause(0.05)
            if replacement is None:
                raise HarnessError("safe_mode_shell_restart_timeout")
            previous = replacement
            if crash < 3:
                current = self._component_identities()
                for sibling in ("prismdrake-settingsd", "prismdrake-notifyd"):
                    if current[sibling] != original_siblings[sibling]:
                        raise HarnessError("safe_mode_sibling_replaced_early")
                if self._session_marker_state() != "ready":
                    raise HarnessError("safe_mode_entered_before_budget")
        safe_components = self._component_identities()
        for sibling in ("prismdrake-settingsd", "prismdrake-notifyd"):
            if safe_components[sibling] == original_siblings[sibling]:
                raise HarnessError("safe_mode_component_epoch_not_replaced")
        self._surface_shell_identity = None

    def _wait_for_safe_mode(self) -> None:
        if self._session is None:
            raise HarnessError("prismdrake_session_not_started")
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            self._attest_private_bus()
            if self._session.poll() is not None:
                raise HarnessError("prismdrake_session_exited_before_safe_mode")
            if self._session_marker_state() == "safe_mode":
                self._component_identities()
                return
            self._operations.pause(0.05)
        raise HarnessError("prismdrake_session_safe_mode_timeout")

    def _prepare_restart_state(self) -> None:
        for identifier in (
            "start_isolated_two_output_x11",
            "start_installed_prismdrake_session",
            "verify_component_readiness",
            "verify_desktop_wallpaper_both_outputs",
            "verify_panel_policy_both_outputs",
            "invoke_launcher_through_shellctl",
            "search_controlled_application",
            "pin_controlled_application",
            "launch_controlled_application",
        ):
            self.run_step("installed_workflow", identifier)
        self._component_identities()

    def _restart_component(self, name: str) -> None:
        executable = self._config.installed_executables.get(name)
        if executable is None:
            raise HarnessError("component_identity_unavailable")
        before = self._component_identities()
        windows = dict(self._controlled_windows)
        old = before[name]
        if not self._operations.stop_identity(old, executable, self._config.stop_timeout):
            raise HarnessError("component_restart_signal_failed")
        if name == "prismdrake-shell":
            self._surface_shell_identity = None

        deadline = self._operations.monotonic() + self._config.readiness_timeout
        replacement: ProcessIdentity | None = None
        while self._operations.monotonic() < deadline:
            try:
                candidate = self._component_identity(name)
            except HarnessError as error:
                if str(error) != "component_identity_invalid":
                    raise
            else:
                if candidate != old:
                    replacement = candidate
                    break
            self._operations.pause(0.05)
        if replacement is None:
            raise HarnessError("component_restart_timeout")

        after = self._component_identities()
        for role, identity in before.items():
            if role != name and after[role] != identity:
                raise HarnessError("unrelated_component_replaced_during_restart")
        self._require_controlled_windows_unchanged(windows)
        if name == "prismdrake-shell":
            self._require_desktop_wallpaper_surfaces()
            self._require_panel_surfaces()
        else:
            if self._component_identity("prismdrake-shell") != before["prismdrake-shell"]:
                raise HarnessError("shell_replaced_during_service_restart")
            self._wait_accessible(
                ControlSelector(CONTROLLED_APPLICATION_NAME, "push_button"),
                "shell_owner_reacquisition_timeout",
            )

    def _require_controlled_windows_unchanged(
        self, expected: Mapping[str, int]
    ) -> None:
        if not expected:
            raise HarnessError("controlled_window_identity_missing")
        for title, identifier in expected.items():
            if self._controlled_window_ids(title) != (identifier,):
                raise HarnessError("controlled_window_changed_during_restart")

    def _wait_accessible(self, selector: ControlSelector, timeout_error: str) -> None:
        shell = self._shell_identity()
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            if self._accessibility.observe(
                shell.pid, selector, self.probe_environment()
            ):
                return
            self._operations.pause(0.05)
        raise HarnessError(timeout_error)

    def _invoke_accessible(self, selector: ControlSelector) -> None:
        self._wait_accessible(selector, "controlled_accessible_action_timeout")
        shell = self._shell_identity()
        if not self._accessibility.invoke(
            shell.pid, selector, self.probe_environment()
        ):
            raise HarnessError("controlled_accessible_action_rejected")

    def _reject_stale_accessible(self, selector: ControlSelector) -> bool:
        shell = self._shell_identity()
        return self._accessibility.reject_stale_action(
            shell.pid, selector, self.probe_environment()
        )

    def _invoke_shellctl(self, group: str, action: str) -> None:
        executable = self._config.installed_executables.get("prismdrake-shellctl")
        if executable is None:
            raise HarnessError("shellctl_unavailable")
        result = self._operations.run(
            (str(executable), group, action), self.probe_environment(), 3.0
        )
        if result.returncode != 0:
            raise HarnessError("shellctl_request_failed")

    def _xdotool(self, *arguments: str) -> CommandResult:
        executable = self._config.xdotool
        if executable is None:
            raise HarnessError("xdotool_unavailable")
        result = self._operations.run(
            (str(executable), *arguments), self.probe_environment(), 3.0
        )
        if result.returncode != 0:
            raise HarnessError("xdotool_request_failed")
        return result

    def _controlled_window_ids(self, title: str) -> tuple[int, ...]:
        if title not in {CONTROLLED_WINDOW_ONE, CONTROLLED_WINDOW_TWO}:
            raise HarnessError("controlled_window_selector_invalid")
        executable = self._config.xdotool
        if executable is None:
            raise HarnessError("xdotool_unavailable")
        result = self._operations.run(
            (str(executable), "search", "--onlyvisible", "--name", f"^{title}$"),
            self.probe_environment(),
            3.0,
        )
        if result.returncode == 1 and not result.stdout and not result.stderr:
            return ()
        if result.returncode != 0 or result.stderr or len(result.stdout) > 1024:
            raise HarnessError("controlled_window_query_failed")
        identifiers: list[int] = []
        for line in result.stdout.splitlines():
            if re.fullmatch(rb"[1-9][0-9]{0,19}", line) is None:
                raise HarnessError("controlled_window_query_invalid")
            identifier = int(line)
            if identifier > 0xFFFFFFFF or identifier in identifiers:
                raise HarnessError("controlled_window_query_invalid")
            identifiers.append(identifier)
        if len(identifiers) > 2:
            raise HarnessError("controlled_window_query_invalid")
        return tuple(identifiers)

    def _wait_controlled_window(self, title: str) -> int:
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            identifiers = self._controlled_window_ids(title)
            if len(identifiers) == 1:
                return identifiers[0]
            if len(identifiers) > 1:
                break
            self._operations.pause(0.05)
        raise HarnessError("controlled_window_timeout")

    def _record_controlled_process(self, title: str) -> None:
        fixture = self._config.controlled_window_fixture
        window = self._controlled_windows.get(title)
        if fixture is None or window is None:
            return
        owner = self._window_cardinals(window, b"_NET_WM_PID", 1)[0]
        identity = self._operations.identify_pid(owner, fixture)
        if identity is None or identity in self._controlled_processes.values():
            raise HarnessError("controlled_process_identity_invalid")
        self._controlled_processes[title] = identity

    def _wait_window_absent(self, title: str) -> None:
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            if not self._controlled_window_ids(title):
                self._controlled_windows.pop(title, None)
                return
            self._operations.pause(0.05)
        raise HarnessError("controlled_window_close_timeout")

    def _window_state_atoms(self, window: int) -> frozenset[bytes]:
        payload = self._window_property(window, b"_NET_WM_STATE")
        if payload == b"_NET_WM_STATE:  not found.\n":
            return frozenset()
        match = re.fullmatch(
            rb"_NET_WM_STATE\(ATOM\) = ([A-Z0-9_, ]{1,1024})\n", payload
        )
        if match is None:
            raise HarnessError("controlled_window_state_invalid")
        atoms = tuple(part.strip() for part in match.group(1).split(b","))
        if not atoms or any(not atom.startswith(b"_NET_WM_STATE_") for atom in atoms):
            raise HarnessError("controlled_window_state_invalid")
        return frozenset(atoms)

    def _active_window(self) -> int | None:
        result = self._run_x11((str(self._config.xprop), "-root", "_NET_ACTIVE_WINDOW"))
        if result.returncode != 0:
            raise HarnessError("controlled_active_window_query_failed")
        if result.stdout == b"_NET_ACTIVE_WINDOW(WINDOW): window id # 0x0\n":
            return None
        identifiers = parse_window_ids(result.stdout, b"_NET_ACTIVE_WINDOW")
        if len(identifiers) != 1:
            raise HarnessError("controlled_active_window_query_failed")
        return identifiers[0]

    def _wait_window_active(self, title: str, *, minimized: bool) -> None:
        window = self._controlled_windows.get(title)
        if window is None:
            raise HarnessError("controlled_window_identity_missing")
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            hidden = b"_NET_WM_STATE_HIDDEN" in self._window_state_atoms(window)
            active = self._active_window() == window
            if hidden == minimized and (minimized or active):
                return
            self._operations.pause(0.05)
        raise HarnessError(
            "controlled_window_minimization_timeout"
            if minimized
            else "controlled_window_activation_timeout"
        )

    def _open_controlled_group(self) -> None:
        self._invoke_accessible(
            ControlSelector(CONTROLLED_APPLICATION_NAME, "push_button", "Press")
        )
        self._wait_accessible(
            ControlSelector(f"Activate {CONTROLLED_WINDOW_ONE}", "menu_item"),
            "controlled_task_group_timeout",
        )

    def _x11_environment(self) -> dict[str, str]:
        if self._display is None:
            raise HarnessError("xorg_not_started")
        env = self._base_environment()
        env["DISPLAY"] = self._display
        return env

    def probe_environment(self) -> Mapping[str, str]:
        """Return the private live epoch environment for bounded observers."""

        if self._session is None or self._session.poll() is not None:
            raise HarnessError("prismdrake_session_not_started")
        return self._private_bus_environment()

    def _run_x11(self, argv: Sequence[str]) -> CommandResult:
        return self._operations.run(argv, self._x11_environment(), 3.0)

    def _start_session(self, configuration_document: str | None = None) -> None:
        if self._runtime is None or self._xorg is None or self._openbox is None:
            raise HarnessError("live_environment_not_started")
        if self._session is not None or self._bus is not None:
            raise HarnessError("live_session_already_started")
        if tuple(self._config.installed_executables.keys()) != EXECUTABLE_NAMES:
            raise HarnessError("installed_executable_set_invalid")
        executables = self._config.installed_executables
        configuration_directory = self._runtime / "config" / "prismdrake"
        try:
            configuration_directory.mkdir(mode=0o700)
            configuration = configuration_directory / "config.toml"
            configuration.write_text(
                configuration_document or _session_config(), encoding="ascii"
            )
            configuration.chmod(0o600)
        except OSError as error:
            raise HarnessError("private_session_configuration_failed") from error
        self._start_private_bus()
        environment = self._private_bus_environment()
        environment.update(
            {
                "QSG_RHI_BACKEND": "software",
                "QT_QPA_PLATFORM": "xcb",
                "QT_QUICK_CONTROLS_STYLE": "Basic",
                "QT_LINUX_ACCESSIBILITY_ALWAYS_ON": "1",
            }
        )
        argv = (
            str(executables["prismdrake-session"]),
            "--settingsd",
            str(executables["prismdrake-settingsd"]),
            "--notifyd",
            str(executables["prismdrake-notifyd"]),
            "--shell",
            str(executables["prismdrake-shell"]),
        )
        self._session = self._operations.spawn(argv, environment)
        self._operations.pause(0.05)
        if self._session.poll() is not None:
            self._session = None
            raise HarnessError("prismdrake_session_start_failed")

    def _start_private_bus(self) -> None:
        if self._runtime is None:
            raise HarnessError("live_environment_not_started")
        address = self._private_bus_address()
        argv = (
            str(self._config.dbus_daemon),
            "--session",
            "--nofork",
            "--nopidfile",
            f"--address={address}",
        )
        child = self._operations.spawn(argv, self._base_environment())
        self._bus = child
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            if child.poll() is not None:
                raise HarnessError("private_session_bus_exited_before_readiness")
            process_identity = self._operations.identify(
                child, self._config.dbus_daemon
            )
            socket_identity = self._inspect_private_bus_socket()
            if (
                process_identity is not None
                and socket_identity is not None
                and self._private_bus_accepts_connections()
            ):
                self._bus_process_identity = process_identity
                self._bus_socket_identity = socket_identity
                return
            self._operations.pause(0.05)
        raise HarnessError("private_session_bus_readiness_failed")

    def _private_bus_address(self) -> str:
        if self._runtime is None:
            raise HarnessError("private_session_bus_not_started")
        return f"unix:path={self._runtime / 'bus'}"

    def _private_bus_environment(self) -> dict[str, str]:
        self._attest_private_bus()
        environment = self._x11_environment()
        environment["DBUS_SESSION_BUS_ADDRESS"] = self._private_bus_address()
        return environment

    def _inspect_private_bus_socket(self) -> tuple[int, int] | None:
        if self._runtime is None:
            raise HarnessError("private_session_bus_not_started")
        try:
            runtime_metadata = self._runtime.lstat()
        except FileNotFoundError:
            return None
        except OSError as error:
            raise HarnessError("private_session_bus_socket_invalid") from error
        if (
            not stat.S_ISDIR(runtime_metadata.st_mode)
            or stat.S_ISLNK(runtime_metadata.st_mode)
            or runtime_metadata.st_uid != os.geteuid()
            or stat.S_IMODE(runtime_metadata.st_mode) != 0o700
        ):
            raise HarnessError("private_session_bus_socket_invalid")
        return self._operations.socket_identity(self._runtime / "bus")

    def _private_bus_accepts_connections(self) -> bool:
        if self._runtime is None:
            return False
        return self._operations.socket_connectable(self._runtime / "bus")

    def _attest_private_bus(self) -> ProcessIdentity:
        if (
            self._bus is None
            or self._bus_process_identity is None
            or self._bus_socket_identity is None
        ):
            raise HarnessError("private_session_bus_not_started")
        identity = self._operations.identify(self._bus, self._config.dbus_daemon)
        if identity != self._bus_process_identity:
            raise HarnessError("private_session_bus_owner_changed")
        socket_identity = self._inspect_private_bus_socket()
        if socket_identity != self._bus_socket_identity:
            raise HarnessError("private_session_bus_socket_replaced")
        return identity

    def _wait_for_session_readiness(self) -> None:
        if self._session is None or self._runtime is None:
            raise HarnessError("prismdrake_session_not_started")
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            self._attest_private_bus()
            if self._session.poll() is not None:
                raise HarnessError("prismdrake_session_exited_before_readiness")
            state = self._session_marker_state()
            if state == "ready":
                return
            if state == "safe_mode":
                raise HarnessError("prismdrake_session_safe_mode_unexpected")
            self._operations.pause(0.05)
        raise HarnessError("prismdrake_session_readiness_failed")

    def _session_marker_state(self) -> str:
        if self._runtime is None:
            raise HarnessError("prismdrake_session_not_started")
        root = self._runtime / "prismdrake"
        try:
            root_metadata = root.lstat()
            entries = tuple(root.iterdir())
        except FileNotFoundError:
            return "pending"
        except OSError as error:
            raise HarnessError("prismdrake_session_runtime_invalid") from error
        if (
            stat.S_ISLNK(root_metadata.st_mode)
            or not stat.S_ISDIR(root_metadata.st_mode)
            or root_metadata.st_uid != os.geteuid()
            or stat.S_IMODE(root_metadata.st_mode) != 0o700
            or len(entries) != 1
        ):
            raise HarnessError("prismdrake_session_runtime_invalid")
        instance = entries[0]
        if re.fullmatch(r"session-[1-9][0-9]{0,19}-[0-9]{1,3}", instance.name) is None:
            raise HarnessError("prismdrake_session_runtime_invalid")
        try:
            metadata = instance.lstat()
        except OSError as error:
            raise HarnessError("prismdrake_session_runtime_invalid") from error
        if (
            stat.S_ISLNK(metadata.st_mode)
            or not stat.S_ISDIR(metadata.st_mode)
            or metadata.st_uid != os.geteuid()
            or stat.S_IMODE(metadata.st_mode) != 0o700
        ):
            raise HarnessError("prismdrake_session_runtime_invalid")
        ready = self._valid_marker(instance / "ready")
        safe_mode = self._valid_marker(instance / "safe-mode")
        if safe_mode:
            return "safe_mode"
        return "ready" if ready else "pending"

    @staticmethod
    def _valid_marker(path: Path) -> bool:
        try:
            metadata = path.lstat()
        except FileNotFoundError:
            return False
        except OSError as error:
            raise HarnessError("prismdrake_session_runtime_invalid") from error
        if (
            stat.S_ISLNK(metadata.st_mode)
            or not stat.S_ISREG(metadata.st_mode)
            or metadata.st_uid != os.geteuid()
            or stat.S_IMODE(metadata.st_mode) != 0o600
            or metadata.st_size != 0
        ):
            raise HarnessError("prismdrake_session_runtime_invalid")
        return True

    def _require_desktop_wallpaper_surfaces(
        self, *, allow_safe_mode: bool = False
    ) -> None:
        self._require_topology(two_outputs=True)
        shell = self._require_surface_shell(allow_safe_mode=allow_safe_mode)
        desktops = self._observe_surface_windows(b"_NET_WM_WINDOW_TYPE_DESKTOP", shell)
        expected = {
            WindowGeometry(0, 0, OUTPUT_WIDTH, OUTPUT_HEIGHT),
            WindowGeometry(OUTPUT_WIDTH, 0, OUTPUT_WIDTH, OUTPUT_HEIGHT),
        }
        if len(desktops) != 2 or set(desktops.values()) != expected:
            raise HarnessError("desktop_wallpaper_surface_invalid")
        # The production readiness byte is emitted only after wallpaper,
        # settings, output, and desktop generations align. Requiring that
        # marker together with two mapped shell-owned DesktopWallpaper views
        # observes the solid fallback on each output without capturing pixels.
        self._desktop_surfaces = desktops
        self._surface_shell_identity = shell

    def _require_panel_surfaces(self) -> None:
        if self._desktop_surfaces is None or self._surface_shell_identity is None:
            raise HarnessError("desktop_surface_observation_missing")
        self._require_topology(two_outputs=True)
        shell = self._require_surface_shell()
        if shell != self._surface_shell_identity:
            raise HarnessError("surface_shell_identity_changed")
        desktops = self._observe_surface_windows(b"_NET_WM_WINDOW_TYPE_DESKTOP", shell)
        if desktops != self._desktop_surfaces:
            raise HarnessError("desktop_surface_epoch_changed")
        panels = self._observe_surface_windows(b"_NET_WM_WINDOW_TYPE_DOCK", shell)
        expected_geometries = {
            WindowGeometry(0, OUTPUT_HEIGHT - PANEL_HEIGHT, OUTPUT_WIDTH, PANEL_HEIGHT),
            WindowGeometry(
                OUTPUT_WIDTH,
                OUTPUT_HEIGHT - PANEL_HEIGHT,
                OUTPUT_WIDTH,
                PANEL_HEIGHT,
            ),
        }
        if len(panels) != 2 or set(panels.values()) != expected_geometries:
            raise HarnessError("panel_surface_invalid")
        for window, geometry in panels.items():
            strut = self._window_cardinals(window, b"_NET_WM_STRUT", 4)
            partial = self._window_cardinals(window, b"_NET_WM_STRUT_PARTIAL", 12)
            if strut != (0, 0, 0, PANEL_HEIGHT) or partial != (
                0,
                0,
                0,
                PANEL_HEIGHT,
                0,
                0,
                0,
                0,
                0,
                0,
                geometry.x,
                geometry.x + geometry.width - 1,
            ):
                raise HarnessError("panel_strut_invalid")

    def _require_surface_shell(
        self, *, allow_safe_mode: bool = False
    ) -> ProcessIdentity:
        if self._session is None or self._session.poll() is not None:
            raise HarnessError("prismdrake_session_not_started")
        expected_state = "safe_mode" if allow_safe_mode else "ready"
        if self._session_marker_state() != expected_state:
            raise HarnessError("prismdrake_session_readiness_lost")
        executable = self._config.installed_executables.get("prismdrake-shell")
        if executable is None:
            raise HarnessError("surface_shell_identity_invalid")
        identities = self._operations.descendants(self._session, executable)
        if len(identities) != 1:
            raise HarnessError("surface_shell_identity_invalid")
        return identities[0]

    def _observe_surface_windows(
        self, window_type: bytes, shell: ProcessIdentity
    ) -> dict[int, WindowGeometry]:
        result = self._run_x11(
            (str(self._config.xprop), "-root", "_NET_CLIENT_LIST_STACKING")
        )
        if result.returncode != 0:
            raise HarnessError("x11_window_list_query_failed")
        windows = parse_window_ids(result.stdout, b"_NET_CLIENT_LIST_STACKING")
        observed: dict[int, WindowGeometry] = {}
        expected_type = b"_NET_WM_WINDOW_TYPE(ATOM) = " + window_type + b"\n"
        valid_type = re.compile(
            rb"_NET_WM_WINDOW_TYPE\(ATOM\) = _NET_WM_WINDOW_TYPE_[A-Z_]{1,64}"
            rb"(?:, _NET_WM_WINDOW_TYPE_[A-Z_]{1,64})*\n"
        )
        for window in windows:
            payload = self._window_property(window, b"_NET_WM_WINDOW_TYPE")
            if payload != expected_type:
                if valid_type.fullmatch(payload) is None:
                    raise HarnessError("x11_window_type_invalid")
                continue
            owner = self._window_cardinals(window, b"_NET_WM_PID", 1)
            if owner != (shell.pid,):
                raise HarnessError("surface_window_owner_invalid")
            hints = self._window_property(window, b"WM_HINTS")
            focus_lines = [
                line
                for line in hints.splitlines()
                if re.fullmatch(
                    rb"[ \t]+Client accepts input or input focus: (?:True|False)",
                    line,
                )
            ]
            if len(focus_lines) != 1 or not focus_lines[0].endswith(b": False"):
                raise HarnessError("surface_window_focus_policy_invalid")
            geometry_result = self._run_x11(
                (str(self._config.xwininfo), "-id", f"0x{window:x}", "-stats")
            )
            if geometry_result.returncode != 0:
                raise HarnessError("x11_window_geometry_query_failed")
            observed[window] = parse_window_geometry(geometry_result.stdout)
        return observed

    def _window_property(self, window: int, property_name: bytes) -> bytes:
        try:
            decoded = property_name.decode("ascii")
        except UnicodeDecodeError as error:
            raise HarnessError("x11_property_name_invalid") from error
        result = self._run_x11(
            (str(self._config.xprop), "-id", f"0x{window:x}", decoded)
        )
        if result.returncode != 0:
            raise HarnessError("x11_window_property_query_failed")
        return result.stdout

    def _window_cardinals(
        self, window: int, property_name: bytes, count: int
    ) -> tuple[int, ...]:
        return parse_cardinals(
            self._window_property(window, property_name), property_name, count
        )

    def _wait_for_xorg(self) -> bool:
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            if self._xorg is None or self._xorg.poll() is not None:
                return False
            result = self._run_x11((str(self._config.xrandr), "--query"))
            if result.returncode == 0:
                try:
                    parse_xrandr_outputs(result.stdout)
                    return True
                except HarnessError:
                    pass
            self._operations.pause(0.05)
        return False

    def _xrandr(self, *arguments: str) -> None:
        result = self._run_x11((str(self._config.xrandr), *arguments))
        if result.returncode != 0:
            raise HarnessError("xrandr_mutation_failed")

    def _configure_primary(self) -> None:
        self._xrandr(
            "--output", PRIMARY_OUTPUT, "--mode", OUTPUT_MODE,
            "--scale", "1x1", "--pos", "0x0", "--primary",
        )

    def _enable_secondary(self) -> None:
        self._xrandr(
            "--output", SECONDARY_OUTPUT, "--mode", OUTPUT_MODE,
            "--scale", "1x1", "--pos", "1600x0",
        )

    def _query_outputs(self) -> dict[str, XrandrOutput]:
        result = self._run_x11((str(self._config.xrandr), "--query"))
        if result.returncode != 0:
            raise HarnessError("xrandr_query_failed")
        return {output.name: output for output in parse_xrandr_outputs(result.stdout)}

    def _require_topology(
        self,
        *,
        two_outputs: bool,
        primary: str = PRIMARY_OUTPUT,
        secondary_position: tuple[int, int] = (1600, 0),
    ) -> None:
        outputs = self._query_outputs()
        first = outputs.get(PRIMARY_OUTPUT)
        second = outputs.get(SECONDARY_OUTPUT)
        if first is None or second is None or not first.active:
            raise HarnessError("xrandr_topology_invalid")
        if (first.width, first.height, first.x, first.y) != (
            OUTPUT_WIDTH, OUTPUT_HEIGHT, 0, 0
        ):
            raise HarnessError("xrandr_topology_invalid")
        if two_outputs:
            if not second.active or (second.width, second.height, second.x, second.y) != (
                OUTPUT_WIDTH, OUTPUT_HEIGHT, *secondary_position
            ):
                raise HarnessError("xrandr_topology_invalid")
        elif second.active:
            raise HarnessError("xrandr_topology_invalid")
        active_primary = [output.name for output in outputs.values() if output.primary]
        if active_primary != [primary]:
            raise HarnessError("xrandr_primary_invalid")

    def _require_only_secondary(self) -> None:
        outputs = self._query_outputs()
        first = outputs.get(PRIMARY_OUTPUT)
        second = outputs.get(SECONDARY_OUTPUT)
        if first is None or second is None or first.active or not second.active:
            raise HarnessError("xrandr_primary_removal_invalid")

    def _start_openbox(self) -> None:
        self._openbox = self._operations.spawn(
            (str(self._config.openbox), "--sm-disable"), self._x11_environment()
        )
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            if self._openbox.poll() is not None:
                break
            result = self._run_x11(
                (str(self._config.xprop), "-root", "_NET_SUPPORTING_WM_CHECK")
            )
            if result.returncode == 0 and re.fullmatch(
                rb"_NET_SUPPORTING_WM_CHECK\(WINDOW\): window id # 0x[0-9a-f]+\n?",
                result.stdout,
            ):
                return
            self._operations.pause(0.05)
        raise HarnessError("window_manager_readiness_failed")
