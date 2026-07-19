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
import pd2_reference_observer as observer  # noqa: E402


REVISION = "a" * 40
PD1_REVISION = "b" * 40
ENVIRONMENT_ID = "gentoo-reference-vm"
BINDING_ENVIRONMENT = {
    "PRISMDRAKE_PD2_SOURCE_REVISION": REVISION,
    "PRISMDRAKE_PD2_REFERENCE_ENVIRONMENT_ID": ENVIRONMENT_ID,
    "PRISMDRAKE_PD2_METHOD_ID": "pd2-live-v1",
    "PRISMDRAKE_PD2_PD1_REFERENCE_REVISION": PD1_REVISION,
}


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


class ReferenceObserverTests(unittest.TestCase):
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
            mock.patch.object(observer, "run_command", return_value=completed),
        ):
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
            self.assertRaisesRegex(observer.ObserverError, "^ctest_selection_empty$"),
        ):
            observer.ctest_result("locale_aware_clock")

    def test_model_runner_maps_only_exact_supported_endpoints_and_compares_pd1(self) -> None:
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
                observer, "configured_path", return_value=Path("/runner")
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

        unsupported = observer.performance_result("session_ready", None)
        self.assertEqual(unsupported["status"], "skipped")
        self.assertEqual(unsupported["reason_code"], "live_adapter_unimplemented")

    def test_fragment_audit_runs_against_current_sources(self) -> None:
        self.assertEqual(observer.fragile_fragment_result()["status"], "passed")


if __name__ == "__main__":
    unittest.main()
