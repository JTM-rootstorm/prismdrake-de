#!/usr/bin/env python3
"""Hermetic tests for the installed-session WP15 timing collector."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest import mock

import pd2_runtime_performance as runtime


REVISION = "a" * 40
ENVIRONMENT_ID = "gentoo-reference-vm"
HASHES = {
    "prismdrake-session": "1" * 64,
    "prismdrake-settingsd": "2" * 64,
    "prismdrake-notifyd": "3" * 64,
    "prismdrake-shell": "4" * 64,
    "prismdrake-shellctl": "5" * 64,
}


class Observation:
    observed = True
    passed = True
    result = "expected_state_observed"


class FakePort:
    def __init__(self) -> None:
        self.steps: list[str] = []
        self.cleanup_calls = 0

    def installed_executable_hashes(self) -> dict[str, str]:
        return HASHES

    def run_step(self, phase: str, identifier: str) -> Observation:
        self.assert_phase = phase
        self.steps.append(identifier)
        return Observation()

    def cleanup(self, phase: str) -> Observation:
        self.assert_cleanup_phase = phase
        self.cleanup_calls += 1
        return Observation()


def installed_demo() -> dict[str, object]:
    return {
        "source_revision": REVISION,
        "installed_executables": [
            {"name": name, "sha256": digest} for name, digest in HASHES.items()
        ],
    }


class RuntimePerformanceTests(unittest.TestCase):
    def test_collects_every_live_endpoint_with_fresh_cleanup(self) -> None:
        ports: list[FakePort] = []
        ticks = iter(range(0, 100_000_000, 1000))

        def factory(_fixture: Path) -> FakePort:
            port = FakePort()
            ports.append(port)
            return port

        with (
            tempfile.TemporaryDirectory() as temporary,
            mock.patch.object(runtime, "_pd2_demo") as demo,
        ):
            fixture = Path(temporary) / "fixture"
            fixture.write_bytes(b"fixture")
            demo.return_value.validate_evidence.return_value = None
            demo.return_value._controlled_fixture_sha256.return_value = "f" * 64
            demo.return_value.WORKFLOW_STEPS = (
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
                "create_normal_notification",
                "create_replacement_notification",
                "invoke_live_notification_action",
                "open_notification_center",
                "toggle_do_not_disturb",
                "verify_history_without_transient",
                "verify_critical_bypass_policy",
                "clear_notification_history",
                "open_quick_settings",
                "switch_lustre_to_forge",
                "verify_complete_settings_generation",
                "switch_back_preserving_accessibility",
                "remove_current_primary_output",
                "verify_surface_migration",
                "readd_removed_output",
                "restart_settingsd",
            )
            document = runtime.collect_runtime_evidence(
                REVISION,
                ENVIRONMENT_ID,
                installed_demo(),
                fixture,
                3,
                port_factory=factory,
                clock_ns=lambda: next(ticks),
            )

        runtime.validate_runtime_evidence(document)
        self.assertEqual(
            tuple(item["name"] for item in document["measurements"]),
            runtime.ENDPOINTS,
        )
        common = next(
            item for item in document["measurements"]
            if item["name"] == "common_action_acknowledgement"
        )
        self.assertEqual(len(common["samples_ns"]), 9)
        self.assertEqual(len(ports), 3)
        self.assertTrue(all(port.cleanup_calls == 1 for port in ports))
        self.assertTrue(all(port.steps[-1] == "readd_removed_output" for port in ports))

    def test_cleanup_failure_masks_a_measurement_failure(self) -> None:
        class FailingPort(FakePort):
            def run_step(self, phase: str, identifier: str) -> Observation:
                raise RuntimeError("private failure")

            def cleanup(self, phase: str) -> object:
                raise RuntimeError("private cleanup")

        with (
            tempfile.TemporaryDirectory() as temporary,
            mock.patch.object(runtime, "_pd2_demo") as demo,
        ):
            fixture = Path(temporary) / "fixture"
            fixture.write_bytes(b"fixture")
            demo.return_value.validate_evidence.return_value = None
            demo.return_value._controlled_fixture_sha256.return_value = "f" * 64
            demo.return_value.WORKFLOW_STEPS = (
                "start_isolated_two_output_x11",
                "restart_settingsd",
            )
            with self.assertRaisesRegex(
                runtime.RuntimePerformanceError, "^runtime_cleanup_failed$"
            ):
                runtime.collect_runtime_evidence(
                    REVISION,
                    ENVIRONMENT_ID,
                    installed_demo(),
                    fixture,
                    3,
                    port_factory=lambda _path: FailingPort(),
                )


if __name__ == "__main__":
    unittest.main()
