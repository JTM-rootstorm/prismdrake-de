#!/usr/bin/env python3
"""Hermetic adversarial tests for the bounded live x11trace supervisor."""

from __future__ import annotations

import stat
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from typing import Any


sys.path.insert(0, str(Path(__file__).resolve().parent))
from pd2_x11trace import MAX_TRACE_BYTES  # noqa: E402
from pd2_x11trace_supervisor import (  # noqa: E402
    MAX_LIST_LENGTH,
    X11TRACE_EXECUTABLE,
    X11TraceConfig,
    X11TraceSupervisor,
    X11TraceSupervisorError,
    _remove_owned_socket,
    inspect_x11trace_executable,
)


HANDSHAKE = (
    "000:<: am lsb-first want 11:0 authorising with '' of length 0\n"
    "000:>: Success, version is 11:0 vendor='The X.Org Founda'... "
    "release=12101016 resource-id=0x00200000 resource-mask=0x001fffff "
    "motion-buffer-size=256 max-request-len=4194303 "
    "roots={root=0x0000050d root-depth=24;};\n"
)
CLEAN_TRACE = (
    HANDSHAKE
    + "000:<:0001: 20: Request(98): QueryExtension name='BIG-REQUESTS'\n"
    + "000:>:0001:32: Reply to QueryExtension: present=true(0x01) "
    "major-opcode=133 first-event=0 first-error=0\n"
).encode("utf-8")
FORBIDDEN_TRACE = (
    CLEAN_TRACE
    + b"000:<:0002: 20: Request(73): GetImage drawable=0x00200001 "
    b"x=0 y=0 width=1 height=1\n"
)


class FakeProcess:
    def __init__(
        self,
        *,
        returncode: int | None = None,
        stubborn: bool = False,
        natural_exit: bool = True,
    ) -> None:
        self.pid = 4242
        self.returncode = returncode
        self.stubborn = stubborn
        self.natural_exit = natural_exit
        self.terminate_calls = 0
        self.kill_calls = 0
        self.wait_calls = 0

    def poll(self) -> int | None:
        return self.returncode

    def terminate(self) -> None:
        self.terminate_calls += 1
        if not self.stubborn:
            self.returncode = -15

    def kill(self) -> None:
        self.kill_calls += 1
        self.returncode = -9

    def wait(self, timeout: float | None = None) -> int:
        del timeout
        self.wait_calls += 1
        if self.returncode is None:
            if self.natural_exit and not self.stubborn:
                self.returncode = 0
                return self.returncode
            raise subprocess.TimeoutExpired("redacted", 1)
        return self.returncode


class UnreapedProcess(FakeProcess):
    def __init__(self) -> None:
        super().__init__(stubborn=True, natural_exit=False)

    def kill(self) -> None:
        self.kill_calls += 1


class ReadyObserver:
    def __init__(self, *, ready: bool = True, disappear: bool = False) -> None:
        self.ready = ready
        self.disappear = disappear
        self.calls = 0

    def __call__(self, number: int) -> tuple[int, int] | None:
        if number != 78:
            raise RuntimeError("wrong fake display")
        self.calls += 1
        if self.calls == 1:
            return None
        if self.disappear and self.calls > 2:
            return None
        return (77, 78) if self.ready else None


class ReplacementObserver:
    def __init__(self) -> None:
        self.calls = 0

    def __call__(self, number: int) -> tuple[int, int] | None:
        if number != 78:
            raise RuntimeError("wrong fake display")
        self.calls += 1
        if self.calls == 1:
            return None
        if self.calls == 2:
            return 77, 78
        return 77, 79


class FakeDisplayCleaner:
    def __init__(self, *, succeeds: bool = True) -> None:
        self.succeeds = succeeds
        self.calls: list[tuple[int, tuple[int, int]]] = []

    def __call__(self, number: int, identity: tuple[int, int]) -> bool:
        self.calls.append((number, identity))
        return self.succeeds


class ProcessFactory:
    def __init__(
        self,
        payload: bytes = CLEAN_TRACE,
        *,
        process: FakeProcess | None = None,
        replace_output: bool = False,
    ) -> None:
        self.payload = payload
        self.process = process or FakeProcess()
        self.replace_output = replace_output
        self.arguments: tuple[str, ...] | None = None
        self.environment: dict[str, str] | None = None
        self.output_path: Path | None = None

    def __call__(self, arguments: tuple[str, ...], environment: Any) -> FakeProcess:
        self.arguments = arguments
        self.environment = dict(environment)
        index = arguments.index("--outfile")
        self.output_path = Path(arguments[index + 1])
        if self.replace_output:
            self.output_path.unlink()
        self.output_path.write_bytes(self.payload)
        self.output_path.chmod(0o600)
        return self.process


class X11TraceSupervisorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.root.chmod(0o700)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def config(self, **changes: Any) -> X11TraceConfig:
        values: dict[str, Any] = {
            "private_root": self.root,
            "real_display": ":77",
            "fake_display": ":78",
            "maximum_capture_seconds": 2.0,
            "readiness_seconds": 0.1,
            "shutdown_seconds": 0.05,
        }
        values.update(changes)
        return X11TraceConfig(**values)

    @staticmethod
    def environment() -> dict[str, str]:
        return {
            "PATH": "/usr/bin:/bin",
            "HOME": "/private/home",
            "DISPLAY": ":999",
            "FAKEDISPLAY": ":998",
            "XAUTHORITY": "/private/auth",
        }

    @staticmethod
    def executable_ok(path: Path) -> bool:
        return path == X11TRACE_EXECUTABLE

    def make_supervisor(
        self,
        factory: ProcessFactory,
        *,
        observer: Any | None = None,
        config: X11TraceConfig | None = None,
        cleaner: FakeDisplayCleaner | None = None,
    ) -> X11TraceSupervisor:
        return X11TraceSupervisor(
            config or self.config(),
            self.environment(),
            process_factory=factory,
            executable_inspector=self.executable_ok,
            fake_display_observer=observer or ReadyObserver(),
            fake_display_cleaner=cleaner or FakeDisplayCleaner(),
        )

    def assert_category(self, category: str, operation: Any) -> None:
        with self.assertRaises(X11TraceSupervisorError) as raised:
            operation()
        self.assertEqual(str(raised.exception), category)
        self.assertIsNone(raised.exception.__cause__)
        self.assertNotIn(str(self.root), str(raised.exception))
        self.assertNotIn("private-window-title", str(raised.exception))

    def test_exact_no_auth_proxy_contract_and_clean_summary(self) -> None:
        factory = ProcessFactory()
        cleaner = FakeDisplayCleaner()
        supervisor = self.make_supervisor(factory, cleaner=cleaner)
        self.assertFalse(supervisor.finished)
        self.assertEqual(supervisor.proxy_display, ":78")
        self.assertIsNotNone(factory.arguments)
        arguments = factory.arguments
        assert arguments is not None
        self.assertEqual(
            arguments[:-1],
            (
                "/usr/bin/x11trace",
                "--nocopyauthentication",
                "--display",
                ":77",
                "--fakedisplay",
                ":78",
                "--waitforclient",
                "--keeprunning",
                "--maxlistlength",
                str(MAX_LIST_LENGTH),
                "--outfile",
            ),
        )
        self.assertNotIn("--copyauthentication", arguments)
        self.assertNotIn("--authfile", arguments)
        self.assertNotIn("--newauthfile", arguments)
        self.assertEqual(factory.environment["DISPLAY"], ":77")
        self.assertEqual(factory.environment["FAKEDISPLAY"], ":78")
        self.assertNotIn("XAUTHORITY", factory.environment)
        self.assertEqual(factory.environment["LC_ALL"], "C")

        client = supervisor.client_environment(self.environment())
        self.assertEqual(client["DISPLAY"], ":78")
        self.assertNotIn("FAKEDISPLAY", client)
        self.assertNotIn("XAUTHORITY", client)
        supervisor.poll()
        summary = supervisor.finish(clients_exited=True)
        self.assertTrue(supervisor.finished)
        self.assertTrue(summary.trace_complete)
        self.assertEqual(summary.forbidden_request_count, 0)
        self.assertEqual(summary.request_count, 1)
        self.assertFalse(factory.output_path.exists())
        self.assertEqual(list(self.root.iterdir()), [])
        self.assertEqual(factory.process.terminate_calls, 1)
        self.assertEqual(factory.process.kill_calls, 0)
        self.assertGreaterEqual(factory.process.wait_calls, 1)
        self.assertEqual(cleaner.calls, [(78, (77, 78))])

    def test_finish_fails_closed_when_exact_proxy_socket_cannot_be_removed(self) -> None:
        factory = ProcessFactory()
        cleaner = FakeDisplayCleaner(succeeds=False)
        supervisor = self.make_supervisor(factory, cleaner=cleaner)
        self.assert_category(
            "x11trace_fake_display_cleanup_failed",
            lambda: supervisor.finish(clients_exited=True),
        )
        self.assertTrue(supervisor.finished)
        self.assertEqual(cleaner.calls, [(78, (77, 78))])
        self.assertFalse(factory.output_path.exists())

    def test_unreaped_proxy_preserves_socket_and_fails_cleanup(self) -> None:
        process = UnreapedProcess()
        factory = ProcessFactory(process=process)
        cleaner = FakeDisplayCleaner()
        supervisor = self.make_supervisor(factory, cleaner=cleaner)
        self.assert_category(
            "x11trace_fake_display_cleanup_failed",
            lambda: supervisor.finish(clients_exited=False),
        )
        self.assertEqual(cleaner.calls, [])
        self.assertGreaterEqual(process.kill_calls, 1)

    def test_poll_rejects_replaced_proxy_socket_identity(self) -> None:
        factory = ProcessFactory()
        cleaner = FakeDisplayCleaner()
        supervisor = self.make_supervisor(
            factory,
            observer=ReplacementObserver(),
            cleaner=cleaner,
        )
        self.assert_category("x11trace_fake_display_invalid", supervisor.poll)
        self.assert_category(
            "x11trace_clients_active",
            lambda: supervisor.finish(clients_exited=False),
        )
        self.assertEqual(cleaner.calls, [(78, (77, 78))])

    def test_exact_owned_socket_cleanup_preserves_identity_replacement(self) -> None:
        path = self.root / "proxy.sock"
        first = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            first.bind(str(path))
        except PermissionError:
            first.close()
            self.skipTest("sandbox blocks AF_UNIX pathname binding")
        metadata = path.lstat()
        identity = (metadata.st_dev, metadata.st_ino)
        self.assertTrue(_remove_owned_socket(path, identity))
        self.assertFalse(path.exists())
        first.close()

        second = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        second.bind(str(path))
        self.assertFalse(_remove_owned_socket(path, identity))
        self.assertTrue(path.exists())
        second.close()
        path.unlink()

    def test_kill_and_reap_paths_are_always_incomplete(self) -> None:
        process = FakeProcess(stubborn=True, natural_exit=False)
        factory = ProcessFactory(process=process)
        supervisor = self.make_supervisor(factory)
        self.assert_category(
            "x11trace_capture_incomplete",
            lambda: supervisor.finish(clients_exited=True),
        )
        self.assertEqual(process.terminate_calls, 2)
        self.assertEqual(process.kill_calls, 1)
        self.assertGreaterEqual(process.wait_calls, 2)

    def test_finish_accepts_natural_zero_exit_race_but_active_poll_rejects_it(self) -> None:
        process = FakeProcess()
        factory = ProcessFactory(process=process)
        supervisor = self.make_supervisor(factory)
        process.returncode = 0
        summary = supervisor.finish(clients_exited=True)
        self.assertEqual(summary.request_count, 1)
        self.assertEqual(process.terminate_calls, 0)

        with tempfile.TemporaryDirectory() as temporary:
            self.root = Path(temporary)
            self.root.chmod(0o700)
            active_process = FakeProcess()
            active = self.make_supervisor(ProcessFactory(process=active_process))
            active_process.returncode = 0
            self.assert_category("x11trace_capture_incomplete", active.poll)
            self.assert_category(
                "x11trace_clients_active",
                lambda: active.finish(clients_exited=False),
            )

    def test_rejects_active_clients_and_erases_raw_capture(self) -> None:
        factory = ProcessFactory()
        supervisor = self.make_supervisor(factory)
        self.assert_category(
            "x11trace_clients_active",
            lambda: supervisor.finish(clients_exited=False),
        )
        self.assertFalse(factory.output_path.exists())
        self.assertIsNotNone(factory.process.poll())

    def test_rejects_forbidden_malformed_empty_and_private_trace_content(self) -> None:
        cases = (
            FORBIDDEN_TRACE,
            b"not x11trace grammar\n",
            b"",
            CLEAN_TRACE + b"private-window-title\xff\n",
        )
        for payload in cases:
            with self.subTest(size=len(payload)):
                with tempfile.TemporaryDirectory() as temporary:
                    self.root = Path(temporary)
                    self.root.chmod(0o700)
                    factory = ProcessFactory(payload)
                    supervisor = self.make_supervisor(factory)
                    self.assert_category(
                        "x11trace_trace_invalid",
                        lambda: supervisor.finish(clients_exited=True),
                    )
                    self.assertFalse(factory.output_path.exists())

    def test_rejects_overflow_and_time_bound_then_kills_and_cleans(self) -> None:
        overflow_factory = ProcessFactory(b"x" * (MAX_TRACE_BYTES + 1))
        self.assert_category(
            "x11trace_output_overflow",
            lambda: self.make_supervisor(overflow_factory),
        )
        self.assertFalse(overflow_factory.output_path.exists())

        with tempfile.TemporaryDirectory() as temporary:
            self.root = Path(temporary)
            self.root.chmod(0o700)
            timeout_factory = ProcessFactory()
            timeout = self.make_supervisor(
                timeout_factory,
                config=self.config(maximum_capture_seconds=0.05),
            )
            time.sleep(0.08)
            self.assert_category("x11trace_process_timeout", timeout.poll)
            try:
                timeout.finish(clients_exited=True)
            except X11TraceSupervisorError:
                pass
            self.assertFalse(timeout_factory.output_path.exists())

    def test_rejects_early_exit_missing_display_and_display_disappearance(self) -> None:
        early = ProcessFactory(process=FakeProcess(returncode=7))
        self.assert_category(
            "x11trace_capture_incomplete", lambda: self.make_supervisor(early)
        )

        missing = ProcessFactory()
        self.assert_category(
            "x11trace_fake_display_timeout",
            lambda: self.make_supervisor(missing, observer=ReadyObserver(ready=False)),
        )
        self.assertIsNotNone(missing.process.poll())
        self.assertFalse(missing.output_path.exists())

        with tempfile.TemporaryDirectory() as temporary:
            self.root = Path(temporary)
            self.root.chmod(0o700)
            disappeared_factory = ProcessFactory()
            disappeared = self.make_supervisor(
                disappeared_factory, observer=ReadyObserver(disappear=True)
            )
            self.assert_category(
                "x11trace_fake_display_invalid", disappeared.poll
            )
            self.assert_category(
                "x11trace_clients_active",
                lambda: disappeared.finish(clients_exited=False),
            )

    def test_rejects_output_replacement_and_preserves_unknown_replacement(self) -> None:
        factory = ProcessFactory(replace_output=True)
        self.assert_category(
            "x11trace_output_invalid", lambda: self.make_supervisor(factory)
        )
        self.assertTrue(factory.output_path.exists())
        self.assertEqual(factory.output_path.read_bytes(), CLEAN_TRACE)

    def test_rejects_invalid_roots_displays_bounds_and_environment(self) -> None:
        factory = ProcessFactory()
        self.root.chmod(0o755)
        self.assert_category(
            "x11trace_private_root_invalid", lambda: self.make_supervisor(factory)
        )
        self.root.chmod(0o700)
        cases = (
            ({"real_display": "localhost:77"}, "x11trace_real_display_invalid"),
            ({"real_display": ":78"}, "x11trace_fake_display_conflict"),
            ({"fake_display": ":-1"}, "x11trace_fake_display_invalid"),
            ({"maximum_capture_seconds": 0.0}, "x11trace_runtime_bound_invalid"),
            ({"readiness_seconds": 11.0}, "x11trace_runtime_bound_invalid"),
        )
        for changes, category in cases:
            with self.subTest(category=category):
                self.assert_category(
                    category,
                    lambda changes=changes: self.make_supervisor(
                        factory, config=self.config(**changes)
                    ),
                )
        self.assert_category(
            "x11trace_client_environment_invalid",
            lambda: X11TraceSupervisor(
                self.config(),
                {"BAD\x00KEY": "value"},
                process_factory=factory,
                executable_inspector=self.executable_ok,
                fake_display_observer=ReadyObserver(),
            ),
        )
        oversized_environment = {f"KEY_{index}": "value" for index in range(128)}
        self.assert_category(
            "x11trace_client_environment_invalid",
            lambda: X11TraceSupervisor(
                self.config(),
                oversized_environment,
                process_factory=factory,
                executable_inspector=self.executable_ok,
                fake_display_observer=ReadyObserver(),
            ),
        )

    def test_exact_executable_inspector_rejects_missing_symlink_and_writable(self) -> None:
        self.assertFalse(inspect_x11trace_executable(self.root / "x11trace"))
        executable = self.root / "x11trace"
        executable.write_text("fixture", encoding="utf-8")
        executable.chmod(0o755)
        self.assertFalse(inspect_x11trace_executable(executable))

        self.assert_category(
            "x11trace_executable_invalid",
            lambda: X11TraceSupervisor(
                self.config(),
                self.environment(),
                process_factory=ProcessFactory(),
                executable_inspector=lambda path: False,
                fake_display_observer=ReadyObserver(),
            ),
        )

    def test_double_finish_and_client_environment_after_finish_fail_closed(self) -> None:
        supervisor = self.make_supervisor(ProcessFactory())
        supervisor.finish(clients_exited=True)
        self.assert_category(
            "x11trace_already_finished",
            lambda: supervisor.finish(clients_exited=True),
        )
        self.assert_category(
            "x11trace_already_finished",
            lambda: supervisor.client_environment(self.environment()),
        )

    def test_raw_capture_is_private_while_running(self) -> None:
        factory = ProcessFactory()
        supervisor = self.make_supervisor(factory)
        output = factory.output_path
        self.assertIsNotNone(output)
        assert output is not None
        self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(output.parent.stat().st_mode), 0o700)
        self.assertEqual(output.parent.parent, self.root)
        supervisor.finish(clients_exited=True)


if __name__ == "__main__":
    unittest.main()
