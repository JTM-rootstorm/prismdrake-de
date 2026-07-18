#!/usr/bin/env python3
"""Validate the non-circular PD1 installed-artifact preflight attestation."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import sys
from typing import Any, Callable

from lifecycle_hashes import LifecycleHashError, canonical_artifact_sha256


ROOT = Path(__file__).resolve().parents[2]
SCHEMA_PATH = Path(__file__).with_name("pd1-installed-artifact-attestation.schema.json")
MAX_ATTESTATION_BYTES = 12_288
SOURCE_URI = "https://github.com/JTM-rootstorm/prismdrake-de.git"
PACKAGE_ATOM = "x11-misc/prismdrake"
PACKAGE_VERSIONED_ATOM = "x11-misc/prismdrake-9999"
INSTALLED_PATHS = (
    "/usr/bin/prismdrake-session",
    "/usr/bin/prismdrake-settingsd",
    "/usr/bin/prismdrake-shell",
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


class AttestationError(RuntimeError):
    """An installed-artifact attestation violates the closed contract."""


class DuplicateJsonKeyError(ValueError):
    """Strict JSON loading encountered a repeated object key."""


def _require(condition: bool, identifier: str) -> None:
    if not condition:
        raise AttestationError(identifier)


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


def load_attestation(path: Path) -> Any:
    """Load one bounded, strict JSON attestation."""

    try:
        raw = path.read_bytes()
    except OSError as error:
        raise AttestationError("attestation_read_failed") from error
    _require(len(raw) <= MAX_ATTESTATION_BYTES, "attestation_oversized")
    try:
        return json.loads(
            raw.decode("utf-8"),
            object_pairs_hook=_reject_duplicate_keys,
            parse_constant=_reject_constant,
            parse_float=_parse_finite_float,
        )
    except (UnicodeError, ValueError, json.JSONDecodeError) as error:
        raise AttestationError("attestation_json_invalid") from error


def validate_attestation_schema(document: Any) -> None:
    """Validate against the tracked, closed schema."""

    try:
        schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as error:
        raise AttestationError("attestation_schema_unavailable") from error
    tools_path = str(ROOT / "tools")
    if tools_path not in sys.path:
        sys.path.insert(0, tools_path)
    from validate import validate_schema  # pylint: disable=import-outside-toplevel

    errors = validate_schema(document, schema, schema, "installed_artifact_attestation")
    _require(not errors, "attestation_schema_mismatch")


def validate_attestation_document(document: Any) -> None:
    """Apply provenance, binding, privacy, and canonical-hash checks."""

    validate_attestation_schema(document)
    encoded = json.dumps(document, ensure_ascii=True, separators=(",", ":"), sort_keys=True)
    _require(len(encoded.encode("utf-8")) <= MAX_ATTESTATION_BYTES, "attestation_oversized")
    lowered = encoded.lower()
    _require(not any(marker in lowered for marker in PRIVATE_MARKERS),
             "attestation_private_data")

    source = document["source"]
    tested = document["tested_artifact"]
    installed = document["installed"]
    revision = source["revision"]
    ebuild_hash = source["ebuild_sha256"]
    _require(source["repository_uri"] == SOURCE_URI, "source_repository_mismatch")
    _require(tested["source_revision"] == revision, "source_revision_mismatch")
    _require(tested["ebuild_sha256"] == ebuild_hash, "ebuild_sha256_mismatch")
    _require(installed["source_revision"] == revision, "source_revision_mismatch")

    executables = installed["executables"]
    _require(tuple(record["path"] for record in executables) == INSTALLED_PATHS,
             "installed_executable_paths_mismatch")
    try:
        artifact_hash = canonical_artifact_sha256(executables)
    except LifecycleHashError as error:
        raise AttestationError(str(error)) from error
    _require(tested["artifact_sha256"] == artifact_hash, "artifact_sha256_mismatch")
    _require(installed["artifact_sha256"] == artifact_hash, "artifact_sha256_mismatch")
    _require(tuple(installed["ownership"]["paths"]) == INSTALLED_PATHS,
             "installed_ownership_paths_mismatch")


def validate_attested_files(
        document: dict[str, Any], executable_paths: tuple[Path, Path, Path], driver: Path,
        hash_file: Callable[[Path], str]) -> None:
    """Bind a validated attestation to the exact files about to be executed."""

    validate_attestation_document(document)
    _require(tuple(str(path) for path in executable_paths) == INSTALLED_PATHS,
             "installed_executable_path_mismatch")
    expected = {record["path"]: record["sha256"]
                for record in document["installed"]["executables"]}
    _require(all(hash_file(path) == expected[str(path)] for path in executable_paths),
             "installed_executable_hash_mismatch")
    _require(hash_file(driver) == document["demo_driver"]["sha256"],
             "installed_demo_driver_mismatch")


def example_attestation_document() -> dict[str, Any]:
    """Return synthetic contract-test data, not a captured VM attestation."""

    executables = [
        {"path": path, "sha256": digest, "regular_executable": True}
        for path, digest in zip(INSTALLED_PATHS, ("c" * 64, "d" * 64, "e" * 64))
    ]
    artifact_hash = canonical_artifact_sha256(executables)
    revision = "0123456789abcdef0123456789abcdef01234567"
    ebuild_hash = "a" * 64
    return {
        "schema_version": 1,
        "attestation_kind": "pd1_installed_artifact_preflight",
        "source": {
            "revision": revision,
            "repository_uri": SOURCE_URI,
            "repository_name": "prismdrake-local",
            "atom": PACKAGE_ATOM,
            "version": "9999",
            "ebuild_path": "x11-misc/prismdrake/prismdrake-9999.ebuild",
            "ebuild_sha256": ebuild_hash,
        },
        "tested_artifact": {
            "source_revision": revision,
            "ebuild_sha256": ebuild_hash,
            "atom": PACKAGE_VERSIONED_ATOM,
            "provenance": "portage_installed",
            "use_test": True,
            "artifact_sha256": artifact_hash,
        },
        "installed": {
            "source_revision": revision,
            "artifact_sha256": artifact_hash,
            "provenance": "portage_installed",
            "data_prefix": "/usr/share/prismdrake",
            "executables": executables,
            "ownership": {
                "tool": "equery",
                "owner_atom": PACKAGE_VERSIONED_ATOM,
                "owned_file_count": 13,
                "file_list_sha256": "3" * 64,
                "paths": list(INSTALLED_PATHS),
            },
        },
        "demo_driver": {"path_class": "exact_source_driver", "sha256": "7" * 64},
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("attestation", type=Path)
    arguments = parser.parse_args()
    try:
        validate_attestation_document(load_attestation(arguments.attestation))
    except AttestationError as error:
        print(f"installed_artifact_attestation: {error}", file=sys.stderr)
        return 1
    print("installed_artifact_attestation: valid bounded version 1 record")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
