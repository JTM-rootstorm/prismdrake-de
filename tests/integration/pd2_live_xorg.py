#!/usr/bin/env python3
"""Live Xorg-dummy adapter for the PD2 installed demonstration contract.

This module owns only observations it can make directly through Xorg, RandR,
and the reference EWMH window manager.  Every shell, service, and accessibility
predicate remains unsupported until a live observer is added; unsupported
steps fail closed and therefore cannot produce PD2 completion evidence.
"""

from __future__ import annotations

import ast
import hashlib
import json
import os
import re
import select
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Protocol

from pd2_demo import EXECUTABLE_NAMES, HarnessError, Observation
from pd2_live_atspi import (
    ALL_CATEGORIES as AT_SPI_CATEGORIES,
    APPLICATION_ERROR_CATEGORIES,
    CategoricalResult as AtspiCategoricalResult,
    ControlSelector,
    MAX_WORKER_MESSAGE_BYTES as MAX_AT_SPI_WORKER_MESSAGE_BYTES,
)
from pd2_live_dbus import LiveDbusProbe, PrivateBusEndpoint
from pd2_live_notification_workflow import (
    InstalledNotificationWorkflow,
    LiveDbusNotificationPort,
    NotificationSurfacePort,
    WorkflowResult,
    validate_workflow_result,
)
from pd2_live_notifications import (
    APPLICATION_NAME,
    CRITICAL_SUMMARY,
    PersistentNotificationProbe,
)
from pd2_live_lifecycle_workflow import (
    CleanupObservation as LifecycleCleanupObservation,
    ComponentObservation as LifecycleComponentObservation,
    InstalledLifecycleWorkflow,
    LogoutObservation as LifecycleLogoutObservation,
    NotificationEpochObservation as LifecycleNotificationEpochObservation,
    OutputIdentity as LifecycleOutputIdentity,
    ProcessIdentity as LifecycleProcessIdentity,
    RestartObservation as LifecycleRestartObservation,
    ReviewedSurfaceObservation as LifecycleReviewedSurfaceObservation,
    SettingsEpochObservation as LifecycleSettingsEpochObservation,
    ShutdownEvent as LifecycleShutdownEvent,
    ShutdownObservation as LifecycleShutdownObservation,
    STEP_CATEGORIES as LIFECYCLE_STEP_CATEGORIES,
    SurfacePlacement as LifecycleSurfacePlacement,
    SurfacePlacementObservation as LifecycleSurfacePlacementObservation,
    TopologyObservation as LifecycleTopologyObservation,
    WindowAuthorityObservation as LifecycleWindowAuthorityObservation,
    validate_workflow_result as validate_lifecycle_workflow_result,
)
from pd2_live_settings_workflow import (
    AccessibilitySurfacePort,
    InstalledSettingsWorkflow,
    SettingsWorkflowConfig,
    SettingsWorkflowResult,
    SurfaceObservationPort,
)
from pd2_notification_failure_replay import (
    FailureEvidence,
    GioNotificationOwnerLease,
    MalformedHistoryFixture,
    create_malformed_history_fixture,
    validate_failure_evidence,
    verify_conflicting_notification_owner,
    verify_malformed_history_recovery,
)
from pd2_tier_c_workflow import (
    ClientCleanupObservation as TierCClientCleanupObservation,
    KeyboardObservation as TierCKeyboardObservation,
    MaterialObservation as TierCMaterialObservation,
    PortResult as TierCPortResult,
    STEP_CONTRACTS,
    TIER_C_STEPS,
    TierCReplayWorkflow,
    TierCStepContract,
    validate_tier_c_result,
)
from pd2_x11trace_supervisor import X11TraceConfig, X11TraceSupervisor
from pd2_xwd import capture_window_fingerprint, transparent_reference_fingerprint


MAX_COMMAND_OUTPUT_BYTES = 65_536
MAX_EXECUTABLE_BYTES = 128 * 1024 * 1024
MAX_DESKTOP_PATH_BYTES = 4096
MAX_PROCESS_SCAN_ENTRIES = 32_768
MAX_PROCESS_ANCESTRY_DEPTH = 64
MAX_SURFACE_WINDOWS = 64
MAX_ACCESSIBILITY_BUS_ADDRESS_BYTES = 4096
SESSION_SHUTDOWN_SEQUENCE = (
    "confirmed",
    "stopping_shell",
    "stopping_notifyd",
    "stopping_settingsd",
    "complete",
)
OUTPUT_MODE = "1600x900"
SECONDARY_OUTPUT_MODE = "1600x900-pd2-secondary"
OUTPUT_WIDTH = 1600
OUTPUT_HEIGHT = 900
PANEL_HEIGHT = 48
PRIMARY_OUTPUT = "DUMMY0"
SECONDARY_OUTPUT = "DUMMY1"
CONTROLLED_APPLICATION_NAME = "Prismdrake Controlled Application"
CONTROLLED_DESKTOP_FILE = "prismdrake-controlled.desktop"
CONTROLLED_WINDOW_ONE = "Prismdrake Demo App One"
CONTROLLED_WINDOW_TWO = "Prismdrake Demo App Two"
KNOWN_NET_WM_STATES = frozenset(
    {
        b"_NET_WM_STATE_MODAL",
        b"_NET_WM_STATE_STICKY",
        b"_NET_WM_STATE_MAXIMIZED_VERT",
        b"_NET_WM_STATE_MAXIMIZED_HORZ",
        b"_NET_WM_STATE_SHADED",
        b"_NET_WM_STATE_SKIP_TASKBAR",
        b"_NET_WM_STATE_SKIP_PAGER",
        b"_NET_WM_STATE_HIDDEN",
        b"_NET_WM_STATE_FULLSCREEN",
        b"_NET_WM_STATE_ABOVE",
        b"_NET_WM_STATE_BELOW",
        b"_NET_WM_STATE_DEMANDS_ATTENTION",
        b"_NET_WM_STATE_FOCUSED",
    }
)
QUICK_SETTINGS_WINDOW_TITLE = "Prismdrake Quick Settings"
LAUNCHER_WINDOW_TITLE = "Prismdrake Launcher"
NOTIFICATION_WINDOW_TITLE = "Prismdrake Notifications"
NOTIFICATION_CENTER_WINDOW_TITLE = "Prismdrake Notification Center"
LOGOUT_WINDOW_TITLE = "Prismdrake Logout Confirmation"
TIER_C_WINDOW_TITLES = frozenset(
    {
        LAUNCHER_WINDOW_TITLE,
        NOTIFICATION_WINDOW_TITLE,
        NOTIFICATION_CENTER_WINDOW_TITLE,
        QUICK_SETTINGS_WINDOW_TITLE,
        LOGOUT_WINDOW_TITLE,
    }
)
TIER_C_KEY_ARGUMENTS: Mapping[str, str] = {
    "SUPER_D": "super+d",
    "TAB": "Tab",
    "SHIFT_TAB": "shift+Tab",
    "ENTER": "Return",
    "ESCAPE": "Escape",
    "SPACE": "space",
    "MENU": "shift+F10",
    "DOWN": "Down",
}
SETTINGS_WORKFLOW_CONFIG = SettingsWorkflowConfig(
    ("panel_primary", "panel_secondary", "quick_settings"),
    (
        ((400, 2), (800, 2), (1200, 2)),
        ((400, 2), (800, 2), (1200, 2)),
        ((24, 2), (190, 2), (355, 2)),
    ),
)
LIFECYCLE_REVIEWED_SURFACES = (
    "panel_primary",
    "panel_secondary",
    "quick_settings",
)


TierCTraceFactory = Callable[
    [X11TraceConfig, Mapping[str, str]], X11TraceSupervisor
]


@dataclass(frozen=True)
class CommandResult:
    """Bounded command result used by the adapter."""

    returncode: int
    stdout: bytes = b""
    stderr: bytes = b""


@dataclass(frozen=True)
class ProcessIdentity:
    """Ephemeral identity used only to prove distinct live process owners."""

    pid: int
    start_time: int


class ChildProcess(Protocol):
    """Minimal exact-child interface needed for bounded cleanup."""

    pid: int

    def poll(self) -> int | None: ...

    def wait(self, timeout: float | None = None) -> int: ...


class Operations(Protocol):
    """Injectable process boundary; production uses real subprocesses."""

    def spawn(self, argv: Sequence[str], env: Mapping[str, str]) -> ChildProcess: ...

    def run(
        self, argv: Sequence[str], env: Mapping[str, str], timeout: float
    ) -> CommandResult: ...

    def stop(self, child: ChildProcess, timeout: float) -> bool: ...

    def display_available(self, number: int) -> bool: ...

    def identify(
        self, child: ChildProcess, executable: Path
    ) -> ProcessIdentity | None: ...

    def descendants(
        self, child: ChildProcess, executable: Path
    ) -> Sequence[ProcessIdentity]: ...

    def identify_pid(self, pid: int, executable: Path) -> ProcessIdentity | None: ...

    def stop_identity(
        self, identity: ProcessIdentity, executable: Path, timeout: float
    ) -> bool: ...

    def fail_identity(
        self, identity: ProcessIdentity, executable: Path, timeout: float
    ) -> bool: ...

    def socket_identity(self, path: Path) -> tuple[int, int] | None: ...

    def socket_connectable(self, path: Path) -> bool: ...

    def monotonic(self) -> float: ...

    def monotonic_ns(self) -> int: ...

    def pause(self, duration: float) -> None: ...


class AccessibilityOperations(Protocol):
    """Bounded shell-owned AT-SPI observation and action boundary."""

    def initialize(self, environment: Mapping[str, str]) -> bool: ...

    def close(self) -> bool: ...

    def application_error_category(self) -> str | None: ...

    def observe_application(
        self, shell_pid: int, environment: Mapping[str, str]
    ) -> bool: ...

    def observe(
        self, shell_pid: int, selector: ControlSelector, environment: Mapping[str, str]
    ) -> bool: ...

    def invoke(
        self, shell_pid: int, selector: ControlSelector, environment: Mapping[str, str]
    ) -> bool: ...

    def observe_state(
        self,
        shell_pid: int,
        selector: ControlSelector,
        state_name: str,
        expected: bool,
        environment: Mapping[str, str],
    ) -> bool: ...

    def observe_absent(
        self, shell_pid: int, selector: ControlSelector, environment: Mapping[str, str]
    ) -> bool: ...

    def reject_stale_action(
        self, shell_pid: int, selector: ControlSelector, environment: Mapping[str, str]
    ) -> bool: ...

    def capture_reference(
        self, shell_pid: int, selector: ControlSelector, environment: Mapping[str, str]
    ) -> object | None: ...

    def observe_reference(
        self, shell_pid: int, reference: object, environment: Mapping[str, str]
    ) -> bool: ...

    def reject_stale_reference(
        self, reference: object, environment: Mapping[str, str]
    ) -> bool: ...


class SessionTransitionMonitor(Protocol):
    """Pre-armed exact observer for the reviewed session transition signal."""

    def finish(self, transaction_id: int, timeout: float) -> tuple[str, ...]: ...

    def close(self) -> bool: ...


SessionTransitionMonitorFactory = Callable[
    [Path, str, Mapping[str, str], float], SessionTransitionMonitor
]


AT_SPI_ENVIRONMENT_NAMES = (
    "AT_SPI_BUS_ADDRESS",
    "DBUS_SESSION_BUS_ADDRESS",
    "DISPLAY",
)
AT_SPI_WORKER_TIMEOUT_SECONDS = 3.0


@dataclass(frozen=True, repr=False)
class _WorkerControlReference:
    """One opaque worker-local reference scoped to an exact bus epoch."""

    epoch: int
    reference_id: int


class _AtspiWorkerProcess:
    """Bounded JSON-line client for one process-isolated libatspi epoch."""

    def __init__(self, environment: Mapping[str, str]) -> None:
        worker_environment = dict(environment)
        worker_path = Path(__file__).resolve().with_name("pd2_live_atspi.py")
        try:
            self._process = subprocess.Popen(  # noqa: S603 - attested exact helper
                (sys.executable, str(worker_path)),
                env=worker_environment,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                close_fds=True,
            )
        except OSError as error:
            raise HarnessError("live_atspi_worker_start_failed") from error
        self._buffer = bytearray()
        try:
            response = self._read_response()
        except Exception:
            self.close()
            raise
        if response != {"category": "atspi_worker_ready", "status": "passed"}:
            self.close()
            raise HarnessError("live_atspi_worker_start_failed")

    @staticmethod
    def _validate_response(document: object) -> dict[str, object]:
        if (
            not isinstance(document, dict)
            or not {"category", "status"} <= set(document)
            or not set(document) <= {"category", "reference_id", "status"}
            or document.get("category") not in AT_SPI_CATEGORIES
            or document.get("status") not in {"passed", "failed"}
        ):
            raise HarnessError("live_atspi_worker_protocol_invalid")
        if "reference_id" in document and (
            isinstance(document["reference_id"], bool)
            or not isinstance(document["reference_id"], int)
            or document["reference_id"] <= 0
            or document["status"] != "passed"
        ):
            raise HarnessError("live_atspi_worker_protocol_invalid")
        return document

    def _read_response(self) -> dict[str, object]:
        stdout = self._process.stdout
        if stdout is None:
            raise HarnessError("live_atspi_worker_protocol_invalid")
        deadline = time.monotonic() + AT_SPI_WORKER_TIMEOUT_SECONDS
        while b"\n" not in self._buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0.0:
                raise HarnessError("live_atspi_worker_timeout")
            ready, _, _ = select.select((stdout,), (), (), remaining)
            if not ready:
                raise HarnessError("live_atspi_worker_timeout")
            block = os.read(stdout.fileno(), MAX_AT_SPI_WORKER_MESSAGE_BYTES)
            if not block:
                raise HarnessError("live_atspi_worker_exited")
            self._buffer.extend(block)
            if len(self._buffer) > MAX_AT_SPI_WORKER_MESSAGE_BYTES:
                raise HarnessError("live_atspi_worker_protocol_invalid")
        payload, _, remainder = self._buffer.partition(b"\n")
        self._buffer = bytearray(remainder)
        try:
            document = json.loads(payload)
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise HarnessError("live_atspi_worker_protocol_invalid") from error
        return self._validate_response(document)

    def request(self, document: Mapping[str, object]) -> dict[str, object]:
        stdin = self._process.stdin
        if stdin is None or self._process.poll() is not None:
            raise HarnessError("live_atspi_worker_exited")
        try:
            payload = json.dumps(
                document, separators=(",", ":"), sort_keys=True
            ).encode("ascii")
        except (TypeError, UnicodeEncodeError) as error:
            raise HarnessError("live_atspi_worker_protocol_invalid") from error
        if len(payload) + 1 > MAX_AT_SPI_WORKER_MESSAGE_BYTES:
            raise HarnessError("live_atspi_worker_protocol_invalid")
        try:
            stdin.write(payload + b"\n")
            stdin.flush()
        except (BrokenPipeError, OSError) as error:
            raise HarnessError("live_atspi_worker_exited") from error
        response = self._read_response()
        has_reference = "reference_id" in response
        expects_reference = (
            document.get("operation") == "capture_reference"
            and response.get("status") == "passed"
        )
        if has_reference != expects_reference:
            raise HarnessError("live_atspi_worker_protocol_invalid")
        return response

    def close(self) -> bool:
        process = self._process
        if process.stdin is not None:
            try:
                process.stdin.close()
            except OSError:
                pass
        clean = True
        try:
            process.wait(timeout=AT_SPI_WORKER_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            clean = False
            process.terminate()
            try:
                process.wait(timeout=AT_SPI_WORKER_TIMEOUT_SECONDS)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=AT_SPI_WORKER_TIMEOUT_SECONDS)
        if process.stdout is not None:
            process.stdout.close()
        return clean and process.returncode == 0


def _selector_document(selector: ControlSelector) -> dict[str, object]:
    return {
        "action": selector.action,
        "name": selector.name,
        "require_description": selector.require_description,
        "require_focused": selector.require_focused,
        "role": selector.role,
    }


class SystemAccessibilityOperations:
    """Real AT-SPI adapter isolated to one helper process per private bus."""

    def __init__(
        self,
        worker_factory: Callable[[Mapping[str, str]], object] | None = None,
    ) -> None:
        self._worker_factory = worker_factory or _AtspiWorkerProcess
        self._worker: object | None = None
        self._environment: dict[str, str] | None = None
        self._epoch = 0
        self._application_error: str | None = None

    @staticmethod
    def _select_environment(environment: Mapping[str, str]) -> dict[str, str]:
        if not all(
            isinstance(environment.get(name), str) and environment[name]
            for name in AT_SPI_ENVIRONMENT_NAMES
        ):
            raise HarnessError("live_atspi_environment_invalid")
        return {name: environment[name] for name in AT_SPI_ENVIRONMENT_NAMES}

    def initialize(self, environment: Mapping[str, str]) -> bool:
        """Start a fresh libatspi process for the verified private bus epoch."""

        selected = self._select_environment(environment)
        if self._worker is not None and not self.close():
            raise HarnessError("live_atspi_worker_cleanup_failed")
        try:
            self._worker = self._worker_factory(selected)
        except HarnessError:
            raise
        except Exception as error:
            raise HarnessError("live_atspi_worker_start_failed") from error
        self._environment = selected
        self._epoch += 1
        self._application_error = None
        return True

    def close(self) -> bool:
        worker = self._worker
        self._worker = None
        self._environment = None
        self._application_error = None
        if worker is None:
            return True
        close = getattr(worker, "close", None)
        if not callable(close):
            return False
        try:
            return close() is True
        except Exception:
            return False

    def _request(
        self, environment: Mapping[str, str], operation: str, **values: object
    ) -> dict[str, object]:
        selected = self._select_environment(environment)
        if selected != self._environment or self._worker is None:
            raise HarnessError("live_atspi_environment_invalid")
        request = getattr(self._worker, "request", None)
        if not callable(request):
            raise HarnessError("live_atspi_worker_protocol_invalid")
        response = request({"operation": operation, **values})
        if not isinstance(response, dict):
            raise HarnessError("live_atspi_worker_protocol_invalid")
        if response.get("category") in {
            "atspi_worker_initialization_failed",
            "atspi_worker_internal_failure",
            "atspi_worker_protocol_invalid",
        }:
            raise HarnessError("live_atspi_worker_protocol_invalid")
        return response

    def application_error_category(self) -> str | None:
        return self._application_error

    def observe(
        self, shell_pid: int, selector: ControlSelector, environment: Mapping[str, str]
    ) -> bool:
        response = self._request(
            environment,
            "observe",
            shell_pid=shell_pid,
            selector=_selector_document(selector),
        )
        return response == {"category": "atspi_control_ready", "status": "passed"}

    def observe_application(
        self, shell_pid: int, environment: Mapping[str, str]
    ) -> bool:
        response = self._request(
            environment, "observe_application", shell_pid=shell_pid
        )
        category = response.get("category")
        passed = response == {
            "category": "atspi_application_ready",
            "status": "passed",
        }
        self._application_error = (
            None
            if passed or category not in APPLICATION_ERROR_CATEGORIES
            else str(category)
        )
        return passed

    def invoke(
        self, shell_pid: int, selector: ControlSelector, environment: Mapping[str, str]
    ) -> bool:
        response = self._request(
            environment,
            "invoke",
            shell_pid=shell_pid,
            selector=_selector_document(selector),
        )
        return response == {"category": "atspi_action_invoked", "status": "passed"}

    def observe_state(
        self,
        shell_pid: int,
        selector: ControlSelector,
        state_name: str,
        expected: bool,
        environment: Mapping[str, str],
    ) -> bool:
        response = self._request(
            environment,
            "observe_state",
            shell_pid=shell_pid,
            selector=_selector_document(selector),
            state_name=state_name,
            expected=expected,
        )
        return response == {
            "category": "atspi_control_state_observed",
            "status": "passed",
        }

    def observe_absent(
        self, shell_pid: int, selector: ControlSelector, environment: Mapping[str, str]
    ) -> bool:
        response = self._request(
            environment,
            "observe_absent",
            shell_pid=shell_pid,
            selector=_selector_document(selector),
        )
        return response == {"category": "atspi_control_absent", "status": "passed"}

    def reject_stale_action(
        self, shell_pid: int, selector: ControlSelector, environment: Mapping[str, str]
    ) -> bool:
        response = self._request(
            environment,
            "reject_stale_action",
            shell_pid=shell_pid,
            selector=_selector_document(selector),
        )
        return response == {
            "category": "atspi_stale_control_rejected",
            "status": "passed",
        }

    def capture_reference(
        self, shell_pid: int, selector: ControlSelector, environment: Mapping[str, str]
    ) -> object | None:
        response = self._request(
            environment,
            "capture_reference",
            shell_pid=shell_pid,
            selector=_selector_document(selector),
        )
        reference_id = response.get("reference_id")
        if (
            response.get("category") != "atspi_control_reference_ready"
            or response.get("status") != "passed"
            or isinstance(reference_id, bool)
            or not isinstance(reference_id, int)
            or reference_id <= 0
        ):
            return None
        return _WorkerControlReference(self._epoch, reference_id)

    def observe_reference(
        self, shell_pid: int, reference: object, environment: Mapping[str, str]
    ) -> bool:
        if (
            not isinstance(reference, _WorkerControlReference)
            or reference.epoch != self._epoch
        ):
            return False
        response = self._request(
            environment,
            "observe_reference",
            shell_pid=shell_pid,
            reference_id=reference.reference_id,
        )
        return response == {
            "category": "atspi_control_reference_ready",
            "status": "passed",
        }

    def reject_stale_reference(
        self, reference: object, environment: Mapping[str, str]
    ) -> bool:
        if (
            not isinstance(reference, _WorkerControlReference)
            or reference.epoch != self._epoch
        ):
            return False
        response = self._request(
            environment,
            "reject_stale_reference",
            reference_id=reference.reference_id,
        )
        return response == {
            "category": "atspi_stale_control_rejected",
            "status": "passed",
        }


class SystemOperations:
    """Real bounded process operations for a private live demonstration run."""

    def spawn(self, argv: Sequence[str], env: Mapping[str, str]) -> ChildProcess:
        try:
            return subprocess.Popen(  # noqa: S603 - exact reviewed argv, no shell
                list(argv),
                env=dict(env),
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
                close_fds=True,
            )
        except OSError as error:
            raise HarnessError("live_process_start_failed") from error

    def run(
        self, argv: Sequence[str], env: Mapping[str, str], timeout: float
    ) -> CommandResult:
        try:
            completed = subprocess.run(  # noqa: S603 - exact reviewed argv, no shell
                list(argv),
                env=dict(env),
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=timeout,
                check=False,
            )
        except subprocess.TimeoutExpired as error:
            raise HarnessError("live_command_timeout") from error
        except OSError as error:
            raise HarnessError("live_command_failed") from error
        if len(completed.stdout) + len(completed.stderr) > MAX_COMMAND_OUTPUT_BYTES:
            raise HarnessError("live_command_output_oversized")
        return CommandResult(completed.returncode, completed.stdout, completed.stderr)

    def stop(self, child: ChildProcess, timeout: float) -> bool:
        deadline = time.monotonic() + timeout
        group = child.pid
        try:
            if child.poll() is None:
                group = os.getpgid(child.pid)
            if self._process_group_alive(group):
                os.killpg(group, signal.SIGTERM)
                try:
                    child.wait(timeout=max(timeout / 2.0, 0.01))
                except subprocess.TimeoutExpired:
                    pass
        except OSError:
            return child.poll() is not None
        if not self._process_group_alive(group):
            return child.poll() is not None
        try:
            os.killpg(group, signal.SIGKILL)
            if child.poll() is None:
                child.wait(timeout=max(deadline - time.monotonic(), 0.01))
        except (OSError, subprocess.TimeoutExpired):
            pass
        while time.monotonic() < deadline:
            if not self._process_group_alive(group):
                return child.poll() is not None
            time.sleep(0.01)
        return child.poll() is not None and not self._process_group_alive(group)

    @staticmethod
    def _process_group_alive(group: int) -> bool:
        try:
            os.killpg(group, 0)
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
        return True

    def display_available(self, number: int) -> bool:
        return not Path(f"/tmp/.X{number}-lock").exists() and not Path(
            f"/tmp/.X11-unix/X{number}"
        ).exists()

    def identify(
        self, child: ChildProcess, executable: Path
    ) -> ProcessIdentity | None:
        if child.poll() is not None:
            return None
        record = self._process_record(child.pid)
        if record is None or not self._executable_matches(child.pid, executable):
            return None
        _parent, start_time = record
        return ProcessIdentity(child.pid, start_time)

    def descendants(
        self, child: ChildProcess, executable: Path
    ) -> Sequence[ProcessIdentity]:
        if child.poll() is not None:
            return ()
        records: dict[int, tuple[int, int]] = {}
        try:
            entries = tuple(Path("/proc").iterdir())
        except OSError:
            return ()
        if len(entries) > MAX_PROCESS_SCAN_ENTRIES:
            return ()
        for entry in entries:
            if not entry.name.isascii() or not entry.name.isdecimal():
                continue
            pid = int(entry.name)
            record = self._process_record(pid)
            if record is not None:
                records[pid] = record

        matches: list[ProcessIdentity] = []
        for pid, (_parent, start_time) in records.items():
            if pid == child.pid or not self._is_descendant(pid, child.pid, records):
                continue
            if self._executable_matches(pid, executable):
                matches.append(ProcessIdentity(pid, start_time))
        return tuple(sorted(matches, key=lambda identity: identity.pid))

    def identify_pid(self, pid: int, executable: Path) -> ProcessIdentity | None:
        record = self._process_record(pid)
        if record is None or not self._executable_matches(pid, executable):
            return None
        return ProcessIdentity(pid, record[1])

    def stop_identity(
        self, identity: ProcessIdentity, executable: Path, timeout: float
    ) -> bool:
        if self.identify_pid(identity.pid, executable) != identity:
            return True
        try:
            os.kill(identity.pid, signal.SIGTERM)
        except ProcessLookupError:
            return True
        except OSError:
            return False
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.identify_pid(identity.pid, executable) != identity:
                return True
            time.sleep(0.01)
        if self.identify_pid(identity.pid, executable) != identity:
            return True
        try:
            os.kill(identity.pid, signal.SIGKILL)
        except ProcessLookupError:
            return True
        except OSError:
            return False
        while time.monotonic() < deadline + 0.25:
            if self.identify_pid(identity.pid, executable) != identity:
                return True
            time.sleep(0.01)
        return False

    def fail_identity(
        self, identity: ProcessIdentity, executable: Path, timeout: float
    ) -> bool:
        if self.identify_pid(identity.pid, executable) != identity:
            return False
        try:
            os.kill(identity.pid, signal.SIGKILL)
        except ProcessLookupError:
            return True
        except OSError:
            return False
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.identify_pid(identity.pid, executable) != identity:
                return True
            time.sleep(0.01)
        return self.identify_pid(identity.pid, executable) != identity

    def socket_identity(self, path: Path) -> tuple[int, int] | None:
        try:
            metadata = path.lstat()
        except FileNotFoundError:
            return None
        except OSError as error:
            raise HarnessError("private_session_bus_socket_invalid") from error
        if not stat.S_ISSOCK(metadata.st_mode) or metadata.st_uid != os.geteuid():
            raise HarnessError("private_session_bus_socket_invalid")
        return metadata.st_dev, metadata.st_ino

    def socket_connectable(self, path: Path) -> bool:
        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            client.settimeout(0.05)
            client.connect(os.fspath(path))
        except OSError:
            return False
        finally:
            client.close()
        return True

    @staticmethod
    def _process_record(pid: int) -> tuple[int, int] | None:
        try:
            payload = Path(f"/proc/{pid}/stat").read_bytes()
        except OSError:
            return None
        closing = payload.rfind(b")")
        if closing < 1 or closing + 2 >= len(payload):
            return None
        fields = payload[closing + 2 :].split()
        if len(fields) <= 19:
            return None
        try:
            parent = int(fields[1])
            start_time = int(fields[19])
        except ValueError:
            return None
        if parent < 0 or start_time <= 0:
            return None
        return parent, start_time

    @staticmethod
    def _is_descendant(
        pid: int, ancestor: int, records: Mapping[int, tuple[int, int]]
    ) -> bool:
        visited: set[int] = set()
        current = pid
        for _depth in range(MAX_PROCESS_ANCESTRY_DEPTH):
            record = records.get(current)
            if record is None:
                return False
            parent = record[0]
            if parent == ancestor:
                return True
            if parent <= 1 or parent in visited:
                return False
            visited.add(parent)
            current = parent
        return False

    @staticmethod
    def _executable_matches(pid: int, executable: Path) -> bool:
        try:
            expected = executable.stat()
            observed = Path(f"/proc/{pid}/exe").stat()
        except OSError:
            return False
        return (expected.st_dev, expected.st_ino) == (observed.st_dev, observed.st_ino)

    def monotonic(self) -> float:
        return time.monotonic()

    def monotonic_ns(self) -> int:
        return time.monotonic_ns()

    def pause(self, duration: float) -> None:
        time.sleep(duration)


def parse_session_transition_output(
    payload: bytes, transaction_id: int
) -> tuple[str, ...]:
    """Return the exact reviewed shutdown sequence without retaining other output."""

    if (
        not isinstance(payload, bytes)
        or not 1 <= len(payload) <= MAX_COMMAND_OUTPUT_BYTES
        or b"\x00" in payload
        or type(transaction_id) is not int
        or not 1 <= transaction_id <= (1 << 64) - 1
    ):
        raise HarnessError("lifecycle_logout_transition_invalid")
    marker = (
        b"path=/org/prismdrake/Session1; "
        b"interface=org.prismdrake.Session1; member=SessionStateChanged"
    )
    observed: list[tuple[int, str]] = []
    for block in re.split(rb"(?=signal time=)", payload):
        if marker not in block:
            continue
        transactions = re.findall(rb"^\s+uint64 ([0-9]{1,20})$", block, re.MULTILINE)
        states = re.findall(
            rb'^\s+string "([a-z_]{1,32})"$', block, re.MULTILINE
        )
        if len(transactions) != 1 or len(states) != 1:
            raise HarnessError("lifecycle_logout_transition_invalid")
        try:
            transaction = int(transactions[0])
            state = states[0].decode("ascii")
        except (ValueError, UnicodeError):
            raise HarnessError("lifecycle_logout_transition_invalid") from None
        observed.append((transaction, state))
    if (
        tuple(state for _transaction, state in observed) != SESSION_SHUTDOWN_SEQUENCE
        or any(transaction != transaction_id for transaction, _state in observed)
    ):
        raise HarnessError("lifecycle_logout_transition_invalid")
    return SESSION_SHUTDOWN_SEQUENCE


class SystemSessionTransitionMonitor:
    """Bounded ``dbus-monitor`` observer armed before the confirm action."""

    _MATCH = (
        "type='signal',sender='org.prismdrake.Session1',"
        "path='/org/prismdrake/Session1',"
        "interface='org.prismdrake.Session1',member='SessionStateChanged'"
    )

    def __init__(
        self,
        executable: Path,
        address: str,
        environment: Mapping[str, str],
        readiness_timeout: float,
    ) -> None:
        self._process: subprocess.Popen[bytes] | None = None
        self._payload = bytearray()
        try:
            process = subprocess.Popen(  # noqa: S603 - exact reviewed argv, no shell
                [str(executable), "--address", address, self._MATCH],
                env=dict(environment),
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                start_new_session=True,
                close_fds=True,
            )
            self._process = process
            self._read_until(b"member=NameAcquired", readiness_timeout)
            self._payload.clear()
        except Exception:
            self.close()
            raise HarnessError("lifecycle_logout_monitor_start_failed") from None

    def _read_until(self, marker: bytes, timeout: float) -> None:
        process = self._process
        if process is None or process.stdout is None or timeout <= 0:
            raise HarnessError("lifecycle_logout_monitor_failed")
        descriptor = process.stdout.fileno()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if marker in self._payload:
                return
            if process.poll() is not None:
                break
            remaining = max(0.0, deadline - time.monotonic())
            ready, _writable, _failed = select.select(
                [descriptor], [], [], min(remaining, 0.05)
            )
            if not ready:
                continue
            chunk = os.read(descriptor, 4096)
            if not chunk:
                break
            self._payload.extend(chunk)
            if len(self._payload) > MAX_COMMAND_OUTPUT_BYTES:
                raise HarnessError("lifecycle_logout_monitor_failed")
        if marker not in self._payload:
            raise HarnessError("lifecycle_logout_monitor_failed")

    def finish(self, transaction_id: int, timeout: float) -> tuple[str, ...]:
        process = self._process
        if process is None:
            raise HarnessError("lifecycle_logout_monitor_failed")
        try:
            self._read_until(b'string "complete"', timeout)
            process.terminate()
            stdout, stderr = process.communicate(timeout=min(max(timeout, 0.05), 2.0))
            self._payload.extend(stdout)
            if (
                stderr
                or len(self._payload) > MAX_COMMAND_OUTPUT_BYTES
                or process.returncode not in {-signal.SIGTERM, 0}
            ):
                raise HarnessError("lifecycle_logout_monitor_failed")
            return parse_session_transition_output(bytes(self._payload), transaction_id)
        except HarnessError:
            raise
        except Exception:
            raise HarnessError("lifecycle_logout_monitor_failed") from None
        finally:
            self.close()

    def close(self) -> bool:
        process = self._process
        self._process = None
        if process is None:
            return True
        try:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=0.2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=0.2)
        except (OSError, subprocess.TimeoutExpired):
            return False
        return process.poll() is not None


class LiveNotificationSurfaceAdapter:
    """Narrow bounded AT-SPI surface for installed notification evidence."""

    _ACTION = ControlSelector("Review", "push_button", "Press")
    _CENTER = ControlSelector("Notification center", "panel")
    _DO_NOT_DISTURB = ControlSelector("Do not disturb", "check_box")
    _TRANSIENT = ControlSelector("Transient notifications", "notification")

    def __init__(
        self,
        accessibility: AccessibilityOperations,
        shell_identity: Callable[[], ProcessIdentity],
        environment: Callable[[], Mapping[str, str]],
        monotonic: Callable[[], float],
        pause: Callable[[float], None],
        dismiss_center: Callable[[], None],
    ) -> None:
        self._accessibility = accessibility
        self._shell_identity = shell_identity
        self._environment = environment
        self._monotonic = monotonic
        self._pause = pause
        self._dismiss_center = dismiss_center

    def _wait(
        self, predicate: Callable[[int, Mapping[str, str]], bool], timeout: float,
        category: str,
    ) -> None:
        if (
            isinstance(timeout, bool)
            or not isinstance(timeout, (int, float))
            or not 0.05 <= timeout <= 10.0
        ):
            raise HarnessError("notification_surface_timeout_invalid")
        deadline = self._monotonic() + float(timeout)
        while self._monotonic() < deadline:
            shell = self._shell_identity()
            environment = self._environment()
            if predicate(shell.pid, environment):
                return
            remaining = deadline - self._monotonic()
            if remaining > 0:
                self._pause(min(0.05, remaining))
        raise HarnessError(category)

    def invoke_live_action(self, timeout_seconds: float) -> None:
        self._wait(
            lambda pid, environment: self._accessibility.observe(
                pid, self._ACTION, environment
            ),
            timeout_seconds,
            "notification_surface_action_timeout",
        )
        shell = self._shell_identity()
        if not self._accessibility.invoke(
            shell.pid, self._ACTION, self._environment()
        ):
            raise HarnessError("notification_surface_action_rejected")

    def observe_center(self, timeout_seconds: float) -> None:
        self._wait(
            lambda pid, environment: self._accessibility.observe(
                pid, self._CENTER, environment
            ),
            timeout_seconds,
            "notification_surface_center_timeout",
        )

    def observe_do_not_disturb(
        self, enabled: bool, timeout_seconds: float
    ) -> None:
        if enabled is not True:
            raise HarnessError("notification_surface_state_not_reviewed")
        self._wait(
            lambda pid, environment: self._accessibility.observe_state(
                pid, self._DO_NOT_DISTURB, "CHECKED", True, environment
            ),
            timeout_seconds,
            "notification_surface_dnd_timeout",
        )

    def dismiss_center(self, timeout_seconds: float) -> None:
        self._dismiss_center()
        self._wait(
            lambda pid, environment: self._accessibility.observe_absent(
                pid, self._CENTER, environment
            ),
            timeout_seconds,
            "notification_surface_center_dismissal_timeout",
        )

    def observe_transient(self, present: bool, timeout_seconds: float) -> None:
        if not isinstance(present, bool):
            raise HarnessError("notification_surface_state_not_reviewed")
        if present:
            predicate = lambda pid, environment: self._accessibility.observe(
                pid, self._TRANSIENT, environment
            )
            category = "notification_surface_transient_presence_timeout"
        else:
            predicate = lambda pid, environment: self._accessibility.observe_absent(
                pid, self._TRANSIENT, environment
            )
            category = "notification_surface_transient_absence_timeout"
        self._wait(predicate, timeout_seconds, category)


class LiveSettingsAccessibilityAdapter:
    """Exact shell-owned AT-SPI observer for the quick-settings surface."""

    _SELECTOR = ControlSelector("Quick settings", "pane")

    def __init__(
        self,
        accessibility: AccessibilityOperations,
        shell_identity: Callable[[], ProcessIdentity],
        environment: Callable[[], Mapping[str, str]],
        monotonic: Callable[[], float],
        pause: Callable[[float], None],
        timeout_seconds: float,
    ) -> None:
        self._accessibility = accessibility
        self._shell_identity = shell_identity
        self._environment = environment
        self._monotonic = monotonic
        self._pause = pause
        self._timeout_seconds = timeout_seconds

    def observe_surface(self, name: str, role: str) -> bool:
        if (name, role) != (self._SELECTOR.name, self._SELECTOR.role):
            return False
        deadline = self._monotonic() + self._timeout_seconds
        while self._monotonic() < deadline:
            shell = self._shell_identity()
            if self._accessibility.observe(
                shell.pid, self._SELECTOR, self._environment()
            ):
                return True
            remaining = deadline - self._monotonic()
            if remaining > 0:
                self._pause(min(0.05, remaining))
        return False


SettingsWindowResolver = Callable[[str], int]
SettingsFingerprintCapture = Callable[
    [int, Sequence[tuple[int, int]]], bytes
]


class LiveSettingsSurfaceAdapter:
    """Sequential reviewed-surface capture through the private XWD boundary."""

    def __init__(
        self,
        config: SettingsWorkflowConfig,
        resolve_window: SettingsWindowResolver,
        capture_fingerprint: SettingsFingerprintCapture,
        pause: Callable[[float], None],
    ) -> None:
        self._config = config
        self._resolve_window = resolve_window
        self._capture_fingerprint = capture_fingerprint
        self._pause = pause

    def reviewed_surface_sequence(self) -> Sequence[str]:
        return self._config.reviewed_surfaces

    def capture_settled_fingerprints(
        self, surface: str, points: Sequence[tuple[int, int]]
    ) -> tuple[bytes, bytes]:
        if (
            surface not in self._config.reviewed_surfaces
            or tuple(points) != self._config.points_for(surface)
        ):
            raise HarnessError("settings_surface_capture_not_reviewed")
        window = self._resolve_window(surface)
        first = self._capture_fingerprint(window, points)
        self._pause(0.05)
        second = self._capture_fingerprint(window, points)
        return first, second


class NotificationFailureReplayPort(Protocol):
    """Narrow installed notification failure-replay boundary."""

    def verify_malformed_history(
        self,
        fixture: MalformedHistoryFixture,
        environment: Mapping[str, str],
        endpoint: PrivateBusEndpoint,
    ) -> FailureEvidence: ...

    def verify_conflicting_owner(
        self,
        executable: Path,
        environment: Mapping[str, str],
        endpoint: PrivateBusEndpoint,
        start_conflicted_session: Callable[[], None],
        sibling_observer: Callable[[], Mapping[str, tuple[int, int]]],
        active_conflict_observer: Callable[[], None],
    ) -> FailureEvidence: ...


class SystemNotificationFailureReplayPort:
    """Compose the strict helper with the attested live private-bus epoch."""

    def verify_malformed_history(
        self,
        fixture: MalformedHistoryFixture,
        environment: Mapping[str, str],
        endpoint: PrivateBusEndpoint,
    ) -> FailureEvidence:
        probe: LiveDbusProbe | None = None
        try:
            probe = LiveDbusProbe(environment, endpoint=endpoint)
            evidence = verify_malformed_history_recovery(fixture, probe)
            validate_failure_evidence(evidence)
            return evidence
        finally:
            if probe is not None:
                probe.close()

    def verify_conflicting_owner(
        self,
        executable: Path,
        environment: Mapping[str, str],
        endpoint: PrivateBusEndpoint,
        start_conflicted_session: Callable[[], None],
        sibling_observer: Callable[[], Mapping[str, tuple[int, int]]],
        active_conflict_observer: Callable[[], None],
    ) -> FailureEvidence:
        with GioNotificationOwnerLease(environment, endpoint) as owner:
            start_conflicted_session()
            evidence = verify_conflicting_notification_owner(
                executable, environment, endpoint, owner, sibling_observer
            )
            active_conflict_observer()
            owner.verify()
        validate_failure_evidence(evidence)
        return evidence


class _Closable(Protocol):
    def close(self) -> None: ...


@dataclass
class _NotificationEpoch:
    workflow: InstalledNotificationWorkflow
    sender: _Closable
    dbus_probe: _Closable

    def close(self) -> bool:
        clean = True
        for resource in (self.sender, self.dbus_probe):
            try:
                resource.close()
            except Exception:
                clean = False
        return clean


@dataclass
class _SettingsEpoch:
    workflow: InstalledSettingsWorkflow
    dbus_probe: _Closable

    def close(self) -> bool:
        try:
            self.dbus_probe.close()
        except Exception:
            return False
        return True


NotificationEpochFactory = Callable[
    [Mapping[str, str], PrivateBusEndpoint, NotificationSurfacePort],
    _NotificationEpoch,
]
SettingsEpochFactory = Callable[
    [
        Mapping[str, str],
        PrivateBusEndpoint,
        AccessibilitySurfacePort,
        SurfaceObservationPort,
    ],
    _SettingsEpoch,
]


def _create_notification_epoch(
    environment: Mapping[str, str],
    endpoint: PrivateBusEndpoint,
    surface: NotificationSurfacePort,
) -> _NotificationEpoch:
    sender: PersistentNotificationProbe | None = None
    dbus_probe: LiveDbusProbe | None = None
    try:
        import gi

        gi.require_version("Gio", "2.0")
        gi.require_version("GLib", "2.0")
        from gi.repository import Gio, GLib

        sender = PersistentNotificationProbe.connect_private(environment, Gio, GLib)
        dbus_probe = LiveDbusProbe(environment, endpoint=endpoint)
        workflow = InstalledNotificationWorkflow(
            sender, LiveDbusNotificationPort(dbus_probe), surface, PRIMARY_OUTPUT
        )
        return _NotificationEpoch(workflow, sender, dbus_probe)
    except Exception:
        for resource in (sender, dbus_probe):
            if resource is not None:
                try:
                    resource.close()
                except Exception:
                    pass
        raise HarnessError("live_notification_epoch_initialization_failed") from None


def _create_settings_epoch(
    environment: Mapping[str, str],
    endpoint: PrivateBusEndpoint,
    accessibility: AccessibilitySurfacePort,
    surfaces: SurfaceObservationPort,
) -> _SettingsEpoch:
    dbus_probe: LiveDbusProbe | None = None
    try:
        dbus_probe = LiveDbusProbe(environment, endpoint=endpoint)
        workflow = InstalledSettingsWorkflow(
            SETTINGS_WORKFLOW_CONFIG, dbus_probe, accessibility, surfaces
        )
        return _SettingsEpoch(workflow, dbus_probe)
    except Exception:
        if dbus_probe is not None:
            try:
                dbus_probe.close()
            except Exception:
                pass
        raise HarnessError("live_settings_epoch_initialization_failed") from None


@dataclass(frozen=True)
class XrandrOutput:
    """One strictly parsed RandR output line."""

    name: str
    connected: bool
    primary: bool
    width: int | None
    height: int | None
    x: int | None
    y: int | None

    @property
    def active(self) -> bool:
        return self.connected and self.width is not None


@dataclass(frozen=True)
class WindowGeometry:
    """Bounded mapped X11 window geometry from ``xwininfo``."""

    x: int
    y: int
    width: int
    height: int


_OUTPUT_LINE = re.compile(
    rb"^(?P<name>[A-Za-z0-9_.:-]{1,128}) "
    rb"(?P<state>connected|disconnected)"
    rb"(?P<primary> primary)?"
    rb"(?: (?P<width>[1-9][0-9]{0,5})x(?P<height>[1-9][0-9]{0,5})"
    rb"\+(?P<x>[0-9]{1,6})\+(?P<y>[0-9]{1,6}))?"
    rb"(?: |$)"
)


def parse_xrandr_outputs(payload: bytes) -> tuple[XrandrOutput, ...]:
    """Parse bounded ``xrandr --query`` output without accepting ambiguity."""

    if not payload or len(payload) > MAX_COMMAND_OUTPUT_BYTES or b"\x00" in payload:
        raise HarnessError("xrandr_output_invalid")
    outputs: list[XrandrOutput] = []
    names: set[str] = set()
    for line in payload.splitlines():
        match = _OUTPUT_LINE.match(line)
        if match is None:
            continue
        try:
            name = match.group("name").decode("ascii")
        except UnicodeDecodeError as error:
            raise HarnessError("xrandr_output_invalid") from error
        if name in names:
            raise HarnessError("xrandr_output_duplicate")
        names.add(name)
        values = {
            key: int(match.group(key)) if match.group(key) is not None else None
            for key in ("width", "height", "x", "y")
        }
        connected = match.group("state") == b"connected"
        if not connected and any(value is not None for value in values.values()):
            raise HarnessError("xrandr_output_invalid")
        outputs.append(
            XrandrOutput(
                name=name,
                connected=connected,
                primary=match.group("primary") is not None,
                width=values["width"],
                height=values["height"],
                x=values["x"],
                y=values["y"],
            )
        )
    if not outputs:
        raise HarnessError("xrandr_output_invalid")
    return tuple(outputs)


def parse_window_ids(payload: bytes, property_name: bytes) -> tuple[int, ...]:
    """Parse one bounded X11 WINDOW-list property without retaining names."""

    if (
        not payload
        or len(payload) > MAX_COMMAND_OUTPUT_BYTES
        or b"\x00" in payload
        or re.fullmatch(rb"_[A-Z0-9_]{1,80}", property_name) is None
    ):
        raise HarnessError("x11_window_list_invalid")
    prefix = property_name + b"(WINDOW): window id # "
    if not payload.startswith(prefix) or not payload.endswith(b"\n"):
        raise HarnessError("x11_window_list_invalid")
    body = payload[len(prefix) : -1]
    if not body:
        return ()
    values = body.split(b", ")
    if len(values) > MAX_SURFACE_WINDOWS:
        raise HarnessError("x11_window_list_invalid")
    result: list[int] = []
    for value in values:
        if re.fullmatch(rb"0x[0-9a-f]{1,8}", value) is None:
            raise HarnessError("x11_window_list_invalid")
        parsed = int(value, 16)
        if parsed == 0 or parsed in result:
            raise HarnessError("x11_window_list_invalid")
        result.append(parsed)
    return tuple(result)


def parse_window_type_atoms(payload: bytes) -> tuple[bytes, ...]:
    """Parse bounded EWMH window types including Qt's reviewed KDE override hint."""

    prefix = b"_NET_WM_WINDOW_TYPE(ATOM) = "
    if (
        not payload.startswith(prefix)
        or not payload.endswith(b"\n")
        or len(payload) > 1024
        or b"\x00" in payload
    ):
        raise HarnessError("x11_window_type_invalid")
    atoms = tuple(payload[len(prefix) : -1].split(b", "))
    if not atoms or len(atoms) > 8 or len(set(atoms)) != len(atoms):
        raise HarnessError("x11_window_type_invalid")
    for atom in atoms:
        if (
            re.fullmatch(rb"_NET_WM_WINDOW_TYPE_[A-Z_]{1,64}", atom) is None
            and atom != b"_KDE_NET_WM_WINDOW_TYPE_OVERRIDE"
        ):
            raise HarnessError("x11_window_type_invalid")
    return atoms


def parse_cardinals(payload: bytes, property_name: bytes, count: int) -> tuple[int, ...]:
    """Parse one exact bounded CARDINAL property."""

    if (
        not payload
        or len(payload) > MAX_COMMAND_OUTPUT_BYTES
        or b"\x00" in payload
        or count < 1
        or count > 16
        or re.fullmatch(rb"_[A-Z0-9_]{1,80}", property_name) is None
    ):
        raise HarnessError("x11_cardinal_property_invalid")
    prefix = property_name + b"(CARDINAL) = "
    if not payload.startswith(prefix) or not payload.endswith(b"\n"):
        raise HarnessError("x11_cardinal_property_invalid")
    values = payload[len(prefix) : -1].split(b", ")
    if len(values) != count:
        raise HarnessError("x11_cardinal_property_invalid")
    result: list[int] = []
    for value in values:
        if re.fullmatch(rb"[0-9]{1,10}", value) is None:
            raise HarnessError("x11_cardinal_property_invalid")
        parsed = int(value)
        if parsed > 0xFFFF_FFFF:
            raise HarnessError("x11_cardinal_property_invalid")
        result.append(parsed)
    return tuple(result)


def parse_window_geometry(payload: bytes) -> WindowGeometry:
    """Extract geometry and mapping state without retaining a window title."""

    if not payload or len(payload) > MAX_COMMAND_OUTPUT_BYTES or b"\x00" in payload:
        raise HarnessError("x11_window_geometry_invalid")
    patterns = {
        "x": rb"^  Absolute upper-left X:\s+(-?[0-9]{1,7})$",
        "y": rb"^  Absolute upper-left Y:\s+(-?[0-9]{1,7})$",
        "width": rb"^  Width:\s+([1-9][0-9]{0,6})$",
        "height": rb"^  Height:\s+([1-9][0-9]{0,6})$",
        "border": rb"^  Border width:\s+([0-9]{1,6})$",
    }
    values: dict[str, int] = {}
    lines = payload.splitlines()
    for key, pattern in patterns.items():
        matches = [re.fullmatch(pattern, line) for line in lines]
        captures = [match.group(1) for match in matches if match is not None]
        if len(captures) != 1:
            raise HarnessError("x11_window_geometry_invalid")
        values[key] = int(captures[0])
    if (
        values["border"] != 0
        or lines.count(b"  Map State: IsViewable") != 1
        or lines.count(b"  Override Redirect State: no") != 1
    ):
        raise HarnessError("x11_window_geometry_invalid")
    return WindowGeometry(values["x"], values["y"], values["width"], values["height"])


def is_valid_unmapped_window_geometry(payload: bytes) -> bool:
    """Recognize a strict xwininfo record whose only invalidity is map state."""

    lines = payload.splitlines()
    unviewable_states = (
        b"  Map State: IsUnMapped",
        b"  Map State: IsUnviewable",
    )
    matches = [state for state in unviewable_states if lines.count(state) == 1]
    if len(matches) != 1 or lines.count(b"  Map State: IsViewable") != 0:
        return False
    normalized = payload.replace(matches[0], b"  Map State: IsViewable")
    try:
        parse_window_geometry(normalized)
    except HarnessError:
        return False
    return True


def _session_config(
    history_mode: str = "session", *, reduced_motion: bool = False
) -> str:
    """Return the complete private PD2 demonstration configuration."""

    if history_mode not in {"session", "persistent"}:
        raise HarnessError("private_session_history_mode_invalid")
    if type(reduced_motion) is not bool:
        raise HarnessError("private_session_reduced_motion_invalid")
    reduced_motion_value = "true" if reduced_motion else "false"
    return f"""schema_version = 2
profile = "lustre"

[appearance]
accent = "#4D7FFF"
transparency_enabled = true
blur_quality = "balanced"
reduced_motion = {reduced_motion_value}
high_contrast = false
text_scale = 1.0
cursor_theme = "default"
cursor_size_px = 24
icon_theme = "hicolor"

[panel]
edge = "bottom"
size_px = 48
autohide = "never"
grouping = "always"
clock_format = "locale"
output_policy = "all"
outputs = []

[launcher]
layout = "expanded"
search_providers = ["applications", "settings"]
recent_items = "local_only"

[notifications]
enabled = true
history = "{history_mode}"
default_timeout_ms = 5000
do_not_disturb = false

[desktop]
wallpaper_source = ""
background_color = "#182033"
wallpaper_mode = "fill"
icons_enabled = false

[integration]
export_gtk = true
export_qt = true
export_xsettings = true
export_portal = true

[accessibility]
focus_emphasis = "standard"
animation_scale = 1.0
minimum_target_size_px = 40

[keyboard]
menu_key_opens_launcher = true
focus_wraps = false

[developer]
diagnostics_enabled = false
mock_capability_overrides = []
"""


def _parse_accessibility_bus_address(payload: bytes) -> str:
    if not payload or len(payload) > MAX_ACCESSIBILITY_BUS_ADDRESS_BYTES:
        raise HarnessError("private_accessibility_bus_address_invalid")
    try:
        parsed = ast.literal_eval(payload.decode("ascii").strip())
    except (SyntaxError, ValueError, UnicodeDecodeError):
        raise HarnessError("private_accessibility_bus_address_invalid") from None
    if (
        type(parsed) is not tuple
        or len(parsed) != 1
        or type(parsed[0]) is not str
    ):
        raise HarnessError("private_accessibility_bus_address_invalid")
    address = parsed[0]
    permitted = frozenset(
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789_./%+-=,:@"
    )
    try:
        address_bytes = address.encode("ascii")
    except UnicodeEncodeError:
        raise HarnessError("private_accessibility_bus_address_invalid") from None
    if (
        not address.startswith(("unix:path=", "unix:abstract="))
        or len(address_bytes) > MAX_ACCESSIBILITY_BUS_ADDRESS_BYTES
        or any(character not in permitted for character in address)
    ):
        raise HarnessError("private_accessibility_bus_address_invalid")
    return address


@dataclass(frozen=True)
class LiveXorgConfig:
    """Exact executable inputs for a private Xorg-dummy demonstration run."""

    xorg: Path
    xrandr: Path
    openbox: Path
    xprop: Path
    xwininfo: Path
    dbus_daemon: Path
    dbus_monitor: Path
    gdbus: Path
    installed_executables: Mapping[str, Path]
    notify_send: Path | None = None
    xdotool: Path | None = None
    xwd: Path | None = None
    controlled_window_fixture: Path | None = None
    first_display: int = 120
    last_display: int = 159
    readiness_timeout: float = 8.0
    stop_timeout: float = 3.0

    def __post_init__(self) -> None:
        if not (1 <= self.first_display <= self.last_display <= 999):
            raise HarnessError("live_display_range_invalid")
        if self.readiness_timeout <= 0 or self.stop_timeout <= 0:
            raise HarnessError("live_timeout_invalid")


def _xorg_config() -> str:
    """Return the private deterministic two-output dummy-server configuration."""

    return """Section \"ServerFlags\"
    Option \"AutoAddDevices\" \"false\"
    Option \"AutoEnableDevices\" \"false\"
EndSection

Section \"Device\"
    Identifier \"PrismdrakeDummyDevice\"
    Driver \"dummy\"
    VideoRam 256000
EndSection

Section \"Monitor\"
    Identifier \"PrismdrakeDummyMonitor\"
    HorizSync 5.0-1000.0
    VertRefresh 5.0-200.0
    Modeline \"1600x900\" 118.25 1600 1696 1856 2112 900 903 908 934 -HSync +VSync
EndSection

Section \"Screen\"
    Identifier \"PrismdrakeDummyScreen\"
    Device \"PrismdrakeDummyDevice\"
    Monitor \"PrismdrakeDummyMonitor\"
    DefaultDepth 24
    SubSection \"Display\"
        Depth 24
        Modes \"1600x900\"
        Virtual 3200 1800
    EndSubSection
EndSection

Section \"ServerLayout\"
    Identifier \"PrismdrakeDummyLayout\"
    Screen \"PrismdrakeDummyScreen\"
EndSection
"""


class LiveXorgHarnessPort:
    """Fail-closed live adapter for the X11-owned subset of the PD2 contract."""

    _NOTIFICATION_STEPS = {
        "create_normal_notification": "create_normal",
        "create_replacement_notification": "replace_normal",
        "invoke_live_notification_action": "invoke_live_action",
        "open_notification_center": "open_center",
        "toggle_do_not_disturb": "enable_do_not_disturb",
        "verify_history_without_transient": "create_suppressed_normal",
        "verify_critical_bypass_policy": "create_critical_bypass",
        "clear_notification_history": "clear_history",
    }
    _SETTINGS_STEPS = {
        "open_quick_settings": "open_quick_settings",
        "switch_lustre_to_forge": "switch_lustre_to_forge",
        "verify_complete_settings_generation": "verify_complete_settings_generation",
        "switch_back_preserving_accessibility": "switch_back_preserving_accessibility",
    }

    _SUPPORTED_STEPS = {
        ("installed_workflow", "start_isolated_two_output_x11"),
        ("installed_workflow", "start_installed_prismdrake_session"),
        ("installed_workflow", "verify_component_readiness"),
        ("installed_workflow", "verify_desktop_wallpaper_both_outputs"),
        ("installed_workflow", "verify_panel_policy_both_outputs"),
        ("installed_workflow", "invoke_launcher_through_shellctl"),
        ("installed_workflow", "search_controlled_application"),
        ("installed_workflow", "pin_controlled_application"),
        ("installed_workflow", "launch_controlled_application"),
        ("installed_workflow", "launch_second_window"),
        ("installed_workflow", "verify_task_grouping"),
        ("installed_workflow", "activate_one_window"),
        ("installed_workflow", "minimize_one_window"),
        ("installed_workflow", "reactivate_one_window"),
        ("installed_workflow", "close_both_windows_through_panel"),
        ("installed_workflow", "verify_authoritative_task_removal"),
        ("installed_workflow", "create_normal_notification"),
        ("installed_workflow", "create_replacement_notification"),
        ("installed_workflow", "invoke_live_notification_action"),
        ("installed_workflow", "open_notification_center"),
        ("installed_workflow", "toggle_do_not_disturb"),
        ("installed_workflow", "verify_history_without_transient"),
        ("installed_workflow", "verify_critical_bypass_policy"),
        ("installed_workflow", "clear_notification_history"),
        ("installed_workflow", "open_quick_settings"),
        ("installed_workflow", "switch_lustre_to_forge"),
        ("installed_workflow", "verify_complete_settings_generation"),
        ("installed_workflow", "switch_back_preserving_accessibility"),
        ("installed_workflow", "verify_accessible_descriptions_states_and_backtab"),
        ("installed_workflow", "verify_output_migration_accessibility"),
        ("installed_workflow", "verify_settingsd_focus_recovery"),
        ("installed_workflow", "verify_notifyd_focus_recovery"),
        ("installed_workflow", "verify_shell_focus_recovery"),
        *(("installed_workflow", identifier) for identifier in LIFECYCLE_STEP_CATEGORIES),
        *(("tier_c_replay", identifier) for identifier in TIER_C_STEPS),
        ("failure_replay", "two_same_scale_outputs_at_startup"),
        ("failure_replay", "add_second_output_after_startup"),
        ("failure_replay", "remove_primary_output"),
        ("failure_replay", "select_new_primary_output"),
        ("failure_replay", "change_output_geometry"),
        ("failure_replay", "restart_settingsd"),
        ("failure_replay", "restart_notifyd"),
        ("failure_replay", "restart_shell"),
        ("failure_replay", "replace_or_restart_window_manager"),
        ("failure_replay", "run_without_compositor"),
        ("failure_replay", "run_with_missing_blur_capability"),
        ("failure_replay", "start_with_invalid_wallpaper"),
        ("failure_replay", "start_with_malformed_pin_state"),
        ("failure_replay", "start_with_malformed_persistent_history"),
        ("failure_replay", "start_when_another_notification_owner_exists"),
        ("failure_replay", "lose_x_server"),
        ("failure_replay", "enter_and_exit_safe_mode"),
    }

    def __init__(
        self,
        config: LiveXorgConfig,
        operations: Operations | None = None,
        accessibility: AccessibilityOperations | None = None,
        notification_epoch_factory: NotificationEpochFactory | None = None,
        settings_epoch_factory: SettingsEpochFactory | None = None,
        notification_failure_replay: NotificationFailureReplayPort | None = None,
        tier_c_trace_factory: TierCTraceFactory | None = None,
        session_transition_monitor_factory: SessionTransitionMonitorFactory | None = None,
    ) -> None:
        self._config = config
        self._operations = operations or SystemOperations()
        self._accessibility = accessibility or SystemAccessibilityOperations()
        self._notification_epoch_factory = (
            notification_epoch_factory or _create_notification_epoch
        )
        self._settings_epoch_factory = settings_epoch_factory or _create_settings_epoch
        self._notification_failure_replay = (
            notification_failure_replay or SystemNotificationFailureReplayPort()
        )
        self._tier_c_trace_factory = tier_c_trace_factory or X11TraceSupervisor
        self._session_transition_monitor_factory = (
            session_transition_monitor_factory or SystemSessionTransitionMonitor
        )
        self._runtime: Path | None = None
        self._display: str | None = None
        self._xorg: ChildProcess | None = None
        self._openbox: ChildProcess | None = None
        self._bus: ChildProcess | None = None
        self._session: ChildProcess | None = None
        self._bus_process_identity: ProcessIdentity | None = None
        self._bus_socket_identity: tuple[int, int] | None = None
        self._accessibility_bus_address: str | None = None
        self._desktop_surfaces: dict[int, WindowGeometry] | None = None
        self._surface_shell_identity: ProcessIdentity | None = None
        self._controlled_windows: dict[str, int] = {}
        self._controlled_processes: dict[str, ProcessIdentity] = {}
        self._common_action_acknowledgements: dict[str, int] = {}
        self._notification_epoch: _NotificationEpoch | None = None
        self._settings_epoch: _SettingsEpoch | None = None
        self._lifecycle_workflow: InstalledLifecycleWorkflow | None = None
        self._lifecycle_topology_generation = 1
        self._lifecycle_connection_epochs = {
            PRIMARY_OUTPUT: 1,
            SECONDARY_OUTPUT: 1,
        }
        self._lifecycle_shell_notifyd_available = False
        self._lifecycle_shell_notifyd_rebind_required = False
        self._lifecycle_survival_notification_prepared = False
        self._lifecycle_shutdown_started: float | None = None
        self._lifecycle_shutdown_components: LifecycleComponentObservation | None = None
        self._lifecycle_cleanup_ready = False
        self._cleanup_processes: tuple[tuple[ProcessIdentity, Path], ...] = ()
        self._cleanup_runtime: Path | None = None
        self._cleanup_bus_path: Path | None = None
        self._tier_c_trace: X11TraceSupervisor | None = None
        self._tier_c_workflow: TierCReplayWorkflow | None = None
        self._tier_c_surface: tuple[int, WindowGeometry] | None = None
        self._tier_c_identifier: str | None = None
        self._tier_c_logout_components: dict[str, ProcessIdentity] | None = None
        self._accessibility_references: dict[str, object] = {}
        self._accessibility_reference_shells: dict[str, ProcessIdentity] = {}

    def installed_executable_hashes(self) -> Mapping[str, str]:
        if tuple(self._config.installed_executables.keys()) != EXECUTABLE_NAMES:
            raise HarnessError("installed_executable_set_invalid")
        return {
            name: self._hash_installed_executable(
                self._config.installed_executables[name], name
            )
            for name in EXECUTABLE_NAMES
        }

    def shell_complete_presentation_ns(self) -> int:
        """Return shell process start through the already-observed panel endpoint."""

        identity = self._component_identity("prismdrake-shell")
        try:
            ticks_per_second = os.sysconf("SC_CLK_TCK")
            finished_ns = time.clock_gettime_ns(time.CLOCK_BOOTTIME)
        except (OSError, ValueError, AttributeError) as error:
            raise HarnessError("shell_start_clock_unavailable") from error
        if not isinstance(ticks_per_second, int) or ticks_per_second <= 0:
            raise HarnessError("shell_start_clock_unavailable")
        started_ns = identity.start_time * 1_000_000_000 // ticks_per_second
        duration_ns = finished_ns - started_ns
        if not 0 <= duration_ns <= 60_000_000_000:
            raise HarnessError("shell_presentation_latency_invalid")
        return duration_ns

    def notification_method_latency_ns(self) -> int:
        """Consume the exact standard Notify call-to-reply duration."""

        epoch = self._notification_epoch
        if epoch is None or not isinstance(epoch.sender, PersistentNotificationProbe):
            raise HarnessError("notification_latency_unavailable")
        try:
            return epoch.sender.take_notify_reply_latency_ns()
        except Exception:
            raise HarnessError("notification_latency_unavailable") from None

    def common_action_acknowledgement_ns(self, identifier: str) -> int:
        """Consume one exact action invocation-to-first-visible duration."""

        if identifier not in {
            "activate_one_window",
            "minimize_one_window",
            "reactivate_one_window",
        }:
            raise HarnessError("common_action_acknowledgement_unavailable")
        latency = self._common_action_acknowledgements.pop(identifier, None)
        if type(latency) is not int or not 0 <= latency <= 60_000_000_000:
            raise HarnessError("common_action_acknowledgement_unavailable")
        return latency

    def run_step(self, phase: str, identifier: str) -> Observation:
        if (phase, identifier) not in self._SUPPORTED_STEPS:
            raise HarnessError("live_step_not_implemented")
        if phase == "tier_c_replay":
            result = self._installed_tier_c().run_step(identifier)
            validate_tier_c_result(result)
            return Observation(True, True)
        if phase == "failure_replay":
            # Each replay scenario begins from a fresh isolated X11 epoch.  The
            # contract invokes scenarios consecutively and cleans only once at
            # the phase boundary, so carrying one scenario's topology into the
            # next would make the observation ambiguous.
            self.cleanup("failure_scenario_reset")
        if identifier == "start_isolated_two_output_x11":
            self._start_environment(two_outputs=True)
            self._require_topology(two_outputs=True)
        elif identifier == "start_installed_prismdrake_session":
            self._start_session(_session_config("persistent"))
        elif identifier == "verify_component_readiness":
            self._wait_for_session_readiness()
            # Bind the process-scoped AT-SPI observer to the exact live shell
            # before the first shell action. The two-output environment has two
            # valid panel controls with the same accessible name, so readiness
            # is intentionally application-scoped rather than control-scoped.
            self._wait_accessible_application()
            self.capture_process_ownership()
        elif identifier == "verify_desktop_wallpaper_both_outputs":
            self._require_desktop_wallpaper_surfaces()
        elif identifier == "verify_panel_policy_both_outputs":
            self._require_panel_surfaces()
        elif identifier == "invoke_launcher_through_shellctl":
            self._invoke_shellctl("launcher", "toggle")
            self._wait_accessible(
                ControlSelector("Search applications", "editable_text", require_focused=True),
                "launcher_search_focus_timeout",
            )
        elif identifier == "search_controlled_application":
            self._xdotool("type", "--clearmodifiers", "--delay", "0", CONTROLLED_APPLICATION_NAME)
            self._wait_accessible(
                ControlSelector(CONTROLLED_APPLICATION_NAME, "push_button"),
                "controlled_launcher_result_timeout",
            )
        elif identifier == "pin_controlled_application":
            self._focus_launcher_result("controlled_launcher_result_focus_timeout")
            self._xdotool("key", "--clearmodifiers", "Shift_L+F10")
            self._invoke_accessible(ControlSelector("Pin to panel", "menu_item", "Press"))
            self._xdotool("key", "--clearmodifiers", "Shift_L+F10")
            self._wait_accessible(
                ControlSelector("Unpin from panel", "menu_item"),
                "controlled_pin_publication_timeout",
            )
            self._xdotool("key", "--clearmodifiers", "Escape")
        elif identifier == "launch_controlled_application":
            self._invoke_accessible(
                ControlSelector(
                    CONTROLLED_APPLICATION_NAME,
                    "push_button",
                    "Press",
                    require_focused=True,
                )
            )
            self._controlled_windows[CONTROLLED_WINDOW_ONE] = self._wait_controlled_window(
                CONTROLLED_WINDOW_ONE
            )
            self._record_controlled_process(CONTROLLED_WINDOW_ONE)
        elif identifier == "launch_second_window":
            self._invoke_shellctl("launcher", "toggle")
            self._focus_launcher_result("controlled_launcher_result_focus_timeout")
            self._xdotool("key", "--clearmodifiers", "Shift_L+F10")
            self._invoke_accessible(ControlSelector("Second window", "menu_item", "Press"))
            self._controlled_windows[CONTROLLED_WINDOW_TWO] = self._wait_controlled_window(
                CONTROLLED_WINDOW_TWO
            )
            self._record_controlled_process(CONTROLLED_WINDOW_TWO)
            if len(set(self._controlled_windows.values())) != 2:
                raise HarnessError("controlled_window_identity_collision")
        elif identifier == "verify_task_grouping":
            self._focus_controlled_panel_task("controlled_task_focus_timeout")
            self._invoke_accessible(
                ControlSelector(
                    CONTROLLED_APPLICATION_NAME,
                    "push_button",
                    "Press",
                    require_focused=True,
                )
            )
            for title in (CONTROLLED_WINDOW_ONE, CONTROLLED_WINDOW_TWO):
                self._wait_accessible(
                    ControlSelector(f"Activate {title}", "menu_item"),
                    "controlled_task_group_timeout",
                )
        elif identifier == "activate_one_window":
            self._invoke_common_action(
                identifier,
                ControlSelector(f"Activate {CONTROLLED_WINDOW_ONE}", "menu_item", "Press"),
            )
            self._wait_window_active(CONTROLLED_WINDOW_ONE, minimized=False)
        elif identifier == "minimize_one_window":
            self._open_controlled_group()
            self._invoke_common_action(
                identifier,
                ControlSelector(f"Minimize {CONTROLLED_WINDOW_ONE}", "menu_item", "Press"),
            )
            self._wait_window_active(CONTROLLED_WINDOW_ONE, minimized=True)
        elif identifier == "reactivate_one_window":
            self._open_controlled_group()
            self._invoke_common_action(
                identifier,
                ControlSelector(f"Activate {CONTROLLED_WINDOW_ONE}", "menu_item", "Press"),
            )
            self._wait_window_active(CONTROLLED_WINDOW_ONE, minimized=False)
        elif identifier == "close_both_windows_through_panel":
            self._open_controlled_group()
            self._invoke_accessible(
                ControlSelector(f"Close {CONTROLLED_WINDOW_TWO}", "menu_item", "Press")
            )
            self._wait_window_absent(CONTROLLED_WINDOW_TWO)
            self._focus_controlled_panel_task("controlled_task_focus_timeout")
            self._xdotool("key", "--clearmodifiers", "Shift_L+F10")
            self._invoke_accessible(
                ControlSelector(
                    f"Close {CONTROLLED_APPLICATION_NAME}", "menu_item", "Press"
                )
            )
            self._wait_window_absent(CONTROLLED_WINDOW_ONE)
        elif identifier == "verify_authoritative_task_removal":
            for title in (CONTROLLED_WINDOW_ONE, CONTROLLED_WINDOW_TWO):
                if self._controlled_window_ids(title):
                    raise HarnessError("controlled_task_removal_timeout")
            if not self._reject_stale_accessible(
                ControlSelector(
                    f"Close {CONTROLLED_APPLICATION_NAME}", "menu_item", "Press"
                )
            ):
                raise HarnessError("stale_task_action_accepted")
            self._controlled_windows.clear()
        elif identifier in self._NOTIFICATION_STEPS:
            self._run_notification_step(identifier)
            if identifier == "open_notification_center":
                self._lifecycle_shell_notifyd_available = True
        elif identifier in self._SETTINGS_STEPS:
            self._run_settings_step(identifier)
        elif identifier == "verify_accessible_descriptions_states_and_backtab":
            self._verify_accessible_metadata_and_backtab()
        elif identifier == "verify_output_migration_accessibility":
            self._verify_output_migration_accessibility()
        elif identifier == "verify_settingsd_focus_recovery":
            self._verify_accessibility_reference(
                "settingsd", stale=True, replacement_shell=False, reacquire=True
            )
        elif identifier == "verify_notifyd_focus_recovery":
            self._verify_accessibility_reference(
                "notifyd", stale=False, replacement_shell=False, reacquire=False
            )
        elif identifier == "verify_shell_focus_recovery":
            self._verify_accessibility_reference(
                "shell", stale=True, replacement_shell=True, reacquire=True
            )
        elif phase == "installed_workflow" and identifier in LIFECYCLE_STEP_CATEGORIES:
            if identifier == "verify_clean_test_environment_cleanup":
                raise HarnessError("lifecycle_cleanup_step_is_adapter_owned")
            restart_key = {
                "restart_settingsd": "settingsd",
                "restart_notifyd": "notifyd",
                "restart_shell": "shell",
            }.get(identifier)
            if restart_key is not None:
                self._focus_and_capture_reference(restart_key)
            result = self._installed_lifecycle().run_step(identifier)
            validate_lifecycle_workflow_result(result)
            if identifier == "verify_unrelated_processes_survive":
                self._lifecycle_cleanup_ready = True
        elif identifier == "two_same_scale_outputs_at_startup":
            self._start_environment(two_outputs=True)
            self._require_topology(two_outputs=True)
            self._start_session()
            self._wait_for_session_readiness()
            self._require_desktop_wallpaper_surfaces()
            self._require_panel_surfaces()
        elif identifier == "add_second_output_after_startup":
            self._start_environment(two_outputs=False)
            self._require_topology(two_outputs=False)
            self._start_session()
            self._wait_for_session_readiness()
            self._require_failure_replay_surfaces(
                {WindowGeometry(0, 0, OUTPUT_WIDTH, OUTPUT_HEIGHT)},
                {WindowGeometry(0, OUTPUT_HEIGHT - PANEL_HEIGHT, OUTPUT_WIDTH, PANEL_HEIGHT)},
            )
            self._enable_secondary()
            self._require_topology(two_outputs=True)
            self._require_failure_replay_surfaces(
                {
                    WindowGeometry(0, 0, OUTPUT_WIDTH, OUTPUT_HEIGHT),
                    WindowGeometry(OUTPUT_WIDTH, 0, OUTPUT_WIDTH, OUTPUT_HEIGHT),
                },
                {
                    WindowGeometry(0, OUTPUT_HEIGHT - PANEL_HEIGHT, OUTPUT_WIDTH, PANEL_HEIGHT),
                    WindowGeometry(
                        OUTPUT_WIDTH,
                        OUTPUT_HEIGHT - PANEL_HEIGHT,
                        OUTPUT_WIDTH,
                        PANEL_HEIGHT,
                    ),
                },
            )
        elif identifier == "remove_primary_output":
            self._start_environment(two_outputs=True)
            self._require_topology(two_outputs=True)
            self._start_session()
            self._wait_for_session_readiness()
            self._require_desktop_wallpaper_surfaces()
            self._require_panel_surfaces()
            self._xrandr("--output", PRIMARY_OUTPUT, "--off")
            self._require_only_secondary()
            self._require_failure_replay_surfaces(
                {WindowGeometry(0, 0, OUTPUT_WIDTH, OUTPUT_HEIGHT)},
                {
                    WindowGeometry(
                        0,
                        OUTPUT_HEIGHT - PANEL_HEIGHT,
                        OUTPUT_WIDTH,
                        PANEL_HEIGHT,
                    )
                },
            )
        elif identifier == "select_new_primary_output":
            self._start_environment(two_outputs=True)
            self._require_topology(two_outputs=True)
            self._start_session()
            self._wait_for_session_readiness()
            self._require_desktop_wallpaper_surfaces()
            self._require_panel_surfaces()
            self._xrandr("--output", SECONDARY_OUTPUT, "--primary")
            self._require_topology(two_outputs=True, primary=SECONDARY_OUTPUT)
            self._require_failure_replay_surfaces(
                {
                    WindowGeometry(0, 0, OUTPUT_WIDTH, OUTPUT_HEIGHT),
                    WindowGeometry(OUTPUT_WIDTH, 0, OUTPUT_WIDTH, OUTPUT_HEIGHT),
                },
                {
                    WindowGeometry(0, OUTPUT_HEIGHT - PANEL_HEIGHT, OUTPUT_WIDTH, PANEL_HEIGHT),
                    WindowGeometry(
                        OUTPUT_WIDTH,
                        OUTPUT_HEIGHT - PANEL_HEIGHT,
                        OUTPUT_WIDTH,
                        PANEL_HEIGHT,
                    ),
                },
            )
        elif identifier == "change_output_geometry":
            self._start_environment(two_outputs=True)
            self._require_topology(two_outputs=True)
            self._start_session()
            self._wait_for_session_readiness()
            self._require_desktop_wallpaper_surfaces()
            self._require_panel_surfaces()
            self._xrandr("--output", SECONDARY_OUTPUT, "--pos", "0x900")
            self._require_topology(two_outputs=True, secondary_position=(0, 900))
            self._require_failure_replay_surfaces(
                {
                    WindowGeometry(0, 0, OUTPUT_WIDTH, OUTPUT_HEIGHT),
                    WindowGeometry(0, OUTPUT_HEIGHT, OUTPUT_WIDTH, OUTPUT_HEIGHT),
                },
                {
                    WindowGeometry(0, OUTPUT_HEIGHT - PANEL_HEIGHT, OUTPUT_WIDTH, PANEL_HEIGHT),
                    WindowGeometry(
                        0,
                        2 * OUTPUT_HEIGHT - PANEL_HEIGHT,
                        OUTPUT_WIDTH,
                        PANEL_HEIGHT,
                    ),
                },
            )
        elif identifier == "restart_settingsd":
            self._prepare_restart_state()
            self._restart_component("prismdrake-settingsd")
            self._invoke_shellctl("quick-settings", "open")
            self._wait_accessible(
                ControlSelector("Quick settings", "pane"),
                "settings_owner_reacquisition_timeout",
            )
            self._xdotool("key", "--clearmodifiers", "Escape")
            self._wait_accessible_absent(
                ControlSelector("Quick settings", "pane"),
                "settings_surface_dismissal_timeout",
            )
        elif identifier == "restart_notifyd":
            self._prepare_restart_state()
            self._restart_component("prismdrake-notifyd")
            self._invoke_shellctl("notifications", "open")
            self._wait_accessible(
                ControlSelector("Notification center", "panel"),
                "notification_owner_reacquisition_timeout",
            )
        elif identifier == "restart_shell":
            self._prepare_restart_state()
            notifyd = self._component_identity("prismdrake-notifyd")
            self._restart_component("prismdrake-shell")
            if self._component_identity("prismdrake-notifyd") != notifyd:
                raise HarnessError("notifyd_replaced_during_shell_restart")
            self._focus_controlled_panel_task("pin_state_reacquisition_timeout")
            self._invoke_shellctl("panel", "focus")
            self._wait_accessible(
                ControlSelector("Open applications", "push_button", require_focused=True),
                "shell_control_reacquisition_timeout",
            )
        elif identifier == "replace_or_restart_window_manager":
            self._prepare_restart_state()
            components = self._component_identities()
            windows = dict(self._controlled_windows)
            if self._openbox is None or not self._operations.stop(
                self._openbox, self._config.stop_timeout
            ):
                raise HarnessError("window_manager_stop_failed")
            previous = self._openbox
            self._openbox = None
            self._start_openbox()
            if self._openbox is previous:
                raise HarnessError("window_manager_replacement_invalid")
            self._require_topology(two_outputs=True)
            self._require_controlled_windows_unchanged(windows)
            self._wait_panel_surfaces_unchanged(
                "window_manager_surface_recovery_timeout"
            )
            self._focus_controlled_panel_task("window_manager_task_focus_timeout")
            self._invoke_accessible(
                ControlSelector(
                    CONTROLLED_APPLICATION_NAME,
                    "push_button",
                    "Press",
                    require_focused=True,
                )
            )
            self._wait_window_active(CONTROLLED_WINDOW_ONE, minimized=False)
            if self._component_identities() != components:
                raise HarnessError("prismdrake_child_changed_during_window_manager_restart")
        elif identifier == "run_without_compositor":
            self._prepare_restart_state()
            self._require_fresh_display_fallback_surfaces()
        elif identifier == "run_with_missing_blur_capability":
            self._prepare_restart_state()
            self._require_fresh_display_fallback_surfaces()
            self._require_configuration_value(
                b'blur_quality = "balanced"\n', "blur_preference_observation_invalid"
            )
        elif identifier == "start_with_invalid_wallpaper":
            configuration = _session_config().replace(
                'wallpaper_source = ""',
                'wallpaper_source = "wallpapers/missing-controlled-image.png"',
            )
            self._start_environment(two_outputs=True)
            self._start_session(configuration)
            self._wait_for_session_readiness()
            self._require_configuration_value(
                b'wallpaper_source = "wallpapers/missing-controlled-image.png"\n',
                "invalid_wallpaper_preference_observation_invalid",
            )
            self._require_desktop_wallpaper_surfaces()
            self._require_panel_surfaces()
            self._component_identities()
            self._require_shell_primary_control()
        elif identifier == "start_with_malformed_pin_state":
            self._start_environment(two_outputs=True)
            malformed = self._write_malformed_pin_state()
            self._start_session()
            self._wait_for_session_readiness()
            self._require_desktop_wallpaper_surfaces()
            self._require_panel_surfaces()
            if self._read_private_regular(malformed, 1024) != b"{ malformed\n":
                raise HarnessError("malformed_pin_state_not_retained")
            for step in (
                "invoke_launcher_through_shellctl",
                "search_controlled_application",
                "pin_controlled_application",
            ):
                self.run_step("installed_workflow", step)
            self._require_recovered_pin_state(malformed)
        elif identifier == "start_with_malformed_persistent_history":
            self._replay_malformed_notification_history()
        elif identifier == "start_when_another_notification_owner_exists":
            self._replay_conflicting_notification_owner()
        elif identifier == "lose_x_server":
            self._prepare_restart_state()
            previous_shell = self._component_identity("prismdrake-shell")
            if self._xorg is None or not self._operations.stop(
                self._xorg, self._config.stop_timeout
            ):
                raise HarnessError("xorg_stop_failed")
            self._xorg = None
            result = self._run_x11((str(self._config.xrandr), "--query"))
            if result.returncode == 0:
                raise HarnessError("x_server_loss_unobserved")
            self._wait_for_x_server_loss_response(previous_shell)
            self.cleanup("x_server_loss_fatal_cleanup")
        elif identifier == "enter_and_exit_safe_mode":
            self._start_environment(two_outputs=True)
            self._start_session()
            self._wait_for_session_readiness()
            self._exhaust_shell_restart_budget()
            self._wait_for_safe_mode()
            self._require_desktop_wallpaper_surfaces(allow_safe_mode=True)
            self._wait_panel_surfaces_unchanged(
                "safe_mode_panel_surface_timeout",
                allow_safe_mode=True,
                primary_only=True,
            )
            self.cleanup("safe_mode_epoch")
            self._start_environment(two_outputs=True)
            self._start_session()
            self._wait_for_session_readiness()
            if self._session_marker_state() != "ready":
                raise HarnessError("fresh_normal_session_not_ready")
            self._require_desktop_wallpaper_surfaces()
            self._require_panel_surfaces()
            self._require_shell_primary_control()
        return Observation(True, True)

    def _replay_malformed_notification_history(self) -> None:
        try:
            self._start_environment(two_outputs=True)
            if self._runtime is None:
                raise HarnessError("live_environment_not_started")
            state_root = self._runtime / "state"
            fixture = create_malformed_history_fixture(state_root)
            configuration = _session_config("persistent")
            self._start_session(configuration)
            self._wait_for_session_readiness()
            before = self._component_identities()
            evidence = self._notification_failure_replay.verify_malformed_history(
                fixture,
                dict(self.probe_environment()),
                self._notification_failure_endpoint(),
            )
            validate_failure_evidence(evidence)
            fixture.verify_retained()
            if self._component_identities() != before:
                raise HarnessError("live_notification_failure_sibling_changed")
            self._require_desktop_wallpaper_surfaces()
            self._require_panel_surfaces()
            self._invoke_shellctl("notifications", "open")
            self._wait_accessible(
                ControlSelector("Notification center", "panel"),
                "malformed_history_shell_unusable",
            )
        except Exception:
            raise HarnessError("live_malformed_notification_history_failed") from None

    def _wait_for_x_server_loss_response(self, previous_shell: ProcessIdentity) -> None:
        if self._session is None:
            raise HarnessError("prismdrake_session_not_started")
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            if self._session.poll() is not None:
                return
            try:
                shell = self._component_identity("prismdrake-shell")
            except HarnessError as error:
                if str(error) == "component_identity_invalid":
                    return
                raise
            if shell != previous_shell:
                return
            self._operations.pause(0.01)
        raise HarnessError("x_server_loss_session_response_timeout")

    def _replay_conflicting_notification_owner(self) -> None:
        try:
            self._start_environment(two_outputs=True)
            self._start_private_bus()
            executable = self._config.installed_executables.get("prismdrake-notifyd")
            if executable is None:
                raise HarnessError("component_identity_unavailable")
            evidence = self._notification_failure_replay.verify_conflicting_owner(
                executable,
                dict(self._private_bus_environment()),
                self._notification_failure_endpoint(),
                self._start_notification_conflict_session,
                self._notification_conflict_sibling_identities,
                self._verify_active_notification_conflict,
            )
            validate_failure_evidence(evidence)
        except Exception:
            raise HarnessError("live_notification_owner_conflict_failed") from None

    def _notification_failure_endpoint(self) -> PrivateBusEndpoint:
        socket_identity = self._bus_socket_identity
        if socket_identity is None:
            raise HarnessError("private_session_bus_not_started")
        return PrivateBusEndpoint(
            self._private_bus_address(), socket_identity[0], socket_identity[1]
        )

    def _start_notification_conflict_session(self) -> None:
        self._start_session(private_bus_started=True)
        self._wait_for_session_readiness()
        if self._session_marker_state() != "ready":
            raise HarnessError("notification_failure_session_not_ready")

    def _notification_conflict_sibling_identities(
        self,
    ) -> Mapping[str, tuple[int, int]]:
        if self._session is None or self._session.poll() is not None:
            raise HarnessError("notification_failure_session_invalid")
        notifyd = self._config.installed_executables.get("prismdrake-notifyd")
        if notifyd is None or self._operations.descendants(self._session, notifyd):
            raise HarnessError("notification_failure_notifyd_not_degraded")
        result = {
            name: self._component_identity(name)
            for name in ("prismdrake-settingsd", "prismdrake-shell")
        }
        return {
            name: (identity.pid, identity.start_time)
            for name, identity in result.items()
        }

    def _verify_active_notification_conflict(self) -> None:
        before = self._notification_conflict_sibling_identities()
        self._require_desktop_wallpaper_surfaces()
        self._require_panel_surfaces()
        self._require_shellctl_unavailable("notifications", "open")
        self._invoke_shellctl("launcher", "toggle")
        self._wait_accessible(
            ControlSelector(
                "Search applications", "editable_text", require_focused=True
            ),
            "notification_failure_launcher_focus_timeout",
        )
        self._operations.pause(0.25)
        if self._notification_conflict_sibling_identities() != before:
            raise HarnessError("notification_failure_sibling_changed")

    def _run_notification_step(self, identifier: str) -> None:
        method_name = self._NOTIFICATION_STEPS.get(identifier)
        if method_name is None:
            raise HarnessError("live_notification_step_not_reviewed")
        workflow = self._notification_workflow_epoch().workflow
        try:
            operation = getattr(workflow, method_name)
            result = operation()
            validate_workflow_result(result)
            if identifier == "clear_notification_history":
                workflow.verify_notify_send(self._invoke_notify_send)
        except Exception:
            raise HarnessError("live_notification_workflow_failed") from None
        if not isinstance(result, WorkflowResult):
            raise HarnessError("live_notification_workflow_failed")

    def _invoke_notify_send(self) -> None:
        executable = self._config.notify_send
        if executable is None:
            raise HarnessError("notify_send_executable_unavailable")
        result = self._operations.run(
            (
                str(executable),
                "--app-name",
                APPLICATION_NAME,
                "--urgency",
                "critical",
                "--expire-time",
                "0",
                CRITICAL_SUMMARY,
            ),
            self.probe_environment(),
            3.0,
        )
        if result.returncode != 0 or result.stdout or result.stderr:
            raise HarnessError("notify_send_invocation_failed")

    def _notification_workflow_epoch(self) -> _NotificationEpoch:
        if self._notification_epoch is not None:
            return self._notification_epoch
        environment = dict(self.probe_environment())
        socket_identity = self._bus_socket_identity
        if socket_identity is None:
            raise HarnessError("private_session_bus_not_started")
        endpoint = PrivateBusEndpoint(
            self._private_bus_address(), socket_identity[0], socket_identity[1]
        )
        surface = LiveNotificationSurfaceAdapter(
            self._accessibility,
            self._shell_identity,
            self.probe_environment,
            self._operations.monotonic,
            self._operations.pause,
            lambda: self._xdotool("key", "--clearmodifiers", "Escape"),
        )
        try:
            epoch = self._notification_epoch_factory(environment, endpoint, surface)
        except Exception:
            raise HarnessError("live_notification_epoch_initialization_failed") from None
        if not isinstance(epoch, _NotificationEpoch):
            raise HarnessError("live_notification_epoch_initialization_failed")
        self._notification_epoch = epoch
        return epoch

    def _run_settings_step(self, identifier: str) -> None:
        if identifier not in self._SETTINGS_STEPS:
            raise HarnessError("live_settings_step_not_reviewed")
        workflow = self._settings_workflow_epoch().workflow
        try:
            result = workflow.run_step(identifier)
        except Exception:
            raise HarnessError("live_settings_workflow_failed") from None
        if (
            not isinstance(result, SettingsWorkflowResult)
            or result.evidence().get("status") != "passed"
        ):
            raise HarnessError("live_settings_workflow_failed")

    def _settings_workflow_epoch(self) -> _SettingsEpoch:
        if self._settings_epoch is not None:
            return self._settings_epoch
        environment = dict(self.probe_environment())
        socket_identity = self._bus_socket_identity
        if socket_identity is None:
            raise HarnessError("private_session_bus_not_started")
        endpoint = PrivateBusEndpoint(
            self._private_bus_address(), socket_identity[0], socket_identity[1]
        )
        accessibility = LiveSettingsAccessibilityAdapter(
            self._accessibility,
            self._shell_identity,
            self.probe_environment,
            self._operations.monotonic,
            self._operations.pause,
            self._config.readiness_timeout,
        )
        surfaces = LiveSettingsSurfaceAdapter(
            SETTINGS_WORKFLOW_CONFIG,
            self._settings_surface_window,
            self._capture_settings_fingerprint,
            self._operations.pause,
        )
        try:
            epoch = self._settings_epoch_factory(
                environment, endpoint, accessibility, surfaces
            )
        except Exception:
            raise HarnessError("live_settings_epoch_initialization_failed") from None
        if not isinstance(epoch, _SettingsEpoch):
            raise HarnessError("live_settings_epoch_initialization_failed")
        self._settings_epoch = epoch
        return epoch

    def _capture_settings_fingerprint(
        self, window: int, points: Sequence[tuple[int, int]]
    ) -> bytes:
        if self._config.xwd is None or self._runtime is None or self._display is None:
            raise HarnessError("live_settings_xwd_unavailable")
        return capture_window_fingerprint(
            self._config.xwd,
            window,
            self._runtime,
            {"DISPLAY": self._display},
            sample_points=points,
        )

    def _settings_surface_window(self, surface: str) -> int:
        if surface in {"panel_primary", "panel_secondary"}:
            if self._desktop_surfaces is None or self._surface_shell_identity is None:
                self._require_desktop_wallpaper_surfaces()
            self._require_panel_surfaces()
        shell = self._shell_identity()
        if surface in {"panel_primary", "panel_secondary"}:
            panels = self._observe_surface_windows(b"_NET_WM_WINDOW_TYPE_DOCK", shell)
            x = 0 if surface == "panel_primary" else OUTPUT_WIDTH
            expected = WindowGeometry(
                x, OUTPUT_HEIGHT - PANEL_HEIGHT, OUTPUT_WIDTH, PANEL_HEIGHT
            )
            matches = [window for window, geometry in panels.items() if geometry == expected]
            if len(matches) != 1:
                raise HarnessError("live_settings_surface_window_invalid")
            return matches[0]
        if surface != "quick_settings":
            raise HarnessError("live_settings_surface_not_reviewed")
        windows = self._visible_named_window_ids(QUICK_SETTINGS_WINDOW_TITLE)
        if len(windows) != 1:
            raise HarnessError("live_settings_surface_window_invalid")
        window = windows[0]
        if self._window_cardinals(window, b"_NET_WM_PID", 1) != (shell.pid,):
            raise HarnessError("live_settings_surface_owner_invalid")
        geometry_result = self._run_x11(
            (str(self._config.xwininfo), "-id", f"0x{window:x}", "-stats")
        )
        if geometry_result.returncode != 0:
            raise HarnessError("live_settings_surface_window_invalid")
        geometry = parse_window_geometry(geometry_result.stdout)
        if (
            geometry.width != 380
            or not 100 <= geometry.height <= OUTPUT_HEIGHT - PANEL_HEIGHT
            or geometry.x != OUTPUT_WIDTH - geometry.width
            or geometry.y != OUTPUT_HEIGHT - PANEL_HEIGHT - geometry.height
        ):
            raise HarnessError("live_settings_surface_window_invalid")
        return window

    def _visible_named_window_ids(self, title: str) -> tuple[int, ...]:
        if title not in TIER_C_WINDOW_TITLES or self._config.xdotool is None:
            raise HarnessError("live_settings_surface_query_invalid")
        result = self._operations.run(
            (
                str(self._config.xdotool),
                "search",
                "--onlyvisible",
                "--name",
                f"^{title}$",
            ),
            self._private_bus_environment(),
            3.0,
        )
        if result.returncode == 1 and not result.stdout and not result.stderr:
            return ()
        if result.returncode != 0 or result.stderr or len(result.stdout) > 1024:
            raise HarnessError("live_settings_surface_query_invalid")
        identifiers: list[int] = []
        for line in result.stdout.splitlines():
            if re.fullmatch(rb"[1-9][0-9]{0,19}", line) is None:
                raise HarnessError("live_settings_surface_query_invalid")
            identifier = int(line)
            if identifier > 0xFFFFFFFF or identifier in identifiers:
                raise HarnessError("live_settings_surface_query_invalid")
            identifiers.append(identifier)
        if len(identifiers) > 1:
            raise HarnessError("live_settings_surface_query_invalid")
        return tuple(identifiers)

    def _focus_named_shell_surface(self, title: str) -> None:
        windows = self._visible_named_window_ids(title)
        if len(windows) != 1:
            raise HarnessError("shell_surface_focus_target_invalid")
        window = windows[0]
        shell = self._shell_identity()
        if self._window_cardinals(window, b"_NET_WM_PID", 1) != (shell.pid,):
            raise HarnessError("shell_surface_focus_owner_invalid")
        self._xdotool("windowfocus", "--sync", str(window))
        focused = self._xdotool("getwindowfocus")
        if (
            focused.stderr
            or len(focused.stdout) > 32
            or re.fullmatch(rb"[1-9][0-9]{0,19}\n?", focused.stdout) is None
            or int(focused.stdout) != window
        ):
            raise HarnessError("shell_surface_focus_failed")

    def _focus_launcher_result(self, timeout_error: str) -> None:
        self._focus_named_shell_surface(LAUNCHER_WINDOW_TITLE)
        self._xdotool("key", "--clearmodifiers", "Down")
        self._wait_accessible(
            ControlSelector(
                CONTROLLED_APPLICATION_NAME, "push_button", require_focused=True
            ),
            timeout_error,
        )

    def _installed_tier_c(self) -> TierCReplayWorkflow:
        if self._tier_c_workflow is not None:
            return self._tier_c_workflow
        self._start_environment(two_outputs=True, tier_c=True)
        if self._tier_c_trace is None:
            raise HarnessError("tier_c_trace_start_failed")
        self._start_session(_session_config(reduced_motion=True))
        self._wait_for_session_readiness()
        self._require_desktop_wallpaper_surfaces()
        self._require_panel_surfaces()
        self._tier_c_workflow = TierCReplayWorkflow(self, self._tier_c_trace)
        return self._tier_c_workflow

    def _tier_c_named_surface(self, title: str) -> tuple[int, WindowGeometry]:
        windows = self._visible_named_window_ids(title)
        if len(windows) != 1:
            raise HarnessError("tier_c_surface_window_invalid")
        window = windows[0]
        shell = self._shell_identity()
        if self._window_cardinals(window, b"_NET_WM_PID", 1) != (shell.pid,):
            raise HarnessError("tier_c_surface_owner_invalid")
        return window, self._tier_c_window_geometry(window)

    def _tier_c_window_geometry(self, window: int) -> WindowGeometry:
        result = self._run_x11(
            (str(self._config.xwininfo), "-id", f"0x{window:x}", "-stats")
        )
        depth_lines = [
            line
            for line in result.stdout.splitlines()
            if re.fullmatch(rb"  Depth: (?:24|32)", line) is not None
        ]
        if result.returncode != 0 or len(depth_lines) != 1:
            raise HarnessError("tier_c_surface_depth_invalid")
        return parse_window_geometry(result.stdout)

    def _tier_c_panel_surface(self) -> tuple[int, WindowGeometry]:
        self._require_panel_surfaces()
        shell = self._shell_identity()
        panels = self._observe_surface_windows(b"_NET_WM_WINDOW_TYPE_DOCK", shell)
        expected = WindowGeometry(
            0, OUTPUT_HEIGHT - PANEL_HEIGHT, OUTPUT_WIDTH, PANEL_HEIGHT
        )
        matches = [
            (window, geometry)
            for window, geometry in panels.items()
            if geometry == expected
        ]
        if len(matches) != 1:
            raise HarnessError("tier_c_surface_window_invalid")
        return matches[0]

    def _tier_c_desktop_surface(self) -> tuple[int, WindowGeometry]:
        self._require_desktop_wallpaper_surfaces()
        expected = WindowGeometry(0, 0, OUTPUT_WIDTH, OUTPUT_HEIGHT)
        matches = [
            (window, geometry)
            for window, geometry in (self._desktop_surfaces or {}).items()
            if geometry == expected
        ]
        if len(matches) != 1:
            raise HarnessError("tier_c_surface_window_invalid")
        return matches[0]

    def _prepare_tier_c_controlled_application(self) -> None:
        if self._config.controlled_window_fixture is None:
            raise HarnessError("controlled_fixture_unavailable")
        if CONTROLLED_WINDOW_ONE not in self._controlled_windows:
            self._controlled_windows[CONTROLLED_WINDOW_ONE] = self._wait_controlled_window(
                CONTROLLED_WINDOW_ONE
            )
        if CONTROLLED_WINDOW_ONE not in self._controlled_processes:
            self._record_controlled_process(CONTROLLED_WINDOW_ONE)

    def prepare_step(self, contract: TierCStepContract) -> TierCPortResult:
        if self._tier_c_trace is None or self._tier_c_workflow is None:
            raise HarnessError("tier_c_environment_not_started")
        identifier = contract.identifier
        if identifier not in TIER_C_STEPS or self._tier_c_identifier is not None:
            raise HarnessError("tier_c_step_state_invalid")

        if identifier == "desktop":
            self._invoke_shellctl("panel", "focus")
            surface = self._tier_c_desktop_surface()
        elif identifier == "panel":
            self._invoke_shellctl("panel", "focus")
            surface = self._tier_c_panel_surface()
        elif identifier == "launcher":
            self._invoke_shellctl("launcher", "toggle")
            self._wait_accessible(
                ControlSelector(
                    "Search applications", "editable_text", require_focused=True
                ),
                "tier_c_launcher_focus_timeout",
            )
            self._xdotool(
                "type",
                "--clearmodifiers",
                "--delay",
                "0",
                CONTROLLED_APPLICATION_NAME,
            )
            self._wait_accessible(
                ControlSelector(CONTROLLED_APPLICATION_NAME, "push_button"),
                "tier_c_controlled_result_timeout",
            )
            surface = self._tier_c_named_surface(LAUNCHER_WINDOW_TITLE)
        elif identifier == "task_actions":
            self._prepare_tier_c_controlled_application()
            self._focus_controlled_panel_task("tier_c_task_focus_timeout")
            surface = self._tier_c_panel_surface()
        elif identifier == "notification":
            self._run_notification_step("create_normal_notification")
            self._invoke_shellctl("panel", "focus")
            surface = self._tier_c_named_surface(NOTIFICATION_WINDOW_TITLE)
        elif identifier == "notification_center":
            self._invoke_shellctl("notifications", "open")
            self._wait_accessible(
                ControlSelector("Notification center", "pane"),
                "tier_c_notification_center_timeout",
            )
            surface = self._tier_c_named_surface(NOTIFICATION_CENTER_WINDOW_TITLE)
        elif identifier == "quick_settings":
            self._invoke_shellctl("quick-settings", "open")
            self._wait_accessible(
                ControlSelector("Quick settings", "pane"),
                "tier_c_quick_settings_timeout",
            )
            surface = self._tier_c_named_surface(QUICK_SETTINGS_WINDOW_TITLE)
        elif identifier == "logout":
            self._invoke_shellctl("session", "logout")
            self._wait_accessible(
                ControlSelector("Log out of Prismdrake", "dialog"),
                "tier_c_logout_timeout",
            )
            surface = self._tier_c_named_surface(LOGOUT_WINDOW_TITLE)
            self._tier_c_logout_components = self._component_identities()
        else:
            raise HarnessError("tier_c_step_state_invalid")

        if identifier in {
            "launcher",
            "notification",
            "notification_center",
            "quick_settings",
            "logout",
        } and b"_NET_WM_STATE_SKIP_TASKBAR" not in self._window_state_atoms(surface[0]):
            raise HarnessError(
                "tier_c_transient_task_policy_invalid"
                if identifier == "notification"
                else "tier_c_singleton_task_policy_invalid"
            )

        self._tier_c_identifier = identifier
        self._tier_c_surface = surface
        return TierCPortResult("tier_c_step_prepared")

    def send_reviewed_keys(
        self, identifier: str, keys: tuple[str, ...]
    ) -> TierCKeyboardObservation:
        if identifier != self._tier_c_identifier or not keys:
            raise HarnessError("tier_c_keyboard_state_invalid")
        if identifier == "notification":
            if keys != ("TAB", "TAB", "ENTER", "SHIFT_TAB"):
                raise HarnessError("tier_c_keyboard_mapping_invalid")
            self._xdotool("key", "--clearmodifiers", "Tab")
            self.observe_atspi(
                ControlSelector(
                    CONTROLLED_APPLICATION_NAME,
                    "push_button",
                    require_focused=True,
                )
            )
            self._xdotool("key", "--clearmodifiers", "Tab")
            self.observe_atspi(
                ControlSelector(
                    "Open notification center",
                    "push_button",
                    require_focused=True,
                )
            )
            self._xdotool("key", "--clearmodifiers", "Return")
            self.observe_atspi(ControlSelector("Notification center", "pane"))
            self.observe_atspi(
                ControlSelector("Do not disturb", "check_box", require_focused=True)
            )
            center = self._tier_c_named_surface(NOTIFICATION_CENTER_WINDOW_TITLE)
            self._wait_x11_focus(
                center[0], "tier_c_notification_center_x11_focus_timeout"
            )
            self._xdotool("key", "--clearmodifiers", "shift+Tab")
            self._wait_accessible_absent(
                ControlSelector("Notification center", "pane"),
                "tier_c_notification_center_dismissal_timeout",
            )
            self.observe_atspi(
                ControlSelector(
                    "Open notification center",
                    "push_button",
                    require_focused=True,
                )
            )
            self._wait_primary_panel_x11_focus(
                "tier_c_notification_center_focus_restore_timeout"
            )
            return TierCKeyboardObservation("tier_c_keyboard_complete", len(keys))
        for key in keys:
            mapped = TIER_C_KEY_ARGUMENTS.get(key)
            if mapped is None:
                raise HarnessError("tier_c_keyboard_mapping_invalid")
            self._xdotool("key", "--clearmodifiers", mapped)
        return TierCKeyboardObservation("tier_c_keyboard_complete", len(keys))

    def observe_atspi(self, selector: ControlSelector) -> AtspiCategoricalResult:
        shell = self._shell_identity()
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            if self._accessibility.observe(
                shell.pid, selector, self.probe_environment()
            ):
                return AtspiCategoricalResult("atspi_control_ready")
            self._operations.pause(0.05)
        raise HarnessError("tier_c_atspi_timeout")

    def _wait_accessible_state(
        self,
        selector: ControlSelector,
        state_name: str,
        expected: bool,
        timeout_error: str,
    ) -> None:
        shell = self._shell_identity()
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            if self._accessibility.observe_state(
                shell.pid,
                selector,
                state_name,
                expected,
                self.probe_environment(),
            ):
                return
            self._operations.pause(0.05)
        raise HarnessError(timeout_error)

    def capture_reviewed_material(
        self, identifier: str, sample_points: tuple[tuple[int, int], ...]
    ) -> TierCMaterialObservation:
        if (
            identifier != self._tier_c_identifier
            or self._tier_c_surface is None
            or self._config.xwd is None
            or self._runtime is None
            or self._display is None
        ):
            raise HarnessError("tier_c_material_state_invalid")
        window, expected_geometry = self._tier_c_surface
        geometry = self._tier_c_window_geometry(window)
        if geometry != expected_geometry:
            raise HarnessError("tier_c_surface_geometry_changed")
        environment = {"DISPLAY": self._display}
        first = capture_window_fingerprint(
            self._config.xwd,
            window,
            self._runtime,
            environment,
            sample_points=sample_points,
            require_opaque_samples=True,
        )
        self._operations.pause(0.05)
        settled = capture_window_fingerprint(
            self._config.xwd,
            window,
            self._runtime,
            environment,
            sample_points=sample_points,
            require_opaque_samples=True,
        )
        transparent = transparent_reference_fingerprint(
            geometry.width, geometry.height, sample_points, depth=24
        )
        return TierCMaterialObservation(
            "tier_c_material_observed",
            len(sample_points),
            len(sample_points),
            "xwd_sha256_opaque_reviewed_samples_v2",
            first,
            settled,
            transparent,
        )

    def observe_desktop_exclusion(self) -> TierCPortResult:
        if self._tier_c_identifier != "desktop" or self._tier_c_surface is None:
            raise HarnessError("tier_c_desktop_accessibility_state_invalid")
        shell = self._shell_identity()
        environment = self.probe_environment()
        panel = ControlSelector(
            "Open notification center", "push_button", require_focused=True
        )
        wallpaper = ControlSelector("Desktop wallpaper", "panel")
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            if self._accessibility.observe(
                shell.pid, panel, environment
            ) and self._accessibility.observe_absent(
                shell.pid, wallpaper, environment
            ):
                return TierCPortResult("tier_c_desktop_accessibility_excluded")
            self._operations.pause(0.05)
        raise HarnessError("tier_c_desktop_accessibility_exclusion_timeout")

    def complete_step(self, identifier: str) -> TierCPortResult:
        if identifier != self._tier_c_identifier or self._tier_c_surface is None:
            raise HarnessError("tier_c_step_state_invalid")
        window, expected_geometry = self._tier_c_surface
        if identifier == "desktop":
            if self._tier_c_window_geometry(window) != expected_geometry:
                raise HarnessError("tier_c_surface_geometry_changed")
            self.observe_atspi(
                ControlSelector(
                    "Open notification center",
                    "push_button",
                    require_focused=True,
                )
            )
        elif identifier == "panel":
            if self._tier_c_window_geometry(window) != expected_geometry:
                raise HarnessError("tier_c_surface_geometry_changed")
            self.observe_atspi(
                ControlSelector(
                    "Open notification center", "push_button", require_focused=True
                )
            )
        elif identifier == "launcher":
            self._controlled_windows[CONTROLLED_WINDOW_ONE] = self._wait_controlled_window(
                CONTROLLED_WINDOW_ONE
            )
            self._record_controlled_process(CONTROLLED_WINDOW_ONE)
            deadline = self._operations.monotonic() + self._config.readiness_timeout
            while self._operations.monotonic() < deadline:
                if not self._visible_named_window_ids(LAUNCHER_WINDOW_TITLE):
                    break
                self._operations.pause(0.05)
            else:
                raise HarnessError("tier_c_launcher_dismissal_timeout")
            self._focus_controlled_panel_task("tier_c_task_publication_timeout")
        elif identifier == "task_actions":
            if self._tier_c_window_geometry(window) != expected_geometry:
                raise HarnessError("tier_c_surface_geometry_changed")
            self.observe_atspi(
                ControlSelector(
                    CONTROLLED_APPLICATION_NAME,
                    "push_button",
                    require_focused=True,
                )
            )
        elif identifier == "notification":
            self.observe_atspi(
                ControlSelector(
                    "Open notification center",
                    "push_button",
                    require_focused=True,
                )
            )
        elif identifier == "notification_center":
            self._wait_accessible_state(
                ControlSelector("Do not disturb", "check_box"),
                "CHECKED",
                True,
                "tier_c_notification_toggle_failed",
            )
            self.observe_atspi(
                ControlSelector(
                    "Clear notification history", "push_button", require_focused=True
                )
            )
            self._xdotool("key", "--clearmodifiers", "Escape")
        elif identifier == "quick_settings":
            forge = ControlSelector(
                "Use Prismdrake Forge", "radio_button", require_focused=True
            )
            self.observe_atspi(forge)
            self._wait_accessible_state(
                forge,
                "CHECKED",
                True,
                "tier_c_profile_toggle_failed",
            )
            self._xdotool("key", "--clearmodifiers", "Escape")
        elif identifier == "logout":
            components = self._tier_c_logout_components
            session = self._session
            if components is None or session is None:
                raise HarnessError("tier_c_logout_state_invalid")
            deadline = self._operations.monotonic() + self._config.readiness_timeout
            while self._operations.monotonic() < deadline:
                if session.poll() is not None:
                    break
                self._operations.pause(0.05)
            else:
                raise HarnessError("tier_c_logout_shutdown_timeout")
            if session.poll() != 0:
                raise HarnessError("tier_c_logout_shutdown_failed")
            for name, identity in components.items():
                executable = self._config.installed_executables.get(name)
                if (
                    executable is None
                    or self._operations.identify_pid(identity.pid, executable) is not None
                ):
                    raise HarnessError("tier_c_logout_component_alive")
            fixture = self._config.controlled_window_fixture
            if fixture is not None and not all(
                self._operations.identify_pid(identity.pid, fixture) == identity
                for identity in self._controlled_processes.values()
            ):
                raise HarnessError("tier_c_logout_killed_unrelated_client")
            self._session = None
            self._surface_shell_identity = None
            self._tier_c_logout_components = None
        else:
            raise HarnessError("tier_c_step_state_invalid")
        self._tier_c_identifier = None
        self._tier_c_surface = None
        return TierCPortResult("tier_c_step_complete")

    def cleanup_clients(self) -> TierCClientCleanupObservation:
        clean = True
        clean = self._accessibility.close() and clean
        if self._settings_epoch is not None:
            clean = self._settings_epoch.close() and clean
            self._settings_epoch = None
        if self._notification_epoch is not None:
            clean = self._notification_epoch.close() and clean
            self._notification_epoch = None
        fixture = self._config.controlled_window_fixture
        if fixture is not None:
            for identity in self._controlled_processes.values():
                clean = self._operations.stop_identity(
                    identity, fixture, self._config.stop_timeout
                ) and clean
        for attribute in ("_session", "_bus", "_openbox"):
            child = getattr(self, attribute)
            if child is not None:
                clean = self._operations.stop(child, self._config.stop_timeout) and clean
                setattr(self, attribute, None)
        self._bus_process_identity = None
        self._bus_socket_identity = None
        self._surface_shell_identity = None
        self._desktop_surfaces = None
        self._controlled_windows.clear()
        self._controlled_processes.clear()
        self._accessibility_references.clear()
        self._accessibility_reference_shells.clear()
        self._tier_c_identifier = None
        self._tier_c_surface = None
        self._tier_c_logout_components = None
        return TierCClientCleanupObservation("tier_c_clients_cleaned", clean)

    def capture_process_ownership(self) -> Mapping[str, object]:
        if (
            self._xorg is None
            or self._openbox is None
            or self._bus is None
            or self._session is None
        ):
            raise HarnessError("live_process_ownership_unavailable")
        session_executable = self._config.installed_executables.get(
            "prismdrake-session"
        )
        if session_executable is None:
            raise HarnessError("live_process_ownership_invalid")
        xorg = self._operations.identify(self._xorg, self._config.xorg)
        openbox = self._operations.identify(self._openbox, self._config.openbox)
        bus = self._attest_private_bus()
        session = self._operations.identify(self._session, session_executable)
        if xorg is None or openbox is None or session is None:
            raise HarnessError("live_process_ownership_invalid")
        identities = (xorg, openbox, bus, session)
        if len(set(identities)) != len(identities):
            raise HarnessError("live_process_ownership_invalid")
        return dict(
            zip(
                (
                    "xorg_dummy_server",
                    "openbox_window_manager",
                    "isolated_session_bus",
                    "prismdrake_session",
                ),
                identities,
                strict=True,
            )
        )

    def _installed_lifecycle(self) -> InstalledLifecycleWorkflow:
        if self._lifecycle_workflow is None:
            self._lifecycle_workflow = InstalledLifecycleWorkflow(
                self,
                self,
                self,
                self,
                LIFECYCLE_REVIEWED_SURFACES,
            )
        return self._lifecycle_workflow

    @staticmethod
    def _lifecycle_identity(role: str, identity: ProcessIdentity) -> LifecycleProcessIdentity:
        return LifecycleProcessIdentity(role, identity.pid, identity.start_time)

    def _private_probe(self) -> LiveDbusProbe:
        socket_identity = self._bus_socket_identity
        if socket_identity is None:
            raise HarnessError("private_session_bus_not_started")
        return LiveDbusProbe(
            dict(self.probe_environment()),
            endpoint=PrivateBusEndpoint(
                self._private_bus_address(), socket_identity[0], socket_identity[1]
            ),
        )

    def topology(self) -> LifecycleTopologyObservation:
        outputs = self._query_outputs()
        controlled: list[LifecycleOutputIdentity] = []
        for name in (PRIMARY_OUTPUT, SECONDARY_OUTPUT):
            output = outputs.get(name)
            if output is None or not output.connected:
                raise HarnessError("lifecycle_controlled_output_missing")
            controlled.append(
                LifecycleOutputIdentity(
                    name,
                    self._lifecycle_connection_epochs[name],
                    output.active,
                    output.primary,
                )
            )
        return LifecycleTopologyObservation(
            self._lifecycle_topology_generation, tuple(controlled)
        )

    def _output_for_geometry(
        self,
        geometry: WindowGeometry,
        topology: LifecycleTopologyObservation,
    ) -> LifecycleOutputIdentity:
        outputs = self._query_outputs()
        matches: list[LifecycleOutputIdentity] = []
        for identity in topology.outputs:
            output = outputs.get(identity.name)
            if (
                output is not None
                and identity.active
                and output.x is not None
                and output.y is not None
                and output.width is not None
                and output.height is not None
                and geometry.x >= output.x
                and geometry.y >= output.y
                and geometry.x + geometry.width <= output.x + output.width
                and geometry.y + geometry.height <= output.y + output.height
            ):
                matches.append(identity)
        if len(matches) != 1:
            raise HarnessError("lifecycle_surface_output_ambiguous")
        return matches[0]

    def surface_placements(self) -> LifecycleSurfacePlacementObservation:
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            try:
                return self._observe_lifecycle_surface_placements()
            except HarnessError:
                self._operations.pause(0.05)
        raise HarnessError("lifecycle_surface_placement_timeout")

    def _observe_lifecycle_surface_placements(
        self,
    ) -> LifecycleSurfacePlacementObservation:
        topology = self.topology()
        shell = self._shell_identity()
        panels = self._observe_surface_windows(b"_NET_WM_WINDOW_TYPE_DOCK", shell)
        placements: list[LifecycleSurfacePlacement] = []
        ordered_panels = sorted(panels.items(), key=lambda item: (item[1].x, item[0]))
        if not ordered_panels:
            raise HarnessError("lifecycle_panel_surface_missing")
        panel_windows = [ordered_panels[0], ordered_panels[-1]]
        for surface, (_window, geometry) in zip(
            ("panel_primary", "panel_secondary"), panel_windows, strict=True
        ):
            output = self._output_for_geometry(geometry, topology)
            placements.append(LifecycleSurfacePlacement(surface, output, True, True))
        quick_window, quick_geometry = self._lifecycle_quick_settings_window(shell)
        placements.append(
            LifecycleSurfacePlacement(
                "quick_settings",
                self._output_for_geometry(quick_geometry, topology),
                True,
                True,
            )
        )
        return LifecycleSurfacePlacementObservation(
            topology.generation, tuple(placements)
        )

    def _lifecycle_quick_settings_window(
        self, shell: ProcessIdentity
    ) -> tuple[int, WindowGeometry]:
        windows = self._visible_named_window_ids(QUICK_SETTINGS_WINDOW_TITLE)
        if len(windows) != 1:
            raise HarnessError("lifecycle_quick_settings_window_invalid")
        quick_window = windows[0]
        if self._window_cardinals(quick_window, b"_NET_WM_PID", 1) != (shell.pid,):
            raise HarnessError("lifecycle_quick_settings_owner_invalid")
        result = self._run_x11(
            (str(self._config.xwininfo), "-id", f"0x{quick_window:x}", "-stats")
        )
        if result.returncode != 0:
            raise HarnessError("lifecycle_quick_settings_geometry_invalid")
        quick_geometry = parse_window_geometry(result.stdout)
        if (
            quick_geometry.width != 380
            or not 100 <= quick_geometry.height <= OUTPUT_HEIGHT - PANEL_HEIGHT
        ):
            raise HarnessError("lifecycle_quick_settings_geometry_invalid")
        return quick_window, quick_geometry

    def remove_output(
        self, output: LifecycleOutputIdentity, topology_generation: int
    ) -> None:
        current = self.topology()
        if current.generation != topology_generation or current.primary != output:
            raise HarnessError("lifecycle_stale_topology_mutation")
        survivor = next(item for item in current.outputs if item.active and item != output)
        self._capture_output_migration_reference()
        self._xrandr("--output", survivor.name, "--primary")
        self._xrandr("--output", output.name, "--off")
        self._lifecycle_topology_generation += 1
        self._surface_shell_identity = None
        self._wait_lifecycle_output_state(output.name, active=False, primary=False)

    def readd_output(
        self, output: LifecycleOutputIdentity, topology_generation: int
    ) -> None:
        current = self.topology()
        if current.generation != topology_generation:
            raise HarnessError("lifecycle_stale_topology_mutation")
        if output.name != PRIMARY_OUTPUT:
            raise HarnessError("lifecycle_output_readd_not_reviewed")
        self._xrandr(
            "--output",
            PRIMARY_OUTPUT,
            "--mode",
            OUTPUT_MODE,
            "--pos",
            "0x0",
            "--primary",
            "--output",
            SECONDARY_OUTPUT,
            "--mode",
            OUTPUT_MODE,
            "--pos",
            f"{OUTPUT_WIDTH}x0",
        )
        self._lifecycle_connection_epochs[PRIMARY_OUTPUT] += 1
        self._lifecycle_topology_generation += 1
        self._surface_shell_identity = None
        self._wait_lifecycle_output_state(PRIMARY_OUTPUT, active=True, primary=True)

    def _wait_lifecycle_output_state(
        self, name: str, *, active: bool, primary: bool
    ) -> None:
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            output = self._query_outputs().get(name)
            if output is not None and output.active == active and output.primary == primary:
                return
            self._operations.pause(0.05)
        raise HarnessError("lifecycle_output_mutation_timeout")

    def components(self) -> LifecycleComponentObservation:
        identities = self._component_identities()
        return LifecycleComponentObservation(
            self._lifecycle_identity(
                "prismdrake-settingsd", identities["prismdrake-settingsd"]
            ),
            self._lifecycle_identity("prismdrake-notifyd", identities["prismdrake-notifyd"]),
            self._lifecycle_identity("prismdrake-shell", identities["prismdrake-shell"]),
        )

    def prepare_survival_fixture(self) -> LifecycleWindowAuthorityObservation:
        self._controlled_windows.clear()
        self._controlled_processes.clear()
        self._xdotool("key", "--clearmodifiers", "Escape")
        self._wait_accessible_absent(
            ControlSelector("Quick settings", "pane"),
            "lifecycle_quick_settings_dismissal_timeout",
        )
        self._invoke_shellctl("launcher", "toggle")
        self._focus_launcher_result("lifecycle_survival_launcher_timeout")
        self._invoke_accessible(
            ControlSelector(
                CONTROLLED_APPLICATION_NAME,
                "push_button",
                "Press",
                require_focused=True,
            )
        )
        self._controlled_windows[CONTROLLED_WINDOW_ONE] = self._wait_controlled_window(
            CONTROLLED_WINDOW_ONE
        )
        self._record_controlled_process(CONTROLLED_WINDOW_ONE)
        self._invoke_shellctl("launcher", "toggle")
        self._focus_launcher_result("lifecycle_survival_launcher_focus_timeout")
        self._xdotool("key", "--clearmodifiers", "Shift_L+F10")
        self._invoke_accessible(ControlSelector("Second window", "menu_item", "Press"))
        self._controlled_windows[CONTROLLED_WINDOW_TWO] = self._wait_controlled_window(
            CONTROLLED_WINDOW_TWO
        )
        self._record_controlled_process(CONTROLLED_WINDOW_TWO)
        if len(self._controlled_processes) != 2:
            raise HarnessError("lifecycle_survival_processes_missing")

        self._restore_lifecycle_quick_settings()

        if self.notification_epoch().durable_record_count == 0:
            raise HarnessError("lifecycle_notification_fixture_timeout")
        return self.window_authority()

    def _restore_lifecycle_quick_settings(self) -> None:
        self._invoke_shellctl("quick-settings", "open")
        self._wait_accessible(
            ControlSelector("Quick settings", "pane"),
            "lifecycle_quick_settings_restore_timeout",
        )
        self.surface_placements()

    def window_authority(self) -> LifecycleWindowAuthorityObservation:
        if self._openbox is None:
            raise HarnessError("lifecycle_window_manager_missing")
        manager = self._operations.identify(self._openbox, self._config.openbox)
        if manager is None or len(self._controlled_processes) != 2:
            raise HarnessError("lifecycle_window_authority_invalid")
        self._require_controlled_windows_unchanged(self._controlled_windows)
        applications: list[LifecycleProcessIdentity] = []
        fixture = self._config.controlled_window_fixture
        if fixture is None:
            raise HarnessError("lifecycle_controlled_fixture_unavailable")
        for title in (CONTROLLED_WINDOW_ONE, CONTROLLED_WINDOW_TWO):
            identity = self._controlled_processes.get(title)
            if (
                identity is None
                or self._operations.identify_pid(identity.pid, fixture) != identity
            ):
                raise HarnessError("lifecycle_application_identity_invalid")
            applications.append(self._lifecycle_identity("application", identity))
        return LifecycleWindowAuthorityObservation(
            self._lifecycle_identity("window-manager", manager),
            tuple(applications),
            tuple(self._controlled_windows[title] for title in (
                CONTROLLED_WINDOW_ONE,
                CONTROLLED_WINDOW_TWO,
            )),
        )

    def restart_exact(
        self, identity: LifecycleProcessIdentity
    ) -> LifecycleRestartObservation:
        role = identity.role
        before = self.components()
        if before.for_role(role) != identity:
            raise HarnessError("lifecycle_stale_restart_identity")
        executable = self._config.installed_executables.get(role)
        if executable is None:
            raise HarnessError("component_identity_unavailable")
        notification_dependency = ControlSelector(
            "Do not disturb", "check_box", require_description=True
        )
        if role == "prismdrake-notifyd":
            _window, geometry = self._lifecycle_quick_settings_window(
                self._shell_identity()
            )
            self._output_for_geometry(geometry, self.topology())
            self._wait_accessible_state(
                notification_dependency,
                "ENABLED",
                True,
                "lifecycle_notifyd_dependency_precondition_timeout",
            )
        local = ProcessIdentity(identity.pid, identity.start_time)
        if not self._operations.fail_identity(
            local, executable, self._config.stop_timeout
        ):
            raise HarnessError("component_restart_signal_failed")
        if role == "prismdrake-shell":
            self._surface_shell_identity = None
            self._lifecycle_shell_notifyd_available = False
            self._lifecycle_shell_notifyd_rebind_required = True
        # Exact old-authority death is the invalidation boundary. A supervised
        # replacement may reacquire D-Bus before a sampling probe can observe
        # the owner gap, but it cannot preserve the killed process identity.
        invalidated = role != "prismdrake-shell"
        if role == "prismdrake-notifyd":
            self._lifecycle_shell_notifyd_available = False
            self._wait_accessible_state(
                notification_dependency,
                "ENABLED",
                False,
                "lifecycle_notifyd_dependency_invalidation_timeout",
            )
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        replacement: ProcessIdentity | None = None
        while self._operations.monotonic() < deadline:
            try:
                candidate = self._component_identity(role)
            except HarnessError as error:
                if str(error) != "component_identity_invalid":
                    raise
                if role != "prismdrake-shell":
                    invalidated = True
            else:
                if candidate != local:
                    replacement = candidate
                    break
            self._operations.pause(0.05)
        if replacement is None:
            raise HarnessError("component_restart_timeout")
        if role == "prismdrake-notifyd":
            self._wait_accessible_state(
                notification_dependency,
                "ENABLED",
                True,
                "lifecycle_notifyd_dependency_reacquisition_timeout",
            )
            self._lifecycle_shell_notifyd_available = True
        after = self.components()
        self._require_controlled_windows_unchanged(self._controlled_windows)
        return LifecycleRestartObservation(
            role,
            identity,
            self._lifecycle_identity(role, replacement),
            after,
            invalidated,
        )

    def settings_epoch(self) -> LifecycleSettingsEpochObservation:
        components = self.components()
        probe = self._private_probe()
        try:
            snapshot = probe.call("settings_get_current_snapshot", 2)
            generation, document = snapshot.payload
            complete = isinstance(document, dict) and document.get("generation") == generation
        finally:
            probe.close()

        panel_selector = ControlSelector(
            "Open applications", "push_button", require_focused=True
        )
        self._invoke_shellctl("panel", "focus")
        self._wait_accessible(
            panel_selector, "lifecycle_settings_panel_accessibility_timeout"
        )
        panel_accessible = self._accessibility.observe(
            components.shell.pid, panel_selector, self.probe_environment()
        )

        # Quick Settings owns the active accessibility scope while open. Attest
        # the panels first, then restore and attest the reviewed active surface;
        # do not infer that background panels remain operable in that scope.
        self._restore_lifecycle_quick_settings()
        surfaces: list[LifecycleReviewedSurfaceObservation] = []
        adapter = LiveSettingsSurfaceAdapter(
            SETTINGS_WORKFLOW_CONFIG,
            self._settings_surface_window,
            self._capture_settings_fingerprint,
            self._operations.pause,
        )
        for surface in LIFECYCLE_REVIEWED_SURFACES:
            first, second = adapter.capture_settled_fingerprints(
                surface, SETTINGS_WORKFLOW_CONFIG.points_for(surface)
            )
            accessible = panel_accessible
            if surface == "quick_settings":
                accessible = self._accessibility.observe(
                    components.shell.pid,
                    ControlSelector("Quick settings", "pane"),
                    self.probe_environment(),
                )
            surfaces.append(
                LifecycleReviewedSurfaceObservation(surface, first, second, accessible)
            )
        return LifecycleSettingsEpochObservation(
            components.settingsd,
            components.shell,
            generation,
            complete,
            tuple(surfaces),
        )

    def notification_epoch(self) -> LifecycleNotificationEpochObservation:
        components = self.components()
        epoch = self._notification_workflow_epoch()
        port = LiveDbusNotificationPort(epoch.dbus_probe)
        state = port.current_state()
        if not self._lifecycle_survival_notification_prepared:
            sender = epoch.sender
            if not isinstance(sender, PersistentNotificationProbe):
                raise HarnessError("lifecycle_notification_sender_unavailable")
            deadline = self._operations.monotonic() + self._config.readiness_timeout
            while self._operations.monotonic() < deadline:
                if not state.records:
                    sender.create_critical()
                    state = port.current_state()
                    continue
                live = tuple(
                    record for record in state.records if record.notification_id is not None
                )
                if live:
                    for record in live:
                        port.dismiss(record.record_id, record.content_generation)
                    state = port.current_state()
                    continue
                if all(
                    not record.transient
                    and all(not enabled for _action_id, enabled in record.actions)
                    for record in state.records
                ):
                    break
                self._operations.pause(0.05)
                state = port.current_state()
            else:
                raise HarnessError("lifecycle_notification_fixture_timeout")
            self._lifecycle_survival_notification_prepared = True
        durable = tuple(
            (
                record.record_id,
                record.content_generation,
                record.urgency,
                tuple(action_id for action_id, _enabled in record.actions),
            )
            for record in state.records
        )
        fingerprint = hashlib.sha256(
            json.dumps(durable, separators=(",", ":")).encode("ascii")
        ).digest()
        safe_runtime_degradation = all(
            record.notification_id is None
            and not record.transient
            and all(not enabled for _action_id, enabled in record.actions)
            for record in state.records
        )
        if self._lifecycle_shell_notifyd_rebind_required:
            self._invoke_shellctl("notifications", "open")
            self._wait_accessible(
                ControlSelector("Notification center", "pane"),
                "lifecycle_shell_notifyd_rebind_timeout",
            )
            self._xdotool("key", "--clearmodifiers", "Escape")
            self._wait_accessible_absent(
                ControlSelector("Notification center", "pane"),
                "lifecycle_shell_notifyd_rebind_dismissal_timeout",
            )
            self._invoke_shellctl("panel", "focus")
            self._wait_accessible(
                ControlSelector(
                    "Open applications", "push_button", require_focused=True
                ),
                "lifecycle_shell_notifyd_rebind_focus_timeout",
            )
            self._lifecycle_shell_notifyd_rebind_required = False
            self._lifecycle_shell_notifyd_available = True
        if not self._lifecycle_shell_notifyd_available:
            raise HarnessError("lifecycle_shell_notifyd_binding_unavailable")
        return LifecycleNotificationEpochObservation(
            components.notifyd,
            components.shell,
            state.generation,
            fingerprint,
            len(state.records),
            state.history_mode == "persistent",
            safe_runtime_degradation,
            self._lifecycle_shell_notifyd_available,
        )

    def _wait_logout_state(
        self, expected: str, *, transaction: int | None = None
    ) -> tuple[int, str]:
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            probe: LiveDbusProbe | None = None
            try:
                probe = self._private_probe()
                observed_transaction, state = probe.call("session_get_state").payload
            except Exception:
                pass
            else:
                if state == expected and (
                    transaction is None or observed_transaction == transaction
                ):
                    return observed_transaction, state
            finally:
                if probe is not None:
                    probe.close()
            self._operations.pause(0.05)
        raise HarnessError("lifecycle_logout_state_timeout")

    def request_through_shellctl(self) -> LifecycleLogoutObservation:
        self._invoke_shellctl("session", "logout")
        transaction, _state = self._wait_logout_state("confirmation_requested")
        self._wait_accessible(
            ControlSelector("Log out of Prismdrake", "dialog"),
            "lifecycle_logout_confirmation_timeout",
        )
        return LifecycleLogoutObservation(transaction, "confirmation_pending")

    def cancel(self, transaction_id: int) -> LifecycleLogoutObservation:
        current, _state = self._wait_logout_state(
            "confirmation_requested", transaction=transaction_id
        )
        self._invoke_accessible(ControlSelector("Cancel logout", "push_button", "Press"))
        self._wait_logout_state("idle")
        return LifecycleLogoutObservation(current, "cancelled")

    def reject_stale_transaction(self, transaction_id: int) -> bool:
        probe = self._private_probe()
        rejected = False
        try:
            probe.call("session_cancel_logout", transaction_id)
        except Exception:
            rejected = True
        finally:
            probe.close()
        if not rejected:
            return False
        observed, state = self._wait_logout_state("idle")
        return observed == 0 and state == "idle"

    def confirm(self, transaction_id: int) -> LifecycleLogoutObservation:
        self._wait_logout_state("confirmation_requested", transaction=transaction_id)
        self._lifecycle_shutdown_components = self.components()
        self._lifecycle_shutdown_started = self._operations.monotonic()
        self._attest_private_bus()
        monitor = self._session_transition_monitor_factory(
            self._config.dbus_monitor,
            self._private_bus_address(),
            self._private_bus_environment(),
            self._config.readiness_timeout,
        )
        try:
            self._invoke_accessible(ControlSelector("Confirm logout", "push_button", "Press"))
            sequence = monitor.finish(transaction_id, self._config.readiness_timeout)
        finally:
            monitor.close()
        self._attest_private_bus()
        if sequence != SESSION_SHUTDOWN_SEQUENCE:
            raise HarnessError("lifecycle_logout_transition_invalid")
        return LifecycleLogoutObservation(transaction_id, "stopping_shell")

    def _identity_alive(self, identity: LifecycleProcessIdentity) -> bool:
        executable = {
            "prismdrake-settingsd": self._config.installed_executables.get(
                "prismdrake-settingsd"
            ),
            "prismdrake-notifyd": self._config.installed_executables.get(
                "prismdrake-notifyd"
            ),
            "prismdrake-shell": self._config.installed_executables.get(
                "prismdrake-shell"
            ),
            "window-manager": self._config.openbox,
            "application": self._config.controlled_window_fixture,
        }.get(identity.role)
        if executable is None:
            return False
        return self._operations.identify_pid(identity.pid, executable) == ProcessIdentity(
            identity.pid, identity.start_time
        )

    def shutdown_observation(self, transaction_id: int) -> LifecycleShutdownObservation:
        components = self._lifecycle_shutdown_components
        started = self._lifecycle_shutdown_started
        if components is None or started is None:
            raise HarnessError("lifecycle_shutdown_not_started")
        events: list[LifecycleShutdownEvent] = []
        order = ("prismdrake-shell", "prismdrake-notifyd", "prismdrake-settingsd")
        deadline = started + min(self._config.readiness_timeout, 10.0)
        for index, role in enumerate(order):
            identity = components.for_role(role)
            later = tuple(components.for_role(item) for item in order[index + 1 :])
            while self._operations.monotonic() < deadline:
                if not self._identity_alive(identity):
                    elapsed = max(
                        0, int((self._operations.monotonic() - started) * 1000)
                    )
                    events.append(LifecycleShutdownEvent(role, identity, elapsed))
                    break
                if any(not self._identity_alive(item) for item in later):
                    raise HarnessError("lifecycle_shutdown_order_invalid")
                self._operations.pause(0.01)
            else:
                raise HarnessError("lifecycle_shutdown_timeout")
        while self._operations.monotonic() < deadline:
            if self._session is not None and self._session.poll() is not None:
                total = max(0, int((self._operations.monotonic() - started) * 1000))
                return LifecycleShutdownObservation(
                    transaction_id, tuple(events), total, True
                )
            self._operations.pause(0.01)
        raise HarnessError("lifecycle_session_shutdown_timeout")

    def identities_alive(
        self, identities: tuple[LifecycleProcessIdentity, ...]
    ) -> bool:
        return bool(identities) and all(self._identity_alive(item) for item in identities)

    def _capture_cleanup_boundary(self) -> None:
        tracked: list[tuple[ProcessIdentity, Path]] = []
        children = (
            (self._xorg, self._config.xorg),
            (self._openbox, self._config.openbox),
            (self._bus, self._config.dbus_daemon),
            (
                self._session,
                self._config.installed_executables.get("prismdrake-session"),
            ),
        )
        for child, executable in children:
            if child is None or executable is None:
                continue
            identity = self._operations.identify(child, executable)
            if identity is not None:
                tracked.append((identity, executable))
        fixture = self._config.controlled_window_fixture
        if fixture is not None:
            tracked.extend(
                (identity, fixture) for identity in self._controlled_processes.values()
            )
        self._cleanup_processes = tuple(tracked)
        self._cleanup_runtime = self._runtime
        self._cleanup_bus_path = (
            None if self._runtime is None else self._runtime / "bus"
        )

    def observe_exact_environment_cleanup(self) -> LifecycleCleanupObservation:
        remaining = sum(
            self._operations.identify_pid(identity.pid, executable) == identity
            for identity, executable in self._cleanup_processes
        )
        runtime_present = (
            self._cleanup_runtime is not None and self._cleanup_runtime.exists()
        )
        bus_present = (
            self._cleanup_bus_path is not None
            and (
                self._operations.socket_identity(self._cleanup_bus_path) is not None
                or self._operations.socket_connectable(self._cleanup_bus_path)
            )
        )
        return LifecycleCleanupObservation(
            remaining, runtime_present, bus_present
        )

    def cleanup(self, phase: str) -> Observation:
        tier_c_error: Exception | None = None
        if phase == "tier_c_replay":
            try:
                if self._tier_c_workflow is not None:
                    result = self._tier_c_workflow.cleanup()
                    validate_tier_c_result(result)
                elif self._tier_c_trace is not None:
                    clients = self.cleanup_clients()
                    self._tier_c_trace.finish(clients_exited=clients.clients_exited)
            except Exception as error:
                tier_c_error = error
            self._tier_c_workflow = None
            self._tier_c_trace = None
        lifecycle_cleanup = phase == "installed_workflow" and self._lifecycle_cleanup_ready
        workflow = self._lifecycle_workflow if lifecycle_cleanup else None
        if lifecycle_cleanup:
            self._capture_cleanup_boundary()
        clean = True
        clean = self._accessibility.close() and clean
        if self._settings_epoch is not None:
            clean = self._settings_epoch.close() and clean
            self._settings_epoch = None
        if self._notification_epoch is not None:
            clean = self._notification_epoch.close() and clean
            self._notification_epoch = None
        fixture = self._config.controlled_window_fixture
        if fixture is not None:
            for identity in self._controlled_processes.values():
                clean = (
                    self._operations.stop_identity(
                        identity, fixture, self._config.stop_timeout
                    )
                    and clean
                )
        for attribute in ("_session", "_bus", "_openbox", "_xorg"):
            child = getattr(self, attribute)
            if child is not None:
                clean = self._operations.stop(child, self._config.stop_timeout) and clean
                setattr(self, attribute, None)
        self._display = None
        self._bus_process_identity = None
        self._bus_socket_identity = None
        self._accessibility_bus_address = None
        self._desktop_surfaces = None
        self._surface_shell_identity = None
        self._controlled_windows.clear()
        self._controlled_processes.clear()
        self._accessibility_references.clear()
        self._accessibility_reference_shells.clear()
        if self._runtime is not None:
            try:
                shutil.rmtree(self._runtime)
            except OSError:
                clean = False
            self._runtime = None
        if not clean:
            raise HarnessError("live_cleanup_failed")
        if workflow is not None:
            result = workflow.run_step("verify_clean_test_environment_cleanup")
            validate_lifecycle_workflow_result(result)
        self._lifecycle_workflow = None
        self._lifecycle_cleanup_ready = False
        self._lifecycle_shutdown_started = None
        self._lifecycle_shutdown_components = None
        self._lifecycle_shell_notifyd_available = False
        self._lifecycle_shell_notifyd_rebind_required = False
        self._lifecycle_survival_notification_prepared = False
        if tier_c_error is not None:
            raise tier_c_error
        return Observation(True, True)

    @staticmethod
    def _hash_installed_executable(path: Path, expected_name: str) -> str:
        try:
            metadata = path.lstat()
        except OSError as error:
            raise HarnessError("installed_executable_unavailable") from error
        if (
            not path.is_absolute()
            or path.name != expected_name
            or stat.S_ISLNK(metadata.st_mode)
            or not stat.S_ISREG(metadata.st_mode)
            or metadata.st_size <= 0
            or metadata.st_size > MAX_EXECUTABLE_BYTES
            or metadata.st_mode & 0o111 == 0
        ):
            raise HarnessError("installed_executable_invalid")
        digest = hashlib.sha256()
        try:
            with path.open("rb") as executable:
                while block := executable.read(1024 * 1024):
                    digest.update(block)
        except OSError as error:
            raise HarnessError("installed_executable_read_failed") from error
        return digest.hexdigest()

    def _start_environment(self, *, two_outputs: bool, tier_c: bool = False) -> None:
        if self._xorg is not None or self._openbox is not None:
            raise HarnessError("live_environment_already_started")
        self._runtime = Path(tempfile.mkdtemp(prefix="prismdrake-pd2-"))
        self._runtime.chmod(0o700)
        for directory in ("home", "config", "cache", "data", "state"):
            path = self._runtime / directory
            path.mkdir(mode=0o700)
        self._write_controlled_desktop_entry()
        config_path = self._runtime / "xorg.conf"
        config_path.write_text(_xorg_config(), encoding="ascii")
        config_path.chmod(0o600)

        for number in range(self._config.first_display, self._config.last_display + 1):
            fake_number = number + 1000
            if not self._operations.display_available(number) or (
                tier_c and not self._operations.display_available(fake_number)
            ):
                continue
            display = f":{number}"
            argv = (
                str(self._config.xorg),
                display,
                "-config",
                str(config_path),
                "-nolisten",
                "tcp",
                "-noreset",
                "-logfile",
                str(self._runtime / "Xorg.log"),
            )
            child = self._operations.spawn(argv, self._base_environment())
            self._display = display
            self._xorg = child
            if self._wait_for_xorg():
                # Do not accept a query raced through an unrelated server while
                # this exact Xorg child was still failing its own startup.
                self._operations.pause(0.05)
                if child.poll() is None:
                    break
            self._operations.stop(child, self._config.stop_timeout)
            self._xorg = None
            self._display = None
        if self._xorg is None:
            raise HarnessError("xorg_readiness_failed")
        self._configure_primary()
        if two_outputs:
            self._enable_secondary()
        else:
            self._xrandr("--output", SECONDARY_OUTPUT, "--off")
        if tier_c:
            if self._runtime is None or self._display is None:
                raise HarnessError("tier_c_environment_not_started")
            real_number = int(self._display[1:])
            fake_display = f":{real_number + 1000}"
            trace_config = X11TraceConfig(
                self._runtime,
                self._display,
                fake_display,
                maximum_capture_seconds=300.0,
                readiness_seconds=min(max(self._config.readiness_timeout, 0.05), 10.0),
                shutdown_seconds=min(max(self._config.stop_timeout, 0.05), 10.0),
            )
            try:
                trace = self._tier_c_trace_factory(
                    trace_config, self._base_environment()
                )
            except Exception:
                raise HarnessError("tier_c_trace_start_failed") from None
            if trace.proxy_display != fake_display or trace.finished:
                try:
                    trace.finish(clients_exited=False)
                except Exception:
                    pass
                raise HarnessError("tier_c_trace_start_failed")
            self._tier_c_trace = trace
        self._start_openbox()

    def _base_environment(self) -> dict[str, str]:
        env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LC_ALL": "C"}
        if self._runtime is not None:
            env.update(
                {
                    "HOME": str(self._runtime / "home"),
                    "XDG_CACHE_HOME": str(self._runtime / "cache"),
                    "XDG_CONFIG_HOME": str(self._runtime / "config"),
                    "XDG_DATA_DIRS": "/usr/local/share:/usr/share",
                    "XDG_DATA_HOME": str(self._runtime / "data"),
                    "XDG_RUNTIME_DIR": str(self._runtime),
                    "XDG_STATE_HOME": str(self._runtime / "state"),
                }
            )
        return env

    def _write_controlled_desktop_entry(self) -> None:
        fixture = self._config.controlled_window_fixture
        if fixture is None:
            return
        try:
            metadata = fixture.lstat()
        except OSError as error:
            raise HarnessError("controlled_fixture_unavailable") from error
        native = os.fspath(fixture)
        if (
            not fixture.is_absolute()
            or stat.S_ISLNK(metadata.st_mode)
            or not stat.S_ISREG(metadata.st_mode)
            or metadata.st_mode & 0o111 == 0
            or any(
                character in native
                for character in ('"', "'", "\\", "`", "$", "%", "\n", "\r")
            )
        ):
            raise HarnessError("controlled_fixture_invalid")
        desktop_exec = f'"{native}"' if any(character.isspace() for character in native) else native
        if self._runtime is None:
            raise HarnessError("live_environment_not_started")
        working_directory = self._runtime / "home"
        working_native = os.fspath(working_directory)
        try:
            working_metadata = working_directory.lstat()
            working_bytes = working_native.encode("ascii")
        except (OSError, UnicodeEncodeError) as error:
            raise HarnessError("controlled_working_directory_invalid") from error
        if (
            not working_directory.is_absolute()
            or stat.S_ISLNK(working_metadata.st_mode)
            or not stat.S_ISDIR(working_metadata.st_mode)
            or len(working_bytes) > MAX_DESKTOP_PATH_BYTES
            or any(byte < 0x20 or byte == 0x7F for byte in working_bytes)
            or "\\" in working_native
        ):
            raise HarnessError("controlled_working_directory_invalid")
        applications = self._runtime / "data" / "applications"
        try:
            applications.mkdir(mode=0o700)
            desktop = applications / CONTROLLED_DESKTOP_FILE
            desktop.write_text(
                "[Desktop Entry]\n"
                "Type=Application\n"
                f"Name={CONTROLLED_APPLICATION_NAME}\n"
                f"Exec={desktop_exec} --instance one\n"
                f"Path={working_native}\n"
                "StartupWMClass=PrismdrakeDemo\n"
                "Actions=Second;\n"
                "\n"
                "[Desktop Action Second]\n"
                "Name=Second window\n"
                f"Exec={desktop_exec} --instance two\n",
                encoding="ascii",
            )
            desktop.chmod(0o600)
        except OSError as error:
            raise HarnessError("controlled_desktop_entry_failed") from error

    def _shell_identity(self) -> ProcessIdentity:
        shell = self._require_surface_shell()
        if self._surface_shell_identity is not None and shell != self._surface_shell_identity:
            raise HarnessError("surface_shell_identity_changed")
        return shell

    def _component_identity(self, name: str) -> ProcessIdentity:
        if name not in {
            "prismdrake-settingsd",
            "prismdrake-notifyd",
            "prismdrake-shell",
        }:
            raise HarnessError("component_identity_role_invalid")
        if self._session is None or self._session.poll() is not None:
            raise HarnessError("prismdrake_session_not_started")
        executable = self._config.installed_executables.get(name)
        if executable is None:
            raise HarnessError("component_identity_unavailable")
        identities = self._operations.descendants(self._session, executable)
        if len(identities) != 1:
            raise HarnessError("component_identity_invalid")
        return identities[0]

    def _component_identities(self) -> dict[str, ProcessIdentity]:
        return {
            name: self._component_identity(name)
            for name in (
                "prismdrake-settingsd",
                "prismdrake-notifyd",
                "prismdrake-shell",
            )
        }

    def _configuration_path(self) -> Path:
        if self._runtime is None:
            raise HarnessError("live_environment_not_started")
        return self._runtime / "config" / "prismdrake" / "config.toml"

    @staticmethod
    def _read_private_regular(path: Path, maximum_bytes: int) -> bytes:
        if maximum_bytes <= 0:
            raise HarnessError("private_state_file_invalid")
        flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
        flags |= getattr(os, "O_NOFOLLOW", 0)
        try:
            descriptor = os.open(path, flags)
        except OSError as error:
            raise HarnessError("private_state_file_invalid") from error
        try:
            metadata = os.fstat(descriptor)
            if (
                not stat.S_ISREG(metadata.st_mode)
                or metadata.st_uid != os.geteuid()
                or stat.S_IMODE(metadata.st_mode) != 0o600
                or metadata.st_size < 0
                or metadata.st_size > maximum_bytes
            ):
                raise HarnessError("private_state_file_invalid")
            blocks: list[bytes] = []
            remaining = maximum_bytes + 1
            while remaining > 0:
                block = os.read(descriptor, remaining)
                if not block:
                    break
                blocks.append(block)
                remaining -= len(block)
            payload = b"".join(blocks)
            if len(payload) != metadata.st_size:
                raise HarnessError("private_state_file_invalid")
            return payload
        except OSError as error:
            raise HarnessError("private_state_file_invalid") from error
        finally:
            os.close(descriptor)

    def _require_configuration_value(self, value: bytes, error_name: str) -> None:
        payload = self._read_private_regular(self._configuration_path(), 64 * 1024)
        if payload.count(value) != 1:
            raise HarnessError(error_name)

    def _write_malformed_pin_state(self) -> Path:
        if self._runtime is None:
            raise HarnessError("live_environment_not_started")
        state_directory = self._runtime / "state" / "prismdrake"
        directory = state_directory / "shell"
        path = directory / "pinned-applications-v1.json"
        try:
            state_directory.mkdir(mode=0o700)
            directory.mkdir(mode=0o700)
            path.write_bytes(b"{ malformed\n")
            path.chmod(0o600)
        except OSError as error:
            raise HarnessError("malformed_pin_state_setup_failed") from error
        return path

    def _require_recovered_pin_state(self, path: Path) -> None:
        payload = self._read_private_regular(path, 64 * 1024)
        try:
            document = json.loads(payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise HarnessError("malformed_pin_state_recovery_invalid") from error
        if document != {
            "schema_version": 1,
            "pins": [CONTROLLED_DESKTOP_FILE],
        }:
            raise HarnessError("malformed_pin_state_recovery_invalid")

    def _require_fresh_display_fallback_surfaces(self) -> None:
        # This attests the harness's fresh-display process construction.  It
        # deliberately does not claim to observe the X compositing-manager
        # selection owner; that requires GetSelectionOwner, not a root property.
        ownership = self.capture_process_ownership()
        if set(ownership) != {
            "xorg_dummy_server",
            "openbox_window_manager",
            "isolated_session_bus",
            "prismdrake_session",
        }:
            raise HarnessError("fresh_display_process_ownership_invalid")
        self._component_identities()
        self._require_desktop_wallpaper_surfaces()
        self._require_panel_surfaces()
        self._require_controlled_windows_unchanged(self._controlled_windows)
        self._require_shell_primary_control()

    def _require_shell_primary_control(self) -> None:
        self._invoke_shellctl("panel", "focus")
        self._wait_accessible(
            ControlSelector("Open applications", "push_button", require_focused=True),
            "shell_primary_control_timeout",
        )

    def _exhaust_shell_restart_budget(self) -> None:
        executable = self._config.installed_executables.get("prismdrake-shell")
        if executable is None:
            raise HarnessError("component_identity_unavailable")
        original_siblings = self._component_identities()
        previous = original_siblings["prismdrake-shell"]
        for crash in range(4):
            if not self._operations.fail_identity(
                previous, executable, self._config.stop_timeout
            ):
                raise HarnessError("safe_mode_crash_signal_failed")
            deadline = self._operations.monotonic() + self._config.readiness_timeout
            replacement: ProcessIdentity | None = None
            while self._operations.monotonic() < deadline:
                try:
                    candidate = self._component_identity("prismdrake-shell")
                except HarnessError as error:
                    if str(error) != "component_identity_invalid":
                        raise
                else:
                    if candidate != previous:
                        replacement = candidate
                        break
                self._operations.pause(0.05)
            if replacement is None:
                raise HarnessError("safe_mode_shell_restart_timeout")
            previous = replacement
            if crash < 3:
                current = self._component_identities()
                for sibling in ("prismdrake-settingsd", "prismdrake-notifyd"):
                    if current[sibling] != original_siblings[sibling]:
                        raise HarnessError("safe_mode_sibling_replaced_early")
                if self._session_marker_state() != "ready":
                    raise HarnessError("safe_mode_entered_before_budget")
        safe_components = self._component_identities()
        for sibling in ("prismdrake-settingsd", "prismdrake-notifyd"):
            if safe_components[sibling] == original_siblings[sibling]:
                raise HarnessError("safe_mode_component_epoch_not_replaced")
        self._surface_shell_identity = None

    def _wait_for_safe_mode(self) -> None:
        if self._session is None:
            raise HarnessError("prismdrake_session_not_started")
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            self._attest_private_bus()
            if self._session.poll() is not None:
                raise HarnessError("prismdrake_session_exited_before_safe_mode")
            if self._session_marker_state() == "safe_mode":
                self._component_identities()
                return
            self._operations.pause(0.05)
        raise HarnessError("prismdrake_session_safe_mode_timeout")

    def _prepare_restart_state(self) -> None:
        for identifier in (
            "start_isolated_two_output_x11",
            "start_installed_prismdrake_session",
            "verify_component_readiness",
            "verify_desktop_wallpaper_both_outputs",
            "verify_panel_policy_both_outputs",
            "invoke_launcher_through_shellctl",
            "search_controlled_application",
            "pin_controlled_application",
            "launch_controlled_application",
        ):
            self.run_step("installed_workflow", identifier)
        self._component_identities()

    def _restart_component(self, name: str) -> None:
        executable = self._config.installed_executables.get(name)
        if executable is None:
            raise HarnessError("component_identity_unavailable")
        before = self._component_identities()
        windows = dict(self._controlled_windows)
        old = before[name]
        if not self._operations.fail_identity(
            old, executable, self._config.stop_timeout
        ):
            raise HarnessError("component_restart_signal_failed")
        if name == "prismdrake-shell":
            self._surface_shell_identity = None

        deadline = self._operations.monotonic() + self._config.readiness_timeout
        replacement: ProcessIdentity | None = None
        while self._operations.monotonic() < deadline:
            try:
                candidate = self._component_identity(name)
            except HarnessError as error:
                if str(error) != "component_identity_invalid":
                    raise
            else:
                if candidate != old:
                    replacement = candidate
                    break
            self._operations.pause(0.05)
        if replacement is None:
            raise HarnessError("component_restart_timeout")

        after = self._component_identities()
        for role, identity in before.items():
            if role != name and after[role] != identity:
                raise HarnessError("unrelated_component_replaced_during_restart")
        self._require_controlled_windows_unchanged(windows)
        if name == "prismdrake-shell":
            self._require_desktop_wallpaper_surfaces()
            self._require_panel_surfaces()
        else:
            if self._component_identity("prismdrake-shell") != before["prismdrake-shell"]:
                raise HarnessError("shell_replaced_during_service_restart")
            # The all-output policy intentionally exposes one copy of the
            # shared task model in each panel.  Focus the exact primary panel
            # before selecting its task so replicated accessible names remain
            # fail-closed instead of becoming an ambiguous global selector.
            self._focus_controlled_panel_task("shell_owner_reacquisition_timeout")
            self._require_panel_surfaces()

    def _require_controlled_windows_unchanged(
        self, expected: Mapping[str, int]
    ) -> None:
        if not expected:
            raise HarnessError("controlled_window_identity_missing")
        for title, identifier in expected.items():
            if self._controlled_window_ids(title) != (identifier,):
                raise HarnessError("controlled_window_changed_during_restart")

    def _wait_accessible(self, selector: ControlSelector, timeout_error: str) -> None:
        shell = self._shell_identity()
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            if self._accessibility.observe(
                shell.pid, selector, self.probe_environment()
            ):
                return
            self._operations.pause(0.05)
        raise HarnessError(timeout_error)

    def _wait_accessible_application(self) -> None:
        shell = self._shell_identity()
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            if self._accessibility.observe_application(
                shell.pid, self.probe_environment()
            ):
                return
            self._operations.pause(0.05)
        category = self._accessibility.application_error_category()
        if category in APPLICATION_ERROR_CATEGORIES:
            raise HarnessError(
                f"shell_accessibility_readiness_timeout_{category}"
            )
        raise HarnessError("shell_accessibility_readiness_timeout")

    def _wait_accessible_absent(
        self, selector: ControlSelector, timeout_error: str
    ) -> None:
        shell = self._shell_identity()
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            if self._accessibility.observe_absent(
                shell.pid, selector, self.probe_environment()
            ):
                return
            self._operations.pause(0.05)
        raise HarnessError(timeout_error)

    def _focus_and_capture_reference(self, key: str) -> None:
        self._invoke_shellctl("panel", "focus")
        selector = ControlSelector(
            "Open applications", "push_button", require_focused=True
        )
        self._wait_accessible(selector, "accessibility_focus_precondition_timeout")
        self._capture_accessibility_reference(key, selector)

    def _capture_output_migration_reference(self) -> None:
        selector = ControlSelector(
            "Use Prismdrake Lustre",
            "radio_button",
            require_focused=True,
            require_description=True,
        )
        self._wait_accessible(
            selector, "accessibility_output_migration_focus_precondition_timeout"
        )
        self._capture_accessibility_reference("output_migration", selector)

    def _capture_accessibility_reference(
        self, key: str, selector: ControlSelector
    ) -> None:
        if key in self._accessibility_references:
            raise HarnessError("accessibility_reference_reused")
        shell = self._shell_identity()
        reference = self._accessibility.capture_reference(
            shell.pid, selector, self.probe_environment()
        )
        if reference is None:
            raise HarnessError("accessibility_reference_capture_failed")
        self._accessibility_references[key] = reference
        self._accessibility_reference_shells[key] = shell

    def _verify_output_migration_accessibility(self) -> None:
        key = "output_migration"
        reference = self._accessibility_references.get(key)
        previous_shell = self._accessibility_reference_shells.get(key)
        if reference is None or previous_shell is None:
            raise HarnessError("accessibility_reference_missing")
        shell = self._shell_identity()
        if shell != previous_shell:
            raise HarnessError("accessibility_focus_owner_invalid")
        environment = self.probe_environment()
        if not self._accessibility.observe_reference(
            shell.pid, reference, environment
        ):
            raise HarnessError("accessibility_focus_not_retained")

        lustre = ControlSelector(
            "Use Prismdrake Lustre",
            "radio_button",
            require_focused=True,
            require_description=True,
        )
        forge = ControlSelector(
            "Use Prismdrake Forge",
            "radio_button",
            require_focused=True,
            require_description=True,
        )
        self._wait_accessible(lustre, "accessibility_focus_recovery_timeout")
        self._xdotool("key", "--clearmodifiers", "Tab")
        self._wait_accessible(forge, "accessibility_keyboard_continuation_timeout")
        self._xdotool("key", "--clearmodifiers", "shift+Tab")
        self._wait_accessible(lustre, "accessibility_backtab_recovery_timeout")
        del self._accessibility_references[key]
        del self._accessibility_reference_shells[key]

    def _verify_accessible_metadata_and_backtab(self) -> None:
        self._xdotool("key", "--clearmodifiers", "Escape")
        self._wait_accessible_absent(
            ControlSelector("Quick settings", "pane"),
            "accessibility_quick_settings_dismissal_timeout",
        )
        self._invoke_shellctl("panel", "focus")
        panel_trigger = ControlSelector(
            "Open quick settings",
            "push_button",
            "Press",
            require_focused=True,
            require_description=True,
        )
        shell = self._shell_identity()
        environment = self.probe_environment()
        for _index in range(8):
            if self._accessibility.observe(shell.pid, panel_trigger, environment):
                break
            self._xdotool("key", "--clearmodifiers", "Tab")
        else:
            raise HarnessError("accessibility_quick_settings_origin_timeout")
        self._invoke_accessible(panel_trigger)
        self._wait_accessible(
            ControlSelector("Quick settings", "pane", require_description=True),
            "accessibility_quick_settings_description_timeout",
        )
        lustre = ControlSelector(
            "Use Prismdrake Lustre",
            "radio_button",
            require_focused=True,
            require_description=True,
        )
        self._wait_accessible(lustre, "accessibility_profile_description_timeout")
        self._wait_accessible_state(
            lustre,
            "CHECKED",
            True,
            "accessibility_profile_state_timeout",
        )
        dnd = ControlSelector(
            "Do not disturb", "check_box", require_description=True
        )
        self._wait_accessible(dnd, "accessibility_dnd_description_timeout")
        self._wait_accessible_state(
            dnd,
            "CHECKED",
            False,
            "accessibility_dnd_state_timeout",
        )
        self._wait_accessible(
            ControlSelector(
                "Request logout", "push_button", require_description=True
            ),
            "accessibility_logout_description_timeout",
        )

        self._xdotool("key", "--clearmodifiers", "shift+Tab")
        focused_panel_trigger = ControlSelector(
            "Open quick settings",
            "push_button",
            require_focused=True,
            require_description=True,
        )
        self._wait_accessible(
            focused_panel_trigger, "accessibility_backtab_panel_focus_timeout"
        )
        self._xdotool("key", "--clearmodifiers", "space")
        self._wait_accessible(lustre, "accessibility_backtab_reentry_timeout")

    def _verify_accessibility_reference(
        self,
        key: str,
        *,
        stale: bool,
        replacement_shell: bool,
        reacquire: bool,
    ) -> None:
        reference = self._accessibility_references.get(key)
        previous_shell = self._accessibility_reference_shells.get(key)
        if reference is None or previous_shell is None:
            raise HarnessError("accessibility_reference_missing")
        shell = self._shell_identity()
        if (shell != previous_shell) is not replacement_shell:
            raise HarnessError("accessibility_focus_owner_invalid")
        environment = self.probe_environment()
        if stale:
            if not self._accessibility.reject_stale_reference(reference, environment):
                raise HarnessError("accessibility_stale_node_live")
        elif not self._accessibility.observe_reference(
            shell.pid, reference, environment
        ):
            raise HarnessError("accessibility_focus_not_retained")

        selector = ControlSelector(
            "Open applications", "push_button", require_focused=True
        )
        if reacquire:
            self._invoke_shellctl("panel", "focus")
        self._wait_accessible(selector, "accessibility_focus_recovery_timeout")
        self._xdotool("key", "--clearmodifiers", "Tab")
        self._wait_accessible(
            ControlSelector(
                CONTROLLED_APPLICATION_NAME, "push_button", require_focused=True
            ),
            "accessibility_keyboard_continuation_timeout",
        )
        self._xdotool("key", "--clearmodifiers", "shift+Tab")
        self._wait_accessible(selector, "accessibility_backtab_recovery_timeout")
        if key == "notifyd":
            self._invoke_shellctl("notifications", "open")
            self._wait_accessible(
                ControlSelector("Notification center", "pane"),
                "accessibility_notifyd_presentation_timeout",
            )
            self._xdotool("key", "--clearmodifiers", "Escape")
            self._wait_accessible_absent(
                ControlSelector("Notification center", "pane"),
                "accessibility_notifyd_presentation_dismissal_timeout",
            )
        del self._accessibility_references[key]
        del self._accessibility_reference_shells[key]

    def _invoke_accessible(self, selector: ControlSelector) -> None:
        self._wait_accessible(selector, "controlled_accessible_action_timeout")
        shell = self._shell_identity()
        if not self._accessibility.invoke(
            shell.pid, selector, self.probe_environment()
        ):
            raise HarnessError("controlled_accessible_action_rejected")

    def _invoke_common_action(
        self, identifier: str, selector: ControlSelector
    ) -> None:
        if (
            identifier in self._common_action_acknowledgements
            or selector.role != "menu_item"
            or selector.action != "Press"
        ):
            raise HarnessError("common_action_acknowledgement_state_invalid")
        self._wait_accessible(selector, "controlled_accessible_action_timeout")
        shell = self._shell_identity()
        environment = self.probe_environment()
        started_ns = self._operations.monotonic_ns()
        if not self._accessibility.invoke(shell.pid, selector, environment):
            raise HarnessError("controlled_accessible_action_rejected")
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            if self._accessibility.observe_absent(shell.pid, selector, environment):
                finished_ns = self._operations.monotonic_ns()
                latency_ns = finished_ns - started_ns
                if not 0 <= latency_ns <= 60_000_000_000:
                    raise HarnessError("common_action_acknowledgement_clock_invalid")
                self._common_action_acknowledgements[identifier] = latency_ns
                return
            self._operations.pause(0.005)
        raise HarnessError("common_action_acknowledgement_timeout")

    def _reject_stale_accessible(self, selector: ControlSelector) -> bool:
        shell = self._shell_identity()
        return self._accessibility.reject_stale_action(
            shell.pid, selector, self.probe_environment()
        )

    def _invoke_shellctl(self, group: str, action: str) -> None:
        executable = self._config.installed_executables.get("prismdrake-shellctl")
        if executable is None:
            raise HarnessError("shellctl_unavailable")
        result = self._operations.run(
            (str(executable), group, action), self._private_bus_environment(), 3.0
        )
        if result.returncode != 0:
            raise HarnessError("shellctl_request_failed")

    def _require_shellctl_unavailable(self, group: str, action: str) -> None:
        executable = self._config.installed_executables.get("prismdrake-shellctl")
        if executable is None:
            raise HarnessError("shellctl_unavailable")
        result = self._operations.run(
            (str(executable), group, action), self._private_bus_environment(), 3.0
        )
        if (
            result.returncode != 5
            or result.stdout
            or result.stderr
            != b"prismdrake-shellctl: The typed control request failed (status 5).\n"
        ):
            raise HarnessError("shellctl_unavailable_status_invalid")

    def _xdotool(self, *arguments: str) -> CommandResult:
        executable = self._config.xdotool
        if executable is None:
            raise HarnessError("xdotool_unavailable")
        result = self._operations.run(
            (str(executable), *arguments), self._private_bus_environment(), 3.0
        )
        if result.returncode != 0:
            raise HarnessError("xdotool_request_failed")
        return result

    def _controlled_window_ids(self, title: str) -> tuple[int, ...]:
        if title not in {CONTROLLED_WINDOW_ONE, CONTROLLED_WINDOW_TWO}:
            raise HarnessError("controlled_window_selector_invalid")
        executable = self._config.xdotool
        if executable is None:
            raise HarnessError("xdotool_unavailable")
        result = self._operations.run(
            (str(executable), "search", "--onlyvisible", "--name", f"^{title}$"),
            self._private_bus_environment(),
            3.0,
        )
        if result.returncode == 1 and not result.stdout and not result.stderr:
            return ()
        if result.returncode != 0 or result.stderr or len(result.stdout) > 1024:
            raise HarnessError("controlled_window_query_failed")
        identifiers: list[int] = []
        for line in result.stdout.splitlines():
            if re.fullmatch(rb"[1-9][0-9]{0,19}", line) is None:
                raise HarnessError("controlled_window_query_invalid")
            identifier = int(line)
            if identifier > 0xFFFFFFFF or identifier in identifiers:
                raise HarnessError("controlled_window_query_invalid")
            identifiers.append(identifier)
        if len(identifiers) > 2:
            raise HarnessError("controlled_window_query_invalid")
        return tuple(identifiers)

    def _wait_controlled_window(self, title: str) -> int:
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            identifiers = self._controlled_window_ids(title)
            if len(identifiers) == 1:
                return identifiers[0]
            if len(identifiers) > 1:
                break
            self._operations.pause(0.05)
        raise HarnessError("controlled_window_timeout")

    def _record_controlled_process(self, title: str) -> None:
        fixture = self._config.controlled_window_fixture
        window = self._controlled_windows.get(title)
        if fixture is None or window is None:
            return
        owner = self._window_cardinals(window, b"_NET_WM_PID", 1)[0]
        identity = self._operations.identify_pid(owner, fixture)
        if identity is None or identity in self._controlled_processes.values():
            raise HarnessError("controlled_process_identity_invalid")
        self._controlled_processes[title] = identity

    def _wait_window_absent(self, title: str) -> None:
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            if not self._controlled_window_ids(title):
                self._controlled_windows.pop(title, None)
                return
            self._operations.pause(0.05)
        raise HarnessError("controlled_window_close_timeout")

    def _window_state_atoms(self, window: int) -> frozenset[bytes]:
        payload = self._window_property(window, b"_NET_WM_STATE")
        if payload in {
            b"_NET_WM_STATE:  not found.\n",
            b"_NET_WM_STATE(ATOM) = \n",
        }:
            return frozenset()
        match = re.fullmatch(
            rb"_NET_WM_STATE\(ATOM\) = ([A-Z0-9_, ]{1,1024})\n", payload
        )
        if match is None:
            raise HarnessError("controlled_window_state_invalid")
        atoms = tuple(part.strip() for part in match.group(1).split(b","))
        if (
            not atoms
            or len(set(atoms)) != len(atoms)
            or any(atom not in KNOWN_NET_WM_STATES for atom in atoms)
        ):
            raise HarnessError("controlled_window_state_invalid")
        return frozenset(atoms)

    def _active_window(self) -> int | None:
        result = self._run_x11((str(self._config.xprop), "-root", "_NET_ACTIVE_WINDOW"))
        if result.returncode != 0:
            raise HarnessError("controlled_active_window_query_failed")
        if result.stdout == b"_NET_ACTIVE_WINDOW(WINDOW): window id # 0x0\n":
            return None
        identifiers = parse_window_ids(result.stdout, b"_NET_ACTIVE_WINDOW")
        if len(identifiers) != 1:
            raise HarnessError("controlled_active_window_query_failed")
        return identifiers[0]

    def _wait_window_active(self, title: str, *, minimized: bool) -> None:
        window = self._controlled_windows.get(title)
        if window is None:
            raise HarnessError("controlled_window_identity_missing")
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            hidden = b"_NET_WM_STATE_HIDDEN" in self._window_state_atoms(window)
            active = self._active_window() == window
            if hidden == minimized and (minimized or active):
                return
            self._operations.pause(0.05)
        raise HarnessError(
            "controlled_window_minimization_timeout"
            if minimized
            else "controlled_window_activation_timeout"
        )

    def _open_controlled_group(self) -> None:
        self._focus_controlled_panel_task("controlled_task_focus_timeout")
        self._invoke_accessible(
            ControlSelector(
                CONTROLLED_APPLICATION_NAME,
                "push_button",
                "Press",
                require_focused=True,
            )
        )
        self._wait_accessible(
            ControlSelector(f"Activate {CONTROLLED_WINDOW_ONE}", "menu_item"),
            "controlled_task_group_timeout",
        )

    def _focus_controlled_panel_task(self, timeout_error: str) -> None:
        self._invoke_shellctl("panel", "focus")
        self._wait_accessible(
            ControlSelector("Open applications", "push_button", require_focused=True),
            "panel_focus_timeout",
        )
        self._wait_primary_panel_x11_focus("panel_x11_focus_timeout")
        self._xdotool("key", "--clearmodifiers", "Tab")
        self._wait_accessible(
            ControlSelector(
                CONTROLLED_APPLICATION_NAME, "push_button", require_focused=True
            ),
            timeout_error,
        )

    def _wait_primary_panel_x11_focus(self, timeout_error: str) -> None:
        self._wait_x11_focus(self._primary_panel_window(), timeout_error)

    def _wait_x11_focus(self, window: int, timeout_error: str) -> None:
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            focused = self._xdotool("getwindowfocus")
            if (
                not focused.stderr
                and len(focused.stdout) <= 32
                and re.fullmatch(rb"[1-9][0-9]{0,19}\n?", focused.stdout)
                is not None
                and int(focused.stdout) == window
            ):
                break
            self._operations.pause(0.05)
        else:
            raise HarnessError(timeout_error)

    def _primary_panel_window(self) -> int:
        self._require_panel_surfaces()
        outputs = self._query_outputs()
        primary = [output for output in outputs.values() if output.active and output.primary]
        if len(primary) != 1:
            raise HarnessError("panel_x11_focus_target_invalid")
        output = primary[0]
        if (
            output.width is None
            or output.height is None
            or output.x is None
            or output.y is None
        ):
            raise HarnessError("panel_x11_focus_target_invalid")
        expected = WindowGeometry(
            output.x,
            output.y + output.height - PANEL_HEIGHT,
            output.width,
            PANEL_HEIGHT,
        )
        shell = self._shell_identity()
        panels = self._observe_surface_windows(b"_NET_WM_WINDOW_TYPE_DOCK", shell)
        matches = [window for window, geometry in panels.items() if geometry == expected]
        if len(matches) != 1:
            raise HarnessError("panel_x11_focus_target_invalid")
        return matches[0]

    def _x11_environment(self) -> dict[str, str]:
        if self._display is None:
            raise HarnessError("xorg_not_started")
        env = self._base_environment()
        env["DISPLAY"] = self._display
        return env

    def _traced_x11_environment(
        self, environment: Mapping[str, str] | None = None
    ) -> dict[str, str]:
        env = dict(environment) if environment is not None else self._x11_environment()
        if self._tier_c_trace is not None:
            try:
                return self._tier_c_trace.client_environment(env)
            except Exception:
                raise HarnessError("tier_c_client_environment_invalid") from None
        return env

    def probe_environment(self) -> Mapping[str, str]:
        """Return the private live epoch environment for bounded observers."""

        if self._session is None or self._session.poll() is not None:
            raise HarnessError("prismdrake_session_not_started")
        environment = self._private_bus_environment()
        if self._tier_c_trace is not None:
            return self._traced_x11_environment(environment)
        return environment

    def _run_x11(self, argv: Sequence[str]) -> CommandResult:
        return self._operations.run(argv, self._x11_environment(), 3.0)

    def _start_session(
        self,
        configuration_document: str | None = None,
        *,
        private_bus_started: bool = False,
    ) -> None:
        if self._runtime is None or self._xorg is None or self._openbox is None:
            raise HarnessError("live_environment_not_started")
        if self._session is not None or (self._bus is not None and not private_bus_started):
            raise HarnessError("live_session_already_started")
        if private_bus_started:
            self._attest_private_bus()
        if tuple(self._config.installed_executables.keys()) != EXECUTABLE_NAMES:
            raise HarnessError("installed_executable_set_invalid")
        executables = self._config.installed_executables
        configuration_directory = self._runtime / "config" / "prismdrake"
        try:
            configuration_directory.mkdir(mode=0o700)
            configuration = configuration_directory / "config.toml"
            configuration.write_text(
                configuration_document or _session_config(), encoding="ascii"
            )
            configuration.chmod(0o600)
        except OSError as error:
            raise HarnessError("private_session_configuration_failed") from error
        if not private_bus_started:
            self._start_private_bus()
        environment = self._private_bus_environment()
        if self._tier_c_trace is not None:
            environment = self._traced_x11_environment(environment)
        environment.update(
            {
                "QT_QUICK_BACKEND": "software",
                "QT_QPA_PLATFORM": "xcb",
                "QT_QUICK_CONTROLS_STYLE": "Basic",
                "QT_LINUX_ACCESSIBILITY_ALWAYS_ON": "1",
            }
        )
        self._accessibility_bus_address = self._activate_accessibility_bus(environment)
        environment["AT_SPI_BUS_ADDRESS"] = self._accessibility_bus_address
        if not self._accessibility.initialize(environment):
            raise HarnessError("live_atspi_initialization_failed")
        argv = (
            str(executables["prismdrake-session"]),
            "--settingsd",
            str(executables["prismdrake-settingsd"]),
            "--notifyd",
            str(executables["prismdrake-notifyd"]),
            "--shell",
            str(executables["prismdrake-shell"]),
        )
        self._session = self._operations.spawn(argv, environment)
        self._operations.pause(0.05)
        if self._session.poll() is not None:
            self._session = None
            raise HarnessError("prismdrake_session_start_failed")

    def _activate_accessibility_bus(self, environment: Mapping[str, str]) -> str:
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            if self._bus is None or self._bus.poll() is not None:
                raise HarnessError(
                    "private_session_bus_exited_before_accessibility_readiness"
                )
            timeout = min(
                1.0, max(deadline - self._operations.monotonic(), 0.01)
            )
            try:
                activation = self._operations.run(
                    (
                        str(self._config.gdbus),
                        "call",
                        "--address",
                        self._private_bus_address(),
                        "--dest",
                        "org.a11y.Bus",
                        "--object-path",
                        "/org/a11y/bus",
                        "--method",
                        "org.a11y.Bus.GetAddress",
                    ),
                    environment,
                    timeout,
                )
                if (
                    activation.returncode != 0
                    or not activation.stdout
                    or activation.stderr
                ):
                    raise HarnessError("private_accessibility_bus_not_activated")
                address = _parse_accessibility_bus_address(activation.stdout)
                timeout = min(
                    1.0, max(deadline - self._operations.monotonic(), 0.01)
                )
                registry = self._operations.run(
                    (
                        str(self._config.gdbus),
                        "call",
                        "--address",
                        address,
                        "--dest",
                        "org.a11y.atspi.Registry",
                        "--object-path",
                        "/org/a11y/atspi/registry",
                        "--method",
                        "org.freedesktop.DBus.Peer.Ping",
                    ),
                    environment,
                    timeout,
                )
                if (
                    registry.returncode == 0
                    and registry.stdout.strip() == b"()"
                    and not registry.stderr
                ):
                    return address
            except HarnessError:
                pass
            self._operations.pause(0.05)
        raise HarnessError("private_accessibility_bus_readiness_failed")

    def _start_private_bus(self) -> None:
        if self._runtime is None:
            raise HarnessError("live_environment_not_started")
        address = self._private_bus_address()
        argv = (
            str(self._config.dbus_daemon),
            "--session",
            "--nofork",
            "--nopidfile",
            f"--address={address}",
        )
        child = self._operations.spawn(argv, self._base_environment())
        self._bus = child
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            if child.poll() is not None:
                raise HarnessError("private_session_bus_exited_before_readiness")
            process_identity = self._operations.identify(
                child, self._config.dbus_daemon
            )
            socket_identity = self._inspect_private_bus_socket()
            if (
                process_identity is not None
                and socket_identity is not None
                and self._private_bus_accepts_connections()
            ):
                self._bus_process_identity = process_identity
                self._bus_socket_identity = socket_identity
                return
            self._operations.pause(0.05)
        raise HarnessError("private_session_bus_readiness_failed")

    def _private_bus_address(self) -> str:
        if self._runtime is None:
            raise HarnessError("private_session_bus_not_started")
        return f"unix:path={self._runtime / 'bus'}"

    def _private_bus_environment(self) -> dict[str, str]:
        self._attest_private_bus()
        environment = self._x11_environment()
        environment["DBUS_SESSION_BUS_ADDRESS"] = self._private_bus_address()
        if self._accessibility_bus_address is not None:
            environment["AT_SPI_BUS_ADDRESS"] = self._accessibility_bus_address
        return environment

    def _inspect_private_bus_socket(self) -> tuple[int, int] | None:
        if self._runtime is None:
            raise HarnessError("private_session_bus_not_started")
        try:
            runtime_metadata = self._runtime.lstat()
        except FileNotFoundError:
            return None
        except OSError as error:
            raise HarnessError("private_session_bus_socket_invalid") from error
        if (
            not stat.S_ISDIR(runtime_metadata.st_mode)
            or stat.S_ISLNK(runtime_metadata.st_mode)
            or runtime_metadata.st_uid != os.geteuid()
            or stat.S_IMODE(runtime_metadata.st_mode) != 0o700
        ):
            raise HarnessError("private_session_bus_socket_invalid")
        return self._operations.socket_identity(self._runtime / "bus")

    def _private_bus_accepts_connections(self) -> bool:
        if self._runtime is None:
            return False
        return self._operations.socket_connectable(self._runtime / "bus")

    def _attest_private_bus(self) -> ProcessIdentity:
        if (
            self._bus is None
            or self._bus_process_identity is None
            or self._bus_socket_identity is None
        ):
            raise HarnessError("private_session_bus_not_started")
        identity = self._operations.identify(self._bus, self._config.dbus_daemon)
        if identity != self._bus_process_identity:
            raise HarnessError("private_session_bus_owner_changed")
        socket_identity = self._inspect_private_bus_socket()
        if socket_identity != self._bus_socket_identity:
            raise HarnessError("private_session_bus_socket_replaced")
        return identity

    def _wait_for_session_readiness(self) -> None:
        if self._session is None or self._runtime is None:
            raise HarnessError("prismdrake_session_not_started")
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            self._attest_private_bus()
            if self._session.poll() is not None:
                raise HarnessError("prismdrake_session_exited_before_readiness")
            state = self._session_marker_state()
            if state == "ready":
                return
            if state == "safe_mode":
                raise HarnessError("prismdrake_session_safe_mode_unexpected")
            self._operations.pause(0.05)
        raise HarnessError("prismdrake_session_readiness_failed")

    def _session_marker_state(self) -> str:
        if self._runtime is None:
            raise HarnessError("prismdrake_session_not_started")
        root = self._runtime / "prismdrake"
        try:
            root_metadata = root.lstat()
            entries = tuple(root.iterdir())
        except FileNotFoundError:
            return "pending"
        except OSError as error:
            raise HarnessError("prismdrake_session_runtime_invalid") from error
        if (
            stat.S_ISLNK(root_metadata.st_mode)
            or not stat.S_ISDIR(root_metadata.st_mode)
            or root_metadata.st_uid != os.geteuid()
            or stat.S_IMODE(root_metadata.st_mode) != 0o700
            or len(entries) != 1
        ):
            raise HarnessError("prismdrake_session_runtime_invalid")
        instance = entries[0]
        if re.fullmatch(r"session-[1-9][0-9]{0,19}-[0-9]{1,3}", instance.name) is None:
            raise HarnessError("prismdrake_session_runtime_invalid")
        try:
            metadata = instance.lstat()
        except OSError as error:
            raise HarnessError("prismdrake_session_runtime_invalid") from error
        if (
            stat.S_ISLNK(metadata.st_mode)
            or not stat.S_ISDIR(metadata.st_mode)
            or metadata.st_uid != os.geteuid()
            or stat.S_IMODE(metadata.st_mode) != 0o700
        ):
            raise HarnessError("prismdrake_session_runtime_invalid")
        ready = self._valid_marker(instance / "ready")
        safe_mode = self._valid_marker(instance / "safe-mode")
        if safe_mode:
            return "safe_mode"
        return "ready" if ready else "pending"

    @staticmethod
    def _valid_marker(path: Path) -> bool:
        try:
            metadata = path.lstat()
        except FileNotFoundError:
            return False
        except OSError as error:
            raise HarnessError("prismdrake_session_runtime_invalid") from error
        if (
            stat.S_ISLNK(metadata.st_mode)
            or not stat.S_ISREG(metadata.st_mode)
            or metadata.st_uid != os.geteuid()
            or stat.S_IMODE(metadata.st_mode) != 0o600
            or metadata.st_size != 0
        ):
            raise HarnessError("prismdrake_session_runtime_invalid")
        return True

    def _require_desktop_wallpaper_surfaces(
        self, *, allow_safe_mode: bool = False
    ) -> None:
        self._require_topology(two_outputs=True)
        shell = self._wait_for_surface_shell(allow_safe_mode=allow_safe_mode)
        desktops = self._observe_surface_windows(b"_NET_WM_WINDOW_TYPE_DESKTOP", shell)
        expected = {
            WindowGeometry(0, 0, OUTPUT_WIDTH, OUTPUT_HEIGHT),
            WindowGeometry(OUTPUT_WIDTH, 0, OUTPUT_WIDTH, OUTPUT_HEIGHT),
        }
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while len(desktops) != 2 or set(desktops.values()) != expected:
            if self._operations.monotonic() >= deadline:
                raise HarnessError("desktop_wallpaper_surface_invalid")
            self._operations.pause(0.05)
            desktops = self._observe_surface_windows(
                b"_NET_WM_WINDOW_TYPE_DESKTOP", shell
            )
        if len(desktops) != 2 or set(desktops.values()) != expected:
            raise HarnessError("desktop_wallpaper_surface_invalid")
        # The production readiness byte is emitted only after wallpaper,
        # settings, output, and desktop generations align. Requiring that
        # marker together with two mapped shell-owned DesktopWallpaper views
        # observes the solid fallback on each output without capturing pixels.
        self._desktop_surfaces = desktops
        self._surface_shell_identity = shell

    def _wait_for_surface_shell(
        self, *, allow_safe_mode: bool = False
    ) -> ProcessIdentity:
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while True:
            try:
                return self._require_surface_shell(allow_safe_mode=allow_safe_mode)
            except HarnessError as error:
                if str(error) != "surface_shell_identity_invalid":
                    raise
                if self._operations.monotonic() >= deadline:
                    raise
            self._operations.pause(0.05)

    def _require_failure_replay_surfaces(
        self,
        expected_desktops: set[WindowGeometry],
        expected_panels: set[WindowGeometry],
        *,
        shell: ProcessIdentity | None = None,
    ) -> None:
        """Require the shell to republish the exact post-mutation output surfaces."""

        if (
            not expected_desktops
            or len(expected_desktops) != len(expected_panels)
            or len(expected_desktops) > 2
        ):
            raise HarnessError("failure_surface_contract_invalid")
        shell = shell or self._require_surface_shell()
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        desktops: dict[int, WindowGeometry] = {}
        panels: dict[int, WindowGeometry] = {}
        while self._operations.monotonic() < deadline:
            desktops = self._observe_surface_windows(
                b"_NET_WM_WINDOW_TYPE_DESKTOP", shell
            )
            panels = self._observe_surface_windows(b"_NET_WM_WINDOW_TYPE_DOCK", shell)
            if (
                len(desktops) == len(expected_desktops)
                and set(desktops.values()) == expected_desktops
                and len(panels) == len(expected_panels)
                and set(panels.values()) == expected_panels
            ):
                break
            self._operations.pause(0.05)
        else:
            raise HarnessError("failure_surface_republication_timeout")

        root_bottom = max(
            geometry.y + geometry.height for geometry in expected_desktops
        )
        for window, geometry in panels.items():
            strut = self._window_cardinals(window, b"_NET_WM_STRUT", 4)
            partial = self._window_cardinals(window, b"_NET_WM_STRUT_PARTIAL", 12)
            bottom = root_bottom - geometry.y
            if strut != (0, 0, 0, bottom) or partial != (
                0,
                0,
                0,
                bottom,
                0,
                0,
                0,
                0,
                0,
                0,
                geometry.x,
                geometry.x + geometry.width - 1,
            ):
                raise HarnessError("failure_panel_strut_invalid")
        self._desktop_surfaces = desktops
        self._surface_shell_identity = shell

    def _require_panel_surfaces(
        self, *, allow_safe_mode: bool = False, primary_only: bool = False
    ) -> None:
        if self._desktop_surfaces is None or self._surface_shell_identity is None:
            raise HarnessError("desktop_surface_observation_missing")
        self._require_topology(two_outputs=True)
        shell = self._require_surface_shell(allow_safe_mode=allow_safe_mode)
        if shell != self._surface_shell_identity:
            raise HarnessError("surface_shell_identity_changed")
        desktops = self._observe_surface_windows(b"_NET_WM_WINDOW_TYPE_DESKTOP", shell)
        if desktops != self._desktop_surfaces:
            raise HarnessError("desktop_surface_epoch_changed")
        panels = self._observe_surface_windows(b"_NET_WM_WINDOW_TYPE_DOCK", shell)
        expected_geometries = {
            WindowGeometry(0, OUTPUT_HEIGHT - PANEL_HEIGHT, OUTPUT_WIDTH, PANEL_HEIGHT)
        }
        if not primary_only:
            expected_geometries.add(
                WindowGeometry(
                    OUTPUT_WIDTH,
                    OUTPUT_HEIGHT - PANEL_HEIGHT,
                    OUTPUT_WIDTH,
                    PANEL_HEIGHT,
                )
            )
        if len(panels) != len(expected_geometries) or (
            set(panels.values()) != expected_geometries
        ):
            raise HarnessError("panel_surface_invalid")
        for window, geometry in panels.items():
            strut = self._window_cardinals(window, b"_NET_WM_STRUT", 4)
            partial = self._window_cardinals(window, b"_NET_WM_STRUT_PARTIAL", 12)
            if strut != (0, 0, 0, PANEL_HEIGHT) or partial != (
                0,
                0,
                0,
                PANEL_HEIGHT,
                0,
                0,
                0,
                0,
                0,
                0,
                geometry.x,
                geometry.x + geometry.width - 1,
            ):
                raise HarnessError("panel_strut_invalid")

    def _wait_panel_surfaces_unchanged(
        self,
        timeout_error: str,
        *,
        allow_safe_mode: bool = False,
        primary_only: bool = False,
    ) -> None:
        """Wait for the expected panel layout while transient surfaces republish."""

        transient_errors = {
            "desktop_surface_epoch_changed",
            "panel_surface_invalid",
        }
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        last_error: HarnessError | None = None
        while self._operations.monotonic() < deadline:
            try:
                self._require_panel_surfaces(
                    allow_safe_mode=allow_safe_mode, primary_only=primary_only
                )
                return
            except HarnessError as error:
                if str(error) not in transient_errors:
                    raise
                last_error = error
            self._operations.pause(0.05)
        raise HarnessError(timeout_error) from last_error

    def _require_surface_shell(
        self, *, allow_safe_mode: bool = False
    ) -> ProcessIdentity:
        if self._session is None or self._session.poll() is not None:
            raise HarnessError("prismdrake_session_not_started")
        expected_state = "safe_mode" if allow_safe_mode else "ready"
        if self._session_marker_state() != expected_state:
            raise HarnessError("prismdrake_session_readiness_lost")
        executable = self._config.installed_executables.get("prismdrake-shell")
        if executable is None:
            raise HarnessError("surface_shell_identity_invalid")
        identities = self._operations.descendants(self._session, executable)
        if len(identities) != 1:
            raise HarnessError("surface_shell_identity_invalid")
        return identities[0]

    def _observe_surface_windows(
        self, window_type: bytes, shell: ProcessIdentity
    ) -> dict[int, WindowGeometry]:
        result = self._run_x11(
            (str(self._config.xprop), "-root", "_NET_CLIENT_LIST_STACKING")
        )
        if result.returncode != 0:
            raise HarnessError("x11_window_list_query_failed")
        windows = parse_window_ids(result.stdout, b"_NET_CLIENT_LIST_STACKING")
        observed: dict[int, WindowGeometry] = {}
        for window in windows:
            try:
                payload = self._window_property(window, b"_NET_WM_WINDOW_TYPE")
                if payload == b"_NET_WM_WINDOW_TYPE:  not found.\n":
                    continue
                if parse_window_type_atoms(payload) != (window_type,):
                    continue
                owner = self._window_cardinals(window, b"_NET_WM_PID", 1)
                if owner != (shell.pid,):
                    raise HarnessError("surface_window_owner_invalid")
                hints = self._window_property(window, b"WM_HINTS")
                focus_lines = [
                    line
                    for line in hints.splitlines()
                    if re.fullmatch(
                        rb"[ \t]+Client accepts input or input focus: (?:True|False)",
                        line,
                    )
                ]
                if len(focus_lines) != 1 or (
                    window_type == b"_NET_WM_WINDOW_TYPE_DESKTOP"
                    and not focus_lines[0].endswith(b": False")
                ):
                    raise HarnessError("surface_window_focus_policy_invalid")
                geometry_result = self._run_x11(
                    (str(self._config.xwininfo), "-id", f"0x{window:x}", "-stats")
                )
                if geometry_result.returncode != 0:
                    raise HarnessError("x11_window_geometry_query_failed")
                try:
                    observed[window] = parse_window_geometry(geometry_result.stdout)
                except HarnessError as error:
                    if str(error) == "x11_window_geometry_invalid" and (
                        is_valid_unmapped_window_geometry(geometry_result.stdout)
                    ):
                        continue
                    raise
            except HarnessError as error:
                if str(error) not in {
                    "x11_window_property_query_failed",
                    "x11_window_geometry_query_failed",
                }:
                    raise
                if self._window_remains_listed(window):
                    raise
        return observed

    def _window_remains_listed(self, window: int) -> bool:
        """Recheck an XID before classifying a failed query as a stale-window race."""

        result = self._run_x11(
            (str(self._config.xprop), "-root", "_NET_CLIENT_LIST_STACKING")
        )
        if result.returncode != 0:
            raise HarnessError("x11_window_list_query_failed")
        return window in parse_window_ids(
            result.stdout, b"_NET_CLIENT_LIST_STACKING"
        )

    def _window_property(self, window: int, property_name: bytes) -> bytes:
        try:
            decoded = property_name.decode("ascii")
        except UnicodeDecodeError as error:
            raise HarnessError("x11_property_name_invalid") from error
        result = self._run_x11(
            (str(self._config.xprop), "-id", f"0x{window:x}", decoded)
        )
        if result.returncode != 0:
            raise HarnessError("x11_window_property_query_failed")
        return result.stdout

    def _window_cardinals(
        self, window: int, property_name: bytes, count: int
    ) -> tuple[int, ...]:
        return parse_cardinals(
            self._window_property(window, property_name), property_name, count
        )

    def _wait_for_xorg(self) -> bool:
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            if self._xorg is None or self._xorg.poll() is not None:
                return False
            result = self._run_x11((str(self._config.xrandr), "--query"))
            if result.returncode == 0:
                try:
                    parse_xrandr_outputs(result.stdout)
                    return True
                except HarnessError:
                    pass
            self._operations.pause(0.05)
        return False

    def _xrandr(self, *arguments: str) -> None:
        result = self._run_x11((str(self._config.xrandr), *arguments))
        if result.returncode != 0:
            raise HarnessError("xrandr_mutation_failed")

    def _configure_primary(self) -> None:
        self._xrandr(
            "--output", PRIMARY_OUTPUT, "--mode", OUTPUT_MODE,
            "--pos", "0x0", "--primary",
        )

    def _enable_secondary(self) -> None:
        self._xrandr(
            "--newmode", SECONDARY_OUTPUT_MODE, "118.25",
            "1600", "1696", "1856", "2112",
            "900", "903", "908", "934", "-hsync", "+vsync",
        )
        self._xrandr("--addmode", SECONDARY_OUTPUT, SECONDARY_OUTPUT_MODE)
        self._xrandr(
            "--output", SECONDARY_OUTPUT, "--mode", SECONDARY_OUTPUT_MODE,
            "--pos", "1600x0",
        )

    def _query_outputs(self) -> dict[str, XrandrOutput]:
        result = self._run_x11((str(self._config.xrandr), "--query"))
        if result.returncode != 0:
            raise HarnessError("xrandr_query_failed")
        return {output.name: output for output in parse_xrandr_outputs(result.stdout)}

    def _require_topology(
        self,
        *,
        two_outputs: bool,
        primary: str = PRIMARY_OUTPUT,
        secondary_position: tuple[int, int] = (1600, 0),
    ) -> None:
        outputs = self._query_outputs()
        first = outputs.get(PRIMARY_OUTPUT)
        second = outputs.get(SECONDARY_OUTPUT)
        if first is None or second is None or not first.active:
            raise HarnessError("xrandr_topology_invalid")
        if (first.width, first.height, first.x, first.y) != (
            OUTPUT_WIDTH, OUTPUT_HEIGHT, 0, 0
        ):
            raise HarnessError("xrandr_topology_invalid")
        if two_outputs:
            if not second.active or (second.width, second.height, second.x, second.y) != (
                OUTPUT_WIDTH, OUTPUT_HEIGHT, *secondary_position
            ):
                raise HarnessError("xrandr_topology_invalid")
        elif second.active:
            raise HarnessError("xrandr_topology_invalid")
        active_primary = [output.name for output in outputs.values() if output.primary]
        if active_primary != [primary]:
            raise HarnessError("xrandr_primary_invalid")

    def _require_only_secondary(self) -> None:
        outputs = self._query_outputs()
        first = outputs.get(PRIMARY_OUTPUT)
        second = outputs.get(SECONDARY_OUTPUT)
        if first is None or second is None or first.active or not second.active:
            raise HarnessError("xrandr_primary_removal_invalid")
        if (second.width, second.height, second.x, second.y) != (
            OUTPUT_WIDTH,
            OUTPUT_HEIGHT,
            0,
            0,
        ):
            raise HarnessError("xrandr_primary_removal_invalid")

    def _start_openbox(self) -> None:
        self._openbox = self._operations.spawn(
            (str(self._config.openbox), "--sm-disable"), self._x11_environment()
        )
        deadline = self._operations.monotonic() + self._config.readiness_timeout
        while self._operations.monotonic() < deadline:
            if self._openbox.poll() is not None:
                break
            result = self._run_x11(
                (str(self._config.xprop), "-root", "_NET_SUPPORTING_WM_CHECK")
            )
            if result.returncode == 0 and re.fullmatch(
                rb"_NET_SUPPORTING_WM_CHECK\(WINDOW\): window id # 0x[0-9a-f]+\n?",
                result.stdout,
            ):
                return
            self._operations.pause(0.05)
        raise HarnessError("window_manager_readiness_failed")
