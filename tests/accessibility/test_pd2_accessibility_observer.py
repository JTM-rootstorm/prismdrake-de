#!/usr/bin/env python3
"""Hermetic tests for the source-bound PD2 accessibility observer."""

from __future__ import annotations

import hashlib
import os
import stat
import sys
import tempfile
import unittest
from collections.abc import Mapping
from pathlib import Path
from unittest import mock


ACCESSIBILITY = Path(__file__).resolve().parent
INTEGRATION = ACCESSIBILITY.parent / "integration"
sys.path.insert(0, str(ACCESSIBILITY))
sys.path.insert(0, str(INTEGRATION))

import pd2_accessibility_observer as observer  # noqa: E402
import pd2_demo  # noqa: E402


REVISION = "a" * 40
ENVIRONMENT_ID = "gentoo-reference-vm"
BINDING_ENVIRONMENT = {
    "PRISMDRAKE_PD2_SOURCE_REVISION": REVISION,
    "PRISMDRAKE_PD2_REFERENCE_ENVIRONMENT_ID": ENVIRONMENT_ID,
    "PRISMDRAKE_PD2_METHOD_ID": observer.METHOD_ID,
}


class FakeHarnessPort:
    def installed_executable_hashes(self) -> Mapping[str, str]:
        return {
            name: hashlib.sha256(name.encode("ascii")).hexdigest()
            for name in pd2_demo.EXECUTABLE_NAMES
        }

    def run_step(self, phase: str, identifier: str) -> pd2_demo.Observation:
        del phase, identifier
        return pd2_demo.Observation(True, True)

    def capture_process_ownership(self) -> Mapping[str, object]:
        return {role: object() for role in pd2_demo.PROCESS_ROLES}

    def cleanup(self, phase: str) -> pd2_demo.Observation:
        del phase
        return pd2_demo.Observation(True, True)


class AccessibilityObserverTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.document = pd2_demo.run_harness(REVISION, FakeHarnessPort())

    def observed(self, surface: str, check: str) -> dict[str, object]:
        with (
            mock.patch.dict(os.environ, BINDING_ENVIRONMENT, clear=True),
            mock.patch.object(observer, "require_gi_atspi", return_value=object()),
            mock.patch.object(observer, "installed_evidence", return_value=self.document),
        ):
            return observer.accessibility_result(surface, check)

    def test_dispatches_the_exact_fourteen_by_twenty_six_matrix(self) -> None:
        results = {
            (surface, check): self.observed(surface, check)
            for surface in observer.SURFACES
            for check in observer.MATRIX_CHECKS
        }
        self.assertEqual(len(results), 14 * 26)
        self.assertTrue(
            all(result["status"] in {"passed", "skipped"} for result in results.values())
        )
        self.assertEqual(results[("running_task", "state_active")]["status"], "skipped")
        self.assertEqual(
            results[("running_task", "escape_recovery")]["status"], "passed"
        )
        self.assertEqual(
            results[("launcher_results", "enter_activation")]["status"], "passed"
        )
        self.assertEqual(
            results[("panel", "accessible_description")]["reason_code"],
            "live_adapter_unimplemented",
        )

    def test_restart_focus_is_never_inferred_from_process_recovery(self) -> None:
        for check in set(observer.MATRIX_CHECKS) - {
            "accessible_name",
            "accessible_role",
        }:
            result = self.observed("service_restart_focus_recovery", check)
            self.assertEqual(result["status"], "skipped")
            self.assertTrue(result["reviewed"])
            self.assertEqual(result["reason_code"], "live_adapter_unimplemented")
            self.assertIn("post restart keyboard focus", result["reason"])
        self.assertEqual(
            self.observed("service_restart_focus_recovery", "accessible_name")[
                "status"
            ],
            "passed",
        )
        self.assertEqual(
            self.observed("service_restart_focus_recovery", "accessible_role")[
                "status"
            ],
            "passed",
        )

    def test_downgrades_indirect_or_non_atspi_state_inferences(self) -> None:
        downgraded = (
            ("pinned_task", "accessible_name"),
            ("pinned_task", "arrow_navigation"),
            ("running_task", "state_active"),
            ("running_task", "state_minimized"),
            ("grouped_task", "state_expanded"),
            ("group_popup", "accessible_role"),
            ("launcher", "state_expanded"),
            ("launcher_results", "state_focused"),
            ("launcher_results", "state_selected"),
            ("transient_notification", "enter_activation"),
            ("transient_notification", "state_urgent"),
            ("notification_center", "state_expanded"),
            ("quick_settings", "state_expanded"),
            ("logout_confirmation", "state_expanded"),
        )
        for surface, check in downgraded:
            with self.subTest(surface=surface, check=check):
                result = self.observed(surface, check)
                self.assertEqual(result["status"], "skipped")
                self.assertEqual(
                    result["reason_code"], "live_adapter_unimplemented"
                )

    def test_live_atspi_requires_all_installed_tier_c_observations(self) -> None:
        with (
            mock.patch.dict(os.environ, BINDING_ENVIRONMENT, clear=True),
            mock.patch.object(observer, "require_gi_atspi", return_value=object()),
            mock.patch.object(observer, "installed_evidence", return_value=self.document),
        ):
            result = observer.global_result("live_atspi")
        self.assertEqual(result["status"], "passed")
        self.assertIn("GI AT SPI", result["method"])

        incomplete = dict(self.document)
        incomplete["tier_c_replay"] = self.document["tier_c_replay"][:-1]
        with (
            mock.patch.dict(os.environ, BINDING_ENVIRONMENT, clear=True),
            mock.patch.object(observer, "require_gi_atspi", return_value=object()),
            mock.patch.object(observer, "installed_evidence", return_value=incomplete),
            self.assertRaisesRegex(
                observer.AccessibilityObserverError,
                "^installed_demo_observation_missing$",
            ),
        ):
            observer.global_result("live_atspi")

    def test_screen_reader_absence_is_an_explicit_dependency_skip(self) -> None:
        with mock.patch.object(Path, "is_file", return_value=False):
            result = observer.global_result("screen_reader_workflow")
        self.assertEqual(result["status"], "skipped")
        self.assertTrue(result["reviewed"])
        self.assertEqual(result["reason_code"], "dependency_unavailable")

    def test_installed_evidence_is_validated_and_revision_bound(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            evidence = Path(temporary) / "installed.json"
            pd2_demo.write_evidence(evidence, self.document)
            environment = {
                **BINDING_ENVIRONMENT,
                "PRISMDRAKE_PD2_INSTALLED_DEMO_EVIDENCE": str(evidence),
            }
            with mock.patch.dict(os.environ, environment, clear=True):
                self.assertEqual(observer.installed_evidence(), self.document)

            environment["PRISMDRAKE_PD2_SOURCE_REVISION"] = "b" * 40
            with (
                mock.patch.dict(os.environ, environment, clear=True),
                self.assertRaisesRegex(
                    observer.AccessibilityObserverError,
                    "^installed_demo_revision_mismatch$",
                ),
            ):
                observer.installed_evidence()

    def test_invalid_matrix_key_fails_closed_and_source_uses_gi(self) -> None:
        with self.assertRaisesRegex(
            observer.AccessibilityObserverError, "^accessibility_probe_invalid$"
        ):
            observer.accessibility_result("panel", "invented_check")
        source = Path(observer.__file__).read_text(encoding="utf-8")
        self.assertIn("from gi.repository import Atspi", source)
        self.assertNotIn("import pyatspi", source)

    def test_direct_collector_invocation_needs_no_role_prefix(self) -> None:
        executable = Path(observer.__file__)
        self.assertTrue(executable.stat().st_mode & stat.S_IXUSR)
        with mock.patch.object(
            sys,
            "argv",
            [
                str(executable),
                "--surface",
                "running_task",
                "--check",
                "escape_recovery",
            ],
        ):
            options = observer.parse_arguments()
        self.assertEqual(options.role, "accessibility")
        self.assertEqual(options.surface, "running_task")
        self.assertEqual(options.check, "escape_recovery")


if __name__ == "__main__":
    unittest.main()
