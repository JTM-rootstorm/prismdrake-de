#!/usr/bin/env python3
"""Generate and strictly validate one bounded performance-evidence document."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path
from typing import Any


FIXTURE_SIZES = [16, 64, 256]
MEASUREMENT_SHAPE = [
    ("settings_initial_load", None),
    ("profile_switch_publication", None),
    *[
        item
        for size in FIXTURE_SIZES
        for item in (
            ("desktop_entry_discovery", size),
            ("application_search_response", size),
            ("ewmh_task_model_update", size),
        )
    ],
]


def fail(identifier: str) -> int:
    print(f"validate_evidence: {identifier}", file=sys.stderr)
    return 1


def valid_summary(summary: dict[str, Any], count: int) -> bool:
    samples = summary["samples_ns"]
    if summary["sample_count"] != count or len(samples) != count:
        return False
    ordered = sorted(samples)
    p95_index = ((len(ordered) - 1) * 95 + 99) // 100
    return (
        summary["minimum_ns"] == ordered[0]
        and summary["median_ns"] == ordered[(len(ordered) - 1) // 2]
        and summary["p95_ns"] == ordered[p95_index]
        and summary["maximum_ns"] == ordered[-1]
    )


def main() -> int:
    if len(sys.argv) != 3:
        return fail("invalid_test_invocation")
    executable = Path(sys.argv[1])
    schema_path = Path(sys.argv[2])
    completed = subprocess.run(
        [
            str(executable),
            "--revision",
            "0000000000000000000000000000000000000000",
            "--environment-id",
            "contract-test",
            "--iterations",
            "3",
        ],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=25,
        check=False,
    )
    if completed.returncode != 0 or completed.stderr:
        return fail("evidence_generation_failed")
    try:
        document = json.loads(completed.stdout)
        schema = json.loads(schema_path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError, UnicodeError):
        return fail("json_read_failed")

    project_root = Path(__file__).resolve().parents[2]
    sys.path.insert(0, str(project_root / "tools"))
    from validate import validate_schema  # pylint: disable=import-outside-toplevel

    if validate_schema(document, schema, schema, "performance_evidence"):
        return fail("schema_validation_failed")
    method = document["method"]
    measurements = document["measurements"]
    shape = [(item["name"], item["fixture_count"]) for item in measurements]
    if method["fixture_sizes"] != FIXTURE_SIZES or shape != MEASUREMENT_SHAPE:
        return fail("measurement_shape_mismatch")
    if not all(
        valid_summary(item["statistics"], method["measured_iterations"])
        for item in measurements
    ):
        return fail("summary_mismatch")
    print("validate_evidence: valid version 1 performance evidence")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
