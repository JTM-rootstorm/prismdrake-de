#!/usr/bin/env python3
"""Validate injected PD2 performance, accessibility, and i18n evidence.

This module does not collect live measurements.  It accepts only a bounded
artifact produced by an independently reviewed collector or a contract-test
fixture and validates the versioned, privacy-safe contract.
"""

from __future__ import annotations

import argparse
import json
import re
import statistics
import sys
from pathlib import Path
from typing import Any, Iterator


MAX_EVIDENCE_BYTES = 524_288
SCHEMA_PATH = Path(__file__).with_name("pd2-evidence-contract.schema.json")

SURFACES = [
    "panel",
    "pinned_task",
    "running_task",
    "grouped_task",
    "group_popup",
    "launcher",
    "launcher_results",
    "desktop_surface_exclusion",
    "transient_notification",
    "notification_center",
    "quick_settings",
    "logout_confirmation",
    "output_migration",
    "service_restart_focus_recovery",
]

MATRIX_CHECKS = [
    "tab_backtab",
    "arrow_navigation",
    "enter_activation",
    "space_activation",
    "escape_recovery",
    "menu_key",
    "shift_f10",
    "accessible_name",
    "accessible_role",
    "accessible_description",
    "state_focused",
    "state_selected",
    "state_active",
    "state_urgent",
    "state_minimized",
    "state_disabled",
    "state_unavailable",
    "state_checked",
    "state_expanded",
    "minimum_target_size",
    "high_contrast",
    "reduced_motion",
    "disabled_transparency",
    "text_scale",
    "rtl_smoke",
    "long_string_layout",
]

INTERNATIONALIZATION_CHECKS = [
    "qt_translation_routing",
    "rtl_fixture",
    "long_string_fixture",
    "locale_aware_clock",
    "localized_desktop_search",
    "fragile_fragment_audit",
]

TIMING_MEASUREMENTS = [
    "session_ready",
    "shell_complete_presentation",
    "launcher_acknowledgement",
    "catalog_refresh",
    "wallpaper_decode_publication",
    "notification_method_latency",
    "notification_center_open",
    "quick_settings_open",
    "profile_switch",
    "dnd_switch",
    "common_action_acknowledgement",
]

COMMON_ACTION_ACKNOWLEDGEMENT_TARGET_MS = 100.0

FORBIDDEN_KEY_TOKENS = {
    "address",
    "body",
    "content",
    "filesystem_path",
    "hostname",
    "path",
    "pid",
    "process_id",
    "summary",
    "thread_id",
    "username",
    "window_id",
    "window_title",
    "xid",
}
PRIVATE_STRING_MARKERS = (
    "/home/",
    "/tmp/",
    "unix:",
    "tcp:",
    "ssh://",
    "file://",
    "dbus-address",
)
IPV4_ADDRESS = re.compile(r"(?<![0-9])(?:[0-9]{1,3}[.]){3}[0-9]{1,3}(?![0-9])")
IPV6_ADDRESS = re.compile(r"\b(?:[0-9a-fA-F]{0,4}:){2,7}[0-9a-fA-F]{0,4}\b")
DBUS_UNIQUE_ADDRESS = re.compile(r"(?<![A-Za-z0-9]):[0-9]+[.][0-9]+")
X11_ID = re.compile(r"\b0x[0-9a-fA-F]{4,16}\b")


class EvidenceError(RuntimeError):
    """A closed PD2 evidence validation failure."""


def _require(condition: bool, identifier: str) -> None:
    if not condition:
        raise EvidenceError(identifier)


def load_evidence(path: Path) -> Any:
    """Load one bounded UTF-8 JSON document."""
    try:
        raw = path.read_bytes()
    except OSError as error:
        raise EvidenceError("evidence_read_failed") from error
    _require(len(raw) <= MAX_EVIDENCE_BYTES, "evidence_oversized")
    try:
        return json.loads(raw.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise EvidenceError("evidence_json_invalid") from error


def validate_schema(document: Any) -> None:
    """Validate against the tracked JSON schema."""
    try:
        schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise EvidenceError("evidence_schema_unavailable") from error
    project_root = Path(__file__).resolve().parents[2]
    sys.path.insert(0, str(project_root / "tools"))
    from validate import validate_schema as project_validate_schema

    _require(
        not project_validate_schema(document, schema, schema, "pd2_evidence"),
        "evidence_schema_mismatch",
    )


def _walk(value: Any) -> Iterator[tuple[str | None, Any]]:
    if isinstance(value, dict):
        for key, child in value.items():
            yield key, child
            yield from _walk(child)
    elif isinstance(value, list):
        for child in value:
            yield None, child
            yield from _walk(child)


def _validate_privacy(document: dict[str, Any]) -> None:
    for key, value in _walk(document):
        if key is not None:
            normalized = key.lower()
            _require(normalized not in FORBIDDEN_KEY_TOKENS, "evidence_private_field")
        if not isinstance(value, str):
            continue
        lowered = value.lower()
        _require(
            not any(marker in lowered for marker in PRIVATE_STRING_MARKERS),
            "evidence_private_data",
        )
        _require(IPV4_ADDRESS.search(value) is None, "evidence_private_data")
        _require(IPV6_ADDRESS.search(value) is None, "evidence_private_data")
        _require(DBUS_UNIQUE_ADDRESS.search(value) is None, "evidence_private_data")
        _require(X11_ID.search(value) is None, "evidence_private_data")


def _iter_measurements(performance: dict[str, Any]) -> Iterator[tuple[str, dict[str, Any]]]:
    for name in TIMING_MEASUREMENTS:
        yield name, performance[name]
    for index, observation in enumerate(performance["search_response"]):
        yield f"search_response_{index}", observation["measurement"]
    yield "icon_decode", performance["icon_decode_cache"]["decode"]
    yield "icon_cache_hit", performance["icon_decode_cache"]["cache_hit"]
    yield "output_add", performance["output_reconciliation"]["add"]
    yield "output_remove", performance["output_reconciliation"]["remove"]
    yield "idle_wakeups", performance["idle_wakeups"]


def _validate_measurement(name: str, measurement: dict[str, Any]) -> None:
    if measurement["status"] == "skipped":
        return
    expected_unit = "wakeups_per_minute" if name == "idle_wakeups" else "milliseconds"
    _require(measurement["unit"] == expected_unit, "measurement_unit_invalid")
    if name == "common_action_acknowledgement":
        _require(
            all(
                sample <= COMMON_ACTION_ACKNOWLEDGEMENT_TARGET_MS
                for sample in measurement["samples"]
            ),
            "common_action_acknowledgement_target_exceeded",
        )
    observed_median = statistics.median(measurement["samples"])
    comparison = measurement["pd1_comparison"]
    comparison_status = comparison["status"]
    if comparison_status in {"not_available", "not_comparable"}:
        return
    baseline = comparison["pd1_value"]
    if comparison_status == "regression":
        _require(observed_median > baseline, "pd1_regression_claim_invalid")
    else:
        _require(observed_median <= baseline, "pd1_regression_hidden")


def _iter_checks(document: dict[str, Any]) -> Iterator[dict[str, Any]]:
    accessibility = document["accessibility"]
    for entry in accessibility["matrix"]:
        yield from entry["checks"].values()
    yield accessibility["live_atspi"]
    yield accessibility["screen_reader_workflow"]
    yield from document["internationalization"].values()


def validate_semantics(document: dict[str, Any]) -> None:
    """Validate cross-field semantics omitted from JSON Schema."""
    _validate_privacy(document)
    purpose = document["evidence_purpose"]
    environment = document["environment"]
    compiler_families = [compiler["family"] for compiler in environment["compilers"]]
    _require(
        len(compiler_families) == len(set(compiler_families)),
        "compiler_environment_invalid",
    )
    if purpose == "contract_test":
        _require(environment["kind"] == "contract_test", "contract_environment_invalid")
        _require(set(document["source_revision"]) == {"0"}, "contract_revision_invalid")
    else:
        _require(environment["kind"] == "reference_vm", "reference_environment_invalid")
        _require(set(document["source_revision"]) != {"0"}, "reference_revision_invalid")

    matrix = document["accessibility"]["matrix"]
    _require([entry["surface"] for entry in matrix] == SURFACES, "accessibility_matrix_invalid")
    for entry in matrix:
        _require(list(entry["checks"]) == MATRIX_CHECKS, "accessibility_check_order_invalid")
    _require(
        list(document["internationalization"]) == INTERNATIONALIZATION_CHECKS,
        "internationalization_check_order_invalid",
    )

    search_sizes = [item["catalog_size"] for item in document["performance"]["search_response"]]
    _require(search_sizes == sorted(set(search_sizes)), "search_catalog_series_invalid")

    measurements = list(_iter_measurements(document["performance"]))
    for name, measurement in measurements:
        _validate_measurement(name, measurement)

    compared = any(
        measurement["status"] == "observed"
        and measurement["pd1_comparison"]["status"] in {"no_regression", "regression"}
        for _, measurement in measurements
    )
    if compared:
        _require(document["pd1_reference"]["status"] == "identified", "pd1_reference_missing")
        _require(
            set(document["pd1_reference"]["source_revision"]) != {"0"},
            "pd1_reference_revision_invalid",
        )
    elif document["pd1_reference"]["status"] == "identified":
        _require(False, "pd1_reference_unused")

    if purpose == "contract_test":
        _require(
            all(measurement["status"] == "skipped" for _, measurement in measurements),
            "contract_test_contains_observation",
        )
        _require(
            all(result["status"] == "skipped" for result in _iter_checks(document)),
            "contract_test_contains_passing_check",
        )


def validate_evidence_document(document: Any) -> None:
    """Validate one complete bounded evidence document."""
    _require(isinstance(document, dict), "evidence_root_invalid")
    encoded = json.dumps(document, ensure_ascii=True, sort_keys=True).encode("utf-8")
    _require(len(encoded) <= MAX_EVIDENCE_BYTES, "evidence_oversized")
    validate_schema(document)
    validate_semantics(document)


def skipped_contract_result() -> dict[str, Any]:
    """Return a reviewed contract-test skip, never a passing evidence result."""
    return {
        "status": "skipped",
        "reviewed": True,
        "reason_code": "contract_test_fixture",
        "reason": "Contract structure test does not claim live evidence",
    }


def example_contract_document() -> dict[str, Any]:
    """Return skip-only synthetic contract data for hermetic negative tests."""
    def skip() -> dict[str, Any]:
        return skipped_contract_result()

    performance = {name: skip() for name in TIMING_MEASUREMENTS}
    performance["search_response"] = [
        {"catalog_size": 10, "measurement": skip()},
        {"catalog_size": 100, "measurement": skip()},
    ]
    performance["icon_decode_cache"] = {"decode": skip(), "cache_hit": skip()}
    performance["output_reconciliation"] = {"add": skip(), "remove": skip()}
    performance["idle_wakeups"] = skip()
    return {
        "schema_version": 1,
        "evidence_kind": "pd2_performance_accessibility",
        "evidence_purpose": "contract_test",
        "release_budget": False,
        "source_revision": "0" * 40,
        "pd1_reference": {
            "status": "unavailable",
            "explanation": "Contract structure test has no PD1 comparison source",
        },
        "environment": {
            "kind": "contract_test",
            "reference_environment_id": "contract-test",
            "operating_system": "Hermetic contract test",
            "architecture": "x86_64",
            "qt_version": "6.11.1",
            "display_backend": "offscreen_contract_test",
            "window_manager": "none",
            "compilers": [{"family": "gcc", "version": "15.3.0"}],
        },
        "method": {
            "collection_mode": "validate_only",
            "clock": "monotonic",
            "sample_isolation": "contract_test",
            "description": "Schema and semantic contract validation only",
        },
        "performance": performance,
        "accessibility": {
            "matrix": [
                {"surface": surface, "checks": {check: skip() for check in MATRIX_CHECKS}}
                for surface in SURFACES
            ],
            "live_atspi": skip(),
            "screen_reader_workflow": skip(),
        },
        "internationalization": {
            check: skip() for check in INTERNATIONALIZATION_CHECKS
        },
        "limitations": ["Synthetic skip only contract fixture"],
        "redaction": {
            "contains_filesystem_paths": False,
            "contains_private_content": False,
            "contains_network_or_bus_addresses": False,
            "contains_process_ids": False,
            "contains_x11_window_ids": False,
            "diagnostics_are_closed_ids": True,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--validate", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        validate_evidence_document(load_evidence(arguments.validate))
    except EvidenceError as error:
        print(f"pd2_evidence_contract: {error}", file=sys.stderr)
        return 1
    print("pd2_evidence_contract: valid bounded version 1 record")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
