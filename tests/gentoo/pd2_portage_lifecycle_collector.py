#!/usr/bin/env python3
"""Collect bounded PD2 lifecycle facts without invoking Portage.

The collector directly verifies source, executes bounded CTest and linkage
observers, and inspects installed files. It has no install, unmerge, or package
resolution path.
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
import subprocess
import sys
import tempfile
from typing import Any

from pd2_installed_artifact_attestation import (
    INSTALLED_EXECUTABLE_PATHS,
    Pd2AttestationError,
    load_attestation,
    validate_attestation_document,
    PACKAGE_ATOM,
    PACKAGE_VERSIONED_ATOM,
    SOURCE_URI,
)
from pd2_harness_manifest import capture_harness_manifest
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
from pd2_portage_lifecycle_evidence import (
    EXCLUDED_TESTS,
    EXCLUSION_REASONS,
    MAX_EVIDENCE_BYTES,
    Pd2EvidenceError,
    USER_DATA_KINDS,
    load_evidence,
    validate_evidence_document,
)


MAX_CAPTURE_FILE_BYTES = 256 * 1024 * 1024
MAX_CAPTURE_JSON_BYTES = 524_288
MAX_COMMAND_OUTPUT_BYTES = 4 * 1024 * 1024
EBUILD_RELATIVE_PATH = Path(
    "packaging/gentoo/repository/x11-misc/prismdrake/prismdrake-9999.ebuild"
)
BUILD_PROVENANCE_PATH = Path("/usr/share/prismdrake/build-provenance.json")
EXCLUSION_REGEX = "^(?:" + "|".join(re.escape(name) for name in EXCLUDED_TESTS) + ")$"


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


def run_checked(command: list[str], cwd: Path | None = None) -> str:
    """Run one bounded, shell-free observer command."""

    try:
        completed = subprocess.run(
            command,
            cwd=cwd,
            check=False,
            capture_output=True,
            text=False,
            timeout=120,
            env={"HOME": "/nonexistent", "LANG": "C", "LC_ALL": "C", "PATH": "/usr/bin:/bin"},
        )
    except (OSError, subprocess.SubprocessError) as error:
        raise Pd2CollectorError("observer_command_failed") from error
    _require(len(completed.stdout) <= MAX_COMMAND_OUTPUT_BYTES and
             len(completed.stderr) <= MAX_COMMAND_OUTPUT_BYTES,
             "observer_output_oversized")
    _require(completed.returncode == 0, "observer_command_failed")
    try:
        return completed.stdout.decode("utf-8")
    except UnicodeError as error:
        raise Pd2CollectorError("observer_output_invalid") from error


def verify_exact_source(
        source_checkout: Path, source_bundle: Path,
        hash_file: Callable[[Path], str] = sha256_regular_file,
        command_runner: Callable[[list[str], Path | None], str] = run_checked,
) -> dict[str, Any]:
    """Verify one clean checkout and one bundle whose HEAD is the same commit."""

    _require(source_checkout.is_absolute() and source_checkout.is_dir() and
             not source_checkout.is_symlink(), "source_checkout_invalid")
    _require(source_bundle.is_absolute(), "source_bundle_invalid")
    bundle_hash = _require_sha256(hash_file(source_bundle))
    revision = command_runner(
        ["/usr/bin/git", "rev-parse", "--verify", "HEAD"], source_checkout,
    ).strip()
    _require(re.fullmatch(r"[0-9a-f]{40}", revision) is not None,
             "source_revision_invalid")
    status = command_runner(
        ["/usr/bin/git", "status", "--porcelain=v1", "--untracked-files=all"],
        source_checkout,
    )
    _require(status == "", "source_checkout_not_clean")
    command_runner(
        ["/usr/bin/git", "bundle", "verify", str(source_bundle)], source_checkout,
    )
    heads = command_runner(
        ["/usr/bin/git", "bundle", "list-heads", str(source_bundle)], source_checkout,
    ).splitlines()
    _require(heads == [f"{revision} HEAD"], "source_bundle_head_mismatch")
    ebuild = source_checkout / EBUILD_RELATIVE_PATH
    ebuild_hash = _require_sha256(hash_file(ebuild))
    return {
        "revision": revision,
        "repository_uri": SOURCE_URI,
        "repository_name": "prismdrake-local",
        "atom": PACKAGE_ATOM,
        "version": "9999",
        "ebuild_path": "x11-misc/prismdrake/prismdrake-9999.ebuild",
        "ebuild_sha256": ebuild_hash,
        "bundle_sha256": bundle_hash,
    }


def load_build_provenance(path: Path, revision: str) -> dict[str, Any]:
    """Read the installed, package-owned build provenance file."""

    _require(path == BUILD_PROVENANCE_PATH, "build_provenance_path_invalid")
    provenance = load_capture_json(path)
    _require(provenance == {"schema_version": 1, "source_revision": revision},
             "build_provenance_mismatch")
    return provenance


def _binding(attestation: dict[str, Any]) -> dict[str, str]:
    validate_attestation_document(attestation)
    return {
        "source_revision": attestation["source"]["revision"],
        "ebuild_sha256": attestation["source"]["ebuild_sha256"],
        "artifact_sha256": attestation["tested_artifact"]["artifact_sha256"],
    }


def _parse_command_json(payload: str) -> Any:
    _require(0 < len(payload.encode("utf-8")) <= MAX_COMMAND_OUTPUT_BYTES,
             "observer_output_invalid")
    try:
        return json.loads(
            payload,
            object_pairs_hook=_reject_duplicate_keys,
            parse_constant=_reject_constant,
            parse_float=_parse_finite_float,
        )
    except (ValueError, json.JSONDecodeError) as error:
        raise Pd2CollectorError("observer_output_invalid") from error


def verify_test_build(
        build_directory: Path, revision: str,
        hash_file: Callable[[Path], str] = sha256_regular_file,
) -> None:
    """Bind a retained CTest tree to its configured source revision."""

    _require(build_directory.is_absolute() and build_directory.is_dir() and
             not build_directory.is_symlink(), "test_build_invalid")
    cache = build_directory / "CMakeCache.txt"
    _require_sha256(hash_file(cache))
    try:
        lines = cache.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as error:
        raise Pd2CollectorError("test_build_cache_invalid") from error
    matches = [line.partition("=")[2] for line in lines if line.startswith(
        "PRISMDRAKE_CONFIGURED_SOURCE_REVISION:INTERNAL=")]
    _require(matches == [revision], "test_build_revision_mismatch")


def _ctest_names(
        ctest: Path, build_directory: Path, command_runner: Callable[[list[str], Path | None], str],
        exclude: bool,
) -> tuple[str, ...]:
    command = [str(ctest), "--test-dir", str(build_directory), "--show-only=json-v1"]
    if exclude:
        command.extend(("--exclude-regex", EXCLUSION_REGEX))
    document = _parse_command_json(command_runner(command, None))
    _require(isinstance(document, dict) and isinstance(document.get("tests"), list),
             "ctest_inventory_invalid")
    names = tuple(item.get("name") for item in document["tests"] if isinstance(item, dict))
    _require(len(names) == len(document["tests"]) and
             all(isinstance(name, str) for name in names), "ctest_inventory_invalid")
    return names


def capture_package_test(
        attestation: dict[str, Any], build_directory: Path, controlled_fixture: Path,
        ctest: Path = Path("/usr/bin/ctest"),
        hash_file: Callable[[Path], str] = sha256_regular_file,
        command_runner: Callable[[list[str], Path | None], str] = run_checked,
) -> dict[str, Any]:
    """Inventory and execute the exact package-test CTest selection."""

    binding = _binding(attestation)
    verify_test_build(build_directory, binding["source_revision"], hash_file)
    _require(hash_file(controlled_fixture) ==
             attestation["demo_harness"]["controlled_fixture_sha256"],
             "controlled_fixture_hash_mismatch")
    all_names = _ctest_names(ctest, build_directory, command_runner, False)
    included_names = _ctest_names(ctest, build_directory, command_runner, True)
    excluded = tuple(name for name in all_names if name not in set(included_names))
    _require(excluded == EXCLUDED_TESTS, "package_test_exclusions_mismatch")
    _require(len(included_names) > 0, "package_test_empty")
    command_runner([
        str(ctest), "--test-dir", str(build_directory), "--output-on-failure",
        "--parallel", "1", "--no-tests=error", "--exclude-regex", EXCLUSION_REGEX,
    ], None)
    return {
        **binding,
        "features_test": True,
        "result": "passed",
        "executed_test_count": len(included_names),
        "excluded_tests": list(EXCLUDED_TESTS),
    }


def capture_sandbox_exclusions(
        attestation: dict[str, Any], build_directory: Path,
        ctest: Path = Path("/usr/bin/ctest"),
        hash_file: Callable[[Path], str] = sha256_regular_file,
        command_runner: Callable[[list[str], Path | None], str] = run_checked,
) -> list[dict[str, Any]]:
    """Execute the exact four excluded tests with their required repetition."""

    binding = _binding(attestation)
    verify_test_build(build_directory, binding["source_revision"], hash_file)
    all_names = set(_ctest_names(ctest, build_directory, command_runner, False))
    _require(set(EXCLUDED_TESTS) <= all_names, "sandbox_exclusion_test_missing")
    records = []
    for index, (test_id, reason) in enumerate(zip(EXCLUDED_TESTS, EXCLUSION_REASONS)):
        attempts = 1 if index < 2 else 5
        for _ in range(attempts):
            command_runner([
                str(ctest), "--test-dir", str(build_directory), "--output-on-failure",
                "--no-tests=error", "--tests-regex", f"^{re.escape(test_id)}$",
            ], None)
        records.append({
            "test_id": test_id,
            "reason": reason,
            **binding,
            "execution_context": "outside_portage_sandbox",
            "result": "passed",
            "attempts": attempts,
            "passes": attempts,
        })
    return records


def capture_direct_linkage(
        attestation: dict[str, Any], scanelf: Path = Path("/usr/bin/scanelf"),
        hash_file: Callable[[Path], str] = sha256_regular_file,
        command_runner: Callable[[list[str], Path | None], str] = run_checked,
) -> dict[str, Any]:
    """Run scanelf over the attested five executable byte identities."""

    binding = _binding(attestation)
    expected = {item["path"]: item["sha256"]
                for item in attestation["installed"]["executables"]}
    paths = [Path(path) for path in INSTALLED_EXECUTABLE_PATHS]
    _require(all(hash_file(path) == expected[str(path)] for path in paths),
             "linkage_executable_hash_mismatch")
    output = command_runner(
        [str(scanelf), "-BF", "%F;%n", *(str(path) for path in paths)], None,
    )
    parsed: dict[str, list[str]] = {}
    for line in output.splitlines():
        path, separator, needed = line.partition(";")
        _require(bool(separator) and path in expected and path not in parsed,
                 "scanelf_output_invalid")
        libraries = sorted(item for item in needed.split(",") if item)
        _require(bool(libraries), "scanelf_output_invalid")
        parsed[path] = libraries
    _require(tuple(parsed) == INSTALLED_EXECUTABLE_PATHS, "scanelf_output_invalid")
    entries = [{"path": path, "executable_sha256": expected[path], "needed": parsed[path]}
               for path in INSTALLED_EXECUTABLE_PATHS]
    return {
        **binding,
        "tool": "scanelf",
        "capture_sha256": canonical_linkage_sha256(entries),
        "entries": entries,
    }


def capture_ordinary_reinstall(
        attestation: dict[str, Any], owned_paths: list[dict[str, str]],
        hash_file: Callable[[Path], str] = sha256_regular_file,
        is_executable: Callable[[Path], bool] = _default_is_executable,
        provenance_loader: Callable[[Path, str], dict[str, Any]] = load_build_provenance,
) -> dict[str, Any]:
    """Recapture exact executable, ownership, and provenance bytes after reinstall."""

    binding = _binding(attestation)
    ownership_records = normalize_ownership_records(owned_paths)
    regular_records = regular_ownership_records(ownership_records)
    ownership = {
        "tool": "equery",
        "owner_atom": PACKAGE_VERSIONED_ATOM,
        "path_count": len(ownership_records),
        "regular_path_count": len(regular_records),
        "path_set_sha256": canonical_ownership_sha256(ownership_records),
        "regular_path_set_sha256": canonical_regular_ownership_sha256(ownership_records),
        "paths": ownership_records,
    }
    paths = tuple(Path(path) for path in INSTALLED_EXECUTABLE_PATHS)
    _require(all(is_executable(path) for path in paths), "installed_executable_invalid")
    executables = [{"path": str(path), "sha256": hash_file(path),
                    "regular_executable": True} for path in paths]
    provenance_loader(BUILD_PROVENANCE_PATH, binding["source_revision"])
    _require(hash_file(BUILD_PROVENANCE_PATH) ==
             attestation["installed"]["build_provenance"]["sha256"],
             "build_provenance_hash_mismatch")
    _require(executables == attestation["installed"]["executables"],
             "ordinary_reinstall_executable_mismatch")
    _require(ownership == attestation["installed"]["ownership"],
             "ordinary_reinstall_ownership_mismatch")
    return {
        **binding,
        "provenance": "portage_installed",
        "use_test": False,
        "result": "passed",
        "executables": executables,
        "ownership": ownership,
    }


def capture_installed_demonstration(
        attestation: dict[str, Any], phase: str, evidence_path: Path,
        hash_file: Callable[[Path], str] = sha256_regular_file,
) -> dict[str, Any]:
    """Validate and normalize one demo output against attested executable bytes."""

    _require(phase in ("initial_install", "ordinary_reinstall"),
             "demonstration_phase_invalid")
    binding = _binding(attestation)
    document = load_capture_json(evidence_path)
    integration_path = str(Path(__file__).resolve().parents[1] / "integration")
    if integration_path not in sys.path:
        sys.path.insert(0, integration_path)
    from pd2_demo import validate_evidence as validate_demo  # pylint: disable=import-outside-toplevel

    try:
        validate_demo(document)
    except RuntimeError as error:
        raise Pd2CollectorError("demonstration_evidence_invalid") from error
    _require(document["source_revision"] == binding["source_revision"],
             "demonstration_source_revision_mismatch")
    _require(document["harness_manifest_sha256"] ==
             attestation["demo_harness"]["manifest_sha256"],
             "demonstration_harness_manifest_mismatch")
    expected = {
        Path(item["path"]).name: item["sha256"]
        for item in attestation["installed"]["executables"]
    }
    observed = {item["name"]: item["sha256"]
                for item in document["installed_executables"]}
    _require(observed == expected, "demonstration_executable_hash_mismatch")
    return {
        "phase": phase,
        **binding,
        "result": "passed",
        "complete": True,
        "evidence_kind": "pd2_installed_multi_output_failure_demonstration",
        "evidence_schema_version": 1,
        "evidence_sha256": hash_file(evidence_path),
        "normalized_results_sha256": document["normalized_results_sha256"],
        "harness_manifest_sha256": attestation["demo_harness"]["manifest_sha256"],
    }


def capture_preflight_attestation(
        source_checkout: Path, source_bundle: Path, owned_paths: list[dict[str, str]],
        controlled_fixture: Path, build_provenance: Path = BUILD_PROVENANCE_PATH,
        hash_file: Callable[[Path], str] = sha256_regular_file,
        is_executable: Callable[[Path], bool] = _default_is_executable,
        command_runner: Callable[[list[str], Path | None], str] = run_checked,
        provenance_loader: Callable[[Path, str], dict[str, Any]] = load_build_provenance,
) -> dict[str, Any]:
    """Verify source and inspect the complete installed and harness boundary."""

    source = verify_exact_source(
        source_checkout, source_bundle, hash_file=hash_file, command_runner=command_runner,
    )
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
    provenance_loader(build_provenance, source["revision"])
    build_provenance_record = {
        "path": str(build_provenance),
        "sha256": _require_sha256(hash_file(build_provenance)),
        "source_revision": source["revision"],
    }
    harness = capture_harness_manifest(source_checkout, controlled_fixture, hash_file)
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
            "build_provenance": build_provenance_record,
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
        "demo_harness": harness,
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
    """Prove that every formerly owned file and symlink is absent after unmerge."""

    try:
        removable_records = removable_ownership_records(owned_paths)
        removable_hash = canonical_removable_ownership_sha256(owned_paths)
    except Pd2LifecycleHashError as error:
        raise Pd2CollectorError(str(error)) from error
    _require(not any(path_exists(Path(record["path"])) for record in removable_records),
             "owned_removable_path_remains")
    return {
        "owned_removable_paths_absent": True,
        "absent_path_count": len(removable_records),
        "removable_path_set_sha256": removable_hash,
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
    _require(all(item.get("harness_manifest_sha256") ==
                 attestation["demo_harness"]["manifest_sha256"]
                 for item in demonstrations),
             "final_demo_harness_mismatch")
    try:
        validate_evidence_document(draft)
    except Pd2EvidenceError as error:
        raise Pd2CollectorError(str(error)) from error
    return draft


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    preflight = subparsers.add_parser("preflight")
    preflight.add_argument("--source-checkout", type=Path, required=True)
    preflight.add_argument("--source-bundle", type=Path, required=True)
    preflight.add_argument("--owned-paths", type=Path, required=True)
    preflight.add_argument("--controlled-window-fixture", type=Path, required=True)
    preflight.add_argument("--build-provenance", type=Path,
                           default=BUILD_PROVENANCE_PATH)
    preflight.add_argument("--output", type=Path, required=True)
    preservation = subparsers.add_parser("preservation")
    for phase in ("before", "after"):
        for kind in USER_DATA_KINDS:
            preservation.add_argument(f"--{phase}-{kind.replace('_', '-')}",
                                      type=Path, required=True)
    preservation.add_argument("--output", type=Path, required=True)
    package_test = subparsers.add_parser("package-test")
    package_test.add_argument("--attestation", type=Path, required=True)
    package_test.add_argument("--test-build", type=Path, required=True)
    package_test.add_argument("--controlled-window-fixture", type=Path, required=True)
    package_test.add_argument("--ctest", type=Path, default=Path("/usr/bin/ctest"))
    package_test.add_argument("--output", type=Path, required=True)
    sandbox_tests = subparsers.add_parser("sandbox-tests")
    sandbox_tests.add_argument("--attestation", type=Path, required=True)
    sandbox_tests.add_argument("--test-build", type=Path, required=True)
    sandbox_tests.add_argument("--ctest", type=Path, default=Path("/usr/bin/ctest"))
    sandbox_tests.add_argument("--output", type=Path, required=True)
    linkage = subparsers.add_parser("linkage")
    linkage.add_argument("--attestation", type=Path, required=True)
    linkage.add_argument("--scanelf", type=Path, default=Path("/usr/bin/scanelf"))
    linkage.add_argument("--output", type=Path, required=True)
    reinstall = subparsers.add_parser("reinstall")
    reinstall.add_argument("--attestation", type=Path, required=True)
    reinstall.add_argument("--owned-paths", type=Path, required=True)
    reinstall.add_argument("--output", type=Path, required=True)
    demonstration = subparsers.add_parser("demonstration")
    demonstration.add_argument("--attestation", type=Path, required=True)
    demonstration.add_argument("--phase", choices=("initial_install", "ordinary_reinstall"),
                               required=True)
    demonstration.add_argument("--evidence", type=Path, required=True)
    demonstration.add_argument("--output", type=Path, required=True)
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
            owned_paths = load_capture_json(arguments.owned_paths)
            _require(isinstance(owned_paths, list), "capture_json_shape_invalid")
            document = capture_preflight_attestation(
                arguments.source_checkout, arguments.source_bundle, owned_paths,
                arguments.controlled_window_fixture, arguments.build_provenance,
            )
        elif arguments.command == "preservation":
            before = {kind: getattr(arguments, f"before_{kind}") for kind in USER_DATA_KINDS}
            after = {kind: getattr(arguments, f"after_{kind}") for kind in USER_DATA_KINDS}
            document = {"user_data": capture_user_state_preservation(before, after)}
        elif arguments.command == "package-test":
            attestation = load_attestation(arguments.attestation)
            document = capture_package_test(
                attestation, arguments.test_build, arguments.controlled_window_fixture,
                arguments.ctest,
            )
        elif arguments.command == "sandbox-tests":
            attestation = load_attestation(arguments.attestation)
            document = {"sandbox_exclusions": capture_sandbox_exclusions(
                attestation, arguments.test_build, arguments.ctest,
            )}
        elif arguments.command == "linkage":
            attestation = load_attestation(arguments.attestation)
            document = capture_direct_linkage(attestation, arguments.scanelf)
        elif arguments.command == "reinstall":
            attestation = load_attestation(arguments.attestation)
            owned_paths = load_capture_json(arguments.owned_paths)
            _require(isinstance(owned_paths, list), "capture_json_shape_invalid")
            document = capture_ordinary_reinstall(attestation, owned_paths)
        elif arguments.command == "demonstration":
            attestation = load_attestation(arguments.attestation)
            document = capture_installed_demonstration(
                attestation, arguments.phase, arguments.evidence,
            )
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
