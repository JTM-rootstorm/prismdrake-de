#!/usr/bin/env python3
"""Build strict PD1 preflight and final lifecycle records from normalized stages."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
from typing import Any, Callable

from installed_artifact_attestation import (
    AttestationError,
    INSTALLED_PATHS,
    example_attestation_document,
    load_attestation,
    validate_attestation_document,
)
from lifecycle_hashes import (
    LifecycleHashError,
    canonical_artifact_sha256,
    canonical_ownership_sha256,
    canonical_tree_sha256,
)
from portage_lifecycle_evidence import (
    EvidenceError,
    load_evidence,
    validate_evidence_document,
)


class CollectorError(RuntimeError):
    """A capture stage cannot produce bound lifecycle evidence."""


def _require(condition: bool, identifier: str) -> None:
    if not condition:
        raise CollectorError(identifier)


def sha256_regular_file(path: Path) -> str:
    """Hash one bounded regular file without following a final symlink."""

    _require(path.is_absolute() and not path.is_symlink(), "capture_file_invalid")
    flags = os.O_RDONLY
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise CollectorError("capture_file_unavailable") from error
    try:
        metadata = os.fstat(descriptor)
        _require(stat.S_ISREG(metadata.st_mode) and metadata.st_size <= 256 * 1024 * 1024,
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
        _require(total == metadata.st_size and os.fstat(descriptor).st_size == metadata.st_size,
                 "capture_file_changed")
        return digest.hexdigest()
    finally:
        os.close(descriptor)


def atomic_write_json(path: Path, document: dict[str, Any]) -> None:
    """Atomically publish private JSON with final mode 0600."""

    _require(path.is_absolute() and path.parent.is_dir() and not path.parent.is_symlink(),
             "output_path_invalid")
    _require(not path.is_symlink() and (not path.exists() or path.is_file()),
             "output_target_invalid")
    payload = (json.dumps(document, ensure_ascii=True, indent=2, sort_keys=True) + "\n").encode()
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
        os.chmod(path, 0o600)
        directory = os.open(path.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    except OSError as error:
        raise CollectorError("output_write_failed") from error
    finally:
        if temporary is not None:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass


def build_preflight_attestation(
        source: dict[str, Any], tested_artifact: dict[str, Any],
        executables: list[dict[str, Any]], owned_files: list[str], driver_sha256: str,
        ownership_tool: str = "equery") -> dict[str, Any]:
    """Build the non-final attestation needed before running the demo."""

    try:
        artifact_hash = canonical_artifact_sha256(executables)
        ownership_hash = canonical_ownership_sha256(owned_files)
    except LifecycleHashError as error:
        raise CollectorError(str(error)) from error
    _require(tuple(record.get("path") for record in executables) == INSTALLED_PATHS,
             "installed_executable_paths_mismatch")
    _require(set(INSTALLED_PATHS).issubset(owned_files), "installed_ownership_incomplete")
    _require(13 <= len(owned_files) <= 10000, "installed_ownership_count_invalid")
    _require(tested_artifact.get("artifact_sha256") == artifact_hash,
             "artifact_sha256_mismatch")

    document = {
        "schema_version": 1,
        "attestation_kind": "pd1_installed_artifact_preflight",
        "source": source,
        "tested_artifact": tested_artifact,
        "installed": {
            "source_revision": source.get("revision"),
            "artifact_sha256": artifact_hash,
            "provenance": "portage_installed",
            "data_prefix": "/usr/share/prismdrake",
            "executables": executables,
            "ownership": {
                "tool": ownership_tool,
                "owner_atom": "x11-misc/prismdrake-9999",
                "owned_file_count": len(owned_files),
                "file_list_sha256": ownership_hash,
                "paths": list(INSTALLED_PATHS),
            },
        },
        "demo_driver": {"path_class": "exact_source_driver", "sha256": driver_sha256},
    }
    try:
        validate_attestation_document(document)
    except AttestationError as error:
        raise CollectorError(str(error)) from error
    return document


def capture_preflight_attestation(
        source: dict[str, Any], owned_files: list[str], driver: Path,
        hash_file: Callable[[Path], str] = sha256_regular_file) -> dict[str, Any]:
    """Measure the exact installed executables and build their preflight record."""

    executable_paths = tuple(Path(path) for path in INSTALLED_PATHS)
    _require(all(path.is_file() and not path.is_symlink() and os.access(path, os.X_OK)
                 for path in executable_paths), "installed_executable_invalid")
    executables = [
        {"path": str(path), "sha256": hash_file(path), "regular_executable": True}
        for path in executable_paths
    ]
    artifact_hash = canonical_artifact_sha256(executables)
    tested_artifact = {
        "source_revision": source.get("revision"),
        "ebuild_sha256": source.get("ebuild_sha256"),
        "atom": "x11-misc/prismdrake-9999",
        "provenance": "portage_installed",
        "use_test": True,
        "artifact_sha256": artifact_hash,
    }
    return build_preflight_attestation(
        source, tested_artifact, executables, owned_files, hash_file(driver),
    )


def finalize_lifecycle(
        attestation: dict[str, Any], draft: dict[str, Any]) -> dict[str, Any]:
    """Bind a complete draft to its pre-demo attestation and validate it."""

    try:
        validate_attestation_document(attestation)
    except AttestationError as error:
        raise CollectorError(str(error)) from error
    _require(draft.get("source") == attestation["source"], "final_source_mismatch")
    _require(draft.get("tested_artifact") == attestation["tested_artifact"],
             "final_tested_artifact_mismatch")
    _require(draft.get("installed") == attestation["installed"], "final_installed_mismatch")
    complete_demo = draft.get("runtime_validation", {}).get("complete_demo", {})
    _require(complete_demo.get("driver_sha256") == attestation["demo_driver"]["sha256"],
             "final_demo_driver_mismatch")
    try:
        validate_evidence_document(draft)
    except EvidenceError as error:
        raise CollectorError(str(error)) from error
    return draft


def _load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise CollectorError("capture_json_invalid") from error


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    tree = subparsers.add_parser("tree-hash")
    tree.add_argument("root", type=Path)
    finalize = subparsers.add_parser("finalize")
    finalize.add_argument("--attestation", type=Path, required=True)
    finalize.add_argument("--draft", type=Path, required=True)
    finalize.add_argument("--output", type=Path, required=True)
    preflight = subparsers.add_parser("preflight")
    preflight.add_argument("--source", type=Path, required=True)
    preflight.add_argument("--owned-files", type=Path, required=True)
    preflight.add_argument("--driver", type=Path, required=True)
    preflight.add_argument("--output", type=Path, required=True)
    example = subparsers.add_parser("example-preflight")
    example.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        if arguments.command == "tree-hash":
            print(canonical_tree_sha256(arguments.root))
        elif arguments.command == "example-preflight":
            atomic_write_json(arguments.output, example_attestation_document())
        elif arguments.command == "preflight":
            source = _load_json(arguments.source)
            owned_files = _load_json(arguments.owned_files)
            _require(isinstance(source, dict) and isinstance(owned_files, list),
                     "capture_json_shape_invalid")
            atomic_write_json(
                arguments.output,
                capture_preflight_attestation(source, owned_files, arguments.driver),
            )
        else:
            attestation = load_attestation(arguments.attestation)
            draft = load_evidence(arguments.draft)
            atomic_write_json(arguments.output, finalize_lifecycle(attestation, draft))
    except (AttestationError, CollectorError, EvidenceError, LifecycleHashError) as error:
        print(f"portage_lifecycle_collector: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
