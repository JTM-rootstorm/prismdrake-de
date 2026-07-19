#!/usr/bin/env python3
"""Canonical hashes for bounded PD2 installed-lifecycle evidence."""

from __future__ import annotations

import hashlib
import json
import re
from typing import Any


MAX_OWNED_PATHS = 4096
OWNED_PATH_KINDS = ("directory", "regular", "symlink")
_INSTALLED_PATH = re.compile(r"/usr(?:/[A-Za-z0-9._+@-]+)*")


class Pd2LifecycleHashError(RuntimeError):
    """A value cannot be represented by the closed PD2 hash contract."""


def canonical_json_sha256(value: Any) -> str:
    """Hash one JSON value with the closed lifecycle serialization."""

    try:
        encoded = json.dumps(
            value,
            allow_nan=False,
            ensure_ascii=True,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")
    except (TypeError, ValueError) as error:
        raise Pd2LifecycleHashError("canonical_json_invalid") from error
    return hashlib.sha256(encoded).hexdigest()


def validate_installed_path(value: Any) -> str:
    """Return one normalized non-private Portage-installed path."""

    if not isinstance(value, str) or len(value) > 512 or _INSTALLED_PATH.fullmatch(value) is None:
        raise Pd2LifecycleHashError("installed_path_invalid")
    return value


def normalize_ownership_records(records: Any) -> list[dict[str, str]]:
    """Normalize the complete Portage-owned path-and-kind set."""

    if not isinstance(records, list) or not 1 <= len(records) <= MAX_OWNED_PATHS:
        raise Pd2LifecycleHashError("ownership_records_invalid")
    normalized: list[dict[str, str]] = []
    for record in records:
        if not isinstance(record, dict) or set(record) != {"kind", "path"}:
            raise Pd2LifecycleHashError("ownership_record_invalid")
        kind = record["kind"]
        if kind not in OWNED_PATH_KINDS:
            raise Pd2LifecycleHashError("ownership_kind_invalid")
        path = validate_installed_path(record["path"])
        if path == "/usr" and kind != "directory":
            raise Pd2LifecycleHashError("ownership_root_kind_invalid")
        normalized.append({"kind": kind, "path": path})
    normalized.sort(key=lambda item: item["path"])
    if len({item["path"] for item in normalized}) != len(normalized):
        raise Pd2LifecycleHashError("ownership_paths_duplicate")
    return normalized


def canonical_ownership_sha256(records: Any) -> str:
    """Hash the complete normalized Portage-owned path-and-kind set."""

    return canonical_json_sha256(normalize_ownership_records(records))


def regular_ownership_records(records: Any) -> list[dict[str, str]]:
    """Select the sorted package-owned regular paths used for unmerge checks."""

    return [record for record in normalize_ownership_records(records)
            if record["kind"] == "regular"]


def canonical_regular_ownership_sha256(records: Any) -> str:
    """Hash exactly the package-owned regular path subset."""

    regular = regular_ownership_records(records)
    if not regular:
        raise Pd2LifecycleHashError("ownership_regular_paths_missing")
    return canonical_json_sha256(regular)


def removable_ownership_records(records: Any) -> list[dict[str, str]]:
    """Select package-owned files and symlinks that unmerge must remove."""

    return [record for record in normalize_ownership_records(records)
            if record["kind"] != "directory"]


def canonical_removable_ownership_sha256(records: Any) -> str:
    """Hash the complete package-owned nondirectory path subset."""

    removable = removable_ownership_records(records)
    if not removable:
        raise Pd2LifecycleHashError("ownership_removable_paths_missing")
    return canonical_json_sha256(removable)


def canonical_executable_sha256(records: Any) -> str:
    """Hash five ordered executable path and content identities."""

    if not isinstance(records, list):
        raise Pd2LifecycleHashError("executable_records_invalid")
    normalized: list[dict[str, str]] = []
    for record in records:
        if (not isinstance(record, dict) or
                set(record) != {"path", "sha256", "regular_executable"} or
                record["regular_executable"] is not True or
                not isinstance(record["sha256"], str) or
                re.fullmatch(r"[0-9a-f]{64}", record["sha256"]) is None):
            raise Pd2LifecycleHashError("executable_record_invalid")
        normalized.append({
            "path": validate_installed_path(record["path"]),
            "sha256": record["sha256"],
        })
    if len({item["path"] for item in normalized}) != len(normalized):
        raise Pd2LifecycleHashError("executable_paths_duplicate")
    return canonical_json_sha256(normalized)


def canonical_linkage_sha256(records: Any) -> str:
    """Hash normalized direct linkage for the five executable identities."""

    if not isinstance(records, list):
        raise Pd2LifecycleHashError("linkage_records_invalid")
    normalized: list[dict[str, Any]] = []
    for record in records:
        if (not isinstance(record, dict) or
                set(record) != {"path", "executable_sha256", "needed"} or
                not isinstance(record["executable_sha256"], str) or
                re.fullmatch(r"[0-9a-f]{64}", record["executable_sha256"]) is None or
                not isinstance(record["needed"], list) or not record["needed"] or
                any(not isinstance(item, str) or
                    re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+\-]{0,79}", item) is None
                    for item in record["needed"])):
            raise Pd2LifecycleHashError("linkage_record_invalid")
        needed = sorted(record["needed"])
        if len(set(needed)) != len(needed):
            raise Pd2LifecycleHashError("linkage_needed_duplicate")
        normalized.append({
            "executable_sha256": record["executable_sha256"],
            "needed": needed,
            "path": validate_installed_path(record["path"]),
        })
    normalized.sort(key=lambda item: item["path"])
    if len({item["path"] for item in normalized}) != len(normalized):
        raise Pd2LifecycleHashError("linkage_paths_duplicate")
    return canonical_json_sha256(normalized)
