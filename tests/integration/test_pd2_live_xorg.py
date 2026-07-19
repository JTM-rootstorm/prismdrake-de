#!/usr/bin/env python3
"""Hermetic tests for the live PD2 Xorg-dummy adapter."""

from __future__ import annotations

import hashlib
import os
import stat
import sys
import tempfile
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
    XrandrOutput,
    _xorg_config,
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
        ambiguous_descendant: bool = False,
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
        self.ambiguous_descendant = ambiguous_descendant

    def spawn(self, argv: Sequence[str], env: Mapping[str, str]) -> FakeChild:
        child = FakeChild(1000 + len(self.children))
        self.children.append(child)
        self.spawn_calls.append((tuple(argv), dict(env)))
        if argv and Path(argv[0]).name == "dbus-run-session":
            root = Path(env["XDG_RUNTIME_DIR"]) / "prismdrake"
            root.mkdir(mode=0o700)
            instance = root / "session-2002-0"
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
        return CommandResult(0)

    def stop(self, child: FakeChild, timeout: float) -> bool:
        del timeout
        self.stop_calls.append(child)
        if self.stop_succeeds:
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
        return ProcessIdentity(child.pid, child.pid * 10)

    def descendants(
        self, child: FakeChild, executable: Path
    ) -> Sequence[ProcessIdentity]:
        argv, _environment = self.spawn_calls[child.pid - 1000]
        if child.poll() is not None or Path(argv[0]).name != "dbus-run-session":
            return ()
        if executable.name == "dbus-daemon":
            identities = [ProcessIdentity(2001, 20_010)]
        elif executable.name == "prismdrake-session":
            identities = [ProcessIdentity(2002, 20_020)]
        else:
            return ()
        if self.ambiguous_descendant:
            identities.append(ProcessIdentity(2003, 20_030))
        return tuple(identities)

    def monotonic(self) -> float:
        return self.now

    def pause(self, duration: float) -> None:
        self.now += duration


def config(installed: Mapping[str, Path] | None = None) -> LiveXorgConfig:
    return LiveXorgConfig(
        xorg=Path("/usr/bin/Xorg"),
        xrandr=Path("/usr/bin/xrandr"),
        openbox=Path("/usr/bin/openbox"),
        xprop=Path("/usr/bin/xprop"),
        dbus_run_session=Path("/usr/bin/dbus-run-session"),
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
            port.run_step("installed_workflow", "verify_panel_policy_both_outputs")
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

        session_argv, session_environment = operations.spawn_calls[2]
        self.assertEqual(
            session_argv,
            (
                "/usr/bin/dbus-run-session",
                "--dbus-daemon=/usr/bin/dbus-daemon",
                "--",
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
        self.assertNotIn("DBUS_SESSION_BUS_ADDRESS", session_environment)
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

    def test_session_readiness_rejects_safe_mode_and_ambiguous_process_owner(self) -> None:
        for operations, expected in (
            (
                FakeOperations(session_marker="safe-mode"),
                "prismdrake_session_safe_mode_unexpected",
            ),
            (
                FakeOperations(ambiguous_descendant=True),
                "live_process_ownership_invalid",
            ),
        ):
            with self.subTest(expected=expected):
                port = LiveXorgHarnessPort(config(installed_paths()), operations)
                port.run_step("installed_workflow", "start_isolated_two_output_x11")
                port.run_step("installed_workflow", "start_installed_prismdrake_session")
                with self.assertRaisesRegex(HarnessError, f"^{expected}$"):
                    port.run_step("installed_workflow", "verify_component_readiness")
                self.assertEqual(
                    port.cleanup("installed_workflow"), Observation(True, True)
                )

    def test_session_readiness_rejects_replaced_marker(self) -> None:
        operations = FakeOperations()
        port = LiveXorgHarnessPort(config(installed_paths()), operations)
        port.run_step("installed_workflow", "start_isolated_two_output_x11")
        port.run_step("installed_workflow", "start_installed_prismdrake_session")
        _argv, environment = operations.spawn_calls[2]
        ready = (
            Path(environment["XDG_RUNTIME_DIR"])
            / "prismdrake"
            / "session-2002-0"
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
