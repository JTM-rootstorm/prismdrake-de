#!/usr/bin/env python3
"""Collect bound live observations for the strict PD2 WP15 evidence contract.

The collector owns orchestration, binding, reviewed-skip policy, cleanup, and
private atomic output. It never infers a pass or manufactures a measurement:
every result comes from an injected live port or a bounded command adapter.
"""

from __future__ import annotations

import copy
import json
import math
import os
import re
import stat
import subprocess
import tempfile
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Protocol

from pd2_evidence_contract import (
    INTERNATIONALIZATION_CHECKS,
    MATRIX_CHECKS,
    SURFACES,
    EvidenceError,
    validate_evidence_document,
)


MAX_OBSERVER_OUTPUT_BYTES = 65_536
LIVE_METHOD_ID = "pd2-live-v1"
ORDINARY_OBSERVER_TIMEOUT_SECONDS = 15.0
PERFORMANCE_OBSERVER_TIMEOUT_SECONDS = 75.0
INTERNATIONALIZATION_OBSERVER_TIMEOUT_SECONDS = 195.0
MAXIMUM_OBSERVER_TIMEOUT_SECONDS = INTERNATIONALIZATION_OBSERVER_TIMEOUT_SECONDS
ROLE_OBSERVER_TIMEOUT_SECONDS = {
    "environment": ORDINARY_OBSERVER_TIMEOUT_SECONDS,
    "performance": PERFORMANCE_OBSERVER_TIMEOUT_SECONDS,
    "accessibility": ORDINARY_OBSERVER_TIMEOUT_SECONDS,
    "internationalization": INTERNATIONALIZATION_OBSERVER_TIMEOUT_SECONDS,
    "cleanup": ORDINARY_OBSERVER_TIMEOUT_SECONDS,
}
REVISION_PATTERN = re.compile(r"[0-9a-f]{40}|[0-9a-f]{64}")
SKIP_CODES = {
    "not_applicable",
    "dependency_unavailable",
    "reference_environment_unsupported",
    "live_adapter_unimplemented",
    "runtime_surface_unavailable",
}

PERFORMANCE_GROUPS = (
    "session_ready",
    "shell_complete_presentation",
    "launcher_acknowledgement",
    "search_response",
    "catalog_refresh",
    "icon_decode_cache",
    "wallpaper_decode_publication",
    "notification_method_latency",
    "notification_center_open",
    "quick_settings_open",
    "profile_switch",
    "dnd_switch",
    "output_reconciliation",
    "idle_wakeups",
    "common_action_acknowledgement",
)

_SIMPLE_PERFORMANCE_GROUPS = {
    "session_ready",
    "shell_complete_presentation",
    "launcher_acknowledgement",
    "catalog_refresh",
    "wallpaper_decode_publication",
    "notification_method_latency",
    "notification_center_open",
    "quick_settings_open",
    "profile_switch",
    "dnd_switch",
    "idle_wakeups",
    "common_action_acknowledgement",
}


def _require(condition: bool, identifier: str) -> None:
    if not condition:
        raise EvidenceError(identifier)


@dataclass(frozen=True)
class ObservationBinding:
    """Identity carried by every independently collected live observation."""

    source_revision: str
    reference_environment_id: str
    method_id: str = LIVE_METHOD_ID


@dataclass(frozen=True)
class BoundResult:
    """One result plus the live identity that produced it."""

    binding: ObservationBinding
    result: Mapping[str, Any]


class LiveEvidencePort(Protocol):
    """Injected live operations for every WP15 observation family."""

    def environment(self) -> BoundResult: ...

    def performance(self, group: str, endpoint: str | None) -> BoundResult: ...

    def accessibility(self, surface: str, check: str) -> BoundResult: ...

    def live_atspi(self) -> BoundResult: ...

    def screen_reader_workflow(self) -> BoundResult: ...

    def internationalization(self, check: str) -> BoundResult: ...

    def cleanup(self) -> BoundResult: ...


@dataclass(frozen=True)
class LiveCollectionConfig:
    """Reviewed identity and skip policy for one reference-VM collection."""

    source_revision: str
    reference_environment_id: str
    pd1_reference: Mapping[str, Any]
    search_catalog_sizes: tuple[int, ...]
    reviewed_skips: Mapping[str, str]
    limitations: tuple[str, ...]

    def __post_init__(self) -> None:
        _require(
            REVISION_PATTERN.fullmatch(self.source_revision) is not None
            and set(self.source_revision) != {"0"},
            "live_revision_invalid",
        )
        _require(
            re.fullmatch(r"[a-z0-9][a-z0-9._-]{0,63}", self.reference_environment_id)
            is not None,
            "live_environment_id_invalid",
        )
        _require(
            len(self.search_catalog_sizes) >= 2
            and self.search_catalog_sizes
            == tuple(sorted(set(self.search_catalog_sizes)))
            and all(1 <= size <= 1_000_000 for size in self.search_catalog_sizes),
            "live_search_series_invalid",
        )
        _require(bool(self.limitations), "live_limitations_missing")
        valid_skip_keys = {
            _performance_key(group, endpoint)
            for group in _SIMPLE_PERFORMANCE_GROUPS
            for endpoint in (None,)
        }
        valid_skip_keys.update(
            _performance_key("search_response", str(size))
            for size in self.search_catalog_sizes
        )
        valid_skip_keys.update(
            _performance_key("icon_decode_cache", endpoint)
            for endpoint in ("decode", "cache_hit")
        )
        valid_skip_keys.update(
            _performance_key("output_reconciliation", endpoint)
            for endpoint in ("add", "remove")
        )
        valid_skip_keys.update(
            f"accessibility.{surface}.{check}"
            for surface in SURFACES
            for check in MATRIX_CHECKS
        )
        valid_skip_keys.update(
            {
                "accessibility.live_atspi",
                "accessibility.screen_reader_workflow",
            }
        )
        valid_skip_keys.update(
            f"internationalization.{check}"
            for check in INTERNATIONALIZATION_CHECKS
        )
        for key, reason_code in self.reviewed_skips.items():
            _require(
                key in valid_skip_keys and reason_code in SKIP_CODES,
                "reviewed_skip_policy_invalid",
            )

    @property
    def binding(self) -> ObservationBinding:
        return ObservationBinding(
            self.source_revision, self.reference_environment_id, LIVE_METHOD_ID
        )


def _copy_result(bound: BoundResult, config: LiveCollectionConfig, key: str) -> dict[str, Any]:
    _require(isinstance(bound, BoundResult), "live_observation_type_invalid")
    _require(bound.binding == config.binding, "live_observation_binding_mismatch")
    _require(isinstance(bound.result, Mapping), "live_observation_result_invalid")
    try:
        result = copy.deepcopy(dict(bound.result))
        json.dumps(result, ensure_ascii=True, allow_nan=False)
    except (TypeError, ValueError) as error:
        raise EvidenceError("live_observation_result_invalid") from error

    reviewed_reason = config.reviewed_skips.get(key)
    if result.get("status") == "skipped":
        _require(reviewed_reason is not None, "live_skip_not_reviewed")
        _require(result.get("reviewed") is True, "live_skip_not_reviewed")
        _require(
            result.get("reason_code") == reviewed_reason,
            "live_skip_policy_mismatch",
        )
        _require(result.get("reason_code") in SKIP_CODES, "live_skip_code_invalid")
    else:
        _require(reviewed_reason is None, "reviewed_skip_was_observed")
    return result


def _performance_key(group: str, endpoint: str | None) -> str:
    return f"performance.{group}" + ("" if endpoint is None else f".{endpoint}")


def _collect_performance(
    port: LiveEvidencePort, config: LiveCollectionConfig
) -> dict[str, Any]:
    results: dict[str, Any] = {}
    for group in PERFORMANCE_GROUPS:
        if group in _SIMPLE_PERFORMANCE_GROUPS:
            results[group] = _copy_result(
                port.performance(group, None), config, _performance_key(group, None)
            )
        elif group == "search_response":
            results[group] = [
                {
                    "catalog_size": size,
                    "measurement": _copy_result(
                        port.performance(group, str(size)),
                        config,
                        _performance_key(group, str(size)),
                    ),
                }
                for size in config.search_catalog_sizes
            ]
        elif group == "icon_decode_cache":
            results[group] = {
                endpoint: _copy_result(
                    port.performance(group, endpoint),
                    config,
                    _performance_key(group, endpoint),
                )
                for endpoint in ("decode", "cache_hit")
            }
        elif group == "output_reconciliation":
            results[group] = {
                endpoint: _copy_result(
                    port.performance(group, endpoint),
                    config,
                    _performance_key(group, endpoint),
                )
                for endpoint in ("add", "remove")
            }
        else:
            raise EvidenceError("live_performance_group_invalid")
    _require(tuple(results) == PERFORMANCE_GROUPS, "live_performance_order_invalid")
    return results


def _collect_accessibility(
    port: LiveEvidencePort, config: LiveCollectionConfig
) -> dict[str, Any]:
    batch_operation = getattr(port, "accessibility_matrix", None)
    if callable(batch_operation):
        batch = batch_operation()
        _require(batch.binding == config.binding, "live_observation_binding_mismatch")
        _require(
            isinstance(batch.result, Mapping) and set(batch.result) == {"matrix"},
            "accessibility_batch_invalid",
        )
        raw_matrix = batch.result.get("matrix")
        _require(
            isinstance(raw_matrix, list)
            and len(raw_matrix) == len(SURFACES)
            and all(isinstance(entry, Mapping) for entry in raw_matrix),
            "accessibility_batch_invalid",
        )
        _require(
            [entry.get("surface") for entry in raw_matrix] == list(SURFACES),
            "accessibility_batch_invalid",
        )
        matrix = []
        for entry, surface in zip(raw_matrix, SURFACES):
            _require(
                isinstance(entry, Mapping) and set(entry) == {"surface", "checks"},
                "accessibility_batch_invalid",
            )
            raw_checks = entry.get("checks")
            _require(
                isinstance(raw_checks, Mapping)
                and list(raw_checks) == list(MATRIX_CHECKS),
                "accessibility_batch_invalid",
            )
            checks = {
                check: _copy_result(
                    BoundResult(batch.binding, raw_checks[check]),
                    config,
                    f"accessibility.{surface}.{check}",
                )
                for check in MATRIX_CHECKS
            }
            matrix.append({"surface": surface, "checks": checks})
    else:
        matrix = []
        for surface in SURFACES:
            checks = {
                check: _copy_result(
                    port.accessibility(surface, check),
                    config,
                    f"accessibility.{surface}.{check}",
                )
                for check in MATRIX_CHECKS
            }
            matrix.append({"surface": surface, "checks": checks})
    return {
        "matrix": matrix,
        "live_atspi": _copy_result(
            port.live_atspi(), config, "accessibility.live_atspi"
        ),
        "screen_reader_workflow": _copy_result(
            port.screen_reader_workflow(),
            config,
            "accessibility.screen_reader_workflow",
        ),
    }


def _collect_internationalization(
    port: LiveEvidencePort, config: LiveCollectionConfig
) -> dict[str, Any]:
    return {
        check: _copy_result(
            port.internationalization(check), config, f"internationalization.{check}"
        )
        for check in INTERNATIONALIZATION_CHECKS
    }


def collect_evidence(
    config: LiveCollectionConfig, port: LiveEvidencePort
) -> dict[str, Any]:
    """Collect the complete WP15 contract and always verify owned cleanup."""

    document: dict[str, Any] | None = None
    failure: Exception | None = None
    try:
        environment = _copy_result(port.environment(), config, "environment")
        _require(
            environment.get("kind") == "reference_vm"
            and environment.get("reference_environment_id")
            == config.reference_environment_id,
            "live_environment_mismatch",
        )
        version = environment.get("qt_version")
        _require(
            isinstance(version, str)
            and re.fullmatch(r"[0-9]+[.][0-9]+[.][0-9]+", version) is not None,
            "live_qt_version_invalid",
        )
        major, minor, _patch = (int(part) for part in version.split("."))
        _require((major, minor) >= (6, 11), "live_qt_floor_not_met")
        document = {
            "schema_version": 1,
            "evidence_kind": "pd2_performance_accessibility",
            "evidence_purpose": "reference_measurement",
            "release_budget": False,
            "source_revision": config.source_revision,
            "pd1_reference": copy.deepcopy(dict(config.pd1_reference)),
            "environment": environment,
            "method": {
                "collection_mode": "injected_measurements",
                "clock": "monotonic",
                "sample_isolation": "fresh_session_per_series",
                "description": "Bound live reference VM observers with fresh isolated series",
            },
            "performance": _collect_performance(port, config),
            "accessibility": _collect_accessibility(port, config),
            "internationalization": _collect_internationalization(port, config),
            "limitations": list(config.limitations),
            "redaction": {
                "contains_filesystem_paths": False,
                "contains_private_content": False,
                "contains_network_or_bus_addresses": False,
                "contains_process_ids": False,
                "contains_x11_window_ids": False,
                "diagnostics_are_closed_ids": True,
            },
        }
        validate_evidence_document(document)
    except Exception as error:
        failure = error

    try:
        cleanup = port.cleanup()
        _require(cleanup.binding == config.binding, "live_cleanup_binding_mismatch")
        _require(dict(cleanup.result) == {"clean": True}, "live_cleanup_failed")
    except Exception as cleanup_error:
        raise EvidenceError("live_cleanup_failed") from cleanup_error
    if failure is not None:
        if isinstance(failure, EvidenceError):
            raise failure
        raise EvidenceError("live_collection_failed") from failure
    _require(document is not None, "live_collection_failed")
    return document


def write_evidence(path: Path, document: Mapping[str, Any]) -> None:
    """Validate and atomically publish one new mode-0600 evidence artifact."""

    _require(path.is_absolute(), "live_output_path_invalid")
    _require(path.parent.is_dir() and not path.exists(), "live_output_path_invalid")
    validate_evidence_document(dict(document))
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
            json.dump(document, stream, indent=2, ensure_ascii=True, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.link(temporary, path, follow_symlinks=False)
        temporary.unlink()
        temporary = None
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
        _require(stat.S_IMODE(path.stat().st_mode) == 0o600, "live_output_mode_invalid")
    except OSError as error:
        raise EvidenceError("live_output_write_failed") from error
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        if temporary is not None:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass


@dataclass(frozen=True)
class ObserverCommand:
    """One exact executable plus fixed role-selection arguments."""

    executable: Path
    prefix_arguments: tuple[str, ...] = ()


@dataclass(frozen=True)
class ObserverCommands:
    """Exact live observer executables; each emits one bound JSON envelope."""

    environment: Path | ObserverCommand
    performance: Path | ObserverCommand
    accessibility: Path | ObserverCommand
    internationalization: Path | ObserverCommand
    cleanup: Path | ObserverCommand

    @classmethod
    def unified(cls, executable: Path) -> "ObserverCommands":
        """Select all protocol roles through one composable observer executable."""

        return cls(
            environment=ObserverCommand(executable, ("--role", "environment")),
            performance=ObserverCommand(executable, ("--role", "performance")),
            accessibility=ObserverCommand(executable, ("--role", "accessibility")),
            internationalization=ObserverCommand(
                executable, ("--role", "internationalization")
            ),
            cleanup=ObserverCommand(executable, ("--role", "cleanup")),
        )


def _observer_command(value: Path | ObserverCommand) -> ObserverCommand:
    return value if isinstance(value, ObserverCommand) else ObserverCommand(value)


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        _require(key not in result, "live_observer_json_invalid")
        result[key] = value
    return result


class CommandLiveEvidencePort:
    """Run exact, role-bounded live observer commands without a shell."""

    def __init__(
        self,
        commands: ObserverCommands,
        environment: Mapping[str, str] | None = None,
        timeout_seconds: float | None = None,
    ) -> None:
        if timeout_seconds is not None:
            _require(
                not isinstance(timeout_seconds, bool)
                and isinstance(timeout_seconds, (int, float))
                and math.isfinite(timeout_seconds)
                and 0 < timeout_seconds <= MAXIMUM_OBSERVER_TIMEOUT_SECONDS,
                "live_observer_timeout_invalid",
            )
        for command_value in (
            commands.environment,
            commands.performance,
            commands.accessibility,
            commands.internationalization,
            commands.cleanup,
        ):
            command = _observer_command(command_value)
            executable = command.executable
            _require(
                executable.is_absolute()
                and executable.is_file()
                and os.access(executable, os.X_OK),
                "live_observer_executable_invalid",
            )
            _require(
                all(
                    isinstance(argument, str)
                    and argument
                    and "\x00" not in argument
                    for argument in command.prefix_arguments
                ),
                "live_observer_arguments_invalid",
            )
        self._commands = commands
        self._environment = dict(environment or {})
        self._timeouts = (
            dict(ROLE_OBSERVER_TIMEOUT_SECONDS)
            if timeout_seconds is None
            else dict.fromkeys(ROLE_OBSERVER_TIMEOUT_SECONDS, float(timeout_seconds))
        )

    def _run(
        self,
        role: str,
        command_value: Path | ObserverCommand,
        arguments: Sequence[str],
    ) -> BoundResult:
        _require(role in self._timeouts, "live_observer_role_invalid")
        command = _observer_command(command_value)
        try:
            completed = subprocess.run(
                [
                    str(command.executable),
                    *command.prefix_arguments,
                    *arguments,
                ],
                env=self._environment,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=self._timeouts[role],
                check=False,
            )
        except (OSError, subprocess.SubprocessError) as error:
            raise EvidenceError("live_observer_execution_failed") from error
        _require(completed.returncode == 0, "live_observer_failed")
        _require(not completed.stderr, "live_observer_diagnostics_present")
        _require(
            0 < len(completed.stdout) <= MAX_OBSERVER_OUTPUT_BYTES,
            "live_observer_output_invalid",
        )
        try:
            envelope = json.loads(
                completed.stdout.decode("utf-8"),
                object_pairs_hook=_reject_duplicate_keys,
                parse_constant=lambda _value: (_ for _ in ()).throw(ValueError()),
            )
        except (UnicodeError, ValueError, json.JSONDecodeError) as error:
            raise EvidenceError("live_observer_json_invalid") from error
        _require(
            isinstance(envelope, dict) and set(envelope) == {"binding", "result"},
            "live_observer_envelope_invalid",
        )
        binding = envelope["binding"]
        _require(
            isinstance(binding, dict)
            and set(binding)
            == {"source_revision", "reference_environment_id", "method_id"},
            "live_observer_binding_invalid",
        )
        _require(isinstance(envelope["result"], dict), "live_observer_result_invalid")
        return BoundResult(
            ObservationBinding(
                binding["source_revision"],
                binding["reference_environment_id"],
                binding["method_id"],
            ),
            envelope["result"],
        )

    def environment(self) -> BoundResult:
        return self._run("environment", self._commands.environment, ())

    def performance(self, group: str, endpoint: str | None) -> BoundResult:
        arguments = ["--group", group]
        if endpoint is not None:
            arguments.extend(("--endpoint", endpoint))
        return self._run("performance", self._commands.performance, arguments)

    def accessibility(self, surface: str, check: str) -> BoundResult:
        return self._run(
            "accessibility",
            self._commands.accessibility,
            ("--surface", surface, "--check", check),
        )

    def accessibility_matrix(self) -> BoundResult:
        return self._run("accessibility", self._commands.accessibility, ("--matrix",))

    def live_atspi(self) -> BoundResult:
        return self._run(
            "accessibility", self._commands.accessibility, ("--global", "live_atspi")
        )

    def screen_reader_workflow(self) -> BoundResult:
        return self._run(
            "accessibility",
            self._commands.accessibility,
            ("--global", "screen_reader_workflow"),
        )

    def internationalization(self, check: str) -> BoundResult:
        return self._run(
            "internationalization",
            self._commands.internationalization,
            ("--check", check),
        )

    def cleanup(self) -> BoundResult:
        return self._run("cleanup", self._commands.cleanup, ())
