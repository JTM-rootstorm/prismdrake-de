#!/usr/bin/env python3
"""Validate bounded PD1 Portage product-lifecycle evidence.

This module validates records captured elsewhere. It does not execute Portage,
modify the host or guest, or imply that the lifecycle has run.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import sys
from typing import Any

from lifecycle_hashes import (
    LifecycleHashError,
    canonical_artifact_sha256,
    canonical_linkage_sha256,
)


ROOT = Path(__file__).resolve().parents[2]
SCHEMA_PATH = Path(__file__).with_name("pd1-portage-lifecycle-evidence.schema.json")
MAX_EVIDENCE_BYTES = 24_576
SOURCE_URI = "https://github.com/JTM-rootstorm/prismdrake-de.git"
PACKAGE_ATOM = "x11-misc/prismdrake"
PACKAGE_VERSIONED_ATOM = "x11-misc/prismdrake-9999"
INSTALLED_PATHS = (
    "/usr/bin/prismdrake-session",
    "/usr/bin/prismdrake-settingsd",
    "/usr/bin/prismdrake-shell",
)
EXCLUDED_TESTS = (
    "DetachedApplicationTest.ExecutesExactArgvWorkingDirectoryAndEnvironmentWithoutShell",
    "LauncherPipelineTest.ExpandsPlansAndLaunchesLiteralArgumentsWithoutAShell",
    "X11DockOpenboxIntegrationTest",
    "TaskControllerOpenboxStabilizationIntegrationTest",
)
EXCLUSION_REASONS = (
    "exact_child_environment",
    "exact_child_environment",
    "openbox_preload_interference",
    "openbox_preload_interference",
)
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


class EvidenceError(RuntimeError):
    """A lifecycle evidence document violates the closed contract."""


class DuplicateJsonKeyError(ValueError):
    """Strict JSON loading encountered a repeated object key."""


def _require(condition: bool, identifier: str) -> None:
    if not condition:
        raise EvidenceError(identifier)


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
    """Load one bounded JSON document with duplicate-key rejection."""

    try:
        raw = path.read_bytes()
    except OSError as error:
        raise EvidenceError("evidence_read_failed") from error
    _require(len(raw) <= MAX_EVIDENCE_BYTES, "evidence_oversized")
    try:
        return json.loads(
            raw.decode("utf-8"),
            object_pairs_hook=_reject_duplicate_keys,
            parse_constant=_reject_constant,
            parse_float=_parse_finite_float,
        )
    except (UnicodeError, ValueError, json.JSONDecodeError) as error:
        raise EvidenceError("evidence_json_invalid") from error


def validate_evidence_schema(document: Any) -> None:
    """Validate against the tracked schema with the project validator."""

    try:
        schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as error:
        raise EvidenceError("evidence_schema_unavailable") from error
    tools_path = str(ROOT / "tools")
    if tools_path not in sys.path:
        sys.path.insert(0, tools_path)
    from validate import validate_schema  # pylint: disable=import-outside-toplevel

    errors = validate_schema(document, schema, schema, "portage_lifecycle_evidence")
    _require(not errors, "evidence_schema_mismatch")


def _require_binding(record: dict[str, Any], revision: str, artifact: str) -> None:
    _require(record["source_revision"] == revision, "source_revision_mismatch")
    _require(record["artifact_sha256"] == artifact, "artifact_sha256_mismatch")


def _executable_map(records: list[dict[str, Any]]) -> dict[str, str]:
    paths = tuple(record["path"] for record in records)
    _require(paths == INSTALLED_PATHS, "installed_executable_paths_mismatch")
    return {record["path"]: record["sha256"] for record in records}


def validate_evidence_document(document: Any) -> None:
    """Apply cross-field, provenance, privacy, and boundedness checks."""

    validate_evidence_schema(document)
    encoded = json.dumps(document, ensure_ascii=True, separators=(",", ":"), sort_keys=True)
    _require(len(encoded.encode("utf-8")) <= MAX_EVIDENCE_BYTES, "evidence_oversized")
    lowered = encoded.lower()
    _require(not any(marker in lowered for marker in PRIVATE_MARKERS), "evidence_private_data")

    source = document["source"]
    revision = source["revision"]
    ebuild_hash = source["ebuild_sha256"]
    _require(source["repository_uri"] == SOURCE_URI, "source_repository_mismatch")

    resolutions = document["resolutions"]
    _require(tuple(item["mode"] for item in resolutions) == ("default", "use_test"),
             "pretend_resolution_order_mismatch")
    _require(tuple(item["use_test"] for item in resolutions) == (False, True),
             "pretend_resolution_use_mismatch")
    _require(all(item["source_revision"] == revision for item in resolutions),
             "source_revision_mismatch")
    _require(all(item["ebuild_sha256"] == ebuild_hash for item in resolutions),
             "ebuild_sha256_mismatch")
    _require(resolutions[0]["dependency_graph_sha256"]
             != resolutions[1]["dependency_graph_sha256"],
             "pretend_dependency_graphs_not_distinct")

    tested = document["tested_artifact"]
    artifact = tested["artifact_sha256"]
    _require(tested["source_revision"] == revision, "source_revision_mismatch")
    _require(tested["ebuild_sha256"] == ebuild_hash, "ebuild_sha256_mismatch")

    package_test = document["package_test"]
    _require_binding(package_test, revision, artifact)
    _require(package_test["ebuild_sha256"] == ebuild_hash, "ebuild_sha256_mismatch")
    _require(tuple(package_test["excluded_tests"]) == EXCLUDED_TESTS,
             "package_test_exclusions_mismatch")

    installed = document["installed"]
    _require_binding(installed, revision, artifact)
    try:
        _require(canonical_artifact_sha256(installed["executables"]) == artifact,
                 "artifact_sha256_mismatch")
    except LifecycleHashError as error:
        raise EvidenceError(str(error)) from error
    installed_hashes = _executable_map(installed["executables"])
    ownership = installed["ownership"]
    _require(tuple(ownership["paths"]) == INSTALLED_PATHS,
             "installed_ownership_paths_mismatch")

    runtime = document["runtime_validation"]
    _require_binding(runtime, revision, artifact)
    linkage_entries = runtime["linkage"]["entries"]
    _require(tuple(entry["path"] for entry in linkage_entries) == INSTALLED_PATHS,
             "runtime_linkage_paths_mismatch")
    for entry in linkage_entries:
        _require(entry["executable_sha256"] == installed_hashes[entry["path"]],
                 "runtime_executable_sha256_mismatch")
        _require(entry["needed"] == sorted(entry["needed"]),
                 "runtime_linkage_not_normalized")
    needed = {library for entry in linkage_entries for library in entry["needed"]}
    _require("libtomlplusplus.so.3" in needed, "runtime_linkage_incomplete")
    try:
        _require(runtime["linkage"]["capture_sha256"] ==
                 canonical_linkage_sha256(linkage_entries), "runtime_linkage_hash_mismatch")
    except LifecycleHashError as error:
        raise EvidenceError(str(error)) from error

    exclusions = document["sandbox_exclusions"]
    _require(tuple(item["test_id"] for item in exclusions) == EXCLUDED_TESTS,
             "sandbox_exclusion_identity_mismatch")
    _require(tuple(item["reason"] for item in exclusions) == EXCLUSION_REASONS,
             "sandbox_exclusion_reason_mismatch")
    for index, exclusion in enumerate(exclusions):
        _require_binding(exclusion, revision, artifact)
        _require(exclusion["passes"] == exclusion["attempts"],
                 "sandbox_exclusion_repeat_failure")
        if index < 2:
            _require(exclusion["attempts"] == 1, "environment_exclusion_repeat_mismatch")
        else:
            _require(exclusion["attempts"] >= 5, "openbox_repeat_count_too_small")

    unmerge = document["unmerge"]
    _require_binding(unmerge, revision, artifact)
    user_data = unmerge["user_data"]
    _require(tuple(item["kind"] for item in user_data) == ("xdg_config", "xdg_state"),
             "user_data_scope_mismatch")
    for item in user_data:
        _require(item["before_sha256"] == item["after_sha256"],
                 "user_data_changed_during_unmerge")

    reinstall = document["ordinary_reinstall"]
    _require_binding(reinstall, revision, artifact)
    _require(reinstall["ebuild_sha256"] == ebuild_hash, "ebuild_sha256_mismatch")
    reinstall_hashes = _executable_map(reinstall["executables"])
    _require(reinstall_hashes == installed_hashes, "ordinary_reinstall_executable_mismatch")


def example_evidence_document() -> dict[str, Any]:
    """Return synthetic contract-test data; this is not captured lifecycle evidence."""

    revision = "0123456789abcdef0123456789abcdef01234567"
    ebuild_hash = "a" * 64
    executable_hashes = ("c" * 64, "d" * 64, "e" * 64)

    def executables() -> list[dict[str, Any]]:
        return [
            {"path": path, "sha256": digest, "regular_executable": True}
            for path, digest in zip(INSTALLED_PATHS, executable_hashes)
        ]

    artifact_hash = canonical_artifact_sha256(executables())
    linkage_entries = [
        {
            "path": INSTALLED_PATHS[0],
            "executable_sha256": executable_hashes[0],
            "needed": ["libQt6Core.so.6", "libc.so.6"],
        },
        {
            "path": INSTALLED_PATHS[1],
            "executable_sha256": executable_hashes[1],
            "needed": ["libQt6Core.so.6", "libtomlplusplus.so.3"],
        },
        {
            "path": INSTALLED_PATHS[2],
            "executable_sha256": executable_hashes[2],
            "needed": ["libQt6Core.so.6", "libQt6Quick.so.6"],
        },
    ]

    return {
        "schema_version": 1,
        "evidence_kind": "pd1_portage_product_lifecycle",
        "source": {
            "revision": revision,
            "repository_uri": SOURCE_URI,
            "repository_name": "prismdrake-local",
            "atom": PACKAGE_ATOM,
            "version": "9999",
            "ebuild_path": "x11-misc/prismdrake/prismdrake-9999.ebuild",
            "ebuild_sha256": ebuild_hash,
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
        "tested_artifact": {
            "source_revision": revision,
            "ebuild_sha256": ebuild_hash,
            "atom": PACKAGE_VERSIONED_ATOM,
            "provenance": "portage_installed",
            "use_test": True,
            "artifact_sha256": artifact_hash,
        },
        "package_test": {
            "source_revision": revision,
            "ebuild_sha256": ebuild_hash,
            "artifact_sha256": artifact_hash,
            "features_test": True,
            "result": "passed",
            "executed_test_count": 524,
            "excluded_tests": list(EXCLUDED_TESTS),
        },
        "installed": {
            "source_revision": revision,
            "artifact_sha256": artifact_hash,
            "provenance": "portage_installed",
            "data_prefix": "/usr/share/prismdrake",
            "executables": executables(),
            "ownership": {
                "tool": "equery",
                "owner_atom": PACKAGE_VERSIONED_ATOM,
                "owned_file_count": 13,
                "file_list_sha256": "3" * 64,
                "paths": list(INSTALLED_PATHS),
            },
        },
        "runtime_validation": {
            "source_revision": revision,
            "artifact_sha256": artifact_hash,
            "atspi": {
                "result": "passed",
                "complete": True,
                "evidence_schema_version": 1,
                "evidence_sha256": "f" * 64,
            },
            "complete_demo": {
                "result": "passed",
                "complete": True,
                "artifact_provenance": "portage_installed",
                "evidence_schema_version": 3,
                "evidence_sha256": "0" * 64,
                "driver_sha256": "7" * 64,
            },
            "linkage": {
                "tool": "scanelf",
                "capture_sha256": canonical_linkage_sha256(linkage_entries),
                "entries": linkage_entries,
            },
        },
        "sandbox_exclusions": [
            {
                "test_id": test_id,
                "reason": reason,
                "source_revision": revision,
                "artifact_sha256": artifact_hash,
                "execution_context": "outside_portage_sandbox",
                "result": "passed",
                "attempts": 5 if index >= 2 else 1,
                "passes": 5 if index >= 2 else 1,
            }
            for index, (test_id, reason) in enumerate(zip(EXCLUDED_TESTS, EXCLUSION_REASONS))
        ],
        "unmerge": {
            "source_revision": revision,
            "artifact_sha256": artifact_hash,
            "atom": PACKAGE_ATOM,
            "result": "passed",
            "installed_executables_absent": True,
            "user_data": [
                {
                    "kind": "xdg_config",
                    "before_sha256": "5" * 64,
                    "after_sha256": "5" * 64,
                    "byte_identical": True,
                },
                {
                    "kind": "xdg_state",
                    "before_sha256": "6" * 64,
                    "after_sha256": "6" * 64,
                    "byte_identical": True,
                },
            ],
        },
        "ordinary_reinstall": {
            "source_revision": revision,
            "ebuild_sha256": ebuild_hash,
            "artifact_sha256": artifact_hash,
            "provenance": "portage_installed",
            "use_test": False,
            "result": "passed",
            "executables": executables(),
            "ownership_atom": PACKAGE_VERSIONED_ATOM,
        },
    }


def _parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    return parser.parse_args()


def main() -> int:
    arguments = _parse_arguments()
    try:
        validate_evidence_document(load_evidence(arguments.evidence))
    except EvidenceError as error:
        print(f"portage_lifecycle_evidence: {error}", file=sys.stderr)
        return 1
    print("portage_lifecycle_evidence: valid bounded version 1 record")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
