#!/usr/bin/env python3
"""Canonical, hermetic hashes used by PD1 Portage lifecycle evidence."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import stat
from typing import Any


MAX_TREE_ENTRIES = 4096
MAX_TREE_FILE_BYTES = 16 * 1024 * 1024
MAX_TREE_TOTAL_BYTES = 64 * 1024 * 1024


class LifecycleHashError(RuntimeError):
    """A value cannot be represented by the canonical lifecycle hash contract."""


def canonical_json_sha256(value: Any) -> str:
    """Hash a JSON value using the single canonical lifecycle serialization."""

    try:
        encoded = json.dumps(
            value,
            allow_nan=False,
            ensure_ascii=True,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")
    except (TypeError, ValueError) as error:
        raise LifecycleHashError("canonical_json_invalid") from error
    return hashlib.sha256(encoded).hexdigest()


def canonical_artifact_sha256(executables: list[dict[str, Any]]) -> str:
    """Hash normalized installed executable identities in their declared order."""

    normalized: list[dict[str, str]] = []
    for record in executables:
        if (not isinstance(record, dict) or set(record) != {
                "path", "sha256", "regular_executable"} or
                record["regular_executable"] is not True or
                not isinstance(record["path"], str) or
                not isinstance(record["sha256"], str)):
            raise LifecycleHashError("artifact_record_invalid")
        normalized.append({"path": record["path"], "sha256": record["sha256"]})
    if not normalized or len({item["path"] for item in normalized}) != len(normalized):
        raise LifecycleHashError("artifact_records_invalid")
    return canonical_json_sha256(normalized)


def canonical_ownership_sha256(paths: list[str]) -> str:
    """Hash the normalized complete Portage-owned absolute path list."""

    if (not isinstance(paths, list) or not paths or
            any(not isinstance(path, str) or not path.startswith("/usr/") for path in paths)):
        raise LifecycleHashError("ownership_paths_invalid")
    normalized = sorted(paths)
    if len(set(normalized)) != len(normalized):
        raise LifecycleHashError("ownership_paths_invalid")
    return canonical_json_sha256(normalized)


def canonical_linkage_sha256(entries: list[dict[str, Any]]) -> str:
    """Hash normalized scanelf linkage entries in executable-path order."""

    normalized: list[dict[str, Any]] = []
    for record in entries:
        if (not isinstance(record, dict) or set(record) != {
                "path", "executable_sha256", "needed"} or
                not isinstance(record["path"], str) or
                not isinstance(record["executable_sha256"], str) or
                not isinstance(record["needed"], list) or
                any(not isinstance(item, str) for item in record["needed"])):
            raise LifecycleHashError("linkage_record_invalid")
        normalized.append({
            "executable_sha256": record["executable_sha256"],
            "needed": sorted(record["needed"]),
            "path": record["path"],
        })
    normalized.sort(key=lambda item: item["path"])
    if not normalized or len({item["path"] for item in normalized}) != len(normalized):
        raise LifecycleHashError("linkage_records_invalid")
    return canonical_json_sha256(normalized)


def _hash_regular_file(path: Path, metadata: os.stat_result) -> str:
    if metadata.st_size > MAX_TREE_FILE_BYTES:
        raise LifecycleHashError("tree_file_oversized")
    flags = os.O_RDONLY
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise LifecycleHashError("tree_file_unavailable") from error
    try:
        opened = os.fstat(descriptor)
        if (not stat.S_ISREG(opened.st_mode) or opened.st_size != metadata.st_size or
                opened.st_dev != metadata.st_dev or opened.st_ino != metadata.st_ino):
            raise LifecycleHashError("tree_file_changed")
        digest = hashlib.sha256()
        total = 0
        while True:
            block = os.read(descriptor, 64 * 1024)
            if not block:
                break
            total += len(block)
            if total > opened.st_size:
                raise LifecycleHashError("tree_file_changed")
            digest.update(block)
        closed = os.fstat(descriptor)
        if total != opened.st_size or closed.st_size != opened.st_size:
            raise LifecycleHashError("tree_file_changed")
        return digest.hexdigest()
    finally:
        os.close(descriptor)


def canonical_tree_sha256(root: Path) -> str:
    """Hash one filesystem tree without following links or exposing its root path."""

    if not isinstance(root, Path) or not root.is_absolute() or root.is_symlink():
        raise LifecycleHashError("tree_root_invalid")
    if not root.exists():
        return canonical_json_sha256({"entries": [], "state": "absent"})
    if not root.is_dir():
        raise LifecycleHashError("tree_root_invalid")

    records: list[dict[str, str]] = []
    total_bytes = 0
    pending = [root]
    while pending:
        directory = pending.pop()
        try:
            entries = sorted(os.scandir(directory), key=lambda entry: entry.name)
        except OSError as error:
            raise LifecycleHashError("tree_directory_unavailable") from error
        for entry in entries:
            path = Path(entry.path)
            relative = path.relative_to(root).as_posix()
            if not relative or relative.startswith("../"):
                raise LifecycleHashError("tree_path_invalid")
            try:
                metadata = entry.stat(follow_symlinks=False)
            except OSError as error:
                raise LifecycleHashError("tree_entry_unavailable") from error
            if stat.S_ISDIR(metadata.st_mode):
                records.append({"kind": "directory", "path": relative})
                pending.append(path)
            elif stat.S_ISREG(metadata.st_mode):
                total_bytes += metadata.st_size
                if total_bytes > MAX_TREE_TOTAL_BYTES:
                    raise LifecycleHashError("tree_oversized")
                records.append({
                    "kind": "file",
                    "path": relative,
                    "sha256": _hash_regular_file(path, metadata),
                })
            elif stat.S_ISLNK(metadata.st_mode):
                try:
                    target = os.readlink(path)
                except OSError as error:
                    raise LifecycleHashError("tree_symlink_unavailable") from error
                records.append({"kind": "symlink", "path": relative, "target": target})
            else:
                raise LifecycleHashError("tree_entry_type_unsupported")
            if len(records) > MAX_TREE_ENTRIES:
                raise LifecycleHashError("tree_too_many_entries")
    records.sort(key=lambda item: item["path"])
    return canonical_json_sha256({"entries": records, "state": "present"})
