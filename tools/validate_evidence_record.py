#!/usr/bin/env python3
"""Revalidate one bounded Prismdrake validation-record envelope."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import sys
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path, PurePosixPath
from typing import Any, Mapping, Sequence


ROOT = Path(__file__).resolve().parent.parent
MAX_RECORD_BYTES = 64 * 1024
MAX_DEPTH = 10
MAX_VALUES = 1024
MAX_STRING_LENGTH = 1024
MAX_EPHEMERAL_LIFETIME = timedelta(days=30)
MAX_CLOCK_SKEW = timedelta(minutes=5)
REVISION = re.compile(r"(?:[0-9a-f]{40}|[0-9a-f]{64})")
SHA256 = re.compile(r"[0-9a-f]{64}")
IDENTIFIER = re.compile(r"[A-Za-z0-9][A-Za-z0-9._:+-]{0,127}")
COMMAND_ID = re.compile(r"[a-z0-9][a-z0-9._-]{0,127}")
REASON_CODE = re.compile(r"[a-z0-9][a-z0-9_]{0,127}")
CANONICAL_TIMESTAMP = re.compile(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z")
SAFE_RELATIVE_PATH = re.compile(
    r"[A-Za-z0-9_-][A-Za-z0-9._-]*(?:/[A-Za-z0-9_-][A-Za-z0-9._-]*)*"
)
ANCHOR_LOCATION = re.compile(
    r"(?:commit:[0-9a-f]{40}|commit:[0-9a-f]{64}|"
    r"custody:[A-Za-z0-9][A-Za-z0-9._:+@/-]{0,127}|"
    r"signature:[A-Za-z0-9][A-Za-z0-9._:+@/-]{0,127})"
)
CONTROL_CHARACTERS = re.compile(r"[\x00-\x1f\x7f]")
PRIVATE_PATH = re.compile(
    r"(?:^|[\s=:])(?:/[^/\s]|~(?:[/\\]|$)|[A-Za-z]:[/\\]|\\\\)"
)
SECRET_PATTERNS = (
    re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----"),
    re.compile(r"\b(?:gh[opsu]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,})\b"),
    re.compile(r"\bsk-(?:proj-)?[A-Za-z0-9_-]{20,}\b"),
    re.compile(r"\bAKIA[0-9A-Z]{16}\b"),
    re.compile(r"\bBearer\s+[A-Za-z0-9._~+/=-]{16,}\b", re.IGNORECASE),
    re.compile(r"\beyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\b"),
    re.compile(r"[A-Za-z][A-Za-z0-9+.-]*://[^\s/:]+:[^\s/@]+@"),
    re.compile(
        r"(?:^|[\s;])(?:--?)?(?:api[-_]?key|authorization|cookie|credential|"
        r"pass(?:word|phrase)?|private[-_]?key|secret|session[-_]?id|token)=\S+",
        re.IGNORECASE,
    ),
)
FORBIDDEN_FIELD_FRAGMENTS = (
    "argv",
    "credential",
    "environmentvariables",
    "notificationbody",
    "password",
    "privatekey",
    "secret",
    "token",
    "windowtitle",
)
RETENTION_CLASSES = frozenset(
    {
        "tracked-public-test-fixture",
        "project-private-ephemeral",
        "project-private-durable",
        "maintainer-private-ephemeral",
        "maintainer-private-durable",
    }
)
RETENTION_LOCATIONS = {
    "tracked-public-test-fixture": (
        "tracked-test-fixtures",
        PurePosixPath("tests/validation/fixtures"),
    ),
    "project-private-ephemeral": (
        "project-local-private-evidence",
        PurePosixPath("build/evidence/project-private"),
    ),
    "project-private-durable": (
        "project-local-private-evidence",
        PurePosixPath("build/evidence/project-private"),
    ),
    "maintainer-private-ephemeral": (
        "project-local-maintainer-evidence",
        PurePosixPath("build/evidence/maintainer-private"),
    ),
    "maintainer-private-durable": (
        "project-local-maintainer-evidence",
        PurePosixPath("build/evidence/maintainer-private"),
    ),
}
ANCHOR_LOCATION_PREFIX = {
    "committed-root-hash": "commit:",
    "append-only-custody": "custody:",
    "separately-authorized-signature": "signature:",
}


class ValidationError(ValueError):
    """A stable validation failure suitable for bounded reporting."""


@dataclass(frozen=True)
class AnchorExpectation:
    """An anchor assertion obtained separately from the evidence writer."""

    type: str
    root_sha256: str
    authority: str
    location: str
    created_at: str
    verification_procedure: str


@dataclass(frozen=True)
class LoadedRecord:
    """Record bytes and immutable metadata captured from one opened file descriptor."""

    document: Any
    mode: int
    relative_path: PurePosixPath


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise ValidationError(reason)


def exact_object(value: Any, keys: set[str], reason: str) -> Mapping[str, Any]:
    require(isinstance(value, dict) and set(value) == keys, reason)
    return value


def identifier(value: Any, reason: str) -> str:
    require(isinstance(value, str) and IDENTIFIER.fullmatch(value) is not None, reason)
    return value


def digest(value: Any, reason: str) -> str:
    require(isinstance(value, str) and SHA256.fullmatch(value) is not None, reason)
    return value


def timestamp(value: Any, reason: str) -> datetime:
    require(
        isinstance(value, str) and CANONICAL_TIMESTAMP.fullmatch(value) is not None,
        reason,
    )
    try:
        return datetime.strptime(value, "%Y-%m-%dT%H:%M:%SZ").replace(
            tzinfo=timezone.utc
        )
    except ValueError as error:
        raise ValidationError(reason) from error


def scan_bounded(value: Any, depth: int = 0, count: list[int] | None = None) -> None:
    if count is None:
        count = [0]
    count[0] += 1
    require(count[0] <= MAX_VALUES, "record_value_count_exceeded")
    require(depth <= MAX_DEPTH, "record_depth_exceeded")

    if isinstance(value, dict):
        require(len(value) <= 64, "record_object_too_large")
        for key, child in value.items():
            require(isinstance(key, str) and len(key) <= 128, "record_key_invalid")
            normalized = re.sub(r"[^a-z0-9]", "", key.lower())
            require(
                not any(fragment in normalized for fragment in FORBIDDEN_FIELD_FRAGMENTS),
                "forbidden_field_present",
            )
            scan_bounded(child, depth + 1, count)
    elif isinstance(value, list):
        require(len(value) <= 128, "record_array_too_large")
        for child in value:
            scan_bounded(child, depth + 1, count)
    elif isinstance(value, str):
        require(len(value) <= MAX_STRING_LENGTH, "record_string_too_large")
        require(CONTROL_CHARACTERS.search(value) is None, "record_control_character")
        require(PRIVATE_PATH.search(value) is None, "private_path_detected")
        require(
            not any(pattern.search(value) for pattern in SECRET_PATTERNS),
            "secret_pattern_detected",
        )
    else:
        require(value is None or type(value) in {bool, int}, "record_type_invalid")


def canonical_payload_sha256(document: Mapping[str, Any]) -> str:
    payload = {key: value for key, value in document.items() if key != "integrity_anchor"}
    encoded = json.dumps(
        payload,
        ensure_ascii=False,
        allow_nan=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def validate_document(
    document: Any,
    expected_anchor: AnchorExpectation | None,
    *,
    validation_time: datetime | None = None,
) -> None:
    scan_bounded(document)
    root = exact_object(
        document,
        {
            "schema_version",
            "record_kind",
            "source",
            "selection",
            "artifacts",
            "environment",
            "result",
            "identities",
            "custody",
            "integrity_anchor",
        },
        "record_shape_invalid",
    )
    require(
        type(root["schema_version"]) is int and root["schema_version"] == 1,
        "schema_version_invalid",
    )
    require(root["record_kind"] == "prismdrake_validation_record", "record_kind_invalid")

    source = exact_object(root["source"], {"revision", "dirty"}, "source_shape_invalid")
    require(
        isinstance(source["revision"], str)
        and REVISION.fullmatch(source["revision"]) is not None
        and set(source["revision"]) != {"0"},
        "source_revision_invalid",
    )
    require(type(source["dirty"]) is bool, "source_dirty_invalid")

    selection = exact_object(
        root["selection"], {"tier", "selector", "command"}, "selection_shape_invalid"
    )
    require(selection["tier"] in {"T0", "T1", "T2", "T3", "T4"}, "tier_invalid")
    identifier(selection["selector"], "selector_invalid")
    require(
        isinstance(selection["command"], str)
        and COMMAND_ID.fullmatch(selection["command"]) is not None,
        "command_invalid",
    )

    artifacts = root["artifacts"]
    require(isinstance(artifacts, list) and 1 <= len(artifacts) <= 64, "artifacts_invalid")
    artifact_names: list[str] = []
    for value in artifacts:
        artifact = exact_object(value, {"name", "sha256"}, "artifact_shape_invalid")
        artifact_names.append(identifier(artifact["name"], "artifact_name_invalid"))
        digest(artifact["sha256"], "artifact_sha256_invalid")
    require(len(artifact_names) == len(set(artifact_names)), "artifact_name_duplicate")

    environment = exact_object(
        root["environment"], {"identity", "toolchains"}, "environment_shape_invalid"
    )
    identifier(environment["identity"], "environment_identity_invalid")
    toolchains = environment["toolchains"]
    require(isinstance(toolchains, list) and 1 <= len(toolchains) <= 16, "toolchains_invalid")
    toolchain_names: list[str] = []
    for value in toolchains:
        toolchain = exact_object(value, {"name", "version"}, "toolchain_shape_invalid")
        toolchain_names.append(identifier(toolchain["name"], "toolchain_name_invalid"))
        require(
            isinstance(toolchain["version"], str) and 1 <= len(toolchain["version"]) <= 128,
            "toolchain_version_invalid",
        )
    require(len(toolchain_names) == len(set(toolchain_names)), "toolchain_name_duplicate")

    result = exact_object(
        root["result"], {"status", "reason_code", "log"}, "result_shape_invalid"
    )
    require(result["status"] in {"pass", "fail", "skip", "blocked"}, "result_status_invalid")
    reason_code = result["reason_code"]
    if result["status"] in {"skip", "blocked"}:
        require(
            isinstance(reason_code, str) and REASON_CODE.fullmatch(reason_code) is not None,
            "result_reason_required",
        )
    else:
        require(
            reason_code is None
            or (isinstance(reason_code, str) and REASON_CODE.fullmatch(reason_code) is not None),
            "result_reason_invalid",
        )
    if result["log"] is not None:
        log = exact_object(result["log"], {"relative_path", "sha256"}, "log_shape_invalid")
        path_value = log["relative_path"]
        require(
            isinstance(path_value, str)
            and 1 <= len(path_value) <= 240
            and SAFE_RELATIVE_PATH.fullmatch(path_value) is not None,
            "log_path_invalid",
        )
        relative_path = PurePosixPath(path_value)
        require(
            not relative_path.is_absolute()
            and "." not in relative_path.parts
            and ".." not in relative_path.parts,
            "log_path_invalid",
        )
        digest(log["sha256"], "log_sha256_invalid")

    identities = exact_object(
        root["identities"],
        {"product", "collector", "schema", "validator"},
        "identities_shape_invalid",
    )
    role_names: list[str] = []
    role_digests: list[str] = []
    for role in ("product", "collector", "schema", "validator"):
        role_identity = exact_object(
            identities[role],
            {"name", "version", "sha256"},
            f"{role}_identity_shape_invalid",
        )
        role_names.append(identifier(role_identity["name"], f"{role}_identity_name_invalid"))
        version = role_identity["version"]
        require(
            isinstance(version, str) and 1 <= len(version) <= 64,
            f"{role}_identity_version_invalid",
        )
        role_digests.append(
            digest(role_identity["sha256"], f"{role}_identity_sha256_invalid")
        )
    require(len(role_names) == len(set(role_names)), "role_identity_names_not_distinct")
    require(len(role_digests) == len(set(role_digests)), "role_identity_digests_not_distinct")

    custody = exact_object(
        root["custody"],
        {
            "writer_authority",
            "retention_class",
            "authorized_location",
            "authorized_audience",
            "disclosure_classification",
            "owner",
            "mode",
            "expires_at",
            "durable_retention_justification",
            "review_deletion_owner",
            "final_disposition",
        },
        "custody_shape_invalid",
    )
    writer_authority = identifier(custody["writer_authority"], "writer_authority_invalid")
    retention_class = custody["retention_class"]
    require(retention_class in RETENTION_CLASSES, "retention_class_invalid")
    authorized_location = identifier(
        custody["authorized_location"], "authorized_location_invalid"
    )
    require(
        authorized_location == RETENTION_LOCATIONS[retention_class][0],
        "authorized_location_mismatch",
    )
    audience = custody["authorized_audience"]
    require(isinstance(audience, list) and 1 <= len(audience) <= 16, "authorized_audience_invalid")
    audience_values = [identifier(value, "authorized_audience_invalid") for value in audience]
    require(len(audience_values) == len(set(audience_values)), "authorized_audience_duplicate")
    classification = custody["disclosure_classification"]
    require(
        classification in {"public-test-fixture", "project-private", "maintainer-private"},
        "disclosure_classification_invalid",
    )
    identifier(custody["owner"], "custody_owner_invalid")
    require(custody["mode"] in {"0600", "0644"}, "custody_mode_invalid")
    expires_at_value = custody["expires_at"]
    durable = custody["durable_retention_justification"]
    require((expires_at_value is None) != (durable is None), "retention_lifetime_invalid")
    expires_at = None
    if expires_at_value is not None:
        expires_at = timestamp(expires_at_value, "retention_expiry_invalid")
    if durable is not None:
        require(
            isinstance(durable, str) and 1 <= len(durable) <= 240,
            "durable_justification_invalid",
        )
    identifier(custody["review_deletion_owner"], "review_deletion_owner_invalid")
    require(
        custody["final_disposition"] in {"delete", "retain", "archive"},
        "final_disposition_invalid",
    )

    anchor = exact_object(
        root["integrity_anchor"],
        {
            "type",
            "algorithm",
            "root_sha256",
            "authority",
            "location",
            "created_at",
            "verification_procedure",
        },
        "integrity_anchor_shape_invalid",
    )
    anchor_type = anchor["type"]
    require(anchor_type in ANCHOR_LOCATION_PREFIX, "integrity_anchor_type_invalid")
    require(anchor["algorithm"] == "sha256", "integrity_anchor_algorithm_invalid")
    root_sha256 = digest(anchor["root_sha256"], "integrity_anchor_sha256_invalid")
    anchor_authority = identifier(anchor["authority"], "integrity_anchor_authority_invalid")
    require(anchor_authority != writer_authority, "integrity_anchor_authority_not_separate")
    require(
        isinstance(anchor["location"], str)
        and ANCHOR_LOCATION.fullmatch(anchor["location"]) is not None
        and anchor["location"].startswith(ANCHOR_LOCATION_PREFIX[anchor_type]),
        "integrity_anchor_location_invalid",
    )
    anchor_reference = anchor["location"].split(":", 1)[1]
    require(
        all(part not in {"", ".", ".."} for part in anchor_reference.split("/")),
        "integrity_anchor_location_invalid",
    )
    created_at = timestamp(anchor["created_at"], "integrity_anchor_created_at_invalid")
    identifier(anchor["verification_procedure"], "integrity_anchor_verification_invalid")

    now = validation_time or datetime.now(timezone.utc)
    require(now.tzinfo is not None, "validation_time_invalid")
    now = now.astimezone(timezone.utc)
    require(created_at <= now + MAX_CLOCK_SKEW, "integrity_anchor_created_in_future")

    if classification == "public-test-fixture":
        require(
            retention_class == "tracked-public-test-fixture"
            and audience_values == ["public"]
            and custody["mode"] == "0644"
            and durable is not None
            and custody["final_disposition"] == "retain",
            "public_fixture_custody_invalid",
        )
    else:
        require("public" not in audience_values, "private_record_audience_invalid")
        require(custody["mode"] == "0600", "private_record_mode_invalid")
        expected_prefix = (
            "project-private" if classification == "project-private" else "maintainer-private"
        )
        require(retention_class.startswith(expected_prefix + "-"), "retention_class_mismatch")
        if expires_at is not None:
            require(retention_class.endswith("-ephemeral"), "retention_class_mismatch")
            require(expires_at > created_at, "retention_expiry_not_after_creation")
            require(
                expires_at - created_at <= MAX_EPHEMERAL_LIFETIME,
                "retention_lifetime_exceeded",
            )
            require(expires_at > now, "record_retention_expired")
            require(
                custody["final_disposition"] in {"delete", "archive"},
                "final_disposition_invalid",
            )
        else:
            require(retention_class.endswith("-durable"), "retention_class_mismatch")
            require(
                custody["final_disposition"] in {"retain", "archive"},
                "final_disposition_invalid",
            )

    require(
        canonical_payload_sha256(root) == root_sha256,
        "integrity_anchor_payload_mismatch",
    )
    require(expected_anchor is not None, "integrity_anchor_expectation_required")
    observed_anchor = AnchorExpectation(
        type=anchor_type,
        root_sha256=root_sha256,
        authority=anchor_authority,
        location=anchor["location"],
        created_at=anchor["created_at"],
        verification_procedure=anchor["verification_procedure"],
    )
    require(observed_anchor == expected_anchor, "integrity_anchor_external_mismatch")


def _project_relative_parts(path: Path) -> tuple[str, ...]:
    raw = os.fspath(path)
    require("\x00" not in raw, "record_path_unsafe")
    candidate = path if path.is_absolute() else ROOT / path
    normalized = Path(os.path.abspath(os.fspath(candidate)))
    try:
        relative = normalized.relative_to(ROOT)
    except ValueError as error:
        raise ValidationError("record_path_outside_project") from error
    require(
        relative.parts
        and all(part not in {"", ".", ".."} for part in relative.parts),
        "record_path_unsafe",
    )
    return relative.parts


def _open_project_file(path: Path) -> int:
    require(
        hasattr(os, "O_NOFOLLOW") and hasattr(os, "O_DIRECTORY"),
        "record_safe_open_unavailable",
    )
    parts = _project_relative_parts(path)
    directory_flags = os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_DIRECTORY
    file_flags = os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW
    try:
        directory_fd = os.open(ROOT, directory_flags)
        try:
            for component in parts[:-1]:
                next_fd = os.open(component, directory_flags, dir_fd=directory_fd)
                os.close(directory_fd)
                directory_fd = next_fd
            return os.open(parts[-1], file_flags, dir_fd=directory_fd)
        finally:
            os.close(directory_fd)
    except OSError as error:
        raise ValidationError("record_path_unsafe") from error


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        require(key not in result, "record_duplicate_key")
        result[key] = value
    return result


def _reject_number(_value: str) -> Any:
    raise ValidationError("record_number_invalid")


def _bounded_integer(value: str) -> int:
    require(len(value.lstrip("-")) <= 20, "record_number_invalid")
    return int(value)


def load_record(path: Path) -> LoadedRecord:
    relative_path = PurePosixPath(*_project_relative_parts(path))
    descriptor = _open_project_file(path)
    try:
        before = os.fstat(descriptor)
        require(stat.S_ISREG(before.st_mode), "record_not_regular_file")
        require(before.st_nlink == 1, "record_hard_link_rejected")
        require(before.st_uid == os.getuid(), "record_owner_invalid")
        require(before.st_size <= MAX_RECORD_BYTES, "record_too_large")
        mode = stat.S_IMODE(before.st_mode)
        require(before.st_mode & 0o7000 == 0, "record_special_mode_rejected")
        require(mode & 0o022 == 0, "record_file_permissions_unsafe")

        chunks: list[bytes] = []
        total = 0
        while total <= MAX_RECORD_BYTES:
            chunk = os.read(
                descriptor,
                min(16 * 1024, MAX_RECORD_BYTES + 1 - total),
            )
            if not chunk:
                break
            chunks.append(chunk)
            total += len(chunk)
        require(total <= MAX_RECORD_BYTES, "record_too_large")

        after = os.fstat(descriptor)
        before_identity = (
            before.st_dev,
            before.st_ino,
            before.st_size,
            before.st_mode,
            before.st_uid,
            before.st_nlink,
            before.st_mtime_ns,
            before.st_ctime_ns,
        )
        after_identity = (
            after.st_dev,
            after.st_ino,
            after.st_size,
            after.st_mode,
            after.st_uid,
            after.st_nlink,
            after.st_mtime_ns,
            after.st_ctime_ns,
        )
        require(before_identity == after_identity, "record_changed_during_read")
    finally:
        os.close(descriptor)

    try:
        text = b"".join(chunks).decode("utf-8")
        document = json.loads(
            text,
            object_pairs_hook=_reject_duplicate_keys,
            parse_constant=_reject_number,
            parse_float=_reject_number,
            parse_int=_bounded_integer,
        )
    except ValidationError:
        raise
    except (UnicodeError, json.JSONDecodeError, RecursionError, MemoryError, ValueError) as error:
        raise ValidationError("record_unreadable") from error
    return LoadedRecord(
        document=document,
        mode=mode,
        relative_path=relative_path,
    )


def validate_file_custody(record: LoadedRecord) -> None:
    custody = record.document["custody"]
    expected_mode = int(custody["mode"], 8)
    require(record.mode == expected_mode, "record_file_mode_mismatch")
    expected_root = RETENTION_LOCATIONS[custody["retention_class"]][1]
    require(
        record.relative_path == expected_root
        or expected_root in record.relative_path.parents,
        "record_authorized_location_mismatch",
    )


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("record", type=Path)
    parser.add_argument(
        "--anchor-type",
        required=True,
        choices=tuple(ANCHOR_LOCATION_PREFIX),
    )
    parser.add_argument("--anchor-root-sha256", required=True)
    parser.add_argument("--anchor-authority", required=True)
    parser.add_argument("--anchor-location", required=True)
    parser.add_argument("--anchor-created-at", required=True)
    parser.add_argument("--anchor-verification-procedure", required=True)
    options = parser.parse_args(argv)
    expected_anchor = AnchorExpectation(
        type=options.anchor_type,
        root_sha256=options.anchor_root_sha256,
        authority=options.anchor_authority,
        location=options.anchor_location,
        created_at=options.anchor_created_at,
        verification_procedure=options.anchor_verification_procedure,
    )
    try:
        record = load_record(options.record)
        validate_document(record.document, expected_anchor)
        validate_file_custody(record)
    except (OSError, ValidationError):
        print("Prismdrake validation record rejected", file=sys.stderr)
        return 1
    print("Prismdrake validation record accepted")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
