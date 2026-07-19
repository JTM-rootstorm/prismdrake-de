#!/usr/bin/env python3
"""Hermetic tests for the persistent PD2 notification sender."""

from __future__ import annotations

import json
import sys
import time
import unittest
from pathlib import Path
from typing import Any


sys.path.insert(0, str(Path(__file__).resolve().parent))
from pd2_live_notifications import (  # noqa: E402
    ACTION_KEY,
    BODY,
    CRITICAL_SUMMARY,
    MAX_ACTIVE_NOTIFICATIONS,
    MAX_DISPATCH_ITERATIONS,
    MAX_NOTIFICATION_OPERATIONS,
    MAX_PENDING_SIGNALS,
    NORMAL_SUMMARY,
    REPLACEMENT_SUMMARY,
    STANDARD_BUS_NAME,
    STANDARD_INTERFACE,
    STANDARD_PATH,
    CategoricalResult,
    GioNotificationTransport,
    LiveNotificationError,
    NotificationSignal,
    PersistentNotificationProbe,
    _private_bus_address,
    validate_categorical_result,
)


class FakeVariant:
    def __init__(self, signature: str, value: Any) -> None:
        self.signature = signature
        self.value = value

    def unpack(self) -> Any:
        return self.value


class FakeVariantType:
    @staticmethod
    def new(signature: str) -> str:
        return signature


class FakeMainContext:
    def __init__(self, *, always_pending: bool = False) -> None:
        self.always_pending = always_pending
        self.active = False
        self.pushes = 0
        self.pops = 0
        self.iterations = 0

    def push_thread_default(self) -> None:
        if self.active:
            raise RuntimeError("private nested context")
        self.active = True
        self.pushes += 1

    def pop_thread_default(self) -> None:
        if not self.active:
            raise RuntimeError("private inactive context")
        self.active = False
        self.pops += 1

    def pending(self) -> bool:
        return self.always_pending

    def iteration(self, may_block: bool) -> bool:
        if may_block:
            raise AssertionError("dispatch must not block")
        self.iterations += 1
        return self.always_pending


class FakeMainContextFactory:
    def __init__(self, context: FakeMainContext) -> None:
        self.context = context
        self.calls = 0

    def new(self) -> FakeMainContext:
        self.calls += 1
        return self.context


class FakeGLib:
    Variant = FakeVariant
    VariantType = FakeVariantType

    def __init__(self, context: FakeMainContext) -> None:
        self.MainContext = FakeMainContextFactory(context)


class FakeConnection:
    def __init__(self, context: FakeMainContext) -> None:
        self.context = context
        self.unique_name: Any = ":1.77"
        self.raise_unique_name = False
        self.fail_subscription_at: int | None = None
        self.subscriptions: list[tuple[Any, ...]] = []
        self.callbacks: dict[str, Any] = {}
        self.unsubscribed: list[int] = []
        self.close_calls = 0
        self.calls: list[tuple[Any, ...]] = []
        self.notify_reply: Any = (7,)

    def get_unique_name(self) -> Any:
        if self.raise_unique_name:
            raise RuntimeError("private peer failure")
        return self.unique_name

    def signal_subscribe(self, *arguments: Any) -> int:
        if not self.context.active:
            raise AssertionError("subscription escaped probe-owned context")
        call_number = len(self.subscriptions) + 1
        if self.fail_subscription_at == call_number:
            raise RuntimeError("private subscription failure")
        self.subscriptions.append(arguments)
        self.callbacks[arguments[2]] = arguments[6]
        return call_number

    def signal_unsubscribe(self, subscription: int) -> None:
        self.unsubscribed.append(subscription)

    def call_sync(self, *arguments: Any) -> FakeVariant:
        self.calls.append(arguments)
        return FakeVariant("(u)", self.notify_reply)

    def close_sync(self, cancellable: Any) -> None:
        if cancellable is not None:
            raise AssertionError("unexpected cancellable")
        self.close_calls += 1

    def emit(self, signal_name: str, values: Any) -> None:
        self.callbacks[signal_name](
            self,
            STANDARD_BUS_NAME,
            STANDARD_PATH,
            STANDARD_INTERFACE,
            signal_name,
            FakeVariant("signal", values),
            None,
        )


class FakeConnectionFactory:
    def __init__(self, connection: FakeConnection) -> None:
        self.connection = connection
        self.calls: list[tuple[Any, ...]] = []

    def new_for_address_sync(self, *arguments: Any) -> FakeConnection:
        self.calls.append(arguments)
        return self.connection


class FakeGio:
    class DBusConnectionFlags:
        AUTHENTICATION_CLIENT = 1
        MESSAGE_BUS_CONNECTION = 2

    class DBusSignalFlags:
        NONE = 0

    class DBusCallFlags:
        NO_AUTO_START = 4

    def __init__(self, connection: FakeConnection) -> None:
        self.DBusConnection = FakeConnectionFactory(connection)


class FakeTransport:
    def __init__(self) -> None:
        self.next_id = 1
        self.requests: list[tuple[int, str, int]] = []
        self.signals: list[NotificationSignal] = []
        self.queued_signal_specs: list[tuple[str, int, str | int]] = []
        self.dispatches = 0
        self.closed = False
        self.replacement_reply: int | None = None
        self.signal_generation = 0
        self.notify_reply_latency_ns: int | None = 750

    def notify(self, replaces_id: int, summary: str, urgency: int) -> int:
        self.requests.append((replaces_id, summary, urgency))
        if replaces_id:
            return (
                replaces_id
                if self.replacement_reply is None
                else self.replacement_reply
            )
        result = self.next_id
        self.next_id += 1
        return result

    def take_notify_reply_latency_ns(self) -> int:
        value = self.notify_reply_latency_ns
        self.notify_reply_latency_ns = None
        if value is None:
            raise RuntimeError
        return value

    def take_signals(self) -> tuple[NotificationSignal, ...]:
        signals = tuple(self.signals)
        self.signals.clear()
        return signals

    def advance_signal_generation(self, deadline: float) -> int:
        self.dispatch(deadline)
        self.signal_generation += 1
        return self.signal_generation

    def dispatch(self, _deadline: float) -> None:
        self.dispatches += 1
        self.signals.extend(
            NotificationSignal(kind, notification_id, detail, self.signal_generation)
            for kind, notification_id, detail in self.queued_signal_specs
        )
        self.queued_signal_specs.clear()

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
        self.assertEqual(self.probe.take_notify_reply_latency_ns(), 750)
        self.transport.notify_reply_latency_ns = 750
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

    def test_rejects_non_uint32_replacement_replies_before_identity_comparison(self) -> None:
        for invalid in (0, True, (1 << 32)):
            transport = FakeTransport()
            probe = PersistentNotificationProbe(transport)
            token, _result = probe.create_normal()
            transport.replacement_reply = invalid
            self.assert_category(
                "notification_notify_reply_invalid",
                lambda probe=probe, token=token: probe.replace(token),
            )
            probe.close()

    def test_replacement_rejects_delayed_same_id_signal_from_prior_content(self) -> None:
        original, _result = self.probe.create_normal()
        self.transport.queued_signal_specs = [("action", 1, ACTION_KEY)]
        replacement, _result = self.probe.replace(original)

        self.assert_category(
            "notification_signal_timeout",
            lambda: self.probe.wait_for_action(replacement, 0.01),
        )
        self.transport.queued_signal_specs = [("action", 1, ACTION_KEY)]
        self.probe.wait_for_action(replacement, 0.1)

    def test_reused_id_rejects_delayed_signal_from_closed_token(self) -> None:
        original, _result = self.probe.create_normal()
        self.transport.queued_signal_specs = [("closed", 1, 3)]
        self.probe.wait_for_close(original, 0.1)
        self.transport.signals = [NotificationSignal("action", 1, ACTION_KEY, 1)]
        self.transport.next_id = 1
        reused, _result = self.probe.create_normal()

        self.assert_category(
            "notification_signal_timeout",
            lambda: self.probe.wait_for_action(reused, 0.01),
        )
        self.transport.queued_signal_specs = [("action", 1, ACTION_KEY)]
        self.probe.wait_for_action(reused, 0.1)

    def test_observes_action_and_close_without_exposing_signal_values(self) -> None:
        token, _result = self.probe.create_normal()
        self.transport.signals.extend(
            [
                NotificationSignal("action", 1, ACTION_KEY, 1),
                NotificationSignal("closed", 1, 3, 1),
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
                NotificationSignal("closed", 2, 3, 2),
                NotificationSignal("action", 1, ACTION_KEY, 1),
            ]
        )

        self.probe.wait_for_action(first, 0.1)
        self.probe.wait_for_close(second, 0.1)

    def test_accepts_later_signal_generation_for_still_live_token(self) -> None:
        first, _result = self.probe.create_normal()
        self.probe.create_critical()
        self.transport.queued_signal_specs = [("closed", 1, 2)]
        self.probe.create_critical()

        self.probe.wait_for_close(first, 0.1)

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
            NotificationSignal("closed", index + 2, 3, 1)
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
            NotificationSignal("action", 1, "private-action", 1),
            NotificationSignal("closed", 1, 5, 1),
            NotificationSignal("private-kind", 1, 1, 1),
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


class GioNotificationTransportTests(unittest.TestCase):
    def setUp(self) -> None:
        self.context = FakeMainContext()
        self.connection = FakeConnection(self.context)
        self.gio = FakeGio(self.connection)
        self.glib = FakeGLib(self.context)
        self.environment = {
            "DBUS_SESSION_BUS_ADDRESS": "unix:path=/tmp/prismdrake-private-bus"
        }

    def assert_category(self, category: str, operation: Any) -> None:
        with self.assertRaises(LiveNotificationError) as raised:
            operation()
        self.assertEqual(str(raised.exception), category)

    def connect(self) -> GioNotificationTransport:
        return GioNotificationTransport.connect(
            self.environment, self.gio, self.glib
        )

    def test_connects_and_subscribes_only_on_probe_owned_private_context(self) -> None:
        transport = self.connect()

        self.assertEqual(
            self.gio.DBusConnection.calls,
            [
                (
                    self.environment["DBUS_SESSION_BUS_ADDRESS"],
                    3,
                    None,
                    None,
                )
            ],
        )
        self.assertEqual(self.context.pushes, 1)
        self.assertEqual(self.context.pops, 1)
        self.assertFalse(self.context.active)
        self.assertEqual(len(self.connection.subscriptions), 2)
        for signal_name, subscription in zip(
            ("ActionInvoked", "NotificationClosed"),
            self.connection.subscriptions,
            strict=True,
        ):
            self.assertEqual(
                subscription[:6],
                (
                    STANDARD_BUS_NAME,
                    STANDARD_INTERFACE,
                    signal_name,
                    STANDARD_PATH,
                    None,
                    0,
                ),
            )
        transport.close()

    def test_notify_uses_typed_bounded_call_and_parses_uint32_reply(self) -> None:
        ticks = iter((1_000, 1_750))
        transport = GioNotificationTransport.connect(
            self.environment,
            self.gio,
            self.glib,
            clock_ns=lambda: next(ticks),
        )

        self.assertEqual(transport.notify(0, NORMAL_SUMMARY, 1), 7)
        self.assertEqual(transport.take_notify_reply_latency_ns(), 750)
        self.assert_category(
            "notification_notify_latency_unavailable",
            transport.take_notify_reply_latency_ns,
        )

        self.assertEqual(len(self.connection.calls), 1)
        call = self.connection.calls[0]
        self.assertEqual(
            call[:4],
            (
                STANDARD_BUS_NAME,
                STANDARD_PATH,
                STANDARD_INTERFACE,
                "Notify",
            ),
        )
        self.assertEqual(call[4].signature, "(susssasa{sv}i)")
        self.assertEqual(call[5], "(u)")
        self.assertEqual(call[6], 4)
        self.assertEqual(call[7], 1000)
        self.assertIsNone(call[8])
        transport.close()

    def test_parses_filtered_callbacks_and_stamps_current_generation(self) -> None:
        transport = self.connect()
        self.connection.emit("ActionInvoked", (7, ACTION_KEY))
        self.assertEqual(
            transport.take_signals(),
            (NotificationSignal("action", 7, ACTION_KEY, 0),),
        )

        generation = transport.advance_signal_generation(time.monotonic() + 1.0)
        self.assertEqual(generation, 1)
        self.connection.emit("NotificationClosed", (7, 3))
        self.assertEqual(
            transport.take_signals(),
            (NotificationSignal("closed", 7, 3, 1),),
        )
        transport.close()

    def test_rejects_malformed_callbacks_and_bounds_signal_queue(self) -> None:
        malformed = self.connect()
        self.connection.emit("ActionInvoked", (7, "private-action"))
        self.assert_category("notification_signal_invalid", malformed.take_signals)
        malformed.close()

        context = FakeMainContext()
        connection = FakeConnection(context)
        transport = GioNotificationTransport.connect(
            self.environment, FakeGio(connection), FakeGLib(context)
        )
        for notification_id in range(1, MAX_PENDING_SIGNALS + 2):
            connection.emit("NotificationClosed", (notification_id, 3))
        self.assert_category("notification_signal_bound", transport.take_signals)
        transport.close()

    def test_bounds_continuously_ready_dispatch_and_honors_deadline(self) -> None:
        context = FakeMainContext(always_pending=True)
        connection = FakeConnection(context)
        transport = GioNotificationTransport.connect(
            self.environment, FakeGio(connection), FakeGLib(context)
        )

        self.assert_category(
            "notification_signal_dispatch_bound",
            lambda: transport.dispatch(time.monotonic() + 1.0),
        )
        self.assertEqual(context.iterations, MAX_DISPATCH_ITERATIONS)
        self.assert_category(
            "notification_signal_dispatch_timeout",
            lambda: transport.dispatch(time.monotonic() - 1.0),
        )
        transport.close()

    def test_rolls_back_partial_subscription_and_closes_connection(self) -> None:
        self.connection.fail_subscription_at = 2
        self.assert_category("notification_signal_subscription_failed", self.connect)

        self.assertEqual(self.connection.unsubscribed, [1])
        self.assertGreaterEqual(self.connection.close_calls, 1)
        self.assertEqual(self.context.pops, 1)
        self.assertFalse(self.context.active)

    def test_closes_connection_when_identity_probe_fails_after_connect(self) -> None:
        self.connection.raise_unique_name = True
        self.assert_category(
            "notification_private_bus_connection_failed", self.connect
        )

        self.assertEqual(self.connection.close_calls, 1)
        self.assertEqual(self.connection.subscriptions, [])
        self.assertEqual(self.context.pops, 1)

    def test_close_unsubscribes_both_filters_and_is_idempotent(self) -> None:
        transport = self.connect()
        transport.close()
        transport.close()

        self.assertEqual(self.connection.unsubscribed, [1, 2])
        self.assertEqual(self.connection.close_calls, 1)


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
