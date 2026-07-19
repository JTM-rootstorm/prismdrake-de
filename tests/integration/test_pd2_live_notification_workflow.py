#!/usr/bin/env python3
"""Hermetic tests for installed notification workflow steps 17-24."""

from __future__ import annotations

import json
import sys
import unittest
from collections.abc import Sequence
from pathlib import Path
from typing import Any


sys.path.insert(0, str(Path(__file__).resolve().parent))
from pd2_live_dbus import ProbeResult  # noqa: E402
from pd2_live_notification_workflow import (  # noqa: E402
    ACTION_TIMEOUT_SECONDS,
    SURFACE_TIMEOUT_SECONDS,
    InstalledNotificationWorkflow,
    LiveDbusNotificationPort,
    NotificationState,
    NotificationWorkflowError,
    WorkflowResult,
    _Record,
    _state_from_document,
    validate_workflow_result,
)
from pd2_live_notifications import (  # noqa: E402
    ACTION_KEY,
    CategoricalResult as SenderResult,
)


def record(
    record_id: int,
    *,
    content_generation: int = 1,
    notification_id: int | None = None,
    urgency: str = "normal",
    transient: bool = False,
    action: bool = True,
) -> _Record:
    return _Record(
        record_id,
        content_generation,
        notification_id,
        urgency,
        transient,
        ((ACTION_KEY, True),) if action else (),
    )


def state(
    generation: int,
    records: Sequence[_Record] = (),
    *,
    dnd: bool = False,
    override: bool | None = None,
    history: str = "session",
) -> NotificationState:
    return NotificationState(
        generation, history, dnd, override, tuple(records)
    )


def successful_states() -> list[NotificationState]:
    first = record(101, notification_id=11, transient=True)
    replaced = record(
        101, content_generation=2, notification_id=11, transient=True
    )
    dnd_first = record(
        101, content_generation=2, notification_id=11, transient=False
    )
    suppressed = record(102, notification_id=12, transient=False)
    critical = record(
        103, notification_id=13, urgency="critical", transient=True
    )
    closed_first = record(
        101,
        content_generation=2,
        notification_id=None,
        transient=False,
        action=False,
    )
    return [
        state(1),
        state(2, [first]),
        state(3, [replaced]),
        state(3, [replaced]),
        state(3, [replaced]),
        state(4, [dnd_first], dnd=True, override=True),
        state(5, [dnd_first, suppressed], dnd=True, override=True),
        state(6, [dnd_first, suppressed, critical], dnd=True, override=True),
        state(7, [closed_first, suppressed, critical], dnd=True, override=True),
        state(8, [suppressed, critical], dnd=True, override=True),
    ]


class FakeDbusPort:
    def __init__(self, states: Sequence[NotificationState]) -> None:
        self.states = list(states)
        self.calls: list[tuple[Any, ...]] = []
        self.dnd_generation = 4
        self.dismiss_generation = 7
        self.clear_generation = 8

    def current_state(self) -> NotificationState:
        self.calls.append(("state",))
        if not self.states:
            raise RuntimeError("private state timeout")
        return self.states.pop(0)

    def open_center(self, output_name: str) -> None:
        self.calls.append(("open", output_name))

    def set_do_not_disturb(self, enabled: bool) -> int:
        self.calls.append(("dnd", enabled))
        return self.dnd_generation

    def clear_history(self) -> int:
        self.calls.append(("clear",))
        return self.clear_generation

    def dismiss(self, record_id: int, content_generation: int) -> int:
        self.calls.append(("dismiss", record_id, content_generation))
        return self.dismiss_generation


class FakeSender:
    def __init__(self) -> None:
        self.calls: list[tuple[Any, ...]] = []
        self.tokens = [object(), object(), object(), object()]
        self.create_count = 0
        self.failure: str | None = None

    def create_normal(self) -> tuple[Any, SenderResult]:
        self.calls.append(("normal",))
        token = self.tokens[self.create_count]
        self.create_count += 1
        return token, SenderResult("notification_created")

    def create_critical(self) -> tuple[Any, SenderResult]:
        self.calls.append(("critical",))
        token = self.tokens[self.create_count]
        self.create_count += 1
        return token, SenderResult("notification_critical_created")

    def replace(self, token: Any) -> tuple[Any, SenderResult]:
        self.calls.append(("replace", token))
        replacement = self.tokens[self.create_count]
        self.create_count += 1
        return replacement, SenderResult("notification_replaced")

    def wait_for_action(self, token: Any, timeout_seconds: float) -> SenderResult:
        self.calls.append(("wait_action", token, timeout_seconds))
        if self.failure == "action":
            raise RuntimeError("private callback timeout")
        return SenderResult("notification_action_observed")

    def wait_for_close(self, token: Any, timeout_seconds: float) -> SenderResult:
        self.calls.append(("wait_close", token, timeout_seconds))
        if self.failure == "close":
            raise RuntimeError("private close timeout")
        return SenderResult("notification_closed_observed")


class FakeSurface:
    def __init__(self) -> None:
        self.calls: list[tuple[Any, ...]] = []
        self.failure: str | None = None

    def _call(self, *values: Any) -> None:
        self.calls.append(values)
        if self.failure == values[0]:
            raise RuntimeError("private surface identity")

    def invoke_live_action(self, timeout_seconds: float) -> None:
        self._call("action", timeout_seconds)

    def observe_center(self, timeout_seconds: float) -> None:
        self._call("center", timeout_seconds)

    def observe_do_not_disturb(
        self, enabled: bool, timeout_seconds: float
    ) -> None:
        self._call("dnd", enabled, timeout_seconds)

    def observe_transient(self, present: bool, timeout_seconds: float) -> None:
        self._call("transient", present, timeout_seconds)


def workflow(
    states: Sequence[NotificationState] | None = None,
) -> tuple[
    InstalledNotificationWorkflow, FakeSender, FakeDbusPort, FakeSurface
]:
    sender = FakeSender()
    dbus = FakeDbusPort(states or successful_states())
    surface = FakeSurface()
    return (
        InstalledNotificationWorkflow(sender, dbus, surface, "DUMMY0"),
        sender,
        dbus,
        surface,
    )


def run_through(
    flow: InstalledNotificationWorkflow, step: int
) -> list[WorkflowResult]:
    operations = (
        flow.create_normal,
        flow.replace_normal,
        flow.invoke_live_action,
        flow.open_center,
        flow.enable_do_not_disturb,
        flow.create_suppressed_normal,
        flow.create_critical_bypass,
        flow.clear_history,
    )
    return [operation() for operation in operations[:step]]


class InstalledNotificationWorkflowTests(unittest.TestCase):
    def assert_category(self, category: str, operation: Any) -> None:
        with self.assertRaises(NotificationWorkflowError) as raised:
            operation()
        self.assertEqual(str(raised.exception), category)

    def test_runs_exact_steps_with_only_categorical_evidence(self) -> None:
        flow, sender, dbus, surface = workflow()

        results = run_through(flow, 8)

        self.assertEqual(
            [result.category for result in results],
            [
                "pd2_notification_normal_created",
                "pd2_notification_replaced",
                "pd2_notification_action_completed",
                "pd2_notification_center_opened",
                "pd2_notification_dnd_enabled",
                "pd2_notification_normal_suppressed",
                "pd2_notification_critical_bypassed",
                "pd2_notification_history_cleared",
            ],
        )
        self.assertEqual(
            sender.calls,
            [
                ("normal",),
                ("replace", sender.tokens[0]),
                ("wait_action", sender.tokens[1], ACTION_TIMEOUT_SECONDS),
                ("normal",),
                ("critical",),
                ("wait_close", sender.tokens[1], ACTION_TIMEOUT_SECONDS),
            ],
        )
        self.assertIn(("open", "DUMMY0"), dbus.calls)
        self.assertIn(("dnd", True), dbus.calls)
        self.assertEqual(
            dbus.calls[-4:],
            [
                ("dismiss", 101, 2),
                ("state",),
                ("clear",),
                ("state",),
            ],
        )
        self.assertEqual(
            surface.calls,
            [
                ("action", ACTION_TIMEOUT_SECONDS),
                ("center", SURFACE_TIMEOUT_SECONDS),
                ("dnd", True, SURFACE_TIMEOUT_SECONDS),
                ("transient", False, SURFACE_TIMEOUT_SECONDS),
                ("transient", True, SURFACE_TIMEOUT_SECONDS),
            ],
        )
        serialized = json.dumps([result.evidence() for result in results])
        for forbidden in (
            "record_id",
            "notification_id",
            "content_generation",
            ACTION_KEY,
            "Fixture summary",
        ):
            self.assertNotIn(forbidden, serialized)

    def test_rejects_skipped_repeated_and_out_of_order_steps(self) -> None:
        flow, _sender, _dbus, _surface = workflow()
        self.assert_category(
            "notification_workflow_order_invalid", flow.replace_normal
        )
        flow.create_normal()
        self.assert_category(
            "notification_workflow_order_invalid", flow.create_normal
        )
        self.assert_category(
            "notification_workflow_order_invalid", flow.open_center
        )

    def test_rejects_stale_and_ambiguous_create_state(self) -> None:
        stale, *_rest = workflow([state(2), state(2, [record(1, notification_id=1)])])
        self.assert_category(
            "notification_workflow_generation_stale", stale.create_normal
        )

        ambiguous, *_rest = workflow(
            [
                state(1),
                state(
                    2,
                    [
                        record(1, notification_id=1, transient=True),
                        record(2, notification_id=2, transient=True),
                    ],
                ),
            ]
        )
        self.assert_category(
            "notification_workflow_record_identity_invalid",
            ambiguous.create_normal,
        )

    def test_rejects_replacement_identity_and_content_generation_changes(self) -> None:
        first = record(1, notification_id=1, transient=True)
        for replacement in (
            record(2, content_generation=2, notification_id=1, transient=True),
            record(1, content_generation=1, notification_id=1, transient=True),
            record(1, content_generation=2, notification_id=2, transient=True),
        ):
            flow, *_rest = workflow([state(1), state(2, [first]), state(3, [replacement])])
            flow.create_normal()
            self.assert_category(
                "notification_workflow_replacement_invalid",
                flow.replace_normal,
            )

    def test_rejects_missing_or_disabled_action_before_surface_invocation(self) -> None:
        for actions in ((), ((ACTION_KEY, False),)):
            flow, _sender, _dbus, surface = workflow()
            run_through(flow, 2)
            flow._state = state(  # pylint: disable=protected-access
                3, [_Record(101, 2, 11, "normal", True, actions)]
            )
            self.assert_category(
                "notification_workflow_action_unavailable",
                flow.invoke_live_action,
            )
            self.assertEqual(surface.calls, [])

    def test_redacts_surface_timeout_and_absence_observation_failures(self) -> None:
        flow, _sender, _dbus, surface = workflow()
        run_through(flow, 2)
        surface.failure = "action"
        self.assert_category(
            "notification_workflow_surface_action_failed",
            flow.invoke_live_action,
        )

        absent_flow, _sender, _dbus, absent_surface = workflow()
        run_through(absent_flow, 5)
        absent_surface.failure = "transient"
        self.assert_category(
            "notification_workflow_transient_absence_failed",
            absent_flow.create_suppressed_normal,
        )

        callback_flow, callback_sender, _dbus, _surface = workflow()
        run_through(callback_flow, 2)
        callback_sender.failure = "action"
        self.assert_category(
            "notification_workflow_action_callback_failed",
            callback_flow.invoke_live_action,
        )

        state_flow, _sender, _dbus, _surface = workflow([state(1)])
        self.assert_category(
            "notification_workflow_state_unavailable", state_flow.create_normal
        )

    def test_rejects_false_suppression_and_missing_critical_bypass(self) -> None:
        states = successful_states()
        states[6] = state(
            5,
            [
                states[5].records[0],
                record(102, notification_id=12, transient=True),
            ],
            dnd=True,
            override=True,
        )
        flow, *_rest = workflow(states)
        run_through(flow, 5)
        self.assert_category(
            "notification_workflow_suppression_invalid",
            flow.create_suppressed_normal,
        )

        states = successful_states()
        states[7] = state(
            6,
            [
                *states[6].records,
                record(
                    103,
                    notification_id=13,
                    urgency="critical",
                    transient=False,
                ),
            ],
            dnd=True,
            override=True,
        )
        flow, *_rest = workflow(states)
        run_through(flow, 6)
        self.assert_category(
            "notification_workflow_critical_bypass_invalid",
            flow.create_critical_bypass,
        )

    def test_clear_requires_all_live_records_and_removes_closed_history(self) -> None:
        states = successful_states()
        states[-1] = state(
            8,
            states[-1].records[:-1],
            dnd=True,
            override=True,
        )
        flow, *_rest = workflow(states)
        run_through(flow, 7)
        self.assert_category(
            "notification_workflow_history_clear_invalid", flow.clear_history
        )

        states = successful_states()
        states[-2] = state(
            7,
            states[-2].records[1:],
            dnd=True,
            override=True,
        )
        flow, *_rest = workflow(states)
        run_through(flow, 7)
        self.assert_category(
            "notification_workflow_history_prepare_invalid", flow.clear_history
        )

        states = successful_states()
        states[-1] = state(
            8,
            [*states[-1].records, record(200)],
            dnd=True,
            override=True,
        )
        flow, _sender, dbus, _surface = workflow(states)
        run_through(flow, 7)
        self.assert_category(
            "notification_workflow_history_clear_invalid", flow.clear_history
        )

    def test_rejects_invalid_state_projection_and_evidence(self) -> None:
        private_document = {
            "generation": 4,
            "history_mode": "session",
            "effective_do_not_disturb": False,
            "runtime_do_not_disturb_override": None,
            "records": [
                {
                    "record_id": 1,
                    "content_generation": 1,
                    "notification_id": 2,
                    "urgency": "normal",
                    "transient": True,
                    "summary": "private summary",
                    "body": "private body",
                    "actions": [{"action_id": ACTION_KEY, "enabled": True}],
                }
            ],
        }
        projected = _state_from_document(4, private_document)
        self.assertNotIn("private", repr(projected))

        duplicate = dict(private_document)
        duplicate["records"] = private_document["records"] * 2
        self.assert_category(
            "notification_workflow_state_invalid",
            lambda: _state_from_document(4, duplicate),
        )
        self.assert_category(
            "notification_workflow_evidence_invalid",
            lambda: validate_workflow_result(WorkflowResult("private-result")),
        )


class FakeTypedProbe:
    def __init__(self, replies: Sequence[ProbeResult]) -> None:
        self.replies = list(replies)
        self.calls: list[tuple[Any, ...]] = []

    def call(self, operation: str, *arguments: Any) -> ProbeResult:
        self.calls.append((operation, *arguments))
        return self.replies.pop(0)


class LiveDbusNotificationPortTests(unittest.TestCase):
    def test_exposes_only_reviewed_typed_notification_calls(self) -> None:
        document = {
            "generation": 8,
            "history_mode": "session",
            "effective_do_not_disturb": True,
            "runtime_do_not_disturb_override": True,
            "records": [],
        }
        probe = FakeTypedProbe(
            [
                ProbeResult(
                    "notification_get_current_state",
                    "notification_state_observed",
                    (8, document),
                ),
                ProbeResult(
                    "shell_open_notification_center",
                    "shell_notification_center_opened",
                    (),
                ),
                ProbeResult(
                    "notification_set_do_not_disturb",
                    "notification_dnd_requested",
                    (9,),
                ),
                ProbeResult(
                    "notification_dismiss",
                    "notification_dismiss_requested",
                    (10,),
                ),
                ProbeResult(
                    "notification_clear_history",
                    "notification_history_clear_requested",
                    (11,),
                ),
            ]
        )
        port = LiveDbusNotificationPort(probe)  # type: ignore[arg-type]

        self.assertEqual(port.current_state().generation, 8)
        port.open_center("DUMMY0")
        self.assertEqual(port.set_do_not_disturb(True), 9)
        self.assertEqual(port.dismiss(101, 2), 10)
        self.assertEqual(port.clear_history(), 11)
        self.assertEqual(
            probe.calls,
            [
                ("notification_get_current_state", 1),
                ("shell_open_notification_center", "DUMMY0"),
                ("notification_set_do_not_disturb", True),
                ("notification_dismiss", 101, 2),
                ("notification_clear_history",),
            ],
        )


if __name__ == "__main__":
    unittest.main()
