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
from pd2_live_xorg import (  # noqa: E402
    CommandResult,
    LiveXorgConfig,
    LiveXorgHarnessPort,
    ProcessIdentity,
    WindowGeometry,
    XrandrOutput,
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
        self.shell_start_time = 20_040

    def spawn(self, argv: Sequence[str], env: Mapping[str, str]) -> FakeChild:
        child = FakeChild(1000 + len(self.children))
        self.children.append(child)
        self.spawn_calls.append((tuple(argv), dict(env)))
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
            if self.query_results:
                return self.query_results.pop(0)
            if self.query_payloads:
                return CommandResult(0, self.query_payloads.pop(0))
            return CommandResult(0, self.query_payload)
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
            if Path(self.spawn_calls[child.pid - 1000][0][0]).name == "dbus-daemon":
                self.bus_socket_path = None
                self.bus_socket_identity = None
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
        if executable.name == "prismdrake-shell":
            identities = [ProcessIdentity(2004, self.shell_start_time)]
        else:
            return ()
        return tuple(identities)

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


def config(installed: Mapping[str, Path] | None = None) -> LiveXorgConfig:
    return LiveXorgConfig(
        xorg=Path("/usr/bin/Xorg"),
        xrandr=Path("/usr/bin/xrandr"),
        openbox=Path("/usr/bin/openbox"),
        xprop=Path("/usr/bin/xprop"),
        xwininfo=Path("/usr/bin/xwininfo"),
        dbus_daemon=Path("/usr/bin/dbus-daemon"),
        installed_executables=installed or {},
        first_display=140,
        last_display=140,
        readiness_timeout=0.2,
        stop_timeout=0.1,
    )


def installed_paths() -> dict[str, Path]:
    return {name: Path("/usr/bin") / name for name in EXECUTABLE_NAMES}


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
        self.assertEqual(document["desktop"]["wallpaper_source"], "")
        self.assertEqual(document["desktop"]["background_color"], "#182033")
        self.assertEqual(document["developer"]["mock_capability_overrides"], [])
        self.assertNotIn("/home/", payload)


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

    def test_unsupported_shell_step_and_unstarted_process_attestation_fail_closed(self) -> None:
        operations = FakeOperations()
        port = LiveXorgHarnessPort(config(), operations)

        with self.assertRaisesRegex(HarnessError, "^live_step_not_implemented$"):
            port.run_step("installed_workflow", "invoke_launcher_through_shellctl")
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
                    operations.shell_start_time += 1
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
            ("replace_or_restart_window_manager", (TWO_OUTPUTS,)),
        )
        for identifier, payloads in cases:
            with self.subTest(identifier=identifier):
                operations = FakeOperations(query_payloads=payloads)
                port = LiveXorgHarnessPort(config(), operations)
                self.assertEqual(
                    port.run_step("failure_replay", identifier),
                    Observation(True, True),
                )
                if identifier == "replace_or_restart_window_manager":
                    self.assertEqual(len(operations.spawn_calls), 3)
                    self.assertEqual(len(operations.stop_calls), 1)
                self.assertEqual(
                    port.cleanup("failure_replay"), Observation(True, True)
                )

    def test_x_server_loss_requires_the_post_stop_query_to_fail(self) -> None:
        operations = FakeOperations(
            query_results=(
                CommandResult(0, TWO_OUTPUTS),
                CommandResult(0, TWO_OUTPUTS),
                CommandResult(1, b"", b"cannot open display"),
            )
        )
        port = LiveXorgHarnessPort(config(), operations)

        self.assertEqual(
            port.run_step("failure_replay", "lose_x_server"),
            Observation(True, True),
        )
        self.assertEqual(port.cleanup("failure_replay"), Observation(True, True))

    def test_x_server_loss_rejects_a_query_that_still_reaches_a_server(self) -> None:
        operations = FakeOperations(
            query_results=(
                CommandResult(0, TWO_OUTPUTS),
                CommandResult(0, TWO_OUTPUTS),
                CommandResult(0, TWO_OUTPUTS),
            )
        )
        port = LiveXorgHarnessPort(config(), operations)

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
