#!/usr/bin/env python3
"""Hermetic tests for the live PD2 Xorg-dummy adapter."""

from __future__ import annotations

import hashlib
import os
import stat
import sys
import tempfile
import tomllib
import unittest
from collections.abc import Mapping, Sequence
from dataclasses import replace
from pathlib import Path
from unittest.mock import patch


sys.path.insert(0, str(Path(__file__).resolve().parent))
import pd2_live_xorg as live_xorg  # noqa: E402
from pd2_demo import EXECUTABLE_NAMES, HarnessError, Observation  # noqa: E402
from pd2_live_notification_workflow import WorkflowResult  # noqa: E402
from pd2_live_lifecycle_workflow import (  # noqa: E402
    ComponentObservation as LifecycleComponentObservation,
    ProcessIdentity as LifecycleProcessIdentity,
    STEP_CATEGORIES as LIFECYCLE_STEP_CATEGORIES,
    WorkflowResult as LifecycleWorkflowResult,
)
from pd2_live_settings_workflow import (  # noqa: E402
    RESULT_CATEGORIES as SETTINGS_RESULT_CATEGORIES,
    SettingsWorkflowResult,
)
from pd2_tier_c_workflow import TIER_C_STEPS  # noqa: E402
from pd2_x11trace import X11TraceSummary  # noqa: E402
from pd2_notification_failure_replay import (  # noqa: E402
    FailureEvidence,
    MALFORMED_HISTORY_BYTES,
)
from pd2_live_xorg import (  # noqa: E402
    CONTROLLED_APPLICATION_NAME,
    CONTROLLED_WINDOW_ONE,
    CONTROLLED_WINDOW_TWO,
    CommandResult,
    LiveXorgConfig,
    LiveXorgHarnessPort,
    LiveNotificationSurfaceAdapter,
    LiveSettingsAccessibilityAdapter,
    LiveSettingsSurfaceAdapter,
    ProcessIdentity,
    QUICK_SETTINGS_WINDOW_TITLE,
    SESSION_SHUTDOWN_SEQUENCE,
    SETTINGS_WORKFLOW_CONFIG,
    SystemNotificationFailureReplayPort,
    WindowGeometry,
    XrandrOutput,
    _NotificationEpoch,
    _SettingsEpoch,
    _session_config,
    _xorg_config,
    parse_cardinals,
    parse_session_transition_output,
    parse_window_geometry,
    parse_window_ids,
    parse_window_type_atoms,
    parse_xrandr_outputs,
)


TWO_OUTPUTS = b"""Screen 0: minimum 64 x 64, current 3200 x 900, maximum 32767 x 32767
DUMMY0 connected primary 1600x900+0+0 (normal left inverted right x axis y axis) 0mm x 0mm
   1600x900      60.00*+
DUMMY1 connected 1600x900+1600+0 (normal left inverted right x axis y axis) 0mm x 0mm
   1600x900      60.00*+
DUMMY2 disconnected (normal left inverted right x axis y axis)
"""

ONE_OUTPUT = b"""Screen 0: minimum 64 x 64, current 1600 x 900, maximum 32767 x 32767
DUMMY0 connected primary 1600x900+0+0 (normal left inverted right x axis y axis) 0mm x 0mm
   1600x900      60.00*+
DUMMY1 disconnected (normal left inverted right x axis y axis)
DUMMY2 disconnected (normal left inverted right x axis y axis)
"""

ONLY_SECONDARY = b"""Screen 0: minimum 64 x 64, current 3200 x 900, maximum 32767 x 32767
DUMMY0 connected primary (normal left inverted right x axis y axis) 0mm x 0mm
DUMMY1 connected 1600x900+1600+0 (normal left inverted right x axis y axis) 0mm x 0mm
DUMMY2 disconnected (normal left inverted right x axis y axis)
"""

SECONDARY_PRIMARY = b"""Screen 0: minimum 64 x 64, current 3200 x 900, maximum 32767 x 32767
DUMMY0 connected 1600x900+0+0 (normal left inverted right x axis y axis) 0mm x 0mm
DUMMY1 connected primary 1600x900+1600+0 (normal left inverted right x axis y axis) 0mm x 0mm
DUMMY2 disconnected (normal left inverted right x axis y axis)
"""

SECONDARY_BELOW = b"""Screen 0: minimum 64 x 64, current 1600 x 1800, maximum 32767 x 32767
DUMMY0 connected primary 1600x900+0+0 (normal left inverted right x axis y axis) 0mm x 0mm
DUMMY1 connected 1600x900+0+900 (normal left inverted right x axis y axis) 0mm x 0mm
DUMMY2 disconnected (normal left inverted right x axis y axis)
"""

DESKTOP_LEFT = 0x500001
DESKTOP_RIGHT = 0x500002
PANEL_LEFT = 0x500003
PANEL_RIGHT = 0x500004
CONTROLLED_ONE_ID = 0x600001
CONTROLLED_TWO_ID = 0x600002
SURFACE_GEOMETRIES = {
    DESKTOP_LEFT: WindowGeometry(0, 0, 1600, 900),
    DESKTOP_RIGHT: WindowGeometry(1600, 0, 1600, 900),
    PANEL_LEFT: WindowGeometry(0, 852, 1600, 48),
    PANEL_RIGHT: WindowGeometry(1600, 852, 1600, 48),
}
SURFACE_TYPES = {
    DESKTOP_LEFT: b"_NET_WM_WINDOW_TYPE_DESKTOP",
    DESKTOP_RIGHT: b"_NET_WM_WINDOW_TYPE_DESKTOP",
    PANEL_LEFT: b"_NET_WM_WINDOW_TYPE_DOCK",
    PANEL_RIGHT: b"_NET_WM_WINDOW_TYPE_DOCK",
}


def xwininfo_payload(window: int, geometry: WindowGeometry) -> bytes:
    return f"""xwininfo: Window id: 0x{window:x} "Private surface"

  Absolute upper-left X:  {geometry.x}
  Absolute upper-left Y:  {geometry.y}
  Relative upper-left X:  0
  Relative upper-left Y:  0
  Width: {geometry.width}
  Height: {geometry.height}
  Depth: 24
  Border width: 0
  Class: InputOutput
  Map State: IsViewable
  Override Redirect State: no
""".encode("ascii")


class FakeChild:
    def __init__(self, pid: int) -> None:
        self.pid = pid
        self.returncode: int | None = None

    def poll(self) -> int | None:
        return self.returncode

    def wait(self, timeout: float | None = None) -> int:
        del timeout
        if self.returncode is None:
            raise AssertionError("wait called before stop")
        return self.returncode


class FakeOperations:
    def __init__(
        self,
        query_payload: bytes = TWO_OUTPUTS,
        query_payloads: Sequence[bytes] = (),
        query_results: Sequence[CommandResult] = (),
        session_marker: str = "ready",
        bus_socket_ready: bool = True,
        bus_exits_early: bool = False,
    ) -> None:
        self.query_payload = query_payload
        self.query_payloads = list(query_payloads)
        self.query_results = list(query_results)
        self.spawn_calls: list[tuple[tuple[str, ...], dict[str, str]]] = []
        self.run_calls: list[tuple[tuple[str, ...], dict[str, str]]] = []
        self.stop_calls: list[FakeChild] = []
        self.stop_identity_calls: list[ProcessIdentity] = []
        self.fail_identity_calls: list[ProcessIdentity] = []
        self.children: list[FakeChild] = []
        self.now = 0.0
        self.stop_succeeds = True
        self.session_marker = session_marker
        self.bus_socket_ready = bus_socket_ready
        self.bus_exits_early = bus_exits_early
        self.bus_socket_path: Path | None = None
        self.bus_socket_identity: tuple[int, int] | None = None
        self.identity_offsets: dict[int, int] = {}
        self.surface_geometries = dict(SURFACE_GEOMETRIES)
        self.surface_types = dict(SURFACE_TYPES)
        self.missing_surface_types: set[int] = set()
        self.surface_owner = 2004
        self.surface_focus = False
        self.surface_focus_windows: set[int] = set()
        self.surface_list = tuple(SURFACE_GEOMETRIES)
        self.window_geometry_payloads = {
            window: xwininfo_payload(window, geometry)
            for window, geometry in SURFACE_GEOMETRIES.items()
        }
        self.panel_strut_bottom = 48
        self.component_pids = {
            "prismdrake-settingsd": 2002,
            "prismdrake-notifyd": 2003,
            "prismdrake-shell": 2004,
        }
        self.component_start_times = {
            "prismdrake-settingsd": 20_020,
            "prismdrake-notifyd": 20_030,
            "prismdrake-shell": 20_040,
        }
        self.controlled_windows: dict[str, int] = {}
        self.controlled_hidden: set[int] = set()
        self.window_state_payloads: dict[int, bytes] = {}
        self.active_window: int | None = None
        self.visible_named_windows: dict[str, int] = {
            live_xorg.LAUNCHER_WINDOW_TITLE: 0x510001,
        }
        self.focused_window: int | None = None
        self.component_stop_succeeds = True
        self.component_replacement_published = True
        self.unrelated_replacement: str | None = None
        self.xorg_stopped = False
        self.xorg_query_survives_stop = False
        self.shell_crashes = 0
        self.safe_mode_published = True
        self.events: list[tuple[str, object]] = []
        self.unavailable_displays: set[int] = set()

    def spawn(self, argv: Sequence[str], env: Mapping[str, str]) -> FakeChild:
        self.events.append(("spawn", Path(argv[0]).name))
        child = FakeChild(1000 + len(self.children))
        self.children.append(child)
        self.spawn_calls.append((tuple(argv), dict(env)))
        if argv and Path(argv[0]).name == "Xorg":
            self.xorg_stopped = False
        if argv and Path(argv[0]).name == "dbus-daemon":
            if self.bus_exits_early:
                child.returncode = 1
            elif self.bus_socket_ready:
                prefix = "--address=unix:path="
                self.bus_socket_path = Path(
                    next(
                        argument[len(prefix) :]
                        for argument in argv
                        if argument.startswith(prefix)
                    )
                )
                self.bus_socket_identity = (1, 100)
        if argv and Path(argv[0]).name == "prismdrake-session":
            self.shell_crashes = 0
            root = Path(env["XDG_RUNTIME_DIR"]) / "prismdrake"
            root.mkdir(mode=0o700)
            instance = root / f"session-{child.pid}-0"
            instance.mkdir(mode=0o700)
            if self.session_marker in {"ready", "safe-mode"}:
                marker = instance / self.session_marker
                marker.touch(mode=0o600)
                marker.chmod(0o600)
        return child

    def run(
        self, argv: Sequence[str], env: Mapping[str, str], timeout: float
    ) -> CommandResult:
        del timeout
        command = tuple(argv)
        self.events.append(("run", Path(command[0]).name))
        self.run_calls.append((command, dict(env)))
        if command[:3] == (
            "/usr/bin/prismdrake-shellctl", "launcher", "toggle"
        ):
            self.visible_named_windows.setdefault(
                live_xorg.LAUNCHER_WINDOW_TITLE, 0x510001
            )
        if command[-1] == "--query":
            if self.xorg_stopped and not self.xorg_query_survives_stop:
                return CommandResult(1, b"", b"cannot open display")
            if self.query_results:
                return self.query_results.pop(0)
            if self.query_payloads:
                return CommandResult(0, self.query_payloads.pop(0))
            return CommandResult(0, self.query_payload)
        if command[0].endswith("xdotool"):
            if len(command) == 5 and command[1:4] == (
                "search",
                "--onlyvisible",
                "--name",
            ):
                title = command[4].removeprefix("^").removesuffix("$")
                identifier = self.visible_named_windows.get(
                    title, self.controlled_windows.get(title)
                )
                return (
                    CommandResult(0, f"{identifier}\n".encode("ascii"))
                    if identifier is not None
                    else CommandResult(1)
                )
            if len(command) == 4 and command[1:3] == ("windowfocus", "--sync"):
                self.focused_window = int(command[3])
                return CommandResult(0)
            if command[1:] == ("getwindowfocus",):
                if self.focused_window is None:
                    return CommandResult(1)
                return CommandResult(
                    0, f"{self.focused_window}\n".encode("ascii")
                )
            return CommandResult(0)
        if command[-1] == "_NET_ACTIVE_WINDOW":
            active = self.active_window or 0
            return CommandResult(
                0,
                f"_NET_ACTIVE_WINDOW(WINDOW): window id # 0x{active:x}\n".encode(
                    "ascii"
                ),
            )
        if "_NET_SUPPORTING_WM_CHECK" in command:
            return CommandResult(
                0, b"_NET_SUPPORTING_WM_CHECK(WINDOW): window id # 0x400001\n"
            )
        if command[-1] == "_NET_CLIENT_LIST_STACKING":
            values = b", ".join(f"0x{window:x}".encode("ascii") for window in self.surface_list)
            return CommandResult(
                0, b"_NET_CLIENT_LIST_STACKING(WINDOW): window id # " + values + b"\n"
            )
        if command[0].endswith("xprop") and len(command) == 4 and command[1] == "-id":
            window = int(command[2], 16)
            property_name = command[3]
            if property_name == "_NET_WM_WINDOW_TYPE":
                if window in self.missing_surface_types:
                    return CommandResult(0, b"_NET_WM_WINDOW_TYPE:  not found.\n")
                window_type = self.surface_types.get(window, b"_NET_WM_WINDOW_TYPE_NORMAL")
                return CommandResult(
                    0, b"_NET_WM_WINDOW_TYPE(ATOM) = " + window_type + b"\n"
                )
            if property_name == "_NET_WM_PID":
                controlled_owners = {
                    CONTROLLED_ONE_ID: 3001,
                    CONTROLLED_TWO_ID: 3002,
                }
                owner = controlled_owners.get(window, self.surface_owner)
                return CommandResult(
                    0, f"_NET_WM_PID(CARDINAL) = {owner}\n".encode("ascii")
                )
            if property_name == "WM_HINTS":
                focus = (
                    "True"
                    if self.surface_focus or window in self.surface_focus_windows
                    else "False"
                )
                return CommandResult(
                    0,
                    (
                        "WM_HINTS(WM_HINTS):\n"
                        f"\t\tClient accepts input or input focus: {focus}\n"
                        "\t\tInitial state is Normal State.\n"
                    ).encode("ascii"),
                )
            if property_name == "_NET_WM_STATE":
                if window in self.window_state_payloads:
                    return CommandResult(0, self.window_state_payloads[window])
                if window in self.controlled_hidden:
                    return CommandResult(
                        0,
                        b"_NET_WM_STATE(ATOM) = _NET_WM_STATE_HIDDEN\n",
                    )
                return CommandResult(0, b"_NET_WM_STATE:  not found.\n")
            geometry = self.surface_geometries[window]
            if property_name == "_NET_WM_STRUT":
                return CommandResult(
                    0,
                    f"_NET_WM_STRUT(CARDINAL) = 0, 0, 0, {self.panel_strut_bottom}\n".encode(
                        "ascii"
                    ),
                )
            if property_name == "_NET_WM_STRUT_PARTIAL":
                values = (
                    0,
                    0,
                    0,
                    48,
                    0,
                    0,
                    0,
                    0,
                    0,
                    0,
                    geometry.x,
                    geometry.x + geometry.width - 1,
                )
                payload = ", ".join(str(value) for value in values)
                return CommandResult(
                    0, f"_NET_WM_STRUT_PARTIAL(CARDINAL) = {payload}\n".encode("ascii")
                )
        if command[0].endswith("xwininfo") and command[1] == "-id":
            window = int(command[2], 16)
            return CommandResult(0, self.window_geometry_payloads[window])
        return CommandResult(0)

    def stop(self, child: FakeChild, timeout: float) -> bool:
        del timeout
        self.stop_calls.append(child)
        if self.stop_succeeds:
            executable = Path(self.spawn_calls[child.pid - 1000][0][0]).name
            if executable == "dbus-daemon":
                self.bus_socket_path = None
                self.bus_socket_identity = None
            elif executable == "Xorg":
                self.xorg_stopped = True
            child.returncode = 0
        return self.stop_succeeds

    def display_available(self, number: int) -> bool:
        return number not in self.unavailable_displays

    def identify(
        self, child: FakeChild, executable: Path
    ) -> ProcessIdentity | None:
        if child.poll() is not None:
            return None
        argv, _environment = self.spawn_calls[child.pid - 1000]
        if Path(argv[0]) != executable:
            return None
        return ProcessIdentity(
            child.pid, child.pid * 10 + self.identity_offsets.get(child.pid, 0)
        )

    def descendants(
        self, child: FakeChild, executable: Path
    ) -> Sequence[ProcessIdentity]:
        argv, _environment = self.spawn_calls[child.pid - 1000]
        if child.poll() is not None or Path(argv[0]).name != "prismdrake-session":
            return ()
        pid = self.component_pids.get(executable.name)
        start_time = self.component_start_times.get(executable.name)
        if pid is None or start_time is None:
            return ()
        return (ProcessIdentity(pid, start_time),)

    def identify_pid(self, pid: int, executable: Path) -> ProcessIdentity | None:
        del executable
        if pid not in (3001, 3002):
            return None
        return ProcessIdentity(pid, pid * 10)

    def stop_identity(
        self, identity: ProcessIdentity, executable: Path, timeout: float
    ) -> bool:
        del timeout
        self.stop_identity_calls.append(identity)
        return self._replace_identity(identity, executable)

    def fail_identity(
        self, identity: ProcessIdentity, executable: Path, timeout: float
    ) -> bool:
        del timeout
        self.fail_identity_calls.append(identity)
        return self._replace_identity(identity, executable)

    def _replace_identity(
        self, identity: ProcessIdentity, executable: Path
    ) -> bool:
        component_pid = self.component_pids.get(executable.name)
        if component_pid is not None:
            current = ProcessIdentity(
                component_pid, self.component_start_times[executable.name]
            )
            if identity != current:
                return False
            if not self.component_stop_succeeds:
                return False
            if self.component_replacement_published:
                self.component_start_times[executable.name] += 1
                if executable.name == "prismdrake-shell":
                    self.shell_crashes += 1
                    if self.shell_crashes == 4:
                        self.component_start_times["prismdrake-settingsd"] += 1
                        self.component_start_times["prismdrake-notifyd"] += 1
                        if self.safe_mode_published:
                            self._publish_safe_mode_marker()
            if self.unrelated_replacement is not None:
                self.component_start_times[self.unrelated_replacement] += 1
            return True
        return identity in (ProcessIdentity(3001, 30_010), ProcessIdentity(3002, 30_020))

    def socket_identity(self, path: Path) -> tuple[int, int] | None:
        if path != self.bus_socket_path:
            return None
        return self.bus_socket_identity

    def socket_connectable(self, path: Path) -> bool:
        return path == self.bus_socket_path and self.bus_socket_identity is not None

    def monotonic(self) -> float:
        return self.now

    def monotonic_ns(self) -> int:
        return int(self.now * 1_000_000_000)

    def pause(self, duration: float) -> None:
        self.now += duration

    def replace_bus_socket(self) -> None:
        if self.bus_socket_identity is None:
            raise AssertionError("private bus socket is not ready")
        self.bus_socket_identity = (1, 101)

    def _publish_safe_mode_marker(self) -> None:
        sessions = [
            (child, environment)
            for child, (argv, environment) in zip(
                self.children, self.spawn_calls, strict=True
            )
            if Path(argv[0]).name == "prismdrake-session"
            and child.poll() is None
        ]
        if len(sessions) != 1:
            raise AssertionError("expected one live Prismdrake session")
        child, environment = sessions[0]
        marker = (
            Path(environment["XDG_RUNTIME_DIR"])
            / "prismdrake"
            / f"session-{child.pid}-0"
            / "safe-mode"
        )
        marker.touch(mode=0o600)
        marker.chmod(0o600)


class FakeAccessibility:
    def __init__(self, operations: FakeOperations) -> None:
        self.operations = operations
        self.pinned = False
        self.observed: list[str] = []
        self.absent: list[str] = []
        self.states: list[tuple[str, str, bool]] = []
        self.invoked: list[str] = []
        self.persist_pin = True
        self.references: list[object] = []

    def observe(
        self, shell_pid: int, selector: object, environment: Mapping[str, str]
    ) -> bool:
        self.observed.append(selector.name)
        if (
            shell_pid != self.operations.surface_owner
            or "DBUS_SESSION_BUS_ADDRESS" not in environment
        ):
            return False
        if selector.name == "Unpin from panel":
            return self.pinned
        if selector.name == f"Close {CONTROLLED_APPLICATION_NAME}":
            return len(self.operations.controlled_windows) == 1
        for title in (CONTROLLED_WINDOW_ONE, CONTROLLED_WINDOW_TWO):
            if selector.name.endswith(title):
                return title in self.operations.controlled_windows
        return True

    def invoke(
        self, shell_pid: int, selector: object, environment: Mapping[str, str]
    ) -> bool:
        if not self.observe(shell_pid, selector, environment):
            return False
        self.invoked.append(selector.name)
        if selector.name == "Pin to panel":
            self.pinned = True
            if self.persist_pin:
                state_directory = Path(environment["XDG_STATE_HOME"]) / "prismdrake"
                state_directory.mkdir(mode=0o700, exist_ok=True)
                directory = state_directory / "shell"
                directory.mkdir(mode=0o700, exist_ok=True)
                pins = directory / "pinned-applications-v1.json"
                pins.write_bytes(
                    b'{"schema_version":1,"pins":["prismdrake-controlled.desktop"]}\n'
                )
                pins.chmod(0o600)
        elif selector.name == CONTROLLED_APPLICATION_NAME:
            self.operations.visible_named_windows.pop(
                live_xorg.LAUNCHER_WINDOW_TITLE, None
            )
            if CONTROLLED_WINDOW_ONE not in self.operations.controlled_windows:
                self.operations.controlled_windows[CONTROLLED_WINDOW_ONE] = CONTROLLED_ONE_ID
                self.operations.active_window = CONTROLLED_ONE_ID
        elif selector.name == "Second window":
            self.operations.visible_named_windows.pop(
                live_xorg.LAUNCHER_WINDOW_TITLE, None
            )
            self.operations.controlled_windows[CONTROLLED_WINDOW_TWO] = CONTROLLED_TWO_ID
            self.operations.active_window = CONTROLLED_TWO_ID
        elif selector.name.startswith("Activate "):
            title = selector.name.removeprefix("Activate ")
            identifier = self.operations.controlled_windows[title]
            self.operations.controlled_hidden.discard(identifier)
            self.operations.active_window = identifier
        elif selector.name.startswith("Minimize "):
            title = selector.name.removeprefix("Minimize ")
            identifier = self.operations.controlled_windows[title]
            self.operations.controlled_hidden.add(identifier)
        elif selector.name == f"Close {CONTROLLED_APPLICATION_NAME}":
            title, identifier = next(iter(self.operations.controlled_windows.items()))
            del self.operations.controlled_windows[title]
            self.operations.controlled_hidden.discard(identifier)
        elif selector.name.startswith("Close "):
            title = selector.name.removeprefix("Close ")
            identifier = self.operations.controlled_windows.pop(title)
            self.operations.controlled_hidden.discard(identifier)
        return True

    def observe_state(
        self,
        shell_pid: int,
        selector: object,
        state_name: str,
        expected: bool,
        environment: Mapping[str, str],
    ) -> bool:
        self.states.append((selector.name, state_name, expected))
        return (
            self.observe(shell_pid, selector, environment)
            and state_name in {"CHECKED", "ENABLED"}
            and isinstance(expected, bool)
        )

    def observe_absent(
        self, shell_pid: int, selector: object, environment: Mapping[str, str]
    ) -> bool:
        self.absent.append(selector.name)
        return (
            shell_pid == self.operations.surface_owner
            and "DBUS_SESSION_BUS_ADDRESS" in environment
            and selector.name
            in {
                "Transient notifications",
                "Desktop wallpaper",
                "Quick settings",
                "Notification center",
            }
            or (
                shell_pid == self.operations.surface_owner
                and "DBUS_SESSION_BUS_ADDRESS" in environment
                and selector.role == "menu_item"
                and selector.name in self.invoked
            )
        )

    def reject_stale_action(
        self, shell_pid: int, selector: object, environment: Mapping[str, str]
    ) -> bool:
        return (
            "DBUS_SESSION_BUS_ADDRESS" in environment
            and shell_pid == self.operations.surface_owner
            and selector.name == f"Close {CONTROLLED_APPLICATION_NAME}"
            and not self.operations.controlled_windows
        )

    def capture_reference(self, shell_pid, selector, environment) -> object | None:
        if not self.observe(shell_pid, selector, environment):
            return None
        reference = object()
        self.references.append(reference)
        return reference

    def observe_reference(self, shell_pid, reference, environment) -> bool:
        return (
            reference in self.references
            and shell_pid == self.operations.surface_owner
            and "DBUS_SESSION_BUS_ADDRESS" in environment
        )

    def reject_stale_reference(self, reference, environment) -> bool:
        return reference in self.references and "DBUS_SESSION_BUS_ADDRESS" in environment


def config(installed: Mapping[str, Path] | None = None) -> LiveXorgConfig:
    return LiveXorgConfig(
        xorg=Path("/usr/bin/Xorg"),
        xrandr=Path("/usr/bin/xrandr"),
        openbox=Path("/usr/bin/openbox"),
        xprop=Path("/usr/bin/xprop"),
        xwininfo=Path("/usr/bin/xwininfo"),
        dbus_daemon=Path("/usr/bin/dbus-daemon"),
        dbus_monitor=Path("/usr/bin/dbus-monitor"),
        installed_executables=installed or {},
        xdotool=Path("/usr/bin/xdotool"),
        xwd=Path("/usr/bin/xwd"),
        first_display=140,
        last_display=140,
        readiness_timeout=0.2,
        stop_timeout=0.1,
    )


def installed_paths() -> dict[str, Path]:
    return {name: Path("/usr/bin") / name for name in EXECUTABLE_NAMES}


class FakeNotificationWorkflow:
    _RESULTS = {
        "create_normal": "pd2_notification_normal_created",
        "replace_normal": "pd2_notification_replaced",
        "invoke_live_action": "pd2_notification_action_completed",
        "open_center": "pd2_notification_center_opened",
        "enable_do_not_disturb": "pd2_notification_dnd_enabled",
        "create_suppressed_normal": "pd2_notification_normal_suppressed",
        "create_critical_bypass": "pd2_notification_critical_bypassed",
        "clear_history": "pd2_notification_history_cleared",
    }

    def __init__(self) -> None:
        self.calls: list[str] = []

    def _run(self, name: str) -> WorkflowResult:
        self.calls.append(name)
        return WorkflowResult(self._RESULTS[name])

    def create_normal(self) -> WorkflowResult:
        return self._run("create_normal")

    def replace_normal(self) -> WorkflowResult:
        return self._run("replace_normal")

    def invoke_live_action(self) -> WorkflowResult:
        return self._run("invoke_live_action")

    def open_center(self) -> WorkflowResult:
        return self._run("open_center")

    def enable_do_not_disturb(self) -> WorkflowResult:
        return self._run("enable_do_not_disturb")

    def create_suppressed_normal(self) -> WorkflowResult:
        return self._run("create_suppressed_normal")

    def create_critical_bypass(self) -> WorkflowResult:
        return self._run("create_critical_bypass")

    def clear_history(self) -> WorkflowResult:
        return self._run("clear_history")


class FakeLifecycleWorkflow:
    def __init__(self) -> None:
        self.calls: list[str] = []

    def run_step(self, identifier: str) -> LifecycleWorkflowResult:
        self.calls.append(identifier)
        return LifecycleWorkflowResult(LIFECYCLE_STEP_CATEGORIES[identifier])


class FakeClosable:
    def __init__(self) -> None:
        self.close_calls = 0

    def close(self) -> None:
        self.close_calls += 1


class FakeTierCTrace:
    def __init__(self, proxy_display: str) -> None:
        self.proxy_display = proxy_display
        self.finished = False
        self.poll_calls = 0
        self.finish_calls: list[bool] = []
        self.client_environments: list[dict[str, str]] = []

    def client_environment(self, environment: Mapping[str, str]) -> dict[str, str]:
        selected = dict(environment)
        selected["DISPLAY"] = self.proxy_display
        selected.pop("XAUTHORITY", None)
        self.client_environments.append(selected)
        return selected

    def poll(self) -> None:
        if self.finished:
            raise RuntimeError("trace finished")
        self.poll_calls += 1

    def finish(self, *, clients_exited: bool) -> X11TraceSummary:
        self.finish_calls.append(clients_exited)
        self.finished = True
        if clients_exited is not True:
            raise RuntimeError("clients active")
        return X11TraceSummary(
            True, 1, 1, 0, 0, 0, 0, 0, 0, 0, False, False
        )


class FakeNotificationEpochFactory:
    def __init__(self) -> None:
        self.workflow = FakeNotificationWorkflow()
        self.sender = FakeClosable()
        self.dbus_probe = FakeClosable()
        self.calls: list[tuple[dict[str, str], object, object]] = []

    def __call__(self, environment, endpoint, surface):
        self.calls.append((dict(environment), endpoint, surface))
        return _NotificationEpoch(
            self.workflow, self.sender, self.dbus_probe  # type: ignore[arg-type]
        )


class FakeSettingsWorkflow:
    def __init__(self) -> None:
        self.calls: list[str] = []

    def run_step(self, identifier: str) -> SettingsWorkflowResult:
        self.calls.append(identifier)
        return SettingsWorkflowResult(SETTINGS_RESULT_CATEGORIES[identifier])


class FakeSettingsEpochFactory:
    def __init__(self) -> None:
        self.workflow = FakeSettingsWorkflow()
        self.dbus_probe = FakeClosable()
        self.calls: list[tuple[dict[str, str], object, object, object]] = []

    def __call__(self, environment, endpoint, accessibility, surfaces):
        self.calls.append((dict(environment), endpoint, accessibility, surfaces))
        return _SettingsEpoch(
            self.workflow, self.dbus_probe  # type: ignore[arg-type]
        )


class FakeNotificationFailureReplay:
    def __init__(self) -> None:
        self.malformed_calls: list[tuple[object, dict[str, str], object]] = []
        self.conflict_calls: list[
            tuple[Path, dict[str, str], object, dict[str, tuple[int, int]]]
        ] = []

    def verify_malformed_history(self, fixture, environment, endpoint):
        fixture.verify_retained()
        self.malformed_calls.append((fixture, dict(environment), endpoint))
        return FailureEvidence("malformed_persistent_history_recovered")

    def verify_conflicting_owner(
        self, executable, environment, endpoint, sibling_observer
    ):
        before = dict(sibling_observer())
        after = dict(sibling_observer())
        if after != before:
            raise RuntimeError("private sibling drift")
        self.conflict_calls.append(
            (executable, dict(environment), endpoint, before)
        )
        return FailureEvidence("notification_owner_conflict_isolated")


class ScriptedNotificationAccessibility:
    def __init__(self) -> None:
        self.visible: dict[str, list[bool]] = {}
        self.states: dict[str, list[bool]] = {}
        self.absent: dict[str, list[bool]] = {}
        self.invoked: list[str] = []
        self.calls: list[tuple[str, str]] = []

    @staticmethod
    def _next(values: dict[str, list[bool]], name: str) -> bool:
        sequence = values.get(name, [])
        return sequence.pop(0) if sequence else False

    def observe(self, shell_pid, selector, environment) -> bool:
        del shell_pid, environment
        self.calls.append(("visible", selector.name))
        return self._next(self.visible, selector.name)

    def invoke(self, shell_pid, selector, environment) -> bool:
        del shell_pid, environment
        self.calls.append(("invoke", selector.name))
        self.invoked.append(selector.name)
        return True

    def observe_state(
        self, shell_pid, selector, state_name, expected, environment
    ) -> bool:
        del shell_pid, environment
        self.calls.append((state_name, selector.name))
        return expected is True and self._next(self.states, selector.name)

    def observe_absent(self, shell_pid, selector, environment) -> bool:
        del shell_pid, environment
        self.calls.append(("absent", selector.name))
        return self._next(self.absent, selector.name)

    def reject_stale_action(self, shell_pid, selector, environment) -> bool:
        del shell_pid, selector, environment
        return False

    def capture_reference(self, shell_pid, selector, environment):
        del shell_pid, selector, environment
        return None

    def observe_reference(self, shell_pid, reference, environment) -> bool:
        del shell_pid, reference, environment
        return False

    def reject_stale_reference(self, reference, environment) -> bool:
        del reference, environment
        return False


class XrandrParserTests(unittest.TestCase):
    def test_parses_active_primary_and_disconnected_outputs(self) -> None:
        outputs = parse_xrandr_outputs(TWO_OUTPUTS)

        self.assertEqual(
            outputs,
            (
                XrandrOutput("DUMMY0", True, True, 1600, 900, 0, 0),
                XrandrOutput("DUMMY1", True, False, 1600, 900, 1600, 0),
                XrandrOutput("DUMMY2", False, False, None, None, None, None),
            ),
        )

    def test_rejects_empty_nul_only_unknown_and_duplicate_output(self) -> None:
        cases = (
            b"",
            b"\x00",
            b"Screen 0: no output records\n",
            TWO_OUTPUTS + TWO_OUTPUTS.splitlines()[1] + b"\n",
        )
        for payload in cases:
            with self.subTest(payload=payload[:30]):
                with self.assertRaises(HarnessError):
                    parse_xrandr_outputs(payload)

    def test_dummy_configuration_is_private_deterministic_and_network_closed(self) -> None:
        payload = _xorg_config()
        self.assertIn('Driver "dummy"', payload)
        self.assertIn('Virtual 3200 1800', payload)
        self.assertIn('Modeline "1600x900"', payload)
        self.assertNotIn("/home/", payload)
        self.assertNotIn("Display", payload.splitlines()[0])


class SurfaceParserTests(unittest.TestCase):
    def test_parses_bounded_window_lists_cardinals_and_geometry(self) -> None:
        self.assertEqual(
            parse_window_ids(
                b"_NET_CLIENT_LIST_STACKING(WINDOW): window id # 0x500001, 0x500004\n",
                b"_NET_CLIENT_LIST_STACKING",
            ),
            (DESKTOP_LEFT, PANEL_RIGHT),
        )
        self.assertEqual(
            parse_cardinals(
                b"_NET_WM_STRUT(CARDINAL) = 0, 0, 0, 48\n",
                b"_NET_WM_STRUT",
                4,
            ),
            (0, 0, 0, 48),
        )
        self.assertEqual(
            parse_window_type_atoms(
                b"_NET_WM_WINDOW_TYPE(ATOM) = _NET_WM_WINDOW_TYPE_UTILITY, "
                b"_KDE_NET_WM_WINDOW_TYPE_OVERRIDE, _NET_WM_WINDOW_TYPE_NORMAL\n"
            ),
            (
                b"_NET_WM_WINDOW_TYPE_UTILITY",
                b"_KDE_NET_WM_WINDOW_TYPE_OVERRIDE",
                b"_NET_WM_WINDOW_TYPE_NORMAL",
            ),
        )
        self.assertEqual(
            parse_window_geometry(
                xwininfo_payload(DESKTOP_RIGHT, SURFACE_GEOMETRIES[DESKTOP_RIGHT])
            ),
            SURFACE_GEOMETRIES[DESKTOP_RIGHT],
        )

    def test_surface_parsers_reject_ambiguous_or_unmapped_observations(self) -> None:
        cases = (
            lambda: parse_window_ids(
                b"_NET_CLIENT_LIST_STACKING(WINDOW): window id # 0x1, 0x1\n",
                b"_NET_CLIENT_LIST_STACKING",
            ),
            lambda: parse_window_ids(
                b"_NET_CLIENT_LIST_STACKING(WINDOW): window id # 0x0\n",
                b"_NET_CLIENT_LIST_STACKING",
            ),
            lambda: parse_cardinals(
                b"_NET_WM_STRUT(CARDINAL) = 0, 0, 48\n", b"_NET_WM_STRUT", 4
            ),
            lambda: parse_window_type_atoms(
                b"_NET_WM_WINDOW_TYPE(ATOM) = _PRIVATE_WINDOW_TYPE\n"
            ),
            lambda: parse_window_type_atoms(
                b"_NET_WM_WINDOW_TYPE(ATOM) = _NET_WM_WINDOW_TYPE_NORMAL, "
                b"_NET_WM_WINDOW_TYPE_NORMAL\n"
            ),
            lambda: parse_window_geometry(
                xwininfo_payload(DESKTOP_LEFT, SURFACE_GEOMETRIES[DESKTOP_LEFT]).replace(
                    b"Map State: IsViewable", b"Map State: IsUnMapped"
                )
            ),
        )
        for case in cases:
            with self.subTest(case=case):
                with self.assertRaises(HarnessError):
                    case()

    def test_private_session_config_selects_two_panel_solid_wallpaper_policy(self) -> None:
        payload = _session_config()
        document = tomllib.loads(payload)
        self.assertEqual(document["schema_version"], 2)
        self.assertEqual(document["panel"]["output_policy"], "all")
        self.assertEqual(document["panel"]["size_px"], 48)
        self.assertEqual(document["panel"]["grouping"], "always")
        self.assertEqual(document["desktop"]["wallpaper_source"], "")
        self.assertEqual(document["desktop"]["background_color"], "#182033")
        self.assertEqual(document["notifications"]["history"], "session")
        self.assertFalse(document["appearance"]["reduced_motion"])
        self.assertEqual(document["developer"]["mock_capability_overrides"], [])
        self.assertNotIn("/home/", payload)

        persistent = tomllib.loads(_session_config("persistent"))
        self.assertEqual(persistent["notifications"]["history"], "persistent")
        reduced = tomllib.loads(_session_config(reduced_motion=True))
        self.assertTrue(reduced["appearance"]["reduced_motion"])
        with self.assertRaisesRegex(
            HarnessError, "^private_session_history_mode_invalid$"
        ):
            _session_config("disabled")
        with self.assertRaisesRegex(
            HarnessError, "^private_session_reduced_motion_invalid$"
        ):
            _session_config(reduced_motion=1)  # type: ignore[arg-type]


class SessionTransitionParserTests(unittest.TestCase):
    @staticmethod
    def payload(
        states: Sequence[str], transactions: Sequence[int] | None = None
    ) -> bytes:
        selected = transactions or (42,) * len(states)
        return b"".join(
            (
                b"signal time=1 sender=:1.4 -> destination=(null destination) "
                b"serial=1 path=/org/prismdrake/Session1; "
                b"interface=org.prismdrake.Session1; member=SessionStateChanged\n"
                + f"   uint64 {transaction}\n".encode("ascii")
                + f'   string "{state}"\n'.encode("ascii")
            )
            for transaction, state in zip(selected, states, strict=True)
        )

    def test_accepts_only_exact_transaction_shutdown_sequence(self) -> None:
        payload = self.payload(SESSION_SHUTDOWN_SEQUENCE)
        self.assertEqual(
            parse_session_transition_output(payload, 42), SESSION_SHUTDOWN_SEQUENCE
        )

    def test_rejects_missing_transition(self) -> None:
        with self.assertRaisesRegex(
            HarnessError, "^lifecycle_logout_transition_invalid$"
        ):
            parse_session_transition_output(
                self.payload(SESSION_SHUTDOWN_SEQUENCE[:-1]), 42
            )

    def test_rejects_reordered_transition(self) -> None:
        reordered = list(SESSION_SHUTDOWN_SEQUENCE)
        reordered[1], reordered[2] = reordered[2], reordered[1]
        with self.assertRaisesRegex(
            HarnessError, "^lifecycle_logout_transition_invalid$"
        ):
            parse_session_transition_output(self.payload(reordered), 42)

    def test_rejects_wrong_transaction(self) -> None:
        transactions = [42] * len(SESSION_SHUTDOWN_SEQUENCE)
        transactions[2] = 43
        with self.assertRaisesRegex(
            HarnessError, "^lifecycle_logout_transition_invalid$"
        ):
            parse_session_transition_output(
                self.payload(SESSION_SHUTDOWN_SEQUENCE, transactions), 42
            )

    def test_rejects_extra_transition(self) -> None:
        with self.assertRaisesRegex(
            HarnessError, "^lifecycle_logout_transition_invalid$"
        ):
            parse_session_transition_output(
                self.payload((*SESSION_SHUTDOWN_SEQUENCE, "complete")), 42
            )

    def test_rejects_malformed_transition_arguments(self) -> None:
        malformed = self.payload(SESSION_SHUTDOWN_SEQUENCE).replace(
            b'   string "confirmed"\n',
            b'   string "confirmed"\n   string "unexpected"\n',
            1,
        )
        with self.assertRaisesRegex(
            HarnessError, "^lifecycle_logout_transition_invalid$"
        ):
            parse_session_transition_output(malformed, 42)


class NotificationSurfaceAdapterTests(unittest.TestCase):
    def test_observes_only_reviewed_shell_owned_notification_controls(self) -> None:
        operations = FakeOperations()
        accessibility = ScriptedNotificationAccessibility()
        accessibility.visible = {
            "Review": [False, True],
            "Notification center": [True],
            "Transient notifications": [True],
        }
        accessibility.states = {"Do not disturb": [True]}
        accessibility.absent = {
            "Notification center": [True],
            "Transient notifications": [True],
        }
        dismissed: list[str] = []
        environment = {
            "DISPLAY": ":140",
            "DBUS_SESSION_BUS_ADDRESS": "unix:path=/tmp/private-bus",
        }
        adapter = LiveNotificationSurfaceAdapter(
            accessibility,
            lambda: ProcessIdentity(2004, 20_040),
            lambda: environment,
            operations.monotonic,
            operations.pause,
            lambda: dismissed.append("Escape"),
        )

        adapter.invoke_live_action(0.2)
        adapter.observe_center(0.2)
        adapter.observe_do_not_disturb(True, 0.2)
        adapter.dismiss_center(0.2)
        adapter.observe_transient(False, 0.2)
        adapter.observe_transient(True, 0.2)

        self.assertEqual(dismissed, ["Escape"])
        self.assertEqual(accessibility.invoked, ["Review"])
        self.assertIn(("CHECKED", "Do not disturb"), accessibility.calls)
        self.assertIn(("absent", "Notification center"), accessibility.calls)
        self.assertIn(("absent", "Transient notifications"), accessibility.calls)
        self.assertIn(("visible", "Transient notifications"), accessibility.calls)

    def test_transient_absence_times_out_when_inspection_cannot_prove_it(self) -> None:
        operations = FakeOperations()
        accessibility = ScriptedNotificationAccessibility()
        adapter = LiveNotificationSurfaceAdapter(
            accessibility,
            lambda: ProcessIdentity(2004, 20_040),
            lambda: {
                "DISPLAY": ":140",
                "DBUS_SESSION_BUS_ADDRESS": "unix:path=/tmp/private-bus",
            },
            operations.monotonic,
            operations.pause,
            lambda: None,
        )

        with self.assertRaisesRegex(
            HarnessError, "^notification_surface_transient_absence_timeout$"
        ):
            adapter.observe_transient(False, 0.1)
        self.assertTrue(accessibility.calls)
        self.assertTrue(all(kind == "absent" for kind, _name in accessibility.calls))


class SettingsSurfaceAdapterTests(unittest.TestCase):
    def test_accessibility_reacquires_exact_shell_owner_until_surface_is_visible(
        self,
    ) -> None:
        operations = FakeOperations()
        calls: list[tuple[int, str, str, str]] = []

        class Accessibility:
            def observe(self, shell_pid, selector, environment):
                calls.append(
                    (shell_pid, selector.name, selector.role, environment["DISPLAY"])
                )
                return len(calls) == 2

        identities = iter(
            (ProcessIdentity(2004, 20_040), ProcessIdentity(2005, 20_050))
        )
        adapter = LiveSettingsAccessibilityAdapter(
            Accessibility(),  # type: ignore[arg-type]
            lambda: next(identities),
            lambda: {
                "DISPLAY": ":140",
                "DBUS_SESSION_BUS_ADDRESS": "unix:path=/tmp/private-bus",
            },
            operations.monotonic,
            operations.pause,
            0.2,
        )

        self.assertTrue(adapter.observe_surface("Quick settings", "pane"))
        self.assertEqual(
            calls,
            [
                (2004, "Quick settings", "pane", ":140"),
                (2005, "Quick settings", "pane", ":140"),
            ],
        )
        self.assertFalse(adapter.observe_surface("Quick settings", "panel"))

    def test_captures_each_fixed_geometry_surface_twice_in_sequence(self) -> None:
        operations = FakeOperations()
        windows = {
            "panel_primary": PANEL_LEFT,
            "panel_secondary": PANEL_RIGHT,
            "quick_settings": 0x500005,
        }
        events: list[tuple[str, object]] = []

        def resolve(surface: str) -> int:
            events.append(("resolve", surface))
            return windows[surface]

        def capture(window: int, points: Sequence[tuple[int, int]]) -> bytes:
            events.append(("capture", (window, tuple(points))))
            return bytes([window & 0xFF]) * 32

        adapter = LiveSettingsSurfaceAdapter(
            SETTINGS_WORKFLOW_CONFIG, resolve, capture, operations.pause
        )

        for surface in SETTINGS_WORKFLOW_CONFIG.reviewed_surfaces:
            points = SETTINGS_WORKFLOW_CONFIG.points_for(surface)
            self.assertEqual(
                adapter.capture_settled_fingerprints(surface, points),
                (bytes([windows[surface] & 0xFF]) * 32,) * 2,
            )

        expected: list[tuple[str, object]] = []
        for surface in SETTINGS_WORKFLOW_CONFIG.reviewed_surfaces:
            window = windows[surface]
            points = SETTINGS_WORKFLOW_CONFIG.points_for(surface)
            expected.extend(
                [
                    ("resolve", surface),
                    ("capture", (window, points)),
                    ("capture", (window, points)),
                ]
            )
        self.assertEqual(events, expected)
        self.assertAlmostEqual(operations.now, 0.15)
        self.assertEqual(
            SETTINGS_WORKFLOW_CONFIG.sample_points,
            (
                ((400, 2), (800, 2), (1200, 2)),
                ((400, 2), (800, 2), (1200, 2)),
                ((24, 2), (190, 2), (355, 2)),
            ),
        )
        with self.assertRaisesRegex(
            HarnessError, "^settings_surface_capture_not_reviewed$"
        ):
            adapter.capture_settled_fingerprints("private", ((0, 0),))


class NotificationFailureReplayAdapterTests(unittest.TestCase):
    def test_malformed_history_probe_is_closed_after_categorical_verification(
        self,
    ) -> None:
        calls: list[tuple[str, object]] = []

        class Probe:
            def __init__(self, environment, *, endpoint):
                calls.append(("probe", (dict(environment), endpoint)))

            def close(self):
                calls.append(("close", None))

        evidence = FailureEvidence("malformed_persistent_history_recovered")

        def verify(fixture, probe):
            calls.append(("verify", (fixture, probe)))
            return evidence

        fixture = object()
        endpoint = object()
        adapter = SystemNotificationFailureReplayPort()
        with patch.object(live_xorg, "LiveDbusProbe", Probe), patch.object(
            live_xorg, "verify_malformed_history_recovery", verify
        ):
            result = adapter.verify_malformed_history(
                fixture,  # type: ignore[arg-type]
                {"DBUS_SESSION_BUS_ADDRESS": "unix:path=/tmp/private"},
                endpoint,  # type: ignore[arg-type]
            )

        self.assertEqual(result, evidence)
        self.assertEqual([kind for kind, _value in calls], ["probe", "verify", "close"])

    def test_owner_lease_encloses_exact_candidate_and_sibling_verification(self) -> None:
        calls: list[tuple[str, object]] = []
        owner = object()

        class Lease:
            def __init__(self, environment, endpoint):
                calls.append(("lease", (dict(environment), endpoint)))

            def __enter__(self):
                calls.append(("enter", None))
                return owner

            def __exit__(self, exception_type, exception, traceback):
                del exception_type, exception, traceback
                calls.append(("exit", None))

        def verify(executable, environment, endpoint, observed_owner, siblings):
            calls.append(
                (
                    "verify",
                    (
                        executable,
                        dict(environment),
                        endpoint,
                        observed_owner,
                        dict(siblings()),
                    ),
                )
            )
            return FailureEvidence("notification_owner_conflict_isolated")

        executable = Path("/usr/bin/prismdrake-notifyd")
        endpoint = object()
        environment = {"DBUS_SESSION_BUS_ADDRESS": "unix:path=/tmp/private"}
        def siblings():
            return {
                "prismdrake-settingsd": (101, 1001),
                "prismdrake-shell": (102, 1002),
            }
        adapter = SystemNotificationFailureReplayPort()
        with patch.object(live_xorg, "GioNotificationOwnerLease", Lease), patch.object(
            live_xorg, "verify_conflicting_notification_owner", verify
        ):
            result = adapter.verify_conflicting_owner(
                executable,
                environment,
                endpoint,  # type: ignore[arg-type]
                siblings,
            )

        self.assertEqual(result.category, "notification_owner_conflict_isolated")
        self.assertEqual(
            [kind for kind, _value in calls], ["lease", "enter", "verify", "exit"]
        )


class LivePortTests(unittest.TestCase):
    def test_failure_signal_requires_exact_pid_start_time_identity(self) -> None:
        operations = live_xorg.SystemOperations()
        expected = ProcessIdentity(4312, 98_765)
        recycled = ProcessIdentity(expected.pid, expected.start_time + 1)

        with patch.object(
            operations, "identify_pid", return_value=recycled
        ), patch.object(live_xorg.os, "kill") as kill:
            self.assertFalse(
                operations.fail_identity(
                    expected, Path("/usr/bin/prismdrake-settingsd"), 1.0
                )
            )
        kill.assert_not_called()

        with patch.object(
            operations, "identify_pid", side_effect=(expected, None)
        ), patch.object(live_xorg.os, "kill") as kill:
            self.assertTrue(
                operations.fail_identity(
                    expected, Path("/usr/bin/prismdrake-settingsd"), 1.0
                )
            )
        kill.assert_called_once_with(expected.pid, live_xorg.signal.SIGKILL)

    def test_performance_endpoints_use_shell_start_and_exact_notify_reply(self) -> None:
        port = LiveXorgHarnessPort(config(), FakeOperations())
        port._component_identity = lambda _name: ProcessIdentity(44, 250)  # type: ignore[method-assign]  # noqa: SLF001
        with patch.object(live_xorg.os, "sysconf", return_value=100), patch.object(
            live_xorg.time, "clock_gettime_ns", return_value=3_000_000_000
        ):
            self.assertEqual(port.shell_complete_presentation_ns(), 500_000_000)

        class Transport:
            def take_notify_reply_latency_ns(self) -> int:
                return 975

        sender = live_xorg.PersistentNotificationProbe(Transport())  # type: ignore[arg-type]
        port._notification_epoch = _NotificationEpoch(  # noqa: SLF001
            object(), sender, object()  # type: ignore[arg-type]
        )
        self.assertEqual(port.notification_method_latency_ns(), 975)

    def test_tier_c_material_accepts_argb_and_rejects_unreviewed_surface_depth(self) -> None:
        operations = FakeOperations()
        operations.window_geometry_payloads[DESKTOP_LEFT] = operations.window_geometry_payloads[
            DESKTOP_LEFT
        ].replace(b"  Depth: 24", b"  Depth: 32")
        port = LiveXorgHarnessPort(config(), operations)
        port._display = ":140"  # noqa: SLF001

        self.assertEqual(
            port._tier_c_window_geometry(DESKTOP_LEFT),  # noqa: SLF001
            WindowGeometry(0, 0, 1600, 900),
        )

        operations.window_geometry_payloads[DESKTOP_LEFT] = operations.window_geometry_payloads[
            DESKTOP_LEFT
        ].replace(b"  Depth: 32", b"  Depth: 16")
        with self.assertRaisesRegex(HarnessError, "^tier_c_surface_depth_invalid$"):
            port._tier_c_window_geometry(DESKTOP_LEFT)  # noqa: SLF001

    def test_quotes_whitespace_fixture_and_rejects_desktop_field_code_paths(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            data = root / "data"
            data.mkdir(mode=0o700)
            (root / "home").mkdir(mode=0o700)
            fixture = root / "controlled fixture"
            fixture.write_bytes(b"fixture")
            fixture.chmod(0o700)
            port = LiveXorgHarnessPort(
                replace(config(), controlled_window_fixture=fixture), FakeOperations()
            )
            port._runtime = root  # noqa: SLF001

            port._write_controlled_desktop_entry()  # noqa: SLF001

            payload = (data / "applications" / "prismdrake-controlled.desktop").read_text(
                encoding="ascii"
            )
            self.assertIn(f'Exec="{fixture}" --instance one\n', payload)
            self.assertIn(f'Exec="{fixture}" --instance two\n', payload)
            self.assertIn(f"Path={root / 'home'}\n", payload)

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "data").mkdir(mode=0o700)
            fixture = root / "controlled-fixture"
            fixture.write_bytes(b"fixture")
            fixture.chmod(0o700)
            port = LiveXorgHarnessPort(
                replace(config(), controlled_window_fixture=fixture), FakeOperations()
            )
            port._runtime = root  # noqa: SLF001
            with self.assertRaisesRegex(
                HarnessError, "^controlled_working_directory_invalid$"
            ):
                port._write_controlled_desktop_entry()  # noqa: SLF001

        for marker in ("%", '"', "\\"):
            with self.subTest(marker=marker), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                (root / "data").mkdir(mode=0o700)
                fixture = root / f"controlled{marker}fixture"
                fixture.write_bytes(b"fixture")
                fixture.chmod(0o700)
                port = LiveXorgHarnessPort(
                    replace(config(), controlled_window_fixture=fixture),
                    FakeOperations(),
                )
                port._runtime = root  # noqa: SLF001
                with self.assertRaisesRegex(
                    HarnessError, "^controlled_fixture_invalid$"
                ):
                    port._write_controlled_desktop_entry()  # noqa: SLF001

    def test_routes_one_tier_c_workflow_through_proxy_and_natural_cleanup(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Path(temporary) / "controlled fixture"
            fixture.write_bytes(b"fixture")
            fixture.chmod(0o700)
            operations = FakeOperations()
            accessibility = FakeAccessibility(operations)
            named = {
                live_xorg.LAUNCHER_WINDOW_TITLE: 0x510001,
                live_xorg.NOTIFICATION_WINDOW_TITLE: 0x510002,
                live_xorg.NOTIFICATION_CENTER_WINDOW_TITLE: 0x510003,
                QUICK_SETTINGS_WINDOW_TITLE: 0x510004,
                live_xorg.LOGOUT_WINDOW_TITLE: 0x510005,
            }
            for index, (title, window) in enumerate(named.items()):
                geometry = WindowGeometry(100 + index, 100, 380, 400)
                operations.controlled_windows[title] = window
                operations.surface_geometries[window] = geometry
                operations.window_geometry_payloads[window] = xwininfo_payload(
                    window, geometry
                )

            traces: list[FakeTierCTrace] = []

            def trace_factory(trace_config, environment):
                self.assertEqual(trace_config.real_display, ":140")
                self.assertEqual(trace_config.fake_display, ":1140")
                self.assertNotIn("DISPLAY", environment)
                self.assertEqual(
                    [event for event in operations.events if event[0] == "spawn"],
                    [("spawn", "Xorg")],
                )
                operations.events.append(("trace", trace_config.fake_display))
                trace = FakeTierCTrace(trace_config.fake_display)
                traces.append(trace)
                return trace

            port = LiveXorgHarnessPort(
                replace(
                    config(installed_paths()), controlled_window_fixture=fixture
                ),
                operations,
                accessibility,
                notification_epoch_factory=FakeNotificationEpochFactory(),
                tier_c_trace_factory=trace_factory,
            )
            real_xdotool = port._xdotool  # noqa: SLF001

            def drive_tier_c_key(*arguments: str):
                result = real_xdotool(*arguments)
                if arguments[:2] != ("key", "--clearmodifiers"):
                    return result
                identifier = port._tier_c_identifier  # noqa: SLF001
                if identifier == "launcher" and arguments[-1] == "Return":
                    operations.controlled_windows.pop(
                        live_xorg.LAUNCHER_WINDOW_TITLE, None
                    )
                    operations.visible_named_windows.pop(
                        live_xorg.LAUNCHER_WINDOW_TITLE, None
                    )
                    operations.controlled_windows[CONTROLLED_WINDOW_ONE] = (
                        CONTROLLED_ONE_ID
                    )
                elif identifier == "logout" and arguments[-1] == "Return":
                    if port._session is None:  # noqa: SLF001
                        raise AssertionError("session missing before logout")
                    port._session.returncode = 0  # type: ignore[attr-defined]  # noqa: SLF001
                return result

            with patch.object(
                live_xorg, "capture_window_fingerprint", return_value=b"M" * 32
            ) as capture, patch.object(port, "_xdotool", side_effect=drive_tier_c_key):
                workflow = None
                for identifier in TIER_C_STEPS:
                    with self.subTest(identifier=identifier):
                        self.assertEqual(
                            port.run_step("tier_c_replay", identifier),
                            Observation(True, True),
                        )
                        if workflow is None:
                            workflow = port._tier_c_workflow  # noqa: SLF001
                        self.assertIs(port._tier_c_workflow, workflow)  # noqa: SLF001
                session_environment = next(
                    environment
                    for argv, environment in operations.spawn_calls
                    if Path(argv[0]).name == "prismdrake-session"
                )
                tier_c_configuration = (
                    Path(session_environment["XDG_CONFIG_HOME"])
                    / "prismdrake"
                    / "config.toml"
                )
                self.assertEqual(
                    tier_c_configuration.read_text(encoding="ascii"),
                    _session_config(reduced_motion=True),
                )
                self.assertEqual(
                    port.cleanup("tier_c_replay"), Observation(True, True)
                )

            self.assertEqual(len(traces), 1)
            self.assertEqual(traces[0].finish_calls, [True])
            self.assertTrue(traces[0].finished)
            trace_index = operations.events.index(("trace", ":1140"))
            self.assertEqual(operations.events[trace_index + 1], ("spawn", "openbox"))
            traced_spawns = [
                environment
                for (argv, environment) in operations.spawn_calls
                if Path(argv[0]).name in {"openbox", "prismdrake-session"}
            ]
            self.assertEqual(
                [environment["DISPLAY"] for environment in traced_spawns],
                [":140", ":1140"],
            )
            traced_commands = [
                environment["DISPLAY"]
                for argv, environment in operations.run_calls
                if Path(argv[0]).name in {"prismdrake-shellctl", "xdotool"}
            ]
            self.assertTrue(traced_commands)
            self.assertEqual(set(traced_commands), {":140"})
            self.assertIn(ProcessIdentity(3001, 30_010), operations.stop_identity_calls)
            self.assertEqual(capture.call_count, len(TIER_C_STEPS) * 2)
            self.assertIn("Desktop wallpaper", accessibility.absent)
            self.assertTrue(
                all(call.args[3]["DISPLAY"] == ":140" for call in capture.call_args_list)
            )
            key_calls = [
                argv[-1]
                for argv, _environment in operations.run_calls
                if argv[:3] == ("/usr/bin/xdotool", "key", "--clearmodifiers")
            ]
            expected_keys: list[str] = []
            for identifier in TIER_C_STEPS:
                if identifier == "task_actions":
                    expected_keys.append("Tab")
                expected_keys.extend(
                    live_xorg.TIER_C_KEY_ARGUMENTS[key]
                    for key in live_xorg.STEP_CONTRACTS[identifier].keys
                )
                if identifier in {"notification_center", "quick_settings"}:
                    expected_keys.append("Escape")
                if identifier == "launcher":
                    expected_keys.append("Tab")
            self.assertEqual(key_calls, expected_keys)

    def test_tier_c_skips_real_display_with_unavailable_proxy_pair(self) -> None:
        operations = FakeOperations()
        operations.unavailable_displays.add(1140)
        traces: list[FakeTierCTrace] = []

        def trace_factory(trace_config, environment):
            del environment
            self.assertEqual(trace_config.real_display, ":141")
            self.assertEqual(trace_config.fake_display, ":1141")
            trace = FakeTierCTrace(trace_config.fake_display)
            traces.append(trace)
            return trace

        port = LiveXorgHarnessPort(
            replace(config(), last_display=141),
            operations,
            tier_c_trace_factory=trace_factory,
        )
        port._start_environment(two_outputs=True, tier_c=True)  # noqa: SLF001

        self.assertEqual(port._display, ":141")  # noqa: SLF001
        self.assertEqual(port.cleanup("tier_c_replay"), Observation(True, True))
        self.assertEqual(len(traces), 1)
        self.assertTrue(traces[0].finished)

    def test_logout_methods_use_shellctl_accessibility_and_reject_stale_id(self) -> None:
        monitor_calls: list[object] = []

        class Monitor:
            def finish(self, transaction_id, timeout):
                monitor_calls.append(("finish", transaction_id, timeout))
                return SESSION_SHUTDOWN_SEQUENCE

            def close(self):
                monitor_calls.append("close")
                return True

        def monitor_factory(executable, address, environment, timeout):
            monitor_calls.append((executable, address, environment, timeout))
            return Monitor()

        port = LiveXorgHarnessPort(
            config(installed_paths()),
            FakeOperations(),
            session_transition_monitor_factory=monitor_factory,
        )
        components = LifecycleComponentObservation(
            LifecycleProcessIdentity("prismdrake-settingsd", 201, 2001),
            LifecycleProcessIdentity("prismdrake-notifyd", 202, 2002),
            LifecycleProcessIdentity("prismdrake-shell", 203, 2003),
        )

        with patch.object(port, "_invoke_shellctl") as shellctl, patch.object(
            port, "_wait_logout_state", return_value=(41, "confirmation_requested")
        ), patch.object(port, "_wait_accessible") as wait_accessible:
            requested = port.request_through_shellctl()
        self.assertEqual(
            (requested.transaction_id, requested.state), (41, "confirmation_pending")
        )
        shellctl.assert_called_once_with("session", "logout")
        self.assertEqual(wait_accessible.call_args.args[0].name, "Log out of Prismdrake")

        with patch.object(
            port,
            "_wait_logout_state",
            side_effect=((41, "confirmation_requested"), (0, "idle")),
        ), patch.object(port, "_invoke_accessible") as invoke:
            cancelled = port.cancel(41)
        self.assertEqual((cancelled.transaction_id, cancelled.state), (41, "cancelled"))
        self.assertEqual(invoke.call_args.args[0].name, "Cancel logout")

        class RejectingProbe:
            def call(self, operation, transaction):
                self.request = (operation, transaction)
                raise RuntimeError("redacted")

            def close(self):
                self.closed = True

        rejecting = RejectingProbe()
        with patch.object(port, "_private_probe", return_value=rejecting), patch.object(
            port, "_wait_logout_state", return_value=(0, "idle")
        ):
            self.assertTrue(port.reject_stale_transaction(41))
        self.assertEqual(rejecting.request, ("session_cancel_logout", 41))
        self.assertTrue(rejecting.closed)

        with patch.object(
            port,
            "_wait_logout_state",
            return_value=(42, "confirmation_requested"),
        ), patch.object(port, "components", return_value=components), patch.object(
            port, "_invoke_accessible"
        ) as invoke, patch.object(port, "_attest_private_bus"), patch.object(
            port, "_private_bus_address", return_value="unix:path=/private/bus"
        ), patch.object(
            port,
            "_private_bus_environment",
            return_value={"DBUS_SESSION_BUS_ADDRESS": "unix:path=/private/bus"},
        ):
            confirmed = port.confirm(42)
        self.assertEqual((confirmed.transaction_id, confirmed.state), (42, "stopping_shell"))
        self.assertEqual(invoke.call_args.args[0].name, "Confirm logout")
        self.assertEqual(port._lifecycle_shutdown_components, components)  # noqa: SLF001
        self.assertEqual(monitor_calls[1][0:2], ("finish", 42))
        self.assertGreaterEqual(monitor_calls.count("close"), 1)

    def test_routes_installed_lifecycle_steps_but_reserves_cleanup_for_adapter(self) -> None:
        port = LiveXorgHarnessPort(config(), FakeOperations())
        workflow = FakeLifecycleWorkflow()
        port._lifecycle_workflow = workflow  # noqa: SLF001 - injected hermetic port

        identifiers = tuple(LIFECYCLE_STEP_CATEGORIES)[:-1]
        with patch.object(port, "_focus_and_capture_reference"):
            for identifier in identifiers:
                self.assertEqual(
                    port.run_step("installed_workflow", identifier),
                    Observation(True, True),
                )
        self.assertEqual(workflow.calls, list(identifiers))
        with self.assertRaisesRegex(
            HarnessError, "^lifecycle_cleanup_step_is_adapter_owned$"
        ):
            port.run_step(
                "installed_workflow", "verify_clean_test_environment_cleanup"
            )
        self.assertEqual(
            port.cleanup("installed_workflow"), Observation(True, True)
        )
        self.assertEqual(workflow.calls, list(LIFECYCLE_STEP_CATEGORIES))

    def test_lifecycle_fixture_dismisses_quick_settings_before_launcher(self) -> None:
        port = LiveXorgHarnessPort(config(installed_paths()), FakeOperations())
        with patch.object(port, "_xdotool") as xdotool, patch.object(
            port, "_wait_accessible_absent"
        ) as wait_absent, patch.object(
            port,
            "_invoke_shellctl",
            side_effect=HarnessError("stop_after_dismissal"),
        ):
            with self.assertRaisesRegex(HarnessError, "^stop_after_dismissal$"):
                port.prepare_survival_fixture()

        xdotool.assert_called_once_with("key", "--clearmodifiers", "Escape")
        selector, timeout_error = wait_absent.call_args.args
        self.assertEqual((selector.name, selector.role), ("Quick settings", "pane"))
        self.assertEqual(timeout_error, "lifecycle_quick_settings_dismissal_timeout")

    def test_lifecycle_fixture_restores_reviewed_quick_settings_surface(self) -> None:
        port = LiveXorgHarnessPort(config(installed_paths()), FakeOperations())
        with patch.object(port, "_invoke_shellctl") as shellctl, patch.object(
            port, "_wait_accessible"
        ) as wait_accessible, patch.object(port, "surface_placements") as placements:
            port._restore_lifecycle_quick_settings()  # noqa: SLF001

        shellctl.assert_called_once_with("quick-settings", "open")
        selector, timeout_error = wait_accessible.call_args.args
        self.assertEqual((selector.name, selector.role), ("Quick settings", "pane"))
        self.assertEqual(timeout_error, "lifecycle_quick_settings_restore_timeout")
        placements.assert_called_once_with()

    def test_settings_epoch_restores_surface_after_replacement_snapshot(self) -> None:
        operations = FakeOperations()
        port = LiveXorgHarnessPort(
            config(installed_paths()), operations, FakeAccessibility(operations)
        )
        for identifier in (
            "start_isolated_two_output_x11",
            "start_installed_prismdrake_session",
            "verify_component_readiness",
        ):
            port.run_step("installed_workflow", identifier)
        events: list[str] = []

        class Snapshot:
            payload = (7, {"generation": 7})

        class Probe:
            def call(self, operation, version):
                self.request = (operation, version)
                events.append("snapshot")
                return Snapshot()

            def close(self):
                events.append("snapshot_closed")

        def capture(_adapter, surface, _points):
            events.append(f"capture:{surface}")
            fingerprint = bytes([len(events)]) * 32
            return fingerprint, fingerprint

        with patch.object(port, "_private_probe", return_value=Probe()), patch.object(
            port,
            "_invoke_shellctl",
            side_effect=lambda *arguments: events.append(
                "shellctl:" + ":".join(arguments)
            ),
        ), patch.object(
            port,
            "_wait_accessible",
            side_effect=lambda *_arguments: events.append("panel_accessible"),
        ), patch.object(
            port,
            "_restore_lifecycle_quick_settings",
            side_effect=lambda: events.append("restore"),
        ), patch.object(
            LiveSettingsSurfaceAdapter,
            "capture_settled_fingerprints",
            autospec=True,
            side_effect=capture,
        ):
            epoch = port.settings_epoch()

        self.assertTrue(epoch.complete_typed_snapshot)
        self.assertEqual(epoch.generation, 7)
        self.assertEqual(
            events,
            [
                "snapshot",
                "snapshot_closed",
                "shellctl:panel:focus",
                "panel_accessible",
                "restore",
                "capture:panel_primary",
                "capture:panel_secondary",
                "capture:quick_settings",
            ],
        )
        self.assertTrue(all(item.accessible for item in epoch.reviewed_surfaces))
        self.assertEqual(
            port.cleanup("installed_workflow"), Observation(True, True)
        )

    def test_output_migration_retains_quick_settings_focus_and_navigation(self) -> None:
        operations = FakeOperations()
        accessibility = FakeAccessibility(operations)
        port = LiveXorgHarnessPort(
            config(installed_paths()), operations, accessibility
        )
        for identifier in (
            "start_isolated_two_output_x11",
            "start_installed_prismdrake_session",
            "verify_component_readiness",
        ):
            port.run_step("installed_workflow", identifier)

        port._capture_output_migration_reference()  # noqa: SLF001
        port._verify_output_migration_accessibility()  # noqa: SLF001

        self.assertEqual(
            [
                argv[-1]
                for argv, _environment in operations.run_calls
                if argv[:3] == ("/usr/bin/xdotool", "key", "--clearmodifiers")
            ],
            ["Tab", "shift+Tab"],
        )
        self.assertIn("Use Prismdrake Lustre", accessibility.observed)
        self.assertIn("Use Prismdrake Forge", accessibility.observed)
        self.assertEqual(port.cleanup("installed_workflow"), Observation(True, True))

    def test_starts_real_adapter_commands_and_observes_two_output_topology(self) -> None:
        operations = FakeOperations()
        port = LiveXorgHarnessPort(config(), operations)

        result = port.run_step("installed_workflow", "start_isolated_two_output_x11")

        self.assertEqual(result, Observation(True, True))
        self.assertEqual(len(operations.spawn_calls), 2)
        xorg_argv, xorg_env = operations.spawn_calls[0]
        openbox_argv, openbox_env = operations.spawn_calls[1]
        self.assertEqual(xorg_argv[:2], ("/usr/bin/Xorg", ":140"))
        self.assertIn("-nolisten", xorg_argv)
        self.assertEqual(openbox_argv, ("/usr/bin/openbox", "--sm-disable"))
        self.assertNotIn("DISPLAY", xorg_env)
        self.assertEqual(openbox_env["DISPLAY"], ":140")
        self.assertTrue(
            any(
                argv[-7:] == (
                    "--output", "DUMMY0", "--mode", "1600x900",
                    "--pos", "0x0", "--primary",
                )
                for argv, _env in operations.run_calls
            )
        )
        self.assertTrue(
            any(
                argv[-13:] == (
                    "--newmode", "1600x900-pd2-secondary", "118.25",
                    "1600", "1696", "1856", "2112",
                    "900", "903", "908", "934", "-hsync", "+vsync",
                )
                for argv, _env in operations.run_calls
            )
        )
        self.assertTrue(
            any(
                argv[-3:] == (
                    "--addmode", "DUMMY1", "1600x900-pd2-secondary",
                )
                for argv, _env in operations.run_calls
            )
        )
        self.assertTrue(
            any(
                argv[-6:] == (
                    "--output", "DUMMY1", "--mode", "1600x900-pd2-secondary",
                    "--pos", "1600x0",
                )
                for argv, _env in operations.run_calls
            )
        )

        cleanup = port.cleanup("installed_workflow")
        self.assertEqual(cleanup, Observation(True, True))
        self.assertEqual(operations.stop_calls, list(reversed(operations.children)))

    def test_unsupported_step_and_unstarted_process_attestation_fail_closed(self) -> None:
        operations = FakeOperations()
        port = LiveXorgHarnessPort(config(), operations)

        with self.assertRaisesRegex(HarnessError, "^live_step_not_implemented$"):
            port.run_step("installed_workflow", "private_unreviewed_step")
        with self.assertRaisesRegex(
            HarnessError, "^live_process_ownership_unavailable$"
        ):
            port.capture_process_ownership()
        self.assertEqual(operations.spawn_calls, [])

    def test_starts_exact_installed_session_and_attests_distinct_owners(self) -> None:
        operations = FakeOperations()
        executables = installed_paths()
        port = LiveXorgHarnessPort(config(executables), operations)

        port.run_step("installed_workflow", "start_isolated_two_output_x11")
        self.assertEqual(
            port.run_step("installed_workflow", "start_installed_prismdrake_session"),
            Observation(True, True),
        )
        self.assertEqual(
            port.run_step("installed_workflow", "verify_component_readiness"),
            Observation(True, True),
        )

        bus_argv, bus_environment = operations.spawn_calls[2]
        session_argv, session_environment = operations.spawn_calls[3]
        self.assertEqual(
            bus_argv[:4],
            (
                "/usr/bin/dbus-daemon",
                "--session",
                "--nofork",
                "--nopidfile",
            ),
        )
        self.assertEqual(len(bus_argv), 5)
        self.assertRegex(
            bus_argv[4], r"^--address=unix:path=/tmp/prismdrake-pd2-.+/bus$"
        )
        self.assertNotIn("DISPLAY", bus_environment)
        self.assertNotIn("DBUS_SESSION_BUS_ADDRESS", bus_environment)
        self.assertEqual(
            session_argv,
            (
                "/usr/bin/prismdrake-session",
                "--settingsd",
                "/usr/bin/prismdrake-settingsd",
                "--notifyd",
                "/usr/bin/prismdrake-notifyd",
                "--shell",
                "/usr/bin/prismdrake-shell",
            ),
        )
        self.assertEqual(session_environment["QT_QPA_PLATFORM"], "xcb")
        self.assertEqual(session_environment["QT_QUICK_BACKEND"], "software")
        self.assertEqual(
            session_environment["DBUS_SESSION_BUS_ADDRESS"],
            bus_argv[4].removeprefix("--address="),
        )
        self.assertEqual(
            port.probe_environment(),
            {
                key: value
                for key, value in session_environment.items()
                if key
                not in {
                    "QT_QPA_PLATFORM",
                    "QT_QUICK_BACKEND",
                    "QT_QUICK_CONTROLS_STYLE",
                    "QT_LINUX_ACCESSIBILITY_ALWAYS_ON",
                }
            },
        )
        ownership = port.capture_process_ownership()
        self.assertEqual(
            tuple(ownership),
            (
                "xorg_dummy_server",
                "openbox_window_manager",
                "isolated_session_bus",
                "prismdrake_session",
            ),
        )
        self.assertEqual(len(set(ownership.values())), 4)

        self.assertEqual(port.cleanup("installed_workflow"), Observation(True, True))
        self.assertEqual(operations.stop_calls, list(reversed(operations.children)))

    def test_keeps_one_attested_notification_epoch_across_steps_17_to_24(self) -> None:
        operations = FakeOperations()
        accessibility = FakeAccessibility(operations)
        factory = FakeNotificationEpochFactory()
        port = LiveXorgHarnessPort(
            config(installed_paths()), operations, accessibility, factory
        )
        for identifier in (
            "start_isolated_two_output_x11",
            "start_installed_prismdrake_session",
            "verify_component_readiness",
        ):
            port.run_step("installed_workflow", identifier)
        self.assertEqual(factory.calls, [])

        identifiers = (
            "create_normal_notification",
            "create_replacement_notification",
            "invoke_live_notification_action",
            "open_notification_center",
            "toggle_do_not_disturb",
            "verify_history_without_transient",
            "verify_critical_bypass_policy",
            "clear_notification_history",
        )
        for identifier in identifiers:
            self.assertEqual(
                port.run_step("installed_workflow", identifier),
                Observation(True, True),
            )

        self.assertEqual(len(factory.calls), 1)
        environment, endpoint, surface = factory.calls[0]
        self.assertEqual(environment["DISPLAY"], ":140")
        self.assertEqual(
            environment["DBUS_SESSION_BUS_ADDRESS"], endpoint.address
        )
        self.assertEqual((endpoint.socket_device, endpoint.socket_inode), (1, 100))
        self.assertIsInstance(surface, LiveNotificationSurfaceAdapter)
        self.assertEqual(
            factory.workflow.calls,
            [
                "create_normal",
                "replace_normal",
                "invoke_live_action",
                "open_center",
                "enable_do_not_disturb",
                "create_suppressed_normal",
                "create_critical_bypass",
                "clear_history",
            ],
        )

        self.assertEqual(port.cleanup("installed_workflow"), Observation(True, True))
        self.assertEqual(factory.sender.close_calls, 1)
        self.assertEqual(factory.dbus_probe.close_calls, 1)

    def test_notification_epoch_initialization_failure_is_categorical(self) -> None:
        operations = FakeOperations()

        def fail_factory(environment, endpoint, surface):
            del environment, endpoint, surface
            raise RuntimeError("private detail")

        port = LiveXorgHarnessPort(
            config(installed_paths()),
            operations,
            FakeAccessibility(operations),
            fail_factory,
        )
        for identifier in (
            "start_isolated_two_output_x11",
            "start_installed_prismdrake_session",
            "verify_component_readiness",
        ):
            port.run_step("installed_workflow", identifier)

        with self.assertRaisesRegex(
            HarnessError, "^live_notification_epoch_initialization_failed$"
        ):
            port.run_step("installed_workflow", "create_normal_notification")
        self.assertEqual(port.cleanup("installed_workflow"), Observation(True, True))

    def test_keeps_one_attested_settings_epoch_across_steps_25_to_28(self) -> None:
        operations = FakeOperations()
        factory = FakeSettingsEpochFactory()
        port = LiveXorgHarnessPort(
            config(installed_paths()),
            operations,
            FakeAccessibility(operations),
            None,
            factory,
        )
        for identifier in (
            "start_isolated_two_output_x11",
            "start_installed_prismdrake_session",
            "verify_component_readiness",
        ):
            port.run_step("installed_workflow", identifier)
        self.assertEqual(factory.calls, [])

        identifiers = tuple(SETTINGS_RESULT_CATEGORIES)
        for identifier in identifiers:
            self.assertEqual(
                port.run_step("installed_workflow", identifier),
                Observation(True, True),
            )

        self.assertEqual(len(factory.calls), 1)
        environment, endpoint, accessibility, surfaces = factory.calls[0]
        self.assertEqual(environment["DISPLAY"], ":140")
        self.assertEqual(environment["DBUS_SESSION_BUS_ADDRESS"], endpoint.address)
        self.assertEqual((endpoint.socket_device, endpoint.socket_inode), (1, 100))
        self.assertIsInstance(accessibility, LiveSettingsAccessibilityAdapter)
        self.assertIsInstance(surfaces, LiveSettingsSurfaceAdapter)
        self.assertEqual(factory.workflow.calls, list(identifiers))
        self.assertEqual(
            surfaces.reviewed_surface_sequence(),
            ("panel_primary", "panel_secondary", "quick_settings"),
        )

        self.assertEqual(port.cleanup("installed_workflow"), Observation(True, True))
        self.assertEqual(factory.dbus_probe.close_calls, 1)

    def test_settings_epoch_initialization_failure_is_categorical(self) -> None:
        operations = FakeOperations()

        def fail_factory(environment, endpoint, accessibility, surfaces):
            del environment, endpoint, accessibility, surfaces
            raise RuntimeError("private detail")

        port = LiveXorgHarnessPort(
            config(installed_paths()),
            operations,
            FakeAccessibility(operations),
            None,
            fail_factory,
        )
        for identifier in (
            "start_isolated_two_output_x11",
            "start_installed_prismdrake_session",
            "verify_component_readiness",
        ):
            port.run_step("installed_workflow", identifier)

        with self.assertRaisesRegex(
            HarnessError, "^live_settings_epoch_initialization_failed$"
        ):
            port.run_step("installed_workflow", "open_quick_settings")
        self.assertEqual(port.cleanup("installed_workflow"), Observation(True, True))

    def test_observes_two_coherent_wallpaper_desktops_and_bottom_panels(self) -> None:
        operations = FakeOperations()
        port = LiveXorgHarnessPort(config(installed_paths()), operations)
        for identifier in (
            "start_isolated_two_output_x11",
            "start_installed_prismdrake_session",
            "verify_component_readiness",
        ):
            port.run_step("installed_workflow", identifier)

        self.assertEqual(
            port.run_step(
                "installed_workflow", "verify_desktop_wallpaper_both_outputs"
            ),
            Observation(True, True),
        )
        self.assertEqual(
            port.run_step("installed_workflow", "verify_panel_policy_both_outputs"),
            Observation(True, True),
        )

        _argv, environment = operations.spawn_calls[3]
        configuration = (
            Path(environment["XDG_CONFIG_HOME"]) / "prismdrake" / "config.toml"
        )
        self.assertEqual(stat.S_IMODE(configuration.stat().st_mode), 0o600)
        self.assertEqual(
            configuration.read_text(encoding="ascii"),
            _session_config("persistent"),
        )
        geometry_queries = [
            argv
            for argv, _environment in operations.run_calls
            if argv[0] == "/usr/bin/xwininfo"
        ]
        self.assertEqual(len(geometry_queries), 6)
        self.assertEqual(port.cleanup("installed_workflow"), Observation(True, True))

    def test_resolves_only_reviewed_settings_windows_at_fixed_geometry(self) -> None:
        operations = FakeOperations()
        quick_window = 0x500005
        quick_geometry = WindowGeometry(1220, 452, 380, 400)
        operations.controlled_windows[QUICK_SETTINGS_WINDOW_TITLE] = quick_window
        operations.surface_geometries[quick_window] = quick_geometry
        operations.window_geometry_payloads[quick_window] = xwininfo_payload(
            quick_window, quick_geometry
        )
        port = LiveXorgHarnessPort(config(installed_paths()), operations)
        for identifier in (
            "start_isolated_two_output_x11",
            "start_installed_prismdrake_session",
            "verify_component_readiness",
            "verify_desktop_wallpaper_both_outputs",
            "verify_panel_policy_both_outputs",
        ):
            port.run_step("installed_workflow", identifier)

        self.assertEqual(port._settings_surface_window("panel_primary"), PANEL_LEFT)
        self.assertEqual(
            port._settings_surface_window("panel_secondary"), PANEL_RIGHT
        )
        self.assertEqual(
            port._settings_surface_window("quick_settings"), quick_window
        )

        port._desktop_surfaces = None  # noqa: SLF001
        port._surface_shell_identity = None  # noqa: SLF001
        self.assertEqual(port._settings_surface_window("panel_primary"), PANEL_LEFT)
        self.assertIsNotNone(port._desktop_surfaces)  # noqa: SLF001
        self.assertIsNotNone(port._surface_shell_identity)  # noqa: SLF001

        operations.window_geometry_payloads[quick_window] = xwininfo_payload(
            quick_window, WindowGeometry(1219, 452, 380, 400)
        )
        with self.assertRaisesRegex(
            HarnessError, "^live_settings_surface_window_invalid$"
        ):
            port._settings_surface_window("quick_settings")
        self.assertEqual(port.cleanup("installed_workflow"), Observation(True, True))

    def test_runs_controlled_launcher_pin_and_two_window_panel_lifecycle(self) -> None:
        operations = FakeOperations()
        operations.visible_named_windows[live_xorg.LAUNCHER_WINDOW_TITLE] = 0x510001
        accessibility = FakeAccessibility(operations)
        port = LiveXorgHarnessPort(
            config(installed_paths()), operations, accessibility
        )
        identifiers = (
            "start_isolated_two_output_x11",
            "start_installed_prismdrake_session",
            "verify_component_readiness",
            "verify_desktop_wallpaper_both_outputs",
            "verify_panel_policy_both_outputs",
            "invoke_launcher_through_shellctl",
            "search_controlled_application",
            "pin_controlled_application",
            "launch_controlled_application",
            "launch_second_window",
            "verify_task_grouping",
            "activate_one_window",
            "minimize_one_window",
            "reactivate_one_window",
            "close_both_windows_through_panel",
            "verify_authoritative_task_removal",
        )

        acknowledgements: list[int] = []
        common_actions = {
            "activate_one_window",
            "minimize_one_window",
            "reactivate_one_window",
        }
        for identifier in identifiers:
            with self.subTest(identifier=identifier):
                self.assertEqual(
                    port.run_step("installed_workflow", identifier),
                    Observation(True, True),
                )
                if identifier in common_actions:
                    acknowledgements.append(
                        port.common_action_acknowledgement_ns(identifier)
                    )

        self.assertEqual(acknowledgements, [0, 0, 0])
        for identifier in (*common_actions, "not-reviewed"):
            with self.subTest(unavailable=identifier), self.assertRaisesRegex(
                HarnessError, "^common_action_acknowledgement_unavailable$"
            ):
                port.common_action_acknowledgement_ns(identifier)

        shellctl_calls = [
            argv
            for argv, _environment in operations.run_calls
            if argv[0] == "/usr/bin/prismdrake-shellctl"
        ]
        self.assertEqual(
            shellctl_calls,
            [
                ("/usr/bin/prismdrake-shellctl", "launcher", "toggle"),
                ("/usr/bin/prismdrake-shellctl", "launcher", "toggle"),
                ("/usr/bin/prismdrake-shellctl", "panel", "focus"),
                ("/usr/bin/prismdrake-shellctl", "panel", "focus"),
                ("/usr/bin/prismdrake-shellctl", "panel", "focus"),
                ("/usr/bin/prismdrake-shellctl", "panel", "focus"),
                ("/usr/bin/prismdrake-shellctl", "panel", "focus"),
            ],
        )
        self.assertTrue(accessibility.pinned)
        self.assertEqual(operations.controlled_windows, {})
        self.assertIn("Pin to panel", accessibility.invoked)
        self.assertIn("Second window", accessibility.invoked)
        self.assertIn(
            f"Close {CONTROLLED_APPLICATION_NAME}", accessibility.invoked
        )
        launcher_focus_calls = [
            argv
            for argv, _environment in operations.run_calls
            if argv[:3] == ("/usr/bin/xdotool", "windowfocus", "--sync")
        ]
        self.assertEqual(
            launcher_focus_calls,
            [
                ("/usr/bin/xdotool", "windowfocus", "--sync", "5308417"),
                ("/usr/bin/xdotool", "windowfocus", "--sync", "5308417"),
            ],
        )
        self.assertEqual(
            [
                argv[-1]
                for argv, _environment in operations.run_calls
                if argv[:3] == ("/usr/bin/xdotool", "key", "--clearmodifiers")
            ],
            [
                "Down",
                "Shift_L+F10",
                "Shift_L+F10",
                "Escape",
                "Down",
                "Shift_L+F10",
                "Tab",
                "Tab",
                "Tab",
                "Tab",
                "Tab",
                "Shift_L+F10",
            ],
        )
        self.assertEqual(port.cleanup("installed_workflow"), Observation(True, True))

    def test_accessibility_metadata_starts_from_surface_owned_focus(self) -> None:
        operations = FakeOperations()
        accessibility = FakeAccessibility(operations)
        port = LiveXorgHarnessPort(
            config(installed_paths()), operations, accessibility
        )
        for identifier in (
            "start_isolated_two_output_x11",
            "start_installed_prismdrake_session",
            "verify_component_readiness",
            "verify_accessible_descriptions_states_and_backtab",
        ):
            port.run_step("installed_workflow", identifier)

        keys = [
            argv[-1]
            for argv, _environment in operations.run_calls
            if argv[:3] == ("/usr/bin/xdotool", "key", "--clearmodifiers")
        ]
        self.assertEqual(keys, ["Escape", "shift+Tab", "space"])
        self.assertIn("Use Prismdrake Lustre", accessibility.observed)
        self.assertIn("Open quick settings", accessibility.observed)
        self.assertIn(("Do not disturb", "CHECKED", False), accessibility.states)
        self.assertEqual(port.cleanup("installed_workflow"), Observation(True, True))

    def test_window_state_parser_accepts_only_exact_empty_or_known_atoms(self) -> None:
        operations = FakeOperations()
        port = LiveXorgHarnessPort(config(), operations)
        port._display = ":140"  # noqa: SLF001
        window = CONTROLLED_ONE_ID

        operations.window_state_payloads[window] = b"_NET_WM_STATE(ATOM) = \n"
        self.assertEqual(port._window_state_atoms(window), frozenset())  # noqa: SLF001
        operations.window_state_payloads[window] = (
            b"_NET_WM_STATE(ATOM) = _NET_WM_STATE_MAXIMIZED_VERT, "
            b"_NET_WM_STATE_MAXIMIZED_HORZ\n"
        )
        self.assertEqual(
            port._window_state_atoms(window),  # noqa: SLF001
            frozenset(
                {
                    b"_NET_WM_STATE_MAXIMIZED_VERT",
                    b"_NET_WM_STATE_MAXIMIZED_HORZ",
                }
            ),
        )

        malformed = (
            b"_NET_WM_STATE(ATOM) =  \n",
            b"_NET_WM_STATE(ATOM) = WM_STATE_HIDDEN\n",
            b"_NET_WM_STATE(ATOM) = _NET_WM_STATE_PRIVATE\n",
            b"_NET_WM_STATE(ATOM) = _NET_WM_STATE_HIDDEN, \n",
            b"_NET_WM_STATE(ATOM) = _NET_WM_STATE_HIDDEN, _NET_WM_STATE_HIDDEN\n",
        )
        for payload in malformed:
            with self.subTest(payload=payload):
                operations.window_state_payloads[window] = payload
                with self.assertRaisesRegex(
                    HarnessError, "^controlled_window_state_invalid$"
                ):
                    port._window_state_atoms(window)  # noqa: SLF001

    def test_surface_observation_fails_closed_for_owner_focus_geometry_and_strut(self) -> None:
        cases = (
            ("owner", "surface_window_owner_invalid"),
            ("focus", "surface_window_focus_policy_invalid"),
            ("geometry", "x11_window_geometry_invalid"),
            ("strut", "panel_strut_invalid"),
        )
        for mutation, expected in cases:
            with self.subTest(mutation=mutation):
                operations = FakeOperations()
                port = LiveXorgHarnessPort(config(installed_paths()), operations)
                for identifier in (
                    "start_isolated_two_output_x11",
                    "start_installed_prismdrake_session",
                    "verify_component_readiness",
                ):
                    port.run_step("installed_workflow", identifier)
                if mutation == "owner":
                    operations.surface_owner += 1
                elif mutation == "focus":
                    operations.surface_focus = True
                elif mutation == "geometry":
                    operations.window_geometry_payloads[DESKTOP_LEFT] = b"unmapped\n"
                else:
                    port.run_step(
                        "installed_workflow", "verify_desktop_wallpaper_both_outputs"
                    )
                    operations.panel_strut_bottom = 47
                identifier = (
                    "verify_panel_policy_both_outputs"
                    if mutation == "strut"
                    else "verify_desktop_wallpaper_both_outputs"
                )
                with self.assertRaisesRegex(HarnessError, f"^{expected}$"):
                    port.run_step("installed_workflow", identifier)
                self.assertEqual(
                    port.cleanup("installed_workflow"), Observation(True, True)
                )

    def test_surface_observation_skips_exact_missing_type_for_stale_client(self) -> None:
        operations = FakeOperations()
        stale_window = 0x1000000
        operations.surface_list = (*operations.surface_list, stale_window)
        operations.missing_surface_types.add(stale_window)
        port = LiveXorgHarnessPort(config(installed_paths()), operations)
        for identifier in (
            "start_isolated_two_output_x11",
            "start_installed_prismdrake_session",
            "verify_component_readiness",
            "verify_desktop_wallpaper_both_outputs",
            "verify_panel_policy_both_outputs",
        ):
            self.assertEqual(
                port.run_step("installed_workflow", identifier),
                Observation(True, True),
            )
        self.assertEqual(port.cleanup("installed_workflow"), Observation(True, True))

    def test_panel_surface_observation_accepts_one_keyboard_focused_dock(self) -> None:
        operations = FakeOperations()
        port = LiveXorgHarnessPort(config(installed_paths()), operations)
        for identifier in (
            "start_isolated_two_output_x11",
            "start_installed_prismdrake_session",
            "verify_component_readiness",
            "verify_desktop_wallpaper_both_outputs",
        ):
            port.run_step("installed_workflow", identifier)
        operations.surface_focus_windows.add(PANEL_LEFT)

        self.assertEqual(
            port.run_step("installed_workflow", "verify_panel_policy_both_outputs"),
            Observation(True, True),
        )
        self.assertEqual(port.cleanup("installed_workflow"), Observation(True, True))

    def test_panel_observation_rejects_desktop_or_shell_epoch_change(self) -> None:
        for mutation, expected in (
            ("desktop", "desktop_surface_epoch_changed"),
            ("shell", "surface_shell_identity_changed"),
        ):
            with self.subTest(mutation=mutation):
                operations = FakeOperations()
                port = LiveXorgHarnessPort(config(installed_paths()), operations)
                for identifier in (
                    "start_isolated_two_output_x11",
                    "start_installed_prismdrake_session",
                    "verify_component_readiness",
                    "verify_desktop_wallpaper_both_outputs",
                ):
                    port.run_step("installed_workflow", identifier)
                if mutation == "desktop":
                    changed = WindowGeometry(0, 0, 1599, 900)
                    operations.window_geometry_payloads[DESKTOP_LEFT] = xwininfo_payload(
                        DESKTOP_LEFT, changed
                    )
                else:
                    operations.component_start_times["prismdrake-shell"] += 1
                with self.assertRaisesRegex(HarnessError, f"^{expected}$"):
                    port.run_step(
                        "installed_workflow", "verify_panel_policy_both_outputs"
                    )
                self.assertEqual(
                    port.cleanup("installed_workflow"), Observation(True, True)
                )

    def test_session_readiness_rejects_safe_mode(self) -> None:
        operations = FakeOperations(session_marker="safe-mode")
        port = LiveXorgHarnessPort(config(installed_paths()), operations)
        port.run_step("installed_workflow", "start_isolated_two_output_x11")
        port.run_step("installed_workflow", "start_installed_prismdrake_session")

        with self.assertRaisesRegex(
            HarnessError, "^prismdrake_session_safe_mode_unexpected$"
        ):
            port.run_step("installed_workflow", "verify_component_readiness")
        self.assertEqual(port.cleanup("installed_workflow"), Observation(True, True))

    def test_private_bus_fails_closed_for_early_exit_and_readiness_timeout(
        self,
    ) -> None:
        for operations, expected in (
            (
                FakeOperations(bus_exits_early=True),
                "private_session_bus_exited_before_readiness",
            ),
            (
                FakeOperations(bus_socket_ready=False),
                "private_session_bus_readiness_failed",
            ),
        ):
            with self.subTest(expected=expected):
                port = LiveXorgHarnessPort(config(installed_paths()), operations)
                port.run_step("installed_workflow", "start_isolated_two_output_x11")
                with self.assertRaisesRegex(HarnessError, f"^{expected}$"):
                    port.run_step(
                        "installed_workflow", "start_installed_prismdrake_session"
                    )
                self.assertEqual(
                    port.cleanup("installed_workflow"), Observation(True, True)
                )

    def test_private_bus_attestation_rejects_socket_replacement_and_owner_drift(
        self,
    ) -> None:
        for mutation, expected in (
            ("socket", "private_session_bus_socket_replaced"),
            ("owner", "private_session_bus_owner_changed"),
        ):
            with self.subTest(mutation=mutation):
                operations = FakeOperations()
                port = LiveXorgHarnessPort(config(installed_paths()), operations)
                port.run_step("installed_workflow", "start_isolated_two_output_x11")
                port.run_step("installed_workflow", "start_installed_prismdrake_session")
                if mutation == "socket":
                    operations.replace_bus_socket()
                else:
                    bus = operations.children[2]
                    operations.identity_offsets[bus.pid] = 1
                with self.assertRaisesRegex(HarnessError, f"^{expected}$"):
                    port.capture_process_ownership()
                self.assertEqual(
                    port.cleanup("installed_workflow"), Observation(True, True)
                )

    def test_session_readiness_rejects_replaced_marker(self) -> None:
        operations = FakeOperations()
        port = LiveXorgHarnessPort(config(installed_paths()), operations)
        port.run_step("installed_workflow", "start_isolated_two_output_x11")
        port.run_step("installed_workflow", "start_installed_prismdrake_session")
        _argv, environment = operations.spawn_calls[3]
        ready = (
            Path(environment["XDG_RUNTIME_DIR"])
            / "prismdrake"
            / "session-1003-0"
            / "ready"
        )
        ready.unlink()
        os.symlink("/dev/null", ready)

        with self.assertRaisesRegex(
            HarnessError, "^prismdrake_session_runtime_invalid$"
        ):
            port.run_step("installed_workflow", "verify_component_readiness")
        self.assertEqual(port.cleanup("installed_workflow"), Observation(True, True))

    def test_consecutive_failure_scenarios_start_fresh_topology_epochs(self) -> None:
        operations = FakeOperations(
            query_payloads=(TWO_OUTPUTS, TWO_OUTPUTS, ONE_OUTPUT, ONE_OUTPUT, TWO_OUTPUTS)
        )
        port = LiveXorgHarnessPort(config(), operations)

        first = port.run_step("failure_replay", "two_same_scale_outputs_at_startup")
        second = port.run_step("failure_replay", "add_second_output_after_startup")

        self.assertEqual(first, Observation(True, True))
        self.assertEqual(second, Observation(True, True))
        self.assertEqual(len(operations.spawn_calls), 4)
        self.assertEqual(len(operations.stop_calls), 2)
        self.assertEqual(port.cleanup("failure_replay"), Observation(True, True))
        self.assertEqual(len(operations.stop_calls), 4)

    def test_cleanup_fails_when_an_owned_exact_child_cannot_be_stopped(self) -> None:
        operations = FakeOperations()
        port = LiveXorgHarnessPort(config(), operations)
        port.run_step("installed_workflow", "start_isolated_two_output_x11")
        operations.stop_succeeds = False

        with self.assertRaisesRegex(HarnessError, "^live_cleanup_failed$"):
            port.cleanup("installed_workflow")
        self.assertEqual(len(operations.stop_calls), 2)

    def test_output_and_process_failure_scenarios_use_live_observations(self) -> None:
        cases = (
            ("remove_primary_output", (TWO_OUTPUTS, TWO_OUTPUTS, ONLY_SECONDARY)),
            ("select_new_primary_output", (TWO_OUTPUTS, SECONDARY_PRIMARY)),
            ("change_output_geometry", (TWO_OUTPUTS, SECONDARY_BELOW)),
        )
        for identifier, payloads in cases:
            with self.subTest(identifier=identifier):
                operations = FakeOperations(query_payloads=payloads)
                port = LiveXorgHarnessPort(config(), operations)
                self.assertEqual(
                    port.run_step("failure_replay", identifier),
                    Observation(True, True),
                )
                self.assertEqual(
                    port.cleanup("failure_replay"), Observation(True, True)
                )

    def test_service_shell_and_window_manager_restarts_preserve_exact_owners(self) -> None:
        cases = (
            ("restart_settingsd", "prismdrake-settingsd"),
            ("restart_notifyd", "prismdrake-notifyd"),
            ("restart_shell", "prismdrake-shell"),
            ("replace_or_restart_window_manager", None),
        )
        for identifier, restarted in cases:
            with self.subTest(identifier=identifier):
                operations = FakeOperations()
                accessibility = FakeAccessibility(operations)
                before = dict(operations.component_start_times)
                port = LiveXorgHarnessPort(
                    config(installed_paths()), operations, accessibility
                )

                self.assertEqual(
                    port.run_step("failure_replay", identifier),
                    Observation(True, True),
                )
                self.assertEqual(
                    operations.controlled_windows,
                    {CONTROLLED_WINDOW_ONE: CONTROLLED_ONE_ID},
                )
                for role, start_time in before.items():
                    expected = start_time + (1 if role == restarted else 0)
                    self.assertEqual(operations.component_start_times[role], expected)
                if identifier == "replace_or_restart_window_manager":
                    self.assertEqual(len(operations.spawn_calls), 5)
                    self.assertEqual(len(operations.stop_calls), 1)
                    self.assertEqual(operations.fail_identity_calls, [])
                else:
                    old = ProcessIdentity(
                        operations.component_pids[restarted], before[restarted]
                    )
                    self.assertEqual(operations.fail_identity_calls, [old])
                    self.assertNotIn(old, operations.stop_identity_calls)
                if identifier == "restart_notifyd":
                    self.assertIn("Notification center", accessibility.observed)
                if identifier == "restart_shell":
                    self.assertTrue(accessibility.pinned)
                    self.assertIn(CONTROLLED_APPLICATION_NAME, accessibility.observed)
                self.assertEqual(
                    port.cleanup("failure_replay"), Observation(True, True)
                )

    def test_lifecycle_restart_uses_exact_failure_invalidation_boundary(self) -> None:
        operations = FakeOperations()
        accessibility = FakeAccessibility(operations)
        port = LiveXorgHarnessPort(
            config(installed_paths()), operations, accessibility
        )
        port.run_step("failure_replay", "restart_settingsd")
        before = port.components()

        restarted = port.restart_exact(before.settingsd)

        old = ProcessIdentity(before.settingsd.pid, before.settingsd.start_time)
        self.assertTrue(restarted.dependent_invalidated)
        self.assertEqual(operations.fail_identity_calls[-1], old)
        self.assertNotIn(old, operations.stop_identity_calls)

        quick_window = 0x500005
        quick_geometry = WindowGeometry(1220, 452, 380, 400)
        operations.controlled_windows[QUICK_SETTINGS_WINDOW_TITLE] = quick_window
        operations.surface_geometries[quick_window] = quick_geometry
        operations.window_geometry_payloads[quick_window] = xwininfo_payload(
            quick_window, quick_geometry
        )
        before = port.components()
        prior_commands = len(operations.run_calls)
        restarted_notifyd = port.restart_exact(before.notifyd)
        notifyd_old = ProcessIdentity(before.notifyd.pid, before.notifyd.start_time)
        self.assertTrue(restarted_notifyd.dependent_invalidated)
        self.assertEqual(operations.fail_identity_calls[-1], notifyd_old)
        self.assertEqual(
            accessibility.states[-3:],
            [
                ("Do not disturb", "ENABLED", True),
                ("Do not disturb", "ENABLED", False),
                ("Do not disturb", "ENABLED", True),
            ],
        )
        restart_commands = operations.run_calls[prior_commands:]
        self.assertFalse(
            any(Path(argv[0]).name == "prismdrake-shellctl" for argv, _env in restart_commands)
        )
        self.assertTrue(port._lifecycle_shell_notifyd_available)  # noqa: SLF001
        self.assertEqual(
            port.cleanup("failure_replay"), Observation(True, True)
        )

    def test_component_restart_fails_closed_for_signal_timeout_and_sibling_drift(self) -> None:
        cases = (
            ("signal", "component_restart_signal_failed"),
            ("timeout", "component_restart_timeout"),
            ("sibling", "unrelated_component_replaced_during_restart"),
        )
        for mutation, expected in cases:
            with self.subTest(mutation=mutation):
                operations = FakeOperations()
                if mutation == "signal":
                    operations.component_stop_succeeds = False
                elif mutation == "timeout":
                    operations.component_replacement_published = False
                else:
                    operations.unrelated_replacement = "prismdrake-shell"
                port = LiveXorgHarnessPort(
                    config(installed_paths()),
                    operations,
                    FakeAccessibility(operations),
                )

                with self.assertRaisesRegex(HarnessError, f"^{expected}$"):
                    port.run_step("failure_replay", "restart_settingsd")
                self.assertEqual(
                    port.cleanup("failure_replay"), Observation(True, True)
                )

    def test_fresh_display_blur_and_corrupt_file_fallbacks_use_private_live_state(self) -> None:
        for identifier in (
            "run_without_compositor",
            "run_with_missing_blur_capability",
            "start_with_invalid_wallpaper",
            "start_with_malformed_pin_state",
        ):
            with self.subTest(identifier=identifier):
                operations = FakeOperations()
                accessibility = FakeAccessibility(operations)
                port = LiveXorgHarnessPort(
                    config(installed_paths()), operations, accessibility
                )

                self.assertEqual(
                    port.run_step("failure_replay", identifier),
                    Observation(True, True),
                )
                self.assertEqual(
                    port.cleanup("failure_replay"), Observation(True, True)
                )

    def test_invalid_wallpaper_fallback_requires_mapped_desktop_surfaces(self) -> None:
        operations = FakeOperations()
        operations.surface_types[DESKTOP_LEFT] = b"_NET_WM_WINDOW_TYPE_NORMAL"
        port = LiveXorgHarnessPort(config(installed_paths()), operations)

        with self.assertRaisesRegex(HarnessError, "^desktop_wallpaper_surface_invalid$"):
            port.run_step("failure_replay", "start_with_invalid_wallpaper")
        self.assertEqual(port.cleanup("failure_replay"), Observation(True, True))

    def test_desktop_surface_observation_waits_for_both_mapped_outputs(self) -> None:
        operations = FakeOperations()
        port = LiveXorgHarnessPort(config(installed_paths()), operations)
        for identifier in (
            "start_isolated_two_output_x11",
            "start_installed_prismdrake_session",
            "verify_component_readiness",
        ):
            port.run_step("installed_workflow", identifier)
        original = port._observe_surface_windows  # noqa: SLF001
        calls = 0

        def delayed(window_type, shell):
            nonlocal calls
            calls += 1
            observed = original(window_type, shell)
            if calls == 1:
                return {DESKTOP_LEFT: observed[DESKTOP_LEFT]}
            return observed

        with patch.object(port, "_observe_surface_windows", side_effect=delayed):
            port._require_desktop_wallpaper_surfaces()  # noqa: SLF001

        self.assertGreaterEqual(calls, 2)
        self.assertEqual(port.cleanup("installed_workflow"), Observation(True, True))

    def test_malformed_pin_recovery_requires_a_valid_private_replacement(self) -> None:
        operations = FakeOperations()
        accessibility = FakeAccessibility(operations)
        accessibility.persist_pin = False
        port = LiveXorgHarnessPort(
            config(installed_paths()), operations, accessibility
        )

        with self.assertRaisesRegex(
            HarnessError, "^malformed_pin_state_recovery_invalid$"
        ):
            port.run_step("failure_replay", "start_with_malformed_pin_state")
        self.assertEqual(port.cleanup("failure_replay"), Observation(True, True))

    def test_replays_malformed_persistent_history_in_one_usable_fresh_session(
        self,
    ) -> None:
        operations = FakeOperations()
        adapter = FakeNotificationFailureReplay()
        port = LiveXorgHarnessPort(
            config(installed_paths()),
            operations,
            FakeAccessibility(operations),
            notification_failure_replay=adapter,
        )

        self.assertEqual(
            port.run_step(
                "failure_replay", "start_with_malformed_persistent_history"
            ),
            Observation(True, True),
        )

        self.assertEqual(len(adapter.malformed_calls), 1)
        fixture, environment, endpoint = adapter.malformed_calls[0]
        self.assertEqual(fixture.path.read_bytes(), MALFORMED_HISTORY_BYTES)
        self.assertEqual(environment["DBUS_SESSION_BUS_ADDRESS"], endpoint.address)
        configuration = (
            Path(environment["XDG_CONFIG_HOME"]) / "prismdrake" / "config.toml"
        ).read_text(encoding="ascii")
        self.assertIn('history = "persistent"', configuration)
        self.assertNotIn('history = "session"', configuration)
        self.assertEqual(
            sum(
                Path(argv[0]).name == "prismdrake-session"
                for argv, _environment in operations.spawn_calls
            ),
            1,
        )
        self.assertTrue(
            any(
                argv == ("/usr/bin/prismdrake-shellctl", "notifications", "open")
                for argv, _environment in operations.run_calls
            )
        )
        self.assertEqual(port.cleanup("failure_replay"), Observation(True, True))

    def test_replays_owner_conflict_with_direct_exact_unchanged_siblings(self) -> None:
        operations = FakeOperations()
        adapter = FakeNotificationFailureReplay()
        port = LiveXorgHarnessPort(
            config(installed_paths()),
            operations,
            FakeAccessibility(operations),
            notification_failure_replay=adapter,
        )

        self.assertEqual(
            port.run_step(
                "failure_replay", "start_when_another_notification_owner_exists"
            ),
            Observation(True, True),
        )

        self.assertEqual(len(adapter.conflict_calls), 1)
        executable, environment, endpoint, siblings = adapter.conflict_calls[0]
        self.assertEqual(executable, Path("/usr/bin/prismdrake-notifyd"))
        self.assertEqual(environment["DBUS_SESSION_BUS_ADDRESS"], endpoint.address)
        self.assertEqual(set(siblings), {"prismdrake-settingsd", "prismdrake-shell"})
        component_commands = [
            argv
            for argv, _environment in operations.spawn_calls
            if Path(argv[0]).name.startswith("prismdrake-")
        ]
        self.assertEqual(
            component_commands,
            [
                ("/usr/bin/prismdrake-settingsd", "--foreground"),
                ("/usr/bin/prismdrake-shell",),
            ],
        )
        self.assertFalse(
            any(
                Path(argv[0]).name == "prismdrake-session"
                for argv, _environment in operations.spawn_calls
            )
        )
        configuration = (
            Path(environment["XDG_CONFIG_HOME"]) / "prismdrake" / "config.toml"
        )
        self.assertEqual(stat.S_IMODE(configuration.stat().st_mode), 0o600)
        self.assertEqual(configuration.read_text(encoding="ascii"), _session_config())
        self.assertEqual(port.cleanup("failure_replay"), Observation(True, True))
        self.assertEqual(operations.stop_calls, list(reversed(operations.children)))

    def test_notification_failure_adapter_errors_are_categorical(self) -> None:
        operations = FakeOperations()

        class FailingReplay(FakeNotificationFailureReplay):
            def verify_malformed_history(self, fixture, environment, endpoint):
                del fixture, environment, endpoint
                raise RuntimeError("private malformed history detail")

        port = LiveXorgHarnessPort(
            config(installed_paths()),
            operations,
            FakeAccessibility(operations),
            notification_failure_replay=FailingReplay(),
        )
        with self.assertRaisesRegex(
            HarnessError, "^live_malformed_notification_history_failed$"
        ):
            port.run_step(
                "failure_replay", "start_with_malformed_persistent_history"
            )
        self.assertEqual(port.cleanup("failure_replay"), Observation(True, True))

    def test_safe_mode_consumes_crash_budget_then_starts_a_fresh_normal_epoch(self) -> None:
        operations = FakeOperations()
        port = LiveXorgHarnessPort(config(installed_paths()), operations)

        self.assertEqual(
            port.run_step("failure_replay", "enter_and_exit_safe_mode"),
            Observation(True, True),
        )
        session_spawns = [
            child
            for child, (argv, _environment) in zip(
                operations.children, operations.spawn_calls, strict=True
            )
            if Path(argv[0]).name == "prismdrake-session"
        ]
        self.assertEqual(len(session_spawns), 2)
        self.assertEqual(session_spawns[0].returncode, 0)
        self.assertIsNone(session_spawns[1].returncode)
        self.assertEqual(operations.shell_crashes, 0)
        self.assertEqual(port.cleanup("failure_replay"), Observation(True, True))

    def test_safe_mode_fails_closed_when_the_marker_is_not_published(self) -> None:
        operations = FakeOperations()
        operations.safe_mode_published = False
        port = LiveXorgHarnessPort(config(installed_paths()), operations)

        with self.assertRaisesRegex(
            HarnessError, "^prismdrake_session_safe_mode_timeout$"
        ):
            port.run_step("failure_replay", "enter_and_exit_safe_mode")
        self.assertEqual(port.cleanup("failure_replay"), Observation(True, True))

    def test_x_server_loss_requires_the_post_stop_query_to_fail(self) -> None:
        operations = FakeOperations()
        port = LiveXorgHarnessPort(
            config(installed_paths()), operations, FakeAccessibility(operations)
        )

        self.assertEqual(
            port.run_step("failure_replay", "lose_x_server"),
            Observation(True, True),
        )
        self.assertTrue(all(child.returncode == 0 for child in operations.children))
        self.assertEqual(port.cleanup("failure_replay"), Observation(True, True))

    def test_x_server_loss_rejects_a_query_that_still_reaches_a_server(self) -> None:
        operations = FakeOperations()
        operations.xorg_query_survives_stop = True
        port = LiveXorgHarnessPort(
            config(installed_paths()), operations, FakeAccessibility(operations)
        )

        with self.assertRaisesRegex(HarnessError, "^x_server_loss_unobserved$"):
            port.run_step("failure_replay", "lose_x_server")
        self.assertEqual(port.cleanup("failure_replay"), Observation(True, True))

    def test_installed_hashes_require_exact_absolute_regular_executables(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            paths: dict[str, Path] = {}
            expected: dict[str, str] = {}
            for name in EXECUTABLE_NAMES:
                path = root / name
                payload = f"installed:{name}\n".encode("ascii")
                path.write_bytes(payload)
                path.chmod(0o700)
                paths[name] = path
                expected[name] = hashlib.sha256(payload).hexdigest()

            port = LiveXorgHarnessPort(config(paths), FakeOperations())
            self.assertEqual(port.installed_executable_hashes(), expected)

            paths[EXECUTABLE_NAMES[0]].chmod(stat.S_IRUSR | stat.S_IWUSR)
            with self.assertRaisesRegex(HarnessError, "^installed_executable_invalid$"):
                port.installed_executable_hashes()

    def test_installed_hashes_reject_symlink_even_when_target_is_executable(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            target = root / "target"
            target.write_bytes(b"executable")
            target.chmod(0o700)
            paths: dict[str, Path] = {}
            for name in EXECUTABLE_NAMES:
                path = root / name
                os.symlink(target, path)
                paths[name] = path

            with self.assertRaisesRegex(HarnessError, "^installed_executable_invalid$"):
                LiveXorgHarnessPort(config(paths), FakeOperations()).installed_executable_hashes()


if __name__ == "__main__":
    unittest.main()
