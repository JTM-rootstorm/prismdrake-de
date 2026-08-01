#!/usr/bin/env python3
"""Hermetic tests for the installed PD2 demonstration command driver."""

from __future__ import annotations

import contextlib
import hashlib
import io
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


INTEGRATION_ROOT = Path(__file__).resolve().parent
DRIVER = INTEGRATION_ROOT / "pd2_demo.py"
sys.path.insert(0, os.fspath(INTEGRATION_ROOT))

import pd2_demo  # noqa: E402


REVISION = "a" * 40
HARNESS_MANIFEST_SHA256 = "b" * 64


class FakeLivePort:
    """Complete deterministic port that records no private runtime facts."""

    def __init__(self, hashes: dict[str, str], fixture: Path | None = None) -> None:
        self.hashes = hashes
        self.fixture = fixture

    def installed_executable_hashes(self) -> dict[str, str]:
        return self.hashes

    def run_step(self, phase: str, identifier: str) -> pd2_demo.Observation:
        del phase, identifier
        return pd2_demo.Observation(True, True)

    def capture_process_ownership(self) -> dict[str, object]:
        return {role: object() for role in pd2_demo.PROCESS_ROLES}

    def cleanup(self, phase: str) -> pd2_demo.Observation:
        if phase == "tier_c_replay" and self.fixture is not None:
            self.fixture.write_bytes(b"changed fixture")
        return pd2_demo.Observation(True, True)


def executable_hashes() -> dict[str, str]:
    return {
        name: hashlib.sha256(name.encode("ascii")).hexdigest()
        for name in pd2_demo.EXECUTABLE_NAMES
    }


def attestation(hashes: dict[str, str]) -> dict[str, object]:
    return {
        "source": {"revision": REVISION},
        "demo_harness": {"manifest_sha256": HARNESS_MANIFEST_SHA256},
        "installed": {
            "executables": [
                {"path": os.fspath(path), "sha256": hashes[name]}
                for name, path in zip(
                    pd2_demo.EXECUTABLE_NAMES,
                    pd2_demo.INSTALLED_EXECUTABLE_PATHS,
                )
            ]
        },
    }


class InstalledExecutionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name).resolve()
        self.preflight = self.root / "preflight.json"
        self.preflight.write_text("{}\n", encoding="ascii")
        self.preflight.chmod(0o600)
        self.fixture = self.root / "controlled-fixture"
        self.fixture.write_bytes(b"fixture executable")
        self.fixture.chmod(0o700)
        self.output = self.root / "evidence.json"
        self.hashes = executable_hashes()
        self.document = attestation(self.hashes)
        self.validation_calls: list[tuple[object, object, object, object]] = []

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def api(self):
        def validate_files(document, paths, driver, hash_file, fixture) -> None:
            del hash_file
            self.validation_calls.append((document, paths, driver, fixture))

        return (lambda path: self.document, validate_files)

    def test_derives_revision_binds_twice_and_writes_private_evidence(self) -> None:
        driver = self.root / "exact-driver.py"
        observed_fixture: list[Path] = []

        def factory(fixture: Path) -> FakeLivePort:
            observed_fixture.append(fixture)
            return FakeLivePort(self.hashes)

        pd2_demo.execute_installed_demonstration(
            self.preflight,
            self.fixture,
            self.output,
            driver_path=driver,
            attestation_api=self.api(),
            hash_file=lambda path: "f" * 64,
            port_factory=factory,
        )

        self.assertEqual(observed_fixture, [self.fixture])
        self.assertEqual(len(self.validation_calls), 2)
        self.assertTrue(all(call[1] == pd2_demo.INSTALLED_EXECUTABLE_PATHS
                            for call in self.validation_calls))
        self.assertTrue(all(call[2] == driver for call in self.validation_calls))
        self.assertTrue(all(call[3] == self.fixture for call in self.validation_calls))
        evidence = pd2_demo.load_evidence(self.output)
        self.assertEqual(evidence["source_revision"], REVISION)
        self.assertEqual(
            evidence["harness_manifest_sha256"], HARNESS_MANIFEST_SHA256
        )
        self.assertEqual(self.output.stat().st_mode & 0o777, 0o600)

    def test_rejects_hash_drift_before_starting_live_environment(self) -> None:
        started = False

        def factory(fixture: Path) -> FakeLivePort:
            nonlocal started
            del fixture
            started = True
            changed = dict(self.hashes)
            changed["prismdrake-shell"] = "0" * 64
            return FakeLivePort(changed)

        with self.assertRaisesRegex(
            pd2_demo.HarnessError, "^attested_executable_set_changed$"
        ):
            pd2_demo.execute_installed_demonstration(
                self.preflight,
                self.fixture,
                self.output,
                driver_path=self.root / "driver.py",
                attestation_api=self.api(),
                port_factory=factory,
            )
        self.assertTrue(started)
        self.assertFalse(self.output.exists())

    def test_detects_controlled_fixture_change_after_mandatory_cleanup(self) -> None:
        with self.assertRaisesRegex(
            pd2_demo.HarnessError, "^controlled_fixture_changed$"
        ):
            pd2_demo.execute_installed_demonstration(
                self.preflight,
                self.fixture,
                self.output,
                driver_path=self.root / "driver.py",
                attestation_api=self.api(),
                port_factory=lambda fixture: FakeLivePort(self.hashes, fixture),
            )
        self.assertFalse(self.output.exists())

    def test_rejects_output_parent_before_loading_or_running(self) -> None:
        loaded = False

        def load(path: Path) -> object:
            nonlocal loaded
            del path
            loaded = True
            return self.document

        with self.assertRaisesRegex(pd2_demo.HarnessError, "^output_parent_invalid$"):
            pd2_demo.execute_installed_demonstration(
                self.preflight,
                self.fixture,
                self.root / "missing" / "evidence.json",
                attestation_api=(load, lambda *args: None),
                port_factory=lambda fixture: FakeLivePort(self.hashes),
            )
        self.assertFalse(loaded)

    def test_rejects_existing_regular_output_before_loading_or_running(self) -> None:
        self.output.write_bytes(b"retained evidence\n")
        self.output.chmod(0o640)
        loaded = False

        def load(path: Path) -> object:
            nonlocal loaded
            del path
            loaded = True
            return self.document

        with self.assertRaisesRegex(pd2_demo.HarnessError, "^output_target_exists$"):
            pd2_demo.execute_installed_demonstration(
                self.preflight,
                self.fixture,
                self.output,
                attestation_api=(load, lambda *args: None),
                port_factory=lambda fixture: FakeLivePort(self.hashes),
            )

        self.assertFalse(loaded)
        self.assertEqual(self.output.read_bytes(), b"retained evidence\n")
        self.assertEqual(stat.S_IMODE(self.output.stat().st_mode), 0o640)

    def test_rejects_writable_or_linked_controlled_fixture(self) -> None:
        self.fixture.chmod(0o777)
        with self.assertRaisesRegex(pd2_demo.HarnessError, "^controlled_fixture_invalid$"):
            pd2_demo._controlled_fixture_sha256(self.fixture)
        self.fixture.chmod(0o700)
        linked = self.root / "linked-fixture"
        linked.symlink_to(self.fixture)
        with self.assertRaisesRegex(pd2_demo.HarnessError, "^controlled_fixture_invalid$"):
            pd2_demo._controlled_fixture_sha256(linked)


class DriverCliTests(unittest.TestCase):
    def test_live_cli_refuses_existing_regular_output_without_replacing_it(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            output = root / "evidence.json"
            output.write_bytes(b"retained evidence\n")
            output.chmod(0o640)
            completed = subprocess.run(
                [
                    sys.executable,
                    os.fspath(DRIVER),
                    "--attestation",
                    os.fspath(root / "missing-attestation.json"),
                    "--controlled-window-fixture",
                    os.fspath(root / "missing-fixture"),
                    "--output",
                    os.fspath(output),
                ],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
                timeout=10,
                env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
            )

            self.assertEqual(completed.returncode, 2)
            self.assertEqual(completed.stdout, b"")
            self.assertEqual(
                completed.stderr,
                b"PD2 installed demonstration failed closed: output_target_exists\n",
            )
            self.assertEqual(output.read_bytes(), b"retained evidence\n")
            self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o640)

    def test_actual_attested_script_validation_entrypoint(self) -> None:
        document = pd2_demo.run_harness(
            REVISION, HARNESS_MANIFEST_SHA256, FakeLivePort(executable_hashes())
        )
        with tempfile.TemporaryDirectory() as temporary:
            evidence = Path(temporary).resolve() / "evidence.json"
            pd2_demo.write_evidence(evidence, document)
            completed = subprocess.run(
                [
                    sys.executable,
                    os.fspath(DRIVER),
                    "--validate-evidence",
                    os.fspath(evidence),
                ],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
                timeout=10,
                env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
            )
        self.assertEqual(completed.returncode, 0, completed.stderr.decode("utf-8"))
        self.assertEqual(completed.stdout, b"")
        self.assertEqual(completed.stderr, b"")

    def test_argument_and_dependency_failures_never_echo_private_paths(self) -> None:
        private_marker = "/home/example/private-attestation.json"
        completed = subprocess.run(
            [sys.executable, os.fspath(DRIVER), "--unknown", private_marker],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=10,
            env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
        )
        self.assertEqual(completed.returncode, 2)
        self.assertEqual(
            completed.stderr,
            b"PD2 installed demonstration failed closed: arguments_invalid\n",
        )
        self.assertNotIn(private_marker.encode("ascii"), completed.stderr)

        with mock.patch.object(
            pd2_demo,
            "execute_installed_demonstration",
            side_effect=RuntimeError(private_marker),
        ):
            error = io.StringIO()
            with contextlib.redirect_stderr(error):
                result = pd2_demo.main([
                    "--attestation", private_marker,
                    "--controlled-window-fixture", "/tmp/private-fixture",
                    "--output", "/tmp/private-output",
                ])
        self.assertEqual(result, 2)
        self.assertEqual(
            error.getvalue(),
            "PD2 installed demonstration failed closed: live_execution_failed\n",
        )
        self.assertNotIn(private_marker, error.getvalue())


class ReviewedLiveConstructionTests(unittest.TestCase):
    def test_actual_live_adapter_accepts_reviewed_driver_configuration(self) -> None:
        from pd2_live_xorg import LiveXorgHarnessPort

        with mock.patch.object(pd2_demo, "_require_reviewed_live_tools"):
            port = pd2_demo._build_live_port(
                Path("/var/tmp/prismdrake-controlled-window-fixture")
            )
        self.assertIsInstance(port, LiveXorgHarnessPort)

    def test_tool_preflight_rejects_symlink_and_writable_executable(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            executable = root / "tool"
            executable.write_bytes(b"tool")
            executable.chmod(0o755)
            link = root / "tool-link"
            link.symlink_to(executable)

            with mock.patch.dict(
                pd2_demo.REVIEWED_LIVE_TOOL_PATHS, {"tool": link}, clear=True
            ):
                with self.assertRaisesRegex(pd2_demo.HarnessError, "^live_tool_invalid$"):
                    pd2_demo._require_reviewed_live_tools()

            executable.chmod(0o777)
            with mock.patch.dict(
                pd2_demo.REVIEWED_LIVE_TOOL_PATHS, {"tool": executable}, clear=True
            ):
                with self.assertRaisesRegex(pd2_demo.HarnessError, "^live_tool_invalid$"):
                    pd2_demo._require_reviewed_live_tools()

            executable.chmod(0o755)
            with mock.patch.dict(
                pd2_demo.REVIEWED_LIVE_TOOL_PATHS, {"tool": executable}, clear=True
            ):
                pd2_demo._require_reviewed_live_tools()

    def test_constructs_explicit_system_adapters_with_reviewed_absolute_paths(self) -> None:
        created: dict[str, object] = {}

        class Config:
            def __init__(self, **values) -> None:
                created["config"] = values

        class Operations:
            pass

        class Accessibility:
            pass

        class Port:
            def __init__(self, config, *, operations, accessibility) -> None:
                created["port"] = (config, operations, accessibility)

        fake_module = SimpleNamespace(
            LiveXorgConfig=Config,
            LiveXorgHarnessPort=Port,
            SystemAccessibilityOperations=Accessibility,
            SystemOperations=Operations,
        )
        fixture = Path("/var/tmp/prismdrake-controlled-window-fixture")
        with mock.patch.object(pd2_demo, "_require_reviewed_live_tools"), mock.patch.dict(
            sys.modules, {"pd2_live_xorg": fake_module}
        ):
            pd2_demo._build_live_port(fixture)

        values = created["config"]
        self.assertEqual(values["controlled_window_fixture"], fixture)
        self.assertEqual(
            values["installed_executables"],
            dict(zip(pd2_demo.EXECUTABLE_NAMES, pd2_demo.INSTALLED_EXECUTABLE_PATHS)),
        )
        self.assertEqual(
            set(pd2_demo.REVIEWED_LIVE_TOOL_PATHS),
            {
                "xorg", "xrandr", "openbox", "xprop", "xwininfo",
                "dbus_daemon", "dbus_monitor", "gdbus", "notify_send",
                "xdotool", "xwd", "x11trace",
            },
        )
        self.assertTrue(all(path.is_absolute()
                            for path in pd2_demo.REVIEWED_LIVE_TOOL_PATHS.values()))
        _config, operations, accessibility = created["port"]
        self.assertIsInstance(operations, Operations)
        self.assertIsInstance(accessibility, Accessibility)


if __name__ == "__main__":
    unittest.main()
