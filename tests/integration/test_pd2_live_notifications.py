#!/usr/bin/env python3
"""Hermetic tests for the persistent PD2 notification sender."""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path
from typing import Any


sys.path.insert(0, str(Path(__file__).resolve().parent))
from pd2_live_notifications import (  # noqa: E402
    ACTION_KEY,
    BODY,
    CRITICAL_SUMMARY,
    MAX_ACTIVE_NOTIFICATIONS,
    MAX_NOTIFICATION_OPERATIONS,
    MAX_PENDING_SIGNALS,
    NORMAL_SUMMARY,
    REPLACEMENT_SUMMARY,
    CategoricalResult,
    LiveNotificationError,
    NotificationSignal,
    PersistentNotificationProbe,
    _private_bus_address,
    validate_categorical_result,
)


class FakeTransport:
    def __init__(self) -> None:
        self.next_id = 1
        self.requests: list[tuple[int, str, int]] = []
        self.signals: list[NotificationSignal] = []
        self.dispatches = 0
        self.closed = False
        self.replacement_reply: int | None = None

    def notify(self, replaces_id: int, summary: str, urgency: int) -> int:
        self.requests.append((replaces_id, summary, urgency))
        if replaces_id:
            return self.replacement_reply or replaces_id
        result = self.next_id
        self.next_id += 1
        return result

    def take_signals(self) -> tuple[NotificationSignal, ...]:
        signals = tuple(self.signals)
        self.signals.clear()
        return signals

    def dispatch(self) -> None:
        self.dispatches += 1

    def close(self) -> None:
        self.closed = True


class PersistentNotificationProbeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.transport = FakeTransport()
        self.probe = PersistentNotificationProbe(self.transport)

    def tearDown(self) -> None:
        self.probe.close()

    def assert_category(self, category: str, operation: Any) -> None:
        with self.assertRaises(LiveNotificationError) as raised:
            operation()
        self.assertEqual(str(raised.exception), category)

    def test_creates_normal_and_critical_notifications_with_categorical_results(self) -> None:
        normal, normal_result = self.probe.create_normal()
        critical, critical_result = self.probe.create_critical()

        self.assertNotEqual(normal, critical)
        self.assertEqual(
            self.transport.requests,
            [(0, NORMAL_SUMMARY, 1), (0, CRITICAL_SUMMARY, 2)],
        )
        self.assertEqual(
            normal_result.as_dict(),
            {"category": "notification_created", "status": "passed"},
        )
        self.assertEqual(
            critical_result.as_dict(),
            {"category": "notification_critical_created", "status": "passed"},
        )

    def test_replaces_identity_and_rejects_the_stale_token(self) -> None:
        original, _result = self.probe.create_normal()
        replacement, result = self.probe.replace(original)

        self.assertEqual(
            self.transport.requests,
            [(0, NORMAL_SUMMARY, 1), (1, REPLACEMENT_SUMMARY, 1)],
        )
        self.assertEqual(result.category, "notification_replaced")
        self.assert_category(
            "notification_token_stale", lambda: self.probe.replace(original)
        )
        self.probe.replace(replacement)

    def test_rejects_a_replacement_that_changes_the_live_identity(self) -> None:
        token, _result = self.probe.create_normal()
        self.transport.replacement_reply = 44
        self.assert_category(
            "notification_replacement_identity", lambda: self.probe.replace(token)
        )

    def test_observes_action_and_close_without_exposing_signal_values(self) -> None:
        token, _result = self.probe.create_normal()
        self.transport.signals.extend(
            [
                NotificationSignal("action", 1, ACTION_KEY),
                NotificationSignal("closed", 1, 3),
            ]
        )

        action = self.probe.wait_for_action(token, 0.1)
        closed = self.probe.wait_for_close(token, 0.1)

        self.assertEqual(
            action.as_dict(),
            {"category": "notification_action_observed", "status": "passed"},
        )
        self.assertEqual(
            closed.as_dict(),
            {"category": "notification_closed_observed", "status": "passed"},
        )
        serialized = json.dumps([action.as_dict(), closed.as_dict()])
        for forbidden in (
            ACTION_KEY,
            BODY,
            NORMAL_SUMMARY,
            REPLACEMENT_SUMMARY,
            CRITICAL_SUMMARY,
            '"notification_id"',
        ):
            self.assertNotIn(forbidden, serialized)

    def test_retains_unrelated_signal_until_its_notification_is_observed(self) -> None:
        first, _result = self.probe.create_normal()
        second, _result = self.probe.create_critical()
        self.transport.signals.extend(
            [
                NotificationSignal("closed", 2, 3),
                NotificationSignal("action", 1, ACTION_KEY),
            ]
        )

        self.probe.wait_for_action(first, 0.1)
        self.probe.wait_for_close(second, 0.1)

    def test_rejects_foreign_stale_capacity_and_duplicate_identities(self) -> None:
        foreign_transport = FakeTransport()
        foreign = PersistentNotificationProbe(foreign_transport)
        foreign_token, _result = foreign.create_normal()
        self.assert_category(
            "notification_token_stale", lambda: self.probe.replace(foreign_token)
        )
        foreign.close()

        self.transport.next_id = 7
        self.probe.create_normal()
        self.transport.next_id = 7
        self.assert_category(
            "notification_live_id_duplicate", self.probe.create_normal
        )

        capacity_transport = FakeTransport()
        capacity_probe = PersistentNotificationProbe(capacity_transport)
        for _index in range(MAX_ACTIVE_NOTIFICATIONS):
            capacity_probe.create_normal()
        self.assert_category(
            "notification_fixture_capacity", capacity_probe.create_normal
        )
        capacity_probe.close()

    def test_rejects_malformed_or_excessive_signals_and_timeout_values(self) -> None:
        token, _result = self.probe.create_normal()
        self.transport.signals = [object()]  # type: ignore[list-item]
        self.assert_category(
            "notification_signal_invalid",
            lambda: self.probe.wait_for_action(token, 0.1),
        )

        bounded_transport = FakeTransport()
        bounded_probe = PersistentNotificationProbe(bounded_transport)
        bounded_token, _result = bounded_probe.create_normal()
        bounded_transport.signals = [
            NotificationSignal("closed", index + 2, 3)
            for index in range(MAX_PENDING_SIGNALS + 1)
        ]
        self.assert_category(
            "notification_signal_bound",
            lambda: bounded_probe.wait_for_action(bounded_token, 0.1),
        )
        bounded_probe.close()

        for value in (True, 0, 5.1, "1"):
            self.assert_category(
                "notification_wait_timeout_invalid",
                lambda value=value: self.probe.wait_for_action(token, value),
            )

        invalid_transport = FakeTransport()
        invalid_probe = PersistentNotificationProbe(invalid_transport)
        invalid_token, _result = invalid_probe.create_normal()
        for signal in (
            NotificationSignal("action", 1, "private-action"),
            NotificationSignal("closed", 1, 5),
            NotificationSignal("private-kind", 1, 1),
        ):
            invalid_transport.signals = [signal]
            self.assert_category(
                "notification_signal_invalid",
                lambda: invalid_probe.wait_for_action(invalid_token, 0.1),
            )
        invalid_probe.close()

    def test_bounds_all_notify_and_replacement_calls(self) -> None:
        token, _result = self.probe.create_normal()
        for _index in range(MAX_NOTIFICATION_OPERATIONS - 1):
            token, _result = self.probe.replace(token)
        self.assert_category(
            "notification_operation_bound", lambda: self.probe.replace(token)
        )

    def test_close_is_idempotent_and_all_future_operations_fail_closed(self) -> None:
        self.probe.close()
        self.probe.close()
        self.assertTrue(self.transport.closed)
        self.assert_category("notification_probe_closed", self.probe.create_normal)


class PrivateBusAddressTests(unittest.TestCase):
    def assert_category(self, category: str, operation: Any) -> None:
        with self.assertRaises(LiveNotificationError) as raised:
            operation()
        self.assertEqual(str(raised.exception), category)

    def test_reads_only_the_explicit_private_unix_path_address(self) -> None:
        environment = {
            "DBUS_SESSION_BUS_ADDRESS": "unix:path=/run/user/1000/prismdrake-test-bus",
            "UNRELATED_PRIVATE_VALUE": "must-not-be-read",
        }
        self.assertEqual(
            _private_bus_address(environment), environment["DBUS_SESSION_BUS_ADDRESS"]
        )

    def test_rejects_missing_ambient_abstract_multiple_and_malformed_addresses(self) -> None:
        for environment in (
            {},
            {"DBUS_SESSION_BUS_ADDRESS": "unix:abstract=/tmp/bus"},
            {"DBUS_SESSION_BUS_ADDRESS": "unix:path=/tmp/a;unix:path=/tmp/b"},
            {"DBUS_SESSION_BUS_ADDRESS": "unix:path=/tmp/a,nonce=x"},
            {"DBUS_SESSION_BUS_ADDRESS": "unix:path=relative"},
            {"DBUS_SESSION_BUS_ADDRESS": "unix:path=/tmp/bus\nprivate"},
        ):
            self.assert_category(
                "notification_private_bus_address_invalid",
                lambda environment=environment: _private_bus_address(environment),
            )

    def test_categorical_validation_rejects_arbitrary_evidence(self) -> None:
        validate_categorical_result(CategoricalResult("notification_created"))
        for result in (
            CategoricalResult("private-summary"),
            CategoricalResult("notification_created", "private-status"),
            object(),
        ):
            self.assert_category(
                "notification_evidence_invalid",
                lambda result=result: validate_categorical_result(result),
            )


if __name__ == "__main__":
    unittest.main()
