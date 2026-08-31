#!/usr/bin/env python3
"""Tests for the explicit Prismdrake validation-tier boundary."""

from __future__ import annotations

import importlib.util
import hashlib
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


def safe_state_paths(*, rust: bool = False) -> dict[str, Path]:
    root = ROOT / "build" / "validation-state-test-double"
    paths = {
        name: root / name
        for name in ("home", "tmp", "cache", "config", "data", "state", "runtime")
    }
    if rust:
        paths["cargo_home"] = root / "cargo-home"
        paths["target"] = root / "target"
    return paths


def reviewed_tool_paths() -> dict[str, Path]:
    return {
        name: path.resolve(strict=True)
        for name, (path, _) in validation_tiers.RUST_TOOL_SPECS.items()
    }


def materialize_workspace(root: Path) -> dict[str, str]:
    expected: dict[str, str] = {}
    for relative in validation_tiers.RUST_WORKSPACE_FILES:
        data = (ROOT / relative).read_bytes()
        destination = root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
        expected[relative] = hashlib.sha256(data).hexdigest()
    return expected


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

    def test_local_selector_uses_isolated_python_environment(self) -> None:
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
                validation_tiers,
                "_execute_command",
                return_value=17,
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
        self.assertFalse(kwargs)

        environment = args[1]
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

    def test_command_execution_is_noninteractive_bounded_and_shell_free(self) -> None:
        process = mock.Mock(pid=12345)
        process.wait.return_value = 0
        environment = {"HOME": str(ROOT / "build" / "validation-state-test-double")}
        with mock.patch.object(
            validation_tiers.subprocess,
            "Popen",
            return_value=process,
        ) as popen:
            result = validation_tiers._execute_command(("/trusted/tool", "check"), environment)

        self.assertEqual(result, 0)
        popen.assert_called_once()
        args, kwargs = popen.call_args
        self.assertEqual(args[0], ("/trusted/tool", "check"))
        self.assertEqual(kwargs["cwd"], ROOT)
        self.assertEqual(kwargs["env"], environment)
        self.assertIs(kwargs["stdin"], subprocess.DEVNULL)
        self.assertIs(kwargs["stdout"], subprocess.DEVNULL)
        self.assertIs(kwargs["stderr"], subprocess.DEVNULL)
        self.assertTrue(kwargs["close_fds"])
        self.assertTrue(kwargs["start_new_session"])
        self.assertEqual(kwargs["umask"], 0o077)
        self.assertNotIn("shell", kwargs)
        process.wait.assert_called_once_with(timeout=300)

    def test_command_timeout_kills_the_private_process_group(self) -> None:
        process = mock.Mock(pid=12345)
        process.wait.side_effect = (subprocess.TimeoutExpired((), 300), -9)
        with (
            mock.patch.object(
                validation_tiers.subprocess,
                "Popen",
                return_value=process,
            ),
            mock.patch.object(validation_tiers.os, "killpg") as killpg,
        ):
            with self.assertRaises(subprocess.TimeoutExpired):
                validation_tiers._execute_command(("/trusted/tool",), {})
        killpg.assert_called_once_with(12345, validation_tiers.signal.SIGKILL)

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
                validation_tiers,
                "_execute_command",
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

    def test_rust_selector_uses_fixed_argv_tools_and_isolated_environment(self) -> None:
        state_paths = safe_state_paths(rust=True)
        tools = reviewed_tool_paths()
        poisoned = {
            "PATH": "/poisoned/bin:.",
            "CARGO_HOME": "/poisoned/cargo",
            "CARGO_TARGET_DIR": "/outside/target",
            "CARGO_BUILD_RUSTC_WRAPPER": "/poisoned/wrapper",
            "RUSTC_WRAPPER": "/poisoned/wrapper",
            "RUSTC_WORKSPACE_WRAPPER": "/poisoned/wrapper",
            "RUSTFLAGS": "-C linker=/poisoned/linker",
            "RUSTDOCFLAGS": "--extern-html-root-url secret",
            "RUSTUP_HOME": "/poisoned/rustup",
            "CARGO_REGISTRIES_CRATES_IO_TOKEN": "secret",
            "SSH_AUTH_SOCK": "/poisoned/agent.sock",
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
                "_trusted_rust_toolchain",
                return_value=tools,
            ),
            mock.patch.object(validation_tiers, "_validate_rust_workspace"),
            mock.patch.object(
                validation_tiers,
                "_prepare_state_paths",
                return_value=state_paths,
            ) as prepare_state,
            mock.patch.object(validation_tiers, "_validate_private_state_tree"),
            mock.patch.object(
                validation_tiers,
                "_execute_command",
                return_value=0,
            ) as execute,
            redirect_stdout(output),
        ):
            result = validation_tiers.main(
                ["--selector", "rust-check", "--tier", "T0", "--run"]
            )

        self.assertEqual(result, 0)
        prepare_state.assert_called_once_with("rust-check")
        command, environment = execute.call_args.args
        self.assertEqual(
            command,
            (
                str(tools["cargo"]),
                "check",
                "--workspace",
                "--all-targets",
                "--locked",
                "--offline",
            ),
        )
        self.assertEqual(environment["PATH"], str(tools["cargo"].parent))
        self.assertEqual(environment["CARGO"], str(tools["cargo"]))
        self.assertEqual(environment["RUSTC"], str(tools["rustc"]))
        self.assertEqual(Path(environment["RUSTC"]).name, "rustc")
        self.assertEqual(environment["RUSTDOC"], str(tools["rustdoc"]))
        self.assertEqual(environment["RUSTFMT"], str(tools["rustfmt"]))
        self.assertEqual(environment["CARGO_NET_OFFLINE"], "true")
        self.assertEqual(environment["CARGO_HOME"], str(state_paths["cargo_home"]))
        self.assertEqual(environment["CARGO_TARGET_DIR"], str(state_paths["target"]))
        self.assertEqual(
            environment["CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER"],
            str(tools["linker"]),
        )
        for name in poisoned:
            if name not in {"PATH", "CARGO_HOME", "CARGO_TARGET_DIR", "TMPDIR"}:
                self.assertNotIn(name, environment)
        for name in ("HOME", "TMPDIR", "CARGO_HOME", "CARGO_TARGET_DIR"):
            Path(environment[name]).relative_to(ROOT)
        self.assertEqual(
            output.getvalue(),
            "T0 rust-check: running\nT0 rust-check: passed\n",
        )

    def test_rust_selector_argv_is_fixed_locked_and_offline(self) -> None:
        commands = {
            entry.name: entry.command
            for entry in validation_tiers.SELECTORS
            if entry.name in validation_tiers.RUST_SELECTOR_NAMES
        }
        self.assertEqual(
            commands["rust-format"],
            (validation_tiers.CARGO_EXECUTABLE, "fmt", "--all", "--check"),
        )
        for selector, subcommand in (
            ("rust-check", "check"),
            ("rust-test", "test"),
            ("rust-clippy", "clippy"),
        ):
            with self.subTest(selector=selector):
                command = commands[selector]
                self.assertEqual(
                    command[0:2], (validation_tiers.CARGO_EXECUTABLE, subcommand)
                )
                self.assertIn("--workspace", command)
                self.assertIn("--all-targets", command)
                self.assertIn("--locked", command)
                self.assertIn("--offline", command)
        self.assertEqual(commands["rust-clippy"][-3:], ("--", "-D", "warnings"))

    def test_reviewed_executable_rejects_substitution_and_writability(self) -> None:
        cargo_path, cargo_hash = validation_tiers.RUST_TOOL_SPECS["cargo"]
        with self.assertRaisesRegex(validation_tiers.BoundaryError, "hash does not match"):
            validation_tiers._trusted_executable(cargo_path, "0" * 64)
        with self.assertRaisesRegex(validation_tiers.BoundaryError, "incompatible basename"):
            validation_tiers._trusted_executable(
                cargo_path,
                cargo_hash,
                expected_basename="rustc",
            )

        with tempfile.TemporaryDirectory(
            prefix="build-validation-tool-test-", dir=ROOT
        ) as temporary:
            executable = Path(temporary) / "cargo"
            executable.write_bytes(b"not an executable")
            executable.chmod(0o777)
            with self.assertRaisesRegex(validation_tiers.BoundaryError, "world-writable"):
                validation_tiers._trusted_executable(
                    executable,
                    hashlib.sha256(executable.read_bytes()).hexdigest(),
                    writable_root=ROOT / "unrelated-boundary",
                )
            executable.chmod(0o755)
            with self.assertRaisesRegex(
                validation_tiers.BoundaryError, "writable by the invoking user"
            ):
                validation_tiers._trusted_executable(
                    executable,
                    hashlib.sha256(executable.read_bytes()).hexdigest(),
                    writable_root=ROOT / "unrelated-boundary",
                )

    def test_reviewed_rust_toolchain_resolves_with_rustc_compatible_basename(self) -> None:
        tools = validation_tiers._trusted_rust_toolchain()
        self.assertEqual(tools["rustc"].name, "rustc")
        self.assertEqual(tools["cargo"].parent, tools["cargo-clippy"].parent)
        self.assertEqual(tools["cargo"].parent, tools["cargo-fmt"].parent)

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

    def test_rust_state_rejects_symlinked_cargo_home(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="build-validation-tier-test-", dir=ROOT
        ) as temporary:
            temporary_path = Path(temporary)
            state_root = temporary_path / "state"
            selector_root = state_root / "rust-check"
            target = temporary_path / "target"
            state_root.mkdir(mode=0o700)
            selector_root.mkdir(mode=0o700)
            target.mkdir(mode=0o700)
            (selector_root / "cargo-home").symlink_to(target, target_is_directory=True)

            with mock.patch.object(validation_tiers, "STATE_ROOT", state_root):
                with self.assertRaisesRegex(
                    validation_tiers.BoundaryError, "symbolic link"
                ):
                    validation_tiers._prepare_state_paths("rust-check")

    def test_reviewed_workspace_contract_accepts_exact_dependency_free_tree(self) -> None:
        validation_tiers._validate_rust_workspace()

    def test_workspace_dependency_and_build_script_drift_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="build-validation-workspace-test-", dir=ROOT
        ) as temporary:
            workspace = Path(temporary)
            expected = materialize_workspace(workspace)

            manifest = workspace / "Cargo.toml"
            manifest.write_text(
                manifest.read_text(encoding="utf-8")
                + "\n[workspace.dependencies]\nserde = \"1\"\n",
                encoding="utf-8",
            )
            expected["Cargo.toml"] = hashlib.sha256(manifest.read_bytes()).hexdigest()
            with self.assertRaisesRegex(
                validation_tiers.BoundaryError,
                "workspace_dependency_surface_detected",
            ):
                validation_tiers._validate_rust_workspace(workspace, expected)

            manifest.write_bytes((ROOT / "Cargo.toml").read_bytes())
            expected["Cargo.toml"] = hashlib.sha256(manifest.read_bytes()).hexdigest()
            build_script = workspace / "crates" / "prismdrake-foundation" / "build.rs"
            build_script.write_text("fn main() {}\n", encoding="utf-8")
            with self.assertRaisesRegex(
                validation_tiers.BoundaryError,
                "workspace file boundary changed",
            ):
                validation_tiers._validate_rust_workspace(workspace, expected)

    def test_workspace_unsafe_native_and_proc_macro_drift_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="build-validation-workspace-test-", dir=ROOT
        ) as temporary:
            workspace = Path(temporary)
            expected = materialize_workspace(workspace)
            source = workspace / "crates" / "prismdrake-foundation" / "src" / "lib.rs"
            source.write_text(
                source.read_text(encoding="utf-8")
                + "\nunsafe extern \"C\" fn unreviewed() {}\n",
                encoding="utf-8",
            )
            relative = "crates/prismdrake-foundation/src/lib.rs"
            expected[relative] = hashlib.sha256(source.read_bytes()).hexdigest()
            with self.assertRaisesRegex(
                validation_tiers.BoundaryError,
                "unreviewed_unsafe_or_native_surface",
            ):
                validation_tiers._validate_rust_workspace(workspace, expected)

            source.write_bytes((ROOT / relative).read_bytes())
            expected[relative] = hashlib.sha256(source.read_bytes()).hexdigest()
            settings_source = (
                workspace
                / "crates"
                / "prismdrake-foundation"
                / "src"
                / "settings.rs"
            )
            settings_source.write_text(
                settings_source.read_text(encoding="utf-8")
                + '\nextern "C" fn unreviewed_module_boundary() {}\n',
                encoding="utf-8",
            )
            settings_relative = (
                "crates/prismdrake-foundation/src/settings.rs"
            )
            expected[settings_relative] = hashlib.sha256(
                settings_source.read_bytes()
            ).hexdigest()
            with self.assertRaisesRegex(
                validation_tiers.BoundaryError,
                "unreviewed_unsafe_or_native_surface",
            ):
                validation_tiers._validate_rust_workspace(workspace, expected)

            settings_source.write_bytes((ROOT / settings_relative).read_bytes())
            expected[settings_relative] = hashlib.sha256(
                settings_source.read_bytes()
            ).hexdigest()
            crate_manifest = workspace / "crates" / "prismdrake-foundation" / "Cargo.toml"
            crate_manifest.write_text(
                crate_manifest.read_text(encoding="utf-8").replace(
                    '[lib]\npath = "src/lib.rs"',
                    '[lib]\npath = "src/lib.rs"\nproc-macro = true',
                ),
                encoding="utf-8",
            )
            manifest_relative = "crates/prismdrake-foundation/Cargo.toml"
            expected[manifest_relative] = hashlib.sha256(
                crate_manifest.read_bytes()
            ).hexdigest()
            with self.assertRaisesRegex(
                validation_tiers.BoundaryError,
                "proc_macro_surface_detected",
            ):
                validation_tiers._validate_rust_workspace(workspace, expected)

    def test_t2_selectors_refuse_unbound_build_execution(self) -> None:
        for selector in ("compiled-gcc", "qml-lint"):
            with self.subTest(selector=selector):
                error = io.StringIO()
                with (
                    mock.patch.object(validation_tiers, "_execute_command") as run,
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
                    mock.patch.object(validation_tiers, "_execute_command") as run,
                    redirect_stderr(error),
                ):
                    result = validation_tiers.main(
                        ["--selector", selector, "--tier", tier, "--run"]
                    )

                self.assertEqual(result, 2)
                self.assertIn(f"refusing {tier} selector", error.getvalue())
                run.assert_not_called()

    def test_tier_mismatch_does_not_run(self) -> None:
        with mock.patch.object(validation_tiers, "_execute_command") as run:
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
                    mock.patch.object(validation_tiers, "_execute_command") as run,
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
