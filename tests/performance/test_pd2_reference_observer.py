#!/usr/bin/env python3
"""Closed-protocol tests for the composable PD2 reference VM observer."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "accessibility"))
import pd2_accessibility_observer as accessibility_observer  # noqa: E402
import pd2_reference_observer as observer  # noqa: E402
import pd2_runtime_performance as runtime_performance  # noqa: E402


REVISION = "a" * 40
PD1_REVISION = "b" * 40
ENVIRONMENT_ID = "gentoo-reference-vm"
BINDING_ENVIRONMENT = {
    "PRISMDRAKE_PD2_SOURCE_REVISION": REVISION,
    "PRISMDRAKE_PD2_REFERENCE_ENVIRONMENT_ID": ENVIRONMENT_ID,
    "PRISMDRAKE_PD2_METHOD_ID": "pd2-live-v1",
    "PRISMDRAKE_PD2_PD1_REFERENCE_REVISION": PD1_REVISION,
}


def write_build_attestation(
    build: Path, source: Path | None = None, revision: str = REVISION
) -> None:
    source = source or build
    (build / "CMakeCache.txt").write_text(
        f"PRISMDRAKE_CONFIGURED_SOURCE_REVISION:INTERNAL={revision}\n"
        f"CMAKE_HOME_DIRECTORY:INTERNAL={source}\n",
        encoding="utf-8",
    )


def performance_document(revision: str, samples: list[int]) -> dict[str, object]:
    measurements = []
    shape = [
        ("settings_initial_load", None),
        ("profile_switch_publication", None),
        *[
            item
            for size in (16, 64, 256)
            for item in (
                ("desktop_entry_discovery", size),
                ("application_search_response", size),
                ("ewmh_task_model_update", size),
            )
        ],
    ]
    for name, size in shape:
        ordered = sorted(samples)
        measurements.append(
            {
                "name": name,
                "fixture_count": size,
                "statistics": {
                    "sample_count": len(samples),
                    "minimum_ns": ordered[0],
                    "median_ns": ordered[(len(ordered) - 1) // 2],
                    "p95_ns": ordered[-1],
                    "maximum_ns": ordered[-1],
                    "samples_ns": samples,
                },
            }
        )
    return {
        "source_revision": revision,
        "reference_environment_id": ENVIRONMENT_ID,
        "method": {"measured_iterations": len(samples), "fixture_sizes": [16, 64, 256]},
        "measurements": measurements,
    }


def runtime_document(samples: list[int]) -> dict[str, object]:
    return {
        "schema_version": 1,
        "evidence_kind": "pd2_live_runtime_performance",
        "source_revision": REVISION,
        "reference_environment_id": ENVIRONMENT_ID,
        "method": {
            "clock": runtime_performance.CLOCK_METHOD,
            "sample_isolation": "fresh_installed_session_per_trial",
            "endpoint_observation": runtime_performance.ENDPOINT_OBSERVATION_METHOD,
            "installed_demo_binding": "validated_exact_revision_and_executable_hashes",
            "iterations": len(samples),
        },
        "measurements": [
            {
                "name": name,
                "samples_ns": samples * 3
                if name == "common_action_acknowledgement"
                else samples,
            }
            for name in runtime_performance.ENDPOINTS
        ],
        "redaction": {
            "contains_filesystem_paths": False,
            "contains_private_content": False,
            "contains_process_ids": False,
            "contains_x11_window_ids": False,
            "diagnostics_are_closed_ids": True,
        },
    }


def asset_document(samples: list[int]) -> dict[str, object]:
    return {
        "schema_version": 1,
        "evidence_kind": "pd2_asset_performance",
        "source_revision": REVISION,
        "reference_environment_id": ENVIRONMENT_ID,
        "method": {
            "clock": "std_chrono_steady_clock",
            "completion": "production_async_callback_on_context_thread",
            "icon_fixture": "warm_file_cache_controlled_local_png_128",
            "measured_iterations": len(samples),
            "wallpaper_fixture": "warm_file_cache_controlled_local_png_1600x900",
        },
        "measurements": [
            {"name": name, "samples_ns": samples}
            for name in ("icon_decode", "icon_cache_hit", "wallpaper_decode_publication")
        ],
        "redaction": {
            "contains_filesystem_paths": False,
            "contains_private_content": False,
            "diagnostics_are_closed_ids": True,
        },
    }


def idle_document(samples: list[int]) -> dict[str, object]:
    return {
        "schema_version": 1,
        "evidence_kind": "pd2_idle_wakeup_series",
        "source_revision": REVISION,
        "reference_environment_id": ENVIRONMENT_ID,
        "method": {
            "source_evidence_kind": "idle_scheduler_wakeups",
            "sample_interval_seconds": 60,
            "sample_aggregation": "sum_received_wakeups_across_measured_roles",
            "measured_roles": ["session", "settingsd", "notifyd", "shell"],
            "source_trial_ids_sha256": "1" * 64,
            "trial_count": len(samples),
        },
        "samples_wakeups_per_minute": samples,
        "redaction": {
            "contains_filesystem_paths": False,
            "contains_private_content": False,
            "contains_process_ids": False,
            "contains_thread_ids": False,
            "contains_x11_window_ids": False,
            "diagnostics_are_closed_ids": True,
        },
    }


class ReferenceObserverTests(unittest.TestCase):
    def test_fallback_not_applicable_policy_matches_dedicated_observer(self) -> None:
        self.assertEqual(
            observer.NOT_APPLICABLE_ACCESSIBILITY,
            set(accessibility_observer.NOT_APPLICABLE),
        )

    def test_build_runner_must_resolve_to_exact_configured_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            expected = build / "tests" / "performance" / "runner"
            expected.parent.mkdir(parents=True)
            expected.write_bytes(b"runner")
            expected.chmod(0o700)
            with (
                mock.patch.object(
                    observer,
                    "configured_build_context",
                    return_value=(build, Path("/source")),
                ),
                mock.patch.object(observer, "configured_path", return_value=expected),
            ):
                self.assertEqual(
                    observer.configured_build_executable(
                        "RUNNER", Path("tests/performance/runner")
                    ),
                    expected,
                )

            external = build / "external"
            external.write_bytes(b"runner")
            external.chmod(0o700)
            with (
                mock.patch.object(
                    observer,
                    "configured_build_context",
                    return_value=(build, Path("/source")),
                ),
                mock.patch.object(observer, "configured_path", return_value=external),
                self.assertRaisesRegex(
                    observer.ObserverError, "^build_runner_binding_invalid$"
                ),
            ):
                observer.configured_build_executable(
                    "RUNNER", Path("tests/performance/runner")
                )

    def test_cli_emits_exact_bound_reviewed_skip_without_runtime_values(self) -> None:
        executable = Path(observer.__file__).resolve()
        environment = {**BINDING_ENVIRONMENT}
        completed = subprocess.run(
            [
                str(executable),
                "--role",
                "accessibility",
                "--surface",
                "panel",
                "--check",
                "state_checked",
            ],
            env=environment,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=5,
        )
        self.assertEqual(completed.returncode, 0)
        self.assertEqual(completed.stderr, b"")
        self.assertEqual(
            json.loads(completed.stdout),
            {
                "binding": {
                    "source_revision": REVISION,
                    "reference_environment_id": ENVIRONMENT_ID,
                    "method_id": "pd2-live-v1",
                },
                "result": {
                    "status": "skipped",
                    "reviewed": True,
                    "reason_code": "not_applicable",
                    "reason": "The named semantic object does not expose this state",
                },
            },
        )

    def test_unobserved_accessibility_check_never_becomes_a_pass(self) -> None:
        result = observer.accessibility_result("panel", "accessible_name", None)
        self.assertEqual(result["status"], "skipped")
        self.assertEqual(result["reason_code"], "live_adapter_unimplemented")

    def test_ctest_check_requires_a_nonempty_successful_selection(self) -> None:
        completed = subprocess.CompletedProcess(
            [], 0, b"1/4 Test #1: Fixture ........ Passed\n", b""
        )
        with (
            tempfile.TemporaryDirectory() as temporary,
            mock.patch.dict(
                os.environ,
                {
                    **BINDING_ENVIRONMENT,
                    "PRISMDRAKE_PD2_BUILD_DIRECTORY": temporary,
                    "PRISMDRAKE_PD2_CTEST": "/bin/true",
                },
                clear=True,
            ),
            mock.patch.object(observer, "attest_source_checkout"),
            mock.patch.object(observer, "run_command", return_value=completed),
        ):
            write_build_attestation(Path(temporary))
            result = observer.ctest_result("locale_aware_clock")
        self.assertEqual(result["status"], "passed")

        with (
            tempfile.TemporaryDirectory() as temporary,
            mock.patch.dict(
                os.environ,
                {
                    **BINDING_ENVIRONMENT,
                    "PRISMDRAKE_PD2_BUILD_DIRECTORY": temporary,
                    "PRISMDRAKE_PD2_CTEST": "/bin/true",
                },
                clear=True,
            ),
            mock.patch.object(
                observer,
                "run_command",
                return_value=subprocess.CompletedProcess([], 0, b"No tests were found\n", b""),
            ),
            mock.patch.object(observer, "attest_source_checkout"),
            self.assertRaisesRegex(observer.ObserverError, "^ctest_selection_empty$"),
        ):
            write_build_attestation(Path(temporary))
            observer.ctest_result("locale_aware_clock")

    def test_grouped_i18n_checks_require_the_exact_intended_test_selection(self) -> None:
        for check, names in observer.I18N_EXACT_TESTS.items():
            output = "".join(
                f"{index}/{len(names)} Test #{index}: {name} .... Passed\n"
                for index, name in enumerate(sorted(names), 1)
            ).encode("utf-8")
            with (
                self.subTest(check=check),
                tempfile.TemporaryDirectory() as temporary,
                mock.patch.dict(
                    os.environ,
                    {
                        **BINDING_ENVIRONMENT,
                        "PRISMDRAKE_PD2_BUILD_DIRECTORY": temporary,
                        "PRISMDRAKE_PD2_CTEST": "/bin/true",
                    },
                    clear=True,
                ),
                mock.patch.object(
                    observer,
                    "run_command",
                    return_value=subprocess.CompletedProcess([], 0, output, b""),
                ),
                mock.patch.object(observer, "attest_source_checkout"),
            ):
                write_build_attestation(Path(temporary))
                self.assertEqual(observer.ctest_result(check)["status"], "passed")

                missing = output.splitlines(keepends=True)[:-1]
                with (
                    mock.patch.object(
                        observer,
                        "run_command",
                        return_value=subprocess.CompletedProcess(
                            [], 0, b"".join(missing), b""
                        ),
                    ),
                    self.assertRaisesRegex(
                        observer.ObserverError, "^ctest_selection_mismatch$"
                    ),
                ):
                    observer.ctest_result(check)

                extra = output + (
                    f"{len(names) + 1}/{len(names) + 1} Test #{len(names) + 1}: "
                    "UnrelatedTest .... Passed\n"
                ).encode("utf-8")
                with (
                    mock.patch.object(
                        observer,
                        "run_command",
                        return_value=subprocess.CompletedProcess([], 0, extra, b""),
                    ),
                    self.assertRaisesRegex(
                        observer.ObserverError, "^ctest_selection_mismatch$"
                    ),
                ):
                    observer.ctest_result(check)

    def test_i18n_checks_reject_a_stale_build_revision(self) -> None:
        completed = subprocess.CompletedProcess(
            [], 0, b"1/1 Test #1: Fixture .... Passed\n", b""
        )
        with (
            tempfile.TemporaryDirectory() as temporary,
            mock.patch.dict(
                os.environ,
                {
                    **BINDING_ENVIRONMENT,
                    "PRISMDRAKE_PD2_BUILD_DIRECTORY": temporary,
                    "PRISMDRAKE_PD2_CTEST": "/bin/true",
                },
                clear=True,
            ),
            mock.patch.object(observer, "run_command", return_value=completed),
            self.assertRaisesRegex(observer.ObserverError, "^build_revision_mismatch$"),
        ):
            write_build_attestation(Path(temporary), revision="b" * 40)
            observer.ctest_result("locale_aware_clock")

    def test_source_attestation_rejects_stale_or_dirty_checkout(self) -> None:
        with (
            mock.patch.dict(os.environ, BINDING_ENVIRONMENT, clear=True),
            mock.patch.object(observer.Path, "is_file", return_value=True),
            mock.patch.object(observer.os, "access", return_value=True),
            mock.patch.object(
                observer,
                "run_command",
                return_value=subprocess.CompletedProcess(
                    [], 0, ("b" * 40 + "\n").encode("ascii"), b""
                ),
            ),
            self.assertRaisesRegex(observer.ObserverError, "^source_revision_mismatch$"),
        ):
            observer.attest_source_checkout(
                Path("/source"), git_executable=Path("/git")
            )

        source = Path("/source")
        results = (
            subprocess.CompletedProcess([], 0, (REVISION + "\n").encode("ascii"), b""),
            subprocess.CompletedProcess([], 0, b"/source\n", b""),
            subprocess.CompletedProcess([], 1, b"", b""),
        )
        with (
            mock.patch.dict(os.environ, BINDING_ENVIRONMENT, clear=True),
            mock.patch.object(observer.Path, "is_file", return_value=True),
            mock.patch.object(observer.os, "access", return_value=True),
            mock.patch.object(observer, "run_command", side_effect=results),
            mock.patch.object(Path, "resolve", return_value=source),
            self.assertRaisesRegex(observer.ObserverError, "^source_checkout_dirty$"),
        ):
            observer.attest_source_checkout(source, git_executable=Path("/git"))

        untracked = (
            subprocess.CompletedProcess([], 0, (REVISION + "\n").encode("ascii"), b""),
            subprocess.CompletedProcess([], 0, b"/source\n", b""),
            subprocess.CompletedProcess([], 0, b"", b""),
            subprocess.CompletedProcess([], 0, b"", b""),
            subprocess.CompletedProcess([], 0, b"?? private-file\n", b""),
        )
        with (
            mock.patch.dict(os.environ, BINDING_ENVIRONMENT, clear=True),
            mock.patch.object(observer.Path, "is_file", return_value=True),
            mock.patch.object(observer.os, "access", return_value=True),
            mock.patch.object(observer, "run_command", side_effect=untracked),
            mock.patch.object(Path, "resolve", return_value=source),
            self.assertRaisesRegex(observer.ObserverError, "^source_checkout_dirty$"),
        ):
            observer.attest_source_checkout(source, git_executable=Path("/git"))

    def test_model_runner_maps_exact_supported_endpoints_and_compares_pd1(self) -> None:
        baseline = performance_document(PD1_REVISION, [2_000_000, 3_000_000, 4_000_000])
        current = performance_document(REVISION, [1_000_000, 2_000_000, 3_000_000])
        with (
            mock.patch.dict(os.environ, BINDING_ENVIRONMENT, clear=True),
            mock.patch.object(
                observer,
                "configured_regular_file",
                return_value=Path("/evidence/pd1.json"),
            ),
            mock.patch.object(observer, "load_json", return_value=baseline),
            mock.patch.object(
                observer,
                "configured_build_executable",
                return_value=Path("/runner"),
            ),
            mock.patch.object(observer, "validate_performance_document"),
            mock.patch.object(
                observer,
                "run_command",
                return_value=subprocess.CompletedProcess(
                    [], 0, json.dumps(current).encode("utf-8"), b""
                ),
            ),
        ):
            result = observer.performance_result("search_response", "64")
        self.assertEqual(result["status"], "observed")
        self.assertEqual(result["samples"], [1.0, 2.0, 3.0])
        self.assertEqual(result["pd1_comparison"]["status"], "no_regression")

    def test_runtime_asset_and_idle_sources_validate_exact_binding(self) -> None:
        with (
            mock.patch.dict(os.environ, BINDING_ENVIRONMENT, clear=True),
            mock.patch.object(
                observer,
                "configured_regular_file",
                return_value=Path("/evidence/runtime.json"),
            ),
            mock.patch.object(
                observer, "load_json", return_value=runtime_document([1, 2, 3])
            ),
        ):
            result = observer.performance_result("session_ready", None)
        self.assertEqual(result["samples"], [0.000001, 0.000002, 0.000003])
        self.assertEqual(result["pd1_comparison"]["status"], "not_available")

        asset = asset_document([1_000_000, 2_000_000, 3_000_000])
        with (
            mock.patch.dict(os.environ, BINDING_ENVIRONMENT, clear=True),
            mock.patch.object(
                observer,
                "configured_build_executable",
                return_value=Path("/runner"),
            ),
            mock.patch.object(
                observer,
                "run_command",
                return_value=subprocess.CompletedProcess(
                    [], 0, json.dumps(asset).encode("utf-8"), b""
                ),
            ),
        ):
            result = observer.performance_result("icon_decode_cache", "cache_hit")
        self.assertEqual(result["samples"], [1.0, 2.0, 3.0])

        with (
            mock.patch.dict(os.environ, BINDING_ENVIRONMENT, clear=True),
            mock.patch.object(
                observer,
                "configured_regular_file",
                return_value=Path("/evidence/idle.json"),
            ),
            mock.patch.object(
                observer, "load_json", return_value=idle_document([3, 6, 9])
            ),
        ):
            result = observer.performance_result("idle_wakeups", None)
        self.assertEqual(result["unit"], "wakeups_per_minute")
        self.assertEqual(result["samples"], [3, 6, 9])

    def test_all_eighteen_two_size_endpoints_have_a_closed_observed_route(self) -> None:
        endpoints = [
            ("session_ready", None),
            ("shell_complete_presentation", None),
            ("launcher_acknowledgement", None),
            ("search_response", "16"),
            ("search_response", "64"),
            ("catalog_refresh", None),
            ("icon_decode_cache", "decode"),
            ("icon_decode_cache", "cache_hit"),
            ("wallpaper_decode_publication", None),
            ("notification_method_latency", None),
            ("notification_center_open", None),
            ("quick_settings_open", None),
            ("profile_switch", None),
            ("dnd_switch", None),
            ("output_reconciliation", "add"),
            ("output_reconciliation", "remove"),
            ("idle_wakeups", None),
            ("common_action_acknowledgement", None),
        ]
        observed = {
            "status": "observed",
            "unit": "milliseconds",
            "samples": [1, 2, 3],
            "method": "test",
            "pd1_comparison": observer.unavailable_comparison(),
        }
        with (
            mock.patch.object(observer, "runtime_performance_result", return_value=observed),
            mock.patch.object(observer, "asset_performance_result", return_value=observed),
            mock.patch.object(observer, "model_performance_result", return_value=observed),
            mock.patch.object(observer, "idle_performance_result", return_value=observed),
        ):
            results = [observer.performance_result(*endpoint) for endpoint in endpoints]
        self.assertEqual(len(results), 18)
        self.assertTrue(all(result["status"] == "observed" for result in results))
        with self.assertRaisesRegex(observer.ObserverError, "^performance_probe_invalid$"):
            observer.performance_result("search_response", "10")

    def test_fragment_audit_runs_against_current_sources(self) -> None:
        with (
            tempfile.TemporaryDirectory() as temporary,
            mock.patch.dict(
                os.environ,
                {
                    **BINDING_ENVIRONMENT,
                    "PRISMDRAKE_PD2_BUILD_DIRECTORY": temporary,
                },
                clear=True,
            ),
            mock.patch.object(observer, "attest_source_checkout"),
        ):
            root = Path(temporary) / "source"
            (root / "src").mkdir(parents=True)
            (root / "src" / "safe.cpp").write_text(
                'const auto message = qsTr("Complete message");\n', encoding="utf-8"
            )
            write_build_attestation(Path(temporary), source=root)
            self.assertEqual(observer.fragile_fragment_result()["status"], "passed")


if __name__ == "__main__":
    unittest.main()
