#!/usr/bin/env python3
"""Strict state machine for PD2 installed notification workflow steps 17-24."""

from __future__ import annotations

from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from typing import Any, Protocol, TypeVar

from pd2_live_dbus import LiveDbusProbe, ProbeResult, validate_evidence
from pd2_live_notifications import (
    ACTION_KEY,
    CategoricalResult as SenderResult,
    NotificationToken,
    PersistentNotificationProbe,
    validate_categorical_result as validate_sender_result,
)


ACTION_TIMEOUT_SECONDS = 3.0
MAX_ACTIONS = 8
MAX_OUTPUT_NAME_BYTES = 255
MAX_RECORDS = 256
SURFACE_TIMEOUT_SECONDS = 3.0
UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1

RESULT_CATEGORIES = (
    "pd2_notification_normal_created",
    "pd2_notification_replaced",
    "pd2_notification_action_completed",
    "pd2_notification_center_opened",
    "pd2_notification_dnd_enabled",
    "pd2_notification_normal_suppressed",
    "pd2_notification_critical_bypassed",
    "pd2_notification_history_cleared",
)


class NotificationWorkflowError(RuntimeError):
    """A workflow failure represented by one fixed redacted category."""


@dataclass(frozen=True)
class WorkflowResult:
    """Categorical evidence containing no notification or runtime identity."""

    category: str
    status: str = "passed"

    def evidence(self) -> dict[str, str]:
        return {"category": self.category, "status": self.status}


@dataclass(frozen=True, repr=False)
class _Record:
    record_id: int
    content_generation: int
    notification_id: int | None
    urgency: str
    transient: bool
    actions: tuple[tuple[str, bool], ...]


@dataclass(frozen=True, repr=False)
class NotificationState:
    """Privacy-minimized projection of one validated notification snapshot."""

    generation: int
    history_mode: str
    effective_do_not_disturb: bool
    runtime_do_not_disturb_override: bool | None
    records: tuple[_Record, ...]


class NotificationDbusPort(Protocol):
    """Only the typed calls required by demonstration steps 17-24."""

    def current_state(self) -> NotificationState: ...

    def open_center(self, output_name: str) -> None: ...

    def set_do_not_disturb(self, enabled: bool) -> int: ...

    def clear_history(self) -> int: ...

    def dismiss(self, record_id: int, content_generation: int) -> int: ...


class NotificationSurfacePort(Protocol):
    """Reviewed live UI actions and observations for the notification flow."""

    def invoke_live_action(self, timeout_seconds: float) -> None: ...

    def observe_center(self, timeout_seconds: float) -> None: ...

    def observe_do_not_disturb(
        self, enabled: bool, timeout_seconds: float
    ) -> None: ...

    def dismiss_center(self, timeout_seconds: float) -> None: ...

    def observe_transient(self, present: bool, timeout_seconds: float) -> None: ...


class NotificationSenderPort(Protocol):
    """Persistent freedesktop sender operations used by the state machine."""

    def create_normal(self) -> tuple[NotificationToken, SenderResult]: ...

    def create_critical(self) -> tuple[NotificationToken, SenderResult]: ...

    def replace(
        self, token: NotificationToken
    ) -> tuple[NotificationToken, SenderResult]: ...

    def wait_for_action(
        self, token: NotificationToken, timeout_seconds: float
    ) -> SenderResult: ...

    def wait_for_close(
        self, token: NotificationToken, timeout_seconds: float
    ) -> SenderResult: ...


def _fail(category: str) -> None:
    raise NotificationWorkflowError(category) from None


def _unsigned(value: Any, maximum: int, category: str) -> int:
    if (
        isinstance(value, bool)
        or not isinstance(value, int)
        or not 1 <= value <= maximum
    ):
        _fail(category)
    return value


def _state_from_document(generation: Any, document: Any) -> NotificationState:
    """Project an already schema-validated payload without retaining content."""

    checked_generation = _unsigned(
        generation, UINT64_MAX, "notification_workflow_state_invalid"
    )
    if not isinstance(document, Mapping):
        _fail("notification_workflow_state_invalid")
    if document.get("generation") != checked_generation:
        _fail("notification_workflow_state_invalid")
    history_mode = document.get("history_mode")
    effective = document.get("effective_do_not_disturb")
    runtime_override = document.get("runtime_do_not_disturb_override")
    records = document.get("records")
    if (
        history_mode not in {"disabled", "session", "persistent"}
        or not isinstance(effective, bool)
        or (runtime_override is not None and not isinstance(runtime_override, bool))
        or not isinstance(records, Sequence)
        or isinstance(records, (str, bytes, bytearray))
        or len(records) > MAX_RECORDS
    ):
        _fail("notification_workflow_state_invalid")

    projected: list[_Record] = []
    record_ids: set[int] = set()
    notification_ids: set[int] = set()
    for item in records:
        if not isinstance(item, Mapping):
            _fail("notification_workflow_state_invalid")
        record_id = _unsigned(
            item.get("record_id"), UINT64_MAX, "notification_workflow_state_invalid"
        )
        content_generation = _unsigned(
            item.get("content_generation"),
            UINT64_MAX,
            "notification_workflow_state_invalid",
        )
        notification_id_value = item.get("notification_id")
        notification_id = (
            None
            if notification_id_value is None
            else _unsigned(
                notification_id_value,
                UINT32_MAX,
                "notification_workflow_state_invalid",
            )
        )
        urgency = item.get("urgency")
        transient = item.get("transient")
        actions = item.get("actions")
        if (
            record_id in record_ids
            or (notification_id is not None and notification_id in notification_ids)
            or urgency not in {"low", "normal", "critical"}
            or not isinstance(transient, bool)
            or not isinstance(actions, Sequence)
            or isinstance(actions, (str, bytes, bytearray))
            or len(actions) > MAX_ACTIONS
        ):
            _fail("notification_workflow_state_invalid")
        action_projection: list[tuple[str, bool]] = []
        action_ids: set[str] = set()
        for action in actions:
            if not isinstance(action, Mapping):
                _fail("notification_workflow_state_invalid")
            action_id = action.get("action_id")
            enabled = action.get("enabled")
            if (
                not isinstance(action_id, str)
                or not action_id
                or len(action_id) > 64
                or action_id in action_ids
                or not isinstance(enabled, bool)
            ):
                _fail("notification_workflow_state_invalid")
            action_ids.add(action_id)
            action_projection.append((action_id, enabled))
        record_ids.add(record_id)
        if notification_id is not None:
            notification_ids.add(notification_id)
        projected.append(
            _Record(
                record_id,
                content_generation,
                notification_id,
                urgency,
                transient,
                tuple(action_projection),
            )
        )
    return NotificationState(
        checked_generation,
        history_mode,
        effective,
        runtime_override,
        tuple(projected),
    )


class LiveDbusNotificationPort:
    """Narrow adapter over the strict typed D-Bus probe."""

    def __init__(self, probe: LiveDbusProbe) -> None:
        self._probe = probe

    @staticmethod
    def _validate_result(result: ProbeResult, operation: str) -> ProbeResult:
        try:
            validate_evidence(result)
        except Exception:
            _fail("notification_workflow_dbus_result_invalid")
        if not isinstance(result, ProbeResult) or result.operation != operation:
            _fail("notification_workflow_dbus_result_invalid")
        return result

    def current_state(self) -> NotificationState:
        result = self._validate_result(
            self._probe.call("notification_get_current_state", 1),
            "notification_get_current_state",
        )
        if len(result.payload) != 2:
            _fail("notification_workflow_dbus_result_invalid")
        return _state_from_document(result.payload[0], result.payload[1])

    def open_center(self, output_name: str) -> None:
        result = self._validate_result(
            self._probe.call("shell_open_notification_center", output_name),
            "shell_open_notification_center",
        )
        if result.payload:
            _fail("notification_workflow_dbus_result_invalid")

    def set_do_not_disturb(self, enabled: bool) -> int:
        result = self._validate_result(
            self._probe.call("notification_set_do_not_disturb", enabled),
            "notification_set_do_not_disturb",
        )
        if len(result.payload) != 1:
            _fail("notification_workflow_dbus_result_invalid")
        return _unsigned(
            result.payload[0],
            UINT64_MAX,
            "notification_workflow_dbus_result_invalid",
        )

    def clear_history(self) -> int:
        result = self._validate_result(
            self._probe.call("notification_clear_history"),
            "notification_clear_history",
        )
        if len(result.payload) != 1:
            _fail("notification_workflow_dbus_result_invalid")
        return _unsigned(
            result.payload[0],
            UINT64_MAX,
            "notification_workflow_dbus_result_invalid",
        )

    def dismiss(self, record_id: int, content_generation: int) -> int:
        result = self._validate_result(
            self._probe.call(
                "notification_dismiss", record_id, content_generation
            ),
            "notification_dismiss",
        )
        if len(result.payload) != 1:
            _fail("notification_workflow_dbus_result_invalid")
        return _unsigned(
            result.payload[0],
            UINT64_MAX,
            "notification_workflow_dbus_result_invalid",
        )


T = TypeVar("T")


class InstalledNotificationWorkflow:
    """Exact-order installed notification workflow for PD2 steps 17-24."""

    def __init__(
        self,
        sender: PersistentNotificationProbe,
        dbus: NotificationDbusPort,
        surface: NotificationSurfacePort,
        output_name: str,
    ) -> None:
        if (
            not isinstance(output_name, str)
            or not output_name
            or "\x00" in output_name
            or len(output_name.encode("utf-8")) > MAX_OUTPUT_NAME_BYTES
        ):
            _fail("notification_workflow_output_invalid")
        self._sender: NotificationSenderPort = sender
        self._dbus = dbus
        self._surface = surface
        self._output_name = output_name
        self._step = 0
        self._state: NotificationState | None = None
        self._primary_token: NotificationToken | None = None
        self._primary_record_id: int | None = None
        self._suppressed_token: NotificationToken | None = None
        self._critical_token: NotificationToken | None = None
        self._critical_record_id: int | None = None

    def _require_step(self, expected: int) -> None:
        if self._step != expected:
            _fail("notification_workflow_order_invalid")

    def _run(self, category: str, operation: Callable[[], T]) -> T:
        try:
            return operation()
        except NotificationWorkflowError:
            raise
        except Exception:
            _fail(category)

    def _result(self, step: int) -> WorkflowResult:
        self._step = step
        result = WorkflowResult(RESULT_CATEGORIES[step - 1])
        validate_workflow_result(result)
        return result

    def _current_state(self) -> NotificationState:
        state = self._run(
            "notification_workflow_state_unavailable", self._dbus.current_state
        )
        if not isinstance(state, NotificationState):
            _fail("notification_workflow_state_invalid")
        return state

    @staticmethod
    def _record_map(state: NotificationState) -> dict[int, _Record]:
        return {record.record_id: record for record in state.records}

    @classmethod
    def _new_record(
        cls, before: NotificationState, after: NotificationState
    ) -> _Record:
        if after.generation <= before.generation:
            _fail("notification_workflow_generation_stale")
        before_records = cls._record_map(before)
        after_records = cls._record_map(after)
        if not set(before_records).issubset(after_records):
            _fail("notification_workflow_record_identity_invalid")
        new_ids = set(after_records) - set(before_records)
        if len(new_ids) != 1:
            _fail("notification_workflow_record_identity_invalid")
        if any(after_records[key] != value for key, value in before_records.items()):
            _fail("notification_workflow_record_identity_invalid")
        return after_records[new_ids.pop()]

    @classmethod
    def _same_except_transient(
        cls, before: NotificationState, after: NotificationState
    ) -> bool:
        before_records = cls._record_map(before)
        after_records = cls._record_map(after)
        if set(before_records) != set(after_records):
            return False
        for record_id, prior in before_records.items():
            current = after_records[record_id]
            if (
                current.record_id != prior.record_id
                or current.content_generation != prior.content_generation
                or current.notification_id != prior.notification_id
                or current.urgency != prior.urgency
                or current.actions != prior.actions
            ):
                return False
        return True

    def create_normal(self) -> WorkflowResult:
        self._require_step(0)
        before = self._current_state()
        if (
            before.history_mode == "disabled"
            or before.effective_do_not_disturb
            or before.records
        ):
            _fail("notification_workflow_baseline_invalid")
        token, sender_result = self._run(
            "notification_workflow_sender_failed", self._sender.create_normal
        )
        self._validate_sender(sender_result, "notification_created")
        after = self._current_state()
        record = self._new_record(before, after)
        if (
            record.notification_id is None
            or record.urgency != "normal"
            or not record.transient
            or (ACTION_KEY, True) not in record.actions
        ):
            _fail("notification_workflow_normal_invalid")
        self._primary_token = token
        self._primary_record_id = record.record_id
        self._state = after
        return self._result(1)

    def replace_normal(self) -> WorkflowResult:
        self._require_step(1)
        before = self._required_state()
        prior = self._required_primary_record(before)
        token, sender_result = self._run(
            "notification_workflow_sender_failed",
            lambda: self._sender.replace(self._required_primary_token()),
        )
        self._validate_sender(sender_result, "notification_replaced")
        after = self._current_state()
        records = self._record_map(after)
        replaced = records.get(prior.record_id)
        if (
            after.generation <= before.generation
            or set(records) != set(self._record_map(before))
            or replaced is None
            or replaced.notification_id != prior.notification_id
            or replaced.content_generation != prior.content_generation + 1
            or replaced.urgency != "normal"
            or not replaced.transient
            or (ACTION_KEY, True) not in replaced.actions
            or any(
                records[key] != value
                for key, value in self._record_map(before).items()
                if key != prior.record_id
            )
        ):
            _fail("notification_workflow_replacement_invalid")
        self._primary_token = token
        self._state = after
        return self._result(2)

    def invoke_live_action(self) -> WorkflowResult:
        self._require_step(2)
        before = self._required_state()
        record = self._required_primary_record(before)
        if (ACTION_KEY, True) not in record.actions:
            _fail("notification_workflow_action_unavailable")
        self._run(
            "notification_workflow_surface_action_failed",
            lambda: self._surface.invoke_live_action(ACTION_TIMEOUT_SECONDS),
        )
        sender_result = self._run(
            "notification_workflow_action_callback_failed",
            lambda: self._sender.wait_for_action(
                self._required_primary_token(), ACTION_TIMEOUT_SECONDS
            ),
        )
        self._validate_sender(sender_result, "notification_action_observed")
        after = self._current_state()
        if after != before:
            _fail("notification_workflow_action_state_changed")
        return self._result(3)

    def open_center(self) -> WorkflowResult:
        self._require_step(3)
        before = self._required_state()
        self._run(
            "notification_workflow_center_open_failed",
            lambda: self._dbus.open_center(self._output_name),
        )
        self._run(
            "notification_workflow_center_observation_failed",
            lambda: self._surface.observe_center(SURFACE_TIMEOUT_SECONDS),
        )
        after = self._current_state()
        if after != before:
            _fail("notification_workflow_center_state_changed")
        return self._result(4)

    def enable_do_not_disturb(self) -> WorkflowResult:
        self._require_step(4)
        before = self._required_state()
        if before.effective_do_not_disturb:
            _fail("notification_workflow_dnd_state_invalid")
        generation = self._run(
            "notification_workflow_dnd_request_failed",
            lambda: self._dbus.set_do_not_disturb(True),
        )
        after = self._current_state()
        checked_generation = _unsigned(
            generation, UINT64_MAX, "notification_workflow_dnd_state_invalid"
        )
        if (
            checked_generation <= before.generation
            or after.generation != checked_generation
            or not after.effective_do_not_disturb
            or after.runtime_do_not_disturb_override is not True
            or not self._same_except_transient(before, after)
            or any(
                record.transient
                != (record.notification_id is not None and record.urgency == "critical")
                for record in after.records
            )
        ):
            _fail("notification_workflow_dnd_state_invalid")
        self._run(
            "notification_workflow_dnd_observation_failed",
            lambda: self._surface.observe_do_not_disturb(
                True, SURFACE_TIMEOUT_SECONDS
            ),
        )
        self._state = after
        return self._result(5)

    def create_suppressed_normal(self) -> WorkflowResult:
        self._require_step(5)
        before = self._required_state()
        self._run(
            "notification_workflow_center_dismissal_failed",
            lambda: self._surface.dismiss_center(SURFACE_TIMEOUT_SECONDS),
        )
        token, sender_result = self._run(
            "notification_workflow_sender_failed", self._sender.create_normal
        )
        self._validate_sender(sender_result, "notification_created")
        after = self._current_state()
        record = self._new_record(before, after)
        if (
            not after.effective_do_not_disturb
            or record.notification_id is None
            or record.urgency != "normal"
            or record.transient
        ):
            _fail("notification_workflow_suppression_invalid")
        self._run(
            "notification_workflow_transient_absence_failed",
            lambda: self._surface.observe_transient(
                False, SURFACE_TIMEOUT_SECONDS
            ),
        )
        self._suppressed_token = token
        self._state = after
        return self._result(6)

    def create_critical_bypass(self) -> WorkflowResult:
        self._require_step(6)
        before = self._required_state()
        token, sender_result = self._run(
            "notification_workflow_sender_failed", self._sender.create_critical
        )
        self._validate_sender(sender_result, "notification_critical_created")
        after = self._current_state()
        record = self._new_record(before, after)
        if (
            not after.effective_do_not_disturb
            or record.notification_id is None
            or record.urgency != "critical"
            or not record.transient
        ):
            _fail("notification_workflow_critical_bypass_invalid")
        self._run(
            "notification_workflow_critical_observation_failed",
            lambda: self._surface.observe_transient(True, SURFACE_TIMEOUT_SECONDS),
        )
        self._critical_token = token
        self._critical_record_id = record.record_id
        self._state = after
        return self._result(7)

    def clear_history(self) -> WorkflowResult:
        self._require_step(7)
        before = self._required_state()
        before_live = tuple(record for record in before.records if record.notification_id)
        if len(before_live) < 3:
            _fail("notification_workflow_live_records_missing")
        primary = self._required_primary_record(before)
        dismiss_generation = self._run(
            "notification_workflow_history_prepare_failed",
            lambda: self._dbus.dismiss(
                primary.record_id, primary.content_generation
            ),
        )
        checked_dismiss_generation = _unsigned(
            dismiss_generation,
            UINT64_MAX,
            "notification_workflow_history_prepare_invalid",
        )
        sender_result = self._run(
            "notification_workflow_close_callback_failed",
            lambda: self._sender.wait_for_close(
                self._required_primary_token(), ACTION_TIMEOUT_SECONDS
            ),
        )
        self._validate_sender(sender_result, "notification_closed_observed")
        history = self._current_state()
        history_records = self._record_map(history)
        closed = history_records.get(primary.record_id)
        other_live = tuple(
            record for record in before_live if record.record_id != primary.record_id
        )
        history_live = tuple(
            record for record in history.records if record.notification_id
        )
        if (
            checked_dismiss_generation <= before.generation
            or history.generation != checked_dismiss_generation
            or closed is None
            or closed.notification_id is not None
            or closed.content_generation != primary.content_generation
            or closed.transient
            or any(enabled for _action_id, enabled in closed.actions)
            or set(history_live) != set(other_live)
        ):
            _fail("notification_workflow_history_prepare_invalid")
        generation = self._run(
            "notification_workflow_history_clear_failed", self._dbus.clear_history
        )
        after = self._current_state()
        checked_generation = _unsigned(
            generation,
            UINT64_MAX,
            "notification_workflow_history_clear_invalid",
        )
        after_live = tuple(record for record in after.records if record.notification_id)
        if (
            checked_generation <= history.generation
            or after.generation != checked_generation
            or set(after_live) != set(other_live)
            or len(after.records) != len(after_live)
        ):
            _fail("notification_workflow_history_clear_invalid")
        self._state = after
        return self._result(8)

    def verify_notify_send(self, invoke: Callable[[], None]) -> None:
        """Prove one literal notify-send notification reaches the live surface.

        The controlled sender remains authoritative for replacement and action
        callback coverage.  This extra acceptance runs only after the closed
        eight-step workflow, removes the existing critical transient first,
        and restores the pre-probe live-record set before returning.
        """

        self._require_step(8)
        before = self._required_state()
        critical_record_id = self._critical_record_id
        if critical_record_id is None:
            _fail("notification_workflow_notify_send_baseline_invalid")
        critical = self._record_map(before).get(critical_record_id)
        if (
            critical is None
            or critical.notification_id is None
            or critical.urgency != "critical"
            or not critical.transient
            or self._critical_token is None
        ):
            _fail("notification_workflow_notify_send_baseline_invalid")

        dismiss_generation = self._run(
            "notification_workflow_notify_send_prepare_failed",
            lambda: self._dbus.dismiss(
                critical.record_id, critical.content_generation
            ),
        )
        sender_result = self._run(
            "notification_workflow_notify_send_prepare_failed",
            lambda: self._sender.wait_for_close(
                self._critical_token, ACTION_TIMEOUT_SECONDS
            ),
        )
        self._validate_sender(sender_result, "notification_closed_observed")
        prepared = self._current_state()
        closed = self._record_map(prepared).get(critical.record_id)
        checked_dismiss_generation = _unsigned(
            dismiss_generation,
            UINT64_MAX,
            "notification_workflow_notify_send_prepare_invalid",
        )
        if (
            checked_dismiss_generation <= before.generation
            or prepared.generation != checked_dismiss_generation
            or closed is None
            or closed.notification_id is not None
            or closed.transient
            or any(enabled for _action_id, enabled in closed.actions)
        ):
            _fail("notification_workflow_notify_send_prepare_invalid")

        clear_generation = self._run(
            "notification_workflow_notify_send_prepare_failed",
            self._dbus.clear_history,
        )
        baseline = self._current_state()
        checked_clear_generation = _unsigned(
            clear_generation,
            UINT64_MAX,
            "notification_workflow_notify_send_prepare_invalid",
        )
        baseline_records = self._record_map(baseline)
        if (
            checked_clear_generation <= prepared.generation
            or baseline.generation != checked_clear_generation
            or not baseline.effective_do_not_disturb
            or baseline.history_mode != prepared.history_mode
            or baseline.runtime_do_not_disturb_override
            != prepared.runtime_do_not_disturb_override
            or critical.record_id in baseline_records
            or set(baseline_records)
            != {
                record.record_id
                for record in prepared.records
                if record.notification_id is not None
            }
        ):
            _fail("notification_workflow_notify_send_prepare_invalid")
        self._run(
            "notification_workflow_notify_send_prepare_failed",
            lambda: self._surface.observe_transient(
                False, SURFACE_TIMEOUT_SECONDS
            ),
        )

        self._run("notification_workflow_notify_send_failed", invoke)
        observed = self._current_state()
        external = self._new_record(baseline, observed)
        if (
            external.notification_id is None
            or external.urgency != "critical"
            or not external.transient
            or external.actions
            or observed.history_mode != baseline.history_mode
            or observed.effective_do_not_disturb
            != baseline.effective_do_not_disturb
            or observed.runtime_do_not_disturb_override
            != baseline.runtime_do_not_disturb_override
        ):
            _fail("notification_workflow_notify_send_state_invalid")
        self._run(
            "notification_workflow_notify_send_observation_failed",
            lambda: self._surface.observe_transient(
                True, SURFACE_TIMEOUT_SECONDS
            ),
        )

        external_dismiss_generation = self._run(
            "notification_workflow_notify_send_cleanup_failed",
            lambda: self._dbus.dismiss(
                external.record_id, external.content_generation
            ),
        )
        dismissed = self._current_state()
        dismissed_external = self._record_map(dismissed).get(external.record_id)
        checked_external_dismiss_generation = _unsigned(
            external_dismiss_generation,
            UINT64_MAX,
            "notification_workflow_notify_send_cleanup_invalid",
        )
        if (
            checked_external_dismiss_generation <= observed.generation
            or dismissed.generation != checked_external_dismiss_generation
            or dismissed_external is None
            or dismissed_external.notification_id is not None
            or dismissed_external.transient
            or dismissed_external.actions
            or dismissed.history_mode != observed.history_mode
            or dismissed.effective_do_not_disturb
            != observed.effective_do_not_disturb
            or dismissed.runtime_do_not_disturb_override
            != observed.runtime_do_not_disturb_override
        ):
            _fail("notification_workflow_notify_send_cleanup_invalid")
        cleanup_generation = self._run(
            "notification_workflow_notify_send_cleanup_failed",
            self._dbus.clear_history,
        )
        final = self._current_state()
        checked_cleanup_generation = _unsigned(
            cleanup_generation,
            UINT64_MAX,
            "notification_workflow_notify_send_cleanup_invalid",
        )
        if (
            checked_cleanup_generation <= dismissed.generation
            or final.generation != checked_cleanup_generation
            or self._record_map(final) != baseline_records
            or final.history_mode != baseline.history_mode
            or final.effective_do_not_disturb
            != baseline.effective_do_not_disturb
            or final.runtime_do_not_disturb_override
            != baseline.runtime_do_not_disturb_override
        ):
            _fail("notification_workflow_notify_send_cleanup_invalid")
        self._run(
            "notification_workflow_notify_send_cleanup_failed",
            lambda: self._surface.observe_transient(
                False, SURFACE_TIMEOUT_SECONDS
            ),
        )
        self._state = final

    @staticmethod
    def _validate_sender(result: SenderResult, expected_category: str) -> None:
        try:
            validate_sender_result(result)
        except Exception:
            _fail("notification_workflow_sender_result_invalid")
        if not isinstance(result, SenderResult) or result.category != expected_category:
            _fail("notification_workflow_sender_result_invalid")

    def _required_state(self) -> NotificationState:
        if self._state is None:
            _fail("notification_workflow_order_invalid")
        return self._state

    def _required_primary_token(self) -> NotificationToken:
        if self._primary_token is None:
            _fail("notification_workflow_order_invalid")
        return self._primary_token

    def _required_primary_record(self, state: NotificationState) -> _Record:
        if self._primary_record_id is None:
            _fail("notification_workflow_order_invalid")
        record = self._record_map(state).get(self._primary_record_id)
        if record is None:
            _fail("notification_workflow_record_identity_invalid")
        return record


def validate_workflow_result(result: WorkflowResult) -> None:
    """Reject arbitrary or identity-bearing workflow evidence."""

    if (
        not isinstance(result, WorkflowResult)
        or result.category not in RESULT_CATEGORIES
        or result.status != "passed"
        or set(result.evidence()) != {"category", "status"}
    ):
        _fail("notification_workflow_evidence_invalid")
