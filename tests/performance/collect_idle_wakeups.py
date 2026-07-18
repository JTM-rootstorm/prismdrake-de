#!/usr/bin/env python3
"""Collect one filtered scheduler-wakeup trial for exact Prismdrake processes."""

from __future__ import annotations

import ctypes
import json
import os
import re
import select
import signal
import subprocess
import sys
import time
from pathlib import Path

from collect_startup_to_panel import (
    InotifyRuntimeTree,
    direct_child_pids,
    enumerate_mapped_docks,
)

from collector_common import (
    ClosedArgumentParser,
    CollectorError,
    environment_id,
    executable,
    positive_integer,
    redaction_contract,
    revision,
)


PERF_LINE = re.compile(r"^([0-9]+);[^;]*;(sched:sched_wakeup(?:_new)?);.*$")
ROLES = ("session", "settingsd", "shell")
MAXIMUM_THREADS_PER_COMPONENT = 256
PR_SET_CHILD_SUBREAPER = 36
COLLECTOR_SHUTDOWN_SECONDS = 2


def parse_arguments() -> object:
    parser = ClosedArgumentParser(add_help=False)
    parser.add_argument("--perf", required=True, type=executable)
    parser.add_argument("--sleep", required=True, type=executable)
    parser.add_argument("--session-pid", required=True)
    parser.add_argument("--settingsd-pid", required=True)
    parser.add_argument("--shell-pid", required=True)
    parser.add_argument("--session-executable", required=True, type=executable)
    parser.add_argument("--settingsd-executable", required=True, type=executable)
    parser.add_argument("--shell-executable", required=True, type=executable)
    parser.add_argument("--xprop", required=True, type=executable)
    parser.add_argument("--runtime-directory", required=True)
    parser.add_argument("--revision", required=True, type=revision)
    parser.add_argument("--environment-id", required=True, type=environment_id)
    parser.add_argument("--duration-seconds", default="60")
    options = parser.parse_args()
    options.duration_seconds = positive_integer(options.duration_seconds, 300)
    if options.duration_seconds != 60 and os.environ.get("PRISMDRAKE_PERFORMANCE_TESTING") != "1":
        raise CollectorError("invalid_duration")
    for role in ROLES:
        value = positive_integer(getattr(options, f"{role}_pid"), 2**31 - 1)
        setattr(options, f"{role}_pid", value)
    pids = [getattr(options, f"{role}_pid") for role in ROLES]
    if len(set(pids)) != len(pids):
        raise CollectorError("duplicate_component_pid")
    runtime = Path(options.runtime_directory)
    if not runtime.is_absolute() or not runtime.is_dir() or runtime.is_symlink():
        raise CollectorError("invalid_runtime_directory")
    options.runtime_directory = runtime
    return options


def verify_executable(pid: int, expected: Path) -> None:
    try:
        if not os.path.samefile(Path("/proc") / str(pid) / "exe", expected):
            raise CollectorError("component_executable_mismatch")
    except OSError as error:
        raise CollectorError("component_executable_unavailable") from error


def verify_components(options: object) -> None:
    for role in ROLES:
        verify_executable(
            getattr(options, f"{role}_pid"), getattr(options, f"{role}_executable")
        )


def pidfd_alive(descriptor: int) -> bool:
    poller = select.poll()
    poller.register(descriptor, select.POLLIN | select.POLLERR | select.POLLHUP)
    return not poller.poll(0)


def open_component_pidfds(options: object) -> dict[str, int]:
    descriptors: dict[str, int] = {}
    try:
        for role in ROLES:
            descriptors[role] = os.pidfd_open(getattr(options, f"{role}_pid"), 0)
    except OSError as error:
        close_pidfds(descriptors)
        raise CollectorError("component_identity_unavailable") from error
    return descriptors


def close_pidfds(descriptors: dict[str, int]) -> None:
    for descriptor in descriptors.values():
        try:
            os.close(descriptor)
        except OSError:
            pass


def verify_identity_boundary(options: object, descriptors: dict[str, int]) -> None:
    for role in ROLES:
        if not pidfd_alive(descriptors[role]):
            raise CollectorError("component_identity_changed")
    verify_components(options)


def verify_supervised_startup_endpoint(options: object) -> None:
    if "DISPLAY" not in os.environ or "DBUS_SESSION_BUS_ADDRESS" not in os.environ:
        raise CollectorError("isolated_display_or_bus_missing")
    children = direct_child_pids(options.session_pid)
    if children != {options.settingsd_pid, options.shell_pid}:
        raise CollectorError("supervised_component_tree_invalid")
    observer = InotifyRuntimeTree(options.runtime_directory)
    try:
        ready, safe_mode = observer.state()
    finally:
        observer.close()
    if not ready or safe_mode:
        raise CollectorError("validated_startup_endpoint_missing")
    docks = enumerate_mapped_docks(options.xprop, os.environ.copy())
    if len(docks) != 1 or docks[0].process_id != options.shell_pid:
        raise CollectorError("validated_startup_endpoint_missing")


def enable_child_subreaper() -> None:
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0:
        raise CollectorError("subreaper_unavailable")


def thread_ids(pid: int) -> tuple[int, ...]:
    task_directory = Path("/proc") / str(pid) / "task"
    try:
        values = tuple(sorted(int(entry.name) for entry in task_directory.iterdir()))
    except (OSError, ValueError) as error:
        raise CollectorError("process_threads_unavailable") from error
    if not values or len(values) > MAXIMUM_THREADS_PER_COMPONENT:
        raise CollectorError("invalid_thread_count")
    return values


def event_filter(values: tuple[int, ...]) -> str:
    return " || ".join(f"pid == {value}" for value in values)


def perf_command(options: object, values: tuple[int, ...]) -> list[str]:
    target_filter = event_filter(values)
    return [
        str(options.perf),
        "stat",
        "-a",
        "--no-big-num",
        "-x",
        ";",
        "-e",
        "sched:sched_wakeup",
        "--filter",
        target_filter,
        "-e",
        "sched:sched_wakeup_new",
        "--filter",
        target_filter,
        "--",
        str(options.sleep),
        str(options.duration_seconds),
    ]


def parse_perf(stderr: str) -> tuple[int, int]:
    counts: dict[str, int] = {}
    for line in stderr.splitlines():
        match = PERF_LINE.fullmatch(line.strip())
        if match:
            counts[match.group(2)] = int(match.group(1))
        elif "<not supported>" in line or "<not counted>" in line:
            raise CollectorError("scheduler_tracepoint_unavailable")
    if set(counts) != {"sched:sched_wakeup", "sched:sched_wakeup_new"}:
        raise CollectorError("invalid_perf_output")
    return counts["sched:sched_wakeup"], counts["sched:sched_wakeup_new"]


def process_group_exists(group_id: int) -> bool:
    try:
        os.killpg(group_id, 0)
        return True
    except ProcessLookupError:
        return False


def _signal_process_groups(
    processes: list[subprocess.Popen[str]], signal_number: signal.Signals
) -> bool:
    cleanup_failed = False
    for process in processes:
        try:
            if process_group_exists(process.pid):
                os.killpg(process.pid, signal_number)
        except (OSError, CollectorError):
            cleanup_failed = True
    return not cleanup_failed


def _reap_adopted_group(group_id: int, deadline: float) -> bool:
    while True:
        try:
            child, _status = os.waitpid(-group_id, os.WNOHANG)
        except ChildProcessError:
            child = 0
        except OSError:
            return False
        if child == 0:
            if not process_group_exists(group_id):
                return True
            if time.monotonic() >= deadline:
                return False
            time.sleep(0.01)


def terminate_and_reap(processes: list[subprocess.Popen[str]]) -> bool:
    """Boundedly terminate and reap every isolated perf process group."""
    cleanup_failed = not _signal_process_groups(processes, signal.SIGTERM)
    deadline = time.monotonic() + COLLECTOR_SHUTDOWN_SECONDS
    for process in processes:
        try:
            process.wait(timeout=max(0.001, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            pass
        except OSError:
            cleanup_failed = True
    if not _signal_process_groups(processes, signal.SIGKILL):
        cleanup_failed = True
    for process in processes:
        try:
            process.wait(timeout=max(0.001, deadline - time.monotonic()))
        except (OSError, subprocess.TimeoutExpired):
            cleanup_failed = True
    for process in processes:
        if not _reap_adopted_group(process.pid, deadline):
            cleanup_failed = True
    return not cleanup_failed


def collect(options: object) -> dict[str, object]:
    component_pidfds = open_component_pidfds(options)
    processes: list[subprocess.Popen[str]] = []
    process_by_role: dict[str, subprocess.Popen[str]] = {}
    outputs: dict[str, str] = {}
    try:
        verify_identity_boundary(options, component_pidfds)
        verify_supervised_startup_endpoint(options)
        time.sleep(5)
        verify_identity_boundary(options, component_pidfds)
        verify_supervised_startup_endpoint(options)
        starting = {role: thread_ids(getattr(options, f"{role}_pid")) for role in ROLES}
        verify_identity_boundary(options, component_pidfds)
        verify_supervised_startup_endpoint(options)
        enable_child_subreaper()
        started_ns = time.monotonic_ns()
        for role in ROLES:
            process = subprocess.Popen(
                perf_command(options, starting[role]),
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                text=True,
                start_new_session=True,
            )
            processes.append(process)
            process_by_role[role] = process
        for role in ROLES:
            process = process_by_role[role]
            _stdout, stderr = process.communicate(timeout=options.duration_seconds + 10)
            if process.returncode != 0:
                raise CollectorError("perf_collection_failed")
            if len(stderr) > 64 * 1024:
                raise CollectorError("perf_output_too_large")
            outputs[role] = stderr
        finished_ns = time.monotonic_ns()
        verify_identity_boundary(options, component_pidfds)
        verify_supervised_startup_endpoint(options)
        ending = {role: thread_ids(getattr(options, f"{role}_pid")) for role in ROLES}
        verify_identity_boundary(options, component_pidfds)
        verify_supervised_startup_endpoint(options)
        if ending != starting:
            raise CollectorError("component_thread_set_changed")

        results: list[dict[str, int | str]] = []
        for role in ROLES:
            wakeup, wakeup_new = parse_perf(outputs[role])
            results.append(
                {
                    "component": role,
                    "thread_count_start": len(starting[role]),
                    "thread_count_end": len(ending[role]),
                    "sched_wakeup_count": wakeup,
                    "sched_wakeup_new_count": wakeup_new,
                    "received_wakeup_count": wakeup + wakeup_new,
                }
            )
    except subprocess.TimeoutExpired as error:
        raise CollectorError("perf_collection_unbounded") from error
    finally:
        cleanup_ok = terminate_and_reap(processes)
        close_pidfds(component_pidfds)
        if not cleanup_ok:
            raise CollectorError("perf_cleanup_failed")
    return {
        "schema_version": 1,
        "evidence_kind": "idle_scheduler_wakeups",
        "release_budget": False,
        "source_revision": options.revision,
        "reference_environment_id": options.environment_id,
        "method": {
            "clock": "python_monotonic_ns",
            "requested_interval_ns": options.duration_seconds * 1_000_000_000,
            "observed_interval_ns": finished_ns - started_ns,
            "settling_interval_ns": 5_000_000_000,
            "tracepoints": ["sched:sched_wakeup", "sched:sched_wakeup_new"],
            "target": "exact_starting_thread_ids",
            "collection_scope": "system_wide",
            "process_identity": "pidfd_and_proc_executable_at_boundaries",
            "contract_eligible": options.duration_seconds == 60,
            "live_tree_ownership": "exact_session_direct_children",
            "startup_endpoint": "ready_marker_and_single_owned_mapped_dock",
        },
        "results": results,
        "limitations": [
            "filtered_received_scheduler_wakeups_not_cpu_utilization_or_context_switches",
            "caller_must_hold_the_documented_display_and_input_state_idle_after_settling",
        ],
        "redaction": redaction_contract(),
    }


def main() -> int:
    try:
        options = parse_arguments()
        print(json.dumps(collect(options), indent=2, sort_keys=True))
        return 0
    except (CollectorError, OSError, subprocess.SubprocessError) as error:
        identifier = str(error) if isinstance(error, CollectorError) else "external_operation_failed"
        print(f"collect_idle_wakeups: {identifier}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
