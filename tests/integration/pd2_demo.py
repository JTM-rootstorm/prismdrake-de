#!/usr/bin/env python3
"""Run or validate the versioned PD2 installed demonstration contract.

The live Xorg/WM/D-Bus adapter is supplied by the installed-session integration
lane. This module owns the closed workflow, privacy boundary, cleanup semantics,
evidence schema binding, and dependency injection seam without manufacturing
live evidence.
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
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any, NoReturn, Protocol


# When invoked directly, keep the adapter's import of ``pd2_demo`` bound to
# this exact module so protocol value classes are not defined a second time.
if __name__ == "__main__":
    sys.modules["pd2_demo"] = sys.modules[__name__]


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
    "verify_output_migration_accessibility",
    "readd_removed_output",
    "restart_settingsd",
    "verify_complete_settings_epoch_rebuild",
    "verify_settingsd_focus_recovery",
    "restart_notifyd",
    "verify_notification_state_reacquisition",
    "verify_notifyd_focus_recovery",
    "restart_shell",
    "verify_windows_and_notification_state_survive",
    "verify_shell_focus_recovery",
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
    "replace_or_restart_window_manager",
    "run_without_compositor",
    "run_with_missing_blur_capability",
    "start_with_invalid_wallpaper",
    "start_with_malformed_pin_state",
    "start_with_malformed_persistent_history",
    "start_when_another_notification_owner_exists",
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
TIER_C_ASSERTIONS = (
    "opaque_materials",
    "no_shell_side_capture",
    "no_blur_attempt",
    "complete_keyboard_operation",
    "readable_text_and_visible_focus",
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

INSTALLED_EXECUTABLE_PATHS = tuple(
    Path("/usr/bin") / name for name in EXECUTABLE_NAMES
)
REVIEWED_LIVE_TOOL_PATHS = {
    "xorg": Path("/usr/bin/Xorg"),
    "xrandr": Path("/usr/bin/xrandr"),
    "openbox": Path("/usr/bin/openbox"),
    "xprop": Path("/usr/bin/xprop"),
    "xwininfo": Path("/usr/bin/xwininfo"),
    "dbus_daemon": Path("/usr/bin/dbus-daemon"),
    "xdotool": Path("/usr/bin/xdotool"),
    "xwd": Path("/usr/bin/xwd"),
    "x11trace": Path("/usr/bin/x11trace"),
}
MAX_BOUND_FILE_BYTES = 128 * 1024 * 1024


class HarnessError(RuntimeError):
    """The PD2 harness contract failed closed."""


class DuplicateJsonKeyError(ValueError):
    """Strict JSON loading encountered a repeated object key."""


class _RedactedArgumentParser(argparse.ArgumentParser):
    """Reject invalid invocations without echoing caller-supplied paths."""

    def error(self, message: str) -> NoReturn:
        del message
        raise HarnessError("arguments_invalid")


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

    def installed_executable_hashes(self) -> Mapping[str, str]:
        """Hash verified executable files from the Portage-installed package."""
        ...

    def run_step(self, phase: str, identifier: str) -> Observation:
        """Observe one named predicate, including every Tier C assertion per surface."""
        ...

    def capture_process_ownership(self) -> Mapping[str, object]:
        """Return distinct opaque identities for the isolated process owners."""
        ...

    def cleanup(self, phase: str) -> Observation:
        """Confirm no isolated-run Prismdrake, fixture, WM, bus, or X process remains."""
        ...


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


def _raise_backend_failure(error: Exception) -> NoReturn:
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
    try:
        executable_values = port.installed_executable_hashes()
    except Exception as error:
        _raise_backend_failure(error)
    executables = _validate_executables(executable_values)

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
            "session_bus": "isolated_private_session_bus",
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
            "tier_c_assertions": list(TIER_C_ASSERTIONS),
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
        "session_bus": "isolated_private_session_bus",
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
        "tier_c_assertions": list(TIER_C_ASSERTIONS),
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


def _sha256_regular_file(path: Path) -> str:
    """Hash one exact bounded regular file without following its final link."""

    require(path.is_absolute() and not path.is_symlink(), "bound_file_invalid")
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise HarnessError("bound_file_unavailable") from error
    try:
        before = os.fstat(descriptor)
        require(
            stat.S_ISREG(before.st_mode)
            and 0 < before.st_size <= MAX_BOUND_FILE_BYTES,
            "bound_file_invalid",
        )
        digest = hashlib.sha256()
        total = 0
        while True:
            block = os.read(descriptor, 1024 * 1024)
            if not block:
                break
            total += len(block)
            require(total <= before.st_size, "bound_file_changed")
            digest.update(block)
        after = os.fstat(descriptor)
        require(
            total == before.st_size
            and after.st_size == before.st_size
            and after.st_mtime_ns == before.st_mtime_ns
            and (after.st_dev, after.st_ino) == (before.st_dev, before.st_ino),
            "bound_file_changed",
        )
        return digest.hexdigest()
    except OSError as error:
        raise HarnessError("bound_file_read_failed") from error
    finally:
        os.close(descriptor)


def _require_private_attestation(path: Path) -> None:
    require(path.is_absolute() and not path.is_symlink(), "attestation_path_invalid")
    try:
        metadata = path.lstat()
    except OSError as error:
        raise HarnessError("attestation_unavailable") from error
    require(
        stat.S_ISREG(metadata.st_mode)
        and metadata.st_uid == os.geteuid()
        and stat.S_IMODE(metadata.st_mode) == 0o600,
        "attestation_file_invalid",
    )


def _controlled_fixture_sha256(path: Path) -> str:
    require(path.is_absolute(), "controlled_fixture_invalid")
    try:
        metadata = path.lstat()
    except OSError as error:
        raise HarnessError("controlled_fixture_unavailable") from error
    require(
        stat.S_ISREG(metadata.st_mode)
        and not stat.S_ISLNK(metadata.st_mode)
        and metadata.st_mode & 0o111 != 0
        and metadata.st_mode & 0o022 == 0,
        "controlled_fixture_invalid",
    )
    return _sha256_regular_file(path)


def _require_output_target(path: Path) -> None:
    require(path.is_absolute() and path.name not in {"", ".", ".."},
            "output_target_invalid")
    require(path.parent.is_dir() and not path.parent.is_symlink(),
            "output_parent_invalid")
    if path.exists() or path.is_symlink():
        require(path.is_file() and not path.is_symlink(), "output_target_invalid")
    temporary = path.with_suffix(path.suffix + ".tmp")
    require(not temporary.exists() and not temporary.is_symlink(),
            "output_temporary_exists")


def _attestation_api() -> tuple[Callable[[Path], Any], Callable[..., None]]:
    gentoo_tests = Path(__file__).resolve().parents[1] / "gentoo"
    gentoo_path = os.fspath(gentoo_tests)
    if gentoo_path not in sys.path:
        sys.path.insert(0, gentoo_path)
    from pd2_installed_artifact_attestation import (  # pylint: disable=import-outside-toplevel
        load_attestation,
        validate_attested_files,
    )

    return load_attestation, validate_attested_files


def _driver_path() -> Path:
    path = Path(__file__)
    if not path.is_absolute():
        path = Path.cwd() / path
    require(not path.is_symlink(), "demo_driver_invalid")
    return path


def _require_reviewed_live_tools() -> None:
    for path in REVIEWED_LIVE_TOOL_PATHS.values():
        require(path.is_absolute(), "live_tool_path_invalid")
        try:
            metadata = path.lstat()
        except OSError as error:
            raise HarnessError("live_tool_unavailable") from error
        require(
            stat.S_ISREG(metadata.st_mode)
            and not stat.S_ISLNK(metadata.st_mode)
            and metadata.st_mode & 0o111 != 0
            and metadata.st_mode & 0o022 == 0
            and os.access(path, os.X_OK),
            "live_tool_invalid",
        )


def _build_live_port(controlled_window_fixture: Path) -> HarnessPort:
    """Construct the production adapter from reviewed absolute VM tools."""

    _require_reviewed_live_tools()
    # Imported lazily so evidence validation remains usable on non-X11 hosts.
    from pd2_live_xorg import (  # pylint: disable=import-outside-toplevel
        LiveXorgConfig,
        LiveXorgHarnessPort,
        SystemAccessibilityOperations,
        SystemOperations,
    )

    config = LiveXorgConfig(
        xorg=REVIEWED_LIVE_TOOL_PATHS["xorg"],
        xrandr=REVIEWED_LIVE_TOOL_PATHS["xrandr"],
        openbox=REVIEWED_LIVE_TOOL_PATHS["openbox"],
        xprop=REVIEWED_LIVE_TOOL_PATHS["xprop"],
        xwininfo=REVIEWED_LIVE_TOOL_PATHS["xwininfo"],
        dbus_daemon=REVIEWED_LIVE_TOOL_PATHS["dbus_daemon"],
        installed_executables=dict(zip(EXECUTABLE_NAMES, INSTALLED_EXECUTABLE_PATHS)),
        xdotool=REVIEWED_LIVE_TOOL_PATHS["xdotool"],
        xwd=REVIEWED_LIVE_TOOL_PATHS["xwd"],
        controlled_window_fixture=controlled_window_fixture,
    )
    return LiveXorgHarnessPort(
        config,
        operations=SystemOperations(),
        accessibility=SystemAccessibilityOperations(),
    )


def execute_installed_demonstration(
    attestation_path: Path,
    controlled_window_fixture: Path,
    output: Path,
    *,
    driver_path: Path | None = None,
    attestation_api: tuple[Callable[[Path], Any], Callable[..., None]] | None = None,
    hash_file: Callable[[Path], str] = _sha256_regular_file,
    port_factory: Callable[[Path], HarnessPort] = _build_live_port,
) -> None:
    """Validate one preflight, execute the live adapter, and publish evidence."""

    _require_output_target(output)
    _require_private_attestation(attestation_path)
    fixture_hash = _controlled_fixture_sha256(controlled_window_fixture)
    selected_driver = driver_path or _driver_path()
    loader, validate_files = attestation_api or _attestation_api()
    attestation = loader(attestation_path)
    validate_files(
        attestation,
        INSTALLED_EXECUTABLE_PATHS,
        selected_driver,
        hash_file,
    )

    source = attestation.get("source") if isinstance(attestation, dict) else None
    revision = source.get("revision") if isinstance(source, dict) else None
    require(isinstance(revision, str), "attestation_source_revision_invalid")
    installed = attestation.get("installed")
    require(isinstance(installed, dict), "attestation_installed_set_invalid")
    records = installed.get("executables")
    require(isinstance(records, list) and len(records) == len(EXECUTABLE_NAMES),
            "attestation_installed_set_invalid")
    expected_hashes = {
        name: record.get("sha256") if isinstance(record, dict) else None
        for name, record in zip(EXECUTABLE_NAMES, records)
    }

    port = port_factory(controlled_window_fixture)
    require(
        port.installed_executable_hashes() == expected_hashes,
        "attested_executable_set_changed",
    )
    document = run_harness(revision, port)

    # Detect a changed source driver, fixture, or installed binary before the
    # success record becomes durable. The live port has already completed its
    # mandatory cleanup by this point.
    validate_files(
        attestation,
        INSTALLED_EXECUTABLE_PATHS,
        selected_driver,
        hash_file,
    )
    require(
        _controlled_fixture_sha256(controlled_window_fixture) == fixture_hash,
        "controlled_fixture_changed",
    )
    write_evidence(output, document)


def main(
    argv: Sequence[str] | None = None, *, driver_path: Path | None = None
) -> int:
    parser = _RedactedArgumentParser(description=__doc__)
    parser.add_argument("--validate-evidence", type=Path)
    parser.add_argument("--attestation", type=Path)
    parser.add_argument("--controlled-window-fixture", type=Path)
    parser.add_argument("--output", type=Path)
    try:
        arguments = parser.parse_args(argv)
        live_values = (
            arguments.attestation,
            arguments.controlled_window_fixture,
            arguments.output,
        )
        if arguments.validate_evidence is not None:
            require(not any(live_values), "arguments_invalid")
            document = load_evidence(arguments.validate_evidence)
            validate_evidence(document)
        else:
            require(all(value is not None for value in live_values), "arguments_invalid")
            execute_installed_demonstration(
                arguments.attestation,
                arguments.controlled_window_fixture,
                arguments.output,
                driver_path=driver_path,
            )
    except HarnessError as error:
        print(f"PD2 installed demonstration failed closed: {error}", file=sys.stderr)
        return 2
    except Exception:
        print(
            "PD2 installed demonstration failed closed: live_execution_failed",
            file=sys.stderr,
        )
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
