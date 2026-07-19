#!/usr/bin/env python3
"""Run and validate the closed PD2 production asset-performance runner."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path
from typing import Any


MAXIMUM_OUTPUT_BYTES = 65_536
MEASUREMENTS = ("icon_decode", "icon_cache_hit", "wallpaper_decode_publication")


class AssetEvidenceError(RuntimeError):
    """One closed asset evidence failure."""


def require(condition: bool, identifier: str) -> None:
    if not condition:
        raise AssetEvidenceError(identifier)


def validate_document(document: Any, revision: str, environment_id: str) -> None:
    require(
        isinstance(document, dict)
        and set(document)
        == {
            "schema_version",
            "evidence_kind",
            "source_revision",
            "reference_environment_id",
            "method",
            "measurements",
            "redaction",
        },
        "asset_evidence_invalid",
    )
    require(
        document["schema_version"] == 1
        and document["evidence_kind"] == "pd2_asset_performance"
        and document["source_revision"] == revision
        and document["reference_environment_id"] == environment_id,
        "asset_evidence_identity_invalid",
    )
    method = document["method"]
    require(
        isinstance(method, dict)
        and set(method)
        == {
            "clock",
            "completion",
            "icon_fixture",
            "measured_iterations",
            "wallpaper_fixture",
        }
        and method["clock"] == "std_chrono_steady_clock"
        and method["completion"] == "production_async_callback_on_context_thread"
        and method["icon_fixture"] == "warm_file_cache_controlled_local_png_128"
        and method["wallpaper_fixture"]
        == "warm_file_cache_controlled_local_png_1600x900"
        and isinstance(method["measured_iterations"], int)
        and 3 <= method["measured_iterations"] <= 100,
        "asset_evidence_method_invalid",
    )
    measurements = document["measurements"]
    require(
        isinstance(measurements, list) and len(measurements) == len(MEASUREMENTS),
        "asset_evidence_measurements_invalid",
    )
    for name, item in zip(MEASUREMENTS, measurements):
        require(
            isinstance(item, dict)
            and set(item) == {"name", "samples_ns"}
            and item["name"] == name
            and isinstance(item["samples_ns"], list)
            and len(item["samples_ns"]) == method["measured_iterations"]
            and all(
                isinstance(sample, int) and 0 <= sample <= 60_000_000_000
                for sample in item["samples_ns"]
            ),
            "asset_evidence_measurements_invalid",
        )
    require(
        document["redaction"]
        == {
            "contains_filesystem_paths": False,
            "contains_private_content": False,
            "diagnostics_are_closed_ids": True,
        },
        "asset_evidence_redaction_invalid",
    )


def run_and_validate(runner: Path, revision: str, environment_id: str) -> None:
    require(runner.is_absolute() and runner.is_file(), "asset_runner_invalid")
    completed = subprocess.run(
        (
            str(runner),
            "--revision",
            revision,
            "--environment-id",
            environment_id,
            "--iterations",
            "3",
        ),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=30,
        check=False,
    )
    require(
        completed.returncode == 0
        and not completed.stderr
        and 0 < len(completed.stdout) <= MAXIMUM_OUTPUT_BYTES,
        "asset_runner_failed",
    )
    try:
        document = json.loads(completed.stdout.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise AssetEvidenceError("asset_runner_output_invalid") from error
    validate_document(document, revision, environment_id)


def main() -> int:
    try:
        require(len(sys.argv) == 3, "arguments_invalid")
        run_and_validate(
            Path(sys.argv[1]), sys.argv[2], "contract-test"
        )
        print("validate_pd2_asset_performance: valid asset evidence")
        return 0
    except (AssetEvidenceError, OSError, subprocess.SubprocessError) as error:
        identifier = (
            str(error)
            if isinstance(error, AssetEvidenceError)
            else "external_operation_failed"
        )
        print(f"validate_pd2_asset_performance: {identifier}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
