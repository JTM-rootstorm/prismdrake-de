#!/usr/bin/env python3
"""Tests for the explicit Prismdrake validation-tier boundary."""

from __future__ import annotations

import importlib.util
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "tools" / "validation_tiers.py"
SPEC = importlib.util.spec_from_file_location("prismdrake_validation_tiers", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
validation_tiers = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = validation_tiers
SPEC.loader.exec_module(validation_tiers)


def safe_state_paths() -> dict[str, Path]:
    root = ROOT / "build" / "validation-state-test-double"
    return {
        name: root / name
        for name in ("home", "tmp", "cache", "config", "data", "state", "runtime")
    }


class ValidationTierTests(unittest.TestCase):
    def test_all_tiers_are_present_and_names_are_unique(self) -> None:
        self.assertEqual(
            {entry.tier for entry in validation_tiers.SELECTORS},
            {"T0", "T1", "T2", "T3", "T4"},
        )
        names = [entry.name for entry in validation_tiers.SELECTORS]
        self.assertEqual(len(names), len(set(names)))

    def test_t2_and_external_tiers_have_no_local_command(self) -> None:
        blocked = [
            entry
            for entry in validation_tiers.SELECTORS
            if entry.tier in {"T2", "T3", "T4"}
        ]
        self.assertEqual(len(blocked), 4)
        for entry in blocked:
            self.assertIsNone(entry.command)
            self.assertFalse(entry.runnable_locally)
            self.assertTrue(entry.blocked_reason)

        for entry in blocked:
            if entry.tier == "T2":
                self.assertIn("committed tier manifest", entry.blocked_reason)
                self.assertIn("exact build identity", entry.blocked_reason)

    def test_local_selector_uses_isolated_python_environment_and_streams(self) -> None:
        completed = subprocess.CompletedProcess(args=(), returncode=17)
        trusted_python = Path(sys.executable).resolve(strict=True)
        state_paths = safe_state_paths()
        poisoned = {
            "PATH": "/poisoned/bin:.",
            "PYTHONPATH": "/poisoned/python",
            "PYTHONHOME": "/poisoned/home",
            "PYTHONUSERBASE": "/poisoned/user-site",
            "LD_PRELOAD": "/poisoned/loader.so",
            "SSH_AUTH_SOCK": "/poisoned/agent.sock",
            "AWS_SECRET_ACCESS_KEY": "secret",
            "HTTPS_PROXY": "https://proxy.invalid",
            "DBUS_SESSION_BUS_ADDRESS": "unix:path=/poisoned/bus",
            "DISPLAY": ":99",
            "TMPDIR": "/outside/tmp",
        }
        output = io.StringIO()

        with (
            mock.patch.dict(os.environ, poisoned, clear=True),
            mock.patch.object(
                validation_tiers,
                "_trusted_python_executable",
                return_value=trusted_python,
            ),
            mock.patch.object(
                validation_tiers,
                "_prepare_state_paths",
                return_value=state_paths,
            ) as prepare_state,
            mock.patch.object(
                validation_tiers.subprocess,
                "run",
                return_value=completed,
            ) as run,
            redirect_stdout(output),
        ):
            result = validation_tiers.main(
                ["--selector", "contracts", "--tier", "T0", "--run"]
            )

        self.assertEqual(result, 17)
        prepare_state.assert_called_once_with("contracts")
        run.assert_called_once()
        args, kwargs = run.call_args
        self.assertEqual(
            args[0],
            (str(trusted_python), "-I", "-B", "tools/validate.py"),
        )
        self.assertEqual(kwargs["cwd"], ROOT)
        self.assertFalse(kwargs["check"])
        self.assertIs(kwargs["stdin"], subprocess.DEVNULL)
        self.assertIs(kwargs["stdout"], subprocess.DEVNULL)
        self.assertIs(kwargs["stderr"], subprocess.DEVNULL)
        self.assertTrue(kwargs["close_fds"])
        self.assertEqual(kwargs["timeout"], 300)
        self.assertNotIn("shell", kwargs)

        environment = kwargs["env"]
        self.assertEqual(
            set(environment),
            {
                "HOME",
                "TMPDIR",
                "TMP",
                "TEMP",
                "XDG_CACHE_HOME",
                "XDG_CONFIG_HOME",
                "XDG_DATA_HOME",
                "XDG_STATE_HOME",
                "XDG_RUNTIME_DIR",
                "LANG",
                "LC_ALL",
                "TZ",
            },
        )
        for name in poisoned:
            if name != "TMPDIR":
                self.assertNotIn(name, environment)
        self.assertEqual(environment["TMPDIR"], str(state_paths["tmp"]))
        self.assertNotEqual(environment["TMPDIR"], poisoned["TMPDIR"])
        for name in (
            "HOME",
            "TMPDIR",
            "XDG_CACHE_HOME",
            "XDG_CONFIG_HOME",
            "XDG_DATA_HOME",
            "XDG_STATE_HOME",
            "XDG_RUNTIME_DIR",
        ):
            Path(environment[name]).relative_to(ROOT)

        self.assertEqual(
            output.getvalue(),
            "T0 contracts: running\nT0 contracts: failed (17)\n",
        )

    def test_local_selector_timeout_has_bounded_output(self) -> None:
        trusted_python = Path(sys.executable).resolve(strict=True)
        output = io.StringIO()
        with (
            mock.patch.object(
                validation_tiers,
                "_trusted_python_executable",
                return_value=trusted_python,
            ),
            mock.patch.object(
                validation_tiers,
                "_prepare_state_paths",
                return_value=safe_state_paths(),
            ),
            mock.patch.object(
                validation_tiers.subprocess,
                "run",
                side_effect=subprocess.TimeoutExpired((), 300),
            ),
            redirect_stdout(output),
        ):
            result = validation_tiers.main(
                ["--selector", "contracts", "--tier", "T0", "--run"]
            )

        self.assertEqual(result, 124)
        self.assertEqual(
            output.getvalue(),
            "T0 contracts: running\nT0 contracts: timed out\n",
        )

    def test_nonabsolute_python_executable_is_rejected(self) -> None:
        with mock.patch.object(
            validation_tiers, "PYTHON_EXECUTABLE_INPUT", "python3"
        ):
            with self.assertRaises(validation_tiers.BoundaryError):
                validation_tiers._trusted_python_executable()

    def test_state_path_symlink_is_rejected_inside_project_test_state(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="build-validation-tier-test-", dir=ROOT
        ) as temporary:
            temporary_path = Path(temporary)
            state_root = temporary_path / "state"
            selector_root = state_root / "contracts"
            target = temporary_path / "target"
            state_root.mkdir(mode=0o700)
            selector_root.mkdir(mode=0o700)
            target.mkdir(mode=0o700)
            (selector_root / "home").symlink_to(target, target_is_directory=True)

            with mock.patch.object(validation_tiers, "STATE_ROOT", state_root):
                with self.assertRaisesRegex(
                    validation_tiers.BoundaryError, "symbolic link"
                ):
                    validation_tiers._prepare_state_paths("contracts")

    def test_t2_selectors_refuse_unbound_build_execution(self) -> None:
        for selector in ("compiled-gcc", "qml-lint"):
            with self.subTest(selector=selector):
                error = io.StringIO()
                with (
                    mock.patch.object(validation_tiers.subprocess, "run") as run,
                    redirect_stderr(error),
                ):
                    result = validation_tiers.main(
                        ["--selector", selector, "--tier", "T2", "--run"]
                    )

                self.assertEqual(result, 2)
                self.assertIn("committed tier manifest", error.getvalue())
                self.assertIn("exact build identity", error.getvalue())
                run.assert_not_called()

    def test_t3_and_t4_selectors_refuse_to_run(self) -> None:
        for selector, tier in (
            ("package-checkpoint", "T3"),
            ("live-environment", "T4"),
        ):
            with self.subTest(selector=selector):
                error = io.StringIO()
                with (
                    mock.patch.object(validation_tiers.subprocess, "run") as run,
                    redirect_stderr(error),
                ):
                    result = validation_tiers.main(
                        ["--selector", selector, "--tier", tier, "--run"]
                    )

                self.assertEqual(result, 2)
                self.assertIn(f"refusing {tier} selector", error.getvalue())
                run.assert_not_called()

    def test_tier_mismatch_does_not_run(self) -> None:
        with mock.patch.object(validation_tiers.subprocess, "run") as run:
            with redirect_stderr(io.StringIO()):
                result = validation_tiers.main(
                    ["--selector", "contracts", "--tier", "T1", "--run"]
                )

        self.assertEqual(result, 2)
        run.assert_not_called()

    def test_ambiguous_cli_modes_are_rejected(self) -> None:
        cases = (
            ["--list", "--run"],
            ["--list", "--selector", "contracts"],
            ["--selector", "contracts", "--json"],
            ["--selector", "contracts", "--json", "--run"],
            ["--list", "--json", "--run"],
        )
        for arguments in cases:
            with self.subTest(arguments=arguments):
                with (
                    mock.patch.object(validation_tiers.subprocess, "run") as run,
                    redirect_stdout(io.StringIO()),
                    redirect_stderr(io.StringIO()),
                ):
                    with self.assertRaises(SystemExit) as raised:
                        validation_tiers.main(arguments)
                self.assertEqual(raised.exception.code, 2)
                run.assert_not_called()

    def test_json_listing_is_bounded_and_machine_readable(self) -> None:
        output = io.StringIO()
        with redirect_stdout(output):
            result = validation_tiers.main(["--list", "--tier", "T2", "--json"])

        self.assertEqual(result, 0)
        records = json.loads(output.getvalue())
        self.assertEqual(len(records), 2)
        for record in records:
            self.assertIsNone(record["command"])
            self.assertFalse(record["runnable_locally"])
            self.assertIn("committed tier manifest", record["blocked_reason"])


if __name__ == "__main__":
    unittest.main()
