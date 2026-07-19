#!/usr/bin/env python3
"""Collect bounded PD2 lifecycle facts without invoking Portage.

Every operation consumes captures made by the VM operator or inspects an exact
provided file. This module has no Portage execution path and cannot install,
unmerge, resolve, test, or demonstrate a package.
"""

from __future__ import annotations

import argparse
from collections.abc import Callable, Mapping
import hashlib
import json
import math
import os
from pathlib import Path
import re
import stat
import sys
import tempfile
from typing import Any

from pd2_installed_artifact_attestation import (
    INSTALLED_EXECUTABLE_PATHS,
    Pd2AttestationError,
    load_attestation,
    validate_attestation_document,
)
from pd2_lifecycle_hashes import (
    Pd2LifecycleHashError,
    canonical_executable_sha256,
    canonical_ownership_sha256,
    canonical_regular_ownership_sha256,
    normalize_ownership_records,
    regular_ownership_records,
)
from pd2_portage_lifecycle_evidence import (
    MAX_EVIDENCE_BYTES,
    Pd2EvidenceError,
    USER_DATA_KINDS,
    load_evidence,
    validate_evidence_document,
)


MAX_CAPTURE_FILE_BYTES = 256 * 1024 * 1024
MAX_CAPTURE_JSON_BYTES = 524_288


class Pd2CollectorError(RuntimeError):
    """A collector stage cannot produce closed, bound lifecycle facts."""


class DuplicateJsonKeyError(ValueError):
    """Strict JSON loading encountered a repeated object key."""


def _require(condition: bool, identifier: str) -> None:
    if not condition:
        raise Pd2CollectorError(identifier)


def _require_sha256(value: Any) -> str:
    _require(isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None,
             "capture_sha256_invalid")
    return value


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


def load_capture_json(path: Path) -> Any:
    """Load one bounded external capture with strict JSON semantics."""

    try:
        raw = path.read_bytes()
    except OSError as error:
        raise Pd2CollectorError("capture_json_read_failed") from error
    _require(0 < len(raw) <= MAX_CAPTURE_JSON_BYTES, "capture_json_size_invalid")
    try:
        return json.loads(
            raw.decode("utf-8"),
            object_pairs_hook=_reject_duplicate_keys,
            parse_constant=_reject_constant,
            parse_float=_parse_finite_float,
        )
    except (UnicodeError, ValueError, json.JSONDecodeError) as error:
        raise Pd2CollectorError("capture_json_invalid") from error


def sha256_regular_file(path: Path) -> str:
    """Hash one bounded regular file without following a final symlink."""

    _require(path.is_absolute() and not path.is_symlink(), "capture_file_invalid")
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise Pd2CollectorError("capture_file_unavailable") from error
    try:
        metadata = os.fstat(descriptor)
        _require(stat.S_ISREG(metadata.st_mode) and
                 metadata.st_size <= MAX_CAPTURE_FILE_BYTES,
                 "capture_file_invalid")
        digest = hashlib.sha256()
        total = 0
        while True:
            block = os.read(descriptor, 64 * 1024)
            if not block:
                break
            total += len(block)
            _require(total <= metadata.st_size, "capture_file_changed")
            digest.update(block)
        after = os.fstat(descriptor)
        _require(total == metadata.st_size and after.st_size == metadata.st_size and
                 after.st_mtime_ns == metadata.st_mtime_ns,
                 "capture_file_changed")
        return digest.hexdigest()
    finally:
        os.close(descriptor)


def atomic_write_json(path: Path, document: dict[str, Any]) -> None:
    """Atomically publish bounded evidence with final mode 0600."""

    _require(path.is_absolute() and path.parent.is_dir() and not path.parent.is_symlink(),
             "output_path_invalid")
    _require(not path.is_symlink() and (not path.exists() or path.is_file()),
             "output_target_invalid")
    payload = (json.dumps(
        document, allow_nan=False, ensure_ascii=True, indent=2, sort_keys=True,
    ) + "\n").encode("utf-8")
    _require(0 < len(payload) <= MAX_EVIDENCE_BYTES, "output_size_invalid")
    temporary: Path | None = None
    try:
        descriptor, name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
        temporary = Path(name)
        os.fchmod(descriptor, 0o600)
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        temporary = None
        metadata = path.lstat()
        _require(stat.S_ISREG(metadata.st_mode) and stat.S_IMODE(metadata.st_mode) == 0o600,
                 "output_mode_invalid")
        directory = os.open(path.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    except OSError as error:
        raise Pd2CollectorError("output_write_failed") from error
    finally:
        if temporary is not None:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass


def _default_is_executable(path: Path) -> bool:
    return path.is_file() and not path.is_symlink() and os.access(path, os.X_OK)


def capture_preflight_attestation(
        source: dict[str, Any], owned_paths: list[dict[str, str]], driver: Path,
        hash_file: Callable[[Path], str] = sha256_regular_file,
        is_executable: Callable[[Path], bool] = _default_is_executable,
) -> dict[str, Any]:
    """Inspect the five installed binaries and normalize full ownership."""

    try:
        ownership_records = normalize_ownership_records(owned_paths)
        regular_records = regular_ownership_records(ownership_records)
    except Pd2LifecycleHashError as error:
        raise Pd2CollectorError(str(error)) from error
    executable_paths = tuple(Path(path) for path in INSTALLED_EXECUTABLE_PATHS)
    _require(all(is_executable(path) for path in executable_paths),
             "installed_executable_invalid")
    executables = [
        {"path": str(path), "sha256": hash_file(path), "regular_executable": True}
        for path in executable_paths
    ]
    artifact_hash = canonical_executable_sha256(executables)
    document = {
        "schema_version": 1,
        "attestation_kind": "pd2_installed_artifact_preflight",
        "source": source,
        "tested_artifact": {
            "source_revision": source.get("revision"),
            "ebuild_sha256": source.get("ebuild_sha256"),
            "atom": "x11-misc/prismdrake-9999",
            "provenance": "portage_installed",
            "use_test": True,
            "artifact_sha256": artifact_hash,
        },
        "installed": {
            "source_revision": source.get("revision"),
            "ebuild_sha256": source.get("ebuild_sha256"),
            "artifact_sha256": artifact_hash,
            "provenance": "portage_installed",
            "data_prefix": "/usr/share/prismdrake",
            "executables": executables,
            "ownership": {
                "tool": "equery",
                "owner_atom": "x11-misc/prismdrake-9999",
                "path_count": len(ownership_records),
                "regular_path_count": len(regular_records),
                "path_set_sha256": canonical_ownership_sha256(ownership_records),
                "regular_path_set_sha256":
                    canonical_regular_ownership_sha256(ownership_records),
                "paths": ownership_records,
            },
        },
        "demo_driver": {
            "kind": "pd2_installed_demonstration_driver",
            "sha256": _require_sha256(hash_file(driver)),
        },
    }
    try:
        validate_attestation_document(document)
    except Pd2AttestationError as error:
        raise Pd2CollectorError(str(error)) from error
    return document


def capture_user_state_preservation(
        before: Mapping[str, Path], after: Mapping[str, Path],
        hash_file: Callable[[Path], str] = sha256_regular_file,
) -> list[dict[str, str]]:
    """Return only normalized kind and byte hashes for three user-state files."""

    _require(tuple(before) == USER_DATA_KINDS and tuple(after) == USER_DATA_KINDS,
             "user_data_scope_mismatch")
    records = []
    for kind in USER_DATA_KINDS:
        before_hash = _require_sha256(hash_file(before[kind]))
        after_hash = _require_sha256(hash_file(after[kind]))
        _require(before_hash == after_hash, "user_data_changed_during_unmerge")
        records.append({
            "kind": kind,
            "before_sha256": before_hash,
            "after_sha256": after_hash,
        })
    return records


def capture_unmerge_absence(
        owned_paths: list[dict[str, str]],
        path_exists: Callable[[Path], bool] = os.path.lexists,
) -> dict[str, Any]:
    """Prove that every formerly owned regular path is absent after unmerge."""

    try:
        regular_records = regular_ownership_records(owned_paths)
        regular_hash = canonical_regular_ownership_sha256(owned_paths)
    except Pd2LifecycleHashError as error:
        raise Pd2CollectorError(str(error)) from error
    _require(not any(path_exists(Path(record["path"])) for record in regular_records),
             "owned_regular_path_remains")
    return {
        "owned_regular_paths_absent": True,
        "absent_path_count": len(regular_records),
        "regular_path_set_sha256": regular_hash,
    }


def finalize_lifecycle(
        attestation: dict[str, Any], draft: dict[str, Any],
) -> dict[str, Any]:
    """Bind a complete draft to the preflight and validate every phase."""

    try:
        validate_attestation_document(attestation)
    except Pd2AttestationError as error:
        raise Pd2CollectorError(str(error)) from error
    _require(draft.get("source") == attestation["source"], "final_source_mismatch")
    _require(draft.get("tested_artifact") == attestation["tested_artifact"],
             "final_tested_artifact_mismatch")
    _require(draft.get("initial_install") == attestation["installed"],
             "final_initial_install_mismatch")
    demonstrations = draft.get("installed_demonstrations")
    _require(isinstance(demonstrations, list) and len(demonstrations) == 2,
             "final_demonstrations_missing")
    _require(all(item.get("driver_sha256") == attestation["demo_driver"]["sha256"]
                 for item in demonstrations),
             "final_demo_driver_mismatch")
    try:
        validate_evidence_document(draft)
    except Pd2EvidenceError as error:
        raise Pd2CollectorError(str(error)) from error
    return draft


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    preflight = subparsers.add_parser("preflight")
    preflight.add_argument("--source", type=Path, required=True)
    preflight.add_argument("--owned-paths", type=Path, required=True)
    preflight.add_argument("--driver", type=Path, required=True)
    preflight.add_argument("--output", type=Path, required=True)
    preservation = subparsers.add_parser("preservation")
    for phase in ("before", "after"):
        for kind in USER_DATA_KINDS:
            preservation.add_argument(f"--{phase}-{kind.replace('_', '-')}",
                                      type=Path, required=True)
    preservation.add_argument("--output", type=Path, required=True)
    unmerge = subparsers.add_parser("unmerge")
    unmerge.add_argument("--attestation", type=Path, required=True)
    unmerge.add_argument("--output", type=Path, required=True)
    finalize = subparsers.add_parser("finalize")
    finalize.add_argument("--attestation", type=Path, required=True)
    finalize.add_argument("--draft", type=Path, required=True)
    finalize.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args(argv)
    try:
        if arguments.command == "preflight":
            source = load_capture_json(arguments.source)
            owned_paths = load_capture_json(arguments.owned_paths)
            _require(isinstance(source, dict) and isinstance(owned_paths, list),
                     "capture_json_shape_invalid")
            document = capture_preflight_attestation(source, owned_paths, arguments.driver)
        elif arguments.command == "preservation":
            before = {kind: getattr(arguments, f"before_{kind}") for kind in USER_DATA_KINDS}
            after = {kind: getattr(arguments, f"after_{kind}") for kind in USER_DATA_KINDS}
            document = {"user_data": capture_user_state_preservation(before, after)}
        elif arguments.command == "unmerge":
            attestation = load_attestation(arguments.attestation)
            validate_attestation_document(attestation)
            document = capture_unmerge_absence(attestation["installed"]["ownership"]["paths"])
        else:
            attestation = load_attestation(arguments.attestation)
            draft = load_evidence(arguments.draft)
            document = finalize_lifecycle(attestation, draft)
        atomic_write_json(arguments.output, document)
    except (Pd2AttestationError, Pd2CollectorError, Pd2EvidenceError,
            Pd2LifecycleHashError) as error:
        print(f"pd2_portage_lifecycle_collector: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
