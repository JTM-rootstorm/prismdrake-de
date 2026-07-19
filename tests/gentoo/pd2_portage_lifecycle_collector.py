#!/usr/bin/env python3
"""Collect bounded PD2 lifecycle facts with explicit Portage observer stages.

The collector directly verifies source, executes bounded QA, resolution, CTest,
and linkage observers, and inspects installed files. It never installs or
unmerges a package.
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
import selectors
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
from typing import Any
import xml.etree.ElementTree as ElementTree

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
    validate_evidence_document,
)


MAX_CAPTURE_FILE_BYTES = 256 * 1024 * 1024
MAX_CAPTURE_JSON_BYTES = 524_288
MAX_COMMAND_OUTPUT_BYTES = 4 * 1024 * 1024
MAX_CTEST_JUNIT_BYTES = 16 * 1024 * 1024
SOURCE_COMMAND_TIMEOUT_SECONDS = 30
OBSERVER_COMMAND_TIMEOUT_SECONDS = 120
CTEST_COMMAND_TIMEOUT_SECONDS = 1800
PORTAGE_COMMAND_TIMEOUT_SECONDS = 1800
CTEST_PATH = Path("/usr/bin/ctest")
SCANELF_PATH = Path("/usr/bin/scanelf")
EQUERY_PATH = Path("/usr/bin/equery")
PKGDEV_PATH = Path("/usr/bin/pkgdev")
PKGCHECK_PATH = Path("/usr/bin/pkgcheck")
PKGCHECK_EXIT_POLICY = "error,warning,style,info"
PORTAGEQ_PATH = Path("/usr/bin/portageq")
ENV_PATH = Path("/usr/bin/env")
EMERGE_PATH = Path("/usr/bin/emerge")
VDB_DIRECTORY = Path("/var/db/pkg/x11-misc/prismdrake-9999")
EBUILD_RELATIVE_PATH = Path(
    "packaging/gentoo/repository/x11-misc/prismdrake/prismdrake-9999.ebuild"
)
CTEST_INVENTORY_RELATIVE_PATH = Path("tests/gentoo/pd2-ctest-inventory-v1.json")
BUILD_PROVENANCE_PATH = Path("/usr/share/prismdrake/build-provenance.json")
EXCLUSION_REGEX = "^(?:" + "|".join(re.escape(name) for name in EXCLUDED_TESTS) + ")$"
OPTIONAL_PACKAGE_SKIP_TEST = "AtomicFileTest.RejectsDestinationOwnedByAnotherUser"
OPTIONAL_PACKAGE_SKIP_REASON = "requires_root_ownership_transition"


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


def _command_timeout(command: list[str]) -> int:
    executable = Path(command[0]) if command else Path()
    if executable == CTEST_PATH:
        return CTEST_COMMAND_TIMEOUT_SECONDS
    if executable.name == "git":
        return SOURCE_COMMAND_TIMEOUT_SECONDS
    if executable in (PKGDEV_PATH, PKGCHECK_PATH, PORTAGEQ_PATH, EMERGE_PATH,
                      ENV_PATH):
        return PORTAGE_COMMAND_TIMEOUT_SECONDS
    return OBSERVER_COMMAND_TIMEOUT_SECONDS


def _terminate_process_group(process: subprocess.Popen[bytes]) -> None:
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()


def run_checked(command: list[str], cwd: Path | None = None) -> str:
    """Run one bounded, shell-free observer command."""

    _require(bool(command) and Path(command[0]).is_absolute(), "observer_command_invalid")
    process: subprocess.Popen[bytes] | None = None
    selector = selectors.DefaultSelector()
    streams: dict[int, bytearray] = {}
    stdout_data = b""
    try:
        process = subprocess.Popen(
            command,
            cwd=cwd,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env={"HOME": "/nonexistent", "LANG": "C", "LC_ALL": "C", "PATH": "/usr/bin:/bin"},
            start_new_session=True,
        )
        _require(process.stdout is not None and process.stderr is not None,
                 "observer_command_failed")
        for stream in (process.stdout, process.stderr):
            os.set_blocking(stream.fileno(), False)
            selector.register(stream, selectors.EVENT_READ)
            streams[stream.fileno()] = bytearray()
        deadline = time.monotonic() + _command_timeout(command)
        while selector.get_map():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                _terminate_process_group(process)
                raise Pd2CollectorError("observer_command_timeout")
            for key, _ in selector.select(min(remaining, 0.25)):
                block = os.read(key.fd, 64 * 1024)
                if not block:
                    selector.unregister(key.fileobj)
                    continue
                target = streams[key.fd]
                target.extend(block)
                if len(target) > MAX_COMMAND_OUTPUT_BYTES:
                    _terminate_process_group(process)
                    raise Pd2CollectorError("observer_output_oversized")
        return_code = process.wait(timeout=1)
        stdout_data = bytes(streams[process.stdout.fileno()])
    except Pd2CollectorError:
        if process is not None and process.poll() is None:
            _terminate_process_group(process)
        raise
    except (OSError, subprocess.SubprocessError) as error:
        if process is not None and process.poll() is None:
            _terminate_process_group(process)
        raise Pd2CollectorError("observer_command_failed") from error
    finally:
        selector.close()
        if process is not None:
            if process.stdout is not None:
                process.stdout.close()
            if process.stderr is not None:
                process.stderr.close()
    _require(return_code == 0, "observer_command_failed")
    try:
        return stdout_data.decode("utf-8")
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


def _verify_clean_source_binding(
        source_checkout: Path, source: dict[str, Any],
        hash_file: Callable[[Path], str],
        command_runner: Callable[[list[str], Path | None], str],
) -> None:
    _require(source_checkout.is_absolute() and source_checkout.is_dir() and
             not source_checkout.is_symlink(), "source_checkout_invalid")
    revision = command_runner(
        ["/usr/bin/git", "rev-parse", "--verify", "HEAD"], source_checkout,
    ).strip()
    _require(revision == source["revision"], "source_revision_mismatch")
    _require(command_runner(
        ["/usr/bin/git", "status", "--porcelain=v1", "--untracked-files=all"],
        source_checkout,
    ) == "", "source_checkout_not_clean")
    _require(hash_file(source_checkout / EBUILD_RELATIVE_PATH) ==
             source["ebuild_sha256"], "source_ebuild_mismatch")


def _repository_tree_sha256(repository: Path) -> str:
    records: list[dict[str, str]] = []
    total_bytes = 0
    try:
        paths = sorted(repository.rglob("*"))
    except OSError as error:
        raise Pd2CollectorError("qa_repository_invalid") from error
    _require(len(paths) <= 1000, "qa_repository_invalid")
    for path in paths:
        _require(not path.is_symlink(), "qa_repository_invalid")
        if path.is_dir():
            continue
        _require(path.is_file(), "qa_repository_invalid")
        try:
            payload = path.read_bytes()
            relative = path.relative_to(repository).as_posix()
        except OSError as error:
            raise Pd2CollectorError("qa_repository_invalid") from error
        total_bytes += len(payload)
        _require(total_bytes <= MAX_CAPTURE_FILE_BYTES, "qa_repository_invalid")
        records.append({
            "path": relative,
            "sha256": hashlib.sha256(payload).hexdigest(),
        })
    _require(records, "qa_repository_invalid")
    return hashlib.sha256(json.dumps(
        records, ensure_ascii=True, separators=(",", ":"), sort_keys=True,
    ).encode("utf-8")).hexdigest()


def capture_portage_qa(
        attestation: dict[str, Any], source_checkout: Path,
        pkgdev: Path = PKGDEV_PATH, pkgcheck: Path = PKGCHECK_PATH,
        hash_file: Callable[[Path], str] = sha256_regular_file,
        command_runner: Callable[[list[str], Path | None], str] = run_checked,
) -> dict[str, Any]:
    """Run manifest generation and pkgcheck against a disposable repository copy."""

    binding = _binding(attestation)
    source = attestation["source"]
    _verify_clean_source_binding(source_checkout, source, hash_file, command_runner)
    repository = source_checkout / "packaging/gentoo/repository"
    _require(repository.is_dir() and not repository.is_symlink(),
             "qa_repository_invalid")
    source_tree_hash = _repository_tree_sha256(repository)
    with tempfile.TemporaryDirectory(prefix="prismdrake-pd2-portage-qa-") as temporary:
        temporary_root = Path(temporary)
        disposable_repository = temporary_root / "repository"
        cache = temporary_root / "pkgcheck-cache"
        try:
            shutil.copytree(repository, disposable_repository)
            cache.mkdir()
        except OSError as error:
            raise Pd2CollectorError("qa_repository_copy_failed") from error
        pkgdev_output = command_runner(
            [str(pkgdev), "manifest"], disposable_repository,
        )
        pkgcheck_output = command_runner([
            str(pkgcheck), "scan", "--exit", PKGCHECK_EXIT_POLICY,
            "--cache-dir", str(cache),
            str(disposable_repository),
        ], None)
        tree_hash = _repository_tree_sha256(disposable_repository)
        _require(hash_file(disposable_repository / EBUILD_RELATIVE_PATH.relative_to(
            "packaging/gentoo/repository")) == source["ebuild_sha256"],
            "qa_ebuild_mismatch")
    return {
        **binding,
        "result": "passed",
        "source_repository_tree_sha256": source_tree_hash,
        "repository_tree_sha256": tree_hash,
        "pkgdev_output_sha256": hashlib.sha256(pkgdev_output.encode("utf-8")).hexdigest(),
        "pkgcheck_output_sha256": hashlib.sha256(
            pkgcheck_output.encode("utf-8")
        ).hexdigest(),
    }


def capture_portage_resolutions(
        attestation: dict[str, Any], source_checkout: Path,
        portageq: Path = PORTAGEQ_PATH, emerge: Path = EMERGE_PATH,
        hash_file: Callable[[Path], str] = sha256_regular_file,
        command_runner: Callable[[list[str], Path | None], str] = run_checked,
) -> list[dict[str, Any]]:
    """Execute both exact package pretend resolutions against the selected overlay."""

    source = attestation["source"]
    _verify_clean_source_binding(source_checkout, source, hash_file, command_runner)
    repository_output = command_runner(
        [str(portageq), "get_repo_path", "/", source["repository_name"]], None,
    )
    repository_lines = repository_output.splitlines()
    _require(len(repository_lines) == 1, "portage_repository_path_invalid")
    repository = Path(repository_lines[0])
    _require(repository.is_absolute() and repository.is_dir() and
             not repository.is_symlink(), "portage_repository_path_invalid")
    installed_ebuild = repository / source["ebuild_path"]
    _require(hash_file(installed_ebuild) == source["ebuild_sha256"],
             "portage_repository_ebuild_mismatch")
    records = []
    for mode, use_value, use_test in (
            ("default", "USE=-test", False), ("use_test", "USE=test", True)):
        output = command_runner([
            str(ENV_PATH), use_value, str(emerge), "--pretend", "--verbose", "--tree",
            "--newuse", f"={PACKAGE_VERSIONED_ATOM}::{source['repository_name']}",
        ], None)
        _require(PACKAGE_ATOM in output, "portage_resolution_output_invalid")
        records.append({
            "mode": mode,
            "source_revision": source["revision"],
            "ebuild_sha256": source["ebuild_sha256"],
            "use_test": use_test,
            "result": "resolved",
            "dependency_graph_sha256": hashlib.sha256(output.encode("utf-8")).hexdigest(),
        })
    _require(records[0]["dependency_graph_sha256"] !=
             records[1]["dependency_graph_sha256"],
             "resolution_dependency_graphs_not_distinct")
    return records


def load_build_provenance(path: Path, revision: str) -> dict[str, Any]:
    """Read the installed, package-owned build provenance file."""

    _require(path == BUILD_PROVENANCE_PATH, "build_provenance_path_invalid")
    provenance = load_capture_json(path)
    _require(provenance == {"schema_version": 1, "source_revision": revision},
             "build_provenance_mismatch")
    return provenance


def _read_bounded_regular(path: Path, maximum: int = MAX_CAPTURE_JSON_BYTES) -> bytes:
    _require(path.is_absolute() and not path.is_symlink(), "vdb_file_invalid")
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise Pd2CollectorError("vdb_file_unavailable") from error
    try:
        metadata = os.fstat(descriptor)
        _require(stat.S_ISREG(metadata.st_mode) and 0 < metadata.st_size <= maximum,
                 "vdb_file_invalid")
        chunks = bytearray()
        while len(chunks) <= metadata.st_size:
            block = os.read(descriptor, min(64 * 1024, metadata.st_size + 1 - len(chunks)))
            if not block:
                break
            chunks.extend(block)
        after = os.fstat(descriptor)
        _require(len(chunks) == metadata.st_size and after.st_size == metadata.st_size and
                 after.st_mtime_ns == metadata.st_mtime_ns,
                 "vdb_file_changed")
        return bytes(chunks)
    finally:
        os.close(descriptor)


def _parse_vdb_contents(payload: bytes) -> list[dict[str, str]]:
    try:
        lines = payload.decode("utf-8").splitlines()
    except UnicodeError as error:
        raise Pd2CollectorError("vdb_contents_invalid") from error
    records = []
    for line in lines:
        kind, separator, remainder = line.partition(" ")
        _require(bool(separator), "vdb_contents_invalid")
        if kind == "obj":
            parts = remainder.rsplit(" ", 2)
            _require(len(parts) == 3, "vdb_contents_invalid")
            path = parts[0]
            normalized_kind = "regular"
        elif kind == "sym":
            path, separator, _target = remainder.partition(" -> ")
            _require(bool(separator), "vdb_contents_invalid")
            normalized_kind = "symlink"
        elif kind == "dir":
            path = remainder
            normalized_kind = "directory"
        else:
            raise Pd2CollectorError("vdb_contents_invalid")
        records.append({"kind": normalized_kind, "path": path})
    try:
        return normalize_ownership_records(records)
    except Pd2LifecycleHashError as error:
        raise Pd2CollectorError(str(error)) from error


def _parse_equery_paths(output: str) -> tuple[str, ...]:
    lines = tuple(line.strip() for line in output.splitlines() if line.strip())
    _require(lines and all(line == "/usr" or line.startswith("/usr/") for line in lines),
             "equery_output_invalid")
    _require(len(set(lines)) == len(lines), "equery_output_invalid")
    return tuple(sorted(lines))


def _installed_kind_matches(record: dict[str, str]) -> bool:
    path = Path(record["path"])
    try:
        metadata = path.lstat()
    except OSError:
        return False
    return ((record["kind"] == "regular" and stat.S_ISREG(metadata.st_mode)) or
            (record["kind"] == "directory" and stat.S_ISDIR(metadata.st_mode)) or
            (record["kind"] == "symlink" and stat.S_ISLNK(metadata.st_mode)))


def capture_portage_install_metadata(
        source: dict[str, Any], use_test: bool, vdb_directory: Path = VDB_DIRECTORY,
        hash_file: Callable[[Path], str] = sha256_regular_file,
        command_runner: Callable[[list[str], Path | None], str] = run_checked,
        kind_matches: Callable[[dict[str, str]], bool] = _installed_kind_matches,
) -> tuple[dict[str, Any], list[dict[str, str]]]:
    """Cross-check VDB, equery, source ebuild, USE state, and installed paths."""

    _require(vdb_directory.is_absolute() and vdb_directory.is_dir() and
             not vdb_directory.is_symlink(), "vdb_directory_invalid")
    ebuild = vdb_directory / "prismdrake-9999.ebuild"
    use_file = vdb_directory / "USE"
    repository_file = vdb_directory / "repository"
    contents_file = vdb_directory / "CONTENTS"
    _require(_require_sha256(hash_file(ebuild)) == source["ebuild_sha256"],
             "vdb_ebuild_mismatch")
    try:
        repository = _read_bounded_regular(repository_file).decode("utf-8").strip()
    except UnicodeError as error:
        raise Pd2CollectorError("vdb_repository_invalid") from error
    _require(repository == source["repository_name"], "vdb_repository_mismatch")
    try:
        use_flags = set(_read_bounded_regular(use_file).decode("utf-8").split())
    except UnicodeError as error:
        raise Pd2CollectorError("vdb_use_invalid") from error
    _require(("test" in use_flags) is use_test, "vdb_use_test_mismatch")
    contents_payload = _read_bounded_regular(contents_file, MAX_CAPTURE_FILE_BYTES)
    ownership_records = _parse_vdb_contents(contents_payload)
    equery_paths = _parse_equery_paths(command_runner(
        [str(EQUERY_PATH), "-C", "-q", "files", f"={PACKAGE_VERSIONED_ATOM}"], None,
    ))
    _require(equery_paths == tuple(record["path"] for record in ownership_records),
             "equery_vdb_ownership_mismatch")
    _require(all(kind_matches(record) for record in ownership_records),
             "installed_owned_path_invalid")
    return ({
        "atom": PACKAGE_VERSIONED_ATOM,
        "repository": repository,
        "ebuild_sha256": source["ebuild_sha256"],
        "use_test": use_test,
        "ownership_sha256": canonical_ownership_sha256(ownership_records),
    }, ownership_records)


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


def _is_within(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def _configured_test_build_revision(
        build_directory: Path, source_checkout: Path,
        hash_file: Callable[[Path], str] = sha256_regular_file,
) -> str:
    """Read one exact lifecycle revision from a source-bound CMake cache."""

    _require(build_directory.is_absolute() and build_directory.is_dir() and
             not build_directory.is_symlink(), "test_build_invalid")
    _require(source_checkout.is_absolute() and source_checkout.is_dir() and
             not source_checkout.is_symlink(), "test_source_invalid")
    cache = build_directory / "CMakeCache.txt"
    _require_sha256(hash_file(cache))
    try:
        lines = cache.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as error:
        raise Pd2CollectorError("test_build_cache_invalid") from error
    revision_matches = [line.partition("=")[2] for line in lines if line.startswith(
        "PRISMDRAKE_CONFIGURED_SOURCE_REVISION:INTERNAL=")]
    source_matches = [line.partition("=")[2] for line in lines if line.startswith(
        "CMAKE_HOME_DIRECTORY:INTERNAL=")]
    _require(source_matches == [str(source_checkout)], "test_build_source_mismatch")
    _require(len(revision_matches) == 1 and
             re.fullmatch(r"[0-9a-f]{40}", revision_matches[0]) is not None,
             "test_build_revision_invalid")
    return revision_matches[0]


def verify_test_build(
        build_directory: Path, source_checkout: Path, revision: str,
        hash_file: Callable[[Path], str] = sha256_regular_file,
        command_runner: Callable[[list[str], Path | None], str] = run_checked,
) -> None:
    """Bind a retained CTest tree to one exact clean source checkout."""

    configured_revision = _configured_test_build_revision(
        build_directory, source_checkout, hash_file,
    )
    _require(configured_revision == revision, "test_build_revision_mismatch")
    observed_revision = command_runner(
        ["/usr/bin/git", "rev-parse", "--verify", "HEAD"], source_checkout,
    ).strip()
    _require(observed_revision == revision, "test_source_revision_mismatch")
    _require(command_runner(
        ["/usr/bin/git", "status", "--porcelain=v1", "--untracked-files=all"],
        source_checkout,
    ) == "", "test_source_not_clean")


def _ctest_inventory(
        ctest: Path, build_directory: Path, command_runner: Callable[[list[str], Path | None], str],
        exclude: bool,
) -> list[dict[str, Any]]:
    command = [str(ctest), "--test-dir", str(build_directory), "--show-only=json-v1"]
    if exclude:
        command.extend(("--exclude-regex", EXCLUSION_REGEX))
    document = _parse_command_json(command_runner(command, None))
    _require(isinstance(document, dict) and isinstance(document.get("tests"), list),
             "ctest_inventory_invalid")
    tests = document["tests"]
    for item in tests:
        _require(isinstance(item, dict) and isinstance(item.get("name"), str) and
                 isinstance(item.get("command"), list) and item["command"] and
                 all(isinstance(token, str) for token in item["command"]),
                 "ctest_inventory_invalid")
        properties = item.get("properties", [])
        _require(isinstance(properties, list) and all(
            isinstance(prop, dict) and set(prop) == {"name", "value"} and
            isinstance(prop["name"], str) and prop["name"]
            for prop in properties
        ), "ctest_inventory_invalid")
        _require(len({prop["name"] for prop in properties}) == len(properties),
                 "ctest_inventory_invalid")
    _require(len({item["name"] for item in tests}) == len(tests),
             "ctest_inventory_invalid")
    return tests


def _canonical_ctest_inventory_sha256(
        inventory: list[dict[str, Any]], build_directory: Path,
        source_checkout: Path,
) -> str:
    """Hash ordered CTest commands and behavior properties independent of roots."""

    configured_revision = _configured_test_build_revision(
        build_directory, source_checkout,
    )
    try:
        build_root = str(build_directory.resolve(strict=True))
        source_root = str(source_checkout.resolve(strict=True))
    except (OSError, RuntimeError) as error:
        raise Pd2CollectorError("ctest_inventory_root_invalid") from error
    replacements = sorted(
        (
            (build_root, "${BUILD}"),
            (source_root, "${SOURCE}"),
            (configured_revision, "${REVISION}"),
        ),
        key=lambda item: len(item[0]), reverse=True,
    )

    def normalize(value: Any) -> Any:
        if isinstance(value, str):
            _require("${REVISION}" not in value,
                     "ctest_inventory_revision_placeholder_invalid")
            for root, token in replacements:
                value = value.replace(root, token)
            return value
        if isinstance(value, list):
            return [normalize(item) for item in value]
        if isinstance(value, dict):
            return {key: normalize(item) for key, item in value.items()}
        return value

    normalized = []
    for item in inventory:
        properties = sorted(
            (
                {"name": prop["name"], "value": normalize(prop["value"])}
                for prop in item.get("properties", [])
                if prop["name"] != "DEF_SOURCE_LINE"
            ),
            key=lambda prop: prop["name"],
        )
        normalized.append({
            "name": item["name"],
            "command": normalize(item["command"]),
            "properties": properties,
        })
    return hashlib.sha256(json.dumps(
        normalized, allow_nan=False, ensure_ascii=True,
        separators=(",", ":"), sort_keys=True,
    ).encode("utf-8")).hexdigest()


def _inventory_contract_sha256(
        source_checkout: Path, build_directory: Path,
        inventory: list[dict[str, Any]],
        hash_file: Callable[[Path], str],
) -> str:
    contract_path = source_checkout / CTEST_INVENTORY_RELATIVE_PATH
    contract = load_capture_json(contract_path)
    _require(isinstance(contract, dict) and set(contract) == {
        "schema_version", "inventory_kind", "exact_test_count",
        "ordered_test_names_sha256", "canonical_inventory_sha256",
        "excluded_tests",
    }, "ctest_inventory_contract_invalid")
    _require(contract["schema_version"] == 1 and
             contract["inventory_kind"] == "pd2_exact_ctest_inventory" and
             isinstance(contract["exact_test_count"], int) and
             1 <= contract["exact_test_count"] <= 10000 and
             tuple(contract["excluded_tests"]) == EXCLUDED_TESTS,
             "ctest_inventory_contract_invalid")
    expected_hash = _require_sha256(contract["ordered_test_names_sha256"])
    expected_inventory_hash = _require_sha256(contract["canonical_inventory_sha256"])
    names = [item["name"] for item in inventory]
    observed_hash = hashlib.sha256(json.dumps(
        names, ensure_ascii=True, separators=(",", ":"),
    ).encode("utf-8")).hexdigest()
    _require(len(names) == contract["exact_test_count"] and
             observed_hash == expected_hash, "ctest_inventory_contract_mismatch")
    _require(_canonical_ctest_inventory_sha256(
        inventory, build_directory, source_checkout,
    ) == expected_inventory_hash, "ctest_inventory_contract_mismatch")
    return _require_sha256(hash_file(contract_path))


def _test_manifest_sha256(
        inventory: list[dict[str, Any]], build_directory: Path, source_checkout: Path,
        controlled_fixture: Path, hash_file: Callable[[Path], str],
) -> str:
    expected_fixture = build_directory / "tests/integration/prismdrake-controlled-window-fixture"
    _require(controlled_fixture == expected_fixture and controlled_fixture.is_absolute(),
             "controlled_fixture_build_mismatch")

    def project_file(candidate: Path) -> Path | None:
        if not candidate.is_absolute():
            return None
        for root in (build_directory, source_checkout):
            try:
                relative = candidate.relative_to(root)
            except ValueError:
                continue
            if ".." in relative.parts:
                return None
            current = root
            try:
                for part in relative.parts:
                    current /= part
                    if current.is_symlink():
                        return None
                resolved_root = root.resolve(strict=True)
                resolved = candidate.resolve(strict=True)
                resolved.relative_to(resolved_root)
            except (OSError, RuntimeError, ValueError):
                return None
            if resolved.is_file():
                return resolved
        return None

    resolved_fixture = project_file(controlled_fixture)
    _require(resolved_fixture is not None, "controlled_fixture_build_mismatch")
    project_files: dict[str, str] = {}
    normalized_tests = []
    for item in inventory:
        command = item["command"]
        found_project_file = False
        for token in command:
            candidate = Path(token)
            resolved = project_file(candidate)
            if resolved is not None:
                project_files[str(resolved)] = _require_sha256(hash_file(resolved))
                found_project_file = True
        _require(found_project_file, "ctest_project_command_unbound")
        normalized_tests.append({"name": item["name"], "command": command})
    project_files[str(resolved_fixture)] = _require_sha256(hash_file(resolved_fixture))
    payload = {
        "tests": normalized_tests,
        "project_files": [
            {"path": path, "sha256": digest}
            for path, digest in sorted(project_files.items())
        ],
    }
    return hashlib.sha256(json.dumps(
        payload, ensure_ascii=True, separators=(",", ":"), sort_keys=True,
    ).encode("utf-8")).hexdigest()


def _parse_passing_junit(
        path: Path, expected_names: tuple[str, ...],
        allowed_skips: tuple[str, ...] = (),
) -> tuple[str, ...]:
    try:
        payload = path.read_bytes()
    except OSError as error:
        raise Pd2CollectorError("ctest_junit_unavailable") from error
    _require(0 < len(payload) <= MAX_CTEST_JUNIT_BYTES, "ctest_junit_size_invalid")
    try:
        root = ElementTree.fromstring(payload)
    except ElementTree.ParseError as error:
        raise Pd2CollectorError("ctest_junit_invalid") from error
    cases = list(root.iter("testcase"))
    names = tuple(case.get("name") for case in cases)
    _require(names == expected_names, "ctest_execution_set_mismatch")
    skipped: list[str] = []
    for case in cases:
        name = case.get("name")
        skipped_element = case.find("skipped")
        _require(case.find("failure") is None and case.find("error") is None,
                 "ctest_not_all_passed")
        if skipped_element is not None:
            _require(name in allowed_skips and name not in skipped,
                     "ctest_not_all_passed")
            _require(case.get("status") == "notrun", "ctest_not_all_passed")
            skipped.append(name)
        else:
            _require(case.get("status", "run") == "run", "ctest_not_all_passed")
    return tuple(skipped)


def _execute_ctest(
        ctest: Path, build_directory: Path, selection: list[str],
        expected_names: tuple[str, ...],
        command_runner: Callable[[list[str], Path | None], str],
        allowed_skips: tuple[str, ...] = (),
) -> tuple[str, ...]:
    with tempfile.TemporaryDirectory(prefix="prismdrake-pd2-ctest-") as temporary:
        junit = Path(temporary) / "results.xml"
        command_runner([
            str(ctest), "--test-dir", str(build_directory), "--output-on-failure",
            "--parallel", "1", "--no-tests=error", *selection,
            "--output-junit", str(junit),
        ], None)
        return _parse_passing_junit(junit, expected_names, allowed_skips)


def capture_package_test(
        attestation: dict[str, Any], build_directory: Path, source_checkout: Path,
        controlled_fixture: Path, ctest: Path = CTEST_PATH,
        hash_file: Callable[[Path], str] = sha256_regular_file,
        command_runner: Callable[[list[str], Path | None], str] = run_checked,
        effective_uid: Callable[[], int] = os.geteuid,
) -> dict[str, Any]:
    """Inventory and execute the exact package-test CTest selection."""

    _require(effective_uid() != 0, "package_test_requires_non_root_user")
    binding = _binding(attestation)
    verify_test_build(
        build_directory, source_checkout, binding["source_revision"], hash_file,
        command_runner,
    )
    _require(hash_file(controlled_fixture) ==
             attestation["demo_harness"]["controlled_fixture_sha256"],
             "controlled_fixture_hash_mismatch")
    all_inventory = _ctest_inventory(ctest, build_directory, command_runner, False)
    included_inventory = _ctest_inventory(ctest, build_directory, command_runner, True)
    all_names = tuple(item["name"] for item in all_inventory)
    included_names = tuple(item["name"] for item in included_inventory)
    disabled_names = tuple(
        item["name"] for item in included_inventory
        if any(prop["name"] == "DISABLED" and prop.get("value") is True
               for prop in item.get("properties", []))
    )
    _require(not disabled_names, "package_test_disabled")
    excluded = tuple(name for name in all_names if name not in set(included_names))
    _require(excluded == EXCLUDED_TESTS, "package_test_exclusions_mismatch")
    _require(len(included_names) > 0, "package_test_empty")
    inventory_contract_hash = _inventory_contract_sha256(
        source_checkout, build_directory, all_inventory, hash_file,
    )
    manifest_hash = _test_manifest_sha256(
        all_inventory, build_directory, source_checkout, controlled_fixture, hash_file,
    )
    skipped = _execute_ctest(
        ctest, build_directory, ["--exclude-regex", EXCLUSION_REGEX], included_names,
        command_runner, (OPTIONAL_PACKAGE_SKIP_TEST,),
    )
    _require(skipped == (OPTIONAL_PACKAGE_SKIP_TEST,),
             "package_test_required_skip_mismatch")
    return {
        **binding,
        "features_test": True,
        "result": "passed",
        "selected_test_count": len(included_names),
        "executed_test_count": len(included_names),
        "passed_test_count": len(included_names) - len(skipped),
        "skipped_test_count": len(skipped),
        "skipped_tests": [
            {"test_id": name, "reason": OPTIONAL_PACKAGE_SKIP_REASON}
            for name in skipped
        ],
        "disabled_test_count": 0,
        "not_run_test_count": 0,
        "test_manifest_sha256": manifest_hash,
        "inventory_contract_sha256": inventory_contract_hash,
        "excluded_tests": list(EXCLUDED_TESTS),
    }


def capture_sandbox_exclusions(
        attestation: dict[str, Any], build_directory: Path, source_checkout: Path,
        controlled_fixture: Path, ctest: Path = CTEST_PATH,
        hash_file: Callable[[Path], str] = sha256_regular_file,
        command_runner: Callable[[list[str], Path | None], str] = run_checked,
) -> list[dict[str, Any]]:
    """Execute the exact four excluded tests with their required repetition."""

    binding = _binding(attestation)
    verify_test_build(
        build_directory, source_checkout, binding["source_revision"], hash_file,
        command_runner,
    )
    all_inventory = _ctest_inventory(ctest, build_directory, command_runner, False)
    all_names = {item["name"] for item in all_inventory}
    _require(set(EXCLUDED_TESTS) <= all_names, "sandbox_exclusion_test_missing")
    inventory_contract_hash = _inventory_contract_sha256(
        source_checkout, build_directory, all_inventory, hash_file,
    )
    manifest_hash = _test_manifest_sha256(
        all_inventory, build_directory, source_checkout, controlled_fixture, hash_file,
    )
    records = []
    for index, (test_id, reason) in enumerate(zip(EXCLUDED_TESTS, EXCLUSION_REASONS)):
        attempts = 1 if index < 2 else 5
        for _ in range(attempts):
            _execute_ctest(
                ctest, build_directory,
                ["--tests-regex", f"^{re.escape(test_id)}$"], (test_id,), command_runner,
            )
        records.append({
            "test_id": test_id,
            "reason": reason,
            **binding,
            "execution_context": "outside_portage_sandbox",
            "result": "passed",
            "test_manifest_sha256": manifest_hash,
            "inventory_contract_sha256": inventory_contract_hash,
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
        attestation: dict[str, Any], vdb_directory: Path = VDB_DIRECTORY,
        hash_file: Callable[[Path], str] = sha256_regular_file,
        is_executable: Callable[[Path], bool] = _default_is_executable,
        provenance_loader: Callable[[Path, str], dict[str, Any]] = load_build_provenance,
        command_runner: Callable[[list[str], Path | None], str] = run_checked,
        kind_matches: Callable[[dict[str, str]], bool] = _installed_kind_matches,
) -> dict[str, Any]:
    """Recapture exact executable, ownership, and provenance bytes after reinstall."""

    binding = _binding(attestation)
    vdb, ownership_records = capture_portage_install_metadata(
        attestation["source"], False, vdb_directory, hash_file, command_runner,
        kind_matches,
    )
    _require(vdb["ownership_sha256"] ==
             attestation["installed"]["vdb"]["ownership_sha256"],
             "ordinary_reinstall_vdb_ownership_mismatch")
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
        "vdb": vdb,
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
        source_checkout: Path, source_bundle: Path, controlled_fixture: Path,
        build_provenance: Path = BUILD_PROVENANCE_PATH,
        vdb_directory: Path = VDB_DIRECTORY,
        hash_file: Callable[[Path], str] = sha256_regular_file,
        is_executable: Callable[[Path], bool] = _default_is_executable,
        command_runner: Callable[[list[str], Path | None], str] = run_checked,
        provenance_loader: Callable[[Path, str], dict[str, Any]] = load_build_provenance,
        kind_matches: Callable[[dict[str, str]], bool] = _installed_kind_matches,
) -> dict[str, Any]:
    """Verify source and inspect the complete installed and harness boundary."""

    source = verify_exact_source(
        source_checkout, source_bundle, hash_file=hash_file, command_runner=command_runner,
    )
    vdb, ownership_records = capture_portage_install_metadata(
        source, True, vdb_directory, hash_file, command_runner, kind_matches,
    )
    regular_records = regular_ownership_records(ownership_records)
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
            "vdb": vdb,
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


def _file_identity(path: Path) -> tuple[int, int]:
    metadata = path.stat(follow_symlinks=False)
    return metadata.st_dev, metadata.st_ino


def capture_user_state_preservation(
        before: Mapping[str, Path], after: Mapping[str, Path],
        hash_file: Callable[[Path], str] = sha256_regular_file,
        file_identity: Callable[[Path], tuple[int, int]] = _file_identity,
) -> list[dict[str, str]]:
    """Return only normalized kind and byte hashes for three user-state files."""

    _require(tuple(before) == USER_DATA_KINDS and tuple(after) == USER_DATA_KINDS,
             "user_data_scope_mismatch")
    paths = [*(before[kind] for kind in USER_DATA_KINDS),
             *(after[kind] for kind in USER_DATA_KINDS)]
    _require(all(path.is_absolute() for path in paths) and
             len({os.fspath(path) for path in paths}) == len(paths),
             "user_data_paths_not_distinct")
    try:
        identities = [file_identity(path) for path in paths]
    except OSError as error:
        raise Pd2CollectorError("user_data_identity_unavailable") from error
    _require(len(set(identities)) == len(identities), "user_data_inodes_not_distinct")
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


def stage_document(kind: str, field: str, value: Any) -> dict[str, Any]:
    """Wrap one collector-owned stage payload in a closed stage envelope."""

    return {"schema_version": 1, "stage_kind": kind, field: value}


def _stage_payload(stage: Any, kind: str, field: str) -> Any:
    _require(isinstance(stage, dict) and set(stage) == {
        "schema_version", "stage_kind", field,
    }, "final_stage_invalid")
    _require(stage["schema_version"] == 1 and stage["stage_kind"] == kind,
             "final_stage_invalid")
    return stage[field]


def finalize_lifecycle(
        attestation: dict[str, Any], qa_stage: dict[str, Any],
        resolutions_stage: dict[str, Any], package_test_stage: dict[str, Any],
        linkage_stage: dict[str, Any], sandbox_tests_stage: dict[str, Any],
        initial_demonstration_stage: dict[str, Any],
        unmerge_stage: dict[str, Any], preservation_stage: dict[str, Any],
        ordinary_reinstall_stage: dict[str, Any],
        reinstall_demonstration_stage: dict[str, Any],
) -> dict[str, Any]:
    """Assemble the lifecycle only from closed collector stage artifacts."""

    try:
        validate_attestation_document(attestation)
    except Pd2AttestationError as error:
        raise Pd2CollectorError(str(error)) from error
    qa = _stage_payload(qa_stage, "pd2_portage_qa", "portage_qa")
    resolutions = _stage_payload(
        resolutions_stage, "pd2_portage_resolutions", "resolutions",
    )
    package_test = _stage_payload(
        package_test_stage, "pd2_package_test", "package_test",
    )
    linkage = _stage_payload(linkage_stage, "pd2_direct_linkage", "direct_linkage")
    sandbox = _stage_payload(
        sandbox_tests_stage, "pd2_sandbox_exclusions", "sandbox_exclusions",
    )
    initial_demo = _stage_payload(
        initial_demonstration_stage, "pd2_installed_demonstration", "demonstration",
    )
    unmerge_absence = _stage_payload(
        unmerge_stage, "pd2_unmerge_absence", "unmerge_absence",
    )
    user_data = _stage_payload(
        preservation_stage, "pd2_user_data_preservation", "user_data",
    )
    ordinary_reinstall = _stage_payload(
        ordinary_reinstall_stage, "pd2_ordinary_reinstall", "ordinary_reinstall",
    )
    reinstall_demo = _stage_payload(
        reinstall_demonstration_stage, "pd2_installed_demonstration", "demonstration",
    )
    binding = _binding(attestation)
    document = {
        "schema_version": 1,
        "evidence_kind": "pd2_portage_installed_lifecycle",
        "source": attestation["source"],
        "portage_qa": qa,
        "resolutions": resolutions,
        "tested_artifact": attestation["tested_artifact"],
        "package_test": package_test,
        "initial_install": attestation["installed"],
        "direct_linkage": linkage,
        "sandbox_exclusions": sandbox,
        "installed_demonstrations": [initial_demo, reinstall_demo],
        "unmerge": {
            **binding,
            "atom": PACKAGE_ATOM,
            "result": "passed",
            **unmerge_absence,
            "user_data": user_data,
        },
        "ordinary_reinstall": ordinary_reinstall,
    }
    try:
        validate_evidence_document(document)
    except Pd2EvidenceError as error:
        raise Pd2CollectorError(str(error)) from error
    return document


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    preflight = subparsers.add_parser("preflight")
    preflight.add_argument("--source-checkout", type=Path, required=True)
    preflight.add_argument("--source-bundle", type=Path, required=True)
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
    qa = subparsers.add_parser("qa")
    qa.add_argument("--attestation", type=Path, required=True)
    qa.add_argument("--source-checkout", type=Path, required=True)
    qa.add_argument("--output", type=Path, required=True)
    resolutions = subparsers.add_parser("resolutions")
    resolutions.add_argument("--attestation", type=Path, required=True)
    resolutions.add_argument("--source-checkout", type=Path, required=True)
    resolutions.add_argument("--output", type=Path, required=True)
    package_test = subparsers.add_parser("package-test")
    package_test.add_argument("--attestation", type=Path, required=True)
    package_test.add_argument("--test-build", type=Path, required=True)
    package_test.add_argument("--source-checkout", type=Path, required=True)
    package_test.add_argument("--controlled-window-fixture", type=Path, required=True)
    package_test.add_argument("--output", type=Path, required=True)
    sandbox_tests = subparsers.add_parser("sandbox-tests")
    sandbox_tests.add_argument("--attestation", type=Path, required=True)
    sandbox_tests.add_argument("--test-build", type=Path, required=True)
    sandbox_tests.add_argument("--source-checkout", type=Path, required=True)
    sandbox_tests.add_argument("--controlled-window-fixture", type=Path, required=True)
    sandbox_tests.add_argument("--output", type=Path, required=True)
    linkage = subparsers.add_parser("linkage")
    linkage.add_argument("--attestation", type=Path, required=True)
    linkage.add_argument("--output", type=Path, required=True)
    reinstall = subparsers.add_parser("reinstall")
    reinstall.add_argument("--attestation", type=Path, required=True)
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
    finalize.add_argument("--qa", type=Path, required=True)
    finalize.add_argument("--resolutions", type=Path, required=True)
    finalize.add_argument("--package-test", type=Path, required=True)
    finalize.add_argument("--linkage", type=Path, required=True)
    finalize.add_argument("--sandbox-tests", type=Path, required=True)
    finalize.add_argument("--initial-demonstration", type=Path, required=True)
    finalize.add_argument("--unmerge", type=Path, required=True)
    finalize.add_argument("--preservation", type=Path, required=True)
    finalize.add_argument("--ordinary-reinstall", type=Path, required=True)
    finalize.add_argument("--reinstall-demonstration", type=Path, required=True)
    finalize.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args(argv)
    try:
        if arguments.command == "preflight":
            document = capture_preflight_attestation(
                arguments.source_checkout, arguments.source_bundle,
                arguments.controlled_window_fixture, arguments.build_provenance,
            )
        elif arguments.command == "preservation":
            before = {kind: getattr(arguments, f"before_{kind}") for kind in USER_DATA_KINDS}
            after = {kind: getattr(arguments, f"after_{kind}") for kind in USER_DATA_KINDS}
            document = stage_document(
                "pd2_user_data_preservation", "user_data",
                capture_user_state_preservation(before, after),
            )
        elif arguments.command == "qa":
            attestation = load_attestation(arguments.attestation)
            document = stage_document(
                "pd2_portage_qa", "portage_qa",
                capture_portage_qa(attestation, arguments.source_checkout),
            )
        elif arguments.command == "resolutions":
            attestation = load_attestation(arguments.attestation)
            document = stage_document(
                "pd2_portage_resolutions", "resolutions",
                capture_portage_resolutions(attestation, arguments.source_checkout),
            )
        elif arguments.command == "package-test":
            attestation = load_attestation(arguments.attestation)
            document = stage_document(
                "pd2_package_test", "package_test", capture_package_test(
                    attestation, arguments.test_build, arguments.source_checkout,
                    arguments.controlled_window_fixture,
                ),
            )
        elif arguments.command == "sandbox-tests":
            attestation = load_attestation(arguments.attestation)
            document = stage_document(
                "pd2_sandbox_exclusions", "sandbox_exclusions",
                capture_sandbox_exclusions(
                    attestation, arguments.test_build, arguments.source_checkout,
                    arguments.controlled_window_fixture,
                ),
            )
        elif arguments.command == "linkage":
            attestation = load_attestation(arguments.attestation)
            document = stage_document(
                "pd2_direct_linkage", "direct_linkage",
                capture_direct_linkage(attestation, SCANELF_PATH),
            )
        elif arguments.command == "reinstall":
            attestation = load_attestation(arguments.attestation)
            document = stage_document(
                "pd2_ordinary_reinstall", "ordinary_reinstall",
                capture_ordinary_reinstall(attestation),
            )
        elif arguments.command == "demonstration":
            attestation = load_attestation(arguments.attestation)
            document = stage_document(
                "pd2_installed_demonstration", "demonstration",
                capture_installed_demonstration(
                    attestation, arguments.phase, arguments.evidence,
                ),
            )
        elif arguments.command == "unmerge":
            attestation = load_attestation(arguments.attestation)
            validate_attestation_document(attestation)
            document = stage_document(
                "pd2_unmerge_absence", "unmerge_absence",
                capture_unmerge_absence(attestation["installed"]["ownership"]["paths"]),
            )
        else:
            attestation = load_attestation(arguments.attestation)
            document = finalize_lifecycle(
                attestation,
                load_capture_json(arguments.qa),
                load_capture_json(arguments.resolutions),
                load_capture_json(arguments.package_test),
                load_capture_json(arguments.linkage),
                load_capture_json(arguments.sandbox_tests),
                load_capture_json(arguments.initial_demonstration),
                load_capture_json(arguments.unmerge),
                load_capture_json(arguments.preservation),
                load_capture_json(arguments.ordinary_reinstall),
                load_capture_json(arguments.reinstall_demonstration),
            )
        atomic_write_json(arguments.output, document)
    except (Pd2AttestationError, Pd2CollectorError, Pd2EvidenceError,
            Pd2LifecycleHashError) as error:
        print(f"pd2_portage_lifecycle_collector: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
