#!/usr/bin/env python3
"""Run the live startup collector and validate its real output contract."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path


COLLECTOR_FAILURE_IDENTIFIERS = frozenset(
    {
        "child_restart_observed",
        "dock_property_output_invalid",
        "dock_property_output_too_large",
        "dock_property_read_failed",
        "dock_property_read_unbounded",
        "duplicate_mapped_dock",
        "external_operation_failed",
        "foreign_mapped_dock",
        "inotify_unavailable",
        "inotify_watch_failed",
        "invalid_arguments",
        "invalid_environment_id",
        "invalid_executable",
        "invalid_inotify_event",
        "invalid_integer",
        "invalid_mapped_dock_contract",
        "invalid_session_instance",
        "invalid_source_revision",
        "invalid_timeout",
        "isolated_display_or_bus_missing",
        "live_endpoint_callback_failed",
        "mapped_client_inventory_failed",
        "mapped_client_inventory_invalid",
        "mapped_dock_not_supervised_shell",
        "mapped_dock_shell_identity_changed",
        "mapped_dock_shell_identity_unavailable",
        "multiple_session_instances",
        "safe_mode_activated",
        "session_child_ownership_unavailable",
        "session_diagnostics_present",
        "session_diagnostics_read_failed",
        "session_diagnostics_too_large",
        "session_exited_before_endpoint",
        "session_shutdown_failed",
        "session_shutdown_unbounded",
        "startup_cleanup_failed",
        "startup_deadline_exceeded",
        "supervised_component_tree_invalid",
        "x11_observer_exited",
        "x11_observer_failed",
        "x11_observer_handshake_failed",
        "x11_observer_shutdown_unbounded",
    }
)


def fail(identifier: str) -> int:
    print(f"validate_live_startup: {identifier}", file=sys.stderr)
    return 1


def collector_failure_identifier(
    completed: subprocess.CompletedProcess[str],
) -> str:
    prefix = "collect_startup_to_panel: "
    if completed.returncode != 2 or completed.stdout or not completed.stderr.endswith("\n"):
        return "collector_failed"
    identifier = completed.stderr[len(prefix) : -1]
    if completed.stderr != f"{prefix}{identifier}\n":
        return "collector_failed"
    if identifier not in COLLECTOR_FAILURE_IDENTIFIERS:
        return "collector_failed"
    return identifier


def main() -> int:
    if len(sys.argv) < 4 or sys.argv[3] != "--":
        return fail("invalid_arguments")
    schema_path = Path(sys.argv[1])
    collector = Path(sys.argv[2])
    try:
        completed = subprocess.run(
            [sys.executable, str(collector), *sys.argv[4:]],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=22,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return fail("collector_unbounded")
    if completed.returncode != 0:
        return fail(collector_failure_identifier(completed))
    if completed.stderr:
        return fail("collector_failed")
    try:
        document = json.loads(completed.stdout)
        schema = json.loads(schema_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return fail("json_read_failed")
    project_root = Path(__file__).resolve().parents[2]
    sys.path.insert(0, str(project_root / "tools"))
    from validate import validate_schema  # pylint: disable=import-outside-toplevel
    from external_evidence_semantics import semantic_error

    if validate_schema(document, schema, schema, "startup_evidence"):
        return fail("schema_validation_failed")
    error = semantic_error(document)
    if error is not None:
        return fail(error)
    print("validate_live_startup: valid live startup evidence")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
