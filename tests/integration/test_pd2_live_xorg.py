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
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parent))
from pd2_demo import EXECUTABLE_NAMES, HarnessError, Observation  # noqa: E402
from pd2_live_notification_workflow import WorkflowResult  # noqa: E402
from pd2_live_xorg import (  # noqa: E402
    CONTROLLED_APPLICATION_NAME,
    CONTROLLED_WINDOW_ONE,
    CONTROLLED_WINDOW_TWO,
    CommandResult,
    LiveXorgConfig,
    LiveXorgHarnessPort,
    LiveNotificationSurfaceAdapter,
    ProcessIdentity,
    WindowGeometry,
    XrandrOutput,
    _NotificationEpoch,
    _session_config,
    _xorg_config,
    parse_cardinals,
    parse_window_geometry,
    parse_window_ids,
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
        self.surface_owner = 2004
        self.surface_focus = False
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
        self.active_window: int | None = None
        self.component_stop_succeeds = True
        self.component_replacement_published = True
        self.unrelated_replacement: str | None = None
        self.xorg_stopped = False
        self.xorg_query_survives_stop = False
        self.shell_crashes = 0
        self.safe_mode_published = True
        self.compositor_owned = False

    def spawn(self, argv: Sequence[str], env: Mapping[str, str]) -> FakeChild:
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
        self.run_calls.append((command, dict(env)))
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
                identifier = self.controlled_windows.get(title)
                return (
                    CommandResult(0, f"{identifier}\n".encode("ascii"))
                    if identifier is not None
                    else CommandResult(1)
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
        if command[-1] == "_NET_WM_CM_S0":
            if self.compositor_owned:
                return CommandResult(
                    0, b"_NET_WM_CM_S0(WINDOW): window id # 0x700001\n"
                )
            return CommandResult(
                0, b"_NET_WM_CM_S0:  no such atom on any window.\n"
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
                window_type = self.surface_types.get(window, b"_NET_WM_WINDOW_TYPE_NORMAL")
                return CommandResult(
                    0, b"_NET_WM_WINDOW_TYPE(ATOM) = " + window_type + b"\n"
                )
            if property_name == "_NET_WM_PID":
                return CommandResult(
                    0, f"_NET_WM_PID(CARDINAL) = {self.surface_owner}\n".encode("ascii")
                )
            if property_name == "WM_HINTS":
                focus = "True" if self.surface_focus else "False"
                return CommandResult(
                    0,
                    (
                        "WM_HINTS(WM_HINTS):\n"
                        f"\t\tClient accepts input or input focus: {focus}\n"
                        "\t\tInitial state is Normal State.\n"
                    ).encode("ascii"),
                )
            if property_name == "_NET_WM_STATE":
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
        del number
        return True

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
        self.invoked: list[str] = []
        self.persist_pin = True

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
                directory = Path(environment["XDG_STATE_HOME"]) / "prismdrake"
                directory.mkdir(mode=0o700, exist_ok=True)
                pins = directory / "pins.json"
                pins.write_bytes(
                    b'{"schema_version":1,"pins":["prismdrake-controlled.desktop"]}\n'
                )
                pins.chmod(0o600)
        elif selector.name == CONTROLLED_APPLICATION_NAME:
            if CONTROLLED_WINDOW_ONE not in self.operations.controlled_windows:
                self.operations.controlled_windows[CONTROLLED_WINDOW_ONE] = CONTROLLED_ONE_ID
                self.operations.active_window = CONTROLLED_ONE_ID
        elif selector.name == "Second window":
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
        return (
            self.observe(shell_pid, selector, environment)
            and state_name == "CHECKED"
            and expected is True
        )

    def observe_absent(
        self, shell_pid: int, selector: object, environment: Mapping[str, str]
    ) -> bool:
        return (
            shell_pid == self.operations.surface_owner
            and "DBUS_SESSION_BUS_ADDRESS" in environment
            and selector.name == "Transient notifications"
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


def config(installed: Mapping[str, Path] | None = None) -> LiveXorgConfig:
    return LiveXorgConfig(
        xorg=Path("/usr/bin/Xorg"),
        xrandr=Path("/usr/bin/xrandr"),
        openbox=Path("/usr/bin/openbox"),
        xprop=Path("/usr/bin/xprop"),
        xwininfo=Path("/usr/bin/xwininfo"),
        dbus_daemon=Path("/usr/bin/dbus-daemon"),
        installed_executables=installed or {},
        xdotool=Path("/usr/bin/xdotool"),
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


class FakeClosable:
    def __init__(self) -> None:
        self.close_calls = 0

    def close(self) -> None:
        self.close_calls += 1


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
        self.assertEqual(document["developer"]["mock_capability_overrides"], [])
        self.assertNotIn("/home/", payload)


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
        accessibility.absent = {"Transient notifications": [True]}
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
        )

        adapter.invoke_live_action(0.2)
        adapter.observe_center(0.2)
        adapter.observe_do_not_disturb(True, 0.2)
        adapter.observe_transient(False, 0.2)
        adapter.observe_transient(True, 0.2)

        self.assertEqual(accessibility.invoked, ["Review"])
        self.assertIn(("CHECKED", "Do not disturb"), accessibility.calls)
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
        )

        with self.assertRaisesRegex(
            HarnessError, "^notification_surface_transient_absence_timeout$"
        ):
            adapter.observe_transient(False, 0.1)
        self.assertTrue(accessibility.calls)
        self.assertTrue(all(kind == "absent" for kind, _name in accessibility.calls))


class LivePortTests(unittest.TestCase):
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
                argv[-8:] == (
                    "--output", "DUMMY1", "--mode", "1600x900",
                    "--scale", "1x1", "--pos", "1600x0",
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
            port.run_step("installed_workflow", "open_quick_settings")
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
        self.assertEqual(session_environment["QSG_RHI_BACKEND"], "software")
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
                    "QSG_RHI_BACKEND",
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
        self.assertEqual(configuration.read_text(encoding="ascii"), _session_config())
        geometry_queries = [
            argv
            for argv, _environment in operations.run_calls
            if argv[0] == "/usr/bin/xwininfo"
        ]
        self.assertEqual(len(geometry_queries), 6)
        self.assertEqual(port.cleanup("installed_workflow"), Observation(True, True))

    def test_runs_controlled_launcher_pin_and_two_window_panel_lifecycle(self) -> None:
        operations = FakeOperations()
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

        for identifier in identifiers:
            with self.subTest(identifier=identifier):
                self.assertEqual(
                    port.run_step("installed_workflow", identifier),
                    Observation(True, True),
                )

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
            ],
        )
        self.assertTrue(accessibility.pinned)
        self.assertEqual(operations.controlled_windows, {})
        self.assertIn("Pin to panel", accessibility.invoked)
        self.assertIn("Second window", accessibility.invoked)
        self.assertIn(
            f"Close {CONTROLLED_APPLICATION_NAME}", accessibility.invoked
        )
        self.assertEqual(port.cleanup("installed_workflow"), Observation(True, True))

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
                if identifier == "restart_notifyd":
                    self.assertIn("Notification center", accessibility.observed)
                if identifier == "restart_shell":
                    self.assertTrue(accessibility.pinned)
                    self.assertIn(CONTROLLED_APPLICATION_NAME, accessibility.observed)
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

    def test_uncomposited_blur_and_corrupt_file_fallbacks_use_private_live_state(self) -> None:
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

    def test_uncomposited_fallback_rejects_a_live_compositor_selection_owner(self) -> None:
        operations = FakeOperations()
        operations.compositor_owned = True
        port = LiveXorgHarnessPort(
            config(installed_paths()), operations, FakeAccessibility(operations)
        )

        with self.assertRaisesRegex(
            HarnessError, "^unexpected_compositor_selection_owner$"
        ):
            port.run_step("failure_replay", "run_without_compositor")
        self.assertEqual(port.cleanup("failure_replay"), Observation(True, True))

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
