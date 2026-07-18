#!/usr/bin/env python3
"""Validate strict external evidence shapes and one live cadence contract run."""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any

from collector_common import redaction_contract
from external_evidence_semantics import semantic_error


REVISION = "0000000000000000000000000000000000000000"


def fail(identifier: str) -> int:
    print(f"validate_external_evidence: {identifier}", file=sys.stderr)
    return 1


def startup_fixture() -> dict[str, Any]:
    return {
        "schema_version": 1,
        "evidence_kind": "startup_to_mapped_panel",
        "release_budget": False,
        "source_revision": REVISION,
        "reference_environment_id": "contract-test",
        "method": {
            "clock": "python_monotonic_ns",
            "deadline_ns": 10_000_000_000,
            "fresh_private_runtime": True,
            "x11_observation": "root_map_notify_with_ewmh_client_resolution",
            "filesystem_observation": "inotify",
            "x11_observer_readiness": "root_property_notify_handshake_before_timestamp",
            "child_restart_observation": "structured_session_diagnostics_until_endpoint",
            "mapped_dock_ownership": "net_wm_pid_exact_direct_shell_child_with_pidfd",
            "mapped_dock_inventory": "post_endpoint_net_client_list_stacking_barrier",
        },
        "result": {
            "duration_ns": 3,
            "ready_marker_ns": 2,
            "mapped_dock_ns": 3,
            "safe_mode": False,
            "child_restart_observed": False,
            "mapped_dock_count": 1,
            "foreign_dock_observed": False,
            "duplicate_dock_observed": False,
        },
        "limitations": [
            "mapped_and_standards_valid_on_xvfb_not_a_reviewed_physical_pixel"
        ],
        "redaction": redaction_contract(),
    }


def idle_fixture() -> dict[str, Any]:
    results = []
    for component in ("session", "settingsd", "shell"):
        results.append(
            {
                "component": component,
                "thread_count_start": 1,
                "thread_count_end": 1,
                "sched_wakeup_count": 2,
                "sched_wakeup_new_count": 1,
                "received_wakeup_count": 3,
            }
        )
    return {
        "schema_version": 1,
        "evidence_kind": "idle_scheduler_wakeups",
        "release_budget": False,
        "source_revision": REVISION,
        "reference_environment_id": "contract-test",
        "method": {
            "clock": "python_monotonic_ns",
            "requested_interval_ns": 60_000_000_000,
            "observed_interval_ns": 60_010_000_000,
            "settling_interval_ns": 5_000_000_000,
            "tracepoints": ["sched:sched_wakeup", "sched:sched_wakeup_new"],
            "target": "exact_starting_thread_ids",
            "collection_scope": "system_wide",
            "process_identity": "pidfd_and_proc_executable_at_boundaries",
            "contract_eligible": True,
            "live_tree_ownership": "exact_session_direct_children",
            "startup_endpoint": "ready_marker_and_single_owned_mapped_dock",
        },
        "results": results,
        "limitations": [
            "filtered_received_scheduler_wakeups_not_cpu_utilization_or_context_switches"
        ],
        "redaction": redaction_contract(),
    }


def main() -> int:
    if len(sys.argv) != 3:
        return fail("invalid_test_invocation")
    schema_path = Path(sys.argv[1])
    cadence_runner = Path(sys.argv[2])
    try:
        schema = json.loads(schema_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return fail("schema_read_failed")
    project_root = Path(__file__).resolve().parents[2]
    sys.path.insert(0, str(project_root / "tools"))
    from validate import validate_schema  # pylint: disable=import-outside-toplevel

    environment = os.environ.copy()
    completed = subprocess.run(
        [
            str(cadence_runner),
            "--revision",
            REVISION,
            "--environment-id",
            "contract-test",
        ],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=environment,
        timeout=45,
        check=False,
    )
    if completed.returncode != 0 or completed.stderr:
        return fail("cadence_generation_failed")
    try:
        cadence = json.loads(completed.stdout)
    except json.JSONDecodeError:
        return fail("cadence_json_invalid")

    documents = [startup_fixture(), idle_fixture(), cadence]
    for document in documents:
        if validate_schema(document, schema, schema, "external_evidence"):
            return fail("schema_validation_failed")
        if semantic_error(document) is not None:
            return fail("semantic_validation_failed")
    mutated = startup_fixture()
    mutated["private_path"] = "/private/value"
    if not validate_schema(mutated, schema, schema, "external_evidence"):
        return fail("strict_schema_negative_failed")
    short_idle = idle_fixture()
    short_idle["method"]["requested_interval_ns"] = 1_000_000_000
    short_idle["method"]["observed_interval_ns"] = 1_010_000_000
    short_idle["method"]["contract_eligible"] = False
    if not validate_schema(short_idle, schema, schema, "external_evidence"):
        return fail("short_idle_schema_negative_failed")
    semantic_mutations = []
    mutated = startup_fixture()
    mutated["result"]["duration_ns"] = 2
    semantic_mutations.append(mutated)
    mutated = startup_fixture()
    mutated["result"]["child_restart_observed"] = True
    semantic_mutations.append(mutated)
    mutated = startup_fixture()
    mutated["result"]["duplicate_dock_observed"] = True
    semantic_mutations.append(mutated)
    mutated = startup_fixture()
    mutated["method"]["deadline_ns"] = 5_000_000_000
    semantic_mutations.append(mutated)
    mutated = startup_fixture()
    mutated["result"]["duration_ns"] = 10_000_000_001
    mutated["result"]["mapped_dock_ns"] = 10_000_000_001
    semantic_mutations.append(mutated)
    mutated = idle_fixture()
    mutated["results"][0]["received_wakeup_count"] = 4
    semantic_mutations.append(mutated)
    mutated = idle_fixture()
    mutated["results"][0]["thread_count_end"] = 2
    semantic_mutations.append(mutated)
    mutated = idle_fixture()
    mutated["results"].reverse()
    semantic_mutations.append(mutated)
    mutated = json.loads(json.dumps(cadence))
    mutated["series"][0], mutated["series"][1] = mutated["series"][1], mutated["series"][0]
    semantic_mutations.append(mutated)
    mutated = json.loads(json.dumps(cadence))
    mutated["series"][0]["statistics"]["p95_ns"] += 1
    semantic_mutations.append(mutated)
    mutated = json.loads(json.dumps(cadence))
    mutated["method"]["font_actual_family"] += " drift"
    semantic_mutations.append(mutated)
    mutated = json.loads(json.dumps(cadence))
    mutated["method"]["font_source_basename"] = "../font.ttf"
    semantic_mutations.append(mutated)
    mutated = json.loads(json.dumps(cadence))
    mutated["method"]["font_source_sha256"] = "0" * 63
    semantic_mutations.append(mutated)
    mutated = json.loads(json.dumps(cadence))
    mutated["method"]["qpa_platform_actual"] = "xcb"
    semantic_mutations.append(mutated)
    mutated = json.loads(json.dumps(cadence))
    mutated["method"]["graphics_backend_actual"] = "opengl"
    semantic_mutations.append(mutated)
    mutated = json.loads(json.dumps(cadence))
    mutated["method"]["qt_graphics_api_reported"] = "unsupported"
    semantic_mutations.append(mutated)
    mutated = json.loads(json.dumps(cadence))
    mutated["method"]["lc_all_environment"] = "en_US.UTF-8"
    semantic_mutations.append(mutated)
    mutated = json.loads(json.dumps(cadence))
    mutated["method"]["runtime_utc_offset_seconds"] = -18_000
    semantic_mutations.append(mutated)
    if any(semantic_error(document) is None for document in semantic_mutations):
        return fail("semantic_negative_failed")
    print("validate_external_evidence: valid strict version 1 contracts")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
