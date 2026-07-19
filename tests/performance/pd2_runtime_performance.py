#!/usr/bin/env python3
"""Collect bounded live WP15 timings through the installed demonstration adapter."""

from __future__ import annotations

import argparse
import json
import os
import re
import stat
import sys
import tempfile
import time
from collections.abc import Callable, Mapping, Sequence
from pathlib import Path
from typing import Any, NoReturn, Protocol


ROOT = Path(__file__).resolve().parents[2]
INTEGRATION = ROOT / "tests" / "integration"
MAX_EVIDENCE_BYTES = 65_536
MINIMUM_ITERATIONS = 3
MAXIMUM_ITERATIONS = 10
CLOCK_METHOD = "python_monotonic_ns_and_linux_boottime_proc_start_ticks"
ENDPOINTS = (
    "session_ready",
    "shell_complete_presentation",
    "launcher_acknowledgement",
    "notification_method_latency",
    "notification_center_open",
    "quick_settings_open",
    "profile_switch",
    "dnd_switch",
    "output_add",
    "output_remove",
    "common_action_acknowledgement",
)
TIMED_STEPS = {
    "invoke_launcher_through_shellctl": "launcher_acknowledgement",
    "open_notification_center": "notification_center_open",
    "toggle_do_not_disturb": "dnd_switch",
    "open_quick_settings": "quick_settings_open",
    "switch_lustre_to_forge": "profile_switch",
    "remove_current_primary_output": "output_remove",
    "readd_removed_output": "output_add",
}
COMMON_ACTION_STEPS = {
    "activate_one_window",
    "minimize_one_window",
    "reactivate_one_window",
}


class RuntimePerformanceError(RuntimeError):
    """One closed live runtime performance failure."""


class ClosedArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> NoReturn:
        del message
        raise RuntimePerformanceError("arguments_invalid")


class LivePort(Protocol):
    def installed_executable_hashes(self) -> Mapping[str, str]: ...

    def run_step(self, phase: str, identifier: str) -> object: ...

    def shell_complete_presentation_ns(self) -> int: ...

    def notification_method_latency_ns(self) -> int: ...

    def cleanup(self, phase: str) -> object: ...


def require(condition: bool, identifier: str) -> None:
    if not condition:
        raise RuntimePerformanceError(identifier)


def _pd2_demo() -> Any:
    path = os.fspath(INTEGRATION)
    if path not in sys.path:
        sys.path.insert(0, path)
    import pd2_demo  # pylint: disable=import-outside-toplevel

    return pd2_demo


def _valid_observation(value: object) -> bool:
    return (
        getattr(value, "observed", None) is True
        and getattr(value, "passed", None) is True
        and getattr(value, "result", None) == "expected_state_observed"
    )


def _expected_hashes(document: Mapping[str, Any]) -> dict[str, str]:
    records = document.get("installed_executables")
    require(isinstance(records, list), "installed_demo_evidence_invalid")
    result: dict[str, str] = {}
    for record in records:
        require(
            isinstance(record, dict)
            and set(record) == {"name", "sha256"}
            and isinstance(record["name"], str)
            and isinstance(record["sha256"], str)
            and record["name"] not in result,
            "installed_demo_evidence_invalid",
        )
        result[record["name"]] = record["sha256"]
    return result


def validate_runtime_evidence(document: Any) -> None:
    require(isinstance(document, dict), "runtime_evidence_invalid")
    require(
        set(document)
        == {
            "schema_version",
            "evidence_kind",
            "source_revision",
            "reference_environment_id",
            "method",
            "measurements",
            "redaction",
        },
        "runtime_evidence_invalid",
    )
    require(
        document["schema_version"] == 1
        and document["evidence_kind"] == "pd2_live_runtime_performance"
        and isinstance(document["source_revision"], str)
        and re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", document["source_revision"])
        is not None
        and isinstance(document["reference_environment_id"], str)
        and re.fullmatch(
            r"[a-z0-9][a-z0-9._-]{0,63}", document["reference_environment_id"]
        )
        is not None,
        "runtime_evidence_identity_invalid",
    )
    method = document["method"]
    require(
        method
        == {
            "clock": CLOCK_METHOD,
            "sample_isolation": "fresh_installed_session_per_trial",
            "endpoint_observation": "exact_reply_shell_start_or_workflow_postcondition",
            "installed_demo_binding": "validated_exact_revision_and_executable_hashes",
            "iterations": method.get("iterations") if isinstance(method, dict) else None,
        }
        and isinstance(method["iterations"], int)
        and MINIMUM_ITERATIONS <= method["iterations"] <= MAXIMUM_ITERATIONS,
        "runtime_evidence_method_invalid",
    )
    measurements = document["measurements"]
    require(
        isinstance(measurements, list) and len(measurements) == len(ENDPOINTS),
        "runtime_evidence_measurements_invalid",
    )
    for endpoint, record in zip(ENDPOINTS, measurements):
        require(
            isinstance(record, dict)
            and set(record) == {"name", "samples_ns"}
            and record["name"] == endpoint
            and isinstance(record["samples_ns"], list),
            "runtime_evidence_measurements_invalid",
        )
        samples = record["samples_ns"]
        expected_count = (
            method["iterations"] * len(COMMON_ACTION_STEPS)
            if endpoint == "common_action_acknowledgement"
            else method["iterations"]
        )
        require(
            len(samples) == expected_count
            and all(isinstance(sample, int) and 0 <= sample <= 60_000_000_000 for sample in samples),
            "runtime_evidence_samples_invalid",
        )
    require(
        document["redaction"]
        == {
            "contains_filesystem_paths": False,
            "contains_private_content": False,
            "contains_process_ids": False,
            "contains_x11_window_ids": False,
            "diagnostics_are_closed_ids": True,
        },
        "runtime_evidence_redaction_invalid",
    )


def collect_runtime_evidence(
    source_revision: str,
    reference_environment_id: str,
    installed_demo: Mapping[str, Any],
    controlled_fixture: Path,
    iterations: int,
    *,
    port_factory: Callable[[Path], LivePort],
    clock_ns: Callable[[], int] = time.monotonic_ns,
) -> dict[str, Any]:
    """Repeat the authoritative installed workflow and retain only durations."""

    demo = _pd2_demo()
    try:
        demo.validate_evidence(installed_demo)
    except Exception as error:
        raise RuntimePerformanceError("installed_demo_evidence_invalid") from error
    require(installed_demo.get("source_revision") == source_revision,
            "installed_demo_revision_mismatch")
    require(
        re.fullmatch(r"[a-z0-9][a-z0-9._-]{0,63}", reference_environment_id)
        is not None,
        "reference_environment_invalid",
    )
    require(MINIMUM_ITERATIONS <= iterations <= MAXIMUM_ITERATIONS,
            "iteration_count_invalid")
    expected_hashes = _expected_hashes(installed_demo)
    fixture_digest = demo._controlled_fixture_sha256(controlled_fixture)
    samples: dict[str, list[int]] = {endpoint: [] for endpoint in ENDPOINTS}
    steps = demo.WORKFLOW_STEPS[: demo.WORKFLOW_STEPS.index("restart_settingsd")]

    for _trial in range(iterations):
        port = port_factory(controlled_fixture)
        failure: Exception | None = None
        session_started: int | None = None
        try:
            require(
                dict(port.installed_executable_hashes()) == expected_hashes,
                "installed_executable_set_changed",
            )
            for identifier in steps:
                if identifier == "start_installed_prismdrake_session":
                    session_started = clock_ns()
                step_started = clock_ns()
                observation = port.run_step("installed_workflow", identifier)
                finished = clock_ns()
                require(_valid_observation(observation), "runtime_step_invalid")
                duration = finished - step_started
                require(duration >= 0, "runtime_clock_invalid")
                endpoint = TIMED_STEPS.get(identifier)
                if endpoint is not None:
                    samples[endpoint].append(duration)
                if identifier in COMMON_ACTION_STEPS:
                    samples["common_action_acknowledgement"].append(duration)
                if identifier == "verify_component_readiness":
                    require(session_started is not None, "runtime_step_order_invalid")
                    samples["session_ready"].append(finished - session_started)
                elif identifier == "verify_panel_policy_both_outputs":
                    shell_duration = port.shell_complete_presentation_ns()
                    require(
                        type(shell_duration) is int
                        and 0 <= shell_duration <= 60_000_000_000,
                        "shell_presentation_latency_invalid",
                    )
                    samples["shell_complete_presentation"].append(shell_duration)
                elif identifier == "create_normal_notification":
                    notify_duration = port.notification_method_latency_ns()
                    require(
                        type(notify_duration) is int
                        and 0 <= notify_duration <= 60_000_000_000,
                        "notification_method_latency_invalid",
                    )
                    samples["notification_method_latency"].append(notify_duration)
        except Exception as error:
            failure = error
        try:
            cleanup = port.cleanup("installed_workflow")
            require(_valid_observation(cleanup), "runtime_cleanup_failed")
        except Exception as error:
            raise RuntimePerformanceError("runtime_cleanup_failed") from error
        if failure is not None:
            if isinstance(failure, RuntimePerformanceError):
                raise failure
            raise RuntimePerformanceError("runtime_collection_failed") from failure
        require(
            dict(port.installed_executable_hashes()) == expected_hashes,
            "installed_executable_set_changed",
        )

    require(
        demo._controlled_fixture_sha256(controlled_fixture) == fixture_digest,
        "controlled_fixture_changed",
    )
    document = {
        "schema_version": 1,
        "evidence_kind": "pd2_live_runtime_performance",
        "source_revision": source_revision,
        "reference_environment_id": reference_environment_id,
        "method": {
            "clock": CLOCK_METHOD,
            "sample_isolation": "fresh_installed_session_per_trial",
            "endpoint_observation": "exact_reply_shell_start_or_workflow_postcondition",
            "installed_demo_binding": "validated_exact_revision_and_executable_hashes",
            "iterations": iterations,
        },
        "measurements": [
            {"name": endpoint, "samples_ns": samples[endpoint]}
            for endpoint in ENDPOINTS
        ],
        "redaction": {
            "contains_filesystem_paths": False,
            "contains_private_content": False,
            "contains_process_ids": False,
            "contains_x11_window_ids": False,
            "diagnostics_are_closed_ids": True,
        },
    }
    validate_runtime_evidence(document)
    return document


def load_json(path: Path) -> Any:
    try:
        raw = path.read_bytes()
    except OSError as error:
        raise RuntimePerformanceError("evidence_read_failed") from error
    require(0 < len(raw) <= MAX_EVIDENCE_BYTES, "evidence_size_invalid")
    try:
        return json.loads(raw.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise RuntimePerformanceError("evidence_json_invalid") from error


def write_evidence(path: Path, document: Mapping[str, Any]) -> None:
    validate_runtime_evidence(dict(document))
    require(path.is_absolute() and path.parent.is_dir() and not path.exists(),
            "output_path_invalid")
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
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
        require(stat.S_IMODE(path.stat().st_mode) == 0o600,
                "output_permissions_invalid")
    except OSError as error:
        raise RuntimePerformanceError("output_write_failed") from error
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        if temporary is not None:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass


def main(argv: Sequence[str] | None = None) -> int:
    parser = ClosedArgumentParser(add_help=False)
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--environment-id", required=True)
    parser.add_argument("--installed-demo-evidence", required=True, type=Path)
    parser.add_argument("--controlled-window-fixture", required=True, type=Path)
    parser.add_argument("--iterations", default="3")
    parser.add_argument("--output", required=True, type=Path)
    try:
        options = parser.parse_args(argv)
        require(
            re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", options.source_revision)
            is not None,
            "source_revision_invalid",
        )
        require(options.iterations.isascii() and options.iterations.isdecimal(),
                "iteration_count_invalid")
        iterations = int(options.iterations)
        demo = _pd2_demo()
        document = collect_runtime_evidence(
            options.source_revision,
            options.environment_id,
            load_json(options.installed_demo_evidence),
            options.controlled_window_fixture,
            iterations,
            port_factory=demo._build_live_port,
        )
        write_evidence(options.output, document)
        return 0
    except (RuntimePerformanceError, OSError) as error:
        identifier = (
            str(error)
            if isinstance(error, RuntimePerformanceError)
            else "external_operation_failed"
        )
        print(f"pd2_runtime_performance: {identifier}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
