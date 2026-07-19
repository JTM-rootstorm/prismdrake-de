#!/usr/bin/env python3
"""Validate a bounded PD2 installed-artifact preflight attestation.

This module validates captured facts and exact file bindings. It never invokes
Portage, installs packages, or manufactures live lifecycle evidence.
"""

from __future__ import annotations

import argparse
from collections.abc import Callable
import json
import math
from pathlib import Path
import sys
from typing import Any

from pd2_lifecycle_hashes import (
    Pd2LifecycleHashError,
    canonical_executable_sha256,
    canonical_ownership_sha256,
    canonical_regular_ownership_sha256,
    normalize_ownership_records,
    regular_ownership_records,
)


ROOT = Path(__file__).resolve().parents[2]
SCHEMA_PATH = Path(__file__).with_name("pd2-installed-artifact-attestation.schema.json")
MAX_ATTESTATION_BYTES = 524_288
SOURCE_URI = "https://github.com/JTM-rootstorm/prismdrake-de.git"
PACKAGE_ATOM = "x11-misc/prismdrake"
PACKAGE_VERSIONED_ATOM = "x11-misc/prismdrake-9999"
INSTALLED_EXECUTABLE_PATHS = (
    "/usr/bin/prismdrake-session",
    "/usr/bin/prismdrake-settingsd",
    "/usr/bin/prismdrake-notifyd",
    "/usr/bin/prismdrake-shell",
    "/usr/bin/prismdrake-shellctl",
)
REQUIRED_INSTALLED_RESOURCE_PATHS = (
    "/usr/share/dbus-1/interfaces/org.prismdrake.NotificationCenter1.xml",
    "/usr/share/dbus-1/interfaces/org.prismdrake.Session1.xml",
    "/usr/share/dbus-1/interfaces/org.prismdrake.Settings1.xml",
    "/usr/share/dbus-1/interfaces/org.prismdrake.Shell1.xml",
    "/usr/share/prismdrake/defaults/config.toml",
    "/usr/share/prismdrake/schemas/prismdrake-config.schema.json",
    "/usr/share/prismdrake/schemas/prismdrake-notification-history.schema.json",
    "/usr/share/prismdrake/schemas/prismdrake-notification-state.schema.json",
    "/usr/share/prismdrake/schemas/prismdrake-pin-state.schema.json",
    "/usr/share/prismdrake/schemas/prismdrake-runtime-snapshot.schema.json",
    "/usr/share/prismdrake/schemas/prismdrake-theme-tokens.schema.json",
    "/usr/share/prismdrake/themes/accessibility.tokens.json",
    "/usr/share/prismdrake/themes/base.tokens.json",
    "/usr/share/prismdrake/themes/forge.tokens.json",
    "/usr/share/prismdrake/themes/lustre.tokens.json",
    "/usr/share/prismdrake/translations/prismdrake_ar_XB.qm",
    "/usr/share/prismdrake/translations/prismdrake_en_XA.qm",
    "/usr/share/xsessions/prismdrake.desktop",
)
_REQUIRED_DOCUMENT_RELATIVE_PATHS = (
    "README.md",
    "index.md",
    "architecture/component-model.md",
    "architecture/configuration.md",
    "architecture/dependency-policy.md",
    "architecture/notification-service.md",
    "architecture/pd2-core-shell.md",
    "architecture/pd2-state-ownership.md",
    "architecture/process-model.md",
    "architecture/shell-control.md",
    "architecture/surface-placement.md",
    "build/building.md",
    "build/dependencies.md",
    "build/testing.md",
    "development/accessibility-testing.md",
    "development/pd2-installed-demonstration.md",
    "packaging/gentoo-local-repository.md",
    "interfaces/README.md",
)
REQUIRED_INSTALLED_FILE_GROUPS = (
    *((path,) for path in REQUIRED_INSTALLED_RESOURCE_PATHS),
    *((f"/usr/share/doc/prismdrake-9999/{path}",
       f"/usr/share/doc/prismdrake-9999/{path}.bz2",
       f"/usr/share/doc/prismdrake-9999/{path}.gz",
       f"/usr/share/doc/prismdrake-9999/{path}.lz4",
       f"/usr/share/doc/prismdrake-9999/{path}.xz",
       f"/usr/share/doc/prismdrake-9999/{path}.zst")
      for path in _REQUIRED_DOCUMENT_RELATIVE_PATHS),
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


class Pd2AttestationError(RuntimeError):
    """A PD2 installed preflight violates the closed contract."""


class DuplicateJsonKeyError(ValueError):
    """Strict JSON loading encountered a repeated object key."""


def _require(condition: bool, identifier: str) -> None:
    if not condition:
        raise Pd2AttestationError(identifier)


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
    """Load one bounded UTF-8 attestation without duplicate keys."""

    try:
        raw = path.read_bytes()
    except OSError as error:
        raise Pd2AttestationError("attestation_read_failed") from error
    _require(0 < len(raw) <= MAX_ATTESTATION_BYTES, "attestation_size_invalid")
    try:
        return json.loads(
            raw.decode("utf-8"),
            object_pairs_hook=_reject_duplicate_keys,
            parse_constant=_reject_constant,
            parse_float=_parse_finite_float,
        )
    except (UnicodeError, ValueError, json.JSONDecodeError) as error:
        raise Pd2AttestationError("attestation_json_invalid") from error


def validate_attestation_schema(document: Any) -> None:
    """Validate the versioned closed schema with the project validator."""

    try:
        schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as error:
        raise Pd2AttestationError("attestation_schema_unavailable") from error
    tools_path = str(ROOT / "tools")
    if tools_path not in sys.path:
        sys.path.insert(0, tools_path)
    from validate import validate_schema  # pylint: disable=import-outside-toplevel

    _require(
        not validate_schema(document, schema, schema, "pd2_installed_attestation"),
        "attestation_schema_mismatch",
    )


def validate_attestation_document(document: Any) -> None:
    """Apply exact source, artifact, ownership, and privacy semantics."""

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
    for record in (tested, installed):
        _require(record["source_revision"] == revision, "source_revision_mismatch")
        _require(record["ebuild_sha256"] == ebuild_hash, "ebuild_sha256_mismatch")

    executables = installed["executables"]
    _require(tuple(record["path"] for record in executables) == INSTALLED_EXECUTABLE_PATHS,
             "installed_executable_paths_mismatch")
    try:
        artifact_hash = canonical_executable_sha256(executables)
        ownership_records = normalize_ownership_records(installed["ownership"]["paths"])
        ownership_hash = canonical_ownership_sha256(ownership_records)
        regular_records = regular_ownership_records(ownership_records)
        regular_hash = canonical_regular_ownership_sha256(ownership_records)
    except Pd2LifecycleHashError as error:
        raise Pd2AttestationError(str(error)) from error
    _require(tested["artifact_sha256"] == artifact_hash and
             installed["artifact_sha256"] == artifact_hash, "artifact_sha256_mismatch")
    ownership = installed["ownership"]
    _require(ownership["paths"] == ownership_records, "ownership_paths_not_normalized")
    _require(ownership["path_count"] == len(ownership_records), "ownership_path_count_mismatch")
    _require(ownership["regular_path_count"] == len(regular_records),
             "ownership_regular_path_count_mismatch")
    _require(ownership["path_set_sha256"] == ownership_hash, "ownership_hash_mismatch")
    _require(ownership["regular_path_set_sha256"] == regular_hash,
             "ownership_regular_hash_mismatch")
    owned_regular_paths = {record["path"] for record in regular_records}
    _require(set(INSTALLED_EXECUTABLE_PATHS) <= owned_regular_paths,
             "executables_not_owned_regular_paths")
    _require(all(any(path in owned_regular_paths for path in alternatives)
                 for alternatives in REQUIRED_INSTALLED_FILE_GROUPS),
             "required_installed_file_missing")


def validate_attested_files(
    document: dict[str, Any], executable_paths: tuple[Path, Path, Path, Path, Path],
    driver: Path, hash_file: Callable[[Path], str],
) -> None:
    """Bind a preflight to the exact installed executables and demo driver."""

    validate_attestation_document(document)
    _require(tuple(str(path) for path in executable_paths) == INSTALLED_EXECUTABLE_PATHS,
             "installed_executable_path_mismatch")
    expected = {record["path"]: record["sha256"]
                for record in document["installed"]["executables"]}
    _require(all(hash_file(path) == expected[str(path)] for path in executable_paths),
             "installed_executable_hash_mismatch")
    _require(hash_file(driver) == document["demo_driver"]["sha256"],
             "installed_demo_driver_mismatch")


def example_attestation_document() -> dict[str, Any]:
    """Return synthetic contract-test data, never a captured VM result."""

    digests = ("b" * 64, "c" * 64, "d" * 64, "e" * 64, "f" * 64)
    executables = [
        {"path": path, "sha256": digest, "regular_executable": True}
        for path, digest in zip(INSTALLED_EXECUTABLE_PATHS, digests)
    ]
    ownership_records = normalize_ownership_records([
        *({"kind": "regular", "path": path} for path in INSTALLED_EXECUTABLE_PATHS),
        *({"kind": "regular", "path": alternatives[0]}
          for alternatives in REQUIRED_INSTALLED_FILE_GROUPS),
        {"kind": "directory", "path": "/usr/share/prismdrake"},
        {"kind": "symlink", "path": "/usr/share/prismdrake/current-theme"},
    ])
    artifact_hash = canonical_executable_sha256(executables)
    revision = "0123456789abcdef0123456789abcdef01234567"
    ebuild_hash = "a" * 64
    return {
        "schema_version": 1,
        "attestation_kind": "pd2_installed_artifact_preflight",
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
            "ebuild_sha256": ebuild_hash,
            "artifact_sha256": artifact_hash,
            "provenance": "portage_installed",
            "data_prefix": "/usr/share/prismdrake",
            "executables": executables,
            "ownership": {
                "tool": "equery",
                "owner_atom": PACKAGE_VERSIONED_ATOM,
                "path_count": len(ownership_records),
                "regular_path_count": len(regular_ownership_records(ownership_records)),
                "path_set_sha256": canonical_ownership_sha256(ownership_records),
                "regular_path_set_sha256":
                    canonical_regular_ownership_sha256(ownership_records),
                "paths": ownership_records,
            },
        },
        "demo_driver": {
            "kind": "pd2_installed_demonstration_driver",
            "sha256": "7" * 64,
        },
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("attestation", type=Path)
    arguments = parser.parse_args(argv)
    try:
        validate_attestation_document(load_attestation(arguments.attestation))
    except Pd2AttestationError as error:
        print(f"pd2_installed_artifact_attestation: {error}", file=sys.stderr)
        return 1
    print("pd2_installed_artifact_attestation: valid bounded version 1 record")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
