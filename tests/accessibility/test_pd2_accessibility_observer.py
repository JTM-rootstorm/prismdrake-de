#!/usr/bin/env python3
"""Hermetic tests for the source-bound PD2 accessibility observer."""

from __future__ import annotations

import hashlib
import json
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
        cls.document = pd2_demo.run_harness(REVISION, "b" * 64, FakeHarnessPort())

    def observed(self, surface: str, check: str) -> dict[str, object]:
        with (
            mock.patch.dict(os.environ, BINDING_ENVIRONMENT, clear=True),
            mock.patch.object(observer, "require_gi_atspi", return_value=object()),
            mock.patch.object(observer, "installed_evidence", return_value=self.document),
        ):
            return observer.accessibility_result(surface, check, {})

    def test_dispatches_the_exact_fourteen_by_twenty_six_matrix(self) -> None:
        results = {
            (surface, check): self.observed(surface, check)
            for surface in observer.SURFACES
            for check in observer.MATRIX_CHECKS
        }
        self.assertEqual(len(results), 14 * 26)
        self.assertEqual(len(observer.OBSERVED), 49)
        self.assertTrue(
            all(result["status"] in {"passed", "skipped"} for result in results.values())
        )
        self.assertEqual(results[("running_task", "long_string_layout")]["status"], "skipped")
        self.assertEqual(
            results[("running_task", "long_string_layout")]["reason_code"],
            "live_adapter_unimplemented",
        )
        self.assertEqual(results[("running_task", "state_active")]["status"], "passed")
        self.assertEqual(
            results[("running_task", "escape_recovery")]["status"], "passed"
        )
        self.assertEqual(
            results[("launcher_results", "enter_activation")]["status"], "passed"
        )
        self.assertEqual(
            results[("panel", "accessible_description")]["status"], "passed"
        )
        self.assertEqual(results[("quick_settings", "tab_backtab")]["status"], "passed")
        self.assertEqual(results[("output_migration", "state_focused")]["status"], "passed")

    def test_restart_focus_is_never_inferred_from_process_recovery(self) -> None:
        for check in set(observer.MATRIX_CHECKS) - {
            "tab_backtab",
            "accessible_name",
            "accessible_role",
            "state_focused",
        }:
            result = self.observed("service_restart_focus_recovery", check)
            self.assertEqual(result["status"], "skipped")
            self.assertTrue(result["reviewed"])
            self.assertEqual(result["reason_code"], "not_applicable")
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
        self.assertEqual(
            self.observed("service_restart_focus_recovery", "state_focused")[
                "status"
            ],
            "passed",
        )
        self.assertEqual(
            self.observed("service_restart_focus_recovery", "tab_backtab")["status"],
            "passed",
        )

    def test_new_live_mappings_require_the_exact_installed_result_ids(self) -> None:
        cases = (
            (
                "verify_accessible_descriptions_states_and_backtab",
                "panel",
                "accessible_description",
            ),
            (
                "verify_output_migration_accessibility",
                "output_migration",
                "state_focused",
            ),
            (
                "verify_settingsd_focus_recovery",
                "service_restart_focus_recovery",
                "tab_backtab",
            ),
            (
                "verify_notifyd_focus_recovery",
                "service_restart_focus_recovery",
                "accessible_name",
            ),
            (
                "verify_shell_focus_recovery",
                "service_restart_focus_recovery",
                "accessible_role",
            ),
        )
        for missing, surface, check in cases:
            with self.subTest(missing=missing, surface=surface, check=check):
                incomplete = dict(self.document)
                incomplete["workflow"] = [
                    record
                    for record in self.document["workflow"]
                    if record["id"] != missing
                ]
                with (
                    mock.patch.dict(os.environ, BINDING_ENVIRONMENT, clear=True),
                    mock.patch.object(observer, "require_gi_atspi", return_value=object()),
                    mock.patch.object(
                        observer, "installed_evidence", return_value=incomplete
                    ),
                    self.assertRaisesRegex(
                        observer.AccessibilityObserverError,
                        "^installed_demo_observation_missing$",
                    ),
                ):
                    observer.accessibility_result(surface, check)

    def test_complete_matrix_has_one_explicit_evidence_class_per_cell(self) -> None:
        matrix = {
            (surface, check)
            for surface in observer.SURFACES
            for check in observer.MATRIX_CHECKS
        }
        live = set(observer.OBSERVED)
        ctest = set(observer.CTEST_OBSERVED)
        not_applicable = set(observer.NOT_APPLICABLE)
        unimplemented = set(observer.UNIMPLEMENTED)
        self.assertEqual(live | ctest | not_applicable | unimplemented, matrix)
        self.assertFalse(live & ctest)
        self.assertFalse(live & not_applicable)
        self.assertFalse(live & unimplemented)
        self.assertFalse(ctest & not_applicable)
        self.assertFalse(ctest & unimplemented)
        self.assertFalse(not_applicable & unimplemented)
        self.assertNotIn("live_adapter_unimplemented", {
            result["reason_code"]
            for key in not_applicable
            if (result := self.observed(*key))["status"] == "skipped"
        })
        for key in unimplemented:
            self.assertEqual(
                self.observed(*key)["reason_code"], "live_adapter_unimplemented"
            )

    def test_new_ctest_cells_name_existing_exact_qml_cases(self) -> None:
        self.assertEqual(len(observer.CTEST_OBSERVED), 161)
        self.assertEqual(len(observer.CTEST_CASE_EVIDENCE), 63)
        self.assertEqual(len(observer.UNIMPLEMENTED), 20)
        sources = {
            "PanelSurfaceRealAdapters": (
                ACCESSIBILITY.parents[1] / "tests/qt/qml-panel/tst_PanelSurface.qml"
            ),
            "LauncherSurfaceRealAdapters": (
                ACCESSIBILITY.parents[1]
                / "tests/qt/launcherqml/qml/tst_LauncherSurface.qml"
            ),
            "NotificationCard": (
                ACCESSIBILITY.parents[1] / "tests/qt/qml/tst_NotificationCard.qml"
            ),
            "NotificationCenterSurface": (
                ACCESSIBILITY.parents[1]
                / "src/shell/notificationcenter/tests/qml"
                / "tst_NotificationCenterSurface.qml"
            ),
            "QuickSettingsSurface": (
                ACCESSIBILITY.parents[1]
                / "src/shell/quicksettings/tests/qml"
                / "tst_QuickSettingsSurface.qml"
            ),
            "LogoutConfirmationSurface": (
                ACCESSIBILITY.parents[1]
                / "src/shell/quicksettings/tests/qml"
                / "tst_LogoutConfirmationSurface.qml"
            ),
        }
        source_text = {
            suite: path.read_text(encoding="utf-8") for suite, path in sources.items()
        }
        for key, cases in observer.CTEST_CASE_EVIDENCE.items():
            with self.subTest(key=key):
                self.assertIn(key, observer.CTEST_OBSERVED)
                result = observer.accessibility_result(
                    *key, package_test={}, installed_document=self.document
                )
                self.assertEqual(result["status"], "passed")
                for case in cases:
                    suite, function = case.split("::", maxsplit=1)
                    self.assertIn(f"function {function}()", source_text[suite])
                    self.assertIn(case, result["method"])

    def test_batch_matrix_validates_shared_inputs_once(self) -> None:
        with (
            mock.patch.dict(os.environ, BINDING_ENVIRONMENT, clear=True),
            mock.patch.object(observer, "require_gi_atspi", return_value=object()),
            mock.patch.object(observer, "installed_evidence", return_value=self.document),
            mock.patch.object(
                observer,
                "package_test_evidence",
                return_value={
                    "artifact_sha256": observer.installed_artifact_sha256(self.document)
                },
            ) as package,
            mock.patch.object(observer, "require_desktop_exclusion_contract") as desktop,
        ):
            result = observer.matrix_result()
        self.assertEqual(len(result["matrix"]), 14)
        self.assertTrue(all(len(entry["checks"]) == 26 for entry in result["matrix"]))
        package.assert_called_once_with()
        desktop.assert_called_once_with(self.document)

        with (
            mock.patch.dict(os.environ, BINDING_ENVIRONMENT, clear=True),
            mock.patch.object(observer, "require_gi_atspi", return_value=object()),
            mock.patch.object(observer, "installed_evidence", return_value=self.document),
            mock.patch.object(
                observer,
                "package_test_evidence",
                return_value={"artifact_sha256": "0" * 64},
            ),
            mock.patch.object(observer, "require_desktop_exclusion_contract"),
            self.assertRaisesRegex(
                observer.AccessibilityObserverError,
                "^package_test_artifact_mismatch$",
            ),
        ):
            observer.matrix_result()

    def test_package_stage_binds_exact_inventory_and_complete_junit_counts(self) -> None:
        inventory_path = (
            ACCESSIBILITY.parents[1] / "tests" / "gentoo"
            / "pd2-ctest-inventory-v1.json"
        )
        inventory_raw = inventory_path.read_bytes()
        inventory = json.loads(inventory_raw)
        ebuild = (
            ACCESSIBILITY.parents[1] / "packaging" / "gentoo" / "repository"
            / "x11-misc" / "prismdrake" / "prismdrake-9999.ebuild"
        )
        selected = inventory["exact_test_count"] - len(inventory["excluded_tests"])
        package = {
            "source_revision": REVISION,
            "ebuild_sha256": hashlib.sha256(ebuild.read_bytes()).hexdigest(),
            "artifact_sha256": "2" * 64,
            "features_test": True,
            "result": "passed",
            "selected_test_count": selected,
            "executed_test_count": selected,
            "passed_test_count": selected - 1,
            "skipped_test_count": 1,
            "skipped_tests": [{
                "test_id": "AtomicFileTest.RejectsDestinationOwnedByAnotherUser",
                "reason": "requires_root_ownership_transition",
            }],
            "disabled_test_count": 0,
            "not_run_test_count": 0,
            "test_manifest_sha256": "3" * 64,
            "inventory_contract_sha256": hashlib.sha256(inventory_raw).hexdigest(),
            "excluded_tests": inventory["excluded_tests"],
        }
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "package.json"
            path.write_text(json.dumps({
                "schema_version": 1,
                "stage_kind": "pd2_package_test",
                "package_test": package,
            }), encoding="utf-8")
            path.chmod(0o600)
            environment = {
                **BINDING_ENVIRONMENT,
                "PRISMDRAKE_PD2_PACKAGE_TEST_EVIDENCE": str(path),
            }
            with mock.patch.dict(os.environ, environment, clear=True):
                self.assertEqual(observer.package_test_evidence(), package)

            for field, value in (
                ("ebuild_sha256", "4" * 64),
                ("executed_test_count", selected - 1),
                ("passed_test_count", selected),
                ("inventory_contract_sha256", "4" * 64),
            ):
                mutated = dict(package)
                mutated[field] = value
                path.write_text(json.dumps({
                    "schema_version": 1,
                    "stage_kind": "pd2_package_test",
                    "package_test": mutated,
                }), encoding="utf-8")
                with (
                    self.subTest(field=field),
                    mock.patch.dict(os.environ, environment, clear=True),
                    self.assertRaisesRegex(
                        observer.AccessibilityObserverError,
                        "^package_test_evidence_invalid$",
                    ),
                ):
                    observer.package_test_evidence()

            path.chmod(0o644)
            with (
                mock.patch.dict(os.environ, environment, clear=True),
                self.assertRaisesRegex(
                    observer.AccessibilityObserverError,
                    "^evidence_file_invalid$",
                ),
            ):
                observer.package_test_evidence()

            path.write_text(
                '{"schema_version":1,"schema_version":1,'
                '"stage_kind":"pd2_package_test","package_test":{}}',
                encoding="utf-8",
            )
            path.chmod(0o600)
            with (
                mock.patch.dict(os.environ, environment, clear=True),
                self.assertRaisesRegex(
                    observer.AccessibilityObserverError,
                    "^package_test_evidence_invalid$",
                ),
            ):
                observer.package_test_evidence()

    def test_desktop_exclusion_is_bound_to_exact_qml_contract(self) -> None:
        observer.require_desktop_exclusion_contract(self.document)
        with (
            mock.patch.object(observer, "read_regular_file", return_value=b"Item {}\n"),
            self.assertRaisesRegex(
                observer.AccessibilityObserverError,
                "^desktop_exclusion_contract_invalid$",
            ),
        ):
            observer.require_desktop_exclusion_contract(self.document)

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
