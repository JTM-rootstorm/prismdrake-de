#!/usr/bin/env python3
"""Hermetic and negative tests for the PD2 evidence contract."""

from __future__ import annotations

import copy
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Any

from pd2_evidence_contract import (
    EvidenceError,
    INTERNATIONALIZATION_CHECKS,
    MATRIX_CHECKS,
    TIMING_MEASUREMENTS,
    example_contract_document,
    validate_evidence_document,
)


def observed(
    value: float,
    *,
    unit: str = "milliseconds",
    comparison_status: str = "no_regression",
    baseline: float | None = None,
) -> dict[str, Any]:
    comparison: dict[str, Any]
    if comparison_status in {"no_regression", "regression"}:
        comparison = {
            "status": comparison_status,
            "statistic": "median",
            "pd1_value": value if baseline is None else baseline,
        }
        if comparison_status == "regression":
            comparison["explanation"] = "Reviewed implementation scope increased"
    else:
        comparison = {
            "status": comparison_status,
            "explanation": "PD1 did not collect an equivalent endpoint",
        }
    return {
        "status": "observed",
        "unit": unit,
        "samples": [value, value, value],
        "method": "Injected monotonic endpoint measurements",
        "pd1_comparison": comparison,
    }


def executed() -> dict[str, str]:
    return {"status": "passed", "method": "Hermetic contract test method"}


def reference_document() -> dict[str, Any]:
    document = example_contract_document()
    document["evidence_purpose"] = "reference_measurement"
    document["source_revision"] = "1" * 40
    document["pd1_reference"] = {
        "status": "identified",
        "source_revision": "2" * 40,
        "evidence_id": "pd1-reviewed-reference",
    }
    document["environment"].update(
        {
            "kind": "reference_vm",
            "reference_environment_id": "gentoo-reference-vm",
            "operating_system": "Gentoo Linux",
            "display_backend": "x11",
            "window_manager": "openbox",
        }
    )
    document["method"].update(
        {
            "collection_mode": "injected_measurements",
            "sample_isolation": "fresh_session_per_series",
            "description": "Reviewed bounded reference VM measurement method",
        }
    )
    performance = document["performance"]
    for name in TIMING_MEASUREMENTS:
        performance[name] = observed(5.0)
    performance["search_response"] = [
        {"catalog_size": 10, "measurement": observed(1.0)},
        {"catalog_size": 100, "measurement": observed(3.0)},
    ]
    performance["icon_decode_cache"] = {
        "decode": observed(4.0),
        "cache_hit": observed(0.5),
    }
    performance["output_reconciliation"] = {
        "add": observed(8.0, comparison_status="not_available"),
        "remove": observed(7.0, comparison_status="not_available"),
    }
    performance["idle_wakeups"] = observed(2.0, unit="wakeups_per_minute")
    for entry in document["accessibility"]["matrix"]:
        entry["checks"] = {check: executed() for check in MATRIX_CHECKS}
    document["accessibility"]["live_atspi"] = executed()
    document["accessibility"]["screen_reader_workflow"] = executed()
    document["internationalization"] = {
        check: executed() for check in INTERNATIONALIZATION_CHECKS
    }
    document["limitations"] = ["Synthetic values exist only inside contract unit tests"]
    return document


class Pd2EvidenceContractTest(unittest.TestCase):
    def test_skip_only_contract_fixture_is_valid_and_claims_no_passes(self) -> None:
        document = example_contract_document()
        validate_evidence_document(document)
        self.assertEqual(document["evidence_purpose"], "contract_test")
        self.assertTrue(
            all(
                result["status"] == "skipped"
                for entry in document["accessibility"]["matrix"]
                for result in entry["checks"].values()
            )
        )

    def test_complete_injected_reference_document_is_valid(self) -> None:
        validate_evidence_document(reference_document())

    def test_validate_only_cli_accepts_bounded_contract_artifact(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            artifact = Path(temporary) / "artifact.json"
            artifact.write_text(json.dumps(example_contract_document()), encoding="utf-8")
            completed = subprocess.run(
                [
                    "python3",
                    str(Path(__file__).with_name("pd2_evidence_contract.py")),
                    "--validate",
                    str(artifact),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(
            completed.stdout,
            "pd2_evidence_contract: valid bounded version 1 record\n",
        )

    def test_rejects_missing_required_metric(self) -> None:
        document = example_contract_document()
        document["performance"].pop("profile_switch")
        with self.assertRaises(EvidenceError):
            validate_evidence_document(document)

    def test_rejects_uncontrolled_or_duplicate_search_sizes(self) -> None:
        document = example_contract_document()
        document["performance"]["search_response"][1]["catalog_size"] = 10
        with self.assertRaisesRegex(EvidenceError, "search_catalog_series_invalid"):
            validate_evidence_document(document)

    def test_rejects_hidden_pd1_regression(self) -> None:
        document = reference_document()
        document["performance"]["session_ready"] = observed(10.0, baseline=5.0)
        with self.assertRaisesRegex(EvidenceError, "pd1_regression_hidden"):
            validate_evidence_document(document)

    def test_rejects_false_regression_claim(self) -> None:
        document = reference_document()
        document["performance"]["session_ready"] = observed(
            5.0, comparison_status="regression", baseline=10.0
        )
        with self.assertRaisesRegex(EvidenceError, "pd1_regression_claim_invalid"):
            validate_evidence_document(document)

    def test_rejects_regression_without_explanation(self) -> None:
        document = reference_document()
        measurement = observed(10.0, comparison_status="regression", baseline=5.0)
        measurement["pd1_comparison"].pop("explanation")
        document["performance"]["session_ready"] = measurement
        with self.assertRaises(EvidenceError):
            validate_evidence_document(document)

    def test_rejects_compared_metric_without_identified_pd1_reference(self) -> None:
        document = reference_document()
        document["pd1_reference"] = {
            "status": "unavailable",
            "explanation": "No reviewed baseline artifact",
        }
        with self.assertRaisesRegex(EvidenceError, "pd1_reference_missing"):
            validate_evidence_document(document)

    def test_rejects_zero_or_duplicate_environment_identity(self) -> None:
        zero_revision = reference_document()
        zero_revision["pd1_reference"]["source_revision"] = "0" * 40
        with self.assertRaisesRegex(EvidenceError, "pd1_reference_revision_invalid"):
            validate_evidence_document(zero_revision)

        duplicate_compiler = reference_document()
        duplicate_compiler["environment"]["compilers"].append(
            {"family": "gcc", "version": "15.3.0"}
        )
        with self.assertRaisesRegex(EvidenceError, "compiler_environment_invalid"):
            validate_evidence_document(duplicate_compiler)

    def test_rejects_unreviewed_skip(self) -> None:
        document = example_contract_document()
        document["accessibility"]["live_atspi"]["reviewed"] = False
        with self.assertRaises(EvidenceError):
            validate_evidence_document(document)

    def test_rejects_missing_surface_or_matrix_check(self) -> None:
        missing_surface = example_contract_document()
        missing_surface["accessibility"]["matrix"].pop()
        with self.assertRaises(EvidenceError):
            validate_evidence_document(missing_surface)

        missing_check = example_contract_document()
        missing_check["accessibility"]["matrix"][0]["checks"].pop("state_expanded")
        with self.assertRaises(EvidenceError):
            validate_evidence_document(missing_check)

    def test_rejects_live_passes_in_contract_test_fixture(self) -> None:
        document = example_contract_document()
        document["accessibility"]["live_atspi"] = executed()
        with self.assertRaisesRegex(EvidenceError, "contract_test_contains_passing_check"):
            validate_evidence_document(document)

    def test_rejects_private_paths_addresses_and_runtime_identifiers(self) -> None:
        values = [
            "/home/operator/private",
            "unix:abstract=private",
            "Connection at 192.168.1.20",
            "Bus :1.42",
            "Address fd00::1",
            "Window 0x400123",
        ]
        for value in values:
            with self.subTest(value=value):
                document = reference_document()
                document["method"]["description"] = value
                with self.assertRaises(EvidenceError):
                    validate_evidence_document(document)

        document = reference_document()
        document["environment"]["process_id"] = 42
        with self.assertRaises(EvidenceError):
            validate_evidence_document(document)

    def test_rejects_observation_in_contract_test_artifact(self) -> None:
        document = example_contract_document()
        document["performance"]["session_ready"] = observed(
            1.0, comparison_status="not_available"
        )
        with self.assertRaisesRegex(EvidenceError, "contract_test_contains_observation"):
            validate_evidence_document(document)

    def test_rejects_unordered_matrix_even_when_schema_shape_is_complete(self) -> None:
        document = example_contract_document()
        matrix = document["accessibility"]["matrix"]
        matrix[0], matrix[1] = matrix[1], matrix[0]
        with self.assertRaisesRegex(EvidenceError, "accessibility_matrix_invalid"):
            validate_evidence_document(document)

    def test_rejects_private_content_shaped_fields(self) -> None:
        document = reference_document()
        document["accessibility"]["live_atspi"] = copy.deepcopy(executed())
        document["accessibility"]["live_atspi"]["content"] = "private text"
        with self.assertRaises(EvidenceError):
            validate_evidence_document(document)


if __name__ == "__main__":
    unittest.main()
