#!/usr/bin/env python3
"""Validate one collected external performance artifact against its strict schema."""

from __future__ import annotations

import json
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        print("validate_external_artifact: invalid_arguments", file=sys.stderr)
        return 2
    try:
        schema_path = Path(sys.argv[1])
        artifact_path = Path(sys.argv[2])
        schema = json.loads(schema_path.read_text(encoding="utf-8"))
        artifact = json.loads(artifact_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        print("validate_external_artifact: json_read_failed", file=sys.stderr)
        return 2
    project_root = Path(__file__).resolve().parents[2]
    sys.path.insert(0, str(project_root / "tools"))
    from validate import validate_schema  # pylint: disable=import-outside-toplevel
    from external_evidence_semantics import semantic_error

    if validate_schema(artifact, schema, schema, "external_evidence"):
        print("validate_external_artifact: schema_validation_failed", file=sys.stderr)
        return 1
    error = semantic_error(artifact)
    if error is not None:
        print(f"validate_external_artifact: {error}", file=sys.stderr)
        return 1
    print("validate_external_artifact: valid version 1 external evidence")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
