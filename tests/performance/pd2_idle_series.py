#!/usr/bin/env python3
"""Assemble repeated strict idle-wakeup trials into one redacted PD2 series."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import sys
import tempfile
from pathlib import Path
from typing import Any, NoReturn, Sequence


ROOT = Path(__file__).resolve().parents[2]
MINIMUM_TRIALS = 3
MAXIMUM_TRIALS = 10
MAXIMUM_INPUT_BYTES = 65_536
REVISION = re.compile(r"[0-9a-f]{40}|[0-9a-f]{64}")
ENVIRONMENT_ID = re.compile(r"[a-z0-9][a-z0-9._-]{0,63}")


class IdleSeriesError(RuntimeError):
    """One closed idle-series assembly or validation failure."""


class DuplicateJsonKeyError(ValueError):
    """A JSON object repeated a key."""


class ClosedArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> NoReturn:
        del message
        raise IdleSeriesError("arguments_invalid")


def require(condition: bool, identifier: str) -> None:
    if not condition:
        raise IdleSeriesError(identifier)


def duplicate_safe_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise DuplicateJsonKeyError("duplicate key")
        result[key] = value
    return result


def load_json(path: Path) -> Any:
    require(
        path.is_absolute() and path.is_file() and not path.is_symlink(),
        "input_path_invalid",
    )
    try:
        raw = path.read_bytes()
    except OSError as error:
        raise IdleSeriesError("input_read_failed") from error
    require(0 < len(raw) <= MAXIMUM_INPUT_BYTES, "input_size_invalid")
    try:
        return json.loads(raw.decode("utf-8"), object_pairs_hook=duplicate_safe_object)
    except (UnicodeError, ValueError, json.JSONDecodeError) as error:
        raise IdleSeriesError("input_json_invalid") from error


def validate_series(document: Any, revision: str, environment_id: str) -> None:
    require(
        isinstance(document, dict)
        and set(document)
        == {
            "schema_version",
            "evidence_kind",
            "source_revision",
            "reference_environment_id",
            "method",
            "samples_wakeups_per_minute",
            "redaction",
        },
        "idle_series_invalid",
    )
    require(
        document["schema_version"] == 1
        and document["evidence_kind"] == "pd2_idle_wakeup_series"
        and document["source_revision"] == revision
        and document["reference_environment_id"] == environment_id,
        "idle_series_identity_invalid",
    )
    method = document["method"]
    samples = document["samples_wakeups_per_minute"]
    require(
        isinstance(method, dict)
        and method
        == {
            "source_evidence_kind": "idle_scheduler_wakeups",
            "sample_interval_seconds": 60,
            "sample_aggregation": "sum_received_wakeups_across_measured_roles",
            "measured_roles": ["session", "settingsd", "notifyd", "shell"],
            "source_trial_ids_sha256": method.get("source_trial_ids_sha256")
            if isinstance(method, dict)
            else None,
            "trial_count": len(samples) if isinstance(samples, list) else None,
        },
        "idle_series_method_invalid",
    )
    require(
        re.fullmatch(r"[0-9a-f]{64}", method["source_trial_ids_sha256"])
        is not None,
        "idle_series_method_invalid",
    )
    require(
        isinstance(samples, list)
        and MINIMUM_TRIALS <= len(samples) <= MAXIMUM_TRIALS
        and all(type(value) is int and 0 <= value <= 1_000_000 for value in samples),
        "idle_series_samples_invalid",
    )
    require(
        document["redaction"]
        == {
            "contains_filesystem_paths": False,
            "contains_private_content": False,
            "contains_process_ids": False,
            "contains_thread_ids": False,
            "contains_x11_window_ids": False,
            "diagnostics_are_closed_ids": True,
        },
        "idle_series_redaction_invalid",
    )


def validate_idle_trial(document: Any, revision: str, environment_id: str) -> None:
    require(isinstance(document, dict), "idle_trial_invalid")
    tools_path = str(ROOT / "tools")
    performance_path = str(Path(__file__).resolve().parent)
    if tools_path not in sys.path:
        sys.path.insert(0, tools_path)
    if performance_path not in sys.path:
        sys.path.insert(0, performance_path)
    from external_evidence_semantics import (  # pylint: disable=import-outside-toplevel
        semantic_error,
    )
    from validate import validate_schema  # pylint: disable=import-outside-toplevel

    schema = load_json(
        Path(__file__).with_name("external-performance-evidence.schema.json")
    )
    require(
        not validate_schema(document, schema, schema, "external_evidence"),
        "idle_trial_schema_invalid",
    )
    require(
        document.get("evidence_kind") == "idle_scheduler_wakeups",
        "idle_trial_kind_invalid",
    )
    require(semantic_error(document) is None, "idle_trial_semantics_invalid")
    require(
        document.get("source_revision") == revision,
        "idle_trial_revision_mismatch",
    )
    require(
        document.get("reference_environment_id") == environment_id,
        "idle_trial_environment_mismatch",
    )
    require(
        isinstance(document.get("trial_id"), str)
        and re.fullmatch(r"[0-9a-f]{32}", document["trial_id"]) is not None,
        "idle_trial_identity_invalid",
    )


def assemble_series(
    documents: Sequence[Any], revision: str, environment_id: str
) -> dict[str, Any]:
    require(REVISION.fullmatch(revision) is not None, "source_revision_invalid")
    require(
        ENVIRONMENT_ID.fullmatch(environment_id) is not None,
        "reference_environment_invalid",
    )
    require(
        MINIMUM_TRIALS <= len(documents) <= MAXIMUM_TRIALS,
        "trial_count_invalid",
    )
    samples: list[int] = []
    trial_ids: list[str] = []
    for document in documents:
        validate_idle_trial(document, revision, environment_id)
        trial_ids.append(document["trial_id"])
        samples.append(
            sum(item["received_wakeup_count"] for item in document["results"])
        )
    require(len(set(trial_ids)) == len(trial_ids), "idle_trial_duplicate")
    trial_digest = hashlib.sha256("\n".join(trial_ids).encode("ascii")).hexdigest()
    result = {
        "schema_version": 1,
        "evidence_kind": "pd2_idle_wakeup_series",
        "source_revision": revision,
        "reference_environment_id": environment_id,
        "method": {
            "source_evidence_kind": "idle_scheduler_wakeups",
            "sample_interval_seconds": 60,
            "sample_aggregation": "sum_received_wakeups_across_measured_roles",
            "measured_roles": ["session", "settingsd", "notifyd", "shell"],
            "source_trial_ids_sha256": trial_digest,
            "trial_count": len(samples),
        },
        "samples_wakeups_per_minute": samples,
        "redaction": {
            "contains_filesystem_paths": False,
            "contains_private_content": False,
            "contains_process_ids": False,
            "contains_thread_ids": False,
            "contains_x11_window_ids": False,
            "diagnostics_are_closed_ids": True,
        },
    }
    validate_series(result, revision, environment_id)
    return result


def write_series(path: Path, document: dict[str, Any]) -> None:
    require(
        path.is_absolute() and path.parent.is_dir() and not path.exists(),
        "output_path_invalid",
    )
    descriptor = -1
    temporary: Path | None = None
    try:
        descriptor, name = tempfile.mkstemp(
            prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
        )
        temporary = Path(name)
        os.fchmod(descriptor, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            descriptor = -1
            json.dump(document, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.link(temporary, path, follow_symlinks=False)
        temporary.unlink()
        temporary = None
        require(
            stat.S_IMODE(path.stat().st_mode) == 0o600,
            "output_permissions_invalid",
        )
    except OSError as error:
        raise IdleSeriesError("output_write_failed") from error
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        if temporary is not None:
            try:
                temporary.unlink()
            except OSError:
                pass


def parse_arguments() -> argparse.Namespace:
    parser = ClosedArgumentParser(add_help=False)
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--environment-id", required=True)
    parser.add_argument("--input", action="append", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    return parser.parse_args()


def run(options: argparse.Namespace) -> None:
    identities: set[tuple[int, int]] = set()
    for path in options.input:
        try:
            record = path.stat()
        except OSError as error:
            raise IdleSeriesError("input_read_failed") from error
        identity = (record.st_dev, record.st_ino)
        require(identity not in identities, "input_path_duplicate")
        identities.add(identity)
    documents = [load_json(path) for path in options.input]
    document = assemble_series(
        documents, options.source_revision, options.environment_id
    )
    write_series(options.output, document)


def main() -> int:
    try:
        run(parse_arguments())
        return 0
    except (IdleSeriesError, OSError) as error:
        identifier = (
            str(error)
            if isinstance(error, IdleSeriesError)
            else "external_operation_failed"
        )
        print(f"pd2_idle_series: {identifier}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
