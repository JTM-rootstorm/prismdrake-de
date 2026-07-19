#!/usr/bin/env python3
"""Run or validate the versioned PD2 installed demonstration contract.

The production Xorg/WM/D-Bus adapter is intentionally supplied later by the
installed-session integration lane. This module owns the closed workflow,
privacy boundary, cleanup semantics, evidence schema binding, and dependency
injection seam without manufacturing live evidence.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import stat
import sys
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Protocol


SCHEMA_VERSION = 1
MAX_EVIDENCE_BYTES = 65_536
EXPECTED_RESULT = "expected_state_observed"
EXECUTABLE_NAMES = (
    "prismdrake-session",
    "prismdrake-settingsd",
    "prismdrake-notifyd",
    "prismdrake-shell",
    "prismdrake-shellctl",
)
PROCESS_ROLES = (
    "xorg_dummy_server",
    "openbox_window_manager",
    "isolated_session_bus",
    "prismdrake_session",
)

WORKFLOW_STEPS = (
    "start_isolated_two_output_x11",
    "start_installed_prismdrake_session",
    "verify_component_readiness",
    "verify_desktop_wallpaper_both_outputs",
    "verify_panel_policy_both_outputs",
    "invoke_launcher_through_shellctl",
    "search_controlled_application",
    "pin_controlled_application",
    "launch_controlled_application",
    "launch_second_window",
    "verify_task_grouping",
    "activate_one_window",
    "minimize_one_window",
    "reactivate_one_window",
    "close_both_windows_through_panel",
    "verify_authoritative_task_removal",
    "create_normal_notification",
    "create_replacement_notification",
    "invoke_live_notification_action",
    "open_notification_center",
    "toggle_do_not_disturb",
    "verify_history_without_transient",
    "verify_critical_bypass_policy",
    "clear_notification_history",
    "open_quick_settings",
    "switch_lustre_to_forge",
    "verify_complete_settings_generation",
    "switch_back_preserving_accessibility",
    "remove_current_primary_output",
    "verify_surface_migration",
    "readd_removed_output",
    "restart_settingsd",
    "verify_complete_settings_epoch_rebuild",
    "restart_notifyd",
    "verify_notification_state_reacquisition",
    "restart_shell",
    "verify_windows_and_notification_state_survive",
    "request_logout_through_shellctl",
    "cancel_logout_once",
    "request_and_confirm_logout",
    "verify_reverse_order_prismdrake_shutdown",
    "verify_unrelated_processes_survive",
    "verify_clean_test_environment_cleanup",
)

FAILURE_SCENARIOS = (
    "two_same_scale_outputs_at_startup",
    "add_second_output_after_startup",
    "remove_primary_output",
    "select_new_primary_output",
    "change_output_geometry",
    "restart_settingsd",
    "restart_notifyd",
    "restart_shell",
    "replace_window_manager",
    "run_without_compositor",
    "run_without_blur_capability",
    "start_with_invalid_wallpaper",
    "start_with_malformed_pin_state",
    "start_with_malformed_persistent_history",
    "start_with_conflicting_notification_owner",
    "lose_x_server",
    "enter_and_exit_safe_mode",
)

TIER_C_STEPS = (
    "desktop",
    "panel",
    "launcher",
    "task_actions",
    "notification",
    "notification_center",
    "quick_settings",
    "logout",
)

PHASES = ("installed_workflow", "failure_replay", "tier_c_replay")
PRIVATE_MARKERS = (
    "dbus_session_bus_address",
    "at_spi_bus_address",
    "unix:",
    '"pid"',
    '"xid"',
    '"title"',
    '"body"',
    '"args"',
    '"arguments"',
    '"command"',
    '"path"',
    "/home/",
    "/root/",
    "/tmp/",
    "/mnt/",
    "file://",
    "display=",
)


class HarnessError(RuntimeError):
    """The PD2 harness contract failed closed."""


class DuplicateJsonKeyError(ValueError):
    """Strict JSON loading encountered a repeated object key."""


def require(condition: bool, identifier: str) -> None:
    if not condition:
        raise HarnessError(identifier)


@dataclass(frozen=True)
class Observation:
    """One already-normalized backend result; it contains no captured content."""

    observed: bool
    passed: bool
    result: str = EXPECTED_RESULT


class HarnessPort(Protocol):
    """Injected owner of all live X11, D-Bus, process, and fixture operations."""

    def installed_executable_hashes(self) -> Mapping[str, str]: ...

    def run_step(self, phase: str, identifier: str) -> Observation: ...

    def capture_process_ownership(self) -> Mapping[str, object]: ...

    def cleanup(self, phase: str) -> Observation: ...


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


def load_evidence(path: Path) -> Any:
    """Load one bounded UTF-8 evidence object without duplicate keys."""

    try:
        raw = path.read_bytes()
    except OSError as error:
        raise HarnessError("evidence_read_failed") from error
    require(0 < len(raw) <= MAX_EVIDENCE_BYTES, "evidence_size_invalid")
    try:
        return json.loads(
            raw.decode("utf-8"),
            object_pairs_hook=_reject_duplicate_keys,
            parse_constant=_reject_constant,
            parse_float=_parse_finite_float,
        )
    except (UnicodeError, ValueError, json.JSONDecodeError) as error:
        raise HarnessError("evidence_json_invalid") from error


def validate_schema(document: Any) -> None:
    """Validate against the tracked versioned schema."""

    project_root = Path(__file__).resolve().parents[2]
    tools_path = str(project_root / "tools")
    if tools_path not in sys.path:
        sys.path.insert(0, tools_path)
    from validate import validate_schema as project_validate  # pylint: disable=import-outside-toplevel

    try:
        schema = json.loads(
            Path(__file__).with_name("pd2-demo-evidence.schema.json").read_text(encoding="utf-8")
        )
    except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as error:
        raise HarnessError("evidence_schema_unavailable") from error
    require(
        not project_validate(document, schema, schema, "pd2_demo_evidence"),
        "evidence_schema_mismatch",
    )


def _result_hash(phase: str, ordinal: int, identifier: str, result: str) -> str:
    payload = json.dumps(
        [SCHEMA_VERSION, phase, ordinal, identifier, result],
        ensure_ascii=True,
        separators=(",", ":"),
    ).encode("ascii")
    return hashlib.sha256(payload).hexdigest()


def _result_record(phase: str, ordinal: int, identifier: str,
                   observation: Observation) -> dict[str, Any]:
    require(isinstance(observation, Observation), "observation_type_invalid")
    require(observation.observed, "step_unobserved")
    require(observation.passed, "step_failed")
    require(observation.result == EXPECTED_RESULT, "step_result_invalid")
    return {
        "ordinal": ordinal,
        "id": identifier,
        "observed": True,
        "passed": True,
        "result": observation.result,
        "result_sha256": _result_hash(phase, ordinal, identifier, observation.result),
    }


def _validate_executables(values: Mapping[str, str]) -> list[dict[str, str]]:
    require(isinstance(values, Mapping), "executable_identity_set_invalid")
    require(tuple(values.keys()) == EXECUTABLE_NAMES, "executable_identity_set_invalid")
    records: list[dict[str, str]] = []
    for name in EXECUTABLE_NAMES:
        digest = values[name]
        require(isinstance(digest, str) and re.fullmatch(r"[0-9a-f]{64}", digest) is not None,
                "executable_hash_invalid")
        records.append({"name": name, "sha256": digest})
    return records


def _validate_process_ownership(values: Mapping[str, object]) -> None:
    require(isinstance(values, Mapping) and tuple(values.keys()) == PROCESS_ROLES,
            "process_ownership_set_invalid")
    identities = list(values.values())
    require(all(identity is not None for identity in identities), "process_ownership_invalid")
    for index, identity in enumerate(identities):
        require(all(identity != previous for previous in identities[:index]),
                "process_ownership_duplicate")


def _aggregate_hash(executables: Sequence[Mapping[str, str]],
                    result_groups: Sequence[Sequence[Mapping[str, Any]]]) -> str:
    normalized = {
        "executables": list(executables),
        "results": [record["result_sha256"] for group in result_groups for record in group],
    }
    payload = json.dumps(
        normalized, ensure_ascii=True, separators=(",", ":"), sort_keys=True
    ).encode("ascii")
    return hashlib.sha256(payload).hexdigest()


def _string_values(value: Any) -> list[str]:
    if isinstance(value, str):
        return [value]
    if isinstance(value, Mapping):
        return [item for child in value.values() for item in _string_values(child)]
    if isinstance(value, Sequence) and not isinstance(value, (str, bytes, bytearray)):
        return [item for child in value for item in _string_values(child)]
    return []


def _raise_backend_failure(error: Exception) -> None:
    if isinstance(error, HarnessError):
        raise error
    raise HarnessError("backend_operation_failed") from error


def _run_phase(port: HarnessPort, phase: str,
               identifiers: Sequence[str]) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    require(phase in PHASES and identifiers, "phase_contract_invalid")
    results: list[dict[str, Any]] = []
    failure: Exception | None = None
    try:
        for index, identifier in enumerate(identifiers, start=1):
            results.append(_result_record(phase, index, identifier,
                                          port.run_step(phase, identifier)))
    except Exception as error:  # Cleanup is mandatory after an injected backend failure.
        failure = error

    try:
        cleanup_record = _result_record(
            phase, len(identifiers) + 1, "phase_cleanup", port.cleanup(phase)
        )
    except Exception as cleanup_error:
        raise HarnessError("cleanup_failed") from cleanup_error
    if failure is not None:
        _raise_backend_failure(failure)
    return results, cleanup_record


def run_harness(source_revision: str, port: HarnessPort) -> dict[str, Any]:
    """Execute the closed contract through an injected live or hermetic backend."""

    require(re.fullmatch(r"[0-9a-f]{40}", source_revision) is not None,
            "source_revision_invalid")
    executables = _validate_executables(port.installed_executable_hashes())

    workflow_results: list[dict[str, Any]] = []
    workflow_failure: Exception | None = None
    try:
        for index, identifier in enumerate(WORKFLOW_STEPS[:-1], start=1):
            workflow_results.append(
                _result_record("installed_workflow", index, identifier,
                               port.run_step("installed_workflow", identifier))
            )
            if index == 3:
                _validate_process_ownership(port.capture_process_ownership())
    except Exception as error:
        workflow_failure = error

    try:
        workflow_cleanup = _result_record(
            "installed_workflow", len(WORKFLOW_STEPS), WORKFLOW_STEPS[-1],
            port.cleanup("installed_workflow")
        )
    except Exception as cleanup_error:
        raise HarnessError("cleanup_failed") from cleanup_error
    if workflow_failure is not None:
        _raise_backend_failure(workflow_failure)
    workflow_results.append(workflow_cleanup)

    failure_results, failure_cleanup = _run_phase(
        port, "failure_replay", FAILURE_SCENARIOS
    )
    tier_c_results, tier_c_cleanup = _run_phase(port, "tier_c_replay", TIER_C_STEPS)

    document: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "evidence_kind": "pd2_installed_multi_output_failure_demonstration",
        "source_revision": source_revision,
        "environment": {
            "artifact_provenance": "portage_installed",
            "display_topology": "isolated_xorg_dummy_two_output_same_scale",
            "window_manager": "openbox_ewmh",
            "session_bus": "isolated_dbus_run_session",
            "capability_tier": "tier_b_standards_only",
            "glasswyrm_native_capabilities": "withheld",
        },
        "installed_executables": executables,
        "process_ownership": {
            "roles": list(PROCESS_ROLES),
            "result": _result_record(
                "installed_workflow", 0, "verify_isolated_process_ownership",
                Observation(True, True)
            ),
        },
        "workflow": workflow_results,
        "failure_replay": failure_results,
        "tier_c_replay": tier_c_results,
        "cleanup": {
            "installed_workflow": workflow_cleanup,
            "failure_replay": failure_cleanup,
            "tier_c_replay": tier_c_cleanup,
        },
        "scope": {
            "installed_artifacts_only": True,
            "capture_mode": "live_installed_observations_required",
            "tier_c_without_compositor": True,
            "glasswyrm_native_protocols": False,
            "secure_session_claim": False,
            "production_ready_claim": False,
        },
    }
    document["normalized_results_sha256"] = _aggregate_hash(
        executables,
        (
            [document["process_ownership"]["result"]],
            workflow_results,
            failure_results,
            [failure_cleanup],
            tier_c_results,
            [tier_c_cleanup],
        ),
    )
    validate_evidence(document)
    validate_schema(document)
    return document


def _validate_result_group(document: Any, phase: str, identifiers: Sequence[str]) -> None:
    require(isinstance(document, list) and len(document) == len(identifiers),
            "result_group_invalid")
    for ordinal, (record, identifier) in enumerate(zip(document, identifiers), start=1):
        require(isinstance(record, dict), "result_record_invalid")
        require(record.get("ordinal") == ordinal and record.get("id") == identifier,
                "result_order_invalid")
        require(record.get("observed") is True and record.get("passed") is True and
                record.get("result") == EXPECTED_RESULT, "result_state_invalid")
        require(record.get("result_sha256") ==
                _result_hash(phase, ordinal, identifier, EXPECTED_RESULT),
                "result_hash_mismatch")


def validate_evidence(document: Any) -> None:
    """Apply strict semantics and privacy checks independent of the schema."""

    validate_schema(document)
    require(isinstance(document, dict), "evidence_root_invalid")
    encoded = json.dumps(document, ensure_ascii=True, separators=(",", ":"), sort_keys=True)
    require(len(encoded.encode("utf-8")) <= MAX_EVIDENCE_BYTES, "evidence_oversized")
    lowered = encoded.lower()
    require(not any(marker in lowered for marker in PRIVATE_MARKERS), "evidence_private_data")
    require(not any(re.search(r":[0-9]+", value) for value in _string_values(document)),
            "evidence_private_data")

    require(document.get("schema_version") == SCHEMA_VERSION and
            document.get("evidence_kind") ==
            "pd2_installed_multi_output_failure_demonstration",
            "evidence_identity_invalid")
    source_revision = document.get("source_revision")
    require(isinstance(source_revision, str) and
            re.fullmatch(r"[0-9a-f]{40}", source_revision) is not None,
            "source_revision_invalid")
    environment = document.get("environment")
    require(environment == {
        "artifact_provenance": "portage_installed",
        "display_topology": "isolated_xorg_dummy_two_output_same_scale",
        "window_manager": "openbox_ewmh",
        "session_bus": "isolated_dbus_run_session",
        "capability_tier": "tier_b_standards_only",
        "glasswyrm_native_capabilities": "withheld",
    }, "environment_contract_invalid")

    executable_values = document.get("installed_executables")
    require(isinstance(executable_values, list) and len(executable_values) == len(EXECUTABLE_NAMES),
            "executable_identity_set_invalid")
    executable_mapping: dict[str, str] = {}
    for record in executable_values:
        require(isinstance(record, dict) and set(record) == {"name", "sha256"},
                "executable_identity_invalid")
        name = record["name"]
        require(isinstance(name, str) and name not in executable_mapping,
                "executable_identity_invalid")
        executable_mapping[name] = record["sha256"]
    _validate_executables(executable_mapping)

    ownership = document.get("process_ownership")
    require(isinstance(ownership, dict) and ownership.get("roles") == list(PROCESS_ROLES),
            "process_ownership_evidence_invalid")
    ownership_result = ownership.get("result")
    require(isinstance(ownership_result, dict) and ownership_result.get("ordinal") == 0 and
            ownership_result.get("id") == "verify_isolated_process_ownership" and
            ownership_result.get("observed") is True and ownership_result.get("passed") is True and
            ownership_result.get("result") == EXPECTED_RESULT and
            ownership_result.get("result_sha256") == _result_hash(
                "installed_workflow", 0, "verify_isolated_process_ownership", EXPECTED_RESULT),
            "process_ownership_evidence_invalid")

    _validate_result_group(document.get("workflow"), "installed_workflow", WORKFLOW_STEPS)
    _validate_result_group(document.get("failure_replay"), "failure_replay", FAILURE_SCENARIOS)
    _validate_result_group(document.get("tier_c_replay"), "tier_c_replay", TIER_C_STEPS)

    cleanup = document.get("cleanup")
    require(isinstance(cleanup, dict) and set(cleanup) == set(PHASES), "cleanup_evidence_invalid")
    require(cleanup["installed_workflow"] == document["workflow"][-1],
            "cleanup_evidence_invalid")
    for phase, ordinal in (("failure_replay", len(FAILURE_SCENARIOS) + 1),
                           ("tier_c_replay", len(TIER_C_STEPS) + 1)):
        record = cleanup[phase]
        require(isinstance(record, dict) and record.get("ordinal") == ordinal and
                record.get("id") == "phase_cleanup" and record.get("observed") is True and
                record.get("passed") is True and record.get("result") == EXPECTED_RESULT and
                record.get("result_sha256") ==
                _result_hash(phase, ordinal, "phase_cleanup", EXPECTED_RESULT),
                "cleanup_evidence_invalid")

    scope = document.get("scope")
    require(scope == {
        "installed_artifacts_only": True,
        "capture_mode": "live_installed_observations_required",
        "tier_c_without_compositor": True,
        "glasswyrm_native_protocols": False,
        "secure_session_claim": False,
        "production_ready_claim": False,
    }, "scope_claim_invalid")
    expected_aggregate = _aggregate_hash(
        executable_values,
        (
            [ownership_result],
            document["workflow"],
            document["failure_replay"],
            [cleanup["failure_replay"]],
            document["tier_c_replay"],
            [cleanup["tier_c_replay"]],
        ),
    )
    require(document.get("normalized_results_sha256") == expected_aggregate,
            "aggregate_hash_mismatch")


def write_evidence(path: Path, document: Any) -> None:
    """Atomically write validated evidence as a private regular file."""

    validate_evidence(document)
    require(path.is_absolute() and path.name not in {"", ".", ".."}, "output_target_invalid")
    require(path.parent.is_dir() and not path.parent.is_symlink(), "output_parent_invalid")
    if path.exists() or path.is_symlink():
        require(path.is_file() and not path.is_symlink(), "output_target_invalid")
    temporary = path.with_suffix(path.suffix + ".tmp")
    require(not temporary.exists() and not temporary.is_symlink(), "output_temporary_exists")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = -1
    try:
        descriptor = os.open(temporary, flags, 0o600)
        payload = (json.dumps(document, indent=2, ensure_ascii=True, sort_keys=True) +
                   "\n").encode("utf-8")
        require(len(payload) <= MAX_EVIDENCE_BYTES, "evidence_oversized")
        with os.fdopen(descriptor, "wb", closefd=True) as stream:
            descriptor = -1
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        require(stat.S_IMODE(path.stat().st_mode) == 0o600, "output_permissions_invalid")
    except OSError as error:
        raise HarnessError("output_write_failed") from error
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--validate-evidence", type=Path)
    arguments = parser.parse_args(argv)
    try:
        if arguments.validate_evidence is None:
            raise HarnessError("installed_backend_not_configured")
        document = load_evidence(arguments.validate_evidence)
        validate_evidence(document)
    except HarnessError as error:
        print(f"PD2 installed demonstration failed closed: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
