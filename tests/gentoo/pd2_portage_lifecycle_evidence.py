#!/usr/bin/env python3
"""Validate bounded PD2 Portage installed-lifecycle evidence.

This module validates normalized captures produced by an external lifecycle
run. It never invokes Portage, changes an installed package, or runs a demo.
"""

from __future__ import annotations

import argparse
import copy
import json
import math
from pathlib import Path
import sys
from typing import Any

from pd2_installed_artifact_attestation import (
    INSTALLED_EXECUTABLE_PATHS,
    REQUIRED_INSTALLED_FILE_GROUPS,
    SOURCE_URI,
    example_attestation_document,
)
from pd2_lifecycle_hashes import (
    Pd2LifecycleHashError,
    canonical_executable_sha256,
    canonical_linkage_sha256,
    canonical_ownership_sha256,
    canonical_removable_ownership_sha256,
    canonical_regular_ownership_sha256,
    normalize_ownership_records,
    removable_ownership_records,
    regular_ownership_records,
)


ROOT = Path(__file__).resolve().parents[2]
SCHEMA_PATH = Path(__file__).with_name("pd2-portage-lifecycle-evidence.schema.json")
MAX_EVIDENCE_BYTES = 524_288
EXCLUDED_TESTS = (
    "DetachedApplicationTest.ExecutesExactArgvWorkingDirectoryAndEnvironmentWithoutShell",
    "LauncherPipelineTest.ExpandsPlansAndLaunchesLiteralArgumentsWithoutAShell",
    "X11DockOpenboxIntegrationTest",
    "TaskControllerOpenboxStabilizationIntegrationTest",
)
OPTIONAL_PACKAGE_SKIP_TEST = "AtomicFileTest.RejectsDestinationOwnedByAnotherUser"
OPTIONAL_PACKAGE_SKIP_REASON = "requires_root_ownership_transition"
EXCLUSION_REASONS = (
    "exact_child_environment",
    "exact_child_environment",
    "openbox_preload_interference",
    "openbox_preload_interference",
)
USER_DATA_KINDS = ("config_toml", "pins", "notification_history")
DEMONSTRATION_PHASES = ("initial_install", "ordinary_reinstall")
PRIVATE_MARKERS = (
    "dbus_session_bus_address",
    "at_spi_bus_address",
    "unix:",
    '"pid"',
    '"xid"',
    '"uid"',
    '"gid"',
    '"username"',
    '"hostname"',
    '"stdout"',
    '"stderr"',
    '"command"',
    '"environment"',
    '"log"',
    "/home/",
    "/root/",
    "/tmp/",
    "/mnt/",
    "file://",
    "build_tree",
)


class Pd2EvidenceError(RuntimeError):
    """A PD2 lifecycle record violates the closed evidence contract."""


class DuplicateJsonKeyError(ValueError):
    """Strict JSON loading encountered a repeated object key."""


def _require(condition: bool, identifier: str) -> None:
    if not condition:
        raise Pd2EvidenceError(identifier)


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise DuplicateJsonKeyError("duplicate object key")
        result[key] = value
    return result


def _reject_constant(value: str) -> Any:
    del value
    raise ValueError("non-finite JSON number")


def _parse_finite_float(value: str) -> float:
    parsed = float(value)
    if not math.isfinite(parsed):
        raise ValueError("non-finite JSON number")
    return parsed


def load_evidence(path: Path) -> Any:
    """Load one bounded UTF-8 evidence document without duplicate keys."""

    try:
        raw = path.read_bytes()
    except OSError as error:
        raise Pd2EvidenceError("evidence_read_failed") from error
    _require(0 < len(raw) <= MAX_EVIDENCE_BYTES, "evidence_size_invalid")
    try:
        return json.loads(
            raw.decode("utf-8"),
            object_pairs_hook=_reject_duplicate_keys,
            parse_constant=_reject_constant,
            parse_float=_parse_finite_float,
        )
    except (UnicodeError, ValueError, json.JSONDecodeError) as error:
        raise Pd2EvidenceError("evidence_json_invalid") from error


def validate_evidence_schema(document: Any) -> None:
    """Validate the versioned closed schema with the project validator."""

    try:
        schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as error:
        raise Pd2EvidenceError("evidence_schema_unavailable") from error
    tools_path = str(ROOT / "tools")
    if tools_path not in sys.path:
        sys.path.insert(0, tools_path)
    from validate import validate_schema  # pylint: disable=import-outside-toplevel

    _require(
        not validate_schema(document, schema, schema, "pd2_portage_lifecycle_evidence"),
        "evidence_schema_mismatch",
    )


def _require_binding(
        record: dict[str, Any], revision: str, ebuild_hash: str, artifact_hash: str | None = None,
) -> None:
    _require(record["source_revision"] == revision, "source_revision_mismatch")
    _require(record["ebuild_sha256"] == ebuild_hash, "ebuild_sha256_mismatch")
    if artifact_hash is not None:
        _require(record["artifact_sha256"] == artifact_hash, "artifact_sha256_mismatch")


def _validate_executables(records: list[dict[str, Any]]) -> dict[str, str]:
    _require(tuple(item["path"] for item in records) == INSTALLED_EXECUTABLE_PATHS,
             "installed_executable_paths_mismatch")
    return {item["path"]: item["sha256"] for item in records}


def validate_evidence_document(document: Any) -> None:
    """Apply exact bindings, reproducibility, privacy, and lifecycle semantics."""

    validate_evidence_schema(document)
    encoded = json.dumps(document, ensure_ascii=True, separators=(",", ":"), sort_keys=True)
    _require(len(encoded.encode("utf-8")) <= MAX_EVIDENCE_BYTES, "evidence_oversized")
    lowered = encoded.lower()
    _require(not any(marker in lowered for marker in PRIVATE_MARKERS), "evidence_private_data")

    source = document["source"]
    revision = source["revision"]
    ebuild_hash = source["ebuild_sha256"]
    _require(source["repository_uri"] == SOURCE_URI, "source_repository_mismatch")

    portage_qa = document["portage_qa"]
    _require_binding(portage_qa, revision, ebuild_hash,
                     document["tested_artifact"]["artifact_sha256"])

    resolutions = document["resolutions"]
    _require(tuple(item["mode"] for item in resolutions) == ("default", "use_test"),
             "resolution_order_mismatch")
    _require(tuple(item["use_test"] for item in resolutions) == (False, True),
             "resolution_use_test_mismatch")
    for resolution in resolutions:
        _require_binding(resolution, revision, ebuild_hash)
    _require(resolutions[0]["dependency_graph_sha256"] !=
             resolutions[1]["dependency_graph_sha256"],
             "resolution_dependency_graphs_not_distinct")

    tested = document["tested_artifact"]
    artifact_hash = tested["artifact_sha256"]
    _require_binding(tested, revision, ebuild_hash)

    package_test = document["package_test"]
    _require_binding(package_test, revision, ebuild_hash, artifact_hash)
    _require(tuple(package_test["excluded_tests"]) == EXCLUDED_TESTS,
             "package_test_exclusions_mismatch")
    skipped_tests = package_test["skipped_tests"]
    _require(package_test["selected_test_count"] ==
             package_test["executed_test_count"] and
             package_test["passed_test_count"] + len(skipped_tests) ==
             package_test["executed_test_count"] and
             package_test["skipped_test_count"] == len(skipped_tests) and
             package_test["disabled_test_count"] == 0 and
             package_test["not_run_test_count"] == 0,
             "package_test_incomplete")
    _require(skipped_tests in ([], [{
        "test_id": OPTIONAL_PACKAGE_SKIP_TEST,
        "reason": OPTIONAL_PACKAGE_SKIP_REASON,
    }]), "package_test_skip_not_allowed")

    initial = document["initial_install"]
    _require_binding(initial, revision, ebuild_hash, artifact_hash)
    executable_hashes = _validate_executables(initial["executables"])
    _require(initial["vdb"]["ebuild_sha256"] == ebuild_hash and
             initial["vdb"]["use_test"] is True, "initial_vdb_binding_mismatch")
    try:
        _require(canonical_executable_sha256(initial["executables"]) == artifact_hash,
                 "artifact_sha256_mismatch")
        ownership_records = normalize_ownership_records(initial["ownership"]["paths"])
        regular_records = regular_ownership_records(ownership_records)
        ownership_hash = canonical_ownership_sha256(ownership_records)
        regular_hash = canonical_regular_ownership_sha256(ownership_records)
        removable_records = removable_ownership_records(ownership_records)
        removable_hash = canonical_removable_ownership_sha256(ownership_records)
    except Pd2LifecycleHashError as error:
        raise Pd2EvidenceError(str(error)) from error
    ownership = initial["ownership"]
    _require(ownership["paths"] == ownership_records, "ownership_paths_not_normalized")
    _require(ownership["path_count"] == len(ownership_records),
             "ownership_path_count_mismatch")
    _require(ownership["regular_path_count"] == len(regular_records),
             "ownership_regular_path_count_mismatch")
    _require(ownership["path_set_sha256"] == ownership_hash, "ownership_hash_mismatch")
    _require(initial["vdb"]["ownership_sha256"] == ownership_hash,
             "initial_vdb_ownership_mismatch")
    _require(ownership["regular_path_set_sha256"] == regular_hash,
             "ownership_regular_hash_mismatch")
    _require(set(INSTALLED_EXECUTABLE_PATHS) <=
             {record["path"] for record in regular_records},
             "executables_not_owned_regular_paths")
    owned_regular_paths = {record["path"] for record in regular_records}
    _require(all(any(path in owned_regular_paths for path in alternatives)
                 for alternatives in REQUIRED_INSTALLED_FILE_GROUPS),
             "required_installed_file_missing")

    linkage = document["direct_linkage"]
    _require_binding(linkage, revision, ebuild_hash, artifact_hash)
    entries = linkage["entries"]
    _require(tuple(item["path"] for item in entries) == INSTALLED_EXECUTABLE_PATHS,
             "linkage_paths_mismatch")
    for entry in entries:
        _require(entry["executable_sha256"] == executable_hashes[entry["path"]],
                 "linkage_executable_hash_mismatch")
        _require(entry["needed"] == sorted(entry["needed"]),
                 "linkage_needed_not_normalized")
    _require(any("libtomlplusplus" in library
                 for entry in entries for library in entry["needed"]),
             "linkage_toml_dependency_missing")
    try:
        _require(linkage["capture_sha256"] == canonical_linkage_sha256(entries),
                 "linkage_capture_hash_mismatch")
    except Pd2LifecycleHashError as error:
        raise Pd2EvidenceError(str(error)) from error

    exclusions = document["sandbox_exclusions"]
    _require(tuple(item["test_id"] for item in exclusions) == EXCLUDED_TESTS,
             "sandbox_exclusion_identity_mismatch")
    _require(tuple(item["reason"] for item in exclusions) == EXCLUSION_REASONS,
             "sandbox_exclusion_reason_mismatch")
    for index, exclusion in enumerate(exclusions):
        _require_binding(exclusion, revision, ebuild_hash, artifact_hash)
        _require(exclusion["test_manifest_sha256"] ==
                 package_test["test_manifest_sha256"],
                 "sandbox_exclusion_test_manifest_mismatch")
        _require(exclusion["inventory_contract_sha256"] ==
                 package_test["inventory_contract_sha256"],
                 "sandbox_exclusion_inventory_contract_mismatch")
        _require(exclusion["passes"] == exclusion["attempts"],
                 "sandbox_exclusion_repeat_failure")
        minimum = 1 if index < 2 else 5
        _require(exclusion["attempts"] == minimum if index < 2 else
                 exclusion["attempts"] >= minimum,
                 "sandbox_exclusion_repeat_count_mismatch")

    demonstrations = document["installed_demonstrations"]
    _require(tuple(item["phase"] for item in demonstrations) == DEMONSTRATION_PHASES,
             "installed_demonstration_phases_mismatch")
    for demonstration in demonstrations:
        _require_binding(demonstration, revision, ebuild_hash, artifact_hash)
    _require(demonstrations[0]["harness_manifest_sha256"] ==
             demonstrations[1]["harness_manifest_sha256"],
             "installed_demonstration_harness_mismatch")

    unmerge = document["unmerge"]
    _require_binding(unmerge, revision, ebuild_hash, artifact_hash)
    _require(unmerge["absent_path_count"] == len(removable_records),
             "unmerge_absent_path_count_mismatch")
    _require(unmerge["removable_path_set_sha256"] == removable_hash,
             "unmerge_removable_path_hash_mismatch")
    user_data = unmerge["user_data"]
    _require(tuple(item["kind"] for item in user_data) == USER_DATA_KINDS,
             "user_data_scope_mismatch")
    for item in user_data:
        _require(item["before_sha256"] == item["after_sha256"],
                 "user_data_changed_during_unmerge")

    reinstall = document["ordinary_reinstall"]
    _require_binding(reinstall, revision, ebuild_hash, artifact_hash)
    _validate_executables(reinstall["executables"])
    _require(reinstall["vdb"]["ebuild_sha256"] == ebuild_hash and
             reinstall["vdb"]["use_test"] is False,
             "ordinary_reinstall_vdb_binding_mismatch")
    _require(reinstall["vdb"]["ownership_sha256"] == initial["vdb"]["ownership_sha256"],
             "ordinary_reinstall_vdb_ownership_mismatch")
    _require(reinstall["executables"] == initial["executables"],
             "ordinary_reinstall_executable_mismatch")
    reinstall_ownership = reinstall["ownership"]
    _require(reinstall_ownership == ownership,
             "ordinary_reinstall_ownership_mismatch")


def example_evidence_document() -> dict[str, Any]:
    """Return synthetic contract-test data, never a captured VM result."""

    attestation = example_attestation_document()
    source = copy.deepcopy(attestation["source"])
    tested = copy.deepcopy(attestation["tested_artifact"])
    initial = copy.deepcopy(attestation["installed"])
    revision = source["revision"]
    ebuild_hash = source["ebuild_sha256"]
    artifact_hash = tested["artifact_sha256"]
    harness_hash = attestation["demo_harness"]["manifest_sha256"]
    executable_hashes = {item["path"]: item["sha256"]
                         for item in initial["executables"]}
    needed = {
        INSTALLED_EXECUTABLE_PATHS[0]: ["libbasu.so.0", "libstdc++.so.6"],
        INSTALLED_EXECUTABLE_PATHS[1]: [
            "libbasu.so.0", "libstdc++.so.6", "libtomlplusplus.so.3"],
        INSTALLED_EXECUTABLE_PATHS[2]: ["libbasu.so.0", "libstdc++.so.6"],
        INSTALLED_EXECUTABLE_PATHS[3]: [
            "libQt6Core.so.6", "libQt6Gui.so.6", "libQt6Qml.so.6",
            "libbasu.so.0", "libstdc++.so.6"],
        INSTALLED_EXECUTABLE_PATHS[4]: ["libbasu.so.0", "libstdc++.so.6"],
    }
    linkage_entries = [
        {
            "path": path,
            "executable_sha256": executable_hashes[path],
            "needed": sorted(needed[path]),
        }
        for path in INSTALLED_EXECUTABLE_PATHS
    ]

    binding = {
        "source_revision": revision,
        "ebuild_sha256": ebuild_hash,
        "artifact_sha256": artifact_hash,
    }
    exclusions = []
    for index, (test_id, reason) in enumerate(zip(EXCLUDED_TESTS, EXCLUSION_REASONS)):
        attempts = 1 if index < 2 else 5
        exclusions.append({
            "test_id": test_id,
            "reason": reason,
            **binding,
            "execution_context": "outside_portage_sandbox",
            "result": "passed",
            "test_manifest_sha256": "c" * 64,
            "inventory_contract_sha256": "7" * 64,
            "attempts": attempts,
            "passes": attempts,
        })
    demonstrations = [
        {
            "phase": phase,
            **binding,
            "result": "passed",
            "complete": True,
            "evidence_kind": "pd2_installed_multi_output_failure_demonstration",
            "evidence_schema_version": 1,
            "evidence_sha256": digest * 64,
            "normalized_results_sha256": normalized * 64,
            "harness_manifest_sha256": harness_hash,
        }
        for phase, digest, normalized in zip(DEMONSTRATION_PHASES, ("8", "9"), ("a", "b"))
    ]
    ownership = initial["ownership"]
    return {
        "schema_version": 1,
        "evidence_kind": "pd2_portage_installed_lifecycle",
        "source": source,
        "portage_qa": {
            **binding,
            "result": "passed",
            "source_repository_tree_sha256": "0" * 64,
            "repository_tree_sha256": "d" * 64,
            "pkgdev_output_sha256": "e" * 64,
            "pkgcheck_output_sha256": "f" * 64,
        },
        "resolutions": [
            {
                "mode": "default",
                "source_revision": revision,
                "ebuild_sha256": ebuild_hash,
                "use_test": False,
                "result": "resolved",
                "dependency_graph_sha256": "1" * 64,
            },
            {
                "mode": "use_test",
                "source_revision": revision,
                "ebuild_sha256": ebuild_hash,
                "use_test": True,
                "result": "resolved",
                "dependency_graph_sha256": "2" * 64,
            },
        ],
        "tested_artifact": tested,
        "package_test": {
            **binding,
            "features_test": True,
            "result": "passed",
            "selected_test_count": 600,
            "executed_test_count": 600,
            "passed_test_count": 599,
            "skipped_test_count": 1,
            "skipped_tests": [{
                "test_id": OPTIONAL_PACKAGE_SKIP_TEST,
                "reason": OPTIONAL_PACKAGE_SKIP_REASON,
            }],
            "disabled_test_count": 0,
            "not_run_test_count": 0,
            "test_manifest_sha256": "c" * 64,
            "inventory_contract_sha256": "7" * 64,
            "excluded_tests": list(EXCLUDED_TESTS),
        },
        "initial_install": initial,
        "direct_linkage": {
            **binding,
            "tool": "scanelf",
            "capture_sha256": canonical_linkage_sha256(linkage_entries),
            "entries": linkage_entries,
        },
        "sandbox_exclusions": exclusions,
        "installed_demonstrations": demonstrations,
        "unmerge": {
            **binding,
            "atom": "x11-misc/prismdrake",
            "result": "passed",
            "owned_removable_paths_absent": True,
            "absent_path_count": len(removable_ownership_records(ownership["paths"])),
            "removable_path_set_sha256": canonical_removable_ownership_sha256(
                ownership["paths"]),
            "user_data": [
                {"kind": kind, "before_sha256": digest * 64, "after_sha256": digest * 64}
                for kind, digest in zip(USER_DATA_KINDS, ("3", "4", "5"))
            ],
        },
        "ordinary_reinstall": {
            **binding,
            "provenance": "portage_installed",
            "use_test": False,
            "result": "passed",
            "vdb": {
                **copy.deepcopy(initial["vdb"]),
                "use_test": False,
            },
            "executables": copy.deepcopy(initial["executables"]),
            "ownership": copy.deepcopy(ownership),
        },
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    arguments = parser.parse_args(argv)
    try:
        validate_evidence_document(load_evidence(arguments.evidence))
    except Pd2EvidenceError as error:
        print(f"pd2_portage_lifecycle_evidence: {error}", file=sys.stderr)
        return 1
    print("pd2_portage_lifecycle_evidence: valid bounded version 1 record")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
